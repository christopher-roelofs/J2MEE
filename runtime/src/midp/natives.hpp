#pragma once
#include "vm/vm.hpp"
#include "util/jar.hpp"
#include <vector>
#include <cstdint>

// Backing data for a Java InputStream backed by raw bytes.
struct StreamEntry {
    std::vector<uint8_t> data;
    int32_t pos = 0;
    int32_t mark = 0;  // for mark()/reset() support
};

// Register all built-in native method implementations.
// Call this before vm.run().
void register_natives(VM& vm, const JarFile& jar);

// Register SDL2-backed graphics, image, and canvas natives.
// Call this after register_natives() and before vm.run().
void register_graphics_natives(VM& vm, const JarFile& jar);

// Register M3G (JSR-184) native bridge. Routes javax/microedition/m3g/*
// methods through to the vendored Khronos M3G core (third_party/m3g).
// Call after register_natives() / register_graphics_natives().
void register_m3g_natives(VM& vm);

// Register LCDUI form/textfield/textbox/StringItem/Item native impls.
// State-only (no visual rendering of Form items yet — that needs full
// LFImpl-equivalent layout work).
void register_lcdui_natives(VM& vm);
