// -----------------------------------------------------------------------------
// sprite.glsl — camera-facing billboard quad with alpha/additive texturing.
//
// One shader, TWO pipelines (CPU-side): the alpha-blend pipeline draws the
// sprite HULL, the additive pipeline draws the LIGHTS overlay on top. Same
// geometry, same UVs, same sampling — the only difference is the blend
// state. Keeps shader permutations at zero.
//
// Billboarding is done by expanding a unit quad along the CAMERA'S right/up
// vectors (extracted CPU-side from the inverse view matrix and passed as
// uniforms). This keeps the sprite perpetually facing the camera regardless
// of camera orientation — classic Privateer / Homeworld look, no banking,
// no pitch.
//
// Aspect ratio: the sprite's world size is defined by `world_size` (its
// *longest* side in world units). The short axis is scaled down via
// `inst_scale.xy`, which the CPU sets from the texture's aspect ratio so
// non-square sprites don't get stretched into squares.
// -----------------------------------------------------------------------------

@vs vs

layout(binding=0) uniform vs_params {
    mat4 view_proj;
    vec4 cam_right;     // world-space camera right vector, .w unused
    vec4 cam_up;        // world-space camera up vector,    .w unused
    vec4 inst_pos;      // .xyz = sprite world position, .w = world_size (half)
    vec4 inst_scale;    // .xy  = aspect multipliers (1.0 for square sprites)
};

in vec2 a_corner;   // unit quad corners in [-1,+1]^2
in vec2 a_uv;       // 0..1, matches corner ordering

out vec2 v_uv;

void main() {
    // Half-extents along camera right/up. `inst_pos.w` is the half-length
    // of the sprite's LONGEST edge; `inst_scale.xy` shrinks the shorter
    // edge for non-square sprites. `inst_scale.zw` carries cos/sin for an
    // optional in-plane roll, used by the atlas inspector to fix individual
    // frames that came back a little crooked. Default cos=1, sin=0.
    vec2 corner = a_corner;
    float c = inst_scale.z;
    float s = inst_scale.w;
    corner = vec2(corner.x * c - corner.y * s,
                  corner.x * s + corner.y * c);
    vec3 offs = cam_right.xyz * (corner.x * inst_pos.w * inst_scale.x)
              + cam_up.xyz    * (corner.y * inst_pos.w * inst_scale.y);
    gl_Position = view_proj * vec4(inst_pos.xyz + offs, 1.0);

    // Sokol/Metal texture origin fun: PNGs loaded through stb_image arrive
    // visually upside-down in our billboard path. Flip V once here so every
    // sprite authoring tool can keep the normal "top of PNG is top of ship"
    // mental model. Do NOT flip the camera unless you enjoy debugging the
    // universe through a haunted funhouse mirror.
    v_uv = vec2(a_uv.x, 1.0 - a_uv.y);
}
@end

@fs fs

layout(binding=0) uniform texture2D u_tex;
layout(binding=0) uniform sampler   u_smp;

layout(binding=1) uniform fs_params {
    vec4 tint;          // .rgb tints the sample, .a is global alpha multiplier
};

in  vec2 v_uv;
out vec4 frag;

void main() {
    vec4 c = texture(sampler2D(u_tex, u_smp), v_uv);
    // DISCARD fully-transparent texels so the sprite's bounding quad does
    // NOT write depth at those pixels. If we let them write depth, later
    // additive effects (sun corona, bloom composite flare) get depth-tested
    // *out* inside the sprite's bounding box even though the art there is
    // transparent — you end up with a hard rectangular "hole" in the
    // atmospheric glow around every sprite. Threshold is intentionally low
    // so anti-aliased edge fringes still blend; anything truly empty dies.
    if (c.a < 0.01) discard;

    // Multiply RGB by tint.rgb (lets code flash a sprite red on damage,
    // dim a distant base, etc). Alpha is multiplied by tint.a so a single
    // uniform controls fade-in/out for both alpha-blend AND additive modes.
    frag = vec4(c.rgb * tint.rgb, c.a * tint.a);
}
@end

@program sprite vs fs
