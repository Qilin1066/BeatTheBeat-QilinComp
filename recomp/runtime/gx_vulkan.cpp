// GX on Vulkan 1.3 (gx_vulkan.hpp): the Sink GX's natives draw through.
//
// GX's transform unit (matrices, projection, lighting, texture coordinate
// generation) runs on the CPU, per vertex, on GX's own state: the game draws
// a few thousand vertices a frame. The GPU gets clip-space vertices with
// their two colours and eight texture coordinates; one fragment shader
// interprets the TEV stages (shaders/gx.frag). The embedded frame buffer
// (EFB) is an offscreen 640x528 image; a display copy blits its source
// rectangle to the window, keeping the console's aspect, and presents.
//
// Not supported yet, each stopping with a message when a draw needs it:
// indirect texturing, bump-map texture coordinates, specular lighting, NBT
// normals, logic ops other than copy, Z textures, depth or YUV copies to
// textures, a scissor box offset. Values GX doesn't define stop too. APPROX
// (marked where they are): the TEV's rounding and konst fractions, CMPR's
// in-between colours, edge LOD and anisotropy, line and point texture
// offsets, fog's fixed-point steps, destination alpha with blending, the
// EFB's pixel format (always RGBA8), and Z compared before texturing: GX
// then writes depth for pixels the alpha test rejects; here they write none.
// Runtime code: no std::format.
#include "gx_vulkan.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "console.hpp"
#include "gx.hpp"
#include "machine.hpp"
#include "native.hpp"

#include "copy_frag.h"
#include "copy_vert.h"
#include "gx_frag.h"
#include "gx_vert.h"

namespace qilin::rt::gx {

namespace {

// ---- Vulkan's functions, loaded through SDL (no Vulkan SDK needed) ----
#define QILIN_VK_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceVersion)
#define QILIN_VK_INSTANCE(X)                                                                                         \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties)       \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                             \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)                             \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceFeatures2)     \
    X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties)
#define QILIN_VK_DEVICE(X)                                                                                           \
    X(vkGetDeviceQueue) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR)                  \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkQueueSubmit2) X(vkDeviceWaitIdle) X(vkCreateCommandPool)        \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer)               \
    X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore) X(vkDestroySemaphore)                 \
    X(vkCreateImage) X(vkDestroyImage) X(vkCreateImageView) X(vkDestroyImageView) X(vkGetImageMemoryRequirements)   \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements)     \
    X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateSampler) X(vkCreateDescriptorSetLayout)                          \
    X(vkCreatePipelineLayout) X(vkCreateShaderModule) X(vkCreateGraphicsPipelines) X(vkCreateDescriptorPool)        \
    X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCmdPipelineBarrier2)         \
    X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets)                     \
    X(vkCmdBindVertexBuffers) X(vkCmdDraw) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetCullMode)               \
    X(vkCmdSetFrontFace) X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp)           \
    X(vkCmdBlitImage) X(vkCmdCopyBufferToImage) X(vkCmdClearAttachments) X(vkCmdClearColorImage) X(vkCmdPushConstants)    \
    X(vkCmdClearDepthStencilImage)                                                                                   \
    X(vkCmdCopyImageToBuffer)

PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
#define QILIN_VK_DECLARE(f) PFN_##f f = nullptr;
QILIN_VK_GLOBAL(QILIN_VK_DECLARE)
QILIN_VK_INSTANCE(QILIN_VK_DECLARE)
QILIN_VK_DEVICE(QILIN_VK_DECLARE)
#undef QILIN_VK_DECLARE

// A Vulkan structure, zeroed, with its type.
template <class T>
T vk_struct(VkStructureType type) {
    T v{};
    v.sType = type;
    return v;
}

// A Vulkan failure while the game runs: nothing to fall back to.
void vk_check(VkResult r, const char* what) {
    if (r == VK_SUCCESS) return;
    std::fprintf(stderr, "qilin_run: Vulkan: %s failed (%d)\n", what, static_cast<int>(r));
    throw Stop{Stop::Unsupported, 0, static_cast<uint32_t>(r)};
}
[[noreturn]] void unsupported(const char* what, uint32_t value) {
    std::fprintf(stderr, "qilin_run: GX: %s (%u): not drawn by the renderer yet\n", what, value);
    throw Stop{Stop::Unsupported, 0, value};
}

constexpr uint32_t kEfbWidth = 640, kEfbHeight = 528;
constexpr VkFormat kEfbColor = VK_FORMAT_R8G8B8A8_UNORM, kEfbDepth = VK_FORMAT_D32_SFLOAT;
constexpr unsigned kFrames = 2;
constexpr VkDeviceSize kVertexBytes = 32u << 20, kUniformBytes = 8u << 20, kUploadBytes = 64u << 20;
constexpr uint32_t kSetsPerFrame = 16384;

// What the vertex shader gets (shaders/gx.vert).
struct GpuVertex {
    float pos[4];
    float col0[4], col1[4];
    float tex[8][3];
};
// The fragment shader's uniform block (shaders/gx.frag), std140.
struct DrawBlock {
    int32_t regs[4][4];
    int32_t konst[4][4];
    uint32_t swaps[4][4];
    uint32_t misc[4];
    uint32_t stages[48][4];
    uint32_t fog[4];
    float fog_abc[4];
    float depth_range[4];
    uint32_t tcscale[8][4];
};
// The copy shader's push constants (shaders/copy.*).
struct CopyBlock {
    float src[4];
    uint32_t mode[4];
};

struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    uint8_t* map = nullptr;
};
struct Image {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t w = 0, h = 0, levels = 1;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};
struct Frame {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    Buffer vtx, ubo, upload, readback;
    VkDeviceSize vtx_used = 0, ubo_used = 0, upload_used = 0;
    uint32_t sets = 0;
};
// A decoded texture: re-checked (hashed) at most once per frame and per
// texture load (State::tex_loads), and evicted when unused for a while.
struct Texture {
    Image image;
    uint64_t hash = 0, frame_checked = ~0ull, loads_checked = ~0ull, last_used = 0;
};
constexpr uint64_t kTextureIdleFrames = 600;  // 10 s at 60 Hz: a texture unused this long is freed

struct Vk {
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0;
    VkPhysicalDeviceMemoryProperties memory{};
    VkDeviceSize ubo_align = 256;
    SDL_Window* window = nullptr;
    // the swapchain
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swap_format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> swap_images;
    std::vector<VkSemaphore> rendered;  // one per swapchain image
    std::atomic<bool> resized{false};
    // rendering
    VkCommandPool cmd_pool = VK_NULL_HANDLE;
    Frame frames[kFrames];
    unsigned frame = 0;
    uint64_t frame_number = 0;
    bool recording = false, rendering = false;
    Image efb_color, efb_depth;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    std::unordered_map<uint32_t, VkPipeline> pipelines;
    std::unordered_map<uint64_t, VkSampler> samplers;
    std::unordered_map<uint64_t, Texture> textures;
    // copies from the EFB to textures (shaders/copy.*)
    VkDescriptorSetLayout copy_set_layout = VK_NULL_HANDLE;
    VkPipelineLayout copy_layout = VK_NULL_HANDLE;
    VkShaderModule copy_vert = VK_NULL_HANDLE, copy_frag = VK_NULL_HANDLE;
    VkPipeline copy_pipeline = VK_NULL_HANDLE;
    VkSampler efb_sampler = VK_NULL_HANDLE;
    Image blank;  // what unused texture maps sample
    std::vector<std::pair<uint64_t, Image>> garbage;  // images to free once their frame is done
} g;

// ---- memory, buffers and images ----
uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g.memory.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (g.memory.memoryTypes[i].propertyFlags & want) == want) return i;
    vk_check(VK_ERROR_OUT_OF_DEVICE_MEMORY, "finding a memory type");
    return 0;
}
VkDeviceMemory allocate(VkMemoryRequirements req, VkMemoryPropertyFlags want) {
    VkMemoryAllocateInfo ai = vk_struct<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, want);
    VkDeviceMemory m;
    vk_check(vkAllocateMemory(g.device, &ai, nullptr, &m), "vkAllocateMemory");
    return m;
}
Buffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer b;
    VkBufferCreateInfo bi = vk_struct<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    bi.size = size, bi.usage = usage, bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vk_check(vkCreateBuffer(g.device, &bi, nullptr, &b.buf), "vkCreateBuffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g.device, b.buf, &req);
    b.mem = allocate(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vk_check(vkBindBufferMemory(g.device, b.buf, b.mem, 0), "vkBindBufferMemory");
    void* p;
    vk_check(vkMapMemory(g.device, b.mem, 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory");
    b.map = static_cast<uint8_t*>(p);
    return b;
}
Image make_image(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect,
                 uint32_t levels = 1) {
    Image im;
    im.w = w, im.h = h, im.levels = levels;
    VkImageCreateInfo ii = vk_struct<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    ii.imageType = VK_IMAGE_TYPE_2D, ii.format = format, ii.extent = {w, h, 1};
    ii.mipLevels = levels, ii.arrayLayers = 1, ii.samples = VK_SAMPLE_COUNT_1_BIT, ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage, ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE, ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vk_check(vkCreateImage(g.device, &ii, nullptr, &im.img), "vkCreateImage");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g.device, im.img, &req);
    im.mem = allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vk_check(vkBindImageMemory(g.device, im.img, im.mem, 0), "vkBindImageMemory");
    VkImageViewCreateInfo vi = vk_struct<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    vi.image = im.img, vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.format = format;
    vi.subresourceRange = {aspect, 0, levels, 0, 1};
    vk_check(vkCreateImageView(g.device, &vi, nullptr, &im.view), "vkCreateImageView");
    return im;
}
void free_image(Image& im) {
    if (im.view) vkDestroyImageView(g.device, im.view, nullptr);
    if (im.img) vkDestroyImage(g.device, im.img, nullptr);
    if (im.mem) vkFreeMemory(g.device, im.mem, nullptr);
    im = Image{};
}

// A layout change, ordered after everything earlier on the queue (simple and
// safe; the renderer isn't barrier-bound).
void transition(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkImageAspectFlags aspect,
                uint32_t levels = 1) {
    VkImageMemoryBarrier2 b = vk_struct<VkImageMemoryBarrier2>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2);
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from, b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img, b.subresourceRange = {aspect, 0, levels, 0, 1};
    VkDependencyInfo d = vk_struct<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
    d.imageMemoryBarrierCount = 1, d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &d);
}
void transition(VkCommandBuffer cb, Image& im, VkImageLayout to, VkImageAspectFlags aspect) {
    if (im.layout == to) return;
    transition(cb, im.img, im.layout, to, aspect, im.levels);
    im.layout = to;
}

// ---- the swapchain ----
bool make_swapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vk_check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.gpu, g.surface, &caps), "surface capabilities");
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(g.window, &w, &h);
        extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h)};
    }
    if (extent.width == 0 || extent.height == 0) return false;  // minimised
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        std::fprintf(stderr, "qilin_run: Vulkan: the window's images can't be copied to\n");
        throw Stop{Stop::Unsupported, 0, 0};
    }
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.gpu, g.surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.gpu, g.surface, &n, formats.data());
    // An 8-bit UNORM format: GX's output is already gamma-encoded, so an
    // sRGB format would encode it twice. It must take blits.
    VkSurfaceFormatKHR format{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    for (const auto& f : formats) {
        if ((f.format != VK_FORMAT_B8G8R8A8_UNORM && f.format != VK_FORMAT_R8G8B8A8_UNORM) ||
            f.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            continue;
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g.gpu, f.format, &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) format = f;
    }
    if (format.format == VK_FORMAT_UNDEFINED) {
        std::fprintf(stderr, "qilin_run: Vulkan: the window has no 8-bit UNORM format that blits can write\n");
        throw Stop{Stop::Unsupported, 0, 0};
    }
    vkGetPhysicalDeviceSurfacePresentModesKHR(g.gpu, g.surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g.gpu, g.surface, &n, modes.data());
    // Mailbox if there is one: presenting never waits for the display, so
    // the game's own clock (VI's retrace) paces it.
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (auto m : modes)
        if (m == VK_PRESENT_MODE_MAILBOX_KHR) mode = m;

    VkSwapchainCreateInfoKHR si = vk_struct<VkSwapchainCreateInfoKHR>(VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);
    si.surface = g.surface;
    si.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) si.minImageCount = std::min(si.minImageCount, caps.maxImageCount);
    si.imageFormat = format.format, si.imageColorSpace = format.colorSpace, si.imageExtent = extent;
    si.imageArrayLayers = 1, si.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    si.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, si.preTransform = caps.currentTransform;
    si.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, si.presentMode = mode, si.clipped = VK_TRUE;
    si.oldSwapchain = g.swapchain;
    VkSwapchainKHR sc;
    vk_check(vkCreateSwapchainKHR(g.device, &si, nullptr, &sc), "vkCreateSwapchainKHR");
    if (g.swapchain) {
        vk_check(vkDeviceWaitIdle(g.device), "vkDeviceWaitIdle");
        vkDestroySwapchainKHR(g.device, g.swapchain, nullptr);
        for (VkSemaphore s : g.rendered) vkDestroySemaphore(g.device, s, nullptr);
    }
    g.swapchain = sc, g.swap_format = format.format, g.extent = extent;
    vkGetSwapchainImagesKHR(g.device, sc, &n, nullptr);
    g.swap_images.resize(n);
    vkGetSwapchainImagesKHR(g.device, sc, &n, g.swap_images.data());
    g.rendered.resize(n);
    for (auto& s : g.rendered) {
        VkSemaphoreCreateInfo ci = vk_struct<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
        vk_check(vkCreateSemaphore(g.device, &ci, nullptr, &s), "vkCreateSemaphore");
    }
    return true;
}

// ---- pipelines and samplers ----
VkShaderModule shader(const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci = vk_struct<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    ci.codeSize = bytes, ci.pCode = code;
    VkShaderModule m;
    vk_check(vkCreateShaderModule(g.device, &ci, nullptr, &m), "vkCreateShaderModule");
    return m;
}

// GXBlendFactor to Vulkan: as a source factor, 2/3 mean the destination's
// colour; as a destination factor, the source's.
VkBlendFactor blend_factor(uint8_t f, bool src) {
    switch (f) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 2: return src ? VK_BLEND_FACTOR_DST_COLOR : VK_BLEND_FACTOR_SRC_COLOR;
    case 3: return src ? VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_DST_ALPHA;
    default: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    }
}

// The fixed-function state a pipeline holds: blending and write masks. The
// rest (viewport, scissor, culling, depth) is dynamic state.
uint32_t pipeline_key(const State& s) {
    uint32_t type = s.blend_type;
    if (type == 2) {
        if (s.logic_op != 3) unsupported("a logic op other than copy", s.logic_op);  // GX_LO_COPY
        type = 0;
    }
    return type | uint32_t(s.blend_src) << 2 | uint32_t(s.blend_dst) << 5 | uint32_t(s.color_update) << 8 |
           uint32_t(s.alpha_update) << 9;
}

VkPipeline pipeline(uint32_t key) {
    if (auto it = g.pipelines.find(key); it != g.pipelines.end()) return it->second;
    const uint32_t type = key & 3;
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT, stages[0].module = g.vert, stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, stages[1].module = g.frag, stages[1].pName = "main";

    VkVertexInputBindingDescription binding{0, sizeof(GpuVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[11];
    attrs[0] = {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuVertex, pos)};
    attrs[1] = {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuVertex, col0)};
    attrs[2] = {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuVertex, col1)};
    for (uint32_t i = 0; i < 8; ++i)
        attrs[3 + i] = {3 + i, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<uint32_t>(offsetof(GpuVertex, tex) + 12 * i)};
    VkPipelineVertexInputStateCreateInfo vin = vk_struct<VkPipelineVertexInputStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
    vin.vertexBindingDescriptionCount = 1, vin.pVertexBindingDescriptions = &binding;
    vin.vertexAttributeDescriptionCount = 11, vin.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo ia = vk_struct<VkPipelineInputAssemblyStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = vk_struct<VkPipelineViewportStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
    vp.viewportCount = 1, vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = vk_struct<VkPipelineRasterizationStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
    rs.polygonMode = VK_POLYGON_MODE_FILL, rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = vk_struct<VkPipelineMultisampleStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = vk_struct<VkPipelineDepthStencilStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = ((key >> 8) & 1 ? VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT : 0u) |
                         ((key >> 9) & 1 ? VK_COLOR_COMPONENT_A_BIT : 0u);
    if (type == 1 || type == 3) {
        att.blendEnable = VK_TRUE;
        if (type == 1) {
            att.srcColorBlendFactor = att.srcAlphaBlendFactor = blend_factor((key >> 2) & 7, true);
            att.dstColorBlendFactor = att.dstAlphaBlendFactor = blend_factor((key >> 5) & 7, false);
            att.colorBlendOp = att.alphaBlendOp = VK_BLEND_OP_ADD;
        } else {  // GX_BM_SUBTRACT: destination minus source, factors ignored
            att.srcColorBlendFactor = att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            att.dstColorBlendFactor = att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            att.colorBlendOp = att.alphaBlendOp = VK_BLEND_OP_REVERSE_SUBTRACT;
        }
    }
    VkPipelineColorBlendStateCreateInfo cb = vk_struct<VkPipelineColorBlendStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
    cb.attachmentCount = 1, cb.pAttachments = &att;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT,          VK_DYNAMIC_STATE_SCISSOR,
                                  VK_DYNAMIC_STATE_CULL_MODE,         VK_DYNAMIC_STATE_FRONT_FACE,
                                  VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                                  VK_DYNAMIC_STATE_DEPTH_COMPARE_OP};
    VkPipelineDynamicStateCreateInfo dy = vk_struct<VkPipelineDynamicStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
    dy.dynamicStateCount = static_cast<uint32_t>(std::size(dyn)), dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri = vk_struct<VkPipelineRenderingCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);
    ri.colorAttachmentCount = 1, ri.pColorAttachmentFormats = &kEfbColor, ri.depthAttachmentFormat = kEfbDepth;
    VkGraphicsPipelineCreateInfo pi = vk_struct<VkGraphicsPipelineCreateInfo>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
    pi.pNext = &ri, pi.stageCount = 2, pi.pStages = stages, pi.pVertexInputState = &vin, pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp, pi.pRasterizationState = &rs, pi.pMultisampleState = &ms, pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb, pi.pDynamicState = &dy, pi.layout = g.layout;
    VkPipeline p;
    vk_check(vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE, 1, &pi, nullptr, &p), "vkCreateGraphicsPipelines");
    g.pipelines.emplace(key, p);
    return p;
}

// A texture map's sampler, from the texture object's mode words as the
// SDK's GXInitTexObjLOD packs them: mode0 has the wraps (bits 0-3), the
// magnification filter (bit 4), the minification filter's hardware code
// (bits 5-7: bit 2 linear, bits 0-1 mipmapped, bit 1 linear between levels;
// the SDK's table maps GX_NEAR..GX_LIN_MIP_LIN to 0, 4, 1, 5, 2, 6) and the
// LOD bias times 32 (bits 9-16, signed); mode1 the minimum and maximum LOD
// times 16. APPROX: edge LOD, anisotropy and the bias clamp aren't modelled.
VkSampler sampler(uint32_t mode0, uint32_t mode1 = 0, uint32_t levels = 1) {
    const uint64_t key = (mode0 & 0x1FEFFu) | uint64_t(mode1 & 0xFFFFu) << 32 | uint64_t(levels) << 48;
    if (auto it = g.samplers.find(key); it != g.samplers.end()) return it->second;
    auto wrap = [](uint32_t w) {
        if (w > 2) unsupported("a texture wrap mode", w);
        return w == 0 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
               : w == 1 ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                        : VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    };
    VkSamplerCreateInfo si = vk_struct<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
    const uint32_t min_code = (mode0 >> 5) & 7;
    si.magFilter = (mode0 >> 4) & 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.minFilter = min_code & 4 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    si.mipmapMode = min_code & 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = wrap(mode0 & 3), si.addressModeV = wrap((mode0 >> 2) & 3);
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if ((min_code & 3) && levels > 1) {
        si.maxLod = std::min(float((mode1 >> 8) & 0xFF) / 16.0f, float(levels - 1));
        si.minLod = std::min(float(mode1 & 0xFF) / 16.0f, si.maxLod);  // Vulkan needs minLod <= maxLod
        si.mipLodBias = float(static_cast<int8_t>((mode0 >> 9) & 0xFF)) / 32.0f;
    }
    VkSampler s;
    vk_check(vkCreateSampler(g.device, &si, nullptr, &s), "vkCreateSampler");
    g.samplers.emplace(key, s);
    return s;
}

// The copy pass: one triangle into an RGBA8 target, the EFB read with
// texelFetch, the rectangle and conversion as push constants.
void create_copy_pipeline() {
    VkDescriptorSetLayoutBinding b{};
    b.binding = 0, b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, b.descriptorCount = 1;
    b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li = vk_struct<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    li.bindingCount = 1, li.pBindings = &b;
    vk_check(vkCreateDescriptorSetLayout(g.device, &li, nullptr, &g.copy_set_layout), "vkCreateDescriptorSetLayout");
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(CopyBlock)};
    VkPipelineLayoutCreateInfo pl = vk_struct<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    pl.setLayoutCount = 1, pl.pSetLayouts = &g.copy_set_layout, pl.pushConstantRangeCount = 1, pl.pPushConstantRanges = &push;
    vk_check(vkCreatePipelineLayout(g.device, &pl, nullptr, &g.copy_layout), "vkCreatePipelineLayout");
    g.copy_vert = shader(k_copy_vert, sizeof k_copy_vert);
    g.copy_frag = shader(k_copy_frag, sizeof k_copy_frag);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT, stages[0].module = g.copy_vert, stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, stages[1].module = g.copy_frag, stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin = vk_struct<VkPipelineVertexInputStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
    VkPipelineInputAssemblyStateCreateInfo ia = vk_struct<VkPipelineInputAssemblyStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = vk_struct<VkPipelineViewportStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
    vp.viewportCount = 1, vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = vk_struct<VkPipelineRasterizationStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
    rs.polygonMode = VK_POLYGON_MODE_FILL, rs.cullMode = VK_CULL_MODE_NONE, rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = vk_struct<VkPipelineMultisampleStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb = vk_struct<VkPipelineColorBlendStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
    cb.attachmentCount = 1, cb.pAttachments = &att;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = vk_struct<VkPipelineDynamicStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
    dy.dynamicStateCount = 2, dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri = vk_struct<VkPipelineRenderingCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);
    const VkFormat target = VK_FORMAT_R8G8B8A8_UNORM;
    ri.colorAttachmentCount = 1, ri.pColorAttachmentFormats = &target;
    VkGraphicsPipelineCreateInfo pi = vk_struct<VkGraphicsPipelineCreateInfo>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
    pi.pNext = &ri, pi.stageCount = 2, pi.pStages = stages, pi.pVertexInputState = &vin, pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp, pi.pRasterizationState = &rs, pi.pMultisampleState = &ms, pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy, pi.layout = g.copy_layout;
    vk_check(vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE, 1, &pi, nullptr, &g.copy_pipeline), "vkCreateGraphicsPipelines");
    VkSamplerCreateInfo si = vk_struct<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vk_check(vkCreateSampler(g.device, &si, nullptr, &g.efb_sampler), "vkCreateSampler");
}

// GX's copy formats: how the copy shader converts (shaders/copy.frag), and
// the texture format of the same size in memory (for the guest-bytes hash).
struct CopyFormat {
    uint32_t mode, mem_format;
};
CopyFormat copy_format(uint32_t f) {
    switch (f) {
    case kTexRgba8: return {0, kTexRgba8};
    case kTexRgb5A3: return {0, kTexRgb5A3};
    case kTexRgb565: return {1, kTexRgb565};
    case kTexI4: return {2, kTexI4};
    case kTexI8: return {2, kTexI8};
    case kTexIA4: return {3, kTexIA4};
    case kTexIA8: return {3, kTexIA8};
    case 0x20: return {4, kTexI4};    // GX_CTF_R4
    case 0x28: return {4, kTexI8};    // GX_CTF_R8
    case 0x22: return {5, kTexIA4};   // GX_CTF_RA4
    case 0x23: return {5, kTexIA8};   // GX_CTF_RA8
    case 0x29: return {6, kTexI8};    // GX_CTF_G8
    case 0x2A: return {7, kTexI8};    // GX_CTF_B8
    case 0x27: return {8, kTexI8};    // GX_CTF_A8
    case 0x2B: return {9, kTexIA8};   // GX_CTF_RG8
    case 0x2C: return {10, kTexIA8};  // GX_CTF_GB8
    default: unsupported("a copy to a texture in this format (depth or YUV)", f);
    }
}

// ---- frames ----
Frame& cur() { return g.frames[g.frame]; }

void begin_frame() {
    if (g.recording) return;
    Frame& f = cur();
    vk_check(vkWaitForFences(g.device, 1, &f.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    vk_check(vkResetFences(g.device, 1, &f.fence), "vkResetFences");
    vk_check(vkResetDescriptorPool(g.device, f.pool, 0), "vkResetDescriptorPool");
    f.vtx_used = f.ubo_used = f.upload_used = 0, f.sets = 0;
    // Images retired two frames ago are no longer in use.
    std::erase_if(g.garbage, [](auto& e) {
        if (e.first + kFrames > g.frame_number) return false;
        free_image(e.second);
        return true;
    });
    // Textures unused for a while go (scenes load into new places; keeping
    // them all would grow without bound): retired like any other image.
    if (g.frame_number % 60 == 0)
        std::erase_if(g.textures, [](auto& e) {
            if (e.second.last_used + kTextureIdleFrames > g.frame_number) return false;
            if (e.second.image.img) g.garbage.emplace_back(g.frame_number, e.second.image);
            return true;
        });
    VkCommandBufferBeginInfo bi = vk_struct<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkResetCommandBuffer(f.cb, 0), "vkResetCommandBuffer");
    vk_check(vkBeginCommandBuffer(f.cb, &bi), "vkBeginCommandBuffer");
    g.recording = true;
}

void end_rendering() {
    if (!g.rendering) return;
    vkCmdEndRendering(cur().cb);
    g.rendering = false;
}

void begin_rendering() {
    begin_frame();
    if (g.rendering) return;
    VkCommandBuffer cb = cur().cb;
    transition(cb, g.efb_color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    transition(cb, g.efb_depth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkRenderingAttachmentInfo color = vk_struct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    color.imageView = g.efb_color.view, color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depth = color;
    depth.imageView = g.efb_depth.view, depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    VkRenderingInfo ri = vk_struct<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
    ri.renderArea = {{0, 0}, {kEfbWidth, kEfbHeight}}, ri.layerCount = 1;
    ri.colorAttachmentCount = 1, ri.pColorAttachments = &color, ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(cb, &ri);
    g.rendering = true;
}

// Space in one of the frame's rings; running out stops (the sizes are generous).
VkDeviceSize take(Frame& f, VkDeviceSize& used, VkDeviceSize limit, VkDeviceSize bytes, VkDeviceSize align,
                  const char* what) {
    const VkDeviceSize at = (used + align - 1) / align * align;
    if (at + bytes > limit) unsupported(what, static_cast<uint32_t>(bytes));
    used = at + bytes;
    (void)f;
    return at;
}

// ---- textures ----
uint64_t hash_bytes(const uint8_t* p, size_t n, uint64_t h) {
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        h = std::rotl((h ^ w) * 0x9E3779B97F4A7C15ull, 29);
    }
    for (; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

// How many levels a texture map's object has: one without mipmaps, else up
// to its maximum LOD (mode1 bits 8-15, times 16), at most down to 1x1.
uint32_t texture_levels(const TexMap& t) {
    if (!t.mipmap) return 1;
    const uint32_t max_lod = ((t.mode1 >> 8) & 0xFF) / 16;
    const uint32_t full = std::bit_width(std::max<uint32_t>(t.width, t.height));
    return std::min(max_lod + 1, full);
}

// Guest addresses as one key, whichever mirror the game used.
uint32_t norm_addr(uint32_t a) { return (a & 0x1FFFFFFFu) | 0x80000000u; }

// A copy from the EFB to a texture (GXCopyTex): the image, the texture it
// stands for, and a hash of the guest bytes there when it was made (the
// copy itself lives on the GPU; if the CPU writes those bytes, the texture
// is theirs again).
struct EfbCopy {
    Image image;
    uint32_t width = 0, height = 0, format = 0;
    uint32_t guest_bytes = 0;  // the copy's size in guest memory, which guest_hash covers
    uint64_t guest_hash = 0;
    uint64_t frame_checked = ~0ull, loads_checked = ~0ull;  // as Texture's
};
std::unordered_map<uint32_t, EfbCopy> g_copies;

void require_texture_memory(uint32_t addr, uint32_t bytes, const char* what) {
    if (!guest_range_ok(addr, bytes)) unsupported(what, addr);
}

// The texture a map holds, decoded and uploaded when it's new or its bytes
// (or its TLUT's) changed since it was last checked; or an EFB copy there.
const Image& texture(const State& s, const TexMap& t) {
    const uint32_t levels = texture_levels(t);
    uint32_t bytes = 0;
    for (uint32_t l = 0; l < levels; ++l) bytes += texture_bytes(t.format, std::max(1u, uint32_t(t.width) >> l), std::max(1u, uint32_t(t.height) >> l));
    if (!bytes) unsupported("texture format", t.format);
    if (auto it = g_copies.find(norm_addr(t.image)); it != g_copies.end()) {
        EfbCopy& c = it->second;
        if (c.width == t.width && c.height == t.height) {
            if (c.frame_checked == g.frame_number && c.loads_checked == s.tex_loads) return c.image;
            require_texture_memory(t.image, c.guest_bytes, "a texture copy's memory outside MEM1 and MEM2");
            if (hash_bytes(g_mem + t.image, c.guest_bytes, 0) == c.guest_hash) {
                c.frame_checked = g.frame_number, c.loads_checked = s.tex_loads;
                return c.image;
            }
        }
        g.garbage.emplace_back(g.frame_number, c.image);  // the CPU rewrote it, or it's read as something else
        g_copies.erase(it);
    }
    const bool indexed = t.format == kTexC4 || t.format == kTexC8 || t.format == kTexC14X2;
    const Tlut* tlut = nullptr;
    if (indexed) {
        if (t.tlut >= s.tlut.size()) unsupported("a TLUT name", t.tlut);
        tlut = &s.tlut[t.tlut];
        if (!tlut->loaded) unsupported("a colour-indexed texture whose TLUT isn't loaded", t.tlut);
        require_texture_memory(tlut->data, 2u * tlut->entries, "a TLUT outside MEM1 and MEM2");
    }
    require_texture_memory(t.image, bytes, "a texture outside MEM1 and MEM2");
    uint64_t key = hash_bytes(nullptr, 0, 0x51C0FFEEull);
    const uint32_t id[7] = {t.image, t.format, t.width, t.height, tlut ? tlut->data : 0u, tlut ? tlut->format : 0u, levels};
    key = hash_bytes(reinterpret_cast<const uint8_t*>(id), sizeof id, key);
    Texture& tex = g.textures[key];
    tex.last_used = g.frame_number;
    if (tex.image.img && tex.frame_checked == g.frame_number && tex.loads_checked == s.tex_loads) return tex.image;
    tex.frame_checked = g.frame_number, tex.loads_checked = s.tex_loads;
    uint64_t h = hash_bytes(g_mem + t.image, bytes, 0);
    if (tlut) h = hash_bytes(g_mem + tlut->data, 2u * tlut->entries, h);
    if (tex.image.img && tex.hash == h) return tex.image;
    tex.hash = h;

    if (!tex.image.img)
        tex.image = make_image(t.width, t.height, VK_FORMAT_R8G8B8A8_UNORM,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, levels);
    Frame& f = cur();
    end_rendering();
    transition(f.cb, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    uint32_t src = t.image;
    for (uint32_t l = 0; l < levels; ++l) {
        const uint32_t w = std::max(1u, uint32_t(t.width) >> l), hgt = std::max(1u, uint32_t(t.height) >> l);
        const VkDeviceSize n = VkDeviceSize(w) * hgt * 4;
        const VkDeviceSize at = take(f, f.upload_used, kUploadBytes, n, 16, "texture uploads in one frame (bytes)");
        decode_texture(t.format, src, w, hgt, tlut, f.upload.map + at);
        src += texture_bytes(t.format, w, hgt);
        VkBufferImageCopy copy{};
        copy.bufferOffset = at;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
        copy.imageExtent = {w, hgt, 1};
        vkCmdCopyBufferToImage(f.cb, f.upload.buf, tex.image.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }
    transition(f.cb, tex.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    ++stats().textures_uploaded;
    return tex.image;
}

// ---- GX's transform unit, on the CPU ----
float be_float(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return std::bit_cast<float>(std::byteswap(v));
}
uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }

float component(const uint8_t* p, uint8_t type, uint8_t frac) {
    const float scale = 1.0f / static_cast<float>(1u << frac);
    switch (type) {
    case kU8: return p[0] * scale;
    case kS8: return static_cast<int8_t>(p[0]) * scale;
    case kU16: return be16(p) * scale;
    case kS16: return static_cast<int16_t>(be16(p)) * scale;
    default: return be_float(p);
    }
}

struct Rgba {
    float r = 255, g = 255, b = 255, a = 255;
};
Rgba vertex_color(const uint8_t* p, uint8_t type) {
    switch (type) {
    case kRgb565: {
        const uint16_t v = be16(p);
        return {float((v >> 11) * 255 / 31), float(((v >> 5) & 63) * 255 / 63), float((v & 31) * 255 / 31), 255};
    }
    case kRgb8:
    case kRgbx8: return {float(p[0]), float(p[1]), float(p[2]), 255};
    case kRgba4: {
        const uint16_t v = be16(p);
        return {float((v >> 12) * 17), float(((v >> 8) & 15) * 17), float(((v >> 4) & 15) * 17), float((v & 15) * 17)};
    }
    case kRgba6: {
        const uint32_t v = uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2];
        auto x = [](uint32_t c) { return float(c * 255 / 63); };
        return {x(v >> 18), x((v >> 12) & 63), x((v >> 6) & 63), x(v & 63)};
    }
    default: return {float(p[0]), float(p[1]), float(p[2]), float(p[3])};
    }
}

// One vertex as it arrived: its attributes, decoded.
struct InVertex {
    uint8_t pn_idx = 0;
    uint8_t tex_idx[8] = {};
    bool has_tex_idx[8] = {};
    float pos[3] = {};
    float nrm[3] = {};
    bool has_nrm = false;
    Rgba col[2];
    bool has_col[2] = {};
    float tex[8][2] = {};
};

void read_vertex(const State& s, const Draw& d, const uint8_t* v, InVertex& out) {
    const auto& f = s.fmt[d.vtxfmt];
    size_t off = 0;
    out.pn_idx = static_cast<uint8_t>(s.current_mtx & 63);
    if (s.desc[kPnMtxIdx] != kNone) out.pn_idx = v[off++];
    for (unsigned i = 0; i < 8; ++i)
        if (s.desc[kTex0MtxIdx + i] != kNone) out.tex_idx[i] = v[off++], out.has_tex_idx[i] = true;
    for (uint32_t a = kPos; a < kPosMtxArray; ++a) {
        const uint8_t desc = s.desc[a];
        if (desc == kNone) continue;
        if (a == kNrm && f[a].cnt != 0) unsupported("normals with binormals and tangents", a);  // sized as three by vertex_size
        const uint8_t* src;
        if (desc == kDirect) {
            src = v + off;
            off += a == kPos                    ? (f[a].cnt ? 3u : 2u) * comp_size(f[a].type, a)
                   : a == kNrm                  ? 3u * comp_size(f[a].type, a)
                   : a == kClr0 || a == kClr1   ? color_size(f[a].type, a)
                                                : (f[a].cnt ? 2u : 1u) * comp_size(f[a].type, a);
        } else {
            const uint32_t index = desc == kIndex8 ? v[off] : be16(v + off);
            off += desc == kIndex8 ? 1 : 2;
            const uint32_t at = s.arrays[a].base + index * s.arrays[a].stride;
            const uint32_t n = (a == kClr0 || a == kClr1) ? 4 : 3 * 4;  // at most one element's bytes
            if (!((at >= kMem1 && at + n <= kMem1 + kMem1Bytes) || (at >= kMem2 && at + n <= kMem2 + kMem2Bytes)))
                unsupported("an indexed vertex attribute outside MEM1 and MEM2", at);
            src = g_mem + at;
        }
        const uint32_t cs = (a == kClr0 || a == kClr1) ? 0 : comp_size(f[a].type, a);
        if (a == kPos) {
            for (unsigned k = 0; k < (f[a].cnt ? 3u : 2u); ++k) out.pos[k] = component(src + k * cs, f[a].type, f[a].frac);
        } else if (a == kNrm) {
            // Normals' fraction bits are fixed by their type (6 for s8, 14 for s16).
            const uint8_t frac = f[a].type == kS8 ? 6 : f[a].type == kS16 ? 14 : 0;
            for (unsigned k = 0; k < 3; ++k) out.nrm[k] = component(src + k * cs, f[a].type, frac);
            out.has_nrm = true;
        } else if (a == kClr0 || a == kClr1) {
            out.col[a - kClr0] = vertex_color(src, f[a].type), out.has_col[a - kClr0] = true;
        } else {
            const unsigned t = a - kTex0;
            for (unsigned k = 0; k < (f[a].cnt ? 2u : 1u); ++k) out.tex[t][k] = component(src + k * cs, f[a].type, f[a].frac);
        }
    }
}

// Matrix memory rows of four (position and texture matrices).
void mul34(const float* m, const float in[4], float out[3]) {
    for (unsigned r = 0; r < 3; ++r) out[r] = m[r * 4] * in[0] + m[r * 4 + 1] * in[1] + m[r * 4 + 2] * in[2] + m[r * 4 + 3] * in[3];
}

// A colour channel pair (GX's lighting): COLOR0/1 for RGB, ALPHA0/1 for
// alpha. The material and ambient come from the vertex or the registers;
// lights add diffuse times attenuation (spot or none) in view space.
void lit_color(const State& s, unsigned which, const InVertex& in, const float view[3], const float normal[3], float out[4]) {
    const Rgba& vc = in.col[which];
    const float vtx[4] = {vc.r, vc.g, vc.b, vc.a};
    for (unsigned part = 0; part < 2; ++part) {  // 0: RGB (COLOR0/1), 1: alpha (ALPHA0/1)
        const ChanCtrl& ch = s.chan[which + 2 * part];
        const Color& mat_reg = s.mat[which];
        const Color& amb_reg = s.amb[which];
        const float mreg[4] = {float(mat_reg.r), float(mat_reg.g), float(mat_reg.b), float(mat_reg.a)};
        const float areg[4] = {float(amb_reg.r), float(amb_reg.g), float(amb_reg.b), float(amb_reg.a)};
        const unsigned k0 = part == 0 ? 0 : 3, k1 = part == 0 ? 3 : 4;
        float light[4] = {0, 0, 0, 0};
        if (ch.enable) {
            for (unsigned k = k0; k < k1; ++k) light[k] = (ch.amb_src ? vtx[k] : areg[k]) / 255.0f;
            for (unsigned l = 0; l < 8; ++l) {
                if (!(ch.light_mask & (1u << l))) continue;
                const Light& L = s.lights[l];
                float d[3] = {L.pos[0] - view[0], L.pos[1] - view[1], L.pos[2] - view[2]};
                const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (dist > 0) d[0] /= dist, d[1] /= dist, d[2] /= dist;
                float diffuse = 1.0f;
                if (ch.diff_fn != 0) {
                    const float dot = d[0] * normal[0] + d[1] * normal[1] + d[2] * normal[2];
                    diffuse = ch.diff_fn == 2 ? std::max(dot, 0.0f) : dot;
                }
                float attn = 1.0f;
                if (ch.attn_fn == 1) {  // GX_AF_SPOT
                    const float cosv = d[0] * L.dir[0] + d[1] * L.dir[1] + d[2] * L.dir[2];
                    const float ang = std::max(0.0f, L.a[0] + L.a[1] * cosv + L.a[2] * cosv * cosv);
                    const float den = L.k[0] + L.k[1] * dist + L.k[2] * dist * dist;
                    attn = den != 0 ? ang / den : 0.0f;
                } else if (ch.attn_fn == 0) {
                    unsupported("specular lighting", l);
                }
                const float lc[4] = {float(L.color.r), float(L.color.g), float(L.color.b), float(L.color.a)};
                for (unsigned k = k0; k < k1; ++k) light[k] += lc[k] / 255.0f * diffuse * attn;
            }
        }
        for (unsigned k = k0; k < k1; ++k) {
            const float mat = ch.mat_src ? vtx[k] : mreg[k];
            out[k] = ch.enable ? mat * std::clamp(light[k], 0.0f, 1.0f) : mat;
        }
    }
}

void transform(const State& s, const InVertex& in, GpuVertex& out) {
    // A position matrix is three rows of matrix memory (64 rows); its normal
    // matrix three rows of normal memory (32 rows of three).
    if (in.pn_idx > 61) unsupported("a position matrix index", in.pn_idx);
    if (in.has_nrm && size_t(in.pn_idx) * 3 + 9 > s.nrm_mtx.size()) unsupported("a normal matrix index", in.pn_idx);
    const float* pm = &s.mtx[size_t(in.pn_idx) * 4];
    const float p4[4] = {in.pos[0], in.pos[1], in.pos[2], 1.0f};
    float view[3];
    mul34(pm, p4, view);
    const float* nm = &s.nrm_mtx[size_t(in.pn_idx) * 3];
    float normal[3] = {0, 0, 0};
    if (in.has_nrm) {
        for (unsigned r = 0; r < 3; ++r) normal[r] = nm[r * 3] * in.nrm[0] + nm[r * 3 + 1] * in.nrm[1] + nm[r * 3 + 2] * in.nrm[2];
        const float len = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
        if (len > 0) normal[0] /= len, normal[1] /= len, normal[2] /= len;
    }
    // Projection into GX's clip space (z/w from -1 at near to 0 at far),
    // then Vulkan's: y down, z/w from 0 to 1.
    const auto& P = s.projection;
    float x, y, z, w;
    if (s.projection_type == 0) {
        x = P[0] * view[0] + P[1] * view[2], y = P[2] * view[1] + P[3] * view[2];
        z = P[4] * view[2] + P[5], w = -view[2];
    } else {
        x = P[0] * view[0] + P[1], y = P[2] * view[1] + P[3], z = P[4] * view[2] + P[5], w = 1.0f;
    }
    out.pos[0] = x, out.pos[1] = -y, out.pos[2] = z + w, out.pos[3] = w;

    float c0[4] = {0, 0, 0, 0}, c1[4] = {0, 0, 0, 0};
    if (s.num_chans >= 1) lit_color(s, 0, in, view, normal, c0);
    if (s.num_chans >= 2) lit_color(s, 1, in, view, normal, c1);
    std::memcpy(out.col0, c0, sizeof c0);
    std::memcpy(out.col1, c1, sizeof c1);

    for (unsigned i = 0; i < 8; ++i) out.tex[i][0] = out.tex[i][1] = 0, out.tex[i][2] = 1;
    for (unsigned i = 0; i < s.num_texgens; ++i) {
        const TexGen& tg = s.texgen[i];
        float src[4] = {0, 0, 1, 1};
        if (tg.src == 0) src[0] = in.pos[0], src[1] = in.pos[1], src[2] = in.pos[2];
        else if (tg.src == 1) src[0] = in.nrm[0], src[1] = in.nrm[1], src[2] = in.nrm[2];
        else if (tg.src >= 4 && tg.src <= 11) src[0] = in.tex[tg.src - 4][0], src[1] = in.tex[tg.src - 4][1];
        else if (tg.type != 10) unsupported("a texture coordinate source", tg.src);
        float stq[3];
        if (tg.type == 10) {  // GX_TG_SRTG: colour 0's red and green
            stq[0] = c0[0] / 255.0f, stq[1] = c0[1] / 255.0f, stq[2] = 1.0f;
        } else if (tg.type <= 1) {
            const uint32_t id = in.has_tex_idx[i] ? in.tex_idx[i] : tg.mtx;
            if (id == 60) {  // GX_IDENTITY
                stq[0] = src[0], stq[1] = src[1], stq[2] = tg.type == 0 ? src[2] : 1.0f;
            } else {
                if (id > 61) unsupported("a texture matrix", id);
                float r[3];
                mul34(&s.mtx[size_t(id) * 4], src, r);
                stq[0] = r[0], stq[1] = r[1], stq[2] = tg.type == 0 ? r[2] : 1.0f;
            }
        } else {
            unsupported("a bump-map texture coordinate", tg.type);
        }
        if (tg.normalize || tg.pt_mtx != 125) {  // the post-transform (dual texture) stage
            if (tg.normalize) {
                const float len = std::sqrt(stq[0] * stq[0] + stq[1] * stq[1] + stq[2] * stq[2]);
                if (len > 0) stq[0] /= len, stq[1] /= len, stq[2] /= len;
            }
            if (tg.pt_mtx != 125) {
                if (tg.pt_mtx < 64 || tg.pt_mtx > 125) unsupported("a post-transform matrix", tg.pt_mtx);
                const float v4[4] = {stq[0], stq[1], stq[2], 1.0f};
                mul34(&s.post_mtx[size_t(tg.pt_mtx - 64) * 4], v4, stq);
            }
        }
        out.tex[i][0] = stq[0], out.tex[i][1] = stq[1], out.tex[i][2] = stq[2];
    }
}

// ---- the draw's uniform block ----
void pack_draw(const State& s, DrawBlock& b) {
    std::memset(&b, 0, sizeof b);
    for (unsigned i = 0; i < 4; ++i) {
        const ColorS10& r = s.tev_color[i];
        b.regs[i][0] = r.r, b.regs[i][1] = r.g, b.regs[i][2] = r.b, b.regs[i][3] = r.a;
        const Color& k = s.kcolor[i];
        b.konst[i][0] = k.r, b.konst[i][1] = k.g, b.konst[i][2] = k.b, b.konst[i][3] = k.a;
        for (unsigned c = 0; c < 4; ++c) b.swaps[i][c] = s.swap_table[i][c];
    }
    b.misc[0] = s.num_tev_stages;
    b.misc[1] = uint32_t(s.alpha_comp0 & 7) | uint32_t(s.alpha_comp1 & 7) << 3 | uint32_t(s.alpha_op & 3) << 6;
    b.misc[2] = uint32_t(s.alpha_ref0) | uint32_t(s.alpha_ref1) << 8;
    // APPROX: destination alpha only where blending doesn't read the source's.
    if (s.dst_alpha_enable && s.blend_type != 1) b.misc[3] = 1u | uint32_t(s.dst_alpha) << 8;
    // Fog (GXSetFog's start, end, near and far): with the depth zs from 0
    // at near to 1 at far, the distance into the fog (eye distance - start) /
    // (end - start) is A / (B - zs) - C in perspective (A = far * near /
    // ((far - near) (end - start)), B = far / (far - near), C = start /
    // (end - start)) and A * zs - C orthographic (A = (far - near) / (end -
    // start), C = (start - near) / (end - start)).
    if (s.fog_type != 0) {
        // GXFogType: 2, 4-7 perspective, 10, 12-15 orthographic.
        if (s.fog_type == 1 || s.fog_type == 3 || s.fog_type == 8 || s.fog_type == 9 || s.fog_type == 11 || s.fog_type > 15)
            unsupported("a fog type", s.fog_type);
        const float n = s.fog_near, fr = s.fog_far, range = s.fog_end - s.fog_start;
        if (range == 0.0f || fr == n) unsupported("fog with an empty range", 0);
        b.fog[0] = s.fog_type;
        b.fog[1] = uint32_t(s.fog_color.r) | uint32_t(s.fog_color.g) << 8 | uint32_t(s.fog_color.b) << 16;
        if (s.fog_type & 8) {
            b.fog_abc[0] = (fr - n) / range, b.fog_abc[2] = (s.fog_start - n) / range;
        } else {
            b.fog_abc[0] = fr * n / ((fr - n) * range), b.fog_abc[1] = fr / (fr - n), b.fog_abc[2] = s.fog_start / range;
        }
    }
    b.depth_range[0] = std::clamp(s.viewport[4], 0.0f, 1.0f), b.depth_range[1] = std::clamp(s.viewport[5], 0.0f, 1.0f);
    for (unsigned i = 0; i < 8; ++i)
        if (s.tex_scale_manual[i]) b.tcscale[i][0] = 1, b.tcscale[i][1] = s.tex_scale_s[i], b.tcscale[i][2] = s.tex_scale_t[i];
    for (unsigned i = 0; i < s.num_tev_stages; ++i) {
        const TevStage& t = s.tev[i];
        // Indirect texturing changes a stage's coordinates only through a
        // matrix, a wrap or adding the previous stage's; with none of them
        // (GX_ITM_OFF, GX_ITW_OFF, no add) the format and bias do nothing.
        if (t.ind_mtx || t.ind_wrap_s || t.ind_wrap_t || t.ind_add_prev) {
            std::fprintf(stderr, "qilin_run: GX: stage %u indirect: stage %u format %u bias %u mtx %u wrap %u/%u add %u\n", i,
                         t.ind_stage, t.ind_format, t.ind_bias, t.ind_mtx, t.ind_wrap_s, t.ind_wrap_t, t.ind_add_prev);
            unsupported("an indirect texture stage", i);
        }
        // Values the shader would misread are stopped here: GXTevKColorSel
        // 0-7 and 12-31, GXTevKAlphaSel 0-7 and 16-31, ops 0-1 and 8-15, the
        // raster channels COLOR0A0, COLOR1A1, COLOR_ZERO and COLOR_NULL.
        if (t.kcolor_sel > 31 || (t.kcolor_sel >= 8 && t.kcolor_sel < 12)) unsupported("a TEV konst colour selection", t.kcolor_sel);
        if (t.kalpha_sel > 31 || (t.kalpha_sel >= 8 && t.kalpha_sel < 16)) unsupported("a TEV konst alpha selection", t.kalpha_sel);
        if (t.color_op > 15 || (t.color_op >= 2 && t.color_op < 8)) unsupported("a TEV colour op", t.color_op);
        if (t.alpha_op > 15 || (t.alpha_op >= 2 && t.alpha_op < 8)) unsupported("a TEV alpha op", t.alpha_op);
        if (t.channel != 4 && t.channel != 5 && t.channel != 6 && t.channel != 0xFF) unsupported("a TEV raster channel", t.channel);
        if ((t.coord != 0xFF && t.coord >= 8) || (t.map != 0xFF && t.map >= 8)) unsupported("a TEV coordinate or map", t.coord);
        if (t.color_out > 3 || t.alpha_out > 3) unsupported("a TEV output register", t.color_out);
        uint32_t* c = b.stages[i * 3];
        uint32_t* a = b.stages[i * 3 + 1];
        uint32_t* o = b.stages[i * 3 + 2];
        c[0] = uint32_t(t.color_in[0]) | uint32_t(t.color_in[1]) << 4 | uint32_t(t.color_in[2]) << 8 | uint32_t(t.color_in[3]) << 12;
        c[1] = uint32_t(t.color_op) | uint32_t(t.color_bias) << 4 | uint32_t(t.color_scale) << 6 |
               uint32_t(t.color_clamp) << 8 | uint32_t(t.color_out) << 9;
        c[2] = t.kcolor_sel, c[3] = uint32_t(t.ras_swap) | uint32_t(t.tex_swap) << 2;
        a[0] = uint32_t(t.alpha_in[0]) | uint32_t(t.alpha_in[1]) << 4 | uint32_t(t.alpha_in[2]) << 8 | uint32_t(t.alpha_in[3]) << 12;
        a[1] = uint32_t(t.alpha_op) | uint32_t(t.alpha_bias) << 4 | uint32_t(t.alpha_scale) << 6 |
               uint32_t(t.alpha_clamp) << 8 | uint32_t(t.alpha_out) << 9;
        const bool textured = t.map != 0xFF && t.coord != 0xFF && !t.tex_disabled;
        o[0] = textured ? (t.map & 7u) : 255u, o[1] = textured ? (t.coord & 7u) : 0u;
        o[2] = t.channel, o[3] = t.kalpha_sel;
        if (t.channel == 7 || t.channel == 8) unsupported("a bump alpha channel", t.channel);
    }
}

// ---- the sink ----
class Renderer final : public Sink {
public:
    void draw(const State& s, const Draw& d) override;
    void copy_display(const State& s, uint32_t xfb, bool clear) override;
    void copy_texture(const State& s, uint32_t dest, bool clear) override;
} g_renderer;

// Clears a rectangle of the EFB to GXSetCopyClear's colour and depth, as a
// copy with its clear flag does.
void clear_efb(const State& s, const uint16_t rect[4]) {
    begin_rendering();
    VkClearAttachment ca[2]{};
    ca[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ca[0].clearValue.color = {{s.clear_color.r / 255.0f, s.clear_color.g / 255.0f, s.clear_color.b / 255.0f, s.clear_color.a / 255.0f}};
    ca[1].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    ca[1].clearValue.depthStencil = {float(s.clear_z) / float(0xFFFFFF), 0};
    const uint32_t x = std::min<uint32_t>(rect[0], kEfbWidth), y = std::min<uint32_t>(rect[1], kEfbHeight);
    VkClearRect r{{{int32_t(x), int32_t(y)}, {std::min<uint32_t>(rect[2], kEfbWidth - x), std::min<uint32_t>(rect[3], kEfbHeight - y)}}, 0, 1};
    if (r.rect.extent.width && r.rect.extent.height) vkCmdClearAttachments(cur().cb, 2, ca, 1, &r);
    end_rendering();
}

// GXCopyTex: the EFB's source rectangle into a texture image the later
// GXLoadTexObj of `dest` picks up (texture()), converted as the format says,
// halved by a 2x2 box with the mipmap flag. The copy stays on the GPU.
void Renderer::copy_texture(const State& s, uint32_t dest, bool clear) {
    ++stats().texture_copies;
    const CopyFormat cf = copy_format(s.tex_dst_format);
    const uint32_t w = s.tex_dst_width, h = s.tex_dst_height;
    if (w == 0 || h == 0 || w > 1024 || h > 1024) unsupported("a texture copy's size", w);
    // The source inside the EFB, and a halving box's whole 2x2 blocks with it.
    if (uint32_t(s.tex_src[0]) + s.tex_src[2] > kEfbWidth || uint32_t(s.tex_src[1]) + s.tex_src[3] > kEfbHeight)
        unsupported("a texture copy from outside the EFB", s.tex_src[0]);
    if (s.tex_dst_mipmap && (2 * w > s.tex_src[2] || 2 * h > s.tex_src[3])) unsupported("a halving copy larger than its source", w);
    begin_frame();
    end_rendering();
    Frame& f = cur();
    EfbCopy& c = g_copies[norm_addr(dest)];
    if (c.image.img && (c.width != w || c.height != h)) g.garbage.emplace_back(g.frame_number, c.image), c.image = Image{};
    if (!c.image.img)
        c.image = make_image(w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT);
    c.width = w, c.height = h, c.format = s.tex_dst_format;
    c.guest_bytes = texture_bytes(cf.mem_format, w, h);
    c.guest_hash = hash_bytes(g_mem + norm_addr(dest), c.guest_bytes, 0);

    transition(f.cb, g.efb_color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    transition(f.cb, c.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    if (++f.sets > kSetsPerFrame) unsupported("draws in one frame", f.sets);
    VkDescriptorSetAllocateInfo ai = vk_struct<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    ai.descriptorPool = f.pool, ai.descriptorSetCount = 1, ai.pSetLayouts = &g.copy_set_layout;
    VkDescriptorSet set;
    vk_check(vkAllocateDescriptorSets(g.device, &ai, &set), "vkAllocateDescriptorSets");
    VkDescriptorImageInfo ii{g.efb_sampler, g.efb_color.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr = vk_struct<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
    wr.dstSet = set, wr.descriptorCount = 1, wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(g.device, 1, &wr, 0, nullptr);

    VkRenderingAttachmentInfo color = vk_struct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    color.imageView = c.image.view, color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = vk_struct<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
    ri.renderArea = {{0, 0}, {w, h}}, ri.layerCount = 1, ri.colorAttachmentCount = 1, ri.pColorAttachments = &color;
    vkCmdBeginRendering(f.cb, &ri);
    vkCmdBindPipeline(f.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.copy_pipeline);
    vkCmdBindDescriptorSets(f.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.copy_layout, 0, 1, &set, 0, nullptr);
    const VkViewport vp{0, 0, float(w), float(h), 0, 1};
    const VkRect2D sc{{0, 0}, {w, h}};
    vkCmdSetViewport(f.cb, 0, 1, &vp);
    vkCmdSetScissor(f.cb, 0, 1, &sc);
    const CopyBlock pc{{float(s.tex_src[0]), float(s.tex_src[1]), float(s.tex_src[2]), float(s.tex_src[3])},
                       {cf.mode, s.tex_dst_mipmap ? 1u : 0u, 0, 0}};
    vkCmdPushConstants(f.cb, g.copy_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDraw(f.cb, 3, 1, 0, 0);
    vkCmdEndRendering(f.cb);
    transition(f.cb, c.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    if (clear) clear_efb(s, s.tex_src);
}

void Renderer::draw(const State& s, const Draw& d) {
    ++stats().draws;
    stats().vertices += d.count;
    const uint8_t prim = d.primitive;
    const bool lines = prim == kLines || prim == kLineStrip, points = prim == kPoints;
    if (s.num_ind_stages != 0) unsupported("indirect texture stages", s.num_ind_stages);
    if (s.ztex_op != 0) unsupported("a Z texture", s.ztex_op);
    if (d.count == 0 || (s.cull == 3 && !lines && !points)) return;  // GX_CULL_ALL draws no triangles
    if (s.scissor_offset[0] != 0 || s.scissor_offset[1] != 0) unsupported("a scissor box offset", s.scissor_offset[0]);
    if (!(s.viewport[2] > 0) || !(s.viewport[3] > 0)) unsupported("a viewport without a positive size", 0);

    // vertices, transformed
    std::vector<GpuVertex> v(d.count);
    for (uint32_t i = 0; i < d.count; ++i) {
        InVertex in;
        read_vertex(s, d, d.vertices.data() + size_t(i) * d.vertex_size, in);
        transform(s, in, v[i]);
    }
    std::vector<uint32_t> order;
    std::vector<GpuVertex> wide;  // lines and points become quads in screen space
    switch (prim) {
    case kQuads:
    case kQuads2:
        for (uint32_t q = 0; q + 3 < d.count; q += 4) order.insert(order.end(), {q, q + 1, q + 2, q, q + 2, q + 3});
        break;
    case kTriangles:
        for (uint32_t i = 0; i + 2 < d.count; i += 3) order.insert(order.end(), {i, i + 1, i + 2});
        break;
    case kTriangleStrip:
        for (uint32_t i = 2; i < d.count; ++i) {
            if (i & 1) order.insert(order.end(), {i - 1, i - 2, i});
            else order.insert(order.end(), {i - 2, i - 1, i});
        }
        break;
    case kTriangleFan:
        for (uint32_t i = 2; i < d.count; ++i) order.insert(order.end(), {0u, i - 1, i});
        break;
    default: {
        // GX widens a line across its minor axis (vertically if it's more
        // horizontal than vertical), by the line width in sixths of a pixel;
        // a point is a square of the point size. Offsets are made in pixels
        // and taken back into clip space at each vertex's w. APPROX: the
        // texture coordinate offsets (GXSetLineWidth's second argument).
        const float vw = std::max(std::fabs(s.viewport[2]), 1.0f), vh = std::max(std::fabs(s.viewport[3]), 1.0f);
        auto shifted = [&](const GpuVertex& p, float dx_px, float dy_px) {
            GpuVertex q = p;
            q.pos[0] += dx_px * 2.0f / vw * p.pos[3];
            q.pos[1] += dy_px * 2.0f / vh * p.pos[3];
            return q;
        };
        if (points) {
            const float r = s.point_size / 2;
            for (const GpuVertex& p : v) {
                const GpuVertex a = shifted(p, -r, -r), b = shifted(p, r, -r), c = shifted(p, r, r), e = shifted(p, -r, r);
                wide.insert(wide.end(), {a, b, c, a, c, e});
            }
            break;
        }
        const float r = s.line_width / 2;
        const uint32_t step = prim == kLines ? 2 : 1;
        for (uint32_t i = 0; i + 1 < d.count; i += step) {
            const GpuVertex& a = v[i];
            const GpuVertex& b = v[i + 1];
            if (a.pos[3] == 0 || b.pos[3] == 0) continue;
            const float dx = (b.pos[0] / b.pos[3] - a.pos[0] / a.pos[3]) * vw, dy = (b.pos[1] / b.pos[3] - a.pos[1] / a.pos[3]) * vh;
            const bool horizontal = std::fabs(dx) >= std::fabs(dy);
            const float ox = horizontal ? 0 : r, oy = horizontal ? r : 0;
            const GpuVertex a0 = shifted(a, -ox, -oy), a1 = shifted(a, ox, oy), b0 = shifted(b, -ox, -oy), b1 = shifted(b, ox, oy);
            wide.insert(wide.end(), {a0, a1, b1, a0, b1, b0});
        }
        break;
    }
    }
    if (order.empty() && wide.empty()) return;
    if (!wide.empty()) {
        v = std::move(wide);
        order.resize(v.size());
        for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    }

    DrawBlock block;
    pack_draw(s, block);
    const uint32_t key = pipeline_key(s);
    VkImageView views[8];
    VkSampler samps[8];
    for (unsigned m = 0; m < 8; ++m) views[m] = g.blank.view, samps[m] = sampler(0);
    for (unsigned i = 0; i < s.num_tev_stages; ++i) {
        const TevStage& t = s.tev[i];
        if (t.map == 0xFF || t.coord == 0xFF || t.tex_disabled) continue;
        const TexMap& tm = s.tex[t.map & 7];
        if (!tm.loaded) unsupported("a TEV stage reading a texture map with nothing loaded", t.map);
        begin_frame();
        const Image& im = texture(s, tm);
        views[t.map & 7] = im.view;
        samps[t.map & 7] = sampler(tm.mode0, tm.mode1, im.levels);
    }

    begin_rendering();
    Frame& f = cur();
    const VkDeviceSize vbytes = order.size() * sizeof(GpuVertex);
    const VkDeviceSize vat = take(f, f.vtx_used, kVertexBytes, vbytes, 16, "vertices in one frame (bytes)");
    auto* dst = reinterpret_cast<GpuVertex*>(f.vtx.map + vat);
    for (size_t i = 0; i < order.size(); ++i) dst[i] = v[order[i]];
    const VkDeviceSize uat = take(f, f.ubo_used, kUniformBytes, sizeof block, g.ubo_align, "draws in one frame");
    std::memcpy(f.ubo.map + uat, &block, sizeof block);

    if (++f.sets > kSetsPerFrame) unsupported("draws in one frame", f.sets);
    VkDescriptorSetAllocateInfo ai = vk_struct<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    ai.descriptorPool = f.pool, ai.descriptorSetCount = 1, ai.pSetLayouts = &g.set_layout;
    VkDescriptorSet set;
    vk_check(vkAllocateDescriptorSets(g.device, &ai, &set), "vkAllocateDescriptorSets");
    VkDescriptorBufferInfo bi{f.ubo.buf, uat, sizeof block};
    VkDescriptorImageInfo ii[8];
    for (unsigned m = 0; m < 8; ++m) ii[m] = {samps[m], views[m], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w[2]{};
    w[0].sType = w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet = set, w[0].dstBinding = 0, w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, w[0].pBufferInfo = &bi;
    w[1].dstSet = set, w[1].dstBinding = 1, w[1].descriptorCount = 8;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, w[1].pImageInfo = ii;
    vkUpdateDescriptorSets(g.device, 2, w, 0, nullptr);

    VkCommandBuffer cb = f.cb;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline(key));
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 1, &set, 0, nullptr);
    vkCmdBindVertexBuffers(cb, 0, 1, &f.vtx.buf, &vat);
    VkViewport vp{s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3], s.viewport[4], s.viewport[5]};
    vp.minDepth = std::clamp(vp.minDepth, 0.0f, 1.0f), vp.maxDepth = std::clamp(vp.maxDepth, 0.0f, 1.0f);
    vkCmdSetViewport(cb, 0, 1, &vp);
    const int32_t sx = static_cast<int32_t>(std::min(s.scissor[0], kEfbWidth)), sy = static_cast<int32_t>(std::min(s.scissor[1], kEfbHeight));
    VkRect2D sc{{sx, sy},
                {std::min(s.scissor[2], kEfbWidth - static_cast<uint32_t>(sx)), std::min(s.scissor[3], kEfbHeight - static_cast<uint32_t>(sy))}};
    vkCmdSetScissor(cb, 0, 1, &sc);
    // GX culls by its screen-space winding: front faces are clockwise there,
    // as in Vulkan's framebuffer space (both y down).
    vkCmdSetCullMode(cb, lines || points ? VK_CULL_MODE_NONE  // GX culls triangles only
                         : s.cull == 1   ? VK_CULL_MODE_FRONT_BIT
                         : s.cull == 2   ? VK_CULL_MODE_BACK_BIT
                                         : VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(cb, VK_FRONT_FACE_CLOCKWISE);
    vkCmdSetDepthTestEnable(cb, s.z_enable ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthWriteEnable(cb, s.z_enable && s.z_update ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthCompareOp(cb, static_cast<VkCompareOp>(s.z_func & 7));  // GX's order is Vulkan's
    vkCmdDraw(cb, static_cast<uint32_t>(order.size()), 1, 0, 0);
}

// GXCopyDisp: the frame. The EFB's source rectangle goes to the window
// (keeping the console's aspect: 16:9 with its widescreen setting), then the
// EFB is cleared there if asked.
void Renderer::copy_display(const State& s, uint32_t, bool clear) {
    ++stats().display_copies;
    if (s.disp_src[2] == 0 || s.disp_src[3] == 0 || uint32_t(s.disp_src[0]) + s.disp_src[2] > kEfbWidth ||
        uint32_t(s.disp_src[1]) + s.disp_src[3] > kEfbHeight)
        unsupported("a display copy from outside the EFB", s.disp_src[0]);
    begin_frame();
    end_rendering();
    Frame& f = cur();
    VkCommandBuffer cb = f.cb;
    if (g.resized.exchange(false) || !g.swapchain) {
        vk_check(vkDeviceWaitIdle(g.device), "vkDeviceWaitIdle");
        make_swapchain();
    }
    uint32_t image = UINT32_MAX;
    if (g.swapchain) {
        const VkResult r = vkAcquireNextImageKHR(g.device, g.swapchain, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &image);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) g.resized = true, image = UINT32_MAX;
        else if (r != VK_SUBOPTIMAL_KHR) vk_check(r, "vkAcquireNextImageKHR");
    }
    if (image != UINT32_MAX) {
        const VkImage dst = g.swap_images[image];
        transition(cb, g.efb_color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        transition(cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        const VkClearColorValue black{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        const float aspect = console().widescreen ? 16.0f / 9.0f : 4.0f / 3.0f;
        float w = float(g.extent.width), h = float(g.extent.height);
        if (w / h > aspect) w = h * aspect;
        else h = w / aspect;
        const int32_t x0 = int32_t((float(g.extent.width) - w) / 2), y0 = int32_t((float(g.extent.height) - h) / 2);
        VkImageBlit blit{};
        blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        const int32_t sx = s.disp_src[0], sy = s.disp_src[1];
        blit.srcOffsets[0] = {sx, sy, 0};
        blit.srcOffsets[1] = {std::min<int32_t>(sx + s.disp_src[2], kEfbWidth), std::min<int32_t>(sy + s.disp_src[3], kEfbHeight), 1};
        blit.dstOffsets[0] = {x0, y0, 0};
        blit.dstOffsets[1] = {x0 + int32_t(w), y0 + int32_t(h), 1};
        vkCmdBlitImage(cb, g.efb_color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &blit, VK_FILTER_LINEAR);
        transition(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    // QILIN_FRAME_STATS=N: every Nth frame's EFB rectangle read back and
    // summed up (numbers only), to tell a drawn frame from an empty one
    // without looking at it.
    static const uint32_t stats_every = [] {
        const char* v = std::getenv("QILIN_FRAME_STATS");
        return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10)) : 0u;
    }();
    const uint32_t rw = std::min<uint32_t>(s.disp_src[2], kEfbWidth - std::min<uint32_t>(s.disp_src[0], kEfbWidth));
    const uint32_t rh = std::min<uint32_t>(s.disp_src[3], kEfbHeight - std::min<uint32_t>(s.disp_src[1], kEfbHeight));
    const bool sample = stats_every && g.frame_number % stats_every == 0 && rw && rh;
    if (sample) {
        transition(cb, g.efb_color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {int32_t(s.disp_src[0]), int32_t(s.disp_src[1]), 0};
        region.imageExtent = {rw, rh, 1};
        vkCmdCopyImageToBuffer(cb, g.efb_color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, f.readback.buf, 1, &region);
    }
    if (clear) clear_efb(s, s.disp_src);
    vk_check(vkEndCommandBuffer(cb), "vkEndCommandBuffer");
    VkCommandBufferSubmitInfo cbi = vk_struct<VkCommandBufferSubmitInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO);
    cbi.commandBuffer = cb;
    VkSemaphoreSubmitInfo wait = vk_struct<VkSemaphoreSubmitInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO);
    VkSemaphoreSubmitInfo signal = vk_struct<VkSemaphoreSubmitInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO);
    wait.semaphore = f.acquired, wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 si = vk_struct<VkSubmitInfo2>(VK_STRUCTURE_TYPE_SUBMIT_INFO_2);
    si.commandBufferInfoCount = 1, si.pCommandBufferInfos = &cbi;
    if (image != UINT32_MAX) {
        signal.semaphore = g.rendered[image], signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        si.waitSemaphoreInfoCount = 1, si.pWaitSemaphoreInfos = &wait;
        si.signalSemaphoreInfoCount = 1, si.pSignalSemaphoreInfos = &signal;
    }
    vk_check(vkQueueSubmit2(g.queue, 1, &si, f.fence), "vkQueueSubmit2");
    g.recording = false;
    if (image != UINT32_MAX) {
        VkPresentInfoKHR pi = vk_struct<VkPresentInfoKHR>(VK_STRUCTURE_TYPE_PRESENT_INFO_KHR);
        pi.waitSemaphoreCount = 1, pi.pWaitSemaphores = &g.rendered[image];
        pi.swapchainCount = 1, pi.pSwapchains = &g.swapchain, pi.pImageIndices = &image;
        const VkResult r = vkQueuePresentKHR(g.queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g.resized = true;
        else vk_check(r, "vkQueuePresentKHR");
        ++stats().frames_presented;
    }
    if (sample) {
        vk_check(vkWaitForFences(g.device, 1, &f.fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        uint64_t lit = 0, sum[3] = {};
        uint8_t seen[4096] = {};
        uint32_t colours = 0;
        for (uint32_t i = 0; i < rw * rh; ++i) {
            const uint8_t* p = f.readback.map + 4 * size_t(i);
            if (p[0] | p[1] | p[2]) ++lit;
            for (unsigned k = 0; k < 3; ++k) sum[k] += p[k];
            const uint32_t bucket = uint32_t(p[0] >> 4) << 8 | uint32_t(p[1] >> 4) << 4 | uint32_t(p[2] >> 4);
            if (!seen[bucket]) seen[bucket] = 1, ++colours;
        }
        const double n = double(rw) * rh;
        std::fprintf(stderr, "qilin_run: frame %llu: %ux%u, %.1f%% not black, mean colour %.0f %.0f %.0f, %u colour buckets\n",
                     static_cast<unsigned long long>(g.frame_number), rw, rh, 100.0 * double(lit) / n, double(sum[0]) / n,
                     double(sum[1]) / n, double(sum[2]) / n, colours);
    }
    g.frame = (g.frame + 1) % kFrames;
    ++g.frame_number;
}

// ---- start-up ----
bool fail(const char* what) {
    std::fprintf(stderr, "qilin_run: Vulkan: %s\n", what);
    return false;
}

bool create_device() {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    vkEnumeratePhysicalDevices(g.instance, &n, gpus.data());
    int best = -1;
    for (VkPhysicalDevice gpu : gpus) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(gpu, &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, qs.data());
        for (uint32_t q = 0; q < qn; ++q) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(gpu, q, g.surface, &present);
            if (!(qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            const int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > best) best = score, g.gpu = gpu, g.family = q;
            break;
        }
    }
    if (!g.gpu) return fail("no GPU with Vulkan 1.3 can draw to this window");
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g.gpu, &props);
    g.ubo_align = std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment, 16);
    std::fprintf(stderr, "qilin_run: Vulkan on %s\n", props.deviceName);

    VkPhysicalDeviceVulkan13Features f13 = vk_struct<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    VkPhysicalDeviceFeatures2 f2 = vk_struct<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    f2.pNext = &f13;
    vkGetPhysicalDeviceFeatures2(g.gpu, &f2);
    if (!f13.dynamicRendering || !f13.synchronization2) return fail("the GPU lacks dynamic rendering or synchronization2");
    VkPhysicalDeviceVulkan13Features want13 = vk_struct<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
    want13.dynamicRendering = VK_TRUE, want13.synchronization2 = VK_TRUE;

    std::vector<const char*> exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    vkEnumerateDeviceExtensionProperties(g.gpu, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> avail(n);
    vkEnumerateDeviceExtensionProperties(g.gpu, nullptr, &n, avail.data());
    for (const auto& e : avail)  // MoltenVK: must be enabled where it exists
        if (std::strcmp(e.extensionName, "VK_KHR_portability_subset") == 0) exts.push_back("VK_KHR_portability_subset");
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qi = vk_struct<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
    qi.queueFamilyIndex = g.family, qi.queueCount = 1, qi.pQueuePriorities = &priority;
    VkDeviceCreateInfo di = vk_struct<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
    di.pNext = &want13, di.queueCreateInfoCount = 1, di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = static_cast<uint32_t>(exts.size()), di.ppEnabledExtensionNames = exts.data();
    if (vkCreateDevice(g.gpu, &di, nullptr, &g.device) != VK_SUCCESS) return fail("vkCreateDevice failed");
#define QILIN_VK_LOAD_DEVICE(f)                                                                     \
    f = reinterpret_cast<PFN_##f>(vkGetDeviceProcAddr(g.device, #f));                              \
    if (!f) return fail("device function " #f " is missing");
    QILIN_VK_DEVICE(QILIN_VK_LOAD_DEVICE)
#undef QILIN_VK_LOAD_DEVICE
    vkGetDeviceQueue(g.device, g.family, 0, &g.queue);
    vkGetPhysicalDeviceMemoryProperties(g.gpu, &g.memory);
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g.gpu, kEfbDepth, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) return fail("D32_SFLOAT depth isn't supported");
    return true;
}

bool create_resources() {
    VkCommandPoolCreateInfo pi = vk_struct<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, pi.queueFamilyIndex = g.family;
    vk_check(vkCreateCommandPool(g.device, &pi, nullptr, &g.cmd_pool), "vkCreateCommandPool");
    for (Frame& f : g.frames) {
        VkCommandBufferAllocateInfo ai = vk_struct<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        ai.commandPool = g.cmd_pool, ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, ai.commandBufferCount = 1;
        vk_check(vkAllocateCommandBuffers(g.device, &ai, &f.cb), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fi = vk_struct<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vk_check(vkCreateFence(g.device, &fi, nullptr, &f.fence), "vkCreateFence");
        VkSemaphoreCreateInfo si = vk_struct<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
        vk_check(vkCreateSemaphore(g.device, &si, nullptr, &f.acquired), "vkCreateSemaphore");
        const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSetsPerFrame},
                                               {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSetsPerFrame * 8}};
        VkDescriptorPoolCreateInfo dp = vk_struct<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
        dp.maxSets = kSetsPerFrame, dp.poolSizeCount = 2, dp.pPoolSizes = sizes;
        vk_check(vkCreateDescriptorPool(g.device, &dp, nullptr, &f.pool), "vkCreateDescriptorPool");
        f.vtx = make_buffer(kVertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        f.ubo = make_buffer(kUniformBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        f.upload = make_buffer(kUploadBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        f.readback = make_buffer(VkDeviceSize(kEfbWidth) * kEfbHeight * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    }
    g.efb_color = make_image(kEfbWidth, kEfbHeight, kEfbColor,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT);
    g.efb_depth = make_image(kEfbWidth, kEfbHeight, kEfbDepth,
                             VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    g.blank = make_image(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT);

    VkDescriptorSetLayoutBinding b[2]{};
    b[0].binding = 0, b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1].binding = 1, b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, b[1].descriptorCount = 8;
    b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li = vk_struct<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    li.bindingCount = 2, li.pBindings = b;
    vk_check(vkCreateDescriptorSetLayout(g.device, &li, nullptr, &g.set_layout), "vkCreateDescriptorSetLayout");
    VkPipelineLayoutCreateInfo pl = vk_struct<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    pl.setLayoutCount = 1, pl.pSetLayouts = &g.set_layout;
    vk_check(vkCreatePipelineLayout(g.device, &pl, nullptr, &g.layout), "vkCreatePipelineLayout");
    g.vert = shader(k_gx_vert, sizeof k_gx_vert);
    g.frag = shader(k_gx_frag, sizeof k_gx_frag);
    create_copy_pipeline();

    // The first frame: the EFB starts black (what GX's memory holds after
    // the SDK's start-up clears is the game's business), the blank texture
    // transparent black.
    begin_frame();
    Frame& f = cur();
    transition(f.cb, g.efb_color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    const VkClearColorValue black{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(f.cb, g.efb_color.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    // Depth starts at the far plane (GX's clear Z default), not undefined.
    transition(f.cb, g.efb_depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    const VkClearDepthStencilValue far{1.0f, 0};
    const VkImageSubresourceRange depth_range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vkCmdClearDepthStencilImage(f.cb, g.efb_depth.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &far, 1, &depth_range);
    transition(f.cb, g.blank, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    vkCmdClearColorImage(f.cb, g.blank.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    transition(f.cb, g.blank, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    return true;
}

}  // namespace

void window_resized() { g.resized = true; }

bool init_vulkan(SDL_Window* window) {
    g.window = window;
    vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    if (!vkGetInstanceProcAddr) return fail("no Vulkan loader");
#define QILIN_VK_LOAD_GLOBAL(f) f = reinterpret_cast<PFN_##f>(vkGetInstanceProcAddr(nullptr, #f));
    QILIN_VK_GLOBAL(QILIN_VK_LOAD_GLOBAL)
#undef QILIN_VK_LOAD_GLOBAL
    uint32_t version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&version);
    if (version < VK_API_VERSION_1_3) return fail("the Vulkan loader is older than 1.3");

    Uint32 count = 0;
    const char* const* sdl_exts = SDL_Vulkan_GetInstanceExtensions(&count);
    std::vector<const char*> exts(sdl_exts, sdl_exts + count);
    VkInstanceCreateInfo ii = vk_struct<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
#ifdef __APPLE__
    exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);  // MoltenVK
    ii.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif
    VkApplicationInfo app = vk_struct<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
    app.pApplicationName = "qilin_run", app.apiVersion = VK_API_VERSION_1_3;
    ii.pApplicationInfo = &app;
    ii.enabledExtensionCount = static_cast<uint32_t>(exts.size()), ii.ppEnabledExtensionNames = exts.data();
    if (vkCreateInstance(&ii, nullptr, &g.instance) != VK_SUCCESS) return fail("vkCreateInstance failed");
#define QILIN_VK_LOAD_INSTANCE(f)                                                                   \
    f = reinterpret_cast<PFN_##f>(vkGetInstanceProcAddr(g.instance, #f));                          \
    if (!f) return fail("instance function " #f " is missing");
    QILIN_VK_INSTANCE(QILIN_VK_LOAD_INSTANCE)
#undef QILIN_VK_LOAD_INSTANCE
    if (!SDL_Vulkan_CreateSurface(window, g.instance, nullptr, &g.surface)) {
        std::fprintf(stderr, "qilin_run: Vulkan: no surface for the window: %s\n", SDL_GetError());
        return false;
    }
    try {
        if (!create_device() || !create_resources()) return false;
        make_swapchain();
    } catch (const Stop&) {
        return false;
    }
    set_sink(&g_renderer);
    return true;
}

}  // namespace qilin::rt::gx
