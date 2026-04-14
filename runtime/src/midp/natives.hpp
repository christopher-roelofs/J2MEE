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
