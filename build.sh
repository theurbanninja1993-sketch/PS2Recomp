#!/usr/bin/env bash
# Builds the game from the player's own disc - the whole pipeline, end to end.
# This is what the builder app's wizard will drive; here it is as a script so the
# individual steps can be run, timed and debugged without a UI in the way.
#
#   tools/build_pipeline.sh --image <disc|image|folder> [options]
#
#     --image PATH   the source: an optical drive (/dev/sr0, E:), an .iso/.bin,
#                    or an already-extracted folder (skips the extraction).
#     --out DIR      where the finished game folder goes (default: dist-pipeline)
#     --work DIR     scratch for the intermediate steps (default: build-pipeline)
#     --from N       start at stage N (resume a long build)
#     --to N         stop after stage N
#     --keep-going   do not stop when a stage fails (collecting diagnostics)
#
# Stages:
#   0 check      is this the right disc? (boot-file hash)
#   1 extract    pull the files off it
#   2 recompile  turn the game's ELF into C++ (ps2_recomp)
#   3 install    put that C++ into the runtime
#   4 build      compile the port
#   5 vu1        learn the VU1 microprograms by running the game, then compile them
#   6 assemble   make the portable folder
#
# Stages are idempotent: 3 and 6 overwrite what they produce, 4 and 5 are builds,
# so --from is safe to re-run. The recompile (~25 min) and the build (10-30 min)
# dominate the total, which is why they are separate stages.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PS2RECOMP="$ROOT"
IMAGE=""
OUT="$PWD/dist"
WORK="$PWD/build"
FROM=0
TO=99
KEEP_GOING=0

while [ $# -gt 0 ]; do
    case "$1" in
        --image) IMAGE="$2"; shift ;;
        --out) OUT="$2"; shift ;;
        --work) WORK="$2"; shift ;;
        --from) FROM="$2"; shift ;;
        --to) TO="$2"; shift ;;
        --keep-going) KEEP_GOING=1 ;;
        -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done

say()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
note() { printf '    %s\n' "$*"; }
die()  { printf '\n\033[31mFAILED: %s\033[0m\n' "$*" >&2; [ $KEEP_GOING -eq 1 ] || exit 1; FAILED=1; }

# ---------------------------------------------------------------- the games
# What the pipeline is allowed to build, keyed by the boot file's hash so a
# scratch disc is refused rather than half-built. The config names the shipped
# function map (addresses only - no game code) that the recompiler needs.
game_name=""; game_config=""; game_elf=""; game_hash=""
identify_game() {
    case "$1" in
        7f69b961cc427e3dcfd6582e25362be8)
            game_name="Grand Theft Auto: Vice City Stories"
            game_config="$ROOT/games/vcs/config/vcs_clean.toml"
            game_elf="SLUS_215.90"
            game_hash="$1" ;;
        *) return 1 ;;
    esac
    return 0
}

# ---------------------------------------------------------------- helpers
ps2_recomp="$PS2RECOMP/build/ps2xRecomp/ps2_recomp"
ps2_iso="$PS2RECOMP/build/ps2xBuilder/ps2x_iso"
vu1_recomp="$PS2RECOMP/build/ps2xRuntime/vu1_recomp"
runner_dir="$PS2RECOMP/ps2xRuntime/src/runner"
include_dir="$PS2RECOMP/ps2xRuntime/include"

need_tool() { [ -x "$1" ] || die "missing tool $1 (build it first)"; }

# The elapsed time of a stage, so the wizard can quote honest numbers.
stage_start=0
stage_begin() { stage_start=$(date +%s); say "$1"; }
stage_end()   { note "took $(( $(date +%s) - stage_start ))s"; }

# ===========================================================================
say "GTA PS2 build pipeline"
note "image : ${IMAGE:-<none>}"
note "out   : $OUT"
note "work  : $WORK"
note "stages: $FROM..$TO"

FAILED=0
mkdir -p "$WORK"

# Where the extracted files live. Set before the stages because every later one
# needs it, and --from may have skipped the stage that creates it.
disc_dir="$WORK/disc"

# The boot file's name. Stage 0 works it out from the disc, but --from can start
# after stage 0, so fall back to looking in the extracted files - which is what
# the recompile and the VU1 run need.
resolve_boot() {
    [ -n "$game_elf" ] && return 0
    local found
    # -L: $disc_dir is a symlink to the extracted folder, and find does not
    # follow a symlink given as its starting point.
    found=$(find -L "$disc_dir" -maxdepth 1 -type f -name 'SLUS_*.??' 2>/dev/null | head -1)
    if [ -n "$found" ]; then
        game_elf="$(basename "$found")"
        note "boot file (from the extracted files): $game_elf"
    fi
    return 0   # a non-zero return here would trip `set -e` at the call site
}
# A folder source needs no extraction, so link it now rather than in stage 1: a
# --from that skips stage 1 still needs $disc_dir to resolve, and resolve_boot
# reads through it.
if [ -d "$IMAGE" ]; then
    mkdir -p "$(dirname "$disc_dir")"
    ln -sfn "$(cd "$IMAGE" && pwd)" "$disc_dir"
fi
resolve_boot

# ---------------------------------------------------------------- stage 0
if [ "$FROM" -le 0 ] && [ "$TO" -ge 0 ]; then
    stage_begin "0. checking the disc"
    [ -n "$IMAGE" ] || die "no --image given"
    if [ -d "$IMAGE" ]; then
        # An already-extracted folder: hash the boot file directly.
        if [ ! -f "$IMAGE/$game_elf" ] && [ ! -f "$IMAGE/${game_elf#*/}" ]; then
            # We do not know which game it is yet, so look for the ELF by name.
            boot=$(find "$IMAGE" -maxdepth 2 -name "SLUS_*.??" | head -1)
            [ -n "$boot" ] || die "no boot file in $IMAGE"
            game_elf="$(basename "$boot")"
        fi
        hash=$(xxh128sum "$IMAGE/$game_elf" 2>/dev/null | cut -d' ' -f1 || true)
        note "extracted folder; boot $game_elf"
        [ -n "$hash" ] && note "hash $hash"
        identify_game "$hash" || die "this disc is not one of the supported games ($game_elf)"
    else
        need_tool "$ps2_iso"
        info=$("$ps2_iso" check "$IMAGE" 2>&1) || die "can't read $IMAGE"
        echo "$info" | sed 's/^/    /'
        hash=$(echo "$info" | sed -n 's/.*boot hash: \([0-9a-f]*\).*/\1/p')
        elf=$(echo "$info" | sed -n 's/.*boot: *\([^ ]*\).*/\1/p')
        identify_game "$hash" || die "this disc is not one of the supported games"
        game_elf="$elf"
        note "recognised: $game_name"
    fi
    stage_end
fi

# The extracted files this run works from.
disc_dir="$WORK/disc"

# ---------------------------------------------------------------- stage 1
if [ "$FROM" -le 1 ] && [ "$TO" -ge 1 ]; then
    stage_begin "1. extracting the disc"
    if [ -d "$IMAGE" ]; then
        note "already extracted (linked at $disc_dir)"
    else
        need_tool "$ps2_iso"
        rm -rf "$disc_dir"; mkdir -p "$disc_dir"
        # Straight off the disc rather than image-then-extract: a 4 GB disc
        # reads at ~4 MB/s, so a second pass would double the wait.
        "$ps2_iso" extract "$IMAGE" "$disc_dir" || die "extract failed"
    fi
    [ -f "$disc_dir/$game_elf" ] || die "no $game_elf in the extracted files"
    stage_end
fi

# ---------------------------------------------------------------- stage 2
generated="$WORK/generated"
if [ "$FROM" -le 2 ] && [ "$TO" -ge 2 ]; then
    stage_begin "2. recompiling the game's ELF"
    need_tool "$ps2_recomp"
    [ -f "$disc_dir/$game_elf" ] || die "no $game_elf at $disc_dir (run stage 1)"
    [ -f "$game_config" ] || die "missing config $game_config"

    # The shipped config names the map and the output; the input and output paths
    # are the player's, so rewrite those three keys rather than hand-editing.
    work_config="$WORK/$(basename "$game_config")"
    map_file="$(dirname "$game_config")/$(basename "$game_config" .toml).csv"
    note "config   -> $work_config"
    note "input    -> $disc_dir/$game_elf"
    note "output   -> $generated"
    python3 - "$game_config" "$work_config" "$disc_dir/$game_elf" "$generated" "$map_file" <<'PY'
import re, sys
src, dst, elf, out, csv = sys.argv[1:6]
text = open(src).read()
def set_key(text, key, value):
    return re.sub(r'(?m)^\s*' + key + r'\s*=.*$', f'{key} = "{value}"', text, count=1)
text = set_key(text, 'input', elf)
text = set_key(text, 'output', out + '/')
text = set_key(text, 'ghidra_output', csv)
open(dst, 'w').write(text)
PY
    rm -rf "$generated"; mkdir -p "$generated"
    ( cd "$ROOT" && "$ps2_recomp" "$work_config" ) || die "ps2_recomp failed"
    count=$(find "$generated" -name '*.cpp' | wc -l)
    note "generated $count .cpp files"
    [ "$count" -gt 0 ] || die "the recompiler produced nothing"
    stage_end
fi

# ---------------------------------------------------------------- stage 3
if [ "$FROM" -le 3 ] && [ "$TO" -ge 3 ]; then
    stage_begin "3. installing the generated code into the runtime"
    [ -d "$generated" ] || die "no generated code at $generated (run stage 2)"
    # Copy, never clean: src/runner also holds main.cpp, which is the runtime's.
    # And copy only what differs - the build keys off timestamps, so rewriting
    # identical files would force a full rebuild every single run.
    mkdir -p "$runner_dir" "$include_dir"
    install_if_changed() {
        local from="$1" to="$2" pattern="$3" f name
        while IFS= read -r f; do
            name="$(basename "$f")"
            if [ -f "$to/$name" ] && cmp -s "$f" "$to/$name"; then
                changed_skip=$((changed_skip + 1))
            else
                cp -f "$f" "$to/$name"
                changed_copy=$((changed_copy + 1))
            fi
        done < <(find "$from" -maxdepth 1 -type f -name "$pattern")
    }
    changed_skip=0 changed_copy=0
    install_if_changed "$generated" "$runner_dir" '*.cpp'
    install_if_changed "$generated" "$include_dir" '*.h'
    note "runner sources: $(find "$runner_dir" -name '*.cpp' | wc -l) ($changed_copy updated, $changed_skip unchanged)"
    note "headers:        $(find "$include_dir" -name 'ps2_recompiled*.h' | wc -l)"
    stage_end
fi

# ---------------------------------------------------------------- stage 4
if [ "$FROM" -le 4 ] && [ "$TO" -ge 4 ]; then
    stage_begin "4. building the port (slow: 10-30 min)"
    build="$PS2RECOMP/build"
    # Configure even when the cache exists: the VU1 directory only becomes
    # populated at stage 5, and re-configuring is what lets the build pick it up
    # on the second run. A missing vu1_programs.cpp is fine - the CMake just
    # skips it - so this is safe before stage 5 has ever run.
    note "configuring"
    cmake -S "$PS2RECOMP" -B "$build" -DCMAKE_BUILD_TYPE=Release \
          -DPS2X_VU1_PROGRAMS="$WORK/vu1" || die "cmake configure failed"
    cmake --build "$build" --target ps2EntryRunner -j "$(nproc)" || die "build failed"
    [ -x "$build/ps2xRuntime/ps2EntryRunner" ] || die "no runner produced"

    # The Windows build is cross-compiled, so it only exists on a machine set up
    # for it. Build it when it is configured; the else is not an error.
    win="$PS2RECOMP/build-win"
    if [ -f "$win/CMakeCache.txt" ]; then
        note "windows build"
        cmake --build "$win" --target ps2EntryRunner -j "$(nproc)" || die "windows build failed"
    else
        note "no build-win configured; skipping the Windows build"
    fi
    stage_end
fi

# ---------------------------------------------------------------- stage 5
# The VU1 microprograms come from the game itself, so the only honest way to
# compile them is to run it and record what it asks for, then compile that. Two
# passes close the gap (the second sees the layouts the first missed).
if [ "$FROM" -le 5 ] && [ "$TO" -ge 5 ]; then
    stage_begin "5. learning the VU1 microprograms"
    need_tool "$vu1_recomp"
    resolve_boot
    [ -n "$game_elf" ] && [ -f "$disc_dir/$game_elf" ] || die "no boot file at $disc_dir (run stage 1)"
    runner="$PS2RECOMP/build/ps2xRuntime/ps2EntryRunner"
    [ -x "$runner" ] || die "no runner to learn from (run stage 4)"
    learn="$WORK/vu1_learn"
    rm -rf "$learn"; mkdir -p "$learn"
    vu1_dir="$WORK/vu1"
    rm -rf "$vu1_dir"; mkdir -p "$vu1_dir"
    script="1000:cross:10;1200:cross:10;1400:cross:10;1600:cross:10;1800:cross:10;2000:cross:10;2200:cross:10"
    # Two passes, and the first pass's programs are compiled *between* them: with
    # them in place the second run gets further and asks for programs the first
    # never reached, which is what closes the gap (the handoff measures ~75% from
    # one pass and ~88% from two).
    for pass in 1 2; do
        note "pass $pass: recording what the game asks for"
        # Pass 1 must start from nothing, or it would only see what is already
        # compiled and never learn anything new.
        local_uncompiled=""
        [ "$pass" -eq 1 ] && local_uncompiled="PS2X_VU1_NO_COMPILED=1"
        ( cd "$learn" && env $local_uncompiled PS2X_HIDDEN_WINDOW=1 PS2X_PAD_SCRIPT="$script" \
              PS2X_VU1_LEARN="$learn" PS2X_NO_LAUNCH_MENU=1 \
              timeout 240 "$runner" "$disc_dir/$game_elf" >"$learn/pass$pass.log" 2>&1 ) || \
            note "pass $pass ended early (fine if it recorded something)"
        note "  $(find "$learn" -name 'vu1_learn_*.bin' | wc -l) capture(s) total"

        "$vu1_recomp" "$vu1_dir" "$learn"/vu1_learn_*.bin || die "vu1_recomp failed"
        [ -f "$vu1_dir/vu1_programs.cpp" ] || die "vu1_recomp wrote no registry"
        note "  $(ls "$vu1_dir"/vu1p_*.cpp 2>/dev/null | wc -l) program file(s)"

        # Re-configure as well as build: the CMake only adds the VU1 sources if
        # vu1_programs.cpp existed when it last configured.
        cmake -S "$PS2RECOMP" -B "$PS2RECOMP/build" -DCMAKE_BUILD_TYPE=Release \
              -DPS2X_VU1_PROGRAMS="$vu1_dir" >/dev/null || die "reconfigure failed"
        cmake --build "$PS2RECOMP/build" --target ps2EntryRunner -j "$(nproc)" || die "rebuild failed"
    done
    if [ "$(find "$learn" -name 'vu1_learn_*.bin' | wc -l)" -eq 0 ]; then
        die "no VU1 captures recorded; the game may not have reached gameplay"
    fi
    # Windows once, at the end: the passes only need the Linux runner.
    if [ -f "$PS2RECOMP/build-win/CMakeCache.txt" ]; then
        cmake -S "$PS2RECOMP" -B "$PS2RECOMP/build-win" -DPS2X_VU1_PROGRAMS="$vu1_dir" >/dev/null 2>&1 || true
        cmake --build "$PS2RECOMP/build-win" --target ps2EntryRunner -j "$(nproc)" || die "windows rebuild failed"
    fi
    stage_end
fi

# ---------------------------------------------------------------- stage 6
if [ "$FROM" -le 6 ] && [ "$TO" -ge 6 ]; then
    stage_begin "6. assembling the portable folder"
    make_portable="$ROOT/tools/make_portable_build.sh"
    [ -x "$make_portable" ] || die "missing $make_portable"
    "$make_portable" --linux --windows --out "$OUT" --game "$disc_dir" || die "assembly failed"
    note "game folder: $OUT"
    stage_end
fi

# ===========================================================================
if [ "$FAILED" -ne 0 ]; then
    printf '\n\033[31mFinished with failures.\033[0m\n'
    exit 1
fi
printf '\n\033[32mPipeline complete.\033[0m\n'
note "game: $OUT"
