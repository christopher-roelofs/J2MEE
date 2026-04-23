# J2ME runtime — Android NDK build.
#
# Produces libmain.so, which SDLActivity dlopens via the
# android.app.lib_name meta-data in the manifest. libmain.so statically
# absorbs SDL2 + satellites + sonivox + the existing runtime sources.
#
# Expected layout (populated by fetch_sdl.sh):
#   app/jni/SDL/          SDL2 source drop
#   app/jni/SDL_image/    SDL2_image source drop
#   app/jni/SDL_ttf/      SDL2_ttf source drop
#   app/jni/SDL_mixer/    SDL2_mixer source drop
#
# The runtime source lives at  runtime/src/  (three levels up from this
# file). We reach into it rather than copying.

cmake_minimum_required(VERSION 3.22)
project(j2me_android CXX C)

# Vendored zlib 1.3 (SDL_image/external/zlib, branch v1.3-SDL) ships a
# version script that references `gz_intmax` without defining it. Newer
# ld.lld treats this as an error; upstream zlib 1.3.1 removed the symbol
# from the map. Relax to warning for every .so we link here.
add_link_options(-Wl,--undefined-version)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ../.. = app/    ../../../ = android/    ../../../../ = runtime/
set(RUNTIME_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")

# ── SDL + satellites (vendored) ─────────────────────────────────────────
# Build each as a shared lib; they end up in the APK's lib/<abi>/ dir.
set(SDL_SHARED ON  CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
add_subdirectory(SDL)

# Satellites resolve SDL via the ::SDL2 target exported above.
set(SDL2IMAGE_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_VENDORED ON CACHE BOOL "" FORCE)  # use bundled zlib/libpng
# stb_image (the default SDL_image backend) is stricter on PNG chunks than
# libpng — JAMDAT's Bejeweled PNGs include non-standard chunks libpng
# tolerates but stb rejects, producing a runtime null Image and an NPE
# inside the game. Force libpng via the vendored copy so the Android build
# matches desktop decode behaviour.
set(SDL2IMAGE_BACKEND_STB OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_PNG ON CACHE BOOL "" FORCE)
set(SDL2IMAGE_JPG OFF CACHE BOOL "" FORCE)
set(SDL2IMAGE_BMP ON  CACHE BOOL "" FORCE)
# libpng + zlib from SDL_image/external add install(EXPORT) rules that
# break because they reference each other's targets from separate export
# sets. We don't install these libraries — they get statically absorbed
# into libSDL2_image.so — so suppress their install rules entirely.
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
add_subdirectory(SDL_image)

set(SDL2TTF_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL2TTF_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDL2TTF_VENDORED ON CACHE BOOL "" FORCE)
set(SDL2TTF_HARFBUZZ OFF CACHE BOOL "" FORCE)
add_subdirectory(SDL_ttf)

set(SDL2MIXER_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_VENDORED ON CACHE BOOL "" FORCE)
# MIDI via vendored timidity — SDL2_mixer's native MIDI backend is forced
# off on Android, so we rely on timidity (which ships with SDL2_mixer).
# Leaving SDL2MIXER_MIDI_TIMIDITY at its default ON is required to avoid
# a FATAL_ERROR from SDL_mixer's CMake when MIDI is enabled without any
# backend.
set(SDL2MIXER_MIDI ON CACHE BOOL "" FORCE)
set(SDL2MIXER_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_MOD OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_MP3 OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_OPUS OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_VORBIS "" CACHE STRING "" FORCE)
set(SDL2MIXER_FLAC OFF CACHE BOOL "" FORCE)
set(SDL2MIXER_WAVPACK OFF CACHE BOOL "" FORCE)
add_subdirectory(SDL_mixer)

# ── Sonivox (EAS MIDI synth) ───────────────────────────────────────────
# Reuse the same subdir the desktop build pulls in — built into a separate
# build tree under our binary dir so it doesn't collide.
add_subdirectory("${RUNTIME_ROOT}/third_party/sonivox"
                 "${CMAKE_CURRENT_BINARY_DIR}/sonivox")

# ── Runtime sources ────────────────────────────────────────────────────
# Mirrors the SOURCES list in runtime/CMakeLists.txt. M3G is excluded — the
# reference impl needs GLES1 + EGL desktop, which Android's GLES2+ doesn't
# provide cleanly. We link the same stub the wasm build uses.
set(J2ME_SOURCES
    ${RUNTIME_ROOT}/src/main.cpp
    ${RUNTIME_ROOT}/src/classfile/class_reader.cpp
    ${RUNTIME_ROOT}/src/classfile/class_file.cpp
    ${RUNTIME_ROOT}/src/vm/vm.cpp
    ${RUNTIME_ROOT}/src/vm/interpreter.cpp
    ${RUNTIME_ROOT}/src/vm/heap.cpp
    ${RUNTIME_ROOT}/src/vm/class_def.cpp
    ${RUNTIME_ROOT}/src/vm/class_loader.cpp
    ${RUNTIME_ROOT}/src/vm/scheduler.cpp
    ${RUNTIME_ROOT}/src/vm/stub_registry.cpp
    ${RUNTIME_ROOT}/src/util/jar.cpp
    ${RUNTIME_ROOT}/src/util/game_config.cpp
    ${RUNTIME_ROOT}/src/midp/natives.cpp
    ${RUNTIME_ROOT}/src/midp/graphics_natives.cpp
    ${RUNTIME_ROOT}/src/midp/audio_eas.cpp
    ${RUNTIME_ROOT}/src/midp/lcdui_natives.cpp
    ${RUNTIME_ROOT}/src/midp/m3g_natives_stub.cpp
    ${RUNTIME_ROOT}/src/backend/display.cpp
    ${RUNTIME_ROOT}/src/backend/overlay.cpp
    android_platform.cpp
)

add_library(main SHARED ${J2ME_SOURCES})
target_include_directories(main PRIVATE
    ${RUNTIME_ROOT}/src
    ${RUNTIME_ROOT}/third_party/json)

target_compile_definitions(main PRIVATE
    J2ME_ANDROID=1)

# -fexceptions: matches the wasm build comment. Interpreter relies on C++
# exceptions for Java exception propagation across native↔interp. Also
# -Wno-unused-parameter for the vendored SDL satellites.
target_compile_options(main PRIVATE
    -fexceptions
    -Wall -Wextra -Wno-unused-parameter)

# Android 15+ on Pixel/ARM devices enforces 16 KB memory page alignment for
# shared libraries; NDK 26 defaults to 4 KB. Without this flag the loader
# shows a "PageSizeMismatchDialog" warning on launch and future OS releases
# will reject the APK outright. Apply to every .so we ship.
target_link_options(main PRIVATE "-Wl,-z,max-page-size=16384")

target_link_libraries(main PRIVATE
    SDL2::SDL2
    SDL2_image::SDL2_image
    SDL2_ttf::SDL2_ttf
    SDL2_mixer::SDL2_mixer
    sonivox_eas
    # NDK system libs: zlib (JAR decompression), android (AAssetManager),
    # log (logcat), GLESv2 (SDL's default renderer backend).
    z
    android
    log
    GLESv2)

# SDLActivity expects SDL2 as a normal shared lib alongside libmain.so —
# the APK packager picks both up automatically because they're ADD_LIBRARY
# shared targets reachable from the root target.
