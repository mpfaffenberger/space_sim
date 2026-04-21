// -----------------------------------------------------------------------------
// post_composite.glsl — final blit from offscreen scene to swapchain.
//
// Reads the scene color and the (already blurred) bloom texture, mixes them
// together, and additionally draws a procedural lens flare based on the
// sun's screen-space position. Output goes straight to the swapchain.
//
// The lens flare has three components, all additive:
//
//   * HALO    — a soft radial gaussian at the sun's projected position.
//               Mostly redundant with bloom, but keeps the flare intense
//               even when the sun disk is small/off-center.
//
//   * STREAKS — an anamorphic horizontal streak + a thinner vertical one
//               through the sun. This is the "J.J. Abrams" effect that
//               every sci-fi lens adds. Width is aspect-corrected so the
//               streak doesn't get fat when the window is wide.
//
//   * GHOSTS  — 4 small disks along the line from the sun through the
//               screen center, at fractional positions. Classic "junk
//               reflecting off lens elements" look.
//
// Visibility is scaled by `flare_intensity`, which the CPU sets to 0 when
// the sun is behind the camera or far off-screen (so no flare artifacts
// ghost in from outside the view).
// -----------------------------------------------------------------------------

@vs vs
out vec2 v_uv;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
@end

@fs fs
layout(binding=0) uniform texture2D u_scene;
layout(binding=1) uniform texture2D u_bloom;
layout(binding=0) uniform sampler   u_smp;

layout(binding=0) uniform post_composite_params {
    vec4 sun_ndc_and_flare;   // .xy = sun NDC [-1,1] (y up), .z = flare intensity, .w = aspect (w/h)
    vec4 tint_and_bloom;      // .rgb = flare tint, .a = bloom blend amount
};

in  vec2 v_uv;
out vec4 frag_color;

vec3 procedural_flare(vec2 pixel_ndc, vec2 sun_ndc, float aspect, vec3 tint) {
    // Aspect-correct the ndc space so circles are actually circular.
    vec2 p = vec2(pixel_ndc.x * aspect, pixel_ndc.y);
    vec2 s = vec2(sun_ndc.x   * aspect, sun_ndc.y);
    vec2 d = p - s;

    // --- HALO ---------------------------------------------------------
    float halo_r = length(d);
    vec3 halo = tint * exp(-halo_r * 5.0) * 0.35;

    // --- STREAKS (anamorphic) -----------------------------------------
    // Horizontal streak: very thin in y, long in x. Vertical: opposite.
    float streak_h = exp(-abs(d.y) * 110.0) * exp(-abs(d.x) * 0.9);
    float streak_v = exp(-abs(d.x) * 200.0) * exp(-abs(d.y) * 1.3);
    vec3 streaks   = tint * (streak_h * 0.9 + streak_v * 0.45);

    // --- GHOSTS -------------------------------------------------------
    // Positions sampled along the line from sun through screen center,
    // in aspect-corrected ndc. Each ghost gets a different tint bias to
    // sell "chromatic glass elements."
    vec3 ghosts = vec3(0.0);
    float spacing[4];  spacing[0] = -0.35; spacing[1] = -0.7;
                       spacing[2] =  0.25; spacing[3] =  0.55;
    vec3  tints[4];
    tints[0] = vec3(1.0, 0.85, 0.55);
    tints[1] = vec3(1.0, 0.55, 0.35);
    tints[2] = vec3(0.85, 0.95, 1.0);
    tints[3] = vec3(0.6,  0.8,  1.0);
    for (int i = 0; i < 4; ++i) {
        vec2 gp = s * spacing[i];
        float gd = length(p - gp);
        ghosts += tints[i] * exp(-gd * 28.0) * 0.18;
    }

    return halo + streaks + ghosts;
}

void main() {
    vec3 scene = texture(sampler2D(u_scene, u_smp), v_uv).rgb;
    vec3 bloom = texture(sampler2D(u_bloom, u_smp), v_uv).rgb;

    vec2 pixel_ndc = v_uv * 2.0 - 1.0;
    vec2 sun_ndc   = sun_ndc_and_flare.xy;
    float flare_i  = sun_ndc_and_flare.z;
    float aspect   = sun_ndc_and_flare.w;

    vec3 flare = vec3(0.0);
    if (flare_i > 0.0) {
        flare = procedural_flare(pixel_ndc, sun_ndc, aspect, tint_and_bloom.rgb) * flare_i;
    }

    vec3 color = scene + bloom * tint_and_bloom.a + flare;
    frag_color = vec4(color, 1.0);
}
@end

@program post_composite vs fs
