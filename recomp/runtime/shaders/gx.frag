// GX's pixel pipeline for one draw: the TEV stages and the alpha compare,
// interpreted from the draw's uniform block (packed by gx_vulkan.cpp's
// pack_draw). Integer arithmetic on GX's own ranges: 8-bit inputs, 11-bit
// signed registers. APPROX: the TEV's rounding, and the konst fractions.
#version 450

layout(location = 0) in vec4 v_col0;
layout(location = 1) in vec4 v_col1;
layout(location = 2) in vec3 v_tex[8];
layout(location = 0) out vec4 o_color;

layout(set = 0, binding = 0, std140) uniform Draw {
    ivec4 regs[4];     // PREV, REG0, REG1, REG2 as GXSetTevColor(S10) left them
    ivec4 konst[4];    // K0..K3
    uvec4 swaps[4];    // swap tables: the source channel of r, g, b, a
    uvec4 misc;        // x stages; y alpha compare (comp0 | comp1 << 3 | op << 6); z refs (ref0 | ref1 << 8); w flags
    uvec4 stages[48];  // per stage: colour, alpha, order (pack_draw)
    uvec4 fog;         // x GXFogType (0 none), y colour (r | g << 8 | b << 16)
    vec4 fog_abc;      // A, B, C as pack_draw computes them from GXSetFog's parameters
    vec4 depth_range;  // the viewport's near and far
    uvec4 tcscale[8];  // per texture coordinate: x manual scaling on, y s scale, z t scale
};
layout(set = 0, binding = 1) uniform sampler2D u_tex[8];

vec4 sample_map(uint map, vec2 uv) {
    switch (map) {  // constant indices: no dynamic indexing of sampler arrays
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
    uvec4 s = swaps[table];
    return ivec4(c[s.x], c[s.y], c[s.z], c[s.w]);
}

// Konst fractions (selectors 0-7: 1, 7/8 ... 1/8) and the K registers.
int kfrac(uint sel) { return sel == 0u ? 255 : int(8u - sel) * 32 - 1; }
ivec3 kcolor(uint sel) {
    if (sel < 8u) return ivec3(kfrac(sel));
    if (sel >= 12u && sel < 16u) return konst[sel - 12u].rgb;
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
    uint n = misc.x;
    for (uint i = 0u; i < n; ++i) {
        uvec4 cs = stages[i * 3u], as = stages[i * 3u + 1u], od = stages[i * 3u + 2u];
        tex = ivec4(0);
        if (od.x != 255u) {
            vec3 tc = v_tex[od.y];
            vec2 uv = tc.z != 0.0 ? tc.xy / tc.z : tc.xy;
            // GX scales coordinates by the texture's size, or by the
            // game's own scale (GXSetTexCoordScaleManually).
            uvec4 ts = tcscale[od.y];
            if (ts.x != 0u) uv = uv * vec2(ts.yz) / map_size(od.x);
            tex = swizzle(ivec4(round(sample_map(od.x, uv) * 255.0)), (cs.w >> 2) & 3u);
        }
        vec4 rc = od.z == 4u ? v_col0 : od.z == 5u ? v_col1 : vec4(0.0);
        ras = swizzle(ivec4(round(clamp(rc, 0.0, 255.0))), cs.w & 3u);
        k3 = kcolor(cs.z);
        ka = kalpha(od.w);

        // colour
        ivec3 A = carg(cs.x & 15u) & 255, B = carg((cs.x >> 4) & 15u) & 255;
        ivec3 C = carg((cs.x >> 8) & 15u) & 255, D = carg((cs.x >> 12) & 15u);
        uint op = cs.y & 15u, bias = (cs.y >> 4) & 3u, scl = (cs.y >> 6) & 3u, out_reg = (cs.y >> 9) & 3u;
        bool clamp8 = ((cs.y >> 8) & 1u) != 0u;
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
        int aA = aarg(as.x & 7u) & 255, aB = aarg((as.x >> 4) & 7u) & 255;
        int aC = aarg((as.x >> 8) & 7u) & 255, aD = aarg((as.x >> 12) & 7u);
        uint aop = as.y & 15u, abias = (as.y >> 4) & 3u, ascl = (as.y >> 6) & 3u, aout = (as.y >> 9) & 3u;
        bool aclamp = ((as.y >> 8) & 1u) != 0u;
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

        r[out_reg].rgb = cv;
        r[aout].a = av;
    }
    ivec4 res = clamp(r[0], 0, 255);

    uint comp0 = misc.y & 7u, comp1 = (misc.y >> 3) & 7u, aop = (misc.y >> 6) & 3u;
    bool t0 = alpha_test(comp0, res.a, int(misc.z & 255u)), t1 = alpha_test(comp1, res.a, int((misc.z >> 8) & 255u));
    bool pass = aop == 0u ? (t0 && t1) : aop == 1u ? (t0 || t1) : aop == 2u ? (t0 != t1) : (t0 == t1);
    if (!pass) discard;

    vec4 outc = vec4(res) / 255.0;
    // Fog: the depth as GX's fog sees it (0 at the viewport's near, 1 at
    // its far), its distance into the fog (0 at the start, 1 at the end),
    // then the curve. APPROX: GX's fixed-point steps.
    if (fog.x != 0u) {
        float zs = (gl_FragCoord.z - depth_range.x) / max(depth_range.y - depth_range.x, 1e-6);
        float ze = (fog.x & 8u) != 0u ? fog_abc.x * zs - fog_abc.z : fog_abc.x / (fog_abc.y - zs) - fog_abc.z;
        float f = clamp(ze, 0.0, 1.0);
        switch (fog.x & 7u) {
        case 4u: f = 1.0 - exp2(-8.0 * f); break;
        case 5u: f = 1.0 - exp2(-8.0 * f * f); break;
        case 6u: f = exp2(-8.0 * (1.0 - f)); break;
        case 7u: f = exp2(-8.0 * (1.0 - f) * (1.0 - f)); break;
        default: break;  // 2: linear
        }
        vec3 fc = vec3(fog.y & 255u, (fog.y >> 8) & 255u, (fog.y >> 16) & 255u) / 255.0;
        outc.rgb = mix(outc.rgb, fc, f);
    }
    if ((misc.w & 1u) != 0u) outc.a = float((misc.w >> 8) & 255u) / 255.0;  // destination alpha
    o_color = outc;
}
