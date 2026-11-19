// A copy from the EFB to a texture (gx_vulkan.cpp's copy_texture): one
// triangle covering the target; v_pos is the EFB position the target's
// pixel centre maps to.
#version 450

layout(push_constant) uniform Copy {
    vec4 src;    // the source rectangle in EFB pixels: x, y, width, height
    uvec4 mode;  // x conversion, y 1 for a 2x2 box (GXSetTexCopyDst's mipmap flag)
} p;

layout(location = 0) out vec2 v_pos;

void main() {
    vec2 c = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(c * 2.0 - 1.0, 0.0, 1.0);
    v_pos = p.src.xy + c * p.src.zw;
}
