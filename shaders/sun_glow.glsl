// -----------------------------------------------------------------------------
// sun_glow.glsl — camera-facing corona + procedural sunbeams.
//
// Drawn as one big billboard quad centered on the sun's world position.
// Two effects stacked, additively blended on top of the solid sun sphere:
//
//   1. Soft radial glow     : exp(-k * r^2) gives a halo that extends
//                             several sun-radii out and reads as atmosphere
//                             / scattered-light diffusion.
//
//   2. Procedural sunbeams  : pow(|sin(angle * N + phase)|, sharpness)
//                             creates N radial spokes. Modulated by the
//                             same radial attenuation so rays fade with
//                             distance instead of extending to the quad
//                             edge as hard stripes.
//
// No screen-space post-process, no framebuffer sampling — just a single
// billboard shader. Works even when nothing occludes the sun; when we later
// add ships flying past it, the solid geometry will naturally block the
// glow pixels behind it (because we keep depth-test on).
// -----------------------------------------------------------------------------

@vs vs
layout(binding=0) uniform sun_glow_vs_params {
    mat4 view_proj;
    vec4 sun_world_pos;   // .xyz = sun center; .w = billboard half-size
    vec4 cam_right;       // world-space camera right (.xyz); .w unused
    vec4 cam_up;          // world-space camera up
};

in vec2 a_corner;   // one of { (-1,-1), (+1,-1), (+1,+1), (-1,+1) }

out vec2 v_uv;      // same as a_corner, in [-1,+1]^2

void main() {
    float size = sun_world_pos.w;
    vec3  world = sun_world_pos.xyz
                + cam_right.xyz * a_corner.x * size
                + cam_up.xyz    * a_corner.y * size;
    v_uv = a_corner;
    gl_Position = view_proj * vec4(world, 1.0);
}
@end

@fs fs
layout(binding=1) uniform sun_glow_fs_params {
    vec4 glow_color;       // rgb glow + alpha scale in .a
    vec4 ray_params;       // .x = ray count, .y = sharpness, .z = phase, .w = ray strength
};

in  vec2 v_uv;
out vec4 frag_color;

void main() {
    float r = length(v_uv);
    if (r > 1.0) discard;

    // -------------------------------------------------------------------
    // Freelancer-style atmospheric halo.
    //
    // Core idea: a gaussian-like bloom that is forced to reach EXACTLY zero
    // at the billboard edge (otherwise you see a hard circular boundary
    // against dark space). We subtract the edge value and renormalise.
    // -------------------------------------------------------------------
    const float k = 4.0;
    float g        = exp(-k * r * r);
    float g_edge   = exp(-k);                       // value at r=1
    float halo     = max(0.0, (g - g_edge) / (1.0 - g_edge));

    // Blown-out white hotspot at the very center — fakes bloom/HDR in LDR
    // by pushing the inner few percent of the corona toward white. This is
    // what makes the sun read as "overexposed" instead of "sticker."
    float bloom = pow(max(0.0, 1.0 - r * 3.0), 3.0);

    // Tint fades from white at center to glow_color at the halo edge —
    // same trick photographers' lens flares use to sell HDR range.
    vec3  tint  = mix(vec3(1.0), glow_color.rgb, smoothstep(0.0, 0.35, r));

    // -------------------------------------------------------------------
    // Optional subtle rays. Kept for flavor but much gentler than before:
    // default ray_strength is low and sharpness is soft, so they read as
    // "diffraction streaks" rather than starburst spokes.
    // -------------------------------------------------------------------
    float angle = atan(v_uv.y, v_uv.x);
    float rays  = pow(abs(sin(angle * ray_params.x + ray_params.z)),
                      ray_params.y);
    rays *= pow(max(0.0, 1.0 - r), 2.0) * ray_params.w;

    vec3 color = tint * halo + vec3(1.0) * bloom + glow_color.rgb * rays;
    frag_color = vec4(color, 1.0) * glow_color.a;
}
@end

@program sun_glow vs fs
