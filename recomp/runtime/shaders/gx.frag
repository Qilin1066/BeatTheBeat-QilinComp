// GX's pixel pipeline for one TEV configuration: the stages, the alpha
// compare and fog. The configuration (stage count, each stage's inputs, ops
// and order, the swap tables, the compare functions, the fog type) is
// specialisation constants, packed by gx_vulkan.cpp's specialise(): the
// driver folds every selector away and compiles straight-line code for it,
// a pipeline per configuration. What changes from draw to draw (registers,
// konst colours, reference values, fog parameters) is the uniform block.
// Integer arithmetic on GX's own ranges: 8-bit inputs, 11-bit signed
// registers. APPROX: the TEV's rounding, and the konst fractions.
#version 450

layout(location = 0) in vec4 v_col0;
layout(location = 1) in vec4 v_col1;
layout(location = 2) in vec3 v_tex[8];
layout(location = 0) out vec4 o_color;

// ---- the configuration ----
layout(constant_id = 0) const uint NUM_STAGES = 0u;
// Per stage: colour (inputs a | b << 4 | c << 8 | d << 12, then op | bias
// << 4 | scale << 6 | clamp << 8 | out << 9 from bit 16), alpha (the same),
// order (map | coord << 8 | channel << 16; map 255: no texture) and
// selections (konst colour | konst alpha << 8 | raster swap << 16 | texture
// swap << 18).
layout(constant_id = 1) const uint S0C = 0u;   layout(constant_id = 2) const uint S0A = 0u;
layout(constant_id = 3) const uint S0O = 0u;   layout(constant_id = 4) const uint S0K = 0u;
layout(constant_id = 5) const uint S1C = 0u;   layout(constant_id = 6) const uint S1A = 0u;
layout(constant_id = 7) const uint S1O = 0u;   layout(constant_id = 8) const uint S1K = 0u;
layout(constant_id = 9) const uint S2C = 0u;   layout(constant_id = 10) const uint S2A = 0u;
layout(constant_id = 11) const uint S2O = 0u;  layout(constant_id = 12) const uint S2K = 0u;
layout(constant_id = 13) const uint S3C = 0u;  layout(constant_id = 14) const uint S3A = 0u;
layout(constant_id = 15) const uint S3O = 0u;  layout(constant_id = 16) const uint S3K = 0u;
layout(constant_id = 17) const uint S4C = 0u;  layout(constant_id = 18) const uint S4A = 0u;
layout(constant_id = 19) const uint S4O = 0u;  layout(constant_id = 20) const uint S4K = 0u;
layout(constant_id = 21) const uint S5C = 0u;  layout(constant_id = 22) const uint S5A = 0u;
layout(constant_id = 23) const uint S5O = 0u;  layout(constant_id = 24) const uint S5K = 0u;
layout(constant_id = 25) const uint S6C = 0u;  layout(constant_id = 26) const uint S6A = 0u;
layout(constant_id = 27) const uint S6O = 0u;  layout(constant_id = 28) const uint S6K = 0u;
layout(constant_id = 29) const uint S7C = 0u;  layout(constant_id = 30) const uint S7A = 0u;
layout(constant_id = 31) const uint S7O = 0u;  layout(constant_id = 32) const uint S7K = 0u;
layout(constant_id = 33) const uint S8C = 0u;  layout(constant_id = 34) const uint S8A = 0u;
layout(constant_id = 35) const uint S8O = 0u;  layout(constant_id = 36) const uint S8K = 0u;
layout(constant_id = 37) const uint S9C = 0u;  layout(constant_id = 38) const uint S9A = 0u;
layout(constant_id = 39) const uint S9O = 0u;  layout(constant_id = 40) const uint S9K = 0u;
layout(constant_id = 41) const uint S10C = 0u; layout(constant_id = 42) const uint S10A = 0u;
layout(constant_id = 43) const uint S10O = 0u; layout(constant_id = 44) const uint S10K = 0u;
layout(constant_id = 45) const uint S11C = 0u; layout(constant_id = 46) const uint S11A = 0u;
layout(constant_id = 47) const uint S11O = 0u; layout(constant_id = 48) const uint S11K = 0u;
layout(constant_id = 49) const uint S12C = 0u; layout(constant_id = 50) const uint S12A = 0u;
layout(constant_id = 51) const uint S12O = 0u; layout(constant_id = 52) const uint S12K = 0u;
layout(constant_id = 53) const uint S13C = 0u; layout(constant_id = 54) const uint S13A = 0u;
layout(constant_id = 55) const uint S13O = 0u; layout(constant_id = 56) const uint S13K = 0u;
layout(constant_id = 57) const uint S14C = 0u; layout(constant_id = 58) const uint S14A = 0u;
layout(constant_id = 59) const uint S14O = 0u; layout(constant_id = 60) const uint S14K = 0u;
layout(constant_id = 61) const uint S15C = 0u; layout(constant_id = 62) const uint S15A = 0u;
layout(constant_id = 63) const uint S15O = 0u; layout(constant_id = 64) const uint S15K = 0u;
layout(constant_id = 65) const uint SWAPS = 0u;     // table t, channel c: 2 bits at t * 8 + c * 2
layout(constant_id = 66) const uint ACOMPARE = 0u;  // comp0 | comp1 << 3 | op << 6
layout(constant_id = 67) const uint FOG_TYPE = 0u;  // GXFogType (0 none)
layout(constant_id = 68) const uint FLAGS = 0u;     // bit 0 destination alpha; bit 8 + n manual scaling of coordinate n

// ---- what changes from draw to draw ----
layout(set = 0, binding = 0, std140) uniform Draw {
    ivec4 regs[4];     // PREV, REG0, REG1, REG2 as GXSetTevColor(S10) left them
    ivec4 konst[4];    // K0..K3
    uvec4 misc;        // x alpha refs (ref0 | ref1 << 8); y destination alpha; z fog colour (r | g << 8 | b << 16)
    vec4 fog_abc;      // A, B, C (gx_vulkan.cpp's pack_draw)
    vec4 depth_range;  // the viewport's near and far
    uvec4 tcscale[8];  // per texture coordinate: y s scale, z t scale (manual scaling)
};
layout(set = 0, binding = 1) uniform sampler2D u_tex[8];

vec4 sample_map(uint map, vec2 uv) {
    switch (map) {
    case 0u: return texture(u_tex[0], uv);
    case 1u: return texture(u_tex[1], uv);
    case 2u: return texture(u_tex[2], uv);
    case 3u: return texture(u_tex[3], uv);
    case 4u: return texture(u_tex[4], uv);
    case 5u: return texture(u_tex[5], uv);
    case 6u: return texture(u_tex[6], uv);
    default: return texture(u_tex[7], uv);
    }
}
vec2 map_size(uint map) {
    switch (map) {
    case 0u: return vec2(textureSize(u_tex[0], 0));
    case 1u: return vec2(textureSize(u_tex[1], 0));
    case 2u: return vec2(textureSize(u_tex[2], 0));
    case 3u: return vec2(textureSize(u_tex[3], 0));
    case 4u: return vec2(textureSize(u_tex[4], 0));
    case 5u: return vec2(textureSize(u_tex[5], 0));
    case 6u: return vec2(textureSize(u_tex[6], 0));
    default: return vec2(textureSize(u_tex[7], 0));
    }
}

ivec4 swizzle(ivec4 c, uint table) {
    uint t = SWAPS >> (table * 8u);
    return ivec4(c[t & 3u], c[(t >> 2) & 3u], c[(t >> 4) & 3u], c[(t >> 6) & 3u]);
}

// Konst fractions (selectors 0-7: 1, 7/8 ... 1/8) and the K registers.
int kfrac(uint sel) { return sel == 0u ? 255 : int(8u - sel) * 32 - 1; }
ivec3 kcolor(uint sel) {
    if (sel < 8u) return ivec3(kfrac(sel));
    if (sel < 16u) return konst[sel - 12u].rgb;
    uint k = (sel - 16u) & 3u, ch = (sel - 16u) >> 2;
    return ivec3(konst[k][ch]);
}
int kalpha(uint sel) {
    if (sel < 8u) return kfrac(sel);
    uint k = (sel - 16u) & 3u, ch = (sel - 16u) >> 2;
    return konst[k][ch];
}

ivec4 r[4];
ivec4 tex, ras;
ivec3 k3;
int ka;

ivec3 carg(uint a) {
    switch (a) {
    case 0u: return r[0].rgb;
    case 1u: return ivec3(r[0].a);
    case 2u: return r[1].rgb;
    case 3u: return ivec3(r[1].a);
    case 4u: return r[2].rgb;
    case 5u: return ivec3(r[2].a);
    case 6u: return r[3].rgb;
    case 7u: return ivec3(r[3].a);
    case 8u: return tex.rgb;
    case 9u: return ivec3(tex.a);
    case 10u: return ras.rgb;
    case 11u: return ivec3(ras.a);
    case 12u: return ivec3(255);
    case 13u: return ivec3(128);
    case 14u: return k3;
    default: return ivec3(0);
    }
}
int aarg(uint a) {
    switch (a) {
    case 0u: return r[0].a;
    case 1u: return r[1].a;
    case 2u: return r[2].a;
    case 3u: return r[3].a;
    case 4u: return tex.a;
    case 5u: return ras.a;
    case 6u: return ka;
    default: return 0;
    }
}

int bias_of(uint b) { return b == 1u ? 128 : b == 2u ? -128 : 0; }
ivec3 scaled(ivec3 v, uint s) { return s == 1u ? v * 2 : s == 2u ? v * 4 : s == 3u ? v >> 1 : v; }
int scaled1(int v, uint s) { return s == 1u ? v * 2 : s == 2u ? v * 4 : s == 3u ? v >> 1 : v; }

// The compare ops' keys: R8, GR16, BGR24 from a colour.
int key(ivec3 c, uint op) {
    uint w = (op - 8u) >> 1;  // 0 R8, 1 GR16, 2 BGR24
    return w == 0u ? c.r : w == 1u ? (c.g << 8 | c.r) : (c.b << 16 | c.g << 8 | c.r);
}

void set_out(uint reg, ivec3 c) {
    if (reg == 0u) r[0].rgb = c; else if (reg == 1u) r[1].rgb = c; else if (reg == 2u) r[2].rgb = c; else r[3].rgb = c;
}
void set_out_a(uint reg, int a) {
    if (reg == 0u) r[0].a = a; else if (reg == 1u) r[1].a = a; else if (reg == 2u) r[2].a = a; else r[3].a = a;
}

// One TEV stage, its configuration as arguments (constants after
// specialisation and inlining).
void stage(uint CW, uint AW, uint OW, uint KW) {
    uint map = OW & 255u, coord = (OW >> 8) & 255u, chan = (OW >> 16) & 255u;
    tex = ivec4(0);
    if (map != 255u) {
        vec3 tc = v_tex[coord];
        vec2 uv = tc.z != 0.0 ? tc.xy / tc.z : tc.xy;
        // GX scales coordinates by the texture's size, or by the game's own
        // scale (GXSetTexCoordScaleManually).
        if (((FLAGS >> (8u + coord)) & 1u) != 0u) uv = uv * vec2(tcscale[coord].yz) / map_size(map);
        tex = swizzle(ivec4(round(sample_map(map, uv) * 255.0)), (KW >> 18) & 3u);
    }
    vec4 rc = chan == 4u ? v_col0 : chan == 5u ? v_col1 : vec4(0.0);
    ras = swizzle(ivec4(round(clamp(rc, 0.0, 255.0))), (KW >> 16) & 3u);
    k3 = kcolor(KW & 255u);
    ka = kalpha((KW >> 8) & 255u);

    // colour
    ivec3 A = carg(CW & 15u) & 255, B = carg((CW >> 4) & 15u) & 255;
    ivec3 C = carg((CW >> 8) & 15u) & 255, D = carg((CW >> 12) & 15u);
    uint op = (CW >> 16) & 15u, bias = (CW >> 20) & 3u, scl = (CW >> 22) & 3u, out_reg = (CW >> 25) & 3u;
    bool clamp8 = ((CW >> 24) & 1u) != 0u;
    ivec3 cv;
    if (op < 2u) {
        ivec3 c1 = C + (C >> 7);
        ivec3 lerp = (A * (256 - c1) + B * c1 + 128) >> 8;
        cv = scaled((op == 0u ? D + lerp : D - lerp) + bias_of(bias), scl);
    } else if (op >= 14u) {
        bvec3 hit = op == 14u ? greaterThan(A, B) : equal(A, B);
        cv = D + ivec3(hit) * C;
    } else {
        bool hit = (op & 1u) == 0u ? key(A, op) > key(B, op) : key(A, op) == key(B, op);
        cv = D + (hit ? C : ivec3(0));
    }
    cv = clamp8 ? clamp(cv, 0, 255) : clamp(cv, -1024, 1023);

    // alpha (its compare ops 8-13 compare the stage's colour inputs)
    int aA = aarg(AW & 7u) & 255, aB = aarg((AW >> 4) & 7u) & 255;
    int aC = aarg((AW >> 8) & 7u) & 255, aD = aarg((AW >> 12) & 7u);
    uint aop = (AW >> 16) & 15u, abias = (AW >> 20) & 3u, ascl = (AW >> 22) & 3u, aout = (AW >> 25) & 3u;
    bool aclamp = ((AW >> 24) & 1u) != 0u;
    int av;
    if (aop < 2u) {
        int c1 = aC + (aC >> 7);
        int lerp = (aA * (256 - c1) + aB * c1 + 128) >> 8;
        av = scaled1((aop == 0u ? aD + lerp : aD - lerp) + bias_of(abias), ascl);
    } else if (aop >= 14u) {
        bool hit = aop == 14u ? aA > aB : aA == aB;
        av = aD + (hit ? aC : 0);
    } else {
        bool hit = (aop & 1u) == 0u ? key(A, aop) > key(B, aop) : key(A, aop) == key(B, aop);
        av = aD + (hit ? aC : 0);
    }
    av = aclamp ? clamp(av, 0, 255) : clamp(av, -1024, 1023);

    set_out(out_reg, cv);
    set_out_a(aout, av);
}

bool alpha_test(uint comp, int a, int ref) {
    switch (comp) {
    case 0u: return false;
    case 1u: return a < ref;
    case 2u: return a == ref;
    case 3u: return a <= ref;
    case 4u: return a > ref;
    case 5u: return a != ref;
    case 6u: return a >= ref;
    default: return true;
    }
}

void main() {
    for (int i = 0; i < 4; ++i) r[i] = regs[i];
    if (NUM_STAGES > 0u) stage(S0C, S0A, S0O, S0K);
    if (NUM_STAGES > 1u) stage(S1C, S1A, S1O, S1K);
    if (NUM_STAGES > 2u) stage(S2C, S2A, S2O, S2K);
    if (NUM_STAGES > 3u) stage(S3C, S3A, S3O, S3K);
    if (NUM_STAGES > 4u) stage(S4C, S4A, S4O, S4K);
    if (NUM_STAGES > 5u) stage(S5C, S5A, S5O, S5K);
    if (NUM_STAGES > 6u) stage(S6C, S6A, S6O, S6K);
    if (NUM_STAGES > 7u) stage(S7C, S7A, S7O, S7K);
    if (NUM_STAGES > 8u) stage(S8C, S8A, S8O, S8K);
    if (NUM_STAGES > 9u) stage(S9C, S9A, S9O, S9K);
    if (NUM_STAGES > 10u) stage(S10C, S10A, S10O, S10K);
    if (NUM_STAGES > 11u) stage(S11C, S11A, S11O, S11K);
    if (NUM_STAGES > 12u) stage(S12C, S12A, S12O, S12K);
    if (NUM_STAGES > 13u) stage(S13C, S13A, S13O, S13K);
    if (NUM_STAGES > 14u) stage(S14C, S14A, S14O, S14K);
    if (NUM_STAGES > 15u) stage(S15C, S15A, S15O, S15K);
    ivec4 res = clamp(r[0], 0, 255);

    uint comp0 = ACOMPARE & 7u, comp1 = (ACOMPARE >> 3) & 7u, aop = (ACOMPARE >> 6) & 3u;
    bool t0 = alpha_test(comp0, res.a, int(misc.x & 255u)), t1 = alpha_test(comp1, res.a, int((misc.x >> 8) & 255u));
    bool pass = aop == 0u ? (t0 && t1) : aop == 1u ? (t0 || t1) : aop == 2u ? (t0 != t1) : (t0 == t1);
    if (!pass) discard;

    vec4 outc = vec4(res) / 255.0;
    // Fog: the depth as GX's fog sees it (0 at the viewport's near, 1 at
    // its far), its distance into the fog (0 at the start, 1 at the end),
    // then the curve. APPROX: GX's fixed-point steps.
    if (FOG_TYPE != 0u) {
        float zs = (gl_FragCoord.z - depth_range.x) / max(depth_range.y - depth_range.x, 1e-6);
        float ze = (FOG_TYPE & 8u) != 0u ? fog_abc.x * zs - fog_abc.z : fog_abc.x / (fog_abc.y - zs) - fog_abc.z;
        float f = clamp(ze, 0.0, 1.0);
        uint curve = FOG_TYPE & 7u;
        if (curve == 4u) f = 1.0 - exp2(-8.0 * f);
        else if (curve == 5u) f = 1.0 - exp2(-8.0 * f * f);
        else if (curve == 6u) f = exp2(-8.0 * (1.0 - f));
        else if (curve == 7u) f = exp2(-8.0 * (1.0 - f) * (1.0 - f));
        vec3 fc = vec3(misc.z & 255u, (misc.z >> 8) & 255u, (misc.z >> 16) & 255u) / 255.0;
        outc.rgb = mix(outc.rgb, fc, f);
    }
    if ((FLAGS & 1u) != 0u) outc.a = float(misc.y & 255u) / 255.0;  // destination alpha
    o_color = outc;
}
