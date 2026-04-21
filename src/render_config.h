#pragma once
// -----------------------------------------------------------------------------
// render_config.h — tiny shared header of pixel-format + MSAA constants.
//
// Every scene-drawing pipeline (skybox, sun, dust, ...) needs to declare
// the attachment format it will write into. Concentrating those constants
// here keeps things DRY and makes "change HDR format" a one-line edit.
// -----------------------------------------------------------------------------

#include "sokol_gfx.h"

// Offscreen scene color attachment. RGBA8 is LDR but matches stb_image
// texture decoding + keeps bandwidth modest. Bump to RGBA16F later if we
// want real HDR (requires shader output tone-mapping too).
constexpr sg_pixel_format kSceneColorFormat = SG_PIXELFORMAT_RGBA8;
constexpr sg_pixel_format kSceneDepthFormat = SG_PIXELFORMAT_DEPTH;

// Half-res target for the bloom ping-pong. Same format to keep things
// compatible with a single blur pipeline.
constexpr sg_pixel_format kBloomColorFormat = SG_PIXELFORMAT_RGBA8;

// Final composite targets the swapchain. On Metal/macOS that's BGRA8;
// sokol's swapchain auto-detection gives us whatever the platform wants.
// We pin it here so pipeline descs can declare it.
constexpr sg_pixel_format kSwapchainColorFormat = SG_PIXELFORMAT_BGRA8;

// MSAA-off everywhere keeps the offscreen path simple (no resolve step).
// Bloom + scene shading tend to hide aliasing anyway. Revisit if shimmer
// bothers us.
constexpr int kSceneSampleCount = 1;
