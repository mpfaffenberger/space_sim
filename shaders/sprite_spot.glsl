// -----------------------------------------------------------------------------
// sprite_spot.glsl — procedural soft-glow billboard for animated lights.
//
// A "light spot" is a small camera-facing quad at a world position (derived
// from a sprite's billboard basis + the light's UV offset) with a radial
// falloff fragment shader. Additively blended so multiple spots overlap
// smoothly, and fed into the existing bloom pass so each spot grows a
// bright halo in post.
//
// The CPU bakes the animation intensity (for blink / pulse / strobe) into
// the alpha uniform — we don't pass `time` to the GPU because per-spot
// phase/hz/kind logic is cleaner in C++ and the resulting intensity is
// just one float. One uniform block per draw is fine for our sprite counts.
//
// Radial falloff: the corner attribute is passed through in [-1,+1]^2;
// the fragment computes r = length(corner), then glow = (1 - smoothstep)².
// Squaring sharpens the peak so the center reads as a bright dot rather
// than a smeared cloud, which is exactly the "navigation light" vibe.
// -----------------------------------------------------------------------------

@vs spot_vs

layout(binding=0) uniform vs_spot_params {
    mat4 view_proj;
    vec4 cam_right;     // world-space camera right, .w unused
    vec4 cam_up;        // world-space camera up,    .w unused
    vec4 spot_pos;      // .xyz = world position, .w = radius (world units)
};

in vec2 a_corner;   // unit quad corners in [-1,+1]^2

out vec2 v_corner;  // pass-through so the fragment shader can compute r

void main() {
    vec3 pos = spot_pos.xyz
             + cam_right.xyz * (a_corner.x * spot_pos.w)
             + cam_up.xyz    * (a_corner.y * spot_pos.w);
    vec4 clip = view_proj * vec4(pos, 1.0);

    // Clip-space depth bias: subtract a small fraction of w from z so the
    // fragment's NDC z is shifted *toward* the near plane by a precision-
    // invariant amount. World-space biases don't work here — at z≈25000
    // with near=1/far=500000 the depth buffer's ULP floor is larger than
    // any reasonable world-space nudge, so the light and hull z-fight and
    // flicker as the camera moves. 0.001*w is ~0.05% of the [0,1] depth
    // range, plenty of headroom to consistently win LEQUAL, while still
    // being imperceptible to the eye (not visibly "floating" off the hull).
    clip.z -= clip.w * 0.001;
    gl_Position = clip;
    v_corner = a_corner;
}
@end

@fs spot_fs

layout(binding=1) uniform fs_spot_params {
    vec4 color_and_intensity;   // .rgb = light colour, .a = 0..1 intensity
};

in  vec2 v_corner;
out vec4 frag;

void main() {
    // Radial distance from center: 0 at center, 1 at corners. Beyond the
    // inscribed circle (r > 1) we fully fade out — smoothstep handles this
    // so the quad doesn't show a hard square border.
    float r = length(v_corner);

    // Two-zone falloff: a *hot core* that blows out to full white at the
    // very center (so bloom's bright-pass threshold always grabs it), then
    // a wide soft corona that reads as the "glow around the bulb". Without
    // this, an RGBA8 LDR scene buffer clamps the output and the glow stays
    // dim. `core` spikes sharply near r=0; `halo` is the diffuse aura.
    float core = 1.0 - smoothstep(0.0, 0.15, r);      // hot center, sharp
    float halo = 1.0 - smoothstep(0.0, 1.0, r);        // soft wide falloff

    // Core goes pure white to saturate the buffer (bloom bait); halo is
    // tinted by the light's color. `intensity` modulates both.
    float a    = (core + halo) * color_and_intensity.a;
    vec3  col  = mix(color_and_intensity.rgb, vec3(1.0), core);
    frag = vec4(col * a, a);
}
@end

@program sprite_spot spot_vs spot_fs
