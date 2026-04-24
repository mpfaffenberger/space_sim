#pragma once
// -----------------------------------------------------------------------------
// mesh_cache.h — binary cache of parsed OBJ geometry.
//
// OBJ parsing spends most of its time on sscanf + string dedup — for
// 50k-vertex ship meshes this adds hundreds of ms to cold start. The
// .npmesh format is a straight dump of what the OBJ loader would
// produce (MeshVertex array, uint16 index array, submesh ranges,
// material metadata) so loading is a read() + memcpy().
//
// Textures are NOT cached. They're loaded from PNG at runtime regardless
// — caching decompressed pixel data would blow up the cache size 10-20×
// without meaningfully affecting startup (stb_image is fast enough that
// PNG decode isn't the bottleneck).
//
// Invalidation is pure mtime: cache is reused iff it's at least as new
// as the OBJ AND the `.materials.json` sidecar (if one exists). Any
// format change bumps the 8-byte magic header, which forces a rebuild.
// -----------------------------------------------------------------------------

#include "mesh.h"
#include <string>

// Try to load `<obj_path-with-.npmesh-ext>`. Returns true and fills `out`
// on success. Returns false (silently) if:
//   * cache file is missing
//   * magic header doesn't match (old format, or not an npmesh file)
//   * cache mtime is older than the OBJ or the sidecar materials.json
//   * any IO error
// The caller falls back to parsing the OBJ directly on a false return.
bool try_load_mesh_cache(const std::string& obj_path, Mesh& out);

// Write a cache next to the OBJ reflecting `mesh`'s current geometry +
// material metadata. Called after a successful text parse so the next
// cold start can skip it. Failures are non-fatal (the cache is an
// optimization, never a correctness requirement).
bool write_mesh_cache(const std::string& obj_path, const Mesh& mesh);
