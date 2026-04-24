// -----------------------------------------------------------------------------
// mesh_cache.cpp — binary read/write for the .npmesh format.
//
// File layout (little-endian, packed):
//   char[8]  magic = "NPMESH01"      (bump version to invalidate existing caches)
//   u32      vertex_count
//   u32      index_count
//   u32      submesh_count
//   u32      material_count
//   f32[3]   aabb_min
//   f32[3]   aabb_max
//   MeshVertex[vertex_count]         (32 bytes each — pos, normal, uv)
//   u16       [index_count]
//   Submesh   [submesh_count]        (12 bytes — start, count, material_idx)
//   foreach material:
//     u16    name_length
//     char[name_length]  name
//     u8     alpha_blend              (1 or 0)
//
// Texture slot contents are intentionally NOT serialized — the sidecar
// materials.json remains the source of truth for texture filenames, and
// gets re-applied every time a mesh is loaded (cache hit or miss).
// -----------------------------------------------------------------------------

#include "mesh_cache.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

constexpr char     kMagic[8]  = { 'N','P','M','E','S','H','0','1' };
constexpr uint32_t kMagicSize = sizeof(kMagic);

std::string cache_path_for(const std::string& obj_path) {
    namespace fs = std::filesystem;
    fs::path p(obj_path);
    return (p.parent_path() / (p.stem().string() + ".npmesh")).string();
}

// Write `n` bytes from `src`. Returns false on short write.
bool write_blob(std::ofstream& f, const void* src, size_t n) {
    f.write(reinterpret_cast<const char*>(src), (std::streamsize)n);
    return f.good();
}

bool read_blob(std::ifstream& f, void* dst, size_t n) {
    f.read(reinterpret_cast<char*>(dst), (std::streamsize)n);
    return f.good() && (size_t)f.gcount() == n;
}

// Is file a at least as new as file b? Returns true if `a` is missing;
// treats a missing `b` as "no constraint" (only `a` matters).
bool cache_is_fresh(const std::string& cache, const std::string& src) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto c_time = fs::last_write_time(cache, ec);
    if (ec) return false;                 // cache missing
    auto s_time = fs::last_write_time(src, ec);
    if (ec) return true;                  // source missing — cache wins
    return c_time >= s_time;
}

} // namespace

bool try_load_mesh_cache(const std::string& obj_path, Mesh& out) {
    const std::string cpath = cache_path_for(obj_path);

    namespace fs = std::filesystem;
    if (!fs::exists(cpath)) return false;

    // mtime must beat both the OBJ and its sidecar (if any).
    if (!cache_is_fresh(cpath, obj_path)) return false;
    fs::path op(obj_path);
    std::string sidecar = (op.parent_path() /
                          (op.stem().string() + ".materials.json")).string();
    if (fs::exists(sidecar) && !cache_is_fresh(cpath, sidecar)) return false;

    std::ifstream f(cpath, std::ios::binary);
    if (!f) return false;

    char magic[8]{};
    if (!read_blob(f, magic, sizeof(magic))) return false;
    for (size_t i = 0; i < sizeof(kMagic); ++i)
        if (magic[i] != kMagic[i]) return false;

    uint32_t vc = 0, ic = 0, smc = 0, mc = 0;
    if (!read_blob(f, &vc,  sizeof(vc)))  return false;
    if (!read_blob(f, &ic,  sizeof(ic)))  return false;
    if (!read_blob(f, &smc, sizeof(smc))) return false;
    if (!read_blob(f, &mc,  sizeof(mc)))  return false;
    if (!read_blob(f, out.aabb_min, sizeof(out.aabb_min))) return false;
    if (!read_blob(f, out.aabb_max, sizeof(out.aabb_max))) return false;

    // Sanity caps — keep a corrupted/malicious file from allocating
    // gigabytes. 10M vertices is ~30× our largest ship, well above any
    // plausible asset.
    if (vc > 10'000'000 || ic > 30'000'000 ||
        smc > 4096      || mc  > 4096) return false;

    out.vertices.assign(vc, {});
    if (!read_blob(f, out.vertices.data(), vc * sizeof(MeshVertex))) return false;

    out.indices.assign(ic, 0);
    if (!read_blob(f, out.indices.data(), ic * sizeof(uint16_t))) return false;

    out.submeshes.assign(smc, {});
    if (!read_blob(f, out.submeshes.data(), smc * sizeof(Submesh))) return false;

    out.materials.clear();
    out.materials.reserve(mc);
    for (uint32_t i = 0; i < mc; ++i) {
        uint16_t nlen = 0;
        if (!read_blob(f, &nlen, sizeof(nlen))) return false;
        if (nlen > 256) return false;     // material names are short

        Material m;
        m.name.resize(nlen);
        if (nlen > 0 && !read_blob(f, m.name.data(), nlen)) return false;

        uint8_t ab = 0;
        if (!read_blob(f, &ab, sizeof(ab))) return false;
        m.alpha_blend = (ab != 0);

        out.materials.push_back(std::move(m));
    }
    return true;
}

bool write_mesh_cache(const std::string& obj_path, const Mesh& mesh) {
    const std::string cpath = cache_path_for(obj_path);
    std::ofstream f(cpath, std::ios::binary | std::ios::trunc);
    if (!f) return false;

    const uint32_t vc  = (uint32_t)mesh.vertices.size();
    const uint32_t ic  = (uint32_t)mesh.indices.size();
    const uint32_t smc = (uint32_t)mesh.submeshes.size();
    const uint32_t mc  = (uint32_t)mesh.materials.size();

    if (!write_blob(f, kMagic,         sizeof(kMagic)))        return false;
    if (!write_blob(f, &vc,            sizeof(vc)))            return false;
    if (!write_blob(f, &ic,            sizeof(ic)))            return false;
    if (!write_blob(f, &smc,           sizeof(smc)))           return false;
    if (!write_blob(f, &mc,            sizeof(mc)))            return false;
    if (!write_blob(f, mesh.aabb_min,  sizeof(mesh.aabb_min))) return false;
    if (!write_blob(f, mesh.aabb_max,  sizeof(mesh.aabb_max))) return false;

    if (vc  && !write_blob(f, mesh.vertices.data(),  vc  * sizeof(MeshVertex))) return false;
    if (ic  && !write_blob(f, mesh.indices.data(),   ic  * sizeof(uint16_t)))   return false;
    if (smc && !write_blob(f, mesh.submeshes.data(), smc * sizeof(Submesh)))    return false;

    for (const auto& m : mesh.materials) {
        const uint16_t nlen = (uint16_t)m.name.size();
        const uint8_t  ab   = m.alpha_blend ? 1u : 0u;
        if (!write_blob(f, &nlen, sizeof(nlen)))          return false;
        if (nlen && !write_blob(f, m.name.data(), nlen))  return false;
        if (!write_blob(f, &ab,   sizeof(ab)))            return false;
    }
    return f.good();
}
