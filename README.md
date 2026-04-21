# space_sim

A from-scratch space-sim renderer in C++20. Runs at 60fps on Metal, boots in
milliseconds, no engine.

> _"The whole point of programming used to be how satisfying it felt. I'm
> trying to remember."_ — probably

## Screenshot

Purple-and-green nebula, plasma sun with fresnel limb-brightening, corona +
god rays, volumetric gas shell, 12 000 parallax-dust specks, bloom
post-process, procedural lens flare, all driven by a seed string you typed
at the command line.

## Build & Run

```bash
cmake -S . -B build
cmake --build build -j
./build/new_privateer
```

That's it. It works on any macOS with a Metal GPU.

```bash
# Try different stars (hot-swap with 1..6 at runtime too):
./build/new_privateer --star blue
./build/new_privateer --star red    --seed sector_4774
./build/new_privateer --star purple --seed troy_rich
```

## Controls

| key                 | action                                            |
|---------------------|---------------------------------------------------|
| `mouse`             | look (pointer captured FPS-style)                 |
| `right-click`       | toggle pointer capture                            |
| `W` / `S`           | throttle forward / reverse                        |
| `A` / `D`           | strafe left / right                               |
| `Space` / `L-Shift` | thrust up / down                                  |
| `Tab` (hold)        | cruise engine — 10× thrust + FOV widen            |
| `X`                 | full brake                                        |
| `1`..`6`            | swap star (yellow / blue / red / green / orange / purple) |
| `Esc` ×2            | quit (double-tap within 1s)                       |

## Generate your own skybox

```bash
# Any string is a valid seed. Presets: --rich / --sparse / --wild.
tools/gen_skybox.sh my_cool_system --rich

# Pick specific nebula colors:
tools/gen_skybox.sh purple_haze --rich --nebula-colors "0.55,0.15,0.85;0.2,0.75,0.3"

./build/new_privateer --seed my_cool_system
```

Requires [`skyboxgen`](https://github.com/mpfaffenberger/skyboxgen) built
next to this repo (or set `SKYBOXGEN=/path/to/skyboxgen`).

## What's in here

| Layer | Lines | Notes |
|---|---|---|
| Procedural skybox cubemap | `skybox.{h,cpp}` + `skybox.glsl` | loads 6 PNGs, flips faces to match D3D/Metal convention, draws with `CLAMP_TO_EDGE` so seams don't bleed |
| 6-DOF flight + cruise engine | `camera.{h,cpp}` | exponential damping, smooth cruise windup/windown, FOV widens on engage |
| Plasma sun | `sun.{cpp,glsl}` | animated fBM granulation, limb-brightening (not darkening — stars are optically thick emitters) |
| Gas shell | `sun_gas.{cpp,glsl}` | noise-ring billboard that hugs the silhouette, breaks the hard sphere boundary |
| Corona + god rays | `sun_corona.{cpp}` + `sun_glow.glsl` | procedural sunbeams, aspect-corrected anamorphic streaks |
| Stellar presets | `star_presets.{h,cpp}` | 6 "star types" differing in color, granulation contrast, flow speed, corona size |
| Parallax dust | `dust.{cpp,glsl}` | 12 k `GL_POINTS`, wrapped in the vertex shader to feel infinite |
| Bloom + lens-flare post | `postprocess*.cpp` + `post_*.glsl` | two-pass separable gaussian at quarter-res, procedural flare tracking sun NDC |
| On-screen HUD | `sokol_debugtext` | speed / mode / distance / position / controls |

Total ≈ 1 700 lines of project code. No engine, no ECS, no asset pipeline.

## Toolchain

| Layer        | Tool                                             | Why                              |
|--------------|--------------------------------------------------|----------------------------------|
| Language     | C++20                                            | matches `skyboxgen`              |
| Build        | CMake 3.20+                                      | boring, universal                |
| Window/Input | [sokol_app](https://github.com/floooh/sokol)     | Metal on Mac, GL/D3D elsewhere   |
| Renderer     | sokol_gfx                                        | modern command-buffer API        |
| Text HUD     | sokol_debugtext                                  | 6 built-in bitmap fonts          |
| Images       | [stb_image](https://github.com/nothings/stb)     | one header, zero config          |
| Math         | [HandmadeMath](https://github.com/HandmadeMath)  | vec/mat/quat, single header      |
| Shaders      | GLSL → [`sokol-shdc`](https://github.com/floooh/sokol-tools) → MSL/SPIR-V/HLSL | write once, run everywhere |

All dependencies live as single-header files in `third_party/`. No package
manager, no submodules, no `vcpkg.json`.

## Layout

```
.
├── src/                C++ sources
├── shaders/            GLSL (cross-compiled to MSL/SPIR-V/HLSL at build time)
├── assets/skybox/troy/ Default skybox (generated reference)
├── third_party/        single-header libs + sokol-shdc binary
├── tools/              shell helpers: gen_skybox.sh, ...
└── CMakeLists.txt
```

## Roadmap

- [x] Procedural skybox (seed → 6 PNGs) with color + nebula tuning
- [x] Mouselook camera, 6-DOF flight, cruise engine
- [x] Plasma sun with corona, gas shell, god rays, stellar presets
- [x] Parallax dust
- [x] Bloom + procedural lens flare
- [x] On-screen HUD
- [ ] Asteroid field (instanced, LOD billboards at distance)
- [ ] Mesh loader (glTF) + a placeholder ship
- [ ] Volumetric nebula gas clouds (raymarched 3D noise)
- [ ] Audio (sokol_audio — engine whoosh, ambient drone)
- [ ] HUD overlays, targeting, weapons
- [ ] Missions, factions, trade, the actual game

## Credits

- [Andre Weissflog](https://github.com/floooh) for sokol — the spine of this
  whole project.
- [Sean Barrett](https://github.com/nothings) for `stb_image` and forever
  reminding us what a good header looks like.
- Chris Roberts & the Digital Anvil team for Freelancer (2003), which is
  still the benchmark 22 years later.

## License

MIT. See `LICENSE` if you see one — otherwise assume MIT and go wild.
