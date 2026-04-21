//------------------------------------------------------------------------------
// skybox.glsl — renders a cubemap as an infinitely-distant background.
//
// Standard skybox trick:
//   - draw a unit cube positioned at the camera
//   - strip the translation from the view matrix (only rotation matters —
//     the sky never gets closer when you fly)
//   - force depth to 1.0 (far plane) via the z=w trick so anything else
//     in the scene draws on top without a depth-test fight.
//
// sokol-shdc annotations (@vs / @fs / @program) tell the cross-compiler
// which entry points to emit. The resulting header lands in
// src/generated/skybox.glsl.h.
//------------------------------------------------------------------------------

@vs vs
layout(binding=0) uniform vs_params {
    mat4 view_proj;   // projection * rotation-only view
};

in vec3 a_pos;
out vec3 v_dir;

void main() {
    v_dir = a_pos;
    vec4 clip = view_proj * vec4(a_pos, 1.0);
    // z = w so perspective-divide yields depth=1 (on the far plane).
    gl_Position = clip.xyww;
}
@end

@fs fs
layout(binding=0) uniform textureCube u_sky_tex;
layout(binding=0) uniform sampler     u_sky_smp;

in  vec3 v_dir;
out vec4 frag_color;

void main() {
    frag_color = texture(samplerCube(u_sky_tex, u_sky_smp), normalize(v_dir));
}
@end

@program skybox vs fs
