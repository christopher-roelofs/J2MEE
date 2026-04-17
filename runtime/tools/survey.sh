#!/usr/bin/env bash
# j2me regression survey.
#
# For each row in the manifest, runs j2me headlessly on the JAR with the
# given flags, classifies how far it got (init / startApp / paint / flush /
# pix), and compares against the declared minimum. Exits non-zero if any
# row regressed; missing JARs skip (the games/ tree may be gitignored).
#
# Reference-frame check: when a row has a committed baseline under
# tools/survey_refs/<jar-basename>.ppm, the current headless output is
# compared bit-exactly to it. Mismatch surfaces in the note column as
# "ref-DIFF". The emulator is deterministic given identical inputs and
# budgets, so any diff is a real shift worth noticing.
#
# Usage:
#   survey.sh                      # default: classify + ref-check
#   survey.sh --update-refs        # write current outputs as the new refs
#   survey.sh <manifest> <j2me-binary> <repo-root>
set -u

die() { echo "survey: $*" >&2; exit 2; }

update_refs=0
if [[ ${1:-} == --update-refs ]]; then
    update_refs=1
    shift
fi

script_dir=$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)
manifest=${1:-$script_dir/survey_manifest.txt}
binary=${2:-}
repo_root=${3:-}
refs_dir=$script_dir/survey_refs

# Resolve defaults: binary under ../build, repo root is two levels up from tools/
if [[ -z $binary ]]; then
    for cand in \
        "$script_dir/../build/j2me" \
        "$script_dir/../../runtime/build/j2me"; do
        [[ -x $cand ]] && binary=$cand && break
    done
fi
[[ -x ${binary:-/nonexistent} ]] || die "j2me binary not found (pass as \$2)"

if [[ -z $repo_root ]]; then
    repo_root=$(cd -- "$script_dir/../.." && pwd)
fi
[[ -d $repo_root ]] || die "repo root not a directory: $repo_root"
[[ -r $manifest ]]  || die "manifest not readable: $manifest"

# Perceptual drift between two PPMs. Prints an integer = count of bytes
# whose |a - b| > 16 grey levels, scaled by 10000 / body_len. 0 means
# identical; 10000 means every byte differs significantly.
ppm_drift() {
    python3 - "$1" "$2" <<'PY'
import sys
def body(p):
    with open(p, 'rb') as f:
        data = f.read()
    # Skip P6 header: 3 whitespace-separated tokens (magic, "w h", maxval),
    # honouring optional '#' comment lines between them.
    i = 0
    for _ in range(3):
        while i < len(data) and data[i:i+1] in (b' ', b'\t', b'\r', b'\n'):
            i += 1
        if i < len(data) and data[i:i+1] == b'#':
            while i < len(data) and data[i:i+1] != b'\n':
                i += 1
            continue  # re-enter outer loop without consuming a token
        while i < len(data) and data[i:i+1] not in (b' ', b'\t', b'\r', b'\n'):
            i += 1
    if i < len(data) and data[i:i+1] in (b' ', b'\t', b'\r', b'\n'):
        i += 1
    return data[i:]
a = body(sys.argv[1]); b = body(sys.argv[2])
n = min(len(a), len(b))
if n == 0:
    print(10000); sys.exit(0)
diff = sum(1 for x, y in zip(a[:n], b[:n]) if abs(x - y) > 16)
print(diff * 10000 // n)
PY
}

# Milestone rank.
rank() {
    case $1 in
        init)     echo 1;;
        startApp) echo 2;;
        paint)    echo 3;;
        flush)    echo 4;;
        pix)      echo 5;;
        *)        echo 0;;
    esac
}

# Pick the most-useful diagnostic line from a run's stderr. Prioritises:
#   1. "JVM exception: X" or "[thread] run() threw X" — unhandled error
#   2. "[repaint] paint threw: X" — painting exploded
#   3. "[midlet] notifyDestroyed() called" — game voluntarily quit early
#   4. repeated "[image] not found in JAR: X" — asset load failures that
#      usually cascade into NPEs
#   5. "[stub] X" on the final interesting line if nothing else fired
# Empty output means "no fingerprint worth reporting".
fingerprint() {
    local out=$1
    local line
    line=$(grep -m1 -E "JVM exception:|\[thread\] run\(\) threw|\[repaint\] paint threw" <<<"$out")
    if [[ -n $line ]]; then
        # Trim trailing whitespace and cap length
        line=${line%%$'\n'*}
        echo "${line:0:90}"; return
    fi
    if grep -q "\[midlet\] notifyDestroyed() called" <<<"$out"; then
        echo "[midlet] notifyDestroyed() — voluntary exit"; return
    fi
    local missing
    missing=$(grep -m1 "\[image\] not found in JAR:" <<<"$out")
    if [[ -n $missing ]]; then
        local n
        n=$(grep -c "\[image\] not found in JAR:" <<<"$out")
        echo "$missing (×$n)"; return
    fi
    echo ''
}

# Run j2me headlessly on one JAR and classify the outcome. The rendered
# PPM is left at $ppm_out (the caller passes its destination in) so the
# ref-check can diff it. Prints one line:
#   <level>\t<fingerprint>
# where <level> ∈ load-fail | init | startApp | paint | flush | pix.
classify() {
    local jar=$1 midlet=$2 flags=$3 ppm_out=$4
    # Budget: 500 ticks is enough to clear splash-delay timers and land on
    # the title/menu for the games we test. 20s wall-clock fence catches any
    # title that fast-loops without flushing (survey.sh must still terminate).
    local out
    out=$(ASAN_OPTIONS=detect_leaks=0 timeout 20 \
             "$binary" "$jar" "$midlet" $flags \
             --headless --ticks 500 --run-ms 15000 \
             --ppm "$ppm_out" --quiet 2>&1)
    # Non-FF body bytes = evidence the surface changed from its init-white
    # state. Games that clear to any non-white colour, blit bitmaps, or draw
    # dark shapes all produce non-0xFF bytes. A surface that was allocated
    # but never touched is still pure 0xFF, so this separates "drew something"
    # from "never painted".
    local non_ff=0
    if [[ -s $ppm_out ]]; then
        non_ff=$(tail -c +30 "$ppm_out" | LC_ALL=C tr -d '\377' | wc -c)
    fi
    local m_init m_start m_paint m_flush
    m_init=$(grep -c "\[survey\] midlet-constructed"  <<<"$out" || true)
    m_start=$(grep -c "\[survey\] startApp-entered"   <<<"$out" || true)
    m_paint=$(grep -c "\[survey\] first-paint"        <<<"$out" || true)
    m_flush=$(grep -c "\[survey\] first-flush"        <<<"$out" || true)

    # "pix" dominates — many titles (Jamdat, direct-draw) never call
    # Canvas.repaint() or GameCanvas.flushGraphics() but still render.
    # Threshold >64 filters accidental single-pixel artifacts.
    local level
    if   (( non_ff > 64 ));    then level=pix
    elif (( m_flush > 0 ));    then level=flush
    elif (( m_paint > 0 ));    then level=paint
    elif (( m_start > 0 ));    then level=startApp
    elif (( m_init  > 0 ));    then level=init
    else                            level=load-fail
    fi
    local fp
    fp=$(fingerprint "$out")
    # Tab-separated so the caller can split cleanly.
    printf '%s\t%s\n' "$level" "$fp"
}

# Name the reference PPM after the JAR's basename so the link is obvious.
ref_path_for() {
    local jar=$1
    local base
    base=$(basename "$jar")
    base=${base%.jar}
    echo "$refs_dir/${base}.ppm"
}

pad() { printf '%-*s' "$1" "$2"; }

total=0; pass=0; fail=0; skip=0
regressions=()

printf '%-58s  %-9s  %-9s  %-4s  %s\n' 'JAR' 'expected' 'actual' 'res' 'note'
printf '%.0s-' {1..130}; echo

while IFS= read -r raw || [[ -n $raw ]]; do
    line=${raw%%#*}
    [[ -z ${line// } ]] && continue
    IFS='|' read -r path midlet flags min_level _note <<<"$line"
    # trim each field
    path=${path%"${path##*[![:space:]]}"};  path=${path#"${path%%[![:space:]]*}"}
    midlet=${midlet%"${midlet##*[![:space:]]}"}; midlet=${midlet#"${midlet%%[![:space:]]*}"}
    flags=${flags%"${flags##*[![:space:]]}"};    flags=${flags#"${flags%%[![:space:]]*}"}
    min_level=${min_level%"${min_level##*[![:space:]]}"}; min_level=${min_level#"${min_level%%[![:space:]]*}"}
    [[ -z $path ]] && continue
    total=$((total+1))
    full=$repo_root/$path
    if [[ ! -e $full ]]; then
        printf '%s  %-9s  %-9s  %-4s  %s\n' "$(pad 58 "${path:0:58}")" \
            "$min_level" '-' 'SKIP' '(missing)'
        skip=$((skip+1))
        continue
    fi
    # Keep the PPM around so ref-check can compare it, then clean up below.
    ppm=$(mktemp --suffix=.ppm)
    IFS=$'\t' read -r actual fp < <(classify "$full" "$midlet" "$flags" "$ppm")
    min_rank=$(rank "$min_level")
    got_rank=$(rank "$actual")

    # ── Reference-frame check ────────────────────────────────────────────
    # Many titles animate their splash or drive timing from System.current
    # TimeMillis, so bit-exact ref matching fails spuriously. During
    # --update-refs we run the title twice and measure self-drift; that
    # drift + a small slack becomes the per-title tolerance stored alongside
    # the reference PPM. Default threshold for titles without a recorded
    # tolerance is 3% body drift at ±16 grey levels.
    #
    # `noref` in a manifest row skips ref checking entirely for that title.
    ref=$(ref_path_for "$full")
    tol_file=${ref%.ppm}.tolerance
    ref_status=''
    skip_ref=0
    [[ $flags == *noref* ]] && skip_ref=1
    if (( update_refs )); then
        if [[ $actual == pix && -s $ppm && $skip_ref == 0 ]]; then
            mkdir -p "$refs_dir"
            # Measure self-drift across 3 additional runs against the ref and
            # take the max. A single extra run under-samples the variance of
            # titles like Bejeweled Twist whose splash animation jitters by
            # different amounts per run depending on when Thread.sleep wakes.
            drift=0
            for i in 1 2 3; do
                ppm_n=$(mktemp --suffix=.ppm)
                ASAN_OPTIONS=detect_leaks=0 timeout 20 \
                    "$binary" "$full" "$midlet" $flags \
                    --headless --ticks 500 --run-ms 15000 \
                    --ppm "$ppm_n" --quiet >/dev/null 2>&1 || true
                if [[ -s $ppm_n ]]; then
                    d=$(ppm_drift "$ppm" "$ppm_n")
                    (( d > drift )) && drift=$d
                fi
                rm -f "$ppm_n"
            done
            # Tolerance = max_self_drift × 1.5 + 500 (50% slack beyond the
            # worst-case sample, plus a 5% floor for deterministic titles).
            # Calibrated so animated splashes (Spore Origins ~85%, Bejeweled
            # Twist ~20%) don't flake run-to-run while still catching a
            # major scene shift.
            self_tol=$(( drift * 150 / 100 + 500 ))
            (( self_tol < 500 )) && self_tol=500
            cp "$ppm" "$ref"
            printf '%d\n' "$self_tol" > "$tol_file"
            ref_status="ref-updated (tol=${self_tol}/10000, self=${drift}/10000)"
        fi
    elif (( skip_ref )); then
        :  # explicitly opted out
    elif [[ $actual == pix && -f $ref ]]; then
        drift=$(ppm_drift "$ref" "$ppm")
        # Per-title tolerance (set at --update-refs time); fall back to 300
        # (3%) for refs committed before tolerances existed.
        tol=300
        [[ -f $tol_file ]] && tol=$(cat "$tol_file")
        if (( drift <= tol )); then
            ref_status="ref-ok (${drift}/${tol})"
        else
            ref_status="ref-DIFF (${drift}/10000 vs tol=${tol})"
        fi
    elif [[ $actual == pix && ! -f $ref ]]; then
        ref_status='ref-missing'
    fi
    rm -f "$ppm"

    # Assemble note: show fingerprint (real issues) + ref-DIFF or ref-updated
    # if either is informative. ref-ok and ref-missing stay silent to reduce
    # noise on the clean rows.
    note=$fp
    [[ $actual == pix && -z $fp ]] && note=''
    case $ref_status in
        ref-DIFF*|ref-updated*)
            if [[ -n $note ]]; then
                note="$note  |  $ref_status"
            else
                note=$ref_status
            fi
            ;;
    esac
    # Ref mismatch is a regression even if the milestone passed.
    ref_regression=0
    [[ $ref_status == ref-DIFF* ]] && ref_regression=1

    if (( got_rank >= min_rank )) && (( ! ref_regression )); then
        printf '%s  %-9s  %-9s  %-4s  %s\n' "$(pad 58 "${path:0:58}")" \
            "$min_level" "$actual" 'PASS' "$note"
        pass=$((pass+1))
    else
        printf '%s  %-9s  %-9s  %-4s  %s\n' "$(pad 58 "${path:0:58}")" \
            "$min_level" "$actual" 'FAIL' "$note"
        fail=$((fail+1))
        if (( got_rank < min_rank )); then
            regressions+=("$path: expected>=$min_level got=$actual${fp:+  |  $fp}")
        else
            regressions+=("$path: $ref_status")
        fi
    fi
done < "$manifest"

printf '%.0s-' {1..100}; echo
echo "total=$total pass=$pass fail=$fail skip=$skip"

if (( fail > 0 )); then
    echo
    echo "regressions:"
    printf '  %s\n' "${regressions[@]}"
    exit 1
fi
exit 0
