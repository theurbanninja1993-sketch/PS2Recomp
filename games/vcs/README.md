# GTA Vice City Stories (SLUS_215.90): recompiler inputs and tools

No game code or data lives here, only addresses and our own scripts.

- `config/vcs_clean.toml`: ps2recomp configuration (stub bindings, options).
  Its paths (`iso/extracted/SLUS_215.90`, `vcs-configs/vcs_clean.csv`,
  `vcs-port/generated_clean/`) are relative to the project root used during
  development; adjust them, or pass your own paths, when regenerating.
- `config/vcs_clean.csv`: the function map (name, start, end, size) built by
  `tools/rebuild_function_map.py` from a Ghidra export + the ELF.
- `config/vcs_entry_hints.txt`: extra entry points found by the map
  generator.
- `tools/`: map generator, MIPS/VU disassemblers, profiling helpers. Each
  script's usage line is at the top.

These are now the authoritative copies. Older working copies outside the repo
(`vcs-configs/`, `tools/` at the project root) are redundant and should be
deleted rather than kept in sync.
