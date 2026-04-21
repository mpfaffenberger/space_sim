// -----------------------------------------------------------------------------
// sun_gas.glsl — noisy animated gas shell around the sun.
//
// A camera-facing billboard a few sun-radii across, drawn additively AFTER
// the solid sphere. Purpose: break up the hard sphere-vs-space boundary
// with lumpy volumetric-looking haze that the sphere "emerges from."
//
// The density field:
//   * peaks just inside the billboard edge (makes a RING of haze at the
//     silhouette, which is the strongest Freelancer-like cue)
//   * drops toward the sphere's projected center (so the sphere itself is
//     mostly unobscured)
//   * modulated by fBM noise for lumpiness and animated over time
//
// No real volumetric integration — this is a screen-space approximation
// that sells the effect for ~30 lines of GPU work. When we add real 3D
// gas clouds elsewhere in the universe we'll use raymarching; until then
// this is the right cost/benefit trade.
// -----------------------------------------------------------------------------

@vs vs
layout(binding=0) uniform sun_gas_vs_params {
    mat4 view_proj;
    vec4 sun_world_pos;   // .xyz = sun center; .w = billboard half-size
    vec4 cam_right;       // world-space camera right
    vec4 cam_up;          // world-space camera up
};

in vec2 a_corner;

out vec2 v_uv;

void main() {
    float size = sun_world_pos.w;
    vec3 world = sun_world_pos.xyz
               + cam_right.xyz * a_corner.x * size
               + cam_up.xyz    * a_corner.y * size;
    v_uv = a_corner;
    gl_Position = view_proj * vec4(world, 1.0);
}
@end

@fs fs
layout(binding=1) uniform sun_gas_fs_params {
    vec4 gas_color;        // .rgb = tint; .a = overall strength
    vec4 gas_params;       // .x = time, .y = noise_scale, .z = ring_center_r, .w = ring_width
};

in  vec2 v_uv;
out vec4 frag_color;

// Same value-noise / fBM as sun.glsl. Duplicated intentionally (each
// shader translates into its own shader module); tiny source, trivially
// folded by the compiler.
float hash(vec3 p) {
    p = fract(p * 0.3183099 + 0.1);
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float vnoise(vec3 p) {
    vec3 i = floor(p); vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(
        mix(mix(hash(i + vec3(0,0,0)), hash(i + vec3(1,0,0)), f.x),
            mix(hash(i + vec3(0,1,0)), hash(i + vec3(1,1,0)), f.x), f.y),
        mix(mix(hash(i + vec3(0,0,1)), hash(i + vec3(1,0,1)), f.x),
            mix(hash(i + vec3(0,1,1)), hash(i + vec3(1,1,1)), f.x), f.y),
        f.z);
}
// Gas shell is low-detail by nature — 2 octaves is enough, halves cost.
float fbm(vec3 p) {
    float v = 0.0, amp = 0.6;
    for (int i = 0; i < 2; ++i) {
        v += vnoise(p) * amp;
        p  *= 2.13;
        amp *= 0.5;
    }
    return v;
}

void main() {
    float r = length(v_uv);
    if (r > 1.0) discard;

    float time    = gas_params.x;
    float nscale  = gas_params.y;
    float rc      = gas_params.z;    // radius of peak density
    float rw      = gas_params.w;    // ring half-width

    // Density ring — gaussian centered on rc. So density peaks around
    // a "shell" distance, tapers to zero at center AND at the edge, which
    // is exactly the silhouette-hugging shape we want.
    float ring = exp(-pow((r - rc) / rw, 2.0));
    // Kill the edge cleanly so no hard billboard boundary is visible.
    ring *= smoothstep(1.0, 0.75, r);

    // Lumpiness — 3D noise so the clouds look different from every angle.
    // z-component derived from `r` gives vertical layering; time scrolls.
    vec3 np = vec3(v_uv * nscale, r * nscale + time * 0.08);
    float cloud = fbm(np);
    // Push contrast so we get gaps of clear space between wisps, like
    // Freelancer's patchy nebula tendrils.
    cloud = smoothstep(0.35, 0.85, cloud);

    float density = ring * cloud;

    // Output: additive, alpha = density * user-scale. Over-bright centers
    // intentionally allowed — the bright sun behind them clips to white
    // and adds to the halo feel.
    frag_color = vec4(gas_color.rgb * density, 1.0) * (gas_color.a * density);
}
@end

@program sun_gas vs fs
