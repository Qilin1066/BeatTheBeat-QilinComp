#version 450
// The CRT look (settings: look = crt), as the other Qilin recomps have it: the
// console's raster lines drawn as beams with darker gaps between them, and an
// aperture grille's red, green and blue stripes down the screen, one window
// pixel each. Light adds up in linear terms (a tube's 2.4 gamma).
//
// A beam's height grows with its brightness (a Gaussian whose total is the
// line's light, so the average brightness is kept). Along a line the frame is
// sampled as it is, keeping the detail of a higher internal resolution;
// across lines, each beam takes its line's colour at the line's centre. The
// gaps fade out where a raster line gets fewer than about two window rows,
// where they would only make moire.
layout(set = 0, binding = 0) uniform sampler2D frame;
layout(push_constant) uniform Push {
    vec2 uv_scale;    // the picture's part of the frame image (it is larger)
    float lines;      // the console's raster lines in the picture
    float scanlines;  // 0-1: how dark the gaps are
    float mask;       // 0-1: the stripes' strength
} push;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 colour;

vec3 line_light(float k) {  // raster line k's light at this column, linear
    const float y = (clamp(k, 0.0, push.lines - 1.0) + 0.5) / push.lines;
    return pow(textureLod(frame, vec2(uv.x, y) * push.uv_scale, 0.0).rgb, vec3(2.4));
}

vec3 beam(vec3 light, float d) {  // a line's light at d lines from its centre
    const vec3 width = mix(vec3(0.18), vec3(0.42), sqrt(light));
    return light * exp(-0.5 * (d * d) / (width * width)) / (width * 2.5066283);
}

void main() {
    const float across = uv.y * push.lines;  // in raster lines
    const float v = across - 0.5;
    const float k = floor(v);
    const float t = v - k;  // 0 on line k's centre, 1 on line k+1's
    const vec3 a = line_light(k), b = line_light(k + 1.0);
    const vec3 lit = beam(a, t) + beam(b, 1.0 - t);
    const vec3 plain = t < 0.5 ? a : b;
    const float rows_per_line = 1.0 / max(fwidth(across), 1e-5);
    const float fade = clamp(rows_per_line - 1.25, 0.0, 1.0);
    vec3 light = mix(plain, lit, push.scanlines * fade);

    const int stripe = int(mod(gl_FragCoord.x, 3.0));
    const float dim = 1.0 - push.mask;
    vec3 grille = vec3(dim);
    grille[stripe] = 1.0;
    light *= grille * (3.0 / (1.0 + 2.0 * dim));  // the stripes keep the average brightness

    colour = vec4(pow(clamp(light, 0.0, 1.0), vec3(1.0 / 2.4)), 1.0);
}
