#include "runtime/ps2_vu1.h"
#include "runtime/ps2_memory.h"
#include "ps2_vu1_jit.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <unordered_map>

// Generated programs (tools/vu1_recomp.cpp) define these; without them no
// program is compiled.
extern const VU1CompiledProgram g_ps2Vu1CompiledPrograms[] __attribute__((weak));
extern const size_t g_ps2Vu1CompiledProgramCount __attribute__((weak));

namespace
{
    uint32_t loadWord(const uint8_t *code, uint32_t offset)
    {
        uint32_t word = 0;
        std::memcpy(&word, code + offset, sizeof(word));
        return word;
    }

    const std::unordered_multimap<uint64_t, const VU1CompiledProgram *> &compiledPrograms()
    {
        static const std::unordered_multimap<uint64_t, const VU1CompiledProgram *> s_programs = []()
        {
            std::unordered_multimap<uint64_t, const VU1CompiledProgram *> programs;
            if (&g_ps2Vu1CompiledProgramCount != nullptr && &g_ps2Vu1CompiledPrograms != nullptr)
                for (size_t i = 0; i < g_ps2Vu1CompiledProgramCount; ++i)
                    programs.emplace(g_ps2Vu1CompiledPrograms[i].key, &g_ps2Vu1CompiledPrograms[i]);
            if (std::getenv("PS2X_VU1_NO_COMPILED"))
                programs.clear();
            return programs;
        }();
        return s_programs;
    }
}

void (*g_ps2Vu1JumpTraceHook)(uint32_t target) = nullptr;

void ps2Vu1ProgramPairs(const uint8_t *code, uint32_t codeSize, uint32_t startPC, std::vector<uint32_t> &normal,
                        std::vector<uint32_t> &delay, const uint32_t *roots, uint32_t rootCount)
{
    normal.clear();
    delay.clear();
    const uint32_t pairs = codeSize / 8u;
    std::vector<uint8_t> seen(pairs, 0u); // bit 0 normal, bit 1 delay slot
    std::vector<uint32_t> work;
    auto wrap = [&](uint32_t pc)
    { return pc >= codeSize ? 0u : pc; };
    auto addNormal = [&](uint32_t pc)
    {
        pc = wrap(pc) & ~7u;
        if (!(seen[pc / 8u] & 1u))
        {
            seen[pc / 8u] |= 1u;
            work.push_back(pc);
        }
    };
    auto addDelay = [&](uint32_t pc)
    {
        pc = wrap(pc) & ~7u;
        seen[pc / 8u] |= 2u;
    };
    addNormal(startPC & 0x3FF8u);
    for (uint32_t i = 0; i < rootCount; ++i)
        addNormal(roots[i] & 0x3FF8u);
    while (!work.empty())
    {
        const uint32_t pc = work.back();
        work.pop_back();
        const uint32_t lower = loadWord(code, pc);
        const uint32_t upper = loadWord(code, pc + 4u);
        const bool iBit = (upper & 0x80000000u) != 0u;
        const bool eBit = (upper & 0x40000000u) != 0u;
        const uint32_t opHi = iBit ? 0xFFu : (lower >> 25) & 0x7Fu;
        const bool branch = opHi == 0x20u || opHi == 0x21u || opHi == 0x24u || opHi == 0x25u || opHi == 0x28u ||
                            opHi == 0x29u || (opHi >= 0x2Cu && opHi <= 0x2Fu);
        if (eBit)
        {
            addDelay(pc + 8u);
            continue;
        }
        if (!branch)
        {
            addNormal(pc + 8u);
            continue;
        }
        addDelay(pc + 8u);
        const int32_t imm = static_cast<int32_t>(lower << 21) >> 21;
        const uint32_t target = (pc + 8u + static_cast<uint32_t>(imm * 8)) & 0x3FFFu;
        if (opHi == 0x20u || opHi == 0x21u)
            addNormal(target);
        else if (opHi != 0x24u && opHi != 0x25u)
            addNormal(target);
        if (opHi != 0x20u && opHi != 0x24u)
            addNormal(pc + 16u); // fall-through or call return point
    }
    for (uint32_t i = 0; i < pairs; ++i)
    {
        if (seen[i] & 1u)
            normal.push_back(i * 8u);
        if (seen[i] & 2u)
            delay.push_back(i * 8u);
    }
}

uint64_t ps2Vu1ProgramKey(const uint8_t *code, uint32_t codeSize, uint32_t startPC, const uint32_t *roots,
                          uint32_t rootCount)
{
    std::vector<uint32_t> normal, delay;
    ps2Vu1ProgramPairs(code, codeSize, startPC, normal, delay, roots, rootCount);
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&](uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
        {
            hash ^= (value >> (8 * i)) & 0xFFu;
            hash *= 1099511628211ull;
        }
    };
    mix(startPC & 0x3FF8u);
    std::vector<uint32_t> all = normal;
    all.insert(all.end(), delay.begin(), delay.end());
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    for (uint32_t pc : all)
    {
        mix(pc);
        mix(loadWord(code, pc));
        mix(loadWord(code, pc + 4u));
    }
    return hash;
}

VU1CompiledFn VU1Interpreter::lookupCompiled(const uint8_t *vuCode, uint32_t codeSize, PS2Memory *memory, uint32_t startPC)
{
    const auto &programs = compiledPrograms();
    if (programs.empty() || m_unit != Unit::VU1 || memory == nullptr || vuCode != memory->getVU1Code())
        return nullptr;
    const uint64_t generation = memory->getVU1CodeGeneration();
    if (generation != m_compiledGeneration || vuCode != m_compiledCode)
    {
        m_compiledGeneration = generation;
        m_compiledCode = vuCode;
        m_compiledCache.fill({});
    }
    CompiledCacheEntry &entry = m_compiledCache[(startPC / 8u) % m_compiledCache.size()];
    if (entry.pc != startPC)
    {
        entry.pc = startPC;
        entry.fn = nullptr;
        // The base key selects candidates; the code at their extra entry
        // points must match too.
        const auto range = programs.equal_range(ps2Vu1ProgramKey(vuCode, codeSize, startPC));
        // Prefer the matching variant with the most compiled jump targets.
        uint32_t bestRoots = 0;
        for (auto it = range.first; it != range.second; ++it)
        {
            const VU1CompiledProgram &program = *it->second;
            if (entry.fn && program.rootCount <= bestRoots)
                continue;
            if (ps2Vu1ProgramKey(vuCode, codeSize, startPC, program.roots, program.rootCount) == program.fullKey)
            {
                entry.fn = program.run;
                bestRoots = program.rootCount;
            }
        }
        static const bool s_log = std::getenv("PS2X_VU1_COMPILED_LOG") != nullptr;
        if (s_log)
            std::cerr << "[vu1:compiled] pc=0x" << std::hex << startPC << " key=" << ps2Vu1ProgramKey(vuCode, codeSize, startPC)
                      << std::dec << (entry.fn ? " compiled" : " interpreted") << std::endl;
    }
    return entry.fn;
}
