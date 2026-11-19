// A copy from the EFB to a texture: the source texel (or the 2x2 box's
// average), converted as the copy format reads it. Intensity is luma with
// BT.601's studio-range coefficients (APPROX: the hardware's own rounding).
#version 450

layout(push_constant) uniform Copy {
    vec4 src;
    uvec4 mode;
} p;
layout(set = 0, binding = 0) uniform sampler2D u_efb;

layout(location = 0) in vec2 v_pos;
layout(location = 0) out vec4 o_color;

void main() {
    vec4 c;
    if (p.mode.y != 0u) {
        ivec2 q = ivec2(floor(v_pos - 0.5));
        c = (texelFetch(u_efb, q, 0) + texelFetch(u_efb, q + ivec2(1, 0), 0) + texelFetch(u_efb, q + ivec2(0, 1), 0) +
             texelFetch(u_efb, q + ivec2(1, 1), 0)) * 0.25;
    } else {
        c = texelFetch(u_efb, ivec2(floor(v_pos)), 0);
    }
    float y = dot(c.rgb, vec3(0.257, 0.504, 0.098)) + 16.0 / 255.0;
    switch (p.mode.x) {
    case 0u: o_color = c; break;                          // RGBA8, RGB5A3
    case 1u: o_color = vec4(c.rgb, 1.0); break;           // RGB565
    case 2u: o_color = vec4(y); break;                    // I4, I8
    case 3u: o_color = vec4(vec3(y), c.a); break;         // IA4, IA8
    case 4u: o_color = vec4(c.r); break;                  // R4, R8
    case 5u: o_color = vec4(vec3(c.r), c.a); break;       // RA4, RA8
    case 6u: o_color = vec4(c.g); break;                  // G8
    case 7u: o_color = vec4(c.b); break;                  // B8
    case 8u: o_color = vec4(c.a); break;                  // A8
    case 9u: o_color = vec4(vec3(c.g), c.r); break;       // RG8, read as IA8
    default: o_color = vec4(vec3(c.b), c.g); break;       // GB8, read as IA8
    }
}
