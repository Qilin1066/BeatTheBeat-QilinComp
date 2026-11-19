// GX texture formats to RGBA8 (gx_texture in gx.hpp). GX stores texels in
// tiles of 32 bytes (64 for RGBA8, whose AR and GB halves are separate),
// tiles left to right, then top to bottom, rows of a texture padded to whole
// tiles. APPROX: CMPR's two in-between colours use exact thirds and halves.
// Runtime code: no std::format.
#include <cstdio>

#include "gx.hpp"
#include "native.hpp"

namespace qilin::rt::gx {

namespace {

struct Tile {
    uint32_t w, h, bytes;
};

bool tile_of(uint32_t format, Tile& t) {
    switch (format) {
    case kTexI4: case kTexC4: case kTexCmpr: t = {8, 8, 32}; return true;
    case kTexI8: case kTexIA4: case kTexC8: t = {8, 4, 32}; return true;
    case kTexIA8: case kTexRgb565: case kTexRgb5A3: case kTexC14X2: t = {4, 4, 32}; return true;
    case kTexRgba8: t = {4, 4, 64}; return true;
    default: return false;
    }
}

uint8_t x4(uint32_t v) { return static_cast<uint8_t>(v * 17); }
uint8_t x5(uint32_t v) { return static_cast<uint8_t>(v << 3 | v >> 2); }
uint8_t x6(uint32_t v) { return static_cast<uint8_t>(v << 2 | v >> 4); }
uint8_t x3(uint32_t v) { return static_cast<uint8_t>(v << 5 | v << 2 | v >> 1); }

void put(uint8_t* p, uint8_t r, uint8_t g, uint8_t b, uint8_t a) { p[0] = r, p[1] = g, p[2] = b, p[3] = a; }

void rgb565(uint8_t* p, uint16_t v) { put(p, x5(v >> 11), x6((v >> 5) & 63), x5(v & 31), 255); }
void rgb5a3(uint8_t* p, uint16_t v) {
    if (v & 0x8000) put(p, x5((v >> 10) & 31), x5((v >> 5) & 31), x5(v & 31), 255);
    else put(p, x4((v >> 8) & 15), x4((v >> 4) & 15), x4(v & 15), x3((v >> 12) & 7));
}
void ia8(uint8_t* p, uint16_t v) {  // alpha in the high byte, intensity in the low
    const uint8_t i = static_cast<uint8_t>(v), a = static_cast<uint8_t>(v >> 8);
    put(p, i, i, i, a);
}

void palette(uint8_t* p, uint32_t index, const Tlut& t) {
    if (index >= t.entries) {  // GX reads whatever TMEM holds there; nothing here does that yet
        std::fprintf(stderr, "qilin_run: GX: texel index %u past the TLUT's %u entries\n", index, t.entries);
        throw Stop{Stop::Unsupported, 0, index};
    }
    const uint16_t v = peek16(t.data + 2 * index);
    switch (t.format) {
    case 0: ia8(p, v); break;
    case 1: rgb565(p, v); break;
    case 2: rgb5a3(p, v); break;
    default: throw Stop{Stop::Unsupported, 0, t.format};
    }
}

// A CMPR sub-block: 4x4 texels, two RGB565 colours and 2-bit indices.
void cmpr_block(uint32_t src, uint8_t* out, uint32_t stride, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h) {
    const uint16_t c0 = peek16(src), c1 = peek16(src + 2);
    uint8_t pal[4][4];
    rgb565(pal[0], c0);
    rgb565(pal[1], c1);
    for (unsigned k = 0; k < 3; ++k) {
        if (c0 > c1) {
            pal[2][k] = static_cast<uint8_t>((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = static_cast<uint8_t>((pal[0][k] + 2 * pal[1][k]) / 3);
        } else {
            pal[2][k] = static_cast<uint8_t>((pal[0][k] + pal[1][k]) / 2);
            pal[3][k] = 0;
        }
    }
    pal[2][3] = 255, pal[3][3] = c0 > c1 ? 255 : 0;
    for (uint32_t y = 0; y < 4; ++y) {
        const uint8_t bits = peek8(src + 4 + y);
        for (uint32_t x = 0; x < 4; ++x) {
            if (x0 + x >= w || y0 + y >= h) continue;
            const uint8_t* c = pal[(bits >> (6 - 2 * x)) & 3];
            put(out + ((y0 + y) * stride + x0 + x) * 4, c[0], c[1], c[2], c[3]);
        }
    }
}

}  // namespace

uint32_t texture_bytes(uint32_t format, uint32_t w, uint32_t h) {
    Tile t;
    if (!tile_of(format, t)) return 0;
    return ((w + t.w - 1) / t.w) * ((h + t.h - 1) / t.h) * t.bytes;
}

void decode_texture(uint32_t format, uint32_t addr, uint32_t w, uint32_t h, const Tlut* tlut, uint8_t* out) {
    Tile t;
    if (!tile_of(format, t)) {
        std::fprintf(stderr, "qilin_run: GX: texture format %u\n", format);
        throw Stop{Stop::Unsupported, 0, format};
    }
    const bool indexed = format == kTexC4 || format == kTexC8 || format == kTexC14X2;
    if (indexed && (!tlut || !tlut->loaded)) {
        std::fprintf(stderr, "qilin_run: GX: a colour-indexed texture without a loaded TLUT\n");
        throw Stop{Stop::Unsupported, 0, addr};
    }
    const uint32_t tiles_x = (w + t.w - 1) / t.w, tiles_y = (h + t.h - 1) / t.h;
    uint32_t src = addr;
    for (uint32_t ty = 0; ty < tiles_y; ++ty)
        for (uint32_t tx = 0; tx < tiles_x; ++tx, src += t.bytes) {
            const uint32_t x0 = tx * t.w, y0 = ty * t.h;
            if (format == kTexCmpr) {
                for (uint32_t b = 0; b < 4; ++b) cmpr_block(src + 8 * b, out, w, x0 + 4 * (b & 1), y0 + 4 * (b >> 1), w, h);
                continue;
            }
            for (uint32_t y = 0; y < t.h; ++y)
                for (uint32_t x = 0; x < t.w; ++x) {
                    const uint32_t i = y * t.w + x;  // texel within the tile
                    if (x0 + x >= w || y0 + y >= h) continue;
                    uint8_t* p = out + ((y0 + y) * w + x0 + x) * 4;
                    switch (format) {
                    case kTexI4: {
                        const uint8_t v = x4((peek8(src + i / 2) >> ((i & 1) ? 0 : 4)) & 15);
                        put(p, v, v, v, v);
                        break;
                    }
                    case kTexI8: {
                        const uint8_t v = peek8(src + i);
                        put(p, v, v, v, v);
                        break;
                    }
                    case kTexIA4: {
                        const uint8_t b = peek8(src + i), v = x4(b & 15);
                        put(p, v, v, v, x4(b >> 4));
                        break;
                    }
                    case kTexIA8: ia8(p, peek16(src + 2 * i)); break;
                    case kTexRgb565: rgb565(p, peek16(src + 2 * i)); break;
                    case kTexRgb5A3: rgb5a3(p, peek16(src + 2 * i)); break;
                    case kTexRgba8:
                        put(p, peek8(src + 2 * i + 1), peek8(src + 32 + 2 * i), peek8(src + 32 + 2 * i + 1),
                            peek8(src + 2 * i));
                        break;
                    case kTexC4: palette(p, (peek8(src + i / 2) >> ((i & 1) ? 0 : 4)) & 15, *tlut); break;
                    case kTexC8: palette(p, peek8(src + i), *tlut); break;
                    default: palette(p, peek16(src + 2 * i) & 0x3FFF, *tlut); break;  // C14X2
                    }
                }
        }
}

}  // namespace qilin::rt::gx
