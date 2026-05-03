// -----------------------------------------------------------------------------
// atlas_grid_viewer.cpp — F4 grid + swap + resize tool. See header for the
// design discussion; this file is the meat.
// -----------------------------------------------------------------------------

#include "atlas_grid_viewer.h"

#include "json.h"
#include "ship_sprite.h"
#include "sprite.h"

#include "imgui.h"
#include "sokol_imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>          // std::system for swap-persistence runner
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace atlas_grid_viewer {

// ---- module-static state ---------------------------------------------------
//
// Visibility default = hidden. Same rationale as debug_panel / light editor:
// dev affordance, shouldn't clutter screenshots or hijack mouse input on
// startup. F4 reveals it on demand.
static bool        g_visible = false;
// Which atlas is being inspected. Empty until first build() call selects
// one (the first map entry, alphabetically — std::unordered_map's iteration
// order isn't stable, but for two ships the result is "Tarsus or Talon"
// either of which is a fine default; the user picks whatever they want
// from the combo on first interaction).
static std::string g_selected_atlas_key;
// Which frame inside the current atlas is selected. -1 = nothing. Index
// into ShipSpriteAtlas::frames (the manifest's natural order). Stays
// stable across SWAP because we mutate the frame's az/el/dir fields in
// place — the index points at the same memory either way.
static int         g_selected_frame_idx = -1;

// ---- regen-notes state -----------------------------------------------------
//
// Free-form review/regeneration comments — one optional per-frame string
// plus one ship-wide "general" string. Keyed by atlas.key so notes for
// Centurion don't bleed into Talon notes when the user flips the ship
// combo. Stored on disk at
//
//    assets/ships/<ship>/sprites/regen_notes.json
//
// in a shape Mike's batch_generate_ship_sprites.py can read for surgical
// re-runs:
//
//    {
//      "ship":    "<ship>",
//      "general": "...",
//      "frames": {
//        "az045_el+030": "swap top/bottom",
//        "az000_el+000": "front view hallucinated 4 engines"
//      }
//    }
//
// frame_key uses the same az/el text shape the renders/sprites already
// use on disk, so it's grep-friendly across the asset tree.
static std::map<std::string, std::string> g_general_notes;            // atlas_key -> text
static std::map<std::string,
                std::map<std::string, std::string>> g_frame_notes;    // atlas_key -> frame_key -> text
static std::set<std::string>             g_notes_loaded;              // atlas_keys whose JSON has been read

// ImGui::InputTextMultiline is char[]-buffer-based, so we shuttle text
// between std::string storage and these scratch buffers. 8 KB is more
// than enough for human review notes; if anyone hits the limit they're
// writing a novel, not a bug report. Frame buffer is sync'd to the map
// whenever the selection changes (or on Save).
static constexpr size_t kNoteBufBytes = 8192;
static char g_general_buf[kNoteBufBytes];
static char g_frame_buf  [kNoteBufBytes];
static std::string g_buffered_atlas_key;     // which atlas g_general_buf is for
static int         g_buffered_frame_idx = -1; // which frame  g_frame_buf   is for

// ---- helpers ---------------------------------------------------------------

// Strip "ships/<ship>/atlas_manifest" → "<ship>". Used so save_tuning can
// label the JSON's "ship" field correctly without us having to plumb the
// ship name through the runtime data.
static std::string ship_name_from_key(const std::string& key) {
    // key looks like "ships/tarsus/atlas_manifest". Find the segment
    // between the first and second '/'.
    const size_t a = key.find('/');
    if (a == std::string::npos) return key;
    const size_t b = key.find('/', a + 1);
    if (b == std::string::npos) return key.substr(a + 1);
    return key.substr(a + 1, b - a - 1);
}

// Convert SpriteArt::name (which is "assets/<stem>" with no .png) to the
// manifest's "sprite" field convention ("<stem>.png", no leading "assets/").
// We need this to match runtime frames against manifest samples in the
// generated swap-persistence script.
static std::string art_name_to_manifest_sprite(const std::string& name) {
    std::string s = name;
    constexpr const char* prefix = "assets/";
    if (s.rfind(prefix, 0) == 0) s.erase(0, std::strlen(prefix));
    s += ".png";
    return s;
}

// Recompute the unit-vector direction on the view sphere after an (az, el)
// mutation. Mirrors the formula in load_ship_sprite_atlas — we keep the
// two in sync by hand because the loader runs at startup only and we
// can't lean on it for runtime swap edits. If the formula ever drifts,
// see ShipSpriteFrame::dir for the canonical comment.
static void recompute_dir(ShipSpriteFrame& f) {
    constexpr float kPi = 3.14159265358979323846f;
    const float az_rad = f.az_deg * kPi / 180.0f;
    const float el_rad = f.el_deg * kPi / 180.0f;
    f.dir = HMM_V3(std::cos(el_rad) * std::sin(az_rad),
                   std::sin(el_rad),
                   std::cos(el_rad) * std::cos(az_rad));
}

// ---- persistence -----------------------------------------------------------

// Write atlas_manifest.tuning.json — same shape that ship_sprite.cpp's
// loader recognises (ship, frames[{az, el, scale, roll_deg}]). We always
// emit ALL frames, not just the modified ones, so a single tuning file
// is a self-contained snapshot. Cheap (82 lines) and avoids partial-state
// confusion if the file already has stale entries.
//
// Note: the loader matches tuning entries to runtime frames by (az, el),
// so the values we write here must reflect each frame's CURRENT post-swap
// labels. They already do — we just mirror frame.az_deg / .el_deg.
static void save_tuning(const ShipSpriteAtlas& atlas) {
    const std::filesystem::path path =
        std::filesystem::path("assets") / (atlas.key + ".tuning.json");
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    out << "{\n  \"ship\": \"" << ship_name_from_key(atlas.key) << "\",\n"
        << "  \"frames\": [\n";
    for (size_t i = 0; i < atlas.frames.size(); ++i) {
        const ShipSpriteFrame& f = atlas.frames[i];
        out << "    { \"az\": "       << f.az_deg
            << ", \"el\": "           << f.el_deg
            << ", \"scale\": "        << f.scale
            << ", \"roll_deg\": "     << f.roll_deg << " }"
            << (i + 1 == atlas.frames.size() ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
    std::printf("[atlas_grid_viewer] wrote %s (%zu frames)\n",
                path.string().c_str(), atlas.frames.size());
}

// ---- regen notes -----------------------------------------------------------

// Stable per-frame key. Mirrors the on-disk filename convention used by
// the renders/sprites pipeline ("az045_el+030") so a `grep regen_notes`
// can be cross-referenced trivially with the asset tree. We round to the
// nearest 0.5° and use 'p' for the half-step to match e.g.
// `centurion_az022p5_el-060.png`.
static std::string frame_key_az_el(float az_deg, float el_deg) {
    auto az_part = [](float v) {
        const float r = std::round(v * 2.0f) / 2.0f;
        const int   whole = (int)std::floor(r);
        const bool  half  = std::fabs(r - whole) > 0.25f;
        char buf[32];
        if (half) std::snprintf(buf, sizeof(buf), "az%03dp5", whole);
        else      std::snprintf(buf, sizeof(buf), "az%03d",   whole);
        return std::string(buf);
    };
    auto el_part = [](float v) {
        const float r = std::round(v * 2.0f) / 2.0f;
        const int   whole = (int)(r >= 0.0f ? std::floor(r) : std::ceil(r));
        const bool  half  = std::fabs(r - (float)whole) > 0.25f;
        char buf[32];
        if (half) std::snprintf(buf, sizeof(buf), "el%+04dp5", whole);
        else      std::snprintf(buf, sizeof(buf), "el%+04d",   whole);
        return std::string(buf);
    };
    return az_part(az_deg) + "_" + el_part(el_deg);
}

// regen_notes.json lives next to the generated sprites because the
// sprite generator already cd-friendly-keys all of its bookkeeping there
// (jobs.jsonl, prompts/, pixelart_batch_specs.json). Keeps everything
// one ship-deep and one tool-touchable.
static std::filesystem::path notes_path_for(const ShipSpriteAtlas& atlas) {
    return std::filesystem::path("assets/ships") /
           ship_name_from_key(atlas.key) /
           "sprites" / "regen_notes.json";
}

// Minimal JSON-string escape for our hand-rolled writer. The textareas
// can include newlines, quotes, and backslashes; everything else stays
// printable. Skips the \u00xx control-char branch deliberately because
// no one is pasting NULs into a code review note. (If they do, the JSON
// parser will tell them.)
static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

// Read regen_notes.json (if present) into the in-memory maps. Called
// the first time an atlas is selected so we don't pay disk I/O at
// startup for atlases the user never opens. Missing file = no notes
// yet, totally fine — leave maps empty.
static void load_notes_for(const ShipSpriteAtlas& atlas) {
    if (g_notes_loaded.count(atlas.key)) return;
    g_notes_loaded.insert(atlas.key);

    const auto path = notes_path_for(atlas);
    if (!std::filesystem::exists(path)) return;

    json::Value root = json::parse_file(path.string());
    if (!root.is_object()) {
        std::fprintf(stderr,
            "[atlas_grid_viewer] %s is not a JSON object — ignoring\n",
            path.string().c_str());
        return;
    }
    if (auto* g = root.find("general"); g && g->is_string()) {
        g_general_notes[atlas.key] = g->as_string();
    }
    if (auto* frames = root.find("frames"); frames && frames->is_object()) {
        auto& bucket = g_frame_notes[atlas.key];
        for (const auto& [k, v] : frames->as_object()) {
            if (v.is_string()) bucket[k] = v.as_string();
        }
    }
    std::printf("[atlas_grid_viewer] loaded notes from %s\n",
                path.string().c_str());
}

// Write regen_notes.json. Pretty-printed so humans can hand-edit. Empty
// frame entries get pruned so the file stays tidy after a user clears
// a comment. (We keep the file even if the maps are empty — a present
// `regen_notes.json` with empty objects is a clear "reviewed, no notes"
// signal in git diffs.)
static void save_notes_for(const ShipSpriteAtlas& atlas) {
    const auto path = notes_path_for(atlas);
    std::filesystem::create_directories(path.parent_path());

    const std::string& general = g_general_notes[atlas.key];
    auto& frames = g_frame_notes[atlas.key];
    // Drop empties so re-saving doesn't accumulate junk.
    for (auto it = frames.begin(); it != frames.end(); ) {
        if (it->second.empty()) it = frames.erase(it);
        else                    ++it;
    }

    std::ofstream out(path);
    out << "{\n";
    out << "  \"ship\": \""    << ship_name_from_key(atlas.key) << "\",\n";
    out << "  \"general\": \"" << json_escape(general)           << "\",\n";
    out << "  \"frames\": {";
    bool first = true;
    for (const auto& [k, v] : frames) {
        if (!first) out << ",";
        first = false;
        out << "\n    \"" << k << "\": \"" << json_escape(v) << "\"";
    }
    out << (frames.empty() ? "}\n" : "\n  }\n");
    out << "}\n";
    std::printf("[atlas_grid_viewer] wrote %s (%zu frame note(s), %zu chars general)\n",
                path.string().c_str(), frames.size(), general.size());
}

// Write a self-contained regeneration script to /tmp and return its path.
//
// Why a script file rather than a one-liner-in-the-clipboard? Two reasons:
//   1. Earlier draft used printf-with-double-quotes for the suffix, which
//      meant we had to hand-escape ", \, $, `, and crossed our fingers on
//      anything else. Users put angle brackets and apostrophes in their
//      regen notes. Predictably, that broke.
//   2. macOS clipboard via ImGui::SetClipboardText hasn't been reliable
//      for us in the past (`apply_swap_to_manifest` already migrated away
//      from a heredoc-in-clipboard for that exact reason). A short
//      `bash /tmp/regen_<ship>_<frame>.sh` is fine to clipboard — that's
//      ASCII-only and 30 chars long.
//
// The script uses a single-quoted heredoc (<<'NP_REGEN_NOTE_END') for the
// suffix body, which disables ALL shell expansion — quotes, backslashes,
// dollar signs, backticks, every variety of fun character a human might
// drop into a regen note all pass through verbatim. The only failure mode
// is if the note contains the literal string "NP_REGEN_NOTE_END" on a
// line of its own, which is sufficiently unlikely we will accept the risk
// and instead use that complaint as evidence of unusually creative QA.
static std::string write_regen_script(const ShipSpriteAtlas& atlas,
                                      const ShipSpriteFrame& f) {
    const std::string ship = ship_name_from_key(atlas.key);
    const std::string fk   = frame_key_az_el(f.az_deg, f.el_deg);
    const auto& fnotes = g_frame_notes[atlas.key];
    const auto  fit    = fnotes.find(fk);
    const std::string per_frame = (fit != fnotes.end()) ? fit->second : std::string();
    const std::string general   = g_general_notes[atlas.key];

    std::string suffix;
    if (!general.empty())   suffix += general + "\n\n";
    if (!per_frame.empty()) suffix += "Per-frame note for az="
        + std::to_string((int)std::round(f.az_deg)) + ", el="
        + std::to_string((int)std::round(f.el_deg)) + ":\n"
        + per_frame + "\n";

    const std::string script_path = "/tmp/regen_" + ship + "_" + fk + ".sh";
    const std::string suffix_path = "/tmp/regen_" + ship + "_" + fk + ".txt";

    char az_arg[32]; std::snprintf(az_arg, sizeof(az_arg), "%g", f.az_deg);
    char el_arg[32]; std::snprintf(el_arg, sizeof(el_arg), "%g", f.el_deg);

    std::string sh;
    sh.reserve(suffix.size() + 1024);
    sh += "#!/usr/bin/env bash\n";
    sh += "# Auto-generated by F4 atlas grid viewer. Regenerates one\n";
    sh += "# frame for ship='" + ship + "' at az=" + az_arg
        + ", el=" + el_arg + " with the regen notes below as\n";
    sh += "# additional prompt suffix.\n";
    sh += "set -euo pipefail\n";
    sh += "cd \"$(dirname \"$0\")/../\" 2>/dev/null || true   # noop fallback\n";
    sh += "# Write the prompt suffix via single-quoted heredoc — no shell\n";
    sh += "# expansion happens between the quoted markers, so any character\n";
    sh += "# in the user's regen note passes through verbatim.\n";
    sh += "cat > " + suffix_path + " <<'NP_REGEN_NOTE_END'\n";
    sh += suffix;
    if (suffix.empty() || suffix.back() != '\n') sh += "\n";
    sh += "NP_REGEN_NOTE_END\n";
    sh += "echo \"[regen] wrote suffix to " + suffix_path + "\"\n";
    sh += "\n";
    sh += "# Run from the repo root regardless of where bash is invoked.\n";
    sh += "cd \"" + std::filesystem::current_path().string() + "\"\n";
    // Finetune path: pass the prior sprite back into the model along with
    // the user's regen note. This is iterate-on-existing, not
    // generate-from-scratch — so it uses tools/finetune_ship_sprite.py
    // (which prepends the prior raw.png to reference_image) instead of
    // batch_generate. Same writes-and-cleans output paths so the refined
    // sprite drops into the existing atlas pipeline.
    sh += "exec python3 tools/finetune_ship_sprite.py \\\n";
    sh += "  --ship "               + ship + " \\\n";
    sh += "  --az "                 + std::string(az_arg) + " \\\n";
    sh += "  --el "                 + std::string(el_arg) + " \\\n";
    sh += "  --prompt-suffix-file " + suffix_path + " \\\n";
    sh += "  --quality high --clean --preview\n";

    if (FILE* fp = std::fopen(script_path.c_str(), "w")) {
        std::fwrite(sh.data(), 1, sh.size(), fp);
        std::fclose(fp);
        // chmod +x so a curious paste of just the path runs it. (`bash <path>`
        // works either way, but a +x bit makes the file friendlier to
        // double-click / direct-execute usage.)
        std::filesystem::permissions(script_path,
            std::filesystem::perms::owner_all |
            std::filesystem::perms::group_read |
            std::filesystem::perms::group_exec |
            std::filesystem::perms::others_read |
            std::filesystem::perms::others_exec,
            std::filesystem::perm_options::replace);
    } else {
        std::fprintf(stderr,
            "[atlas_grid_viewer] failed to write %s\n",
            script_path.c_str());
    }
    return script_path;
}

// macOS-flavoured clipboard helper: pipe the given text into pbcopy via
// a popen so we don't rely on ImGui's clipboard bridge (which ships
// inert on sokol-app builds unless wired manually). On non-macOS hosts
// the launch silently fails, which is fine — the user can still copy
// the path printed to stderr by hand.
static void copy_to_pasteboard(const std::string& text) {
#if defined(__APPLE__)
    if (FILE* p = popen("pbcopy", "w")) {
        std::fwrite(text.data(), 1, text.size(), p);
        pclose(p);
    } else {
        std::fprintf(stderr,
            "[atlas_grid_viewer] popen(pbcopy) failed; clipboard not updated\n");
    }
#else
    (void)text;
#endif
}

// Run the regen script in the background so the engine doesn't block on
// the ~150 s API call. We append `&` and redirect output to the script's
// own .log next to the .sh — engine returns immediately, generation
// happens in a child shell, user can `tail -f` if they want progress.
static void run_regen_script_background(const std::string& script_path) {
    const std::string log_path = script_path + ".log";
    const std::string cmd = "bash " + script_path
        + " > " + log_path + " 2>&1 &";
    std::printf("[atlas_grid_viewer] launching: %s\n", cmd.c_str());
    const int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::fprintf(stderr,
            "[atlas_grid_viewer] background launch returned rc=%d (script=%s)\n",
            rc, script_path.c_str());
    } else {
        std::printf("[atlas_grid_viewer] regen running in background; "
                    "tail -f %s for progress\n",
                    log_path.c_str());
    }
}

// Build a self-contained Python snippet that rewrites
// assets/<atlas.key>.json so each sample's (az, el) match the current
// runtime values. Mike copies the snippet → pastes into a shell → runs
// it. Idempotent: pasting twice produces identical output.
//
// Why a Python helper rather than a C++ writer? Our json::Value uses an
// unordered_map internally, so emitting it loses field order. Python's
// json module preserves dict insertion order, which means re-saved
// manifests keep their human-friendly key ordering and produce minimal
// diffs. The Python is small enough to embed verbatim — no separate
// file or PATH lookup needed.
//
// Match strategy: we key the override dict by the manifest's "sprite"
// field (relative path, with .png). That's stable under sample reordering
// in the manifest, so a future re-sort of samples doesn't break the swap
// history. Falls back gracefully — samples whose sprite isn't in the dict
// are left untouched.
// Apply the current in-engine (az, el) labels to the on-disk manifest.
// Earlier draft just copied a heredoc-shell script to the clipboard for
// the user to paste — clipboard reliability across platforms is
// sketchy and Mike (reasonably) expected the button to actually do
// the thing. Now: write the python to /tmp, run it via std::system,
// log the result. Engine briefly hangs while python runs (~50 ms);
// async-launch via popen would be marginal-cleanup-not-worth-it.
static void apply_swap_to_manifest(const ShipSpriteAtlas& atlas) {
    // Build the same script body as before but as a standalone file —
    // no heredoc framing needed. Path-resolves the manifest relative
    // to the engine's cwd (= repo root when launched via
    // ./build/new_privateer).
    std::string py;
    py.reserve(4096);
    py += "# Generated by F4 atlas grid viewer. Writes current\n";
    py += "# in-engine (az, el) labels back into the on-disk manifest.\n";
    py += "import json, pathlib, sys\n";
    py += "p = pathlib.Path('assets/" + atlas.key + ".json')\n";
    py += "m = json.loads(p.read_text())\n";
    py += "overrides = {\n";
    for (const ShipSpriteFrame& f : atlas.frames) {
        if (!f.art) continue;
        char line[256];
        std::snprintf(line, sizeof(line),
            "    %-90s: (%g, %g),\n",
            ("'" + art_name_to_manifest_sprite(f.art->name) + "'").c_str(),
            f.az_deg, f.el_deg);
        py += line;
    }
    py += "}\n";
    py += "n = 0\n";
    py += "for s in m['samples']:\n";
    py += "    key = s.get('sprite')\n";
    py += "    if key in overrides:\n";
    py += "        s['az'], s['el'] = overrides[key]\n";
    py += "        n += 1\n";
    py += "p.write_text(json.dumps(m, indent=2) + '\\n')\n";
    py += "print(f'[swap-persist] updated {p} ({n} sample labels)', flush=True)\n";

    // Write to /tmp then run. Stable filename (per-atlas) so successive
    // applies overwrite cleanly and you can inspect the last one.
    const std::string tmp_path = "/tmp/np_swap_persist_"
        + std::filesystem::path(atlas.key).filename().string() + ".py";
    if (FILE* f = std::fopen(tmp_path.c_str(), "w")) {
        std::fwrite(py.data(), 1, py.size(), f);
        std::fclose(f);
    } else {
        std::fprintf(stderr, "[atlas_grid_viewer] failed to write %s\n",
                     tmp_path.c_str());
        return;
    }

    const std::string cmd = "python3 " + tmp_path;
    std::printf("[atlas_grid_viewer] running: %s\n", cmd.c_str());
    const int rc = std::system(cmd.c_str());
    if (rc != 0) {
        std::fprintf(stderr,
            "[atlas_grid_viewer] swap-persist failed (rc=%d). Inspect script: %s\n",
            rc, tmp_path.c_str());
    }
}

// ---- lifecycle -------------------------------------------------------------

void init() {
    std::printf("[atlas_grid_viewer] ready — F4 to toggle\n");
}

void shutdown() {}

bool handle_event(const sapp_event* e) {
    if (e->type == SAPP_EVENTTYPE_KEY_DOWN &&
        e->key_code == SAPP_KEYCODE_F4) {
        g_visible = !g_visible;
        return true;     // consumed — don't let it leak to the game
    }
    return false;
}

// ---- build -----------------------------------------------------------------

void build(std::unordered_map<std::string, ShipSpriteAtlas>& atlases) {
    if (!g_visible) return;

    // Pre-size the window to fit a 16-column grid + inspector pane comfortably.
    // Once the user resizes manually ImGui remembers the choice via its .ini.
    ImGui::SetNextWindowSize(ImVec2(1280.0f, 720.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Ship Atlas Grid Viewer (F4)", &g_visible)) {
        ImGui::End();
        return;
    }

    if (atlases.empty()) {
        ImGui::TextDisabled("No ship atlases loaded — nothing to inspect.");
        ImGui::End();
        return;
    }

    // First-run + stale-key recovery: pick a sane default if our memory
    // of last frame's selection no longer matches a loaded atlas.
    if (g_selected_atlas_key.empty() ||
        atlases.find(g_selected_atlas_key) == atlases.end()) {
        g_selected_atlas_key = atlases.begin()->first;
        g_selected_frame_idx = -1;
    }

    // Ship picker — a flat combo over loaded atlases. Two ships in the
    // common case so a tab bar would be overkill, but a combo also scales
    // gracefully if more atlases get loaded later.
    if (ImGui::BeginCombo("ship", g_selected_atlas_key.c_str())) {
        for (auto& [key, _atlas] : atlases) {
            const bool is_sel = (key == g_selected_atlas_key);
            if (ImGui::Selectable(key.c_str(), is_sel)) {
                g_selected_atlas_key = key;
                g_selected_frame_idx = -1;     // reset selection on ship change
            }
            if (is_sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    ShipSpriteAtlas& atlas = atlases.at(g_selected_atlas_key);

    // Pull notes for this atlas off disk on first open. Cheap & lazy.
    load_notes_for(atlas);

    // Sync the general buffer to whichever atlas is on screen. Switching
    // ships flushes the previous atlas's edits back to its map first so
    // we don't lose typing-in-progress to a combo click.
    if (g_buffered_atlas_key != g_selected_atlas_key) {
        if (!g_buffered_atlas_key.empty()) {
            g_general_notes[g_buffered_atlas_key] = g_general_buf;
        }
        const std::string& src = g_general_notes[g_selected_atlas_key];
        std::snprintf(g_general_buf, kNoteBufBytes, "%s", src.c_str());
        g_buffered_atlas_key  = g_selected_atlas_key;
        g_buffered_frame_idx  = -1;     // force a re-sync of the frame buffer too
    }

    // Index frames by elevation row. std::map keeps elevations sorted —
    // we then iterate in REVERSE so the highest-el row (camera looking
    // straight down at the ship) renders at the top of the grid, matching
    // the spatial intuition of "above is up". Within each row we sort by
    // az ascending so (az=0) is leftmost.
    std::map<int, std::vector<int>> rows;
    for (int i = 0; i < (int)atlas.frames.size(); ++i) {
        rows[(int)std::round(atlas.frames[i].el_deg)].push_back(i);
    }
    for (auto& [_el, indices] : rows) {
        std::sort(indices.begin(), indices.end(), [&](int a, int b) {
            return atlas.frames[a].az_deg < atlas.frames[b].az_deg;
        });
    }

    // Layout: grid on the left, inspector on the right. Sized so the
    // grid's 16 cells + el labels fit comfortably at 64-px thumbnails;
    // the inspector takes the remainder of the window width.
    constexpr float kCellSize    = 64.0f;
    constexpr float kGridWidth   = kCellSize * 16.0f + 90.0f;  // + el-label gutter
    const ImVec4    kSelColor    (1.00f, 0.60f, 0.00f, 1.0f);  // hi-vis amber
    const ImVec4    kIdleColor   (0.20f, 0.20f, 0.20f, 1.0f);

    ImGui::BeginChild("##grid", ImVec2(kGridWidth, 0), true);
    ImGui::TextUnformatted("Click a cell to select. Click another to swap (az, el).");
    ImGui::TextDisabled("Cells: %zu  |  selection: %s",
                        atlas.frames.size(),
                        g_selected_frame_idx < 0
                            ? "(none)"
                            : "click another cell to swap, or click selection again to clear");
    ImGui::Separator();

    // High-el rows first → low-el rows last. Visually matches "looking
    // down at the ship from above" → top of grid; "from below" → bottom.
    for (auto rit = rows.rbegin(); rit != rows.rend(); ++rit) {
        const int el = rit->first;
        const std::vector<int>& indices = rit->second;

        // Row label gutter — fixed width so cells line up across rows
        // even when pole rows have only one entry.
        ImGui::AlignTextToFramePadding();
        ImGui::Text("el %+4d", el);
        ImGui::SameLine(80.0f);

        for (size_t i = 0; i < indices.size(); ++i) {
            const int idx = indices[i];
            ShipSpriteFrame& f = atlas.frames[idx];
            const bool is_selected = (g_selected_frame_idx == idx);

            ImGui::PushID(idx);
            // Highlight selected cells with a thick amber border. We use
            // the frame border so it's drawn around the ImageButton's
            // outline rather than overlaying the texture.
            ImGui::PushStyleColor(ImGuiCol_Border,
                                  is_selected ? kSelColor : kIdleColor);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize,
                                is_selected ? 3.0f : 1.0f);

            if (f.art) {
                const ImVec2 size(kCellSize, kCellSize);
                if (ImGui::ImageButton("##cell",
                                       simgui_imtextureid(f.art->hull.view),
                                       size)) {
                    if (g_selected_frame_idx < 0) {
                        // First pick — just select.
                        g_selected_frame_idx = idx;
                    } else if (g_selected_frame_idx == idx) {
                        // Re-click on selected → deselect.
                        g_selected_frame_idx = -1;
                    } else {
                        // SWAP. We move (az, el) labels between the two
                        // frames; the SpriteArt* (PNG content) stays put.
                        // Net effect: the cell-selector now picks the
                        // first cell's PNG when the runtime view direction
                        // matches the second cell's old (az, el), and
                        // vice versa.
                        ShipSpriteFrame& a = atlas.frames[g_selected_frame_idx];
                        ShipSpriteFrame& b = atlas.frames[idx];
                        std::swap(a.az_deg,  b.az_deg);
                        std::swap(a.el_deg,  b.el_deg);
                        // dir is a precomputed cache of (az, el) — recompute
                        // from scratch rather than swapping, in case future
                        // refactors widen ShipSpriteFrame and forget this.
                        recompute_dir(a);
                        recompute_dir(b);
                        g_selected_frame_idx = -1;
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "az %.0f  el %.0f\nscale %.2f  roll %.1f°\nfile: %s",
                        f.az_deg, f.el_deg, f.scale, f.roll_deg,
                        f.art->name.c_str());
                }
            } else {
                // Defensive — shouldn't happen post-load, but keep the
                // grid layout intact if some frame failed to load its art.
                ImGui::Dummy(ImVec2(kCellSize, kCellSize));
            }

            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
            ImGui::PopID();

            if (i + 1 < indices.size()) ImGui::SameLine();
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##inspector", ImVec2(0, 0), true);

    // Selection-change handling: if the user clicked a different cell,
    // flush the previous cell's text buffer into the map first, then
    // load the new cell's note (or a blank string). Without this, edits
    // would be lost any time the selection shifted.
    if (g_buffered_frame_idx != g_selected_frame_idx) {
        if (g_buffered_frame_idx >= 0 &&
            g_buffered_frame_idx <  (int)atlas.frames.size()) {
            const ShipSpriteFrame& prev = atlas.frames[g_buffered_frame_idx];
            const std::string fk = frame_key_az_el(prev.az_deg, prev.el_deg);
            std::string text = g_frame_buf;
            if (text.empty()) g_frame_notes[atlas.key].erase(fk);
            else              g_frame_notes[atlas.key][fk] = text;
        }
        g_frame_buf[0] = '\0';
        if (g_selected_frame_idx >= 0 &&
            g_selected_frame_idx <  (int)atlas.frames.size()) {
            const ShipSpriteFrame& cur = atlas.frames[g_selected_frame_idx];
            const std::string fk = frame_key_az_el(cur.az_deg, cur.el_deg);
            const auto& bucket = g_frame_notes[atlas.key];
            const auto it = bucket.find(fk);
            if (it != bucket.end()) {
                std::snprintf(g_frame_buf, kNoteBufBytes, "%s", it->second.c_str());
            }
        }
        g_buffered_frame_idx = g_selected_frame_idx;
    }

    if (g_selected_frame_idx < 0 ||
        g_selected_frame_idx >= (int)atlas.frames.size()) {
        ImGui::TextDisabled("(click a cell to select it for inspection)");
    } else {
        ShipSpriteFrame& f = atlas.frames[g_selected_frame_idx];
        ImGui::Text("Selected: az %.0f, el %.0f", f.az_deg, f.el_deg);
        ImGui::TextDisabled("%s",
                            f.art ? f.art->name.c_str() : "(no art)");

        if (f.art) {
            // Aspect-correct preview. 256 along the longest axis matches
            // the size used in the existing F2 / debug panel previews —
            // big enough to read fine details but doesn't dominate the
            // window so we still see the grid alongside it.
            const float longest = (float)std::max(f.art->hull_w, f.art->hull_h);
            const float pw = 256.0f * (float)f.art->hull_w / longest;
            const float ph = 256.0f * (float)f.art->hull_h / longest;
            ImGui::Image(simgui_imtextureid(f.art->hull.view), ImVec2(pw, ph));
        }

        // Live-mutate scale + roll. Bounds chosen pragmatically: scale
        // ≥ 0.3× catches things like over-cropped pole cells; ≤ 2.5×
        // is the upper end before the cell starts clipping outside its
        // billboard. Roll is the full ±180° because elevation cells can
        // need any rotation to align with cap_up.
        ImGui::SliderFloat("scale",       &f.scale,    0.3f,   2.5f, "%.3f");
        ImGui::SliderFloat("roll (deg)",  &f.roll_deg, -180.0f, 180.0f, "%.1f");

        // Per-frame regen note. Edits stay in g_frame_buf until either
        // selection changes or "Save notes" is clicked. Sized at ~6
        // text rows so multiline feedback fits without scroll-fishing.
        ImGui::Spacing();
        ImGui::TextUnformatted("Regen note for this frame:");
        ImGui::InputTextMultiline("##frame_note", g_frame_buf, kNoteBufBytes,
                                  ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 6.0f));

        // Helper lambda — flush the live buffers into the in-memory note
        // maps so any of the regen buttons below see the current screen
        // content even if the user hasn't clicked Save yet. Both buttons
        // need this, hence the lambda.
        auto flush_buffers = [&]() {
            const std::string fk = frame_key_az_el(f.az_deg, f.el_deg);
            std::string text = g_frame_buf;
            if (text.empty()) g_frame_notes[atlas.key].erase(fk);
            else              g_frame_notes[atlas.key][fk] = text;
            g_general_notes[atlas.key] = g_general_buf;
        };

        if (ImGui::Button("Copy regen command")) {
            flush_buffers();
            const std::string script_path = write_regen_script(atlas, f);
            // Clipboard payload is the SHORT command, not the whole script.
            // Reliable to copy + paste, ~30 chars of pure ASCII. The script
            // contains the spicy heredoc and arg list.
            const std::string clipboard_payload = "bash " + script_path;
            ImGui::SetClipboardText(clipboard_payload.c_str()); // ImGui-internal
            copy_to_pasteboard(clipboard_payload);              // OS clipboard (pbcopy)
            std::printf("[atlas_grid_viewer] regen script: %s\n"
                        "[atlas_grid_viewer] clipboard:    %s\n",
                        script_path.c_str(), clipboard_payload.c_str());
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Writes /tmp/regen_<ship>_<frame>.sh containing a self-contained\n"
                "regenerate-this-frame command (with current notes baked in via\n"
                "a single-quoted heredoc — survives any user-typed character).\n"
                "Copies a short `bash /tmp/...sh` to the clipboard so you can\n"
                "paste-and-run it in a separate terminal.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Regen now (bg)")) {
            flush_buffers();
            const std::string script_path = write_regen_script(atlas, f);
            run_regen_script_background(script_path);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Writes the regen script and immediately fork-launches it as\n"
                "a background bash process. Engine returns instantly; the\n"
                "~150 s OpenAI image-edit API call runs out-of-process.\n"
                "Tail /tmp/regen_<ship>_<frame>.sh.log for progress.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Reload from disk")) {
            // f.art is `const SpriteArt*` per the runtime contract — the
            // SpriteArt itself lives in main.cpp's `sprite_art` cache where
            // it's mutable. We're the legitimate hot-reload path, so a
            // const_cast is fine here (mirrors the pattern elsewhere in
            // main.cpp around the sprite-light editor handoff).
            if (f.art) {
                SpriteArt* mut = const_cast<SpriteArt*>(f.art);
                if (reload_sprite_art(*mut)) {
                    std::printf("[atlas_grid_viewer] reloaded '%s' from disk\n",
                                mut->name.c_str());
                } else {
                    std::fprintf(stderr,
                        "[atlas_grid_viewer] reload failed for '%s'\n",
                        mut->name.c_str());
                }
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Re-reads this cell's PNGs (hull + lights sidecar) and\n"
                "any *.lights.json animation sidecar from disk, replacing\n"
                "the in-memory GPU textures. Use after a Regen finishes\n"
                "to see the new sprite without restarting the engine.");
        }
        ImGui::SameLine();
        if (ImGui::Button("clear selection")) g_selected_frame_idx = -1;
    }

    // Ship-wide regen notes — always visible, regardless of selection.
    // Useful for global directives like "keep nose extra long" that the
    // copied per-frame regen command should fold into every prompt.
    ImGui::Separator();
    ImGui::TextUnformatted("Ship-wide regen notes:");
    ImGui::InputTextMultiline("##general_note", g_general_buf, kNoteBufBytes,
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4.0f));

    if (ImGui::Button("Save notes (regen_notes.json)")) {
        // Flush both live buffers into the map before serialising —
        // otherwise the user's most recent typing wouldn't make it.
        if (g_selected_frame_idx >= 0 &&
            g_selected_frame_idx <  (int)atlas.frames.size()) {
            const ShipSpriteFrame& cur = atlas.frames[g_selected_frame_idx];
            const std::string fk = frame_key_az_el(cur.az_deg, cur.el_deg);
            std::string text = g_frame_buf;
            if (text.empty()) g_frame_notes[atlas.key].erase(fk);
            else              g_frame_notes[atlas.key][fk] = text;
        }
        g_general_notes[atlas.key] = g_general_buf;
        save_notes_for(atlas);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Writes assets/ships/%s/sprites/regen_notes.json with all\n"
            "per-frame and ship-wide notes typed so far. Read by\n"
            "batch_generate_ship_sprites.py for surgical regen runs.",
            ship_name_from_key(atlas.key).c_str());
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Hot reload");
    if (ImGui::Button("Reload all cells from disk")) {
        // De-dup pointers — multiple ShipSpriteFrames can share one
        // SpriteArt (the cache is keyed by stem path). Reloading the
        // same SpriteArt twice would destroy a freshly-allocated GPU
        // image, so collect unique pointers first.
        std::set<SpriteArt*> uniq;
        for (ShipSpriteFrame& f : atlas.frames) {
            if (f.art) uniq.insert(const_cast<SpriteArt*>(f.art));
        }
        size_t ok = 0, fail = 0;
        for (SpriteArt* a : uniq) {
            if (reload_sprite_art(*a)) ++ok; else ++fail;
        }
        std::printf("[atlas_grid_viewer] reloaded %zu/%zu cells (failed=%zu) for atlas '%s'\n",
                    ok, uniq.size(), fail, atlas.key.c_str());
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Re-reads every cell PNG (hull + lights + animation sidecar)\n"
            "in the current atlas from disk. Useful after running a\n"
            "large batch regen or a feathering pass — same effect as\n"
            "restarting the engine, without losing fly-by-wire state.");
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Persistence");
    if (ImGui::Button("Save tuning (.tuning.json)")) {
        save_tuning(atlas);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Writes assets/%s.tuning.json with the current\n"
            "scale + roll_deg for every cell. The loader picks\n"
            "those up next time the engine starts.",
            atlas.key.c_str());
    }

    if (ImGui::Button("Apply swap to manifest")) {
        apply_swap_to_manifest(atlas);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Copies a one-liner Python heredoc to the clipboard.\n"
            "Paste it into a shell at the repo root to rewrite\n"
            "assets/%s.json with the current in-engine\n"
            "(az, el) labels for every sample. Idempotent.",
            atlas.key.c_str());
    }
    ImGui::EndChild();

    ImGui::End();
}

} // namespace atlas_grid_viewer
