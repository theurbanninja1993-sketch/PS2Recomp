// Linked into the VU1 program plugin (not the game): the interpreter methods
// compiled programs call, forwarded to the game through its host table.
#include "runtime/ps2_vu1_plugin.h"

#if defined(_WIN32)
#define PS2X_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define PS2X_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

extern const VU1CompiledProgram g_ps2Vu1CompiledPrograms[];
extern const size_t g_ps2Vu1CompiledProgramCount;

namespace
{
    const Ps2xVu1HostApi *g_host = nullptr;
}

void VU1Interpreter::execLower(uint32_t instr, uint8_t *vuData, uint32_t dataSize, GS &gs, PS2Memory *memory, uint32_t upperInstr)
{
    g_host->execLower(*this, instr, vuData, dataSize, gs, memory, upperInstr);
}

void VU1Interpreter::normalizeFmacResult(float *result, uint8_t dest, uint8_t laneFlags[4])
{
    g_host->normalizeFmacResult(*this, result, dest, laneFlags);
}

void VU1Interpreter::commitReadyPipelines()
{
    g_host->commitReadyPipelines(*this);
}

void VU1Interpreter::advanceTo(uint64_t targetCycle)
{
    g_host->advanceTo(*this, targetCycle);
}

void VU1Interpreter::progressXgkick()
{
    g_host->progressXgkick(*this);
}

void VU1Interpreter::reportReservedInstruction(bool upper, uint32_t instruction)
{
    g_host->reportReservedInstruction(*this, upper, instruction);
}

PS2X_PLUGIN_EXPORT const VU1CompiledProgram *ps2xVu1PluginPrograms(const Ps2xVu1HostApi *host, size_t *count)
{
    *count = 0;
    if (!host || host->version != kPs2xVu1PluginVersion || host->interpreterSize != sizeof(VU1Interpreter))
        return nullptr;
    g_host = host;
    *count = g_ps2Vu1CompiledProgramCount;
    return g_ps2Vu1CompiledPrograms;
}
