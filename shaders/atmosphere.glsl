// -----------------------------------------------------------------------------
// atmosphere.glsl — thick scattering atmosphere shell.
//
// Not a physically-accurate radiative-transfer solve (no ray-march, no
// Rayleigh/Mie integrals). Instead, three cheap dot-product terms combine
// to give the visual cues the brain reads as "real atmosphere":
//
//   1. fresnel path-length proxy
//      1 - max(N·V, 0) raised to a low exponent → atmosphere is densest
//      at the silhouette where the view ray grazes tangent to the shell,
//      thinnest where the ray goes straight in. Stands in for the actual
//      optical path length through a spherical atmosphere.
//
//   2. day/night mask
//      smoothstep(N·L) gives a soft terminator. Without this the halo
//      would glow equally on the night side which reads as radioactive.
//
//   3. Henyey-Greenstein-ish forward scatter
//      When the sun is behind the atmosphere pixel from the camera's POV
//      (V ≈ -L) we're looking along the forward-scatter peak — this is
//      what makes sunsets glow. We bias the atmosphere colour toward a
//      warm tint wherever this term is strong and the atmosphere is
//      thick (fresnel * forward).
//
// The shell is drawn with CULL_BACK (not cull-front) so the inside of
// the hemisphere facing the camera renders, putting the atmosphere
// *over* the planet surface for subtle limb brightening rather than
// only in a thin ring outside the silhouette.
// -----------------------------------------------------------------------------

@vs vs
layout(binding=0) uniform atm_vs_params {
    mat4 view_proj;
    mat4 model;
};

in vec3 a_pos;
in vec3 a_normal;
in vec2 a_uv;           // unused but must match mesh vertex layout

out vec3 v_world_pos;
out vec3 v_world_normal;

void main() {
    vec4 world = model * vec4(a_pos, 1.0);
    v_world_pos    = world.xyz;
    v_world_normal = normalize((model * vec4(a_normal, 0.0)).xyz);
    gl_Position    = view_proj * world;
}
@end

@fs fs
layout(binding=1) uniform atm_fs_params {
    vec4 sun_pos;        // .xyz = world position of sun
    vec4 camera_pos;     // .xyz = camera world position
    vec4 atm_color;      // .rgb = base sky colour, .a = overall strength
};

in  vec3 v_world_pos;
in  vec3 v_world_normal;
out vec4 frag_color;

void main() {
    vec3 N = normalize(v_world_normal);
    vec3 V = normalize(camera_pos.xyz - v_world_pos);
    vec3 L = normalize(sun_pos.xyz    - v_world_pos);

    // --- 1. Atmospheric density (path-length proxy) -------------------
    // Two contributions:
    //   baseline — always-present haze even when looking straight down
    //              at the surface. Without this, nadir views see clean
    //              albedo with zero atmosphere, which reads as "planet
    //              with no air" rather than "thick soupy world".
    //   fresnel  — extra density at grazing angles where the view ray
    //              traverses more atmospheric column.
    //
    // HAZE_FLOOR = 0.55 is "pea-soup thick" — the foggy scifi look.
    // Drop to ~0.2 for Earth-like, or up to ~0.8 for Venus-cloud.
    const float HAZE_FLOOR = 0.55;
    float NdotV   = max(dot(N, V), 0.0);
    float fresnel = pow(1.0 - NdotV, 2.0);
    float density = mix(HAZE_FLOOR, 1.0, fresnel);

    // --- 2. Day/night mask --------------------------------------------
    // smoothstep over a ~25° window gives a soft terminator. Offsets
    // let the atmosphere "leak" a few degrees into the shadow side
    // (twilight), which reads as atmospheric scattering rather than
    // a hard light/dark line.
    float NdotL    = dot(N, L);
    float daylight = smoothstep(-0.15, 0.35, NdotL);

    // --- 3. Forward-scatter (Mie-ish peak) ----------------------------
    // mu = 1 when sun is directly behind the atmosphere pixel relative
    // to the camera. Narrow peak (exponent 6) concentrates the sunset
    // glow; fresnel gates it to thick-atmosphere regions only.
    float mu      = -dot(L, V);
    float forward = pow(clamp(mu * 0.5 + 0.5, 0.0, 1.0), 6.0);

    // --- Colour: cool base + warm forward-scatter ---------------------
    // Physical motivation: Rayleigh scatters blue out of short paths
    // (thin-atmosphere view looks blue), but along thick forward
    // scatter paths the blue has already been kicked sideways and
    // you see what's left — reds and oranges. The warm tint is gated
    // by fresnel (not density) so nadir view stays cool-blue and only
    // silhouette/limb views warm up.
    vec3 sky_color    = atm_color.rgb;
    vec3 scatter_warm = vec3(1.00, 0.62, 0.35);
    vec3 col = mix(sky_color, scatter_warm, forward * fresnel);

    // --- Compose (premultiplied alpha) --------------------------------
    // coverage  = how much this pixel replaces what's behind (alpha).
    // emission  = colour that will be added on top of the attenuated
    //             destination; premultiplied by coverage so the blend
    //             equation `SRC + DST*(1-SRC.a)` composes correctly.
    //
    // The forward-scatter peak adds brightness but *not* more
    // coverage — it makes the rim glow *through* the fog, like a sun
    // punching through clouds, rather than just opaquing the rim.
    float coverage = clamp(density * daylight * atm_color.a, 0.0, 1.0);
    float sun_rim  = forward * fresnel * daylight * 2.5;

    vec3 emission  = col * (coverage + sun_rim);
    frag_color = vec4(emission, coverage);
}
@end

@program atmosphere vs fs
