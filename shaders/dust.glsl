// -----------------------------------------------------------------------------
// dust.glsl — near-field parallax specks.
//
// Dust lives in a cube of half-extent `wrap_extent` around a reference point
// (the camera). On the CPU we keep a static buffer of random positions in a
// centered box of size [-1, +1] and offset them by the camera's floor position
// in the vertex shader, wrapping each axis back into range. That makes the
// field feel infinite without re-uploading vertex data.
//
// No lighting, no texture — just soft dots. Purely a "motion is happening"
// cue. The point size shrinks with distance so faraway dust doesn't dominate.
// -----------------------------------------------------------------------------

@vs vs
layout(binding=0) uniform vs_params {
    mat4 view_proj;
    vec4 cam_pos;       // .xyz = camera world pos, .w unused
    vec4 field_params;  // .x = wrap_extent, .y = point_size_px, .zw unused
};

in vec3 a_pos;   // random position in [-1, +1]^3

out float v_alpha;

void main() {
    float E = field_params.x;

    // Translate by the camera position, then wrap so the speck always lands
    // in [cam - E, cam + E]. This makes the cloud *follow* the camera, giving
    // the illusion of an infinite parallax field.
    vec3 p = a_pos * E + cam_pos.xyz;
    vec3 rel = p - cam_pos.xyz;
    rel = mod(rel + E, 2.0 * E) - E;
    p   = cam_pos.xyz + rel;

    // Fade dust near the cubic boundary to avoid a hard pop when it wraps.
    float edge = max(max(abs(rel.x), abs(rel.y)), abs(rel.z)) / E;
    v_alpha = clamp(1.0 - smoothstep(0.75, 1.0, edge), 0.0, 1.0);

    gl_Position  = view_proj * vec4(p, 1.0);
    gl_PointSize = field_params.y;
}
@end

@fs fs
in  float v_alpha;
out vec4  frag_color;

void main() {
    // Soft round dot via distance to point-center in [-0.5, 0.5].
    vec2  d = gl_PointCoord - vec2(0.5);
    float r = length(d);
    if (r > 0.5) discard;
    float a = smoothstep(0.5, 0.1, r) * v_alpha;
    // Slightly warm white — reads as 'icy dust' against the nebula.
    frag_color = vec4(vec3(1.0, 1.0, 1.0), a);
}
@end

@program dust vs fs
