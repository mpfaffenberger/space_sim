#pragma once
// -----------------------------------------------------------------------------
// obj_loader.h — minimal Wavefront OBJ mesh parser.
//
// Supports:
//   v x y z              — vertex position
//   vn x y z             — vertex normal
//   vt u v               — vertex texture coord
//   f a b c [d]          — face (triangles or quads; quads split into 2 tris)
//   f a/b/c or a//c      — face w/ optional uv & normal indices
//   # ...                — comments
//
// Deliberately ignores: materials (`mtllib`/`usemtl`), groups (`g`/`o`),
// smoothing (`s`). Those matter when we graduate to glTF; for placeholder
// geometry they're just noise.
//
// If the OBJ has no normals, we'll compute them ourselves (flat, area-
// weighted) so the result always has real normals and the caller doesn't
// have to. Same for missing UVs — zeroed.
// -----------------------------------------------------------------------------

#include "mesh.h"
#include <string>

// Load an OBJ file from disk. Returns true on success and fills `out`.
// On failure prints a reason to stderr and returns false.
bool load_obj_file(const std::string& path, Mesh& out);

// Parse an OBJ from an already-loaded text buffer. Useful for tests and for
// embedding placeholder meshes as string literals.
bool load_obj_text(const std::string& text, Mesh& out);
