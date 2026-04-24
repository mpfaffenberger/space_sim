#pragma once
// -----------------------------------------------------------------------------
// mesh.h — minimal CPU-side mesh with GPU buffer upload helpers.
//
// Interleaved (pos, normal) vertex layout, uint16 indices. That's enough
// for anything procedurally generated up to ~65k vertices per mesh, which
// is orders of magnitude more than an asteroid needs.
//
// Deliberately no UVs, no tangents, no skinning. YAGNI — we'll add fields
// when a shader actually wants them. Adding now would bloat every vertex
// of every future instanced mesh for a maybe-use.
// -----------------------------------------------------------------------------

#include "material.h"
#include "sokol_gfx.h"
#include <cstdint>
#include <vector>

// A Submesh is a contiguous range inside a Mesh's shared index buffer,
// bound to one Material. Every mesh has at least one Submesh — OBJs
// without any `usemtl` directive get a single implicit submesh spanning
// the whole buffer with an implicit (empty) material at index 0.
struct Submesh {
    uint32_t index_start  = 0;   // first element (not byte) offset
    uint32_t index_count  = 0;
    uint32_t material_idx = 0;   // index into Mesh::materials
};

struct MeshVertex {
    float pos[3];
    float normal[3];
    float uv[2];     // zero for meshes without UVs (asteroids); filled by OBJ loader
};
static_assert(sizeof(MeshVertex) == 32, "MeshVertex layout assumption broken");

struct Mesh {
    std::vector<MeshVertex> vertices;
    std::vector<uint16_t>   indices;

    // Per-mesh material table + submesh list. Populated by the OBJ loader
    // via the `.materials.json` sidecar; for meshes without one, a single
    // empty Material and single full-buffer Submesh stand in.
    std::vector<Material>   materials;
    std::vector<Submesh>    submeshes;

    sg_buffer vbuf{};
    sg_buffer ibuf{};
    int       index_count = 0;

    // Axis-aligned bounding box over the vertices. Populated alongside
    // upload() so downstream code (placement scaling, culling, collision)
    // can reason about a mesh's intrinsic dimensions regardless of the
    // wildly inconsistent units different authoring tools export in.
    float aabb_min[3] = { 0, 0, 0 };
    float aabb_max[3] = { 0, 0, 0 };

    // Longest dimension of the AABB. This is what we call the "natural
    // length" of the mesh — good enough for ships (they're elongated)
    // and still sensible for roughly-spherical things like asteroids.
    float longest_extent() const;

    // Fill aabb_min/aabb_max from the current vertex array. One-pass,
    // O(verts). Called automatically by upload() for meshes that don't
    // already have one (asteroid field, procedural icosphere, anything
    // that populates vertices[] directly); called explicitly by the OBJ
    // loader before the cache write so the npmesh file carries the bbox.
    void recompute_aabb();

    // Upload CPU arrays to GPU. Safe to clear the CPU side after. Does
    // NOT touch `materials` — they own their own GPU images and are
    // populated by the OBJ loader BEFORE upload().
    bool upload();
    void destroy();   // also tears down every Material in the table
};
