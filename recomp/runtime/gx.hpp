// GX, native at its API (docs/runtime.md): the state the game's GX calls set
// up, kept as the API describes it (not as Hollywood's registers), and the
// draws decoded from the vertex stream lifted code writes to the FIFO. What
// draws it (Vulkan) reads this state through a Sink. Runtime code: no
// std::format.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace qilin::rt::gx {

// ---- the API's numbers ----
// Vertex attributes (GXAttr).
enum : uint32_t {
    kPnMtxIdx = 0, kTex0MtxIdx = 1, kPos = 9, kNrm = 10, kClr0 = 11, kClr1 = 12, kTex0 = 13,
    kPosMtxArray = 21, kNrmMtxArray = 22, kTexMtxArray = 23, kLightArray = 24, kNbt = 25,
    kAttrCount = 26, kAttrNull = 0xFF,
};
// How an attribute arrives (GXAttrType).
enum : uint8_t { kNone = 0, kDirect = 1, kIndex8 = 2, kIndex16 = 3 };
// Component types; colours use their own (GXCompType).
enum : uint8_t { kU8 = 0, kS8 = 1, kU16 = 2, kS16 = 3, kF32 = 4 };
enum : uint8_t { kRgb565 = 0, kRgb8 = 1, kRgbx8 = 2, kRgba4 = 3, kRgba6 = 4, kRgba8 = 5 };
// Primitives (GXPrimitive): the command byte, with the vertex format in the low 3 bits.
enum : uint8_t {
    kQuads = 0x80, kQuads2 = 0x88, kTriangles = 0x90, kTriangleStrip = 0x98, kTriangleFan = 0xA0,
    kLines = 0xA8, kLineStrip = 0xB0, kPoints = 0xB8,
};

struct Color {
    uint8_t r = 0, g = 0, b = 0, a = 0;
};
struct ColorS10 {
    int16_t r = 0, g = 0, b = 0, a = 0;
};

struct AttrFmt {
    uint8_t cnt = 0, type = 0, frac = 0;
};

// A texture as GXLoadTexObj gave it to a map.
struct TexMap {
    bool loaded = false;
    uint32_t image = 0;  // guest address of the texels
    uint16_t width = 0, height = 0;
    uint32_t format = 0;
    uint8_t wrap_s = 0, wrap_t = 0;
    bool mipmap = false;
    uint32_t tlut = 0;
    uint32_t mode0 = 0, mode1 = 0;  // the object's filter and LOD words, decoded by the renderer
};

struct Tlut {
    bool loaded = false;
    uint32_t data = 0, format = 0, entries = 0;
};

struct TevStage {
    uint8_t color_in[4] = {15, 15, 15, 15}, alpha_in[4] = {7, 7, 7, 7};  // ZERO
    uint8_t color_op = 0, color_bias = 0, color_scale = 0, color_out = 0;
    uint8_t alpha_op = 0, alpha_bias = 0, alpha_scale = 0, alpha_out = 0;
    bool color_clamp = true, alpha_clamp = true;
    uint8_t coord = 0xFF, map = 0xFF, channel = 0xFF;  // GXSetTevOrder
    bool tex_disabled = false;  // GX_TEX_DISABLE with the map: the coordinate kept, no texture looked up
    uint8_t ras_swap = 0, tex_swap = 0;
    uint8_t kcolor_sel = 0, kalpha_sel = 0;
    // GXSetTevIndirect
    uint8_t ind_stage = 0, ind_format = 0, ind_bias = 0, ind_mtx = 0, ind_wrap_s = 0, ind_wrap_t = 0;
    bool ind_add_prev = false, ind_utc_lod = false;
    uint8_t ind_alpha = 0;
};

struct TexGen {
    uint8_t type = 1, src = 4, normalize = 0;  // GX_TG_MTX2x4, GX_TG_TEX0
    uint32_t mtx = 60, pt_mtx = 125;           // GX_IDENTITY, GX_PTIDENTITY
};

struct ChanCtrl {
    bool enable = false;
    uint8_t amb_src = 0, mat_src = 0, diff_fn = 0, attn_fn = 0;
    uint32_t light_mask = 0;
};

struct Light {
    bool loaded = false;
    float pos[3] = {}, dir[3] = {}, a[3] = {}, k[3] = {};
    Color color;
};

struct State {
    // vertices
    std::array<uint8_t, kAttrCount> desc{};  // how each attribute arrives
    bool nbt = false;                        // normals with binormal and tangent
    std::array<std::array<AttrFmt, kAttrCount>, 8> fmt{};
    struct Array {
        uint32_t base = 0;
        uint8_t stride = 0;
    };
    std::array<Array, kAttrCount> arrays{};
    // transforms: the transform unit's matrix memory, as GXLoad*MtxImm fills it
    std::array<float, 256> mtx{};       // position and texture matrices (row-major 4-wide rows)
    std::array<float, 96> nrm_mtx{};    // normal matrices (3x3)
    std::array<float, 256> post_mtx{};  // post-transform texture matrices
    uint32_t current_mtx = 0;
    std::array<uint32_t, 8> tex_mtx{};  // GXSetCurrentMtx doesn't set these; texgens do
    uint32_t projection_type = 0;       // 0 perspective, 1 orthographic
    std::array<float, 6> projection{};
    float viewport[6] = {};             // x, y, w, h, near, far
    bool viewport_jitter = false;
    uint32_t scissor[4] = {}, scissor_offset[2] = {};
    uint8_t cull = 0, clip = 0;
    bool coplanar = false;
    // lighting
    uint8_t num_chans = 0;
    std::array<ChanCtrl, 4> chan{};  // COLOR0, COLOR1, ALPHA0, ALPHA1
    std::array<Color, 2> amb{}, mat{};
    std::array<Light, 8> lights{};
    // texture coordinates
    uint8_t num_texgens = 0;
    std::array<TexGen, 8> texgen{};
    std::array<bool, 8> tex_offset_lines{}, tex_offset_points{};
    std::array<bool, 8> tex_scale_manual{};
    std::array<uint16_t, 8> tex_scale_s{}, tex_scale_t{};
    std::array<bool, 8> cyl_wrap_s{}, cyl_wrap_t{};
    // TEV
    uint8_t num_tev_stages = 0, num_ind_stages = 0;
    std::array<TevStage, 16> tev{};
    std::array<ColorS10, 4> tev_color{};  // PREV, REG0, REG1, REG2
    std::array<Color, 4> kcolor{};
    std::array<std::array<uint8_t, 4>, 4> swap_table{};
    struct IndStage {
        uint8_t coord = 0, map = 0, scale_s = 0, scale_t = 0;
    };
    std::array<IndStage, 4> ind{};
    struct IndMtx {
        float m[6] = {};
        int8_t scale_exp = 0;
    };
    std::array<IndMtx, 3> ind_mtx{};
    // pixels
    uint8_t alpha_comp0 = 7, alpha_ref0 = 0, alpha_op = 0, alpha_comp1 = 7, alpha_ref1 = 0;
    bool z_enable = true, z_update = true;
    uint8_t z_func = 3;
    bool z_comp_before_tex = true;
    uint8_t blend_type = 0, blend_src = 4, blend_dst = 5, logic_op = 0;
    bool color_update = true, alpha_update = true, dither = true;
    bool dst_alpha_enable = false;
    uint8_t dst_alpha = 0;
    uint8_t fog_type = 0;
    float fog_start = 0, fog_end = 0, fog_near = 0, fog_far = 0;
    Color fog_color;
    bool fog_range_adj = false;
    uint16_t fog_range_center = 0;
    uint8_t ztex_op = 0, ztex_fmt = 0;
    uint32_t ztex_bias = 0;
    uint8_t pixel_fmt = 0, z_fmt = 0;
    float line_width = 0, point_size = 0;
    uint8_t line_tex_offset = 0, point_tex_offset = 0;
    // textures
    std::array<TexMap, 8> tex{};
    uint64_t tex_loads = 0;  // texture and TLUT loads and invalidations so far: the renderer re-checks after one
    std::array<Tlut, 20> tlut{};
    // copies
    Color clear_color;
    uint32_t clear_z = 0xFFFFFF;
    uint16_t disp_src[4] = {}, disp_dst_width = 0, disp_dst_height = 0;
    uint32_t disp_yscale = 256;  // 256 / scale, 9 bits
    uint16_t tex_src[4] = {}, tex_dst_width = 0, tex_dst_height = 0;
    uint32_t tex_dst_format = 0;
    bool tex_dst_mipmap = false;
    uint8_t copy_clamp = 0, gamma = 0;
    bool frame2field = false;
    bool aa = false;
    uint8_t sample_pattern[12][2] = {};
    bool vfilter_enable = false;
    uint8_t vfilter[7] = {};
    uint8_t field_mask_odd = 1, field_mask_even = 1, field_mode = 0;
    bool half_aspect = false;
};

State& state();

// A draw, decoded: its primitive, its vertex format and the raw vertex
// bytes as they arrived (direct attributes; indices for indexed ones). The
// renderer decodes the attributes with the state at the time of the draw.
struct Draw {
    uint8_t primitive = 0, vtxfmt = 0;
    uint16_t count = 0;
    uint32_t vertex_size = 0;
    std::vector<uint8_t> vertices;
};

// What draws GX's output. The runtime has one; until the Vulkan renderer
// exists it is a headless one that counts what it is given.
class Sink {
public:
    virtual ~Sink() = default;
    virtual void draw(const State& s, const Draw& d) = 0;
    // GXCopyDisp: the embedded frame buffer to the external one at `xfb`.
    virtual void copy_display(const State& s, uint32_t xfb, bool clear) = 0;
    // GXCopyTex: the embedded frame buffer to a texture at `dest`.
    virtual void copy_texture(const State& s, uint32_t dest, bool clear) = 0;
};
Sink& sink();

// The size of one vertex in format `vtxfmt` with the current descriptors.
uint32_t vertex_size(const State& s, unsigned vtxfmt);
// The size of one attribute component and of a colour (gx_fifo.cpp); an
// undefined type stops.
uint32_t comp_size(uint8_t type, uint32_t attr);
uint32_t color_size(uint8_t type, uint32_t attr);

// GXTexFmt (gx_texture.cpp decodes them).
enum : uint32_t {
    kTexI4 = 0, kTexI8 = 1, kTexIA4 = 2, kTexIA8 = 3, kTexRgb565 = 4, kTexRgb5A3 = 5, kTexRgba8 = 6,
    kTexC4 = 8, kTexC8 = 9, kTexC14X2 = 10, kTexCmpr = 14,
};
// The bytes a texture's first level takes in guest memory; 0 for a format
// GX doesn't define.
uint32_t texture_bytes(uint32_t format, uint32_t w, uint32_t h);
// The first level as RGBA8 (r, g, b, a bytes, w * h * 4); colour-indexed
// formats through `tlut`. An unknown format stops.
void decode_texture(uint32_t format, uint32_t addr, uint32_t w, uint32_t h, const Tlut* tlut, uint8_t* out);

// The renderer replaces the headless sink (gx_vulkan.cpp).
void set_sink(Sink* s);

}  // namespace qilin::rt::gx
