#include "mesh.h"

#include <algorithm>
#include <cstdio>

float Mesh::longest_extent() const {
    return std::max({ aabb_max[0] - aabb_min[0],
                      aabb_max[1] - aabb_min[1],
                      aabb_max[2] - aabb_min[2] });
}

void Mesh::recompute_aabb() {
    aabb_min[0] = aabb_min[1] = aabb_min[2] =  1e30f;
    aabb_max[0] = aabb_max[1] = aabb_max[2] = -1e30f;
    for (const auto& v : vertices) {
        for (int i = 0; i < 3; ++i) {
            if (v.pos[i] < aabb_min[i]) aabb_min[i] = v.pos[i];
            if (v.pos[i] > aabb_max[i]) aabb_max[i] = v.pos[i];
        }
    }
}

bool Mesh::upload() {
    if (vertices.empty() || indices.empty()) {
        std::fprintf(stderr, "[mesh] upload called on empty mesh\n");
        return false;
    }

    // If the caller hasn't populated the bbox, do it now. The cache-hit
    // and text-parse paths both set it before upload(); this is mainly
    // for procedural callers (asteroids) that build vertex arrays inline.
    if (aabb_max[0] < aabb_min[0]) recompute_aabb();

    sg_buffer_desc vbd{};
    vbd.usage.vertex_buffer = true;
    vbd.data.ptr  = vertices.data();
    vbd.data.size = vertices.size() * sizeof(MeshVertex);
    vbuf = sg_make_buffer(&vbd);

    sg_buffer_desc ibd{};
    ibd.usage.index_buffer = true;
    ibd.data.ptr  = indices.data();
    ibd.data.size = indices.size() * sizeof(uint16_t);
    ibuf = sg_make_buffer(&ibd);

    index_count = (int)indices.size();

    if (sg_query_buffer_state(vbuf) != SG_RESOURCESTATE_VALID ||
        sg_query_buffer_state(ibuf) != SG_RESOURCESTATE_VALID) {
        std::fprintf(stderr, "[mesh] GPU buffer creation failed\n");
        return false;
    }
    return true;
}

void Mesh::destroy() {
    for (auto& m : materials) m.destroy();
    materials.clear();
    submeshes.clear();
    sg_destroy_buffer(ibuf);
    sg_destroy_buffer(vbuf);
}
