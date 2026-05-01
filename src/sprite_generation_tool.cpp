// -----------------------------------------------------------------------------
// sprite_generation_tool.cpp — F6 ImGui workbench for sprite-generation jobs.
// -----------------------------------------------------------------------------

#include "sprite_generation_tool.h"

#include "imgui.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace sprite_generation_tool {
namespace {

static bool g_visible = false;
static std::atomic<bool> g_running{false};
static std::mutex g_status_mutex;
static std::string g_status = "idle";
static std::atomic<int> g_last_exit_code{0};

static char g_ship[64] = "centurion";
static char g_display_name[96] = "Centurion";
static float g_az = 45.0f;
static float g_el = 0.0f;
static int g_quality = 2; // low, medium, high, auto
static bool g_use_default_canonical_refs = true;
static bool g_clean = true;
static bool g_preview = true;
static bool g_force = false;
static bool g_write_prompt_only = false;
static char g_primary_reference[512] = "";
static char g_extra_references[2048] = "";
static char g_prompt_suffix[4096] = "";
static char g_custom_prompt[8192] = "";

const char* quality_name() {
    static const char* names[] = {"low", "medium", "high", "auto"};
    return names[g_quality < 0 || g_quality > 3 ? 2 : g_quality];
}

void set_status(std::string s) {
    std::lock_guard<std::mutex> lock(g_status_mutex);
    g_status = std::move(s);
}

std::string get_status() {
    std::lock_guard<std::mutex> lock(g_status_mutex);
    return g_status;
}

std::filesystem::path find_repo_root() {
    std::filesystem::path p = std::filesystem::current_path();
    for (int i = 0; i < 8; ++i) {
        if (std::filesystem::exists(p / "tools" / "generate_ship_sprite_job.py") &&
            std::filesystem::exists(p / "CMakeLists.txt")) {
            return p;
        }
        if (!p.has_parent_path()) break;
        p = p.parent_path();
    }
    return std::filesystem::current_path();
}

std::string shell_quote(const std::filesystem::path& path) {
    const std::string in = path.string();
    std::string out = "'";
    for (char c : in) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string json_escape(const char* raw) {
    std::string out;
    for (const unsigned char c : std::string(raw ? raw : "")) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

std::vector<std::string> split_lines(const char* text) {
    std::vector<std::string> out;
    std::istringstream ss(text ? text : "");
    std::string line;
    while (std::getline(ss, line)) {
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) {
            line.pop_back();
        }
        size_t first = 0;
        while (first < line.size() && std::isspace(static_cast<unsigned char>(line[first]))) {
            ++first;
        }
        if (first < line.size()) out.push_back(line.substr(first));
    }
    return out;
}

std::filesystem::path request_path(const std::filesystem::path& repo) {
    return repo / "assets" / "shipgen" / "sprite_generation_request.json";
}

std::filesystem::path log_path(const std::filesystem::path& repo) {
    return repo / "assets" / "shipgen" / "sprite_generation.log";
}

bool write_request_file(const std::filesystem::path& repo, std::string& err) {
    const auto req_path = request_path(repo);
    std::error_code ec;
    std::filesystem::create_directories(req_path.parent_path(), ec);
    if (ec) {
        err = "failed to create request dir: " + ec.message();
        return false;
    }

    std::ofstream out(req_path);
    if (!out) {
        err = "failed to write request: " + req_path.string();
        return false;
    }

    const auto extra_refs = split_lines(g_extra_references);
    out << "{\n";
    out << "  \"ship\": \"" << json_escape(g_ship) << "\",\n";
    out << "  \"display_name\": \"" << json_escape(g_display_name) << "\",\n";
    out << "  \"az\": " << g_az << ",\n";
    out << "  \"el\": " << g_el << ",\n";
    out << "  \"primary_reference\": \"" << json_escape(g_primary_reference) << "\",\n";
    out << "  \"extra_references\": [";
    for (size_t i = 0; i < extra_refs.size(); ++i) {
        if (i) out << ", ";
        out << "\"" << json_escape(extra_refs[i].c_str()) << "\"";
    }
    out << "],\n";
    out << "  \"use_default_canonical_refs\": "
        << (g_use_default_canonical_refs ? "true" : "false") << ",\n";
    out << "  \"prompt_suffix\": \"" << json_escape(g_prompt_suffix) << "\",\n";
    out << "  \"custom_prompt\": \"" << json_escape(g_custom_prompt) << "\",\n";
    out << "  \"quality\": \"" << quality_name() << "\",\n";
    out << "  \"clean\": " << (g_clean ? "true" : "false") << ",\n";
    out << "  \"preview\": " << (g_preview ? "true" : "false") << ",\n";
    out << "  \"force\": " << (g_force ? "true" : "false") << ",\n";
    out << "  \"write_prompt_only\": " << (g_write_prompt_only ? "true" : "false") << "\n";
    out << "}\n";
    return true;
}

std::string read_log_tail(const std::filesystem::path& repo) {
    const auto path = log_path(repo);
    std::ifstream in(path, std::ios::binary);
    if (!in) return "(no log yet)";
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    const std::streamoff keep = 6000;
    in.seekg(size > keep ? size - keep : 0, std::ios::beg);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void launch_generation() {
    if (g_running.exchange(true)) {
        set_status("already running; let the current goblin finish");
        return;
    }

    const auto repo = find_repo_root();
    const auto script = repo / "tools" / "generate_ship_sprite_job.py";
    if (!std::filesystem::exists(script)) {
        g_running = false;
        set_status("script not found: " + script.string());
        return;
    }

    std::string err;
    if (!write_request_file(repo, err)) {
        g_running = false;
        set_status(err);
        return;
    }

    const auto req = request_path(repo);
    const auto log = log_path(repo);
    const std::string cmd =
        "cd " + shell_quote(repo) +
        " && /usr/bin/env python3 " + shell_quote(script) +
        " --request " + shell_quote(req) +
        " > " + shell_quote(log) + " 2>&1";

    set_status("running: " + cmd);
    std::thread([cmd]() {
        const int rc = std::system(cmd.c_str());
        g_last_exit_code.store(rc);
        g_running = false;
        set_status(rc == 0 ? "done" : "failed; check log");
    }).detach();
}

void open_path(const std::filesystem::path& p) {
#if defined(__APPLE__)
    const std::string cmd = "open " + shell_quote(p);
#else
    const std::string cmd = "xdg-open " + shell_quote(p);
#endif
    std::system(cmd.c_str());
}

} // namespace

void init() {
    std::printf("[sprite_gen_tool] ready — F6 to toggle\n");
}

void shutdown() {
    // Detached job may still be running. We intentionally do not kill it here;
    // the Python process writes files atomically-ish and should finish or fail
    // on its own. Sophisticated process supervision is YAGNI for v1.
}

bool handle_event(const sapp_event* e) {
    if (e->type == SAPP_EVENTTYPE_KEY_DOWN && e->key_code == SAPP_KEYCODE_F6) {
        g_visible = !g_visible;
        return true;
    }
    return false;
}

void build() {
    if (!g_visible) return;

    const auto repo = find_repo_root();
    ImGui::SetNextWindowSize(ImVec2(920, 760), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Sprite Generation Workbench (F6)", &g_visible)) {
        ImGui::End();
        return;
    }

    ImGui::TextWrapped("One-cell sprite generation front-end. Writes assets/shipgen/sprite_generation_request.json and launches tools/generate_ship_sprite_job.py. The engine stays blissfully ignorant of API nonsense.");
    ImGui::Separator();

    ImGui::InputText("ship slug", g_ship, sizeof(g_ship));
    ImGui::InputText("display name", g_display_name, sizeof(g_display_name));
    ImGui::InputFloat("azimuth", &g_az, 22.5f, 45.0f, "%.1f");
    ImGui::InputFloat("elevation", &g_el, 30.0f, 60.0f, "%.1f");
    ImGui::Combo("quality", &g_quality, "low\0medium\0high\0auto\0");

    ImGui::Checkbox("use default canonical refs", &g_use_default_canonical_refs);
    ImGui::SameLine();
    ImGui::Checkbox("clean alpha", &g_clean);
    ImGui::SameLine();
    ImGui::Checkbox("preview", &g_preview);
    ImGui::SameLine();
    ImGui::Checkbox("force", &g_force);
    ImGui::Checkbox("write prompt only / no model call", &g_write_prompt_only);

    ImGui::SeparatorText("References");
    ImGui::InputText("primary reference override", g_primary_reference, sizeof(g_primary_reference));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Blank = use assets/ships/<ship>/renders/manifest.json for this az/el.");
    }
    ImGui::InputTextMultiline("extra reference images", g_extra_references,
                              sizeof(g_extra_references), ImVec2(-1, 92));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("One path per line. Relative paths are from repo root.");
    }

    ImGui::SeparatorText("Prompting");
    ImGui::InputTextMultiline("prompt suffix", g_prompt_suffix,
                              sizeof(g_prompt_suffix), ImVec2(-1, 120));
    ImGui::InputTextMultiline("FULL custom prompt override", g_custom_prompt,
                              sizeof(g_custom_prompt), ImVec2(-1, 170));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Optional. If non-empty, replaces the generated structured prompt entirely.");
    }

    ImGui::Separator();
    if (ImGui::Button(g_running ? "Generating..." : "Generate", ImVec2(140, 0))) {
        launch_generation();
    }
    ImGui::SameLine();
    if (ImGui::Button("Write request only")) {
        std::string err;
        if (write_request_file(repo, err)) set_status("wrote " + request_path(repo).string());
        else set_status(err);
    }
    ImGui::SameLine();
    if (ImGui::Button("Open sprites folder")) {
        open_path(repo / "assets" / "ships" / g_ship / "sprites");
    }
    ImGui::SameLine();
    if (ImGui::Button("Open log")) {
        open_path(log_path(repo));
    }

    ImGui::Text("status: %s", get_status().c_str());
    ImGui::Text("last exit code: %d", g_last_exit_code.load());
    ImGui::TextWrapped("request: %s", request_path(repo).string().c_str());
    ImGui::TextWrapped("log: %s", log_path(repo).string().c_str());

    ImGui::SeparatorText("Log tail");
    const std::string tail = read_log_tail(repo);
    ImGui::BeginChild("##log_tail", ImVec2(-1, -1), ImGuiChildFlags_Border,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextUnformatted(tail.c_str());
    ImGui::EndChild();

    ImGui::End();
}

} // namespace sprite_generation_tool
