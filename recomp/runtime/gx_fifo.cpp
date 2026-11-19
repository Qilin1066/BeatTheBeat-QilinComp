// GX's command stream: draws. GXBegin (native) opens one; lifted code then
// writes its vertices to the write-gather pipe (rt::fifo_st8/16/32, from the
// stores the lifter found there), and the draw is complete when its vertex
// count has arrived. Display lists (GXCallDisplayList) carry the same draw
// commands in guest memory. Any other command, or data outside a draw,
// stops: nothing in the game needs one yet, and guessing would draw wrong
// without a word. Runtime code: no std::format.
#include <cstdio>

#include "gx.hpp"
#include "native.hpp"

namespace qilin::rt::gx {

namespace {

constexpr uint32_t kWgp = 0xCC008000u;  // the write-gather pipe

// The lifted store's address, for reports (checked builds keep it).
uint32_t store_pc() {
#ifdef QILIN_CHECKED_MEMORY
    return g_pc;
#else
    return 0;
#endif
}

// The bytes one component of type `type` takes; a type GX doesn't define stops.
uint32_t comp_size(uint8_t type, uint32_t attr) {
    switch (type) {
    case kU8:
    case kS8: return 1;
    case kU16:
    case kS16: return 2;
    case kF32: return 4;
    default:
        std::fprintf(stderr, "qilin_run: GX: attribute %u has component type %u\n", attr, type);
        throw Stop{Stop::Unsupported, 0, type};
    }
}
uint32_t color_size(uint8_t type, uint32_t attr) {
    switch (type) {
    case kRgb565:
    case kRgba4: return 2;
    case kRgb8:
    case kRgba6: return 3;
    case kRgbx8:
    case kRgba8: return 4;
    default:
        std::fprintf(stderr, "qilin_run: GX: colour attribute %u has type %u\n", attr, type);
        throw Stop{Stop::Unsupported, 0, type};
    }
}

struct Stream {
    bool open = false;
    Draw draw;
    uint32_t expected = 0;  // bytes
} g_stream;

void finish(Draw& d) {
    sink().draw(state(), d);
    d.vertices.clear();
}

void begin(uint8_t primitive, uint8_t vtxfmt, uint16_t count, uint32_t pc) {
    if (g_stream.open) {
        std::fprintf(stderr, "qilin_run: GX: a draw begins before the last one's %u vertices all arrived\n",
                     g_stream.draw.count);
        throw Stop{Stop::Unsupported, pc, g_stream.draw.count};
    }
    Draw& d = g_stream.draw;
    d.primitive = primitive, d.vtxfmt = vtxfmt, d.count = count;
    d.vertex_size = vertex_size(state(), vtxfmt);
    d.vertices.clear();
    g_stream.expected = d.vertex_size * count;
    if (g_stream.expected == 0) {
        finish(d);
        return;
    }
    d.vertices.reserve(g_stream.expected);
    g_stream.open = true;
}

void append(uint32_t ea, uint32_t v, unsigned bytes) {
    if (ea != kWgp) throw Stop{Stop::Memory, store_pc(), ea};  // a lifted store the analysis put on the pipe, elsewhere
    if (!g_stream.open) {
        std::fprintf(stderr, "qilin_run: GX: FIFO data outside a draw (%u bytes, %08X)\n", bytes, v);
        throw Stop{Stop::Unsupported, store_pc(), v};
    }
    Draw& d = g_stream.draw;
    for (unsigned i = bytes; i-- > 0;) d.vertices.push_back(static_cast<uint8_t>(v >> (8 * i)));
    if (d.vertices.size() > g_stream.expected) {
        std::fprintf(stderr, "qilin_run: GX: more vertex data than the draw's %u vertices of %u bytes\n", d.count,
                     d.vertex_size);
        throw Stop{Stop::Unsupported, store_pc(), static_cast<uint32_t>(d.vertices.size())};
    }
    if (d.vertices.size() == g_stream.expected) {
        g_stream.open = false;
        finish(d);
    }
}

bool is_draw(uint8_t cmd) { return cmd >= kQuads && cmd < kPoints + 8; }

}  // namespace

uint32_t vertex_size(const State& s, unsigned vf) {
    const auto& f = s.fmt[vf];
    uint32_t n = 0;
    auto index = [](uint8_t desc) { return desc == kIndex8 ? 1u : 2u; };
    // Matrix indices: a byte each, direct only.
    for (uint32_t a = kPnMtxIdx; a < kPos; ++a)
        if (s.desc[a] != kNone) n += 1;
    for (uint32_t a = kPos; a < kPosMtxArray; ++a) {
        const uint8_t desc = s.desc[a];
        if (desc == kNone) continue;
        if (a == kNrm) {
            const bool three = s.nbt || f[a].cnt != 0;  // normal, binormal and tangent
            if (desc == kDirect) n += (three ? 9 : 3) * comp_size(f[a].type, a);
            else n += (three && f[a].cnt == 2 ? 3 : 1) * index(desc);  // NBT3: an index each
        } else if (desc != kDirect) {
            n += index(desc);
        } else if (a == kPos) {
            n += (f[a].cnt ? 3 : 2) * comp_size(f[a].type, a);
        } else if (a == kClr0 || a == kClr1) {
            n += color_size(f[a].type, a);
        } else {
            n += (f[a].cnt ? 2 : 1) * comp_size(f[a].type, a);
        }
    }
    return n;
}

// GXBegin(primitive, vtxfmt, count)
QILIN_NATIVE(GXBegin) {
    const uint32_t prim = c.r[3], vf = c.r[4];
    if (!is_draw(static_cast<uint8_t>(prim)) || (prim & 7) || vf >= 8) {
        std::fprintf(stderr, "qilin_run: GX: GXBegin(%u, %u)\n", prim, vf);
        throw Stop{Stop::Unsupported, c.lr - 4, prim};
    }
    begin(static_cast<uint8_t>(prim), static_cast<uint8_t>(vf), static_cast<uint16_t>(c.r[5]), c.lr - 4);
}

// GXCallDisplayList(list, bytes): its draw commands; NOPs and vertex-cache
// invalidations do nothing here.
QILIN_NATIVE(GXCallDisplayList) {
    const uint32_t list = c.r[3], bytes = c.r[4];
    for (uint32_t p = list; p < list + bytes;) {
        const uint8_t cmd = peek8(p++);
        if (cmd == 0x00 || cmd == 0x48) continue;  // NOP, invalidate the vertex cache
        if (!is_draw(cmd)) {
            std::fprintf(stderr, "qilin_run: GX: display list %08X has command %02X at +%u (only draws are decoded yet)\n",
                         list, cmd, p - 1 - list);
            throw Stop{Stop::Unsupported, c.lr - 4, cmd};
        }
        const uint16_t count = peek16(p);
        p += 2;
        begin(cmd & 0xF8, cmd & 7, count, c.lr - 4);
        if (!g_stream.open) continue;
        if (p + g_stream.expected > list + bytes) {
            std::fprintf(stderr, "qilin_run: GX: display list %08X ends inside a draw\n", list);
            throw Stop{Stop::Unsupported, c.lr - 4, list};
        }
        Draw& d = g_stream.draw;
        d.vertices.assign(g_mem + p, g_mem + p + g_stream.expected);
        p += g_stream.expected;
        g_stream.open = false;
        finish(d);
    }
}

}  // namespace qilin::rt::gx

namespace qilin::rt {
void fifo_st8(uint32_t ea, uint32_t v) { gx::append(ea, v & 0xFF, 1); }
void fifo_st16(uint32_t ea, uint32_t v) { gx::append(ea, v & 0xFFFF, 2); }
void fifo_st32(uint32_t ea, uint32_t v) { gx::append(ea, v, 4); }
}  // namespace qilin::rt
