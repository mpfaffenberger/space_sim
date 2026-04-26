# Texture / Sprite Resource Architecture

Living design note. The TL;DR: **we can't ship 10,000 ships if each ship is
80 individual `sg_image` resources.** This doc captures why, what we do
today, and the path forward.

## Today (April 2026)

* Every view-sphere cell of every ship is loaded at startup as its own
  `sg_image` + `sg_view` pair via `load_png_slot()` in `sprite.cpp`.
* `init_cb()` in `main.cpp` raises Sokol's pool sizes to `image_pool_size =
  view_pool_size = 8192`, `buffer_pool_size = 1024` to absorb growth.
* All textures stay resident on the GPU for the lifetime of the process —
  no streaming, no eviction.

This is **fine** up to roughly the 50-ship mark, after which the per-image
slot pressure and the resident-VRAM cost both start to bite.

## Why pool size alone is a dead-end

```
  per cell  : 1022 x 1020 x 4 bytes (RGBA8)  ≈ 4 MB GPU
  per ship  : 80 cells                        ≈ 320 MB GPU
  100 ships : 100 × 320 MB                    ≈ 32 GB VRAM    ← lol no
```

Even if we bumped `image_pool_size` to 65,536, no consumer GPU has the
VRAM to actually back it. The pool is just a tracking-slot allocator —
the real wall is total resident texture memory.

There's also a silent-failure footgun: when the image pool overflows,
`sg_make_image` returns `SG_INVALID_ID` and the program keeps running.
Symptoms (real bugs we hit) include sprite cells "failing to load" with
no visible error and the HUD disappearing entirely. Treat any
`sg::IMAGE_POOL_EXHAUSTED` log line as a hard signal that something needs
to scale differently — not as a "just bump it" moment.

## Tier 1: per-ship atlas pages (~when adding the 4th–5th ship)

Pack a ship's 80 cells into a single texture page instead of 80 separate
images. One 8192² atlas holds 64 cells of 1022² with room to spare; an
8192×4096 holds the rest. So **1 ship → 1 (or 2) `sg_image`s + 80 UV
rects** instead of 80 images.

Wins:

* Image-pool pressure drops 80× — the 8192-slot bump above will last
  effectively forever.
* Single-texture binds during render = fewer state changes per frame.
* Opens the door to BC7/ASTC compression on the page (Metal/MoltenVK
  both support it), which can cut VRAM 4–8× per asset.
* Mipmap chains become trivial (one chain per page, not 80).

Implementation sketch:

* New `ShipAtlasPage` resource: holds one `sg_image`, one `sg_view`, and
  a `std::vector<UVRect>` indexed by `(az_bin, el_bin)` exactly like the
  current `atlas_manifest.json`.
* `tools/build_ship_atlas_manifest.py` already knows the cell layout —
  extend it to also emit a packed PNG and the UV-rect table (or generate
  the page at engine load time from the existing per-cell PNGs the
  first time and cache it on disk).
* `sprite.cpp` keeps the same external API; internally a sprite handle
  becomes `(page_id, uv_rect)` instead of `(image_id, view_id)`.

## Tier 2: LRU residency / streaming (~1000+ assets)

Once total atlas-page count starts approaching VRAM, we evict pages that
haven't been rendered for N seconds (or aren't in the current sector +
1-jump neighbors).

Required pieces:

* A residency manager keyed by atlas-page id, tracking last-frame-used.
* Async PNG decode on a worker thread (Sokol upload from main thread is
  fine; the slow part is `stbi_load` + alpha-key + quantize).
* Spatial hint plumbing — system loader should declare which ships are
  reachable from the current system so we can pre-warm one jump ahead
  and only fully evict things 2+ jumps away.
* Per-page residency state: `UNLOADED → LOADING → RESIDENT → EVICTING`.

This is a bigger lift but the previous tier sets it up cleanly: pages are
the natural unit of streaming.

## Tier 3 (speculative): mip streaming + virtual texturing

Only worth doing if Tier 2 still isn't enough. Trade more code complexity
for finer-grained VRAM control by streaming individual mip levels rather
than whole pages. Modern engines do this; it's a lot of plumbing.

## Decision log

| Date       | Trigger                             | Action |
|------------|-------------------------------------|--------|
| 2026-04-26 | Adding 2nd ship (Talon) blew the    | Bump pools to 8192/8192/1024. Wrote this doc. |
|            | 128-image default; HUD disappeared. |        |
