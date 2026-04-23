#!/usr/bin/env bash
#
# Fetches SDL2 + satellite sources into app/jni/ so the NDK CMake build
# can consume them. Run once from runtime/android/ before ./gradlew assembleDebug.
#
# Versions chosen for stability: SDL 2.30 is the long-term 2.x branch and
# each satellite tracks it. Bump these to get newer patches.

set -euo pipefail
cd "$(dirname "$0")"

SDL_VERSION=2.30.11
SDL_IMAGE_VERSION=2.8.2
SDL_TTF_VERSION=2.22.0
SDL_MIXER_VERSION=2.8.0

JNI_DIR="app/jni"
mkdir -p "$JNI_DIR"

fetch_unpack () {
    local url=$1 dest=$2 strip=$3
    if [[ -d "$JNI_DIR/$dest" ]]; then
        echo "[skip] $dest already exists"
        return
    fi
    echo "[fetch] $url"
    local tmp
    tmp=$(mktemp)
    curl -fL "$url" -o "$tmp"
    mkdir -p "$JNI_DIR/$dest"
    tar -xzf "$tmp" --strip-components="$strip" -C "$JNI_DIR/$dest"
    rm -f "$tmp"
}

fetch_unpack "https://github.com/libsdl-org/SDL/releases/download/release-${SDL_VERSION}/SDL2-${SDL_VERSION}.tar.gz" \
             "SDL" 1
fetch_unpack "https://github.com/libsdl-org/SDL_image/releases/download/release-${SDL_IMAGE_VERSION}/SDL2_image-${SDL_IMAGE_VERSION}.tar.gz" \
             "SDL_image" 1
fetch_unpack "https://github.com/libsdl-org/SDL_ttf/releases/download/release-${SDL_TTF_VERSION}/SDL2_ttf-${SDL_TTF_VERSION}.tar.gz" \
             "SDL_ttf" 1
fetch_unpack "https://github.com/libsdl-org/SDL_mixer/releases/download/release-${SDL_MIXER_VERSION}/SDL2_mixer-${SDL_MIXER_VERSION}.tar.gz" \
             "SDL_mixer" 1

# SDL_image's vendored libpng path needs a sibling clone of libpng + zlib
# sources. The tarball ships only the glue — external/download.sh pulls
# the actual submodule sources. Skipped if already populated.
if [[ ! -d "$JNI_DIR/SDL_image/external/libpng" ]]; then
    echo "[fetch] SDL_image external submodules (zlib + libpng + …)"
    (cd "$JNI_DIR/SDL_image" && ./external/download.sh)
fi

# SDL ships Java scaffold that must be compiled into the APK. Copy it into
# our app/src/main/java tree so the Gradle Android plugin picks it up.
JAVA_SRC="app/src/main/java/org/libsdl/app"
mkdir -p "$JAVA_SRC"
cp -r "$JNI_DIR/SDL/android-project/app/src/main/java/org/libsdl/app/." "$JAVA_SRC/"

echo
echo "SDL sources in place."
echo "Next: drop a JAR into app/src/main/assets/games/, edit assets/args.cfg,"
echo "      then from this directory:  ./gradlew assembleDebug"
