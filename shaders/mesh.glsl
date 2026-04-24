// -----------------------------------------------------------------------------
// mesh.glsl — generic lit-mesh shader for loaded geometry (ships, stations).
//
// Non-instanced, per-draw model matrix + tint. Lighting mirrors the asteroid
// approach (Lambert diffuse from the sun, nebula-tinted rim light, cool
// ambient) but stays smooth-shaded — ship hulls WANT smooth normals from
// the artist, we shouldn't facet them like rocks.
// -----------------------------------------------------------------------------

@vs vs
layout(binding=0) uniform mesh_vs_params {
    mat4 view_proj;
    mat4 model;        // world transform for this draw
};

in vec3 a_pos;
in vec3 a_normal;
in vec2 a_uv;

out vec3 v_world_pos;
out vec3 v_world_normal;
out vec2 v_uv;

void main() {
    vec4 world = model * vec4(a_pos, 1.0);
    v_world_pos = world.xyz;
    // Assuming uniform scale — no inverse-transpose needed. If we ever
    // want non-uniform scaling we'll pass the 3×3 normal matrix here.
    v_world_normal = normalize((model * vec4(a_normal, 0.0)).xyz);
    v_uv = a_uv;
    gl_Position = view_proj * world;
}
@end

@fs fs
// Four texture slots. All sampled with the same bilinear-repeat sampler.
// Absent textures are backed by tiny 1×1 fallbacks (white / gray / black /
// flat-normal-blue) so the shader path stays uniform regardless of which
// maps a given mesh provides.
layout(binding=0) uniform texture2D u_albedo;   // diffuse
layout(binding=1) uniform texture2D u_spec;     // specular intensity (.r)
layout(binding=2) uniform texture2D u_glow;     // emissive RGB
layout(binding=3) uniform texture2D u_normal;   // tangent-space normal
layout(binding=0) uniform sampler   u_smp;

// Build a tangent-space → world-space frame from screen-space derivatives
// of world position and UV. Lets us do proper tangent-space normal
// mapping WITHOUT storing tangents in the vertex buffer. Credit: Christian
// Schüler, "Followup: Normal Mapping Without Precomputed Tangents"
// (http://www.thetenthplanet.de/archives/1180). Accuracy is worse than
// precomputed tangents at extreme UV stretching, but WCU meshes are
// tame and the visual cost is invisible in practice — we trade a tiny
// bit of quality for a simpler mesh pipeline.
mat3 cotangent_frame(vec3 N, vec3 world_p, vec2 uv) {
    vec3 dp1  = dFdx(world_p);
    vec3 dp2  = dFdy(world_p);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    // scale-invariant — avoids artifacts at different UV densities
    float invmax = inversesqrt(max(dot(T, T), dot(B, B)));
    return mat3(T * invmax, B * invmax, N);
}

layout(binding=1) uniform mesh_fs_params {
    vec4 sun_pos;
    vec4 sun_color;     // .rgb, .a = ambient strength
    vec4 camera_pos;
    vec4 rim_tint;      // .rgb + .a strength
    vec4 body_tint;     // .rgb multiplier, .a specular strength
};

in  vec3 v_world_pos;
in  vec3 v_world_normal;
in  vec2 v_uv;
out vec4 frag_color;

void main() {
    // Wavefront stores V with 0 at the bottom, GPUs sample with 0 at top.
    vec2 uv = vec2(v_uv.x, 1.0 - v_uv.y);

    vec3  albedo   = texture(sampler2D(u_albedo, u_smp), uv).rgb * body_tint.rgb;
    float spec_map = texture(sampler2D(u_spec,   u_smp), uv).r;   // 0 = matte
    vec3  glow_map = texture(sampler2D(u_glow,   u_smp), uv).rgb; // emissive

    // Fetch tangent-space normal, remap [0,1] → [-1,1]. A flat normal
    // (0,0,1) in tangent space comes from the RGB fallback (128,128,255).
    vec3 n_tan = texture(sampler2D(u_normal, u_smp), uv).rgb * 2.0 - 1.0;

    // Start with the interpolated vertex normal, then rotate the sampled
    // tangent-space normal into world space via the on-the-fly TBN.
    vec3 N_geo = normalize(v_world_normal);
    mat3 TBN   = cotangent_frame(N_geo, v_world_pos, uv);
    vec3 N     = normalize(TBN * n_tan);
    vec3 L = normalize(sun_pos.xyz    - v_world_pos);
    vec3 V = normalize(camera_pos.xyz - v_world_pos);
    vec3 H = normalize(L + V);

    // Single unified light-strength scalar in [AMBIENT_FLOOR .. 1.0]. The
    // shadowed side floors at `AMBIENT_FLOOR × albedo`; the lit side tops
    // out at `1.0 × albedo × sun_color`. Texture dominates, never clips.
    // AMBIENT_FLOOR is per-draw (sun_color.a) so ships can keep a bright,
    // readable shadow side (~0.45) while planets go properly dark (~0.15)
    // and let their atmospheric halo do the silhouette work.
    float AMBIENT_FLOOR = sun_color.a;
    float ndl   = dot(N, L);
    float wrap  = clamp(ndl * 0.5 + 0.5, 0.0, 1.0);
    float light = mix(AMBIENT_FLOOR, 1.0, wrap);
    vec3  shade = albedo * sun_color.rgb * light;

    // Blinn-Phong highlight, gated by the spec map. Ships with no spec
    // map get a 0.5 fallback → moderate shine everywhere; ships with a
    // real map get proper "painted hull is matte, exposed metal is shiny"
    // behaviour. `body_tint.a` is the per-ship global spec multiplier.
    float ndh  = max(dot(N, H), 0.0);
    float spec = pow(ndh, 64.0) * spec_map * body_tint.a
                 * max(ndl, 0.0) * 0.9;
    vec3  specular = sun_color.rgb * spec;

    // Subtle rim — silhouette lift, not a wash.
    float rim_fac = pow(1.0 - max(dot(N, V), 0.0), 4.0);
    vec3  rim     = rim_tint.rgb * rim_fac * rim_tint.a * 0.25;

    // Emissive is added on top of everything, unaffected by lighting.
    // That's what makes engine exhausts and running lights glow in deep
    // shadow — they're self-illuminated. Unit multiplier; the texture
    // itself controls brightness.
    vec3 emissive = glow_map;

    frag_color = vec4(shade + specular + rim + emissive, 1.0);
}
@end

@program mesh vs fs
