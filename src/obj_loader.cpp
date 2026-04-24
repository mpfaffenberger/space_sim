// -----------------------------------------------------------------------------
// obj_loader.cpp — straightforward line-by-line OBJ parser.
//
// Also loads the companion `<stem>.materials.json` sidecar (written by
// tools/import_wcu_meshes.sh) to populate per-submesh texture slots.
// The OBJ itself only carries `usemtl` names; the sidecar maps those
// names to actual PNG files.
// -----------------------------------------------------------------------------

#include "obj_loader.h"
#include "json.h"
#include "mesh_cache.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace {

struct Vec3 { float x, y, z; };
struct Vec2 { float u, v; };

// OBJ face indices reference three separate pools (v, vt, vn). When
// outputting to our packed MeshVertex[], two face corners that reference
// the same (v, vt, vn) triple should collapse into one vertex — otherwise
// the vertex buffer balloons 3× or more. Standard trick: hash the triple
// to an output index.
struct Triple {
    int v, t, n;
    bool operator==(const Triple& o) const { return v == o.v && t == o.t && n == o.n; }
};
struct TripleHash {
    size_t operator()(const Triple& k) const noexcept {
        // Three ints into one 64-bit; plenty of room for any reasonable OBJ.
        return ((size_t)(k.v + 1) * 73856093u) ^
               ((size_t)(k.t + 1) * 19349663u) ^
               ((size_t)(k.n + 1) * 83492791u);
    }
};

// Resolve OBJ's 1-based index + optional negative (relative) indexing.
int resolve_index(int raw, int pool_size) {
    if (raw > 0)  return raw - 1;                // 1-based → 0-based
    if (raw < 0)  return pool_size + raw;        // negative counts back from end
    return -1;                                   // 0 means "absent" in our usage
}

// Parse one face corner ("a", "a/b", "a//c", "a/b/c") starting at `tok`,
// writing resolved 0-based indices (or -1 for absent) into out_v/t/n.
// Returns true on success.
bool parse_corner(const char* tok, int v_count, int t_count, int n_count,
                  int& out_v, int& out_t, int& out_n) {
    int a = 0, b = 0, c = 0;
    int nfields = std::sscanf(tok, "%d/%d/%d", &a, &b, &c);
    if (nfields == 3) {
        out_v = resolve_index(a, v_count);
        out_t = resolve_index(b, t_count);
        out_n = resolve_index(c, n_count);
        return true;
    }
    // "a//c"
    nfields = std::sscanf(tok, "%d//%d", &a, &c);
    if (nfields == 2) {
        out_v = resolve_index(a, v_count);
        out_t = -1;
        out_n = resolve_index(c, n_count);
        return true;
    }
    // "a/b"
    nfields = std::sscanf(tok, "%d/%d", &a, &b);
    if (nfields == 2) {
        out_v = resolve_index(a, v_count);
        out_t = resolve_index(b, t_count);
        out_n = -1;
        return true;
    }
    // "a"
    nfields = std::sscanf(tok, "%d", &a);
    if (nfields == 1) {
        out_v = resolve_index(a, v_count);
        out_t = -1;
        out_n = -1;
        return true;
    }
    return false;
}

void compute_flat_normals(std::vector<MeshVertex>& verts,
                          const std::vector<uint16_t>& indices) {
    // Zero-init then accumulate area-weighted face normals.
    for (auto& v : verts) {
        v.normal[0] = 0.0f; v.normal[1] = 0.0f; v.normal[2] = 0.0f;
    }
    for (size_t i = 0; i + 2 < indices.size(); i += 3) {
        const auto& a = verts[indices[i]];
        const auto& b = verts[indices[i+1]];
        const auto& c = verts[indices[i+2]];
        float ex = b.pos[0] - a.pos[0], ey = b.pos[1] - a.pos[1], ez = b.pos[2] - a.pos[2];
        float fx = c.pos[0] - a.pos[0], fy = c.pos[1] - a.pos[1], fz = c.pos[2] - a.pos[2];
        float nx = ey * fz - ez * fy;
        float ny = ez * fx - ex * fz;
        float nz = ex * fy - ey * fx;
        for (int k = 0; k < 3; ++k) {
            verts[indices[i+k]].normal[0] += nx;
            verts[indices[i+k]].normal[1] += ny;
            verts[indices[i+k]].normal[2] += nz;
        }
    }
    for (auto& v : verts) {
        float len = std::sqrt(v.normal[0]*v.normal[0]
                            + v.normal[1]*v.normal[1]
                            + v.normal[2]*v.normal[2]);
        if (len > 1e-6f) {
            v.normal[0] /= len; v.normal[1] /= len; v.normal[2] /= len;
        } else {
            v.normal[0] = 0; v.normal[1] = 1; v.normal[2] = 0;
        }
    }
}

} // namespace

bool load_obj_text(const std::string& text, Mesh& out) {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec2> uvs;

    std::unordered_map<Triple, uint16_t, TripleHash> dedup;
    out.vertices.clear();
    out.indices.clear();
    out.materials.clear();
    out.submeshes.clear();

    // Submesh tracking. `usemtl foo` closes the running range (if non-empty)
    // and starts a new one bound to whatever material_idx `foo` resolves to.
    // `mat_name_to_idx` dedupes so two `usemtl red` sections share one
    // Material but produce two Submeshes (the renderer still wants separate
    // index ranges even when they share bindings).
    std::unordered_map<std::string, uint32_t> mat_name_to_idx;
    uint32_t current_submesh_start = 0;
    uint32_t current_mat_idx       = 0;
    bool     have_open_submesh     = false;

    auto close_open_submesh = [&]() {
        if (!have_open_submesh) return;
        const uint32_t end = (uint32_t)out.indices.size();
        if (end > current_submesh_start) {
            out.submeshes.push_back({ current_submesh_start,
                                      end - current_submesh_start,
                                      current_mat_idx });
        }
        have_open_submesh = false;
    };

    std::istringstream in(text);
    std::string line;
    bool had_normals = false;

    int line_no = 0;
    while (std::getline(in, line)) {
        line_no++;
        // strip comments + CR
        size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // skip blank
        size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;

        if (line.compare(first, 2, "v ") == 0) {
            Vec3 v{};
            if (std::sscanf(line.c_str() + first + 2, "%f %f %f", &v.x, &v.y, &v.z) == 3)
                positions.push_back(v);
        } else if (line.compare(first, 3, "vn ") == 0) {
            Vec3 n{};
            if (std::sscanf(line.c_str() + first + 3, "%f %f %f", &n.x, &n.y, &n.z) == 3) {
                normals.push_back(n);
                had_normals = true;
            }
        } else if (line.compare(first, 3, "vt ") == 0) {
            Vec2 t{};
            if (std::sscanf(line.c_str() + first + 3, "%f %f", &t.u, &t.v) >= 2)
                uvs.push_back(t);
        } else if (line.compare(first, 2, "f ") == 0) {
            // Tokenise face corners by whitespace.
            std::istringstream ls(line.substr(first + 2));
            std::vector<Triple> corners;
            std::string tok;
            bool face_ok = true;
            while (ls >> tok) {
                int v = -1, t = -1, n = -1;
                if (!parse_corner(tok.c_str(),
                                  (int)positions.size(),
                                  (int)uvs.size(),
                                  (int)normals.size(),
                                  v, t, n)) {
                    // Shouldn't happen on clean input — import_wcu_meshes.sh
                    // trims truncated lines now. Left as a safety net for
                    // hand-edited or third-party OBJs. Silent on purpose:
                    // the import script is the right place to complain.
                    face_ok = false;
                    break;
                }
                corners.push_back({v, t, n});
            }
            // Drop degenerate (<3 corner) faces silently — same reasoning:
            // clean input never produces these; noisy input shouldn't spam.
            if (!face_ok || corners.size() < 3) continue;
            // Fan-triangulate (OK for convex polygons, which OBJ faces always
            // are when exported from real modellers).
            auto emit_corner = [&](const Triple& tr) -> uint16_t {
                auto it = dedup.find(tr);
                if (it != dedup.end()) return it->second;
                MeshVertex mv{};
                if (tr.v >= 0 && tr.v < (int)positions.size()) {
                    mv.pos[0] = positions[tr.v].x;
                    mv.pos[1] = positions[tr.v].y;
                    mv.pos[2] = positions[tr.v].z;
                }
                if (tr.n >= 0 && tr.n < (int)normals.size()) {
                    mv.normal[0] = normals[tr.n].x;
                    mv.normal[1] = normals[tr.n].y;
                    mv.normal[2] = normals[tr.n].z;
                }
                if (tr.t >= 0 && tr.t < (int)uvs.size()) {
                    mv.uv[0] = uvs[tr.t].u;
                    mv.uv[1] = uvs[tr.t].v;
                }
                if (out.vertices.size() >= 65535) {
                    std::fprintf(stderr, "[obj] mesh exceeds uint16 index limit (65535 verts)\n");
                    return (uint16_t)0;
                }
                uint16_t idx = (uint16_t)out.vertices.size();
                out.vertices.push_back(mv);
                dedup.emplace(tr, idx);
                return idx;
            };
            // Drop triangles whose corners reference out-of-range indices —
            // this shields us from buggy producers (e.g. Vega Strike's
            // vega-meshtool occasionally truncates the last v/vn line of
            // its OBJ output, leaving a dangling higher index). Better to
            // miss one triangle than draw a spike through the origin.
            auto valid = [&](const Triple& t) {
                return t.v >= 0 && t.v < (int)positions.size();
            };
            if (!valid(corners[0])) continue;
            uint16_t i0 = emit_corner(corners[0]);
            for (size_t k = 1; k + 1 < corners.size(); ++k) {
                if (!valid(corners[k]) || !valid(corners[k + 1])) continue;
                uint16_t i1 = emit_corner(corners[k]);
                uint16_t i2 = emit_corner(corners[k + 1]);
                out.indices.push_back(i0);
                out.indices.push_back(i1);
                out.indices.push_back(i2);
            }
        }
        else if (line.compare(first, 7, "usemtl ") == 0) {
            // Close the running submesh so subsequent indices land in a
            // fresh range. Resolve (or create) the material slot for the
            // new name — materials are deduped by name; submeshes are not.
            close_open_submesh();

            std::string name = line.substr(first + 7);
            // Trim trailing whitespace (lines sometimes have stray tabs).
            while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
                name.pop_back();

            auto it = mat_name_to_idx.find(name);
            if (it == mat_name_to_idx.end()) {
                Material m;
                m.name = name;
                current_mat_idx = (uint32_t)out.materials.size();
                out.materials.push_back(std::move(m));
                mat_name_to_idx.emplace(std::move(name), current_mat_idx);
            } else {
                current_mat_idx = it->second;
            }
            current_submesh_start = (uint32_t)out.indices.size();
            have_open_submesh = true;
        }
        // mtllib / g / o / s — intentionally ignored (mtllib would point
        // to a .mtl file that vega-meshtool leaves empty anyway; our
        // texture info comes from the <stem>.materials.json sidecar
        // written by import_wcu_meshes.sh).
    }

    close_open_submesh();

    // Backwards compat: a mesh with no `usemtl` directives (our icosphere,
    // hand-authored test OBJs) gets one implicit material + one submesh
    // spanning the entire index buffer. Keeps the renderer's per-submesh
    // draw loop universal — no special-case for "mesh without materials".
    if (out.submeshes.empty() && !out.indices.empty()) {
        if (out.materials.empty()) {
            Material m;
            m.name = "__implicit__";
            out.materials.push_back(std::move(m));
        }
        out.submeshes.push_back({ 0, (uint32_t)out.indices.size(), 0 });
    }

    if (out.vertices.empty() || out.indices.empty()) {
        std::fprintf(stderr, "[obj] parsed but produced empty mesh\n");
        return false;
    }
    if (!had_normals) {
        // Author didn't provide normals — compute flat ones so shaders always
        // have something usable.
        compute_flat_normals(out.vertices, out.indices);
    }
    return true;
}

// Look up `name` (case-sensitive) in `mesh.materials` and return its index,
// or -1 if not found. Linear scan is fine — material tables are tiny (≤ ~32).
static int find_material_by_name(const Mesh& mesh, const std::string& name) {
    for (size_t i = 0; i < mesh.materials.size(); ++i)
        if (mesh.materials[i].name == name) return (int)i;
    return -1;
}

// Try each candidate path in order until one loads. Silent on misses —
// materials can legitimately omit slots.
static bool try_load_slot(const std::vector<std::string>& candidates,
                          TextureSlot& slot) {
    for (const auto& p : candidates)
        if (load_texture_png(p, slot)) return true;
    return false;
}

// Merge sidecar `<stem>.materials.json` into `mesh.materials`. The sidecar
// may reference materials the OBJ didn't use (harmless — they're skipped)
// and the OBJ may reference materials the sidecar didn't list (harmless —
// they stay with empty texture slots → neutral fallbacks at draw time).
static void load_material_sidecar(const std::string& obj_path, Mesh& mesh) {
    namespace fs = std::filesystem;
    fs::path op(obj_path);
    fs::path sidecar = op.parent_path() / (op.stem().string() + ".materials.json");
    if (!fs::exists(sidecar)) return;

    json::Value root = json::parse_file(sidecar.string());
    const json::Value* mats = root.find("materials");
    if (!mats || !mats->is_array()) return;

    const std::string mesh_dir = op.parent_path().string() + "/";
    int matched = 0, missing = 0;
    for (const auto& entry : mats->as_array()) {
        if (!entry.is_object()) continue;
        const std::string mat_name = entry.string_or("");
        const std::string& name    = entry.find("name")
                                     ? entry["name"].as_string()
                                     : mat_name;
        int idx = find_material_by_name(mesh, name);
        if (idx < 0) continue;   // sidecar-only entry; OBJ didn't use it
        Material& m = mesh.materials[idx];

        // For each slot, try (sidecar-named file) → (<stem>_slot.png legacy).
        // The legacy fallback is what kept the one-texture-per-ship setup
        // working before materials.json existed — keeps older meshes alive
        // without special-casing them here.
        const std::string stem = mesh_dir + op.stem().string();
        auto slot_load = [&](const char* key, const char* legacy_suffix,
                             TextureSlot& slot) {
            std::vector<std::string> candidates;
            if (const auto* v = entry.find(key); v && v->is_string())
                candidates.push_back(mesh_dir + v->as_string());
            candidates.push_back(stem + legacy_suffix);
            try_load_slot(candidates, slot);
        };
        slot_load("diffuse", ".png",       m.diffuse);
        slot_load("spec",    "_spec.png",  m.spec);
        slot_load("glow",    "_glow.png",  m.glow);
        slot_load("normal",  "_norm.png",  m.normal);

        if (const auto* v = entry.find("alpha_blend"); v && v->is_bool())
            m.alpha_blend = v->as_bool();

        matched++;
        if (!m.diffuse.valid) missing++;
    }
    if (missing > 0) {
        std::fprintf(stderr,
            "[obj] '%s': %d/%d materials with no diffuse (using white fallback)\n",
            op.filename().string().c_str(), missing, matched);
    }
}

bool load_obj_file(const std::string& path, Mesh& out) {
    // Fast path: load the binary cache if it's fresh. Skips OBJ text
    // parsing entirely. Textures are NOT in the cache — they're resolved
    // from the sidecar JSON in both paths, so that step stays consistent.
    if (!try_load_mesh_cache(path, out)) {
        std::ifstream f(path);
        if (!f) {
            std::fprintf(stderr, "[obj] cannot open '%s'\n", path.c_str());
            return false;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        if (!load_obj_text(ss.str(), out)) return false;

        // Populate AABB before caching so the cache file is self-sufficient
        // (the cache-hit branch above doesn't run the recompute — it reads
        // the stored values directly).
        out.recompute_aabb();

        // Best-effort cache write — a failure here just means next cold
        // start parses the OBJ again, no correctness impact.
        write_mesh_cache(path, out);
    }

    // Always apply the sidecar JSON's texture info on top. Keeps textures
    // mutable without having to invalidate the binary geometry cache
    // every time someone tweaks a PNG (or the sidecar itself — which is
    // also mtime-checked against the cache, so edits force a rebuild).
    load_material_sidecar(path, out);
    return true;
}
