// GX on Vulkan 1.3 (gx_vulkan.cpp): the renderer behind GX's Sink.
// Runtime code: no std::format.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct SDL_Window;

namespace qilin::rt::gx {

// Creates the renderer on the window (on the main thread) and makes it GX's
// sink. False, with the reason on stderr, if Vulkan 1.3 or a feature the
// renderer needs is missing: there is no fallback.
bool init_vulkan(SDL_Window* window);
// The window's drawable size changed: the swapchain is remade at the next frame.
void window_resized();
// The display lock is on at `speed` (host.cpp, before the game's threads
// start): from here each frame's present is steered to land just before a
// vblank (Windows: the compositor's vblank times), by nudging the game's
// clock within 0.2% of that speed.
void lock_frames_to_vblank(double speed);
// The vblank lock's numbers for the run report (empty if it isn't on).
std::string vblank_report();

// ---- for the settings menu (menu.cpp) ----
// What ImGui's Vulkan backend needs to draw into the window's images
// (Vulkan handles, as void*: this header doesn't include Vulkan's). False
// if the window's images can't be drawn into: no menu then.
struct OverlayTarget {
    void *instance, *physical_device, *device, *queue;
    uint32_t queue_family, images, format;
    void (*get_instance_proc_addr)();  // PFN_vkGetInstanceProcAddr, as the renderer loaded it
};
bool overlay_target(OverlayTarget& t);
// Something to draw over the picture in each frame (a VkCommandBuffer
// inside a rendering pass on the window's image); empty: nothing.
void set_overlay(std::function<void(void* command_buffer)> draw);
// While the game is parked (pause.hpp), on the main thread: a frame of the
// picture last shown with the overlay over it.
void present_paused();
// While the game is parked: the settings' internal resolution, anisotropic
// filtering and vsync take effect.
void apply_display_settings();
// The GPUs that can draw to the window, and the one in use.
const std::vector<std::string>& gpu_names();
std::string gpu_in_use();
// The GPU's anisotropic filtering limit (1: it has none).
float anisotropy_limit();

}  // namespace qilin::rt::gx
