// GX's state setters, objects and copies, native (gx.hpp). What the SDK
// sets up at GXInit comes from the SDK itself: native GXInit calls the lifted
// __GXInitGX, which calls these setters with the SDK's own defaults. Pure
// functions on caller-owned objects (texture, TLUT and light objects, the
// Y-scale arithmetic, overscan) are lifted (libraries.cpp); the natives here
// decode those objects in the SDK's own layout, read from its getters.
// Runtime code: no std::format.
#include "gx.hpp"

#include <bit>
#include <cmath>
#include <cstdio>

#include "machine.hpp"
#include "native.hpp"

namespace qilin::rt::gx {

namespace {

State g_state;

// ---- guest data ----
float guest_float(uint32_t a) { return std::bit_cast<float>(peek32(a)); }
Color guest_color(uint32_t a) { return Color{peek8(a), peek8(a + 1), peek8(a + 2), peek8(a + 3)}; }
float arg_f(const Context& c, unsigned n) { return static_cast<float>(c.f[n]); }
// A virtual address from a physical one GX objects hold (MEM1 or MEM2).
uint32_t virt(uint32_t phys) { return phys | 0x80000000u; }

[[noreturn]] void bad_arg(const Context& c, const char* what, uint32_t v) {
    std::fprintf(stderr, "qilin_run: GX: %s %u\n", what, v);
    throw Stop{Stop::Unsupported, c.lr - 4, v};
}

// ---- the headless sink: counts what it would draw ----
class Headless final : public Sink {
public:
    void draw(const State&, const Draw& d) override {
        ++stats().draws;
        stats().vertices += d.count;
    }
    void copy_display(const State&, uint32_t, bool) override { ++stats().display_copies; }
    void copy_texture(const State&, uint32_t, bool) override { ++stats().texture_copies; }
} g_headless;

// D-form instruction fields, for tables the runtime finds through code.
bool dform(uint32_t w, unsigned op, unsigned rd, unsigned ra, int32_t& d) {
    if (w >> 26 != op || ((w >> 21) & 31) != rd || ((w >> 16) & 31) != ra) return false;
    d = static_cast<int16_t>(w & 0xFFFF);
    return true;
}

// GXSetTevOp's preset tables: colour and alpha environment words for stage
// 0 and for later stages, five modes each, found through its `lis r5`/`addi
// r5,r5` pair.
uint32_t tev_op_tables() {
    static uint32_t base = 0;
    if (base) return base;
    const uint32_t fn = require_function("GXSetTevOp");
    int32_t hi = 0, lo = 0;
    if (!dform(peek32(fn + 4), 15, 5, 0, hi) || !dform(peek32(fn + 8), 14, 5, 5, lo)) {
        std::fprintf(stderr, "qilin_run: GXSetTevOp (%08X) isn't the supported build's\n", fn);
        throw Stop{Stop::Unsupported, fn, 0};
    }
    base = static_cast<uint32_t>(hi) * 0x10000u + static_cast<uint32_t>(lo);
    return base;
}

// TEV environment words, as the hardware takes them (and GXSetTevOp's
// tables hold them): selectors in the low bits, then bias, op, clamp, scale
// and the output register. A bias of 3 marks a compare op.
void color_env(TevStage& t, uint32_t w) {
    t.color_in[3] = w & 15, t.color_in[2] = (w >> 4) & 15, t.color_in[1] = (w >> 8) & 15, t.color_in[0] = (w >> 12) & 15;
    const uint8_t bias = (w >> 16) & 3, op = (w >> 18) & 1, scale = (w >> 20) & 3;
    t.color_op = bias == 3 ? static_cast<uint8_t>(8 + (scale << 1) + op) : op;
    t.color_bias = bias == 3 ? 0 : bias, t.color_scale = bias == 3 ? 0 : scale;
    t.color_clamp = (w >> 19) & 1, t.color_out = (w >> 22) & 3;
}
void alpha_env(TevStage& t, uint32_t w) {  // the swap selections (bits 0-3) aren't in GXSetTevOp's tables
    t.alpha_in[3] = (w >> 4) & 7, t.alpha_in[2] = (w >> 7) & 7, t.alpha_in[1] = (w >> 10) & 7, t.alpha_in[0] = (w >> 13) & 7;
    const uint8_t bias = (w >> 16) & 3, op = (w >> 18) & 1, scale = (w >> 20) & 3;
    t.alpha_op = bias == 3 ? static_cast<uint8_t>(8 + (scale << 1) + op) : op;
    t.alpha_bias = bias == 3 ? 0 : bias, t.alpha_scale = bias == 3 ? 0 : scale;
    t.alpha_clamp = (w >> 19) & 1, t.alpha_out = (w >> 22) & 3;
}

TevStage& stage(const Context& c, uint32_t n) {
    if (n >= 16) bad_arg(c, "TEV stage", n);
    return g_state.tev[n];
}
uint32_t check(const Context& c, uint32_t v, uint32_t limit, const char* what) {
    if (v >= limit) bad_arg(c, what, v);
    return v;
}

uint32_t g_fifo_obj = 0;  // GXInit's FIFO object, in the runtime's guest scratch
uint32_t g_tex_region_cb = 0, g_tlut_region_cb = 0;

}  // namespace

State& state() { return g_state; }
namespace {
Sink* g_sink = &g_headless;
}
Sink& sink() { return *g_sink; }
void set_sink(Sink* s) { g_sink = s ? s : &g_headless; }

// ---- initialisation ----
// GXInit(base, size): the SDK's defaults, from its own __GXInitGX (lifted),
// which calls the setters below. Returns the CPU FIFO object.
QILIN_NATIVE(GXInit) {
    g_state = State{};
    if (!g_fifo_obj) g_fifo_obj = guest_alloc(128, 32);
    call_guest(c, require_function("__GXInitGX"));
    c.r[3] = g_fifo_obj;
}
QILIN_NATIVE(GXSetMisc) {}  // FIFO tuning (the write-gather pipe's flush behaviour): nothing to tune
QILIN_NATIVE(__GXSetGenMode) {}  // flushes register state the native state doesn't batch
QILIN_NATIVE(GXSetGPMetric) {}   // performance counters: not modelled, and the game reads none
QILIN_NATIVE(GXClearGPMetric) {}
QILIN_NATIVE(GXClearBoundingBox) {}
QILIN_NATIVE(GXSetTexRegionCallback) {
    const uint32_t old = g_tex_region_cb;
    g_tex_region_cb = c.r[3];
    c.r[3] = old;
}
QILIN_NATIVE(GXSetTlutRegionCallback) {
    const uint32_t old = g_tlut_region_cb;
    g_tlut_region_cb = c.r[3];
    c.r[3] = old;
}

// ---- caches and synchronisation: no caches; the sink draws as it's called ----
QILIN_NATIVE(GXInvalidateVtxCache) {}
QILIN_NATIVE(GXInvalidateTexAll) {}
QILIN_NATIVE(GXDrawDone) {}
QILIN_NATIVE(GXPixModeSync) {}

// ---- vertex formats and arrays ----
QILIN_NATIVE(GXClearVtxDesc) {
    g_state.desc.fill(kNone);
    g_state.desc[kPos] = kDirect;  // as the SDK's: position stays direct
    g_state.nbt = false;
}
QILIN_NATIVE(GXSetVtxDesc) {
    const uint32_t attr = c.r[3], type = check(c, c.r[4], 4, "vertex attribute type");
    if (attr == kNbt) {
        g_state.desc[kNrm] = static_cast<uint8_t>(type);
        g_state.nbt = type != kNone;
    } else {
        g_state.desc[check(c, attr, kNbt, "vertex attribute")] = static_cast<uint8_t>(type);
        if (attr == kNrm) g_state.nbt = false;
    }
}
void set_attr_fmt(const Context& c, uint32_t vf, uint32_t attr, uint32_t cnt, uint32_t type, uint32_t frac) {
    check(c, vf, 8, "vertex format");
    if (attr == kNbt) attr = kNrm, cnt = cnt ? cnt : 1;
    g_state.fmt[vf][check(c, attr, kNbt, "vertex attribute")] =
        AttrFmt{static_cast<uint8_t>(cnt), static_cast<uint8_t>(type), static_cast<uint8_t>(frac)};
}
QILIN_NATIVE(GXSetVtxAttrFmt) { set_attr_fmt(c, c.r[3], c.r[4], c.r[5], c.r[6], c.r[7] & 0xFF); }
// A list of {attr, cnt, type, frac} (16 bytes each), ended by GX_VA_NULL.
QILIN_NATIVE(GXSetVtxAttrFmtv) {
    for (uint32_t p = c.r[4];; p += 16) {
        const uint32_t attr = peek32(p);
        if (attr == kAttrNull) break;
        set_attr_fmt(c, c.r[3], attr, peek32(p + 4), peek32(p + 8), peek8(p + 12));
    }
}
QILIN_NATIVE(GXSetArray) {
    const uint32_t attr = c.r[3] == kNbt ? kNrm : check(c, c.r[3], kAttrCount, "array attribute");
    g_state.arrays[attr] = State::Array{c.r[4], static_cast<uint8_t>(c.r[5])};
}

// ---- transforms ----
// Matrices are copied into the transform unit's memory as the GPU keeps
// them: rows of four (position, texture), three (normal).
void load_mtx(uint32_t src, float* dst, unsigned rows, unsigned cols) {
    for (unsigned r = 0; r < rows; ++r)
        for (unsigned k = 0; k < cols; ++k) dst[r * cols + k] = guest_float(src + 4 * (r * 4 + k));
}
QILIN_NATIVE(GXLoadPosMtxImm) {  // (mtx[3][4], id): id is a row index (GX_PNMTX0 = 0, 3, 6 ...)
    const uint32_t id = check(c, c.r[4], 62, "position matrix");
    load_mtx(c.r[3], &g_state.mtx[id * 4], 3, 4);
}
QILIN_NATIVE(GXLoadNrmMtxImm) {  // (mtx[3][4], id): the upper 3x3
    const uint32_t id = check(c, c.r[4], 30, "normal matrix");
    load_mtx(c.r[3], &g_state.nrm_mtx[id * 3], 3, 3);
}
QILIN_NATIVE(GXLoadTexMtxImm) {  // (mtx, id, type): 3x4, or 2x4 for GX_MTX2x4; ids from 64 are post-transform
    const uint32_t id = c.r[4], rows = c.r[5] == 1 ? 2 : 3;
    if (id >= 64) {
        check(c, id, 126, "post-transform matrix");
        load_mtx(c.r[3], &g_state.post_mtx[(id - 64) * 4], 3, 4);
    } else {
        check(c, id, 64 - rows + 1, "texture matrix");
        load_mtx(c.r[3], &g_state.mtx[id * 4], rows, 4);
    }
}
QILIN_NATIVE(GXSetCurrentMtx) { g_state.current_mtx = c.r[3]; }
// (mtx[4][4], type): perspective keeps m00, m02, m11, m12, m22, m23;
// orthographic m00, m03, m11, m13, m22, m23.
QILIN_NATIVE(GXSetProjection) {
    const uint32_t m = c.r[3], type = check(c, c.r[4], 2, "projection type");
    auto at = [m](unsigned r, unsigned k) { return guest_float(m + 4 * (r * 4 + k)); };
    g_state.projection_type = type;
    g_state.projection = type == 0 ? std::array<float, 6>{at(0, 0), at(0, 2), at(1, 1), at(1, 2), at(2, 2), at(2, 3)}
                                   : std::array<float, 6>{at(0, 0), at(0, 3), at(1, 1), at(1, 3), at(2, 2), at(2, 3)};
}
// (v[7]): the type (as a float), then the six parameters.
QILIN_NATIVE(GXSetProjectionv) {
    g_state.projection_type = static_cast<uint32_t>(guest_float(c.r[3]));
    for (unsigned i = 0; i < 6; ++i) g_state.projection[i] = guest_float(c.r[3] + 4 * (i + 1));
}
QILIN_NATIVE(GXSetViewport) {
    for (unsigned i = 0; i < 6; ++i) g_state.viewport[i] = arg_f(c, i + 1);
    g_state.viewport_jitter = false;
}
QILIN_NATIVE(GXSetViewportJitter) {
    for (unsigned i = 0; i < 6; ++i) g_state.viewport[i] = arg_f(c, i + 1);
    g_state.viewport_jitter = c.r[3] == 0;  // field 0: offset by half a line
}
QILIN_NATIVE(GXGetViewportv) {
    for (unsigned i = 0; i < 6; ++i) poke32(c.r[3] + 4 * i, std::bit_cast<uint32_t>(g_state.viewport[i]));
}
QILIN_NATIVE(GXSetZScaleOffset) {}  // depth scale and offset for the 16-bit depth formats: none here yet
QILIN_NATIVE(GXSetScissor) {
    for (unsigned i = 0; i < 4; ++i) g_state.scissor[i] = c.r[3 + i];
}
QILIN_NATIVE(GXGetScissor) {
    for (unsigned i = 0; i < 4; ++i) poke32(c.r[3 + i], g_state.scissor[i]);
}
QILIN_NATIVE(GXSetScissorBoxOffset) { g_state.scissor_offset[0] = c.r[3], g_state.scissor_offset[1] = c.r[4]; }
QILIN_NATIVE(GXSetCullMode) { g_state.cull = static_cast<uint8_t>(check(c, c.r[3], 4, "cull mode")); }
QILIN_NATIVE(GXSetClipMode) { g_state.clip = static_cast<uint8_t>(c.r[3]); }
QILIN_NATIVE(GXSetCoPlanar) { g_state.coplanar = c.r[3] & 0xFF; }

// ---- lighting ----
QILIN_NATIVE(GXSetNumChans) { g_state.num_chans = static_cast<uint8_t>(check(c, c.r[3] & 0xFF, 3, "colour channels")); }
// (chan, enable, amb_src, mat_src, light_mask, diff_fn, attn_fn); GX_COLOR0A0
// and GX_COLOR1A1 set the colour and the alpha channel.
QILIN_NATIVE(GXSetChanCtrl) {
    const ChanCtrl ctl{(c.r[4] & 0xFF) != 0, static_cast<uint8_t>(c.r[5]), static_cast<uint8_t>(c.r[6]),
                       static_cast<uint8_t>(c.r[8]), static_cast<uint8_t>(c.r[9]), c.r[7]};
    const uint32_t chan = check(c, c.r[3], 6, "colour channel");
    if (chan >= 4) g_state.chan[chan - 4] = ctl, g_state.chan[chan - 2] = ctl;
    else g_state.chan[chan] = ctl;
}
// (chan, color): GX_COLOR0/1 set RGB, GX_ALPHA0/1 alpha, GX_COLOR0A0/1A1 all four.
void chan_color(const Context& c, std::array<Color, 2>& dst) {
    const uint32_t chan = check(c, c.r[3], 6, "colour channel");
    const Color v = guest_color(c.r[4]);
    Color& d = dst[chan & 1];
    if (chan == 0 || chan == 1) d.r = v.r, d.g = v.g, d.b = v.b;
    else if (chan == 2 || chan == 3) d.a = v.a;
    else d = v;
}
QILIN_NATIVE(GXSetChanAmbColor) { chan_color(c, g_state.amb); }
QILIN_NATIVE(GXSetChanMatColor) { chan_color(c, g_state.mat); }
// (obj, light): the light is a bit (GX_LIGHT0 = 1 ... GX_LIGHT7 = 0x80). The
// object as the lifted GXInitLight* functions fill it: colour +12,
// attenuation +16..+36, position +40, direction +52.
QILIN_NATIVE(GXLoadLightObjImm) {
    const uint32_t bit = c.r[4];
    if (bit == 0 || (bit & (bit - 1)) || bit > 0x80) bad_arg(c, "light", bit);
    Light& l = g_state.lights[static_cast<unsigned>(std::countr_zero(bit))];
    const uint32_t o = c.r[3];
    l.loaded = true;
    l.color = guest_color(o + 12);
    for (unsigned i = 0; i < 3; ++i) {
        l.a[i] = guest_float(o + 16 + 4 * i), l.k[i] = guest_float(o + 28 + 4 * i);
        l.pos[i] = guest_float(o + 40 + 4 * i), l.dir[i] = guest_float(o + 52 + 4 * i);
    }
}

// ---- texture coordinates ----
QILIN_NATIVE(GXSetNumTexGens) { g_state.num_texgens = static_cast<uint8_t>(check(c, c.r[3] & 0xFF, 9, "texgens")); }
// (dst, type, src, mtx, normalize, pt_mtx)
QILIN_NATIVE(GXSetTexCoordGen2) {
    TexGen& t = g_state.texgen[check(c, c.r[3], 8, "texture coordinate")];
    t.type = static_cast<uint8_t>(c.r[4]), t.src = static_cast<uint8_t>(c.r[5]);
    t.mtx = c.r[6], t.normalize = static_cast<uint8_t>(c.r[7]), t.pt_mtx = c.r[8];
}
QILIN_NATIVE(GXEnableTexOffsets) {
    const uint32_t coord = check(c, c.r[3], 8, "texture coordinate");
    g_state.tex_offset_lines[coord] = c.r[4] & 0xFF, g_state.tex_offset_points[coord] = c.r[5] & 0xFF;
}
QILIN_NATIVE(GXSetTexCoordScaleManually) {
    const uint32_t coord = check(c, c.r[3], 8, "texture coordinate");
    g_state.tex_scale_manual[coord] = c.r[4] & 0xFF;
    g_state.tex_scale_s[coord] = static_cast<uint16_t>(c.r[5]), g_state.tex_scale_t[coord] = static_cast<uint16_t>(c.r[6]);
}
QILIN_NATIVE(GXSetTexCoordCylWrap) {
    const uint32_t coord = check(c, c.r[3], 8, "texture coordinate");
    g_state.cyl_wrap_s[coord] = c.r[4] & 0xFF, g_state.cyl_wrap_t[coord] = c.r[5] & 0xFF;
}
QILIN_NATIVE(GXSetLineWidth) {
    g_state.line_width = static_cast<float>(c.r[3] & 0xFF) / 6.0f;  // in sixths of a pixel
    g_state.line_tex_offset = static_cast<uint8_t>(c.r[4]);
}
QILIN_NATIVE(GXSetPointSize) {
    g_state.point_size = static_cast<float>(c.r[3] & 0xFF) / 6.0f;
    g_state.point_tex_offset = static_cast<uint8_t>(c.r[4]);
}

// ---- textures ----
// (obj, map): the object in the SDK's layout (its getters): wrap modes in
// the first word, size in the third, the image's physical address >> 5 in
// the fourth, format +0x14, TLUT +0x18, mipmap flag in the last byte.
QILIN_NATIVE(GXLoadTexObj) {
    const uint32_t o = c.r[3];
    TexMap& t = g_state.tex[check(c, c.r[4], 8, "texture map")];
    const uint32_t w0 = peek32(o), w2 = peek32(o + 8);
    t.loaded = true;
    t.mode0 = w0, t.mode1 = peek32(o + 4);
    t.wrap_s = w0 & 3, t.wrap_t = (w0 >> 2) & 3;
    t.width = static_cast<uint16_t>((w2 & 0x3FF) + 1), t.height = static_cast<uint16_t>(((w2 >> 10) & 0x3FF) + 1);
    t.image = virt((peek32(o + 12) << 5) & 0x1FFFFFE0u);
    t.format = peek32(o + 20), t.tlut = peek32(o + 24), t.mipmap = peek8(o + 31) & 1;
}
// (obj, name): the object as the lifted GXInitTlutObj fills it: the format
// at bits 10-11 of the first word, the table's physical address >> 5 in the
// low 24 bits of the second, the entry count at +8.
QILIN_NATIVE(GXLoadTlut) {
    const uint32_t o = c.r[3];
    Tlut& t = g_state.tlut[check(c, c.r[4], 20, "TLUT")];
    t.loaded = true;
    t.format = (peek32(o) >> 10) & 3;
    t.data = virt((peek32(o + 4) & 0xFFFFFF) << 5);
    t.entries = peek16(o + 8);
}

// ---- TEV ----
QILIN_NATIVE(GXSetNumTevStages) { g_state.num_tev_stages = static_cast<uint8_t>(check(c, c.r[3] & 0xFF, 17, "TEV stages")); }
QILIN_NATIVE(GXSetTevOrder) {
    TevStage& t = stage(c, c.r[3]);
    t.coord = static_cast<uint8_t>(c.r[4]), t.map = static_cast<uint8_t>(c.r[5]), t.channel = static_cast<uint8_t>(c.r[6]);
}
QILIN_NATIVE(GXSetTevColorIn) {
    TevStage& t = stage(c, c.r[3]);
    for (unsigned i = 0; i < 4; ++i) t.color_in[i] = static_cast<uint8_t>(check(c, c.r[4 + i], 16, "TEV colour input"));
}
QILIN_NATIVE(GXSetTevAlphaIn) {
    TevStage& t = stage(c, c.r[3]);
    for (unsigned i = 0; i < 4; ++i) t.alpha_in[i] = static_cast<uint8_t>(check(c, c.r[4 + i], 8, "TEV alpha input"));
}
// (stage, op, bias, scale, clamp, out)
QILIN_NATIVE(GXSetTevColorOp) {
    TevStage& t = stage(c, c.r[3]);
    t.color_op = static_cast<uint8_t>(c.r[4]), t.color_bias = static_cast<uint8_t>(c.r[5]);
    t.color_scale = static_cast<uint8_t>(c.r[6]), t.color_clamp = c.r[7] & 0xFF, t.color_out = static_cast<uint8_t>(c.r[8]);
}
QILIN_NATIVE(GXSetTevAlphaOp) {
    TevStage& t = stage(c, c.r[3]);
    t.alpha_op = static_cast<uint8_t>(c.r[4]), t.alpha_bias = static_cast<uint8_t>(c.r[5]);
    t.alpha_scale = static_cast<uint8_t>(c.r[6]), t.alpha_clamp = c.r[7] & 0xFF, t.alpha_out = static_cast<uint8_t>(c.r[8]);
}
// (stage, mode): the SDK's presets, from its own tables.
QILIN_NATIVE(GXSetTevOp) {
    const uint32_t s = c.r[3], mode = check(c, c.r[4], 5, "TEV mode"), base = tev_op_tables();
    TevStage& t = stage(c, s);
    color_env(t, peek32(base + (s == 0 ? 0 : 20) + 4 * mode));
    alpha_env(t, peek32(base + (s == 0 ? 40 : 60) + 4 * mode));
}
QILIN_NATIVE(GXSetTevColor) { g_state.tev_color[check(c, c.r[3], 4, "TEV register")] = [&] {
    const Color v = guest_color(c.r[4]);
    return ColorS10{v.r, v.g, v.b, v.a};
}(); }
QILIN_NATIVE(GXSetTevColorS10) {
    const uint32_t p = c.r[4];
    g_state.tev_color[check(c, c.r[3], 4, "TEV register")] =
        ColorS10{static_cast<int16_t>(peek16(p)), static_cast<int16_t>(peek16(p + 2)), static_cast<int16_t>(peek16(p + 4)),
                 static_cast<int16_t>(peek16(p + 6))};
}
QILIN_NATIVE(GXSetTevKColor) { g_state.kcolor[check(c, c.r[3], 4, "TEV constant colour")] = guest_color(c.r[4]); }
QILIN_NATIVE(GXSetTevKColorSel) { stage(c, c.r[3]).kcolor_sel = static_cast<uint8_t>(c.r[4]); }
QILIN_NATIVE(GXSetTevKAlphaSel) { stage(c, c.r[3]).kalpha_sel = static_cast<uint8_t>(c.r[4]); }
QILIN_NATIVE(GXSetTevSwapMode) {
    TevStage& t = stage(c, c.r[3]);
    t.ras_swap = static_cast<uint8_t>(check(c, c.r[4], 4, "swap table")), t.tex_swap = static_cast<uint8_t>(check(c, c.r[5], 4, "swap table"));
}
QILIN_NATIVE(GXSetTevSwapModeTable) {
    auto& t = g_state.swap_table[check(c, c.r[3], 4, "swap table")];
    for (unsigned i = 0; i < 4; ++i) t[i] = static_cast<uint8_t>(check(c, c.r[4 + i], 4, "swap channel"));
}
// (stage, ind_stage, format, bias, mtx, wrap_s, wrap_t, add_prev, utc_lod, alpha)
QILIN_NATIVE(GXSetTevIndirect) {
    TevStage& t = stage(c, c.r[3]);
    t.ind_stage = static_cast<uint8_t>(c.r[4]), t.ind_format = static_cast<uint8_t>(c.r[5]);
    t.ind_bias = static_cast<uint8_t>(c.r[6]), t.ind_mtx = static_cast<uint8_t>(c.r[7]);
    t.ind_wrap_s = static_cast<uint8_t>(c.r[8]), t.ind_wrap_t = static_cast<uint8_t>(c.r[9]);
    t.ind_add_prev = c.r[10] & 0xFF;
    // utc_lod and alpha are the 9th and 10th arguments: on the stack (EABI)
    t.ind_utc_lod = peek8(c.r[1] + 8 + 3) != 0;
    t.ind_alpha = peek8(c.r[1] + 12 + 3);
}
QILIN_NATIVE(GXSetNumIndStages) { g_state.num_ind_stages = static_cast<uint8_t>(check(c, c.r[3] & 0xFF, 5, "indirect stages")); }
QILIN_NATIVE(GXSetIndTexOrder) {
    auto& s = g_state.ind[check(c, c.r[3], 4, "indirect stage")];
    s.coord = static_cast<uint8_t>(c.r[4]), s.map = static_cast<uint8_t>(c.r[5]);
}
QILIN_NATIVE(GXSetIndTexCoordScale) {
    auto& s = g_state.ind[check(c, c.r[3], 4, "indirect stage")];
    s.scale_s = static_cast<uint8_t>(c.r[4]), s.scale_t = static_cast<uint8_t>(c.r[5]);
}
// (id, offset[2][3], scale_exp): GX_ITM_0..2 are 1..3.
QILIN_NATIVE(GXSetIndTexMtx) {
    const uint32_t id = c.r[3];
    if (id < 1 || id > 3) bad_arg(c, "indirect matrix", id);
    auto& m = g_state.ind_mtx[id - 1];
    for (unsigned i = 0; i < 6; ++i) m.m[i] = guest_float(c.r[4] + 4 * i);
    m.scale_exp = static_cast<int8_t>(c.r[5] & 0xFF);
}

// ---- pixels ----
QILIN_NATIVE(GXSetAlphaCompare) {
    g_state.alpha_comp0 = static_cast<uint8_t>(c.r[3]), g_state.alpha_ref0 = static_cast<uint8_t>(c.r[4]);
    g_state.alpha_op = static_cast<uint8_t>(c.r[5]);
    g_state.alpha_comp1 = static_cast<uint8_t>(c.r[6]), g_state.alpha_ref1 = static_cast<uint8_t>(c.r[7]);
}
QILIN_NATIVE(GXSetZMode) {
    g_state.z_enable = c.r[3] & 0xFF, g_state.z_func = static_cast<uint8_t>(c.r[4]), g_state.z_update = c.r[5] & 0xFF;
}
QILIN_NATIVE(GXSetZCompLoc) { g_state.z_comp_before_tex = c.r[3] & 0xFF; }
QILIN_NATIVE(GXSetBlendMode) {
    g_state.blend_type = static_cast<uint8_t>(c.r[3]), g_state.blend_src = static_cast<uint8_t>(c.r[4]);
    g_state.blend_dst = static_cast<uint8_t>(c.r[5]), g_state.logic_op = static_cast<uint8_t>(c.r[6]);
}
QILIN_NATIVE(GXSetColorUpdate) { g_state.color_update = c.r[3] & 0xFF; }
QILIN_NATIVE(GXSetAlphaUpdate) { g_state.alpha_update = c.r[3] & 0xFF; }
QILIN_NATIVE(GXSetDither) { g_state.dither = c.r[3] & 0xFF; }
QILIN_NATIVE(GXSetDstAlpha) { g_state.dst_alpha_enable = c.r[3] & 0xFF, g_state.dst_alpha = static_cast<uint8_t>(c.r[4]); }
// (type, startz, endz, nearz, farz, color)
QILIN_NATIVE(GXSetFog) {
    g_state.fog_type = static_cast<uint8_t>(c.r[3]);
    g_state.fog_start = arg_f(c, 1), g_state.fog_end = arg_f(c, 2), g_state.fog_near = arg_f(c, 3), g_state.fog_far = arg_f(c, 4);
    g_state.fog_color = guest_color(c.r[4]);
}
QILIN_NATIVE(GXSetFogRangeAdj) {
    g_state.fog_range_adj = c.r[3] & 0xFF, g_state.fog_range_center = static_cast<uint16_t>(c.r[4]);
    if (g_state.fog_range_adj) bad_arg(c, "fog range adjustment (its table isn't kept yet)", 1);
}
QILIN_NATIVE(GXSetZTexture) {
    g_state.ztex_op = static_cast<uint8_t>(c.r[3]), g_state.ztex_fmt = static_cast<uint8_t>(c.r[4]), g_state.ztex_bias = c.r[5];
}
QILIN_NATIVE(GXSetPixelFmt) { g_state.pixel_fmt = static_cast<uint8_t>(c.r[3]), g_state.z_fmt = static_cast<uint8_t>(c.r[4]); }
// GXGetPixelFmt(GXPixelFmt*, GXZFmt*): what GXSetPixelFmt set (CDynTex
// saves it around its render to texture).
QILIN_NATIVE(GXGetPixelFmt) {
    poke32(c.r[3], g_state.pixel_fmt);
    poke32(c.r[4], g_state.z_fmt);
}
QILIN_NATIVE(GXSetFieldMask) { g_state.field_mask_odd = c.r[3] & 0xFF, g_state.field_mask_even = c.r[4] & 0xFF; }
QILIN_NATIVE(GXSetFieldMode) { g_state.field_mode = c.r[3] & 0xFF, g_state.half_aspect = c.r[4] & 0xFF; }
// CPU access to the embedded frame buffer (GXPeek/GXPoke): its own state,
// for when the game reads or writes pixels directly. Nothing here does yet.
QILIN_NATIVE(GXPokeColorUpdate) {}
QILIN_NATIVE(GXPokeAlphaUpdate) {}
QILIN_NATIVE(GXPokeDither) {}
QILIN_NATIVE(GXPokeBlendMode) {}
QILIN_NATIVE(GXPokeAlphaMode) {}
QILIN_NATIVE(GXPokeAlphaRead) {}
QILIN_NATIVE(GXPokeDstAlpha) {}
QILIN_NATIVE(GXPokeZMode) {}

// ---- copies ----
QILIN_NATIVE(GXSetCopyClear) {
    g_state.clear_color = guest_color(c.r[3]);
    g_state.clear_z = c.r[4] & 0xFFFFFF;
}
QILIN_NATIVE(GXSetDispCopySrc) {
    for (unsigned i = 0; i < 4; ++i) g_state.disp_src[i] = static_cast<uint16_t>(c.r[3 + i]);
}
QILIN_NATIVE(GXSetDispCopyDst) {
    g_state.disp_dst_width = static_cast<uint16_t>(c.r[3]), g_state.disp_dst_height = static_cast<uint16_t>(c.r[4]);
}
QILIN_NATIVE(GXSetTexCopySrc) {
    for (unsigned i = 0; i < 4; ++i) g_state.tex_src[i] = static_cast<uint16_t>(c.r[3 + i]);
}
QILIN_NATIVE(GXSetTexCopyDst) {
    g_state.tex_dst_width = static_cast<uint16_t>(c.r[3]), g_state.tex_dst_height = static_cast<uint16_t>(c.r[4]);
    g_state.tex_dst_format = c.r[5], g_state.tex_dst_mipmap = c.r[6] & 0xFF;
}
// (scale): returns how many lines the external frame buffer gets, by the
// SDK's arithmetic (its GXSetDispCopyYScale): s = 256 / scale in 9 bits,
// lines = (height - 1) * 256 / s + 1, plus one when s is in 129..255 and
// its odd part divides the copy's height; at most 1024.
QILIN_NATIVE(GXSetDispCopyYScale) {
    const float q = 256.0f / static_cast<float>(c.f[1]);
    const uint32_t s = static_cast<uint32_t>(q) & 0x1FF, height = g_state.disp_src[3];
    if (s == 0 || height == 0) bad_arg(c, "display copy scale", s);
    g_state.disp_yscale = s;
    uint32_t lines = ((height - 1) << 8) / s + 1;
    if (s >= 129 && s <= 255) {
        uint32_t odd = s;
        while (!(odd & 1)) odd >>= 1;
        if (height % odd == 0) ++lines;
    }
    c.r[3] = lines > 1024 ? 1024 : lines;
}
QILIN_NATIVE(GXSetCopyClamp) { g_state.copy_clamp = static_cast<uint8_t>(c.r[3]); }
// (aa, sample_pattern[12][2], vfilter_enable, vfilter[7])
QILIN_NATIVE(GXSetCopyFilter) {
    g_state.aa = c.r[3] & 0xFF;
    if (g_state.aa && c.r[4])
        for (unsigned i = 0; i < 24; ++i) g_state.sample_pattern[i / 2][i % 2] = peek8(c.r[4] + i);
    g_state.vfilter_enable = c.r[5] & 0xFF;
    if (g_state.vfilter_enable && c.r[6])
        for (unsigned i = 0; i < 7; ++i) g_state.vfilter[i] = peek8(c.r[6] + i);
}
QILIN_NATIVE(GXSetDispCopyGamma) { g_state.gamma = static_cast<uint8_t>(c.r[3]); }
QILIN_NATIVE(GXSetDispCopyFrame2Field) { g_state.frame2field = c.r[3] & 0xFF; }
QILIN_NATIVE(GXCopyDisp) { sink().copy_display(g_state, c.r[3], c.r[4] & 0xFF); }
QILIN_NATIVE(GXCopyTex) { sink().copy_texture(g_state, c.r[3], c.r[4] & 0xFF); }

}  // namespace qilin::rt::gx
