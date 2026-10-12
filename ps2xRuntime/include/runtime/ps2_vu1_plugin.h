#pragma once

// Compiled VU1 programs loaded at startup from a shared library
// (vu1_programs.so / .dll next to the executable, or PS2X_VU1_PLUGIN), so
// programs learned on the player's machine (PS2X_VU1_LEARN + vu1_recomp)
// need no relink of the game. The plugin is built from the same headers as
// the game; the few interpreter methods compiled code calls are reached
// through this table instead of being linked from the executable.

#include "runtime/ps2_vu1.h"

#include <cstddef>
#include <cstdint>

struct Ps2xVu1HostApi
{
    uint32_t version;
    uint32_t interpreterSize; // sizeof(VU1Interpreter): plugin and game must agree
    void (*execLower)(VU1Interpreter &, uint32_t, uint8_t *, uint32_t, GS &, PS2Memory *, uint32_t);
    void (*normalizeFmacResult)(VU1Interpreter &, float *, uint8_t, uint8_t *);
    void (*commitReadyPipelines)(VU1Interpreter &);
    void (*advanceTo)(VU1Interpreter &, uint64_t);
    void (*progressXgkick)(VU1Interpreter &);
    void (*reportReservedInstruction)(VU1Interpreter &, bool, uint32_t);
};

constexpr uint32_t kPs2xVu1PluginVersion = 1;
// extern "C" entry: returns the plugin's program table (count in *count),
// or null when the host table doesn't match.
using Ps2xVu1PluginEntry = const VU1CompiledProgram *(*)(const Ps2xVu1HostApi *host, size_t *count);
#define PS2X_VU1_PLUGIN_ENTRY "ps2xVu1PluginPrograms"
