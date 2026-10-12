# GTA Vice City Stories (SLUS_215.90) recomp: handoff notes

State as of 2026-10-12 (last commit `f3e4ec0`). Read this before changing
anything. A reviewer will check work against it.

## Ground rules

- **Never `git push`** without the owner's explicit OK. Commit locally on
  `main` in `PS2Recomp/`, one logical change per commit, with a message that
  says what changed, why, and how it was verified.
- **Never commit `ps2xRuntime/src/runner/register_functions.cpp`.** It's the
  generated function registry (240k lines), and the tracked file is upstream's
  stub. Stage files explicitly; never `git add -A`.
- Never commit game code or data: generated `FUN_*.cpp`, `vcs-vu1/`, VU1/GS
  captures, ISO contents, texture packs.
- Test runs must use a **visible window** (no `PS2X_HIDDEN_WINDOW`). The owner
  watches them. Run one game instance at a time, and don't leave runs behind.
- Don't touch the owner's settings: use `XDG_CONFIG_HOME=<tmp dir>` for test
  runs. `PS2X_EXIT_AFTER_FRAME=N` skips the launch menu and quits at frame N.
- Don't ask the owner what to do next. Pick the most important open item.

## Layout (project root `/mnt/2TBSSD/projects/Vice City Stories Recomp`)

| Path | What |
|---|---|
| `PS2Recomp/` | git repo (fork of ran-j/PS2Recomp, GPLv3): recompiler + runtime |
| `PS2Recomp/ps2xRuntime/src/lib/game_overrides.cpp` | VCS-specific fixes and features (bottom of the file) |
| `iso/extracted/` | the disc (boot ELF `SLUS_215.90`) |
| `vcs-configs/vcs_clean.toml`, `vcs_clean.csv` | recompiler config + function map. **Not in git: back these up** |
| `tools/` | map generator (`rebuild_function_map.py`), disassemblers, profilers. **Not in git** |
| `vcs-vu1/` | AOT-compiled VU1 microprograms (generated, linked when configured) |
| `scratch/` | captures, logs, screenshots, `ext/` (llvm-mingw, PCSX2 source) |
| `GTA Vice City Stories Texture Pack/` | owner's HD pack, symlinked as `build/ps2xRuntime/mods/HD Textures for VCS` |

## Build and run

Linux (main dev build; PGO profile in `build/pgo`):

    cd PS2Recomp/build && cmake --build . --target ps2EntryRunner -j 14
    cd scratch/shots && env PS2X_GS_GPU=1 ../../PS2Recomp/build/ps2xRuntime/ps2EntryRunner ../../iso/extracted/SLUS_215.90

Windows (cross-compiled, tested under Wine 11 / Proton):

    cmake -B build-win -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/llvm-mingw-x86_64.cmake \
      -DLLVM_MINGW_ROOT=<root>/scratch/ext/llvm-mingw-dl/llvm-mingw-20241217-ucrt-ubuntu-20.04-x86_64 \
      -DPS2X_VU1_PROGRAMS=<root>/vcs-vu1
    make -C build-win -j14 ps2EntryRunner
    wine build-win/ps2xRuntime/ps2EntryRunner.exe   # boots ./game/SLUS_215.90 next to the exe

Regenerating game code (only needed after recompiler/map changes, about 25 min
full build): see the `vcs-recomp-function-map` notes. `ps2recomp
vcs-configs/vcs_clean.toml` writes `vcs-port/generated_clean/`, which is copied
into `ps2xRuntime/src/runner/` + `include/`.

## Testing recipes

- **Fast boot to gameplay** (skips intro/cutscene, gameplay at ~frame 2400, the
  bike is next to the player; Triangle at ~2800 to mount):
  `PS2X_PAD_SCRIPT="1000:cross:10;1200:cross:10;1400:cross:10;1600:cross:10;1800:cross:10;2000:cross:10;2200:cross:10"`.
  Don't use it when testing the intro, cutscenes or logos.
- **Intro cutscene ~frames 6000-9000** (no script) is a good streaming-heavy
  scene for renderer perf.
- **Screenshots:** `PS2X_SCREENSHOT_EVERY=N` (PNG in cwd). Check them, don't
  assume.
- **Per-thread load:** `PS2X_PERF_LOG=<file>` (every 600 frames: late frames,
  audio, % per thread, VU1 compiled/interpreted counts).
- **Profiling:** no `perf` on this box. Run with `PS2X_ALLOW_PTRACE=1`, then
  `eu-stack -p <tid>` repeatedly (it dumps all threads; split on `TID n:`).
  The `ScopeTimer` numbers in the GPU renderer proved unreliable. Trust
  sampled stacks and A/B runs instead.
- **Pitfall:** `pgrep -f` / `pkill -f` match their own shell command. Use
  `pgrep -x ps2EntryRunner`.
- More env vars: `PS2X_PAD_DEBUG`, `PS2X_TEXREPL_STATS`,
  `PS2X_TEXREPL_LOG_MISSES`, `PS2X_MPEG_END_TRACE`, `PS2X_MPEG_AUDIO_TRACE`,
  `PS2X_GS_SCALE`, `PS2X_OPEN_SETTINGS`, `PS2X_MODS_DIR`,
  `PS2X_DUMP_RAM_FRAME`, `PS2X_WATCH_WRITE`, F9 = GS capture, F7 = log marker.

## What works (owner-confirmed)

Full speed in gameplay (GPU renderer + compiled VU1), audio, intro/logo
movies with full audio, cutscene lip sync, F2 settings menu (display,
resolution scale 1-4x, aspect, FOV, draw distance, controls/remapping,
classic/modern gamepad layouts, mods), first-launch menu, controller support
(Select+Start opens the menu; last-used controller, or one chosen in the
menu), mouse look on foot and in vehicles (wall/ground collision), PCSX2
texture packs from `mods/`, Windows build under Wine.

## Open work, in rough priority

1. **Release packaging (source-only).** A prebuilt exe would distribute
   Rockstar's (translated) code, and Take-Two DMCA'd re3/reVC. Plan: publish
   the runtime + VCS overrides + configs + scripts; users point a script at
   their own disc, which extracts it, runs `ps2recomp` with
   `vcs_clean.toml/.csv` and builds (Linux + Windows). Blocker: `vcs-vu1`
   comes from runtime captures (game microcode), so users need a way to
   generate it themselves (e.g. extract VU1 microprograms statically from the
   ELF for `vu1_recomp`). Without it VU1 is interpreted, about a third of the
   speed. Owner decides on publishing.

   **The owner wants a guided builder app** (no commands for players):
   - Wizard flow: pick disc image (.iso, later .chd), verify it's
     SLUS_215.90 by hash, pick an install folder, Build (progress bar + ETA),
     done. The result is a ready folder (exe, FFmpeg DLLs, `mods/`) plus a
     shortcut.
   - UI with raylib + ImGui like the game's settings menu. Linux + Windows.
   - Bundled pieces:
     - an ISO9660 reader (in-app; see `tools/bin2iso.py`);
     - the `ps2recomp` binary + `vcs_clean.toml/.csv` (addresses only);
     - a C++ compiler: llvm-mingw Windows-hosted build on Windows, a portable
       clang (or Zig's bundled clang) on Linux;
     - the runtime precompiled as a static library, so only the generated
       game code is compiled on the player's machine (~10-30 min).
   - VU1: generate `vcs-vu1` from the player's ELF (needs the static
     microcode extraction above). Feasibility check (2026-10-12): of 14
     distinct program starts in `scratch/vu1_capture.bin` (record = 16-byte
     header 'CVU1' + VU1State (664 B) + 16 KB code + 16 KB data + VU1State +
     16 KB data), 13 appear verbatim in the ELF (e.g. 0x1ad2b0, 0x2acaa0,
     0x382470). Check what `ps2Vu1ProgramKey` hashes: the program alone, or
     the whole micro-memory layout. If the latter, the extractor must also
     reproduce which programs are co-resident (follow the VIF MPG uploads in
     the game's DMA chains). Find the 14th (runtime-patched, or loaded from
     another disc file).

     Findings, later on 2026-10-12:
     - The key (`ps2_vu1_programs.cpp`) covers only the program itself: its
       startPC plus the address + words of every pair reachable from it
       (JR/JALR not followed). Co-resident programs don't matter.
     - The ELF has no inline VIF MPG headers before these blobs. RenderWare
       builds the uploads at runtime from pointer tables, e.g. at file offset
       0x382888: pointers 0x2ac240, 0x2ac228, ... stepping 0x18. Fully
       static extraction therefore means decoding RW's PS2 pipeline
       structures to recover load address + entry points per blob.
     - **Recommended alternative: self-learning VU1.** When the game meets
       an uncompiled program, it writes {code image, startPC, traced JR
       targets} to `vu1_cache/`. The builder (or an "optimize" step at next
       launch, with the bundled compiler) runs `vu1_recomp` on the cache into
       a plugin DLL/.so that the game loads at startup (registry merged with
       the built-in table). The builder can seed it with an automated run:
       intro + PS2X_PAD_SCRIPT fast boot + some frames of gameplay. This
       works for any game, ships no game code, and coverage grows as the
       player plays.
     - **Implemented (e6027eb): `PS2X_VU1_LEARN=<dir>`** records up to 4 MSCAL
       runs per program that has no compiled version
       (`<dir>/vu1_learn_<time>.bin`, capture format). Then
       `vu1_recomp <outdir> <dir>/*.bin`, and build with
       `-DPS2X_VU1_PROGRAMS=<outdir>`.
       Measured on the fast-boot run, starting from nothing
       (`PS2X_VU1_NO_COMPILED=1` for pass 1):

       | Learned set | Recorded | VU1 calls compiled | Late frames / 600 |
       |---|---|---|---|
       | Pass 1 | 13 programs, 2.6 MB | ~75% | – |
       | Pass 1 + pass 2 (built with the pass-1 set) | 15 variants | ~88% | 0-2 |
       | Full `vcs-vu1` (hand-made captures) | – | 100% | 0 |

       More passes keep closing the gap. Programs whose JR targets depend on
       co-resident code need runs under each layout.
     - Next steps:
       - a plugin library loaded at startup instead of relinking the exe;
       - merging learned files across sessions;
       - possibly more runs per key;
       - the builder running 2-3 automated learning passes (the intro plus
         the fast-boot pad script) before handing over.
   - Suggested order:
     1. ISO reader + hash check, in a standalone library with a CLI test;
     2. the wizard UI;
     3. drive `ps2recomp` + the compile with progress parsing;
     4. packaging the bundle per OS.

   Test on a clean Wine prefix / Proton, and on real Windows before
   release.

   **Future: "GTA Stories Collection" launcher.** The owner wants the
   builder to grow into one frontend that builds and launches both VCS and
   LCS (an LCS port is in progress separately). Design the builder around a
   **per-game manifest** from the start:
   - disc ID + hash;
   - recompiler TOML + function map;
   - the game's runtime overrides;
   - display name and art;
   - the default boot ELF.

   UI: a library screen showing each game as Build (no disc yet) or Play,
   with per-game mods and settings. Both games use the PS2Recomp runtime;
   keep game-specific code behind `PS2_REGISTER_GAME_OVERRIDE`.
2. **Custom radio station:** a `music` folder (MP3/OGG/FLAC/WAV) as an extra
   in-vehicle station, like the GTA III/VC PC "MP3 Player". Needs the VCS radio
   station selection + audio stream path found and decoded PCM injected.
3. **Texture-pack overhead:** the CPU mirror of texture uploads
   (`GSCpuBackend::UploadImage` → `GSMem::WriteRow`) still costs the render
   thread ~+15 pts in streaming scenes. Next lever: a lazy mirror keyed by
   upload content hash. Owner to re-test the airport area first.
4. Mods: `.pnach` patches (counted by the mod loader, not applied), file
   overrides (replace disc files from a mod folder).
5. Controllers: PlayStation button names in the menu, rumble.
6. VU1 compiled-program coverage (captures in heavy areas, e.g. the airport;
   regenerate `vcs-vu1` with `vu1_recomp vcs-vu1 <captures...>`).

## Where past fixes live

Read the `vcs-recomp-progress` memory notes (owner's Claude memory) or `git log`
before re-investigating a bug. Most VCS-specific behaviour is hooked in
`applyGtaVcsUs()` at the end of `game_overrides.cpp`, and movie playback in
`Kernel/Stubs/MPEG.cpp`.
