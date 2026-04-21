// -----------------------------------------------------------------------------
// sun.glsl — plasma sphere with animated granulation + limb brightening.
//
// The flat-shaded sphere we started with reads as "billiard ball, painted
// white." This shader gives the sun three properties real plasma has:
//
//   1. GRANULATION — convection cells across the surface, via fBM noise
//      sampled at the world-space normal and scrolled slowly through
//      time. Two scales layered for hot-spot detail over broad cells.
//
//   2. LIMB BRIGHTENING — in a diffuse emitter like a star, rays that
//      graze the surface traverse more emitting plasma than rays heading
//      straight at the core, so the silhouette is actually BRIGHTER than
//      the centre. (The opposite of a planet's atmosphere.) We fake this
//      with `pow(1 - facing, p)` pushed into the warm glow color.
//
//   3. GENTLE TIME EVOLUTION — the noise fields scroll slowly so the sun
//      looks like it's breathing/boiling rather than static.
//
// No textures, no bloom post-process — just the sphere shader doing work.
// -----------------------------------------------------------------------------

@vs vs
layout(binding=0) uniform vs_params {
    mat4 view_proj;
    vec4 world_pos;   // .xyz = sun center in world space
    vec4 radius;      // .x   = radius
};

in vec3 a_pos;        // unit-sphere position (doubles as normal)

out vec3 v_normal;
out vec3 v_world_pos;

void main() {
    vec3 world = world_pos.xyz + a_pos * radius.x;
    v_world_pos = world;
    v_normal    = a_pos;
    gl_Position = view_proj * vec4(world, 1.0);
}
@end

@fs fs
layout(binding=1) uniform fs_params {
    vec4 core_color;               // .rgb = bright plasma base color
    vec4 glow_color;               // .rgb = warm edge / corona color
    vec4 view_and_tightness;       // .xyz = camera world pos, .w = limb exponent
    vec4 plasma_params;            // .x = time (s), .y = granule scale, .z = flow speed, .w = contrast
};

in  vec3 v_normal;
in  vec3 v_world_pos;
out vec4 frag_color;

// -- value noise, 3D ---------------------------------------------------------
// Cheap hash-based noise. Not perceptually perfect but fast, tileable, and
// good enough to sell "boiling plasma." Replace with Perlin/Simplex later
// if we ever want something more directionally-neutral.
float hash(vec3 p) {
    p = fract(p * 0.3183099 + 0.1);
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float vnoise(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);   // smoothstep hermite
    return mix(
        mix(mix(hash(i + vec3(0,0,0)), hash(i + vec3(1,0,0)), f.x),
            mix(hash(i + vec3(0,1,0)), hash(i + vec3(1,1,0)), f.x), f.y),
        mix(mix(hash(i + vec3(0,0,1)), hash(i + vec3(1,0,1)), f.x),
            mix(hash(i + vec3(0,1,1)), hash(i + vec3(1,1,1)), f.x), f.y),
        f.z);
}
// 3-octave fBM — was 4, dropping one shaves ~25% of per-pixel cost and the
// loss is imperceptible once the sun is in motion (the time-animated noise
// already hides the reduced spectral detail).
float fbm(vec3 p) {
    float v = 0.0, amp = 0.5;
    for (int i = 0; i < 3; ++i) {
        v += vnoise(p) * amp;
        p *= 2.13;
        amp *= 0.5;
    }
    return v;
}

void main() {
    vec3 n        = normalize(v_normal);
    vec3 view_dir = normalize(view_and_tightness.xyz - v_world_pos);
    float facing  = clamp(dot(n, view_dir), 0.0, 1.0);

    float time        = plasma_params.x;
    float granule_sc  = plasma_params.y;
    float flow        = plasma_params.z;
    float contrast    = plasma_params.w;

    // Two noise layers: broad granulation cells scroll slowly; finer
    // hot-spot detail scrolls faster and in a different direction so the
    // combined motion doesn't look like a single conveyor belt.
    vec3 pA = n * granule_sc       + vec3(time * flow, 0.0, time * flow * 0.7);
    vec3 pB = n * granule_sc * 2.7 + vec3(0.0, time * flow * 1.6, -time * flow * 0.9);
    float nA = fbm(pA);
    float nB = fbm(pB);
    float temp = nA * 0.65 + nB * 0.35;
    // Contrast push. Kept mild on purpose — plasma cells vary by a few
    // percent in brightness, not by the "moon-crater" contrast you get
    // when you smoothstep too aggressively.
    temp = smoothstep(0.4 - 0.15 * contrast, 0.6 + 0.15 * contrast, temp);

    // Granulation as a *brightness modulation* rather than a color mix —
    // this is what keeps the surface reading as "hot plasma" instead of
    // "rocky terrain." Range ~[0.88, 1.22] is subtle but clearly alive.
    float brightness = 0.88 + 0.34 * temp;
    vec3  surface    = core_color.rgb * brightness;

    // Limb brightening — more plasma depth along grazing rays. The warm
    // glow_color here does double duty: it's also the color used by the
    // corona billboard, so the edge of the sphere bleeds smoothly into
    // the halo around it.
    float limb = pow(1.0 - facing, view_and_tightness.w);
    surface += glow_color.rgb * limb * 1.8;

    // Tiny brightness pulse tied to large-scale noise — sells "it's alive."
    surface *= 0.9 + 0.2 * nA;

    frag_color = vec4(surface, 1.0);
}
@end

@program sun vs fs
