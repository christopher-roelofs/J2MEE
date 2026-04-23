#!/usr/bin/env bash
#
# Copies the runtime's keypad assets + DejaVu/FontAwesome fonts into
# app/src/main/assets/ and writes manifest.txt for the Android bootstrap
# to consume. Run after fetch_sdl.sh and after any keypad asset update;
# re-running is idempotent.

set -euo pipefail
cd "$(dirname "$0")"

ASSETS="app/src/main/assets"
RUNTIME=".."

mkdir -p "$ASSETS/fonts" "$ASSETS/games"

# Keypad sprites + layouts ─ wipe first so removals propagate.
rm -rf "$ASSETS/keypad"
cp -r "$RUNTIME/assets/keypad" "$ASSETS/keypad"

# Vendored icon font.
cp "$RUNTIME/third_party/fontawesome/fa-solid-900.ttf" "$ASSETS/fonts/"

# DejaVu — mandatory. Prefer system install; fall back to the vendored
# copy under launcher/ if present.
for f in DejaVuSans DejaVuSans-Bold DejaVuSansMono DejaVuSansMono-Bold; do
    src=""
    for cand in \
        "/usr/share/fonts/truetype/dejavu/$f.ttf" \
        "$RUNTIME/../launcher/fonts/$f.ttf"; do
        [[ -f "$cand" ]] && { src="$cand"; break; }
    done
    if [[ -z "$src" ]]; then
        echo "[error] $f.ttf not found — install ttf-dejavu or drop a copy" >&2
        exit 1
    fi
    cp "$src" "$ASSETS/fonts/$f.ttf"
done

# args.cfg — stub only if absent so the user's edits survive re-runs.
if [[ ! -f "$ASSETS/args.cfg" ]]; then
    cat > "$ASSETS/args.cfg" <<'EOF'
game.jar
com.example.MIDletClass
240x320
EOF
    echo "[info] wrote default args.cfg — edit it to name your MIDlet"
fi

# Placeholder so assets/games/ gets packed even when empty.
[[ -e "$ASSETS/games/.keep" ]] || : > "$ASSETS/games/.keep"

# manifest.txt — one path per line, relative to assets root. The bootstrap
# iterates this list to extract bundled files out of the APK into internal
# storage. Excludes itself and args.cfg (args.cfg is read in-place from
# the extracted tree, so we do want it in the manifest actually).
(cd "$ASSETS" && find . -type f ! -name manifest.txt \
    | sed 's|^\./||' | LC_ALL=C sort > manifest.txt)

echo "[ok] packed $(wc -l < "$ASSETS/manifest.txt") files into $ASSETS/"
