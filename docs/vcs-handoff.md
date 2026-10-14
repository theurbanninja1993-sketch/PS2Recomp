# GTA Vice City Stories (SLUS_215.90) recomp: handoff notes

State as of 2026-10-12 (last commit `f3d6ca3`). Read this before changing
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
| `vcs-configs/vcs_clean.toml`, `vcs_clean.csv` | recompiler config + function map (copy in git: `PS2Recomp/games/vcs/config/`, keep in sync) |
| `tools/` | map generator (`rebuild_function_map.py`), disassemblers, profilers (copy in git: `PS2Recomp/games/vcs/tools/`) |
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

   - **Wine vs Proton (2026-10-13):** the system wine (11.19) has jittery frame
     pacing (`PS2X_PERF_LOG` over the fast-boot run: late=2..57/600,
     lostMs up to 554 ms) and the radio music audibly ran slow, while the
     emulator itself was never the bottleneck (eeMcyc constant, VU1 fully
     compiled, 0 interpreted calls). Running the same exe with the bundled
     wine from `proton-cachyos-native` (any Proton install works) fixed it:
     late=0..22/600, steady-state lostMs 0-29 ms --- the win is **ntsync**
     (the prefix boot printed "ntsync: up and running"; needs the ntsync
     kernel driver, checked with CachyOS's ntsync-enabled kernels).
       WINEPREFIX=/some/proton-pfx \
       /usr/share/steam/compatibilitytools.d/proton-cachyos-native/files/lib/wine/x86_64-unix/wine \
         ps2EntryRunner.exe
     The runner is OpenGL (GL 4.6 core passthrough to native Mesa), so DXVK
     does nothing here; Proton helps only through sync primitives.
     **Owner-confirmed (2026-10-13):** the radio music is at full speed
     under Proton, and all of yesterday's radio work (custom station,
     D-pad hold Radio Off toggle, unmute on station change) works on the
     Windows build.

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
     - **Plugin implemented:** `-DPS2X_VU1_PLUGIN_PROGRAMS=<vu1_recomp outdir>`
       builds `vu1_programs.so`/`.dll` next to the runner (target
       `ps2_vu1_plugin`). The game loads it at startup (or from
       `PS2X_VU1_PLUGIN`) and merges its programs with the built-in table.
       The plugin links nothing from the game: the 6 interpreter methods
       compiled code calls go through `Ps2xVu1HostApi`
       (`ps2_vu1_plugin.h`, shim in `src/lib/vu/ps2_vu1_plugin_shim.cpp`).
       Version + `sizeof(VU1Interpreter)` are checked, so the plugin must
       come from the same build/headers.
       Tested on Linux: a runner with no built-in programs + the 2-pass
       learned plugin ran ~90% of VU1 calls compiled, 0 late frames. The
       Windows DLL also loads under Wine (15 programs; the table is built on the
       first VU1 call, after the intro movie).
     - Next steps:
       - merging learned files across sessions;
       - possibly more runs per key;
       - the builder running 2-3 automated learning passes (the intro plus
         the fast-boot pad script) before handing over.
   - Suggested order:
     1. ISO reader + hash check, in a standalone library with a CLI test.
        **Done:** `ps2xBuilder/` (`ps2x_builder` library: `ps2x::IsoImage`,
        `isoBootFile`) and the `ps2x_iso info|list|hash|extract` CLI. It
        reads 2048-byte .iso and raw 2352-byte .bin images. A full
        extraction of `iso/vcs.iso` (341 files, 2.16 GB) is byte-identical
        to `iso/extracted` (empty dirs included). It cross-builds for Windows
        and runs under Wine. Known-good boot ELF hash (XXH3-128):
        `/SLUS_215.90 = 7f69b961cc427e3dcfd6582e25362be8`. The manifest
        should check this.
        **Requested: read from a physical DVD drive** (owner's idea, like
        PCSX2). PS2 DVDs are plain ISO9660 to a PC drive, so `IsoImage` can
        read the device instead of a file:
        - Linux: `/dev/sr*` works with fopen (2048-byte sectors).
        - Windows: use `CreateFileW(L"\\.\D:")` + `ReadFile` with
          sector-aligned offsets/lengths, not fopen.
        - Enumerate drives: Linux `/dev/sr*`; Windows `GetLogicalDrives` +
          `GetDriveTypeW == DRIVE_CDROM`.
        - Offer "extract straight from the disc" and "also save an .iso
          backup".
        - Blue CD-ROM PS2 games would need raw mode-2 reads (ioctl /
          `IOCTL_CDROM_RAW_READ`). Not needed for VCS/LCS (DVDs).
        - Real-drive findings (owner's external Blu-ray, Ultimate
          Spider-Man disc, 2026-10-12):
          - The drive needs ~8 s after inserting before reads work ("No
            medium found" until then), and can drop back to that state.
            The builder must poll and retry ("Waiting for disc...").
          - On a USB hub port it hit I/O errors and USB resets. A direct
            port fixed it. Show a hint when reads fail repeatedly.
          - It read at ~4 MB/s (a 4 GB disc takes ~17 min). Show progress +
            ETA, and extract straight from the disc rather than
            image-then-extract.
          - **Verified:** `ps2x_iso extract /dev/sr0` (559 s, 17 files,
            3.9 GB) is byte-identical to extracting a `dd` image of the same
            disc (dd: 532 s, 7.8 MB/s). Disc reading works on Linux. The
            Windows drive path (`\\.\D:`) is still untested on hardware.
        - **Implemented:** `ps2x::opticalDrives()` and
          `IsoImage::open("/dev/sr0" | "D:")`, plus `ps2x_iso drives`. The
          owner has an external Blu-ray drive: test with `ps2x_iso drives`,
          then `ps2x_iso info|hash /dev/sr0` (expect the boot hash above).
        - Regeneration check (2026-10-12): `ps2_recomp` with
          `games/vcs/config/vcs_clean.toml` (paths made absolute) takes ~2.4 s
          and reproduces `ps2xRuntime/src/runner` (10,225 files) and both
          headers byte-for-byte. The builder's recompile step is
          deterministic.
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
2. **Custom radio station + Radio Off: mostly DONE (owner-confirmed).** A
   `music` folder (MP3/OGG/FLAC/WAV) plays as the in-vehicle "Custom
   Soundtracks" station on the game's own silent slot, and "Radio Off" is now a
   runtime toggle (hold D-pad Down ~0.75 s, or `M`), not a station.
   - **Station variable FOUND (2026-10-12, owner's F8 session in a car):**
     u8 at **0x4E4755** (static data; same values at 0x4E475B and in heap
     copies 0x1D859BA / 0x1FB5834). Values: 0 Flash FM, 1 V Rock, 2
     Paradise, 3 VCPR, 4 VCFL Radio, 5 The Wave 103, 6 Fresh 105, 7
     Espantoso, 8 Emotion 98.3, **9 Radio Off**. 0x4E4756 lags one step at
     one point (probably the station actually playing vs the one selected).
   - **Custom station = slot 9 (e6689ee):** the game has no spare station
     value (the field clamps >= 9 to 9), so the custom stop *is* the game's
     own silent slot 9; every transition is one the game expects. Slot 9's
     GXT name is written per frame: **"Custom" while the radio is on,
     "Radio Off" while the runtime radio is off** (vcsKeepCustomName; GXT is
     UTF-16LE, located by scanning for the strings). Natural tuning into 9
     shows "Custom"; the toggle's re-tune shows "Radio Off".
   - **Radio Off toggle (af935e3, a13b850, 83a69dc, f3d6ca3):** D-pad Down
     (host button, both layouts) or `M` held ~0.75 s toggles the runtime
     radio off/on; the game's own station pop-up shows "Radio Off" (slot 9's
     name while the runtime radio is off). Off = push the
     game to slot 9 **once** (desired/playing/last bytes + the station
     field), then leave it alone - the game's own update holds the silent
     slot, so no station audio plays while the engine/atmosphere keep going,
     and our music folder pauses. A station press (field departs 9) switches
     the radio back on. A per-frame push was tried and removed: it fought
     the game, never silenced the current station, and dragged every station
     change back to the silent slot.
     - Verified under Wine/Proton (2026-10-13): station cycle to slot 9,
       `[music] playing ...`, cycle continues past it (headless); the owner
       then confirmed the D-pad hold toggle and the radio feature set on the
       controller under Proton, at full speed.
   - Key addresses: the radio singleton is at **0x4E41E0** (+0x575 playing
     [*overloaded, see below*], +0x576 desired, +0x577/+0x578 lock flags,
     +0x57B last, +0x564/+0x568 owning vehicle); the station field is +0x2BA
     of the entity from `FUN_1B5768(0x514A90)`; the pending change is
     +1/+9 at **0x48613C**; the cycle is **mod 10**, applied by
     `FUN_0018b8d8`, and `FUN_0018aad8` (hooked as vcsRadioUpdate) clamps
     values >= 9 to 9. 0x4E4755 is not a reliable station value (radio
     *modes* 9..30 live there when the slot is unset); match the field.
   - Slot 9 is the leftover PC/PSP **"custom tracks"** station, disabled on
     PS2: GXT `FEA_CSS` = "CUSTOM SOUNDTRACKS:"; the frontend audio page
     `FRONTEND/DEFAULT/AUDPG.PSP` defines the selector (RADIO_STATION_MI,
     states S0..S9) plus CUSTOM_TRACKS_EDIT_MI and CUSTOM_RADIO_ICONS_MI;
     the ELF option list has HOOK_USE_CUSTOM_TRACKS.
   - HUD: the station logo pop-up (HUD message `0xB2`) still shows slot 9's
     "Radio Off" logo on the custom stop (the owner's logo overlay was
     removed earlier at the owner's request).
   - `ps2_music_station.cpp` runs the folder (raylib streams; F10 skips a
     track, Mods tab has on/off, volume, shuffle, open folder, rescan).
   - Done (2026-10-13, commit 19ad92f): track skip on D-pad Right/Left single
     press (F10 = keyboard next) with real previous-track support; the music
     pauses with the game's own pause menu and resumes on the same track
     (pause flag **0x4CD2AC**, set 1/0 by the open/close handlers at
     0x2F4D64 / 0x2F4D70 on the START press, gates the station query); a
     Mods tab settings toggle for the Radio Off hold (`radio_off_enabled`);
     input device prompts (Xbox / Steam / PlayStation / keyboard wording in
     the Mods music help, now-playing overlay, settings footer).
   - Remaining (one item): the **frontend audio page selector** for the
     custom stop. `FRONTEND/DEFAULT/AUDPG.PSP` declares the radio selector as
     `<9c ad="10">` (= state count) with `S0..S9 = FEA_FM0..8 + FEA_NON`, and
     c81bf35 renames the 10th key `FEA_NON -> FEA_CSS` in the read buffer
     (CD.cpp `readRegisteredCdSectors`). Owner check (2026-10-13): the
     in-game audio menu still cycles only the nine real stations - no 10th
     entry, nothing "Radio Off"/custom. So the page's 10th state is not what
     the selector enumerates; the option list is built ELF-side.
     - New findings (2026-10-13): the hook registry is `FUN_003e2530`
       (0x3E2530, dispatcher over a jump table at vaddr **0x4C1D80**, 20
       entries, each case returning the hook's name string from the table at
       0x4C1BF8+): idx 12 = `HOOK_USE_CUSTOM_TRACKS` (name at 0x4C1CC8),
       idx 13 = `HOOK_RADIO_STATION`. Next probes: find who builds the audio
       page's option list (the option -> hook mapping) and whether the
       custom-tracks availability gate hides the 10th station entry; when
       found, the runtime can satisfy the gate (a music folder exists).
       ELF note: single PT_LOAD, file offset + 0x1000 = vaddr (text/data).
     - When tuned via the menu, it must set the station the way the L1/R1
       path does (slot 9 + `g_vcsCustomStation`), else the vehicle tunes to
       a normal station on entry.
3. **Texture-pack overhead - measured and located (2026-10-13).** Owner
   re-tested the airport: slight frame drops persist with the HD pack. A/B
   on the fast-boot drive (Linux native, GPU renderer, `PS2X_PERF_LOG`):
   the render thread runs **GsGpu ~44-57% with the pack vs ~24-30% without**
   (~+20 pts), which tips the airport into late frames. New
   `PS2X_TEXREPL_STATS` fields (added today) split it:
   - **Owner reported general gameplay slowdown again (2026-10-13 late)** -
     next work item after the menu/cutscene changes. The mirror cost below
     is the first suspect (it runs for every upload, pack or not); ask the
     owner to run `PS2X_PERF_LOG=1 PS2X_TEXREPL_STATS=1` for a fresh split,
     then implement the lazy mirror.
   - **mirrorMs ~590-950 ms per 5 s period (~16-19% of the render thread)**
     = `GSCpuBackend::UploadImage` decoding uploads into the CPU copy.
   - **hashMs ~16-30 ms** = texture/CLUT hashing - negligible.
   - Per-format bytes: **PSM 0x14 (T4, 4-bit palettized) is ~95%** of both
     mirrored and hashed bytes (~140 MB per period; T8 ~2-9 MB, C32 ~6-9 MB
     mirrored but never hashed). The hash reads the same bytes ~30x faster
     (block-linear 256-byte reads + XXH3 vs the per-pixel swizzle
     `WriteRowT` -> `PixelStorageTraits::Write` with nibble RMW).
   - Design direction (the doc's "lazy mirror keyed by upload content"):
     keep raw uploads in a side overlay (stream + transfer rect) and, for a
     texture region fully covered by one upload, **synthesize the 256-byte
     hash blocks from the stream via the fixed in-block column table**
     (`ColumnTable4` etc.) instead of maintaining `m_vram` per pixel;
     overlapping/mixed regions fall back to the current "CPU-stale =
     hash miss" policy. In-block layouts are the GS column interleaves (the
     column tables in `ps2_gs_memory.cpp`), NOT row-major - a naive block
     memcpy would corrupt data. Estimated 5-10x on the mirror cost.
   - Verification harness: `tools/gs_replay` frame diffs + comparing
     `[texrepl]` hit/hash counts before/after (identical hits = identical
     mirrored bytes).
4. Mods: file overrides (replace disc files from a mod folder). `.pnach`
   memory patches are now applied (be626b9): PCSX2 `patch=1,EE,ADDR,TYPE,VALUE`
   byte/short/word lines from enabled mod folders are written to EE RAM each
   frame. New setting "Apply .pnach memory patches" (`pnach_cheats`, default
   on; Mods tab). `extended` writes and conditional/pointer code words are
   skipped. Unit tests in `ps2xTest/src/ps2_mods_tests.cpp`.
5. Controllers: PlayStation button names in the menu, rumble.
6. VU1 compiled-program coverage (captures in heavy areas, e.g. the airport;
   regenerate `vcs-vu1` with `vu1_recomp vcs-vu1 <captures...>`).
7. **Player-selectable render API (Vulkan; D3D11 for Windows).** Owner's ask:
   a settings choice of rendering API --- Vulkan on Linux, Vulkan + DirectX on
   Windows. Feasible by design: the GS frontend is decoupled from
   rasterization behind the 42-line `GSRasterBackend` interface
   (`runtime/gs/gs_backend.h`), with two living backends (`GSCpuBackend`,
   the bit-exact reference, and `GSGpuBackend`, the GL 4.6 surfaceless
   renderer in `src/lib/gs/gpu/gs_gpu_renderer.cpp`, ~2.4k lines; selected
   with `PS2X_GS_GPU=1`, reading `ps2HostSettingsGpuRenderer()`). A Vulkan
   (or D3D11/12) renderer is a new backend in the same slot: same frontend,
   same local-memory model, same GS-semantics knowledge (blend
   `(A-B)*C>>7`, swizzles, DATE, FBMSK), same frame-diff verification via
   `tools/gs_replay` and `PS2X_GS_CAPTURE`.
   - **Effort (honest):** ~3-6 weeks each for Vulkan and ~the same for D3D11
     (D3D12 more), for someone fluent in Vulkan and this GS pipeline. The
     bulk is backend boilerplate (surfaceless/headless present, sync,
     barriers, descriptor/memory management, SPIR-V/HLSL re-expression of
     the uber shader) plus present-path wiring (today the GL backend reads
     the DISPLAY back into a `PresentationFrame`; the host window side is
     rlgl/GL and would need a shared-texture/DMA path for Vulkan/D3D).
   - **First cheap step:** stop gating the backend on the env var; add a
     "Renderer: CPU / OpenGL" dropdown in the F2 settings menu (the menu
     already has internal resolution), so the selector UX exists before the
     new backends do. Then each new backend is one more entry.
   - **What it buys:** on Linux native, OpenGL passthrough is already full
     speed --- Vulkan there is player choice and future-proofing, not
     perf. D3D11 pays off on real (non-Wine) Windows and under Wine via
     DXVK. Today's actual Windows pacing problem was sync, not graphics:
     fixed by running under Proton (ntsync), see the Build and run note.
   - Don't start until the radio feature, its owner test under Proton, and
     the remaining radio niceties are closed out.
8. **Button prompts: per-device replacement of the game's on-screen button
   glyphs. Runtime DONE + verified (2026-10-13); asset keys pending.**
   - Mechanism: the game's prompt glyphs are ordinary uploaded textures, so
     PCSX2-named replacement PNGs swap them, exactly like texture packs.
   - **`PS2X_TEXREPL_DUMP=<dir>`** (new): writes every texture the
     replacement matcher sees as `<tex0Hash>-<clutHash>-<bits>.png`, the
     PCSX2 naming - i.e. what a pack file for that texture must be called.
     RGBA expansion is best effort (CLUT/16-bit via the same rules as the
     hashing). With **`PS2X_TEXREPL_DUMP_ALL=1`** it also captures textures
     the matcher refuses (unsupported formats, region clamp): use it when
     hunting glyphs that may be drawn differently. If a glyph never appears
     even then, it is baked into other art (e.g. a shared sheet) rather than
     being a separate texture.
   - **Prompt set layer** (new): PCSX2-named PNGs in `prompts/<set>/` next to
     the executable (or `PS2X_PROMPTS_DIR`), sets `xbox`, `steam`,
     `keyboard`, `dualsense`, `dualshock`, `switch`. Loaded into the
     replacement index
     AFTER the mods on purpose (last file wins), so a user texture pack can
     never override the prompt glyphs - verified: 791 test textures, 511
     overriding the HD pack's keys.
   - **Setting `button_prompts`** (Mods tab): Auto (follows the active
     device via `ps2InputDeviceKind()`, per-frame re-resolve) / Xbox /
     Steam / Keyboard / PlayStation (DualShock + DualSense) / Switch / Off.
   - **Pipeline: `tools/make_prompt_packs.py`** (also in
     `games/vcs/tools/`): manifest (`packs.json`) driven compositor; renders
     the AL2009man prompt SVGs (honest queue: scrape
     https://github.com/AL2009man/Gamepad-Asset-Pack, submodule
     Gamepad-Prompt-Asset-Pack, MIT with attribution) via `rsvg-convert`,
     single-glyph or atlas-cell placements, Switch face buttons come from
     the Pro Controller FaceButton sheet (2x2, A=bottom), Steam falls back
     to the Xbox glyphs (Deck layout; swap art later), keyboard set renders
     keycap text labels (defaults per the port's default bindings; adjust
     per action).
   - **Identification progress (owner, 2026-10-13 evening):** the pause-screen
     controller diagram is `955f573d239ed3c1-ed1f7a64aaa8fafc-00002213.png`
     (256x256); face buttons + d-pad are the 16x16 tiles:
     `60a3a644...-00001113` = Cross, `d0d9f396...-00001113` = Triangle,
     `a1b7cc92...-00001113` = D-pad Up, `3c60f292...-00001113` = D-pad Right
     (owner-corrected), `784c33b4...-00001113` = D-pad Left and
     `47e8e408...-00001113` = D-pad Down (by mirror/elimination; owner to
     double-check). **Done already:** the five sets (xbox, steam, dualsense,
     dualshock, switch - PS split into DualSense/DualShock 4 at the owner's
     ask, detection by name) each carry the controller-page art (from the
     Controller Asset Pack bases) + the 2 faces + 4 d-pad arrows at 4x;
     verified: `[prompts] xbox: 7 textures (7 overriding mod packs)`.
     Still to find on the contact sheet: Circle, Square, Start/Select, and
     L1/R1 if they exist as glyphs (keyboard set = text keycaps via the
     script; no controller art for it - intentional).
   - **Controller portraits (owner picks, 2026-10-13 late):** all five use
     the pack's **full** layout exports (the `*_base.png` files are the
     "No Thumbstick and Triggers" exports, so the sticks looked missing):
     Xbox Series X **Black** (`Xbox Series X Controller Overlay.png`),
     Switch Pro (`Switch Pro Controller Overlay.png`), DualShock 4
     **Jet Black** (`DualShock 4 Controller V2 Model Overlay.png`),
     DualSense **White** (`DualSense Controller Layout (White-Black).png`),
     Steam Deck (`Steam Deck VSCView Overlay.png`). The Steam Deck
     templates paint the screen flat green (every variant, including the
     "Black" theme SVG), which looked wrong on the game's dark page - the
     `screen_black` option in `packs.json` flattens green-dominant pixels
     to black inside `compose_controller()`. The white halo outline was
     dropped after the owner test.
   - **Keyboard set (2026-10-13 late):** now ships the six menu glyph
     textures too (cross/triangle + the 4 d-pads), so menus stop showing
     the PS2 icons for keyboard users; the keyboard art is a keycap square
     (dark rounded square + gray rim + white label): W/LMB/S/E for the
     face buttons (port defaults) and the arrow keys for the d-pad (the
     Menu directions are bound to the arrow keys). The font-atlas cells
     use the same keycaps. `keyboard/` = 6 textures + sheet + previews +
     manifest; no controller diagram (intentional).
   - **Button-name text follows the live mappings (2026-10-13 late).** The
     GXT button words in menus (the map screen's "L1 and R1 buttons",
     Empire menus' "CROSS - ...", the controls lists) now resolve at
     display time: gamepad sets ask `ps2controls::hostButtonForPs2(ps2)` -
     the host button that currently reaches that PS2 button in the current
     context, preferring Modern/custom remaps over the classic identity
     (so "Modern" shows RT for fire, not B); keyboard sets use the action
     that presses it (`vcsKeyboardAction`: menu actions first - the map
     options line reads ENTER - then the on-foot / in-vehicle action of the
     pad context). Text is re-applied whenever the prompt set, a binding,
     the gamepad map/layout or the pad context changes
     (`vcsMappingSignature`; the pad backend publishes
     `ps2controls::setCurrentContext`). Keyboard menu strings that embed
     the symbol tokens ("Hold ~X~ to display the map options.") are
     rewritten to the bound key name with a fit-check ("Hold ENTER to
     display map options."); console sets keep the tokens because their
     glyphs already follow the set. Limitation: the *glyph* textures stay
     positional (baked per set), so with Modern/custom layouts an icon can
     still show the classic button - runtime art generation would be the
     follow-up. `PS2X_PROMPT_TRACE=1` logs the site counts.
   - **Menu layout decoupled from gameplay (2026-10-13 late).** The game's
     pause menu / map is a third pad context (`ps2controls::Menus`), probed
     from the game's own pause flag (`0x004CD2AC`, the same flag the Custom
     Soundtracks query reads) and wired like the vehicle probe. The menu map
     stays classic PS2 in every preset, so Modern / custom remaps never
     change how the menus navigate, and menu-only edits never leak into
     gameplay. The Gamepad buttons table gained the **In menus** column; the
     runtime prompt art resolves the keyboard cells to the menu actions
     (Enter / Backspace / arrows) while a menu is open, and the controller
     art falls back to the classic glyphs (identical to the shipped art).
     **B is the back button** (modern convention): the game's own menus read
     Triangle as back, so the menu map sends Triangle from B, while X and Y
     keep the base game's Square (target) and Triangle (legend) roles on the
     map. Circle is no longer sent in menus (nothing needs it so far); the
     prompt text/art follow via hostButtonForPs2. The keyboard's Backspace
     already sends Triangle. Keyboard in menus: WASD presses the D-pad
     (discrete steps, like the arrow keys - no analog stick), the mouse wheel
     zooms the map (up = R1 in, down = L1 out) instead of cycling weapons,
     and Q / RMB stay the L1/R1 map zoom keys. Each wheel notch holds the
     zoom button ~6 frames and fast scrolling queues steps, so the wheel
     keeps up; the pad's L1/R1 feel is unchanged. The Button prompts
     dropdown sits at the top of the Controls tab, under the controller
     selection.
     - **The keyboard's menu keys are their own set** (Controls -> Menu
       keys): accept = Enter, back = Backspace, the directions = arrows +
       WASD, Map: target = Space, and Skip cutscene = Enter. Only these work
       in the game's menus - the gameplay bindings no longer leak in (and
       the menu keys never fire in gameplay), except Skip cutscene, which
       sends Cross while the cutscene probe reports a cutscene.
     - **Cutscene flag (2026-10-13 late).** F8 RAM dumps in two cutscenes
       (B/C) and two gameplay states (A/D) diff to 33 word-level boolean
       candidates; `vcsInCutscene` reads **0x006F4D14** (heavily referenced
       by the game code, 15 functions across modules). The pad logs
       `[pad] cutscene=0/1` on every change so the flag can be verified
       live; `PS2X_CUTSCENE_FLAG=<hex>` overrides the address for testing.
       Other candidates if it is wrong: 0x6F4DA0, 0x6F4D64, 0x6F4D10,
       0x74D3F0, 0x486E28, 0x4874F4, 0x4CDA40, 0x711E94.
       The **FMV movies** (opening logos / credits) are not covered by the
       flag: `isMpegMovieActive` (MPEG stubs) remembers the presented frame
       of the last picture served and reports a movie for 45 frames after
       it; the probe returns true for either. Owner-confirmed the in-engine
       skip works; the movie path is new.
     - **The map zoom on the keyboard is the mouse wheel alone**: the wheel
       prompt replaces the L1/R1 icon pair (one "MOUSE WHEEL" keycap, the
       second icon blank; the map's "L1 and R1 buttons" hint reads "Mouse
       Wheel"), and Q / RMB no longer press L1/R1.
   - **Map-screen icons covered (2026-10-13 late).** The map legend draws
     its button icons as their own textures; miss logging + single-key
     colour tests on the owner's capture 6209 found them:
     `c075546801b74bf1-6b7c470bee19b607-00001113` = square (Target),
     `1c274bfc4808cd11-21fc390fe03ad8c0-00001113` = L1 and
     `23ece69c64b71407-f6ce098a91a7ac7b-00001113` = R1 (the Zoom pair).
     xbox/steam/switch get face-button discs (X / Y) and shoulder pills
     (LB/RB, L/R); the keyboard set gets keycaps with the keys that work on
     the map (SPACE = Jump sends Square, Q = Interact sends L1, RMB = Aim
     sends R1). The PlayStation sets use the pack's own face art so the
     icons match the tutorial HUD cells (DualSense = the white-style
     square, DualShock = the Color square); both use the same dark L1/R1
     pill (dark fill + light print, like the black bumpers on the real
     controllers). The cross/d-pad legend icons were already covered.
   - **Runtime prompt art (2026-10-13, both milestones).**
     `ps2_prompt_art.cpp` regenerates the prompt textures from the live
     bindings and writes them to
     `<config>/ps2recomp/prompts_rt/<set>/`; the prompt loader reads that
     folder after the shipped `prompts/<set>/` files (last wins).
     - Keyboard: keycaps labelled with the bound keys (same action labels
       as the shipped art, arrow glyphs included). Long labels ("ENTER",
       "BKSP", "RMB") draw as wide keys - like a real Enter key - with the
       text across the full tile, instead of squeezed into a square; the
       shipped keycap generator (`tools/prompt_glyphs.py`) matches.
     - Controller sets: the glyph of the host button that currently reaches
       each PS2 button (`hostButtonForPs2`), so Classic / Modern / Custom
       and the on-foot vs in-vehicle contexts show in the art too (Modern
       on foot: the Circle cell becomes an RT pill, L1 a B disc, ...).
       Styles: Xbox/Steam/Switch letter discs (brand rings, official
       colours), PlayStation symbol discs (DualSense white style with gray
       symbols, DualShock Color style with the pack's sampled colours),
       pills for shoulders / sticks / menu buttons, arrows for host D-pad
       inputs.
     - Rewrites are debounced (~45 stable frames) and keyed by an
       effective-content signature, so Classic layouts hash the same in
       both pad contexts (riding a vehicle rewrites nothing) while Modern /
       Custom rewrite when the context changes; stale files are removed so
       dropped mappings fall back to the shipped art.
     - Validated with a standalone hidden-window raylib harness for all six
       sets plus a Modern-layout case; both executables relinked.
   - **Tutorial cells filled (2026-10-13 late).** The HUD tutorials draw the
     button cells small (a ~32px atlas block at about 0.7x on screen), so
     the flat shoulder pill (44% of the cell height) left the L2/R2 hints
     (shoulders bound to face buttons, e.g. Modern driving: LT/RT) only a
     few pixels tall. Pills drawn into the sheet cells now fill 80% of the
     cell; the map icons keep the flat shoulder look. Verified by
     replaying the owner's two vehicle tutorials (1996 = "Press X to
     accelerate", 2520 = "Press Square to use the vehicle's brakes") with
     the owner's settings (DualSense + Modern): the prompts now read as
     large L2/R2 pills.
   - **Menu flips rebuild only the prompt layers (2026-10-13 late).** The
     runtime art rewrites on every pad-context change (menu open, vehicle
     entry/exit with remapped layouts) and each rewrite ran a full
     `ps2ModsRefresh`, rescanning every mod folder (~1 s with a big
     disabled pack). The mods' replacement files are now cached in
     `g_modsIndex` and `ps2IndexWithPrompts()` rebuilds the prompt set +
     runtime art on top of it; the full scan runs only at startup, on
     Rescan and on mod toggles.
   - **Prompt flips are now same-frame (2026-10-13 late).** The wrong keys
     for a moment when opening the map (owner: "E for back" then Backspace)
     had three sources: the menu debounce still waited two frames (now one),
     the replacement textures reloaded through the async worker (files up to
     256 KB - all the prompt art - now load inline on the render thread at
     the first draw that needs them), and the pause flag is set after the
     port reads the pad, so the art was always one frame late. A fresh Start
     press now predicts the toggle for the art only
     (`ps2PromptArtSetContext`; the pad mapping keeps the real context until
     the flag is seen) and the prompt layers rebuild at the pad read, early
     in the frame, so the new art lands before that frame's draw. Big pack
     textures still use the worker. This also removed the reload latency on
     vehicle entry/exit (relevant to the slowdown report).
   - Also done: the dropdown now has **"Original (PS2 art / texture packs)"**
     (= Off, shows the game's own art and any texture-pack art), it lives in
     the **Controls tab** (next to the gamepad layouts) with a glyph preview,
     and the prompt sets load independently of the texture packs - the old
     "Use texture packs" master toggle is gone (per-pack toggles only; the
     master switch silently disabled the prompt sets).
   - Layout-adaptive labels (Classic/Modern/custom bindings changing the
     controller-page text) are still open: the page's labels are page text,
     not part of the diagram texture, so that needs a runtime-generated
     diagram (compose per (device, layout) at index build or procedurally) -
     design noted, not started.
   - **Font-embedded button symbols solved (2026-10-13).** The tutorial prompts
     (Circle = melee, Square = brake/reverse, ...) are FONT GLYPHS in the
     256x256 T8 UI atlas at tbp0=13592 (key
     `3ece7b57667e958b-f663bcea3bca5023-00002213.png`) - found after the F9
     captures proved every prompt screen samples only that sheet family and
     the replayed captures surfaced the full 256px window. Cell coordinates
     (owner-located, shape-verified): square (0,230), cross (24,230), circle
     (46,230), triangle (212,206), ~22px each.
     `tools/make_font_sheet.py` redraws just those cells per set (button art
     from the AL2009man pack; keyboard = the port's default bindings,
     W/LMB/S/E) and writes the whole atlas under
     `prompts/<set>/<atlas key>.png`; every other pixel is byte-identical so
     the rest of the text is untouched. The v2 note about shared-sheet
     replacement now applies only for extending this (e.g. adding L1/R1 or
     Start/Select symbols in the font if they exist).
   - **GS-scale alpha (fixed 2026-10-13 late) - the "see-through prompts"
     root cause.** Replacement textures are sampled through the GS blend
     emulation, whose factor is `k = alpha/128` (`0x80` = opaque; the dumper
     writes that scale and the DDS converter uses it too). Art authored in
     `0..255` alpha makes `k ~ 2`: the source is over-brightened and the
     destination factor goes negative, so prompt glyphs rendered washed out
     and "showing the game world through" on the HUD (menus hid it; white
     art saturated worst). `prompt_glyphs.to_gs_alpha()` now scales every
     generated replacement (glyph tiles, atlas cells, controller art) to
     `0..128`; `preview_*.png` keep `0..255` for the settings UI and the
     controller preview is its own full-alpha file (`preview_controller.png`,
     referenced from `manifest.txt`). Art is dark again (the approved look):
     Xbox letters in official colours, Switch/Steam white + brand ring,
     DualShock = the pack's Color SVGs (coloured symbols, like the real
     DS4), DualSense = the pack's **white style** (white disc, gray
     `#808080` symbol - the "Style - Transparent" layer group; the SVG's
     three style groups are selected explicitly in `_svg_with_style()`),
     keyboard keycaps; controller portraits are the plain dark art (the
     white halo was removed after the owner test). Verified by replaying
     capture 1661: the cross prompt renders exactly as authored
     (`(28,28,32)` disc, green `A`); DualSense renders as the white button.
     Backlog: the "Black" style (`Style - Outline (Black)`: black disc,
     white symbol) is available as an alternative if a prompt set ever
     needs a dark-button look.
   - The owner has the self-contained contact sheet at
     `texdump/_contact_sheet.html` (open with the full file:// path).

9. **Xbox One controller connected but unresponsive (owner report,
   2026-10-13 late).** The DualSense works; the Xbox One pad is seen by the
   game but no input registers. Leads:
   - The pad selection logs `[pad] using controller <i>: <name> (connected: ...)`
     on every change - check which slot is actually active. The settings'
     `controller=` pin (`ps2SetPreferredGamepad`) wins while that exact name
     is connected, so a still-connected DualSense keeps the Xbox pad ignored.
   - If the Xbox pad IS the active slot and dead: the buttons are likely
     unmapped. The port reads pads through raylib (GLFW + SDL's
     gamecontrollerdb); a joystick that is present but has no mapping reads
     as all-zero. raylib's `SetGamepadMappings` can load an SDL mapping
     string at runtime - we need the device's GUID (raymath doesn't expose
     it; read `/proc/bus/input/devices` or log `glfwGetJoystickGUID`).
   - Also check the transport: Bluetooth Xbox One pads need `xpadneo` (the
     stock `xpad` only handles USB); an unmapped hid-generic node would
     explain "seen but dead".

10. **Steam Controller (2026, Puck dongle) / Steam Input testing.** Goal:
    exercise the Steam prompt set + the Steam device path without the Steam
    client. The owner has the **2026 Steam Controller** (Puck dongle
    `28de:1304`):
    - `benashby/steam-puck-bridge` - the Linux userspace driver for that
      dongle: disables lizard mode and exposes a virtual Xbox 360 gamepad via
      uinput, explicitly without Steam running. Main option for this pad.
    - Name the virtual pad "Steam Controller" so `ps2InputDeviceKind()`
      matches "steam" and Auto picks the Steam prompt set.
    - The older tools target the 2015 pad only - fallback references:
      `rodrigorc/inputmap` (virtual devices, by the hid-steam author) and
      `kozec/sc-controller` (GUI, own config format); the kernel `hid-steam`
      driver is likewise the 2015 driver.
    - Note: real Steam Input (action sets/layouts) still needs the Steam
      client; the bridge just makes the device usable as a normal gamepad,
      which is what the port needs for its Steam prompt set.

## Third-party assets (release-page credits)

Both packs are by **AL2009man**, MIT-licensed, (c) 2024 Al. Lopez:

- **Gamepad Asset Pack** - https://github.com/AL2009man/Gamepad-Asset-Pack
  (the assets were originally created for
  [VSCView](https://github.com/Nielk1/VSCView/)). Used for the
  controller-page portrait art: `Controller Asset Pack/` - Xbox Series X,
  DualSense, DualShock 4, Switch Pro and Steam Deck images.
- **Gamepad Prompt Asset Pack** -
  https://github.com/AL2009man/Gamepad-Prompt-Asset-Pack. Used for the
  button-prompt glyphs: PlayStation face glyphs (Colour + white styles),
  Xbox face buttons, Nintendo glyphs, D-pad arrows.
- Minor: the keyboard keycap labels are rendered with **DejaVu Sans Bold**
  (Bitstream Vera / DejaVu licence) and the SVGs with `rsvg-convert`
  (LGPL); neither ships in the release, only the baked PNGs.
- Licence note: the Prompt Asset Pack README carves out platform "Guide"
  buttons from its commercial-use grant; our prompt configs never use
  guide/home glyphs (face buttons, d-pads, start/select only). The
  controller portraits come from the Controller Asset Pack (plain MIT).

## Where past fixes live

Read the `vcs-recomp-progress` memory notes (owner's Claude memory) or `git log`
before re-investigating a bug. Most VCS-specific behaviour is hooked in
`applyGtaVcsUs()` at the end of `game_overrides.cpp`, and movie playback in
`Kernel/Stubs/MPEG.cpp`.
