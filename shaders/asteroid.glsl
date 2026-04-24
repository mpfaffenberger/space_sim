// -----------------------------------------------------------------------------
// asteroid.glsl — instanced lumpy-rock renderer.
//
// Geometry: subdivided icosahedron, noise-displaced on the CPU once per
// mesh variant. Instances supply position/scale/rotation/tint; rotation
// is applied per-frame in the vertex shader via Rodrigues' formula so the
// instance buffer is static.
//
// Shading:
//   1. FLAT per-pixel face normal from screen-space derivatives of the
//      world position — turns each triangle into a visible facet without
//      needing extra geometry. The single biggest "this looks like a
//      rock" upgrade available.
//   2. Procedural DIRT (this file's recent addition) — 3D noise sampled
//      at the rock's local position gives each asteroid its own mineral
//      grain, color zones, and crevice darkening. Per-instance seed
//      offset prevents identical patterns on repeated mesh variants.
//   3. Lambert diffuse + cool ambient + nebula-tinted rim light.
// -----------------------------------------------------------------------------

@vs vs
layout(binding=0) uniform asteroid_vs_params {
    mat4 view_proj;
    vec4 sun_pos_time;
};

// Per-vertex
in vec3 a_pos;
in vec3 a_normal;

// Per-instance
in vec4 i_pos_scale;
in vec4 i_axis_phase;
in vec4 i_speed_tint;

out vec3  v_world_pos;
out vec3  v_smooth_normal;
out vec3  v_local_pos;      // un-rotated, un-scaled — texture coord for procedural dirt
out vec3  v_tint;
out float v_tex_seed;       // per-instance scalar to offset noise sampling

vec3 rotate_axis_angle(vec3 v, vec3 axis, float angle) {
    float c = cos(angle);
    float s = sin(angle);
    return v * c + cross(axis, v) * s + axis * dot(axis, v) * (1.0 - c);
}

void main() {
    float angle = i_axis_phase.w + i_speed_tint.x * sun_pos_time.w;
    vec3  axis  = i_axis_phase.xyz;

    vec3 p_rot = rotate_axis_angle(a_pos,    axis, angle);
    vec3 n_rot = rotate_axis_angle(a_normal, axis, angle);
    vec3 world = p_rot * i_pos_scale.w + i_pos_scale.xyz;

    v_world_pos     = world;
    v_smooth_normal = n_rot;
    v_local_pos     = a_pos;       // pre-rotation, pre-scale — stable per asteroid
    v_tint          = i_speed_tint.yzw;
    v_tex_seed      = i_axis_phase.w;   // each instance has unique phase in [0, 2π]

    gl_Position = view_proj * vec4(world, 1.0);
}
@end

@fs fs
layout(binding=1) uniform asteroid_fs_params {
    vec4 sun_color_amb;
    vec4 sun_world_pos;
    vec4 camera_pos;
    vec4 rim_tint;
};

in  vec3  v_world_pos;
in  vec3  v_smooth_normal;
in  vec3  v_local_pos;
in  vec3  v_tint;
in  float v_tex_seed;
out vec4  frag_color;

// ---- 3D value noise (same family as sun.glsl; kept local for TU hygiene) ----
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
float fbm3(vec3 p) {
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 3; ++i) {
        v += vnoise(p) * a;
        p *= 2.13;
        a *= 0.5;
    }
    return v;
}

void main() {
    // ---- flat face normal via screen-space derivatives ------------------
    vec3 dpdx = dFdx(v_world_pos);
    vec3 dpdy = dFdy(v_world_pos);
    vec3 N    = normalize(cross(dpdx, dpdy));
    if (dot(N, v_smooth_normal) < 0.0) N = -N;

    vec3 L = normalize(sun_world_pos.xyz - v_world_pos);
    vec3 V = normalize(camera_pos.xyz - v_world_pos);

    // ---- procedural dirt, sampled in local (per-asteroid) space ---------
    // v_local_pos lives on the unit-sphere-ish original mesh, so a freq
    // of ~3 gives 3 noise cycles across the rock — a handful of large
    // patches, which reads as "different minerals." The tex_seed offset
    // decorrelates same-mesh instances.
    vec3 seed_off = vec3(v_tex_seed * 12.3, v_tex_seed * 7.1, v_tex_seed * 17.5);
    vec3 sp       = v_local_pos * 3.2 + seed_off;

    float grain    = fbm3(sp * 2.0);                          // medium-freq grain
    float patches  = fbm3(sp * 0.55 + vec3(11.7, 3.1, 5.9));  // big color zones
    float crevice  = 1.0 - abs(vnoise(sp * 2.5) * 2.0 - 1.0); // ridged → thin dark lines
    crevice        = pow(crevice, 3.0);                       // tighten

    // Color zones: blend between a dark basalt-ish color and a lighter
    // tan. Both derive from v_tint so the per-instance palette still
    // dominates, but patches give within-rock variation.
    vec3 albedo_dark  = v_tint * vec3(0.55, 0.52, 0.48);
    vec3 albedo_light = v_tint * vec3(1.18, 1.10, 0.95);
    vec3 albedo = mix(albedo_dark, albedo_light,
                      smoothstep(0.35, 0.70, patches));

    // Grain modulates brightness — rocks look "dusty" rather than painted.
    albedo *= mix(0.78, 1.15, grain);

    // Crevices darken (cheap AO). Only bite hard in the ridged lines.
    albedo *= mix(1.0, 0.35, crevice);

    // ---- lighting ------------------------------------------------------
    float ndl  = max(dot(N, L), 0.0);
    vec3  diff = albedo * sun_color_amb.rgb * ndl;

    vec3 amb = albedo * vec3(0.09, 0.10, 0.14) * sun_color_amb.a;

    float rim_fac = pow(1.0 - max(dot(N, V), 0.0), 2.5);
    vec3  rim     = rim_tint.rgb * rim_fac * rim_tint.a;

    frag_color = vec4(diff + amb + rim, 1.0);
}
@end

@program asteroid vs fs
