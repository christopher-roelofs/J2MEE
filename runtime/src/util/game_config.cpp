#include "game_config.hpp"

#include "json.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

std::string GameConfig::path_for(const std::string& jar_stem) {
    const char* home = std::getenv("HOME");
    if (!home || jar_stem.empty()) return {};
    return (fs::path(home) / ".j2me" / jar_stem / "config.json").string();
}

GameConfig GameConfig::load(const std::string& jar_stem) {
    GameConfig cfg;
    std::string path = path_for(jar_stem);
    if (path.empty()) return cfg;
    std::ifstream f(path);
    if (!f) {
        // Missing file: write a defaults skeleton so users have something
        // concrete to edit on first run.
        cfg.save(jar_stem);
        return cfg;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    try {
        auto j = nlohmann::json::parse(ss.str());
        cfg.resolution     = j.value("resolution",     cfg.resolution);
        cfg.keypad_layout  = j.value("keypad_layout",  cfg.keypad_layout);
        cfg.keypad_visible = j.value("keypad_visible", cfg.keypad_visible);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[config] %s: %s (using defaults)\n",
                     path.c_str(), e.what());
    }
    return cfg;
}

bool GameConfig::save(const std::string& jar_stem) const {
    std::string path = path_for(jar_stem);
    if (path.empty()) return false;
    fs::create_directories(fs::path(path).parent_path());
    nlohmann::json j;
    j["resolution"]     = resolution;
    j["keypad_layout"]  = keypad_layout;
    j["keypad_visible"] = keypad_visible;
    std::ofstream f(path);
    if (!f) return false;
    f << j.dump(2) << "\n";
    return f.good();
}

std::optional<std::pair<int, int>> GameConfig::parsed_resolution() const {
    if (resolution.empty() || resolution == "auto") return std::nullopt;
    int w = 0, h = 0;
    if (std::sscanf(resolution.c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0)
        return std::make_pair(w, h);
    return std::nullopt;
}
