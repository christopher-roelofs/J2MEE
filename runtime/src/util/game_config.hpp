#pragma once
// Per-game preferences loaded from ~/.j2me/<jar-stem>/config.json. Each
// JAR gets its own file — same directory tree that RMS saves already
// live under. First-run creates a file populated with defaults so users
// have something concrete to edit.
//
// Scope today: resolution + on-screen keypad prefs. New fields are
// straightforward to add: extend the struct, extend load()/save().

#include <optional>
#include <string>

struct GameConfig {
    // "176x208" or "auto". "auto" = let the runtime derive (boxal.inf
    // hint, manifest Nokia-MIDlet-Original-Display-Size, or default
    // 240×320). Case-insensitive; the parser tolerates leading/trailing
    // whitespace.
    std::string resolution = "auto";

    // Folder name under runtime/assets/keypad/layouts/ to start with.
    // Layout cycle still works at runtime — this is just the initial
    // choice. Empty string means "whatever layout index 0 is".
    std::string keypad_layout = "minimal";

    // On-screen keypad starts visible. The user can tap the ▲ chevron
    // or press Tab at any time to collapse it to a thin strip.
    bool keypad_visible = true;

    // Resolve the config file path for a given JAR stem. Does not
    // create the file. Returns "" if $HOME isn't set.
    static std::string path_for(const std::string& jar_stem);

    // Load a config from disk for the given JAR stem. Missing file,
    // unreadable file, or parse errors all return a default-constructed
    // config (callers still get sensible values). Malformed fields are
    // silently reset to their defaults.
    static GameConfig load(const std::string& jar_stem);

    // Write current state to disk, creating the parent directory if
    // needed. Returns true on success. Safe to call repeatedly — writes
    // are complete, no partial file left behind on error.
    bool save(const std::string& jar_stem) const;

    // Parse the "resolution" field into width/height. Returns nullopt
    // if the field is "auto" or malformed (caller keeps existing res).
    std::optional<std::pair<int, int>> parsed_resolution() const;
};
