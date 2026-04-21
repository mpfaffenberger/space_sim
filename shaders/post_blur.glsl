// -----------------------------------------------------------------------------
// post_blur.glsl — one shader used for three things:
//
//   1. Bright-pass + horizontal blur (scene → bloom_a). threshold > 0.
//   2. Vertical blur            (bloom_a → bloom_b).    threshold = 0.
//
// Decoupling bright-pass into its own dedicated pass would cost one extra
// fullscreen draw with zero visual benefit. Branching on `threshold` here
// keeps the post pipeline to 2 passes total.
// -----------------------------------------------------------------------------

@vs vs
// Fullscreen triangle trick: derive clip-space + uv from the vertex ID.
// No vertex buffer needed — we just draw 3 vertices with no bindings.
// (sokol-shdc supports this; see the invocation in postprocess.cpp.)
out vec2 v_uv;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
@end

@fs fs
layout(binding=0) uniform texture2D u_src;
layout(binding=0) uniform sampler   u_smp;

layout(binding=0) uniform post_blur_params {
    vec4 texel_and_dir;    // .xy = 1 / src_size (texel size); .zw = blur direction (1,0) or (0,1)
    vec4 blur_cfg;         // .x = radius (pixels), .y = threshold (0 = no brightpass), .z,.w unused
};

in  vec2 v_uv;
out vec4 frag_color;

// 9-tap gaussian with sigma ≈ radius/2. Weights precomputed:
//   w[0] = 0.2270270270
//   w[1] = 0.1945945946
//   w[2] = 0.1216216216
//   w[3] = 0.0540540541
//   w[4] = 0.0162162162
// Sum * 2 - center = 1.0 (approximately — normalised in the loop anyway).

// Soft-knee bright-pass. Two-sided smoothstep around the threshold means
// pixels that waver across the boundary (animated noise, MSAA dithering)
// no longer cause the sparkle/flicker you get from a hard cutoff.
vec3 bright_pass(vec3 c, float threshold) {
    float lum = max(max(c.r, c.g), c.b);
    float t   = smoothstep(threshold * 0.85, threshold * 1.15, lum);
    return c * t;
}

void main() {
    vec2 texel = texel_and_dir.xy;
    vec2 dir   = texel_and_dir.zw;
    float radius   = blur_cfg.x;
    float threshold = blur_cfg.y;

    // Five positive offsets; center + two sides = 9 taps total.
    float weights[5];
    weights[0] = 0.2270270270;
    weights[1] = 0.1945945946;
    weights[2] = 0.1216216216;
    weights[3] = 0.0540540541;
    weights[4] = 0.0162162162;

    vec3 sum = texture(sampler2D(u_src, u_smp), v_uv).rgb * weights[0];
    for (int i = 1; i < 5; ++i) {
        vec2 off = dir * texel * float(i) * radius;
        sum += texture(sampler2D(u_src, u_smp), v_uv + off).rgb * weights[i];
        sum += texture(sampler2D(u_src, u_smp), v_uv - off).rgb * weights[i];
    }

    if (threshold > 0.0) {
        sum = bright_pass(sum, threshold);
    }

    frag_color = vec4(sum, 1.0);
}
@end

@program post_blur vs fs
