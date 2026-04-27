// -----------------------------------------------------------------------------
// system_def.cpp — JSON → StarSystem materialisation.
// -----------------------------------------------------------------------------

#include "system_def.h"
#include "json.h"

#include <cstdio>
#include <filesystem>

namespace {

// Read an optional [x, y, z] triple into a HMM_Vec3, leaving the caller-
// supplied default if the key is absent or the value doesn't parse.
HMM_Vec3 vec3_or(const json::Value* v, HMM_Vec3 def) {
    if (!v || !v->is_array() || v->as_array().size() != 3) return def;
    const auto& arr = v->as_array();
    if (!arr[0].is_number() || !arr[1].is_number() || !arr[2].is_number()) return def;
    return { arr[0].as_float(), arr[1].as_float(), arr[2].as_float() };
}

PlacedMeshDef parse_mesh(const json::Value& v) {
    PlacedMeshDef m;
    if (auto* p = v.find("obj"))            m.obj_path      = p->as_string();
    m.position  = vec3_or(v.find("position"),  m.position);
    m.euler_deg = vec3_or(v.find("euler_deg"), m.euler_deg);
    m.tint      = vec3_or(v.find("tint"),       m.tint);
    if (auto* p = v.find("length_meters")) m.length_meters  = p->as_float();
    if (auto* p = v.find("scale"))         m.scale          = p->as_float();
    if (auto* p = v.find("spec"))          m.spec           = p->as_float();
    if (auto* p = v.find("texture_preset"))m.texture_preset = p->as_string();
    if (auto* p = v.find("double_sided")) m.double_sided   = p->as_bool();
    if (auto* p = v.find("ambient_floor")) m.ambient_floor = p->as_float();
    if (auto* p = v.find("rim_strength"))  m.rim_strength  = p->as_float();
    // Atmosphere block: { "thickness": 0.04, "color": [r,g,b], "strength": 1.2 }
    if (auto* atm = v.find("atmosphere"); atm && atm->is_object()) {
        if (auto* p = atm->find("thickness")) m.atm_thickness = p->as_float();
        m.atm_color = vec3_or(atm->find("color"), m.atm_color);
        if (auto* p = atm->find("strength"))  m.atm_strength  = p->as_float();
    }
    return m;
}

// A placed sprite entry. Minimal schema — sprite stem, world position,
// and length in world units. Everything else about the sprite (lights,
// textures) is resolved from files next to the PNG.
PlacedSpriteDef parse_sprite(const json::Value& v) {
    PlacedSpriteDef s;
    if (auto* p = v.find("sprite"))         s.sprite        = p->as_string();
    s.position = vec3_or(v.find("position"), s.position);
    if (auto* p = v.find("length_meters"))  s.length_meters = p->as_float();
    return s;
}

PlacedShipSpriteDef parse_ship_sprite(const json::Value& v) {
    PlacedShipSpriteDef s;
    if (auto* p = v.find("atlas"))          s.atlas         = p->as_string();
    s.position = vec3_or(v.find("position"), s.position);
    if (auto* p = v.find("length_meters"))  s.length_meters = p->as_float();
    if (auto* p = v.find("lights_enabled")) s.lights_enabled = p->as_bool();

    // Optional `motion` block. Either field is independently optional;
    // omitting both keeps the ship static (back-compat for every existing
    // scene). Angular velocity is authored in deg/s for human readability;
    // the runtime converts to rad/s once at scene-load time.
    if (auto* m = v.find("motion")) {
        s.angular_velocity_deg = vec3_or(m->find("angular_velocity_deg"),
                                         s.angular_velocity_deg);
        if (auto* p = m->find("forward_speed")) s.forward_speed = p->as_float();
    }
    return s;
}

// A nav waypoint — name, kind tag, position. Kind defaults to "nav" so a
// minimal entry { "name": "X", "position": [...] } parses cleanly.
NavPointDef parse_nav(const json::Value& v) {
    NavPointDef n;
    if (auto* p = v.find("name")) n.name = p->as_string();
    if (auto* p = v.find("kind")) n.kind = p->as_string();
    n.position = vec3_or(v.find("position"), n.position);
    return n;
}

AsteroidFieldDef parse_field(const json::Value& v) {
    AsteroidFieldDef f;
    f.center      = vec3_or(v.find("center"),      f.center);
    f.half_extent = vec3_or(v.find("half_extent"), f.half_extent);
    if (auto* p = v.find("count"))       f.count       = p->as_int();
    if (auto* p = v.find("base_radius")) f.base_radius = p->as_float();
    if (auto* p = v.find("size_min"))    f.size_min    = p->as_float();
    if (auto* p = v.find("size_max"))    f.size_max    = p->as_float();
    if (auto* p = v.find("seed"))        f.seed        = p->as_u32();
    return f;
}

// If the caller passed "troy", resolve to assets/systems/troy.json.
// If they passed a path with a slash or .json suffix, use it literally.
std::string resolve_path(const std::string& s) {
    const bool has_slash = s.find('/') != std::string::npos;
    // Guard the .json-suffix check with a length test — otherwise `size()-5`
    // underflows when the name is < 5 chars (hello, "troy") and matches
    // std::string::npos by coincidence, sending us down the literal-path
    // branch with a relative filename like "troy" that doesn't exist.
    const bool has_json_suffix = s.size() >= 5 &&
                                 s.compare(s.size() - 5, 5, ".json") == 0;
    if (has_slash || has_json_suffix) return s;
    return "assets/systems/" + s + ".json";
}

} // namespace

std::optional<StarSystem> load_system(const std::string& name_or_path) {
    const std::string path = resolve_path(name_or_path);
    if (!std::filesystem::exists(path)) {
        std::fprintf(stderr, "[system] file not found: %s\n", path.c_str());
        return std::nullopt;
    }

    json::Value root = json::parse_file(path);
    if (!root.is_object()) {
        std::fprintf(stderr, "[system] '%s': root is not a JSON object\n", path.c_str());
        return std::nullopt;
    }

    StarSystem s;
    s.name        = root.find("name")        ? root["name"].string_or(s.name)               : s.name;
    s.description = root.find("description") ? root["description"].string_or(s.description) : s.description;
    s.skybox_seed = root.find("skybox_seed") ? root["skybox_seed"].string_or(s.skybox_seed) : s.skybox_seed;

    if (auto* star = root.find("star")) {
        if (auto* p = star->find("preset")) s.star_preset = p->as_string();
    }

    if (auto* p = root.find("studio_lighting")) s.studio_lighting = p->as_bool();

    if (auto* fields = root.find("asteroid_fields"); fields && fields->is_array()) {
        for (const auto& f : fields->as_array()) {
            s.asteroid_fields.push_back(parse_field(f));
        }
    }

    if (auto* meshes = root.find("placed_meshes"); meshes && meshes->is_array()) {
        for (const auto& m : meshes->as_array()) {
            s.placed_meshes.push_back(parse_mesh(m));
        }
    }

    if (auto* sprites = root.find("placed_sprites"); sprites && sprites->is_array()) {
        for (const auto& sp : sprites->as_array()) {
            s.placed_sprites.push_back(parse_sprite(sp));
        }
    }

    if (auto* ships = root.find("placed_ship_sprites"); ships && ships->is_array()) {
        for (const auto& sp : ships->as_array()) {
            s.placed_ship_sprites.push_back(parse_ship_sprite(sp));
        }
    }

    if (auto* navs = root.find("nav_points"); navs && navs->is_array()) {
        for (const auto& n : navs->as_array()) {
            s.nav_points.push_back(parse_nav(n));
        }
    }

    if (auto* ps = root.find("player_start")) {
        s.player_start = vec3_or(ps->find("position"), s.player_start);
    }

    std::printf("[system] loaded '%s' — %s (skybox=%s, star=%s, fields=%zu, meshes=%zu, sprites=%zu, ship_sprites=%zu, navs=%zu)\n",
                path.c_str(), s.name.c_str(), s.skybox_seed.c_str(),
                s.star_preset.c_str(), s.asteroid_fields.size(),
                s.placed_meshes.size(), s.placed_sprites.size(),
                s.placed_ship_sprites.size(), s.nav_points.size());
    return s;
}
