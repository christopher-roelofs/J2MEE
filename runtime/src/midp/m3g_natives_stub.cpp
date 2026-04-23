// Emscripten (and any non-GL host) builds use this stub in place of
// m3g_natives.cpp + m3g_backend.cpp. M3G (JSR-184) requires a GLES1
// context which the wasm POC doesn't ship. Games that probe
// Graphics3D.getInstance() will still find the class; every method
// registration is absent, so they'll hit the default stub path.
//
// Keep this file source-identical across builds that exclude the m3g
// static library — the linker just needs a definition for the entry
// symbol register_m3g_natives().

#include "natives.hpp"

void register_m3g_natives(VM& /*vm*/) {
    // no-op: M3G is unavailable in this build
}
