cmake_minimum_required(VERSION 3.20)
project(j2me_runtime CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Vendored Sonivox EAS synthesizer (Apache-2.0). Defines sonivox_eas target.
add_subdirectory(third_party/sonivox)

add_compile_options(-Wall -Wextra -Wpedantic)

# Debug build enables sanitizers
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
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
    src/util/jar.cpp
    src/midp/natives.cpp
    src/midp/graphics_natives.cpp
    src/midp/audio_eas.cpp
    src/backend/display.cpp
)

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

add_executable(j2me ${SOURCES})
target_include_directories(j2me PRIVATE src)

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

target_link_libraries(j2me PRIVATE sonivox_eas)
