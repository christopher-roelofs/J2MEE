cmake_minimum_required(VERSION 3.20)
project(j2me_runtime CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Vendored Sonivox EAS synthesizer (Apache-2.0). Defines sonivox_eas target.
add_subdirectory(third_party/sonivox)

# Vendored Khronos M3G reference (EPL-1.0). Defines m3g target. Skipped under
# Emscripten — the reference impl needs GLES1 + EGL, which the wasm POC
# doesn't provide. The wasm build links src/midp/m3g_natives_stub.cpp instead
# of the real m3g_natives.cpp + m3g_backend.cpp.
if(NOT EMSCRIPTEN)
    add_subdirectory(third_party/m3g)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# Debug build enables sanitizers — but not under Emscripten (ASan/UBSan are
# not supported the same way there, and the POC build sticks with Release).
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT EMSCRIPTEN)
    add_compile_options(-fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

set(SOURCES
    src/main.cpp
    src/classfile/class_reader.cpp
    src/classfile/class_file.cpp
    src/vm/vm.cpp
    src/vm/interpreter.cpp
    src/vm/heap.cpp
    src/vm/class_def.cpp
    src/vm/class_loader.cpp
    src/vm/scheduler.cpp
    src/vm/stub_registry.cpp
    src/util/jar.cpp
    src/util/game_config.cpp
    src/midp/natives.cpp
    src/midp/graphics_natives.cpp
    src/midp/audio_eas.cpp
    src/midp/lcdui_natives.cpp
    src/backend/display.cpp
    src/backend/overlay.cpp
)

if(EMSCRIPTEN)
    list(APPEND SOURCES src/midp/m3g_natives_stub.cpp)
else()
    list(APPEND SOURCES
        src/midp/m3g_backend.cpp
        src/midp/m3g_natives.cpp)
endif()

if(EMSCRIPTEN)
    # Emscripten ships SDL2 / SDL2_image / SDL2_ttf / SDL2_mixer / zlib as
    # "ports" — activated by link flags, not find_package. Compile-time
    # flags must match the link-time USE flags for headers to resolve.
    set(EM_SDL_FLAGS
        "-sUSE_SDL=2"
        "-sUSE_SDL_IMAGE=2"
        "-sUSE_SDL_TTF=2"
        "-sUSE_SDL_MIXER=2"
        "-sUSE_ZLIB=1"
        "-sSDL2_IMAGE_FORMATS=[\"png\"]"
        "-sSDL2_MIXER_FORMATS=[\"mid\",\"wav\"]")
    add_compile_options(${EM_SDL_FLAGS})
    # Keep the warning level sane — vendored headers from ports can be noisy.
    add_compile_options(-Wno-unused-command-line-argument)
    # Enable JS-backed C++ exceptions. Our interpreter uses throw/catch for
    # Java exception propagation across native↔interp boundaries, and the
    # fiber thunk catches at the fiber entry. -fexceptions makes Emscripten
    # emit EH tables so those catches actually work in wasm.
    add_compile_options(-fexceptions)
    # -O2: the Asyncify transform multiplies local-variable usage inside
    # every instrumented function. -O0 / -O1 leave counts so high that
    # V8 rejects the wasm with "local count too large". -O2 runs enough
    # optimization passes in wasm-opt to deduplicate locals. It's a rough
    # middle ground on link time — slower than -O1 would be, much faster
    # than -O3 for iteration.
    add_compile_options(-O2)
    add_link_options(-O2)
else()
    find_package(ZLIB REQUIRED)
    find_package(SDL2 REQUIRED)

    # SDL2_image may not have a CMake config on all distros — try config first, fall back to pkg-config
    find_package(SDL2_image QUIET)
    if(NOT SDL2_image_FOUND)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL2_IMAGE REQUIRED SDL2_image)
    endif()

    # SDL2_ttf for text rendering
    find_package(SDL2_ttf QUIET)
    if(NOT SDL2_ttf_FOUND)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL2_TTF REQUIRED SDL2_ttf)
    endif()

    # SDL2_mixer for audio (MIDI, WAV)
    find_package(SDL2_mixer QUIET)
    if(NOT SDL2_mixer_FOUND)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL2_MIXER REQUIRED SDL2_mixer)
    endif()
endif()

add_executable(j2me ${SOURCES})
target_include_directories(j2me PRIVATE src third_party/json)

if(EMSCRIPTEN)
    target_link_libraries(j2me PRIVATE sonivox_eas)
    target_link_options(j2me PRIVATE ${EM_SDL_FLAGS})

    # --- Asset preloading ------------------------------------------------
    # All paths resolve inside MEMFS at runtime. We ship:
    #   /game.jar              — the J2ME JAR (Bejeweled for the POC)
    #   /fonts/DejaVuSans.ttf  — must match FONT_PATH in graphics_natives.cpp
    # The preload paths substitute the system font locations the native build
    # hard-codes. graphics_natives.cpp selects them via the J2ME_EMSCRIPTEN
    # compile-time branch.
    set(J2ME_WEB_GAME "${CMAKE_SOURCE_DIR}/../games/bejeweled_240x320-16705.jar"
        CACHE FILEPATH "Path to the JAR preloaded into the web build")
    set(J2ME_WEB_MIDLET "ca.jamdat.flight.J2MEApp"
        CACHE STRING "MIDlet class name passed to VM.run() in the web build")
    set(J2ME_WEB_RES "240x320"
        CACHE STRING "Screen resolution hint for the web build")

    set(J2ME_WEB_FONTS_DIR "${CMAKE_BINARY_DIR}/web_assets/fonts")
    file(MAKE_DIRECTORY "${J2ME_WEB_FONTS_DIR}")
    configure_file(
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
        "${J2ME_WEB_FONTS_DIR}/DejaVuSans.ttf" COPYONLY)
    configure_file(
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
        "${J2ME_WEB_FONTS_DIR}/DejaVuSans-Bold.ttf" COPYONLY)
    configure_file(
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
        "${J2ME_WEB_FONTS_DIR}/DejaVuSansMono.ttf" COPYONLY)
    configure_file(
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"
        "${J2ME_WEB_FONTS_DIR}/DejaVuSansMono-Bold.ttf" COPYONLY)
    # Icon font used by the on-screen keypad overlay. Vendored in
    # third_party/fontawesome (Font Awesome Free, OFL-1.1).
    configure_file(
        "${CMAKE_SOURCE_DIR}/third_party/fontawesome/fa-solid-900.ttf"
        "${J2ME_WEB_FONTS_DIR}/fa-solid-900.ttf" COPYONLY)

    set(J2ME_WEB_GAME_DST "${CMAKE_BINARY_DIR}/web_assets/game.jar")
    configure_file("${J2ME_WEB_GAME}" "${J2ME_WEB_GAME_DST}" COPYONLY)

    target_compile_definitions(j2me PRIVATE
        J2ME_WEB_MIDLET="${J2ME_WEB_MIDLET}"
        J2ME_WEB_RES="${J2ME_WEB_RES}")

    # Split "WxH" into two vars so shell.html can bake them into both the
    # canvas backing-store attributes and its fixed CSS display size.
    # Display is exactly 2× backing so image-rendering: pixelated stays
    # pixel-perfect. If someone overrides J2ME_WEB_RES we re-run config.
    string(REGEX MATCH "^([0-9]+)x([0-9]+)$" _ "${J2ME_WEB_RES}")
    set(J2ME_WEB_W "${CMAKE_MATCH_1}")
    set(J2ME_WEB_H "${CMAKE_MATCH_2}")
    if(NOT J2ME_WEB_W OR NOT J2ME_WEB_H)
        message(FATAL_ERROR "J2ME_WEB_RES must be WIDTHxHEIGHT (got '${J2ME_WEB_RES}')")
    endif()
    math(EXPR J2ME_WEB_CSS_W "${J2ME_WEB_W} * 2")
    math(EXPR J2ME_WEB_CSS_H "${J2ME_WEB_H} * 2")
    configure_file(
        "${CMAKE_SOURCE_DIR}/web/shell.html"
        "${CMAKE_BINARY_DIR}/web_assets/shell.html"
        @ONLY)

    # Emscripten link options:
    #   -sASYNCIFY               supports ucontext swapcontext + SDL_Delay
    #   -sASYNCIFY_STACK_SIZE    green-thread stacks can grow deep
    #   -sALLOW_MEMORY_GROWTH    heap starts at 128 MB and can grow
    #   -sINITIAL_MEMORY         pre-allocate enough for loaded JAR + images
    #   -sSTACK_SIZE             main-thread C stack (not Java thread stacks)
    #   --preload-file           bundle data into MEMFS
    #   -sEXIT_RUNTIME=0         main() returning is not an exit; keep going
    target_link_options(j2me PRIVATE
        "-sASYNCIFY=1"
        "-sASYNCIFY_STACK_SIZE=131072"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sINITIAL_MEMORY=134217728"
        "-sSTACK_SIZE=1048576"
        "-sEXIT_RUNTIME=0"
        "-sEXPORTED_RUNTIME_METHODS=[\"ccall\",\"cwrap\"]"
        "-fexceptions"
        "--preload-file=${CMAKE_BINARY_DIR}/web_assets/game.jar@/game.jar"
        "--preload-file=${J2ME_WEB_FONTS_DIR}@/fonts"
        "--shell-file=${CMAKE_BINARY_DIR}/web_assets/shell.html")
    set_target_properties(j2me PROPERTIES SUFFIX ".html")
else()
    if(SDL2_image_FOUND)
        target_link_libraries(j2me PRIVATE ZLIB::ZLIB SDL2::SDL2 SDL2_image::SDL2_image)
    else()
        target_include_directories(j2me PRIVATE ${SDL2_IMAGE_INCLUDE_DIRS})
        target_link_libraries(j2me PRIVATE ZLIB::ZLIB SDL2::SDL2 ${SDL2_IMAGE_LIBRARIES})
    endif()

    if(SDL2_ttf_FOUND)
        target_link_libraries(j2me PRIVATE SDL2_ttf::SDL2_ttf)
    else()
        target_include_directories(j2me PRIVATE ${SDL2_TTF_INCLUDE_DIRS})
        target_link_libraries(j2me PRIVATE ${SDL2_TTF_LIBRARIES})
    endif()

    if(SDL2_mixer_FOUND)
        target_link_libraries(j2me PRIVATE SDL2_mixer::SDL2_mixer)
    else()
        target_include_directories(j2me PRIVATE ${SDL2_MIXER_INCLUDE_DIRS})
        target_link_libraries(j2me PRIVATE ${SDL2_MIXER_LIBRARIES})
    endif()

    target_link_libraries(j2me PRIVATE sonivox_eas m3g)
endif()
