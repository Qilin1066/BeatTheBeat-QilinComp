// GX's vertices arrive transformed and lit (gx_vulkan.cpp does GX's
// transform unit on the CPU): clip position, the two colour channels (0..255)
// and eight texture coordinates (s, t, q).
#version 450

layout(location = 0) in vec4 a_pos;
layout(location = 1) in vec4 a_col0;
layout(location = 2) in vec4 a_col1;
layout(location = 3) in vec3 a_tex[8];

layout(location = 0) out vec4 v_col0;
layout(location = 1) out vec4 v_col1;
layout(location = 2) out vec3 v_tex[8];

void main() {
    gl_Position = a_pos;
    v_col0 = a_col0;
    v_col1 = a_col1;
    for (int i = 0; i < 8; ++i) v_tex[i] = a_tex[i];
}
