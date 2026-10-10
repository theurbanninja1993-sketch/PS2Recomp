#ifndef PS2_VU1_JIT_H
#define PS2_VU1_JIT_H

// Inline helpers for recompiled VU1 microprograms (tools/vu1_recomp.cpp).
// Generated code runs on VU1Interpreter's own state, so every helper here
// mirrors the interpreter's behaviour exactly (verified with vu1_replay);
// instruction fields are template/constant arguments.

#include "runtime/ps2_vu1.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

// Pairs reachable from startPC: `normal` are executed as ordinary pairs (branch
// targets, fall-throughs), `delay` as branch / E-bit delay slots.
void ps2Vu1ProgramPairs(const uint8_t *code, uint32_t codeSize, uint32_t startPC, std::vector<uint32_t> &normal,
                        std::vector<uint32_t> &delay, const uint32_t *roots = nullptr, uint32_t rootCount = 0);

struct VU1Jit
{
    using V = VU1Interpreter;

    static constexpr uint8_t lane(uint32_t component) { return static_cast<uint8_t>(1u << (3u - component)); }

    static VU1State &st(V &vu) { return vu.m_state; }

    // PS2 operands: denormals read as signed zero, Inf/NaN as signed FLT_MAX.
    static inline float N(float value)
    {
        uint32_t bits;
        std::memcpy(&bits, &value, 4);
        const uint32_t exponent = bits & 0x7F800000u;
        const uint32_t sign = bits & 0x80000000u;
        // Branch-free selects: these conditions depend on data.
        bits = exponent == 0u ? sign : bits;
        bits = exponent == 0x7F800000u ? (sign | 0x7F7FFFFFu) : bits;
        std::memcpy(&value, &bits, 4);
        return value;
    }

    static inline int32_t floatToInt(float value, float scale)
    {
        const double scaled = static_cast<double>(value) * static_cast<double>(scale);
        if (scaled >= static_cast<double>(std::numeric_limits<int32_t>::max()))
            return std::numeric_limits<int32_t>::max();
        if (scaled <= static_cast<double>(std::numeric_limits<int32_t>::min()))
            return std::numeric_limits<int32_t>::min();
        return static_cast<int32_t>(scaled);
    }

    static inline float f32(uint32_t bits)
    {
        float value;
        std::memcpy(&value, &bits, 4);
        return value;
    }

    // ------------------------------------------------------------ cycles

    static inline uint64_t now(const V &vu) { return vu.m_cycle; }

    static inline bool atBudget(V &vu, uint64_t budgetEnd) { return vu.m_cycle >= budgetEnd || vu.m_stopRequested; }

    // Compiled code commits flag entries lazily (syncFlags); before handing
    // back to the interpreter its commit schedule must include them again.
    static inline void leave(V &vu)
    {
        if (vu.m_flagCount != 0u)
            vu.m_nextCommitCycle = std::min(vu.m_nextCommitCycle, vu.m_flagPipeline[vu.m_flagHead].readyCycle);
    }
    static inline int ended(V &vu)
    {
        leave(vu);
        return VU1CompiledEnded;
    }
    static inline int leaveAt(V &vu, uint32_t pc)
    {
        leave(vu);
        vu.m_state.pc = pc;
        return VU1CompiledContinue;
    }
    // Before reading MAC/status/clip: commit the flag entries ready now.
    __attribute__((always_inline)) static inline void syncFlags(V &vu)
    {
        uint64_t ignored = UINT64_MAX;
        vu.commitFlags(vu.m_cycle, ignored);
    }
    __attribute__((always_inline)) static inline VU1Interpreter::FlagPipelineEntry *allocFlags(V &vu)
    {
        if (vu.m_flagCount == VU1Interpreter::kMaxFlagEntries)
            syncFlags(vu); // the interpreter would have committed the ready ones already
        if (vu.m_flagCount == VU1Interpreter::kMaxFlagEntries)
            return nullptr;
        auto &entry = vu.m_flagPipeline[(vu.m_flagHead + vu.m_flagCount++) & (VU1Interpreter::kMaxFlagEntries - 1u)];
        entry.readyCycle = vu.m_cycle + VU1Interpreter::kFmacLatency;
        entry.issueCycle = vu.m_cycle;
        entry.mac = entry.status = entry.extraSticky = entry.clip = 0u;
        entry.valid = true;
        entry.writesMac = entry.writesStatus = entry.writesSticky = entry.writesClip = false;
        return &entry;
    }

    static inline int exitAt(V &vu, uint32_t pc)
    {
        leave(vu);
        vu.m_state.pc = pc;
        return VU1CompiledBudget;
    }

    // Leaving in a branch delay slot: the interpreter finishes the branch.
    static inline int exitInDelaySlot(V &vu, uint32_t pc, bool taken, uint32_t target)
    {
        leave(vu);
        vu.m_state.pc = pc;
        if (taken)
        {
            vu.m_state.branchPending = true;
            vu.m_state.branchTarget = target;
            vu.m_state.branchDelay = 0u;
        }
        return VU1CompiledBudget;
    }

    // commitReadyPipelines for the fast-commit model (no queued VF/VI/ACC
    // writes): flags, Q, P and stores, in the interpreter's order.
    static void commitSlow(V &vu)
    {
        if (vu.m_vfMask | vu.m_viMask | vu.m_accMask)
        {
            vu.commitReadyPipelines();
            return;
        }
        uint64_t next = UINT64_MAX;
        const uint64_t cycle = vu.m_cycle;
        if (vu.m_fdiv.valid)
        {
            if (vu.m_fdiv.readyCycle <= cycle)
            {
                vu.m_state.q = vu.m_fdiv.value;
                const uint32_t currentDi = vu.m_fdiv.statusDi & 0x30u;
                vu.m_state.status = (vu.m_state.status & 0xFCFu) | currentDi | (currentDi << 6);
                vu.m_fdiv = {};
            }
            else
                next = std::min(next, vu.m_fdiv.readyCycle);
        }
        for (uint32_t live = vu.m_efuMask; live != 0u; live &= live - 1u)
        {
            const uint32_t index = static_cast<uint32_t>(__builtin_ctz(live));
            auto &entry = vu.m_efu[index];
            if (entry.readyCycle <= cycle)
            {
                vu.m_state.p = entry.value;
                entry.valid = false;
                vu.m_efuMask &= ~(1u << index);
            }
            else
                next = std::min(next, entry.readyCycle);
        }
        for (uint32_t live = vu.m_storeMask; live != 0u; live &= live - 1u)
        {
            const uint32_t index = static_cast<uint32_t>(__builtin_ctz(live));
            auto &store = vu.m_storePipeline[index];
            if (store.readyCycle > cycle)
            {
                next = std::min(next, store.readyCycle);
                continue;
            }
            if (vu.m_activeVuData && store.address + 16u <= vu.m_activeVuDataSize)
            {
                uint32_t *words = reinterpret_cast<uint32_t *>(vu.m_activeVuData + store.address);
                for (uint32_t c = 0; c < 4u; ++c)
                    if (store.laneMask & lane(c))
                        std::memcpy(&words[c], &store.words[c], 4);
            }
            store.valid = false;
            vu.m_storeMask &= ~(1u << index);
        }
        vu.m_nextCommitCycle = next;
    }

    // Q/P/store commits (flag entries are committed lazily, see syncFlags).
    __attribute__((always_inline)) static inline void commit(V &vu)
    {
        if (vu.m_cycle >= vu.m_nextCommitCycle)
            commitSlow(vu);
    }

    static inline void endCycle(V &vu)
    {
        ++vu.m_cycle; // state.cycles is refreshed when run() returns
        commit(vu);
        if (vu.m_xgkick.active)
            vu.progressXgkick();
    }
    // Programs with FSSET/FCSET keep the interpreter's per-cycle flag commits
    // (FSSET replaces sticky bits, so its order against Q commits matters).
    static inline void endCycleEagerFlags(V &vu)
    {
        endCycle(vu);
        syncFlags(vu);
    }

    // The interpreter's stall loop; false when the budget ran out.
    template <typename Ready>
    static inline bool stall(V &vu, uint64_t budgetEnd, Ready ready)
    {
        uint64_t r = ready();
        while (r > vu.m_cycle)
        {
            if (r >= budgetEnd)
            {
                vu.advanceTo(budgetEnd);
                break;
            }
            vu.advanceTo(r);
            r = ready();
        }
        return vu.m_cycle < budgetEnd;
    }

    template <uint8_t reg, uint8_t lanes>
    static inline uint64_t vf(const V &vu, uint64_t r)
    {
        if (vu.m_vfRegReady[reg] <= r)
            return r;
        for (uint32_t c = 0; c < 4u; ++c)
            if (lanes & lane(c))
                r = std::max(r, vu.m_vfReady[reg][c]);
        return r;
    }
    template <uint8_t reg>
    static inline uint64_t vi(const V &vu, uint64_t r) { return std::max(r, vu.m_viReady[reg]); }
    template <uint8_t lanes>
    static inline uint64_t acc(const V &vu, uint64_t r)
    {
        for (uint32_t c = 0; c < 4u; ++c)
            if (lanes & lane(c))
                r = std::max(r, vu.m_accReady[c]);
        return r;
    }
    static inline uint64_t fdiv(const V &vu, uint64_t r) { return vu.m_fdiv.valid ? std::max(r, vu.m_fdiv.readyCycle) : r; }
    static inline uint64_t efuResource(const V &vu, uint64_t r) { return std::max(r, vu.m_efuResourceReady); }
    static inline uint64_t waitP(const V &vu, uint64_t r)
    {
        for (const auto &entry : vu.m_efu)
            if (entry.valid)
                r = std::max(r, entry.readyCycle);
        return r;
    }
    static inline uint64_t xgkick(const V &vu, uint64_t r) { return vu.m_xgkick.active ? std::max(r, vu.m_cycle + 1u) : r; }

    template <uint8_t reg, uint8_t lanes, uint32_t latency>
    static inline void markVf(V &vu)
    {
        const uint64_t ready = vu.m_cycle + latency;
        for (uint32_t c = 0; c < 4u; ++c)
            if (lanes & lane(c))
                vu.m_vfReady[reg][c] = ready;
        vu.m_vfRegReady[reg] = std::max(vu.m_vfRegReady[reg], ready);
    }
    template <uint8_t reg, uint32_t latency>
    static inline void markVi(V &vu) { vu.m_viReady[reg] = vu.m_cycle + latency; }
    template <uint8_t lanes>
    static inline void markAcc(V &vu)
    {
        for (uint32_t c = 0; c < 4u; ++c)
            if (lanes & lane(c))
                vu.m_accReady[c] = vu.m_cycle + 1u; // kAccForwardLatency
    }

    // ------------------------------------------------------- VI branches

    template <uint8_t reg>
    static inline int32_t branchVi(const V &vu)
    {
        if (reg == 0u)
            return 0;
        if (vu.m_viBranchBackupValid && vu.m_viBranchBackupReg == reg)
            return vu.m_viBranchBackupValue;
        return vu.m_state.vi[reg];
    }
    static inline void clearBranchBackup(V &vu) { vu.m_viBranchBackupValid = false; }
    template <uint8_t reg>
    static inline void recordBranchBackup(V &vu, int32_t oldValue)
    {
        vu.m_viBranchBackupValue = oldValue;
        vu.m_viBranchBackupReg = reg;
        vu.m_viBranchBackupValid = true;
    }

    // ------------------------------------------------------------- FMAC

    template <uint8_t dest>
    static inline void applyDest(float *dst, const float *result)
    {
        if (dest & 0x8u)
            dst[0] = result[0];
        if (dest & 0x4u)
            dst[1] = result[1];
        if (dest & 0x2u)
            dst[2] = result[2];
        if (dest & 0x1u)
            dst[3] = result[3];
    }

    // Sticky flags of a product-sum's exact product (calculateFmacProductSticky).
    __attribute__((always_inline)) static inline uint32_t productFlags(float left, float right)
    {
        // Operands are normalized (no denormals/Inf/NaN). With biased
        // exponents ea, eb the product lies in [2^(ea+eb-254), 2^(ea+eb-252)),
        // safely inside the normal float range for 128 <= ea+eb <= 379.
        uint32_t lb, rb;
        std::memcpy(&lb, &left, 4);
        std::memcpy(&rb, &right, 4);
        const uint32_t sign = ((lb ^ rb) >> 31) << 1;
        const uint32_t ea = (lb >> 23) & 0xFFu, eb = (rb >> 23) & 0xFFu;
        if (ea == 0u || eb == 0u)
            return sign | 0x1u;
        if (ea + eb >= 128u && ea + eb <= 379u)
            return sign;
        const double product = static_cast<double>(left) * static_cast<double>(right);
        const double magnitude = std::fabs(product);
        uint32_t flags = std::signbit(product) ? 0x2u : 0u;
        if (magnitude == 0.0)
            flags |= 0x1u;
        else if (magnitude > static_cast<double>(std::numeric_limits<float>::max()))
            flags |= 0x8u;
        else if (magnitude < static_cast<double>(std::numeric_limits<float>::min()))
            flags |= 0x5u;
        return flags & 0xFu;
    }

    // Normalize, queue MAC/status (4-cycle flag pipeline) and store an FMAC
    // result (applyFmacDest / applyFmacDestAcc).
    // exactZero: lanes (component bit c) whose result is known exact, so a
    // zero there is a true zero (ADD/SUB, or MUL by a zero operand) and
    // needs no exact recomputation.
    template <uint8_t dest>
    __attribute__((always_inline)) static inline void fmacFinish(V &vu, uint32_t instr, float *result, float *dst,
                                                                 uint32_t extraSticky, uint32_t exactZero = 0u)
    {
        uint32_t mac = 0u, status = 0u;
        bool slow = false;
        for (uint32_t c = 0; c < 4u; ++c)
        {
            if (!(dest & lane(c)))
                continue;
            uint32_t bits;
            std::memcpy(&bits, &result[c], 4);
            const uint32_t exponent = (bits >> 23) & 0xFFu;
            const bool normal = exponent - 2u <= 251u;
            const uint32_t zero = ((bits << 1) == 0u && (exactZero & (1u << c))) ? 1u : 0u;
            slow |= !normal && !zero;
            const uint32_t sign = bits >> 31;
            mac |= (zero * lane(c)) | ((sign * lane(c)) << 4);
            status |= zero | (sign << 1);
        }
        if (slow)
        {
            uint8_t laneFlags[4] = {0, 0, 0, 0};
            vu.m_currentUpperInstruction = instr;
            vu.normalizeFmacResult(result, dest, laneFlags);
            mac = status = 0u;
            for (uint32_t c = 0; c < 4u; ++c)
            {
                if (!(dest & lane(c)))
                    continue;
                const uint32_t flags = laneFlags[c];
                const uint32_t l = lane(c);
                mac |= ((flags & 1u) ? l : 0u) | ((flags & 2u) ? l << 4 : 0u) | ((flags & 4u) ? l << 8 : 0u) |
                       ((flags & 8u) ? l << 12 : 0u);
                status |= flags;
            }
        }
        if (dest != 0u)
        {
            if (auto *entry = allocFlags(vu))
            {
                entry->mac = mac;
                entry->status = status;
                entry->extraSticky = extraSticky;
                entry->writesMac = true;
                entry->writesStatus = true;
            }
            else
            {
                vu.reportReservedInstruction(true, 0xFFFFFFFFu);
            }
        }
        applyDest<dest>(dst, result);
    }

    // ------------------------------------------------------- memory, misc

    static inline uint8_t *data(V &vu) { return vu.m_activeVuData; }
    static inline uint32_t dataSize(const V &vu) { return vu.m_activeVuDataSize; }

    static inline void store(V &vu, uint32_t address, const uint32_t words[4], uint8_t laneMask)
    {
        const uint32_t free = ~vu.m_storeMask & ((1u << VU1Interpreter::kMaxPendingStores) - 1u);
        if (free == 0u)
        {
            vu.reportReservedInstruction(false, 0xFFFFFFFCu);
            return;
        }
        const uint32_t index = static_cast<uint32_t>(__builtin_ctz(free));
        auto &entry = vu.m_storePipeline[index];
        entry.valid = true;
        vu.m_storeMask |= 1u << index;
        entry.readyCycle = vu.m_cycle + 1u;
        vu.m_nextCommitCycle = std::min(vu.m_nextCommitCycle, entry.readyCycle);
        entry.address = address;
        entry.laneMask = laneMask;
        std::copy(words, words + 4, entry.words.begin());
    }

    // Uncommon instructions run through the interpreter's own implementation.
    static inline void execUpper(V &vu, uint32_t instr) { vu.execUpper(instr); }
    static inline void execLower(V &vu, uint32_t instr, uint32_t upper)
    {
        vu.execLower(instr, vu.m_activeVuData, vu.m_activeVuDataSize, *vu.m_activeGs, vu.m_activeMemory, upper);
    }
    // CLIP: judgement of vf[fs].xyz against |vf[ft].w| (raw bits), queued
    // through the flag pipeline like queueClip.
    static inline void clip(V &vu, const float *xyz, const float *w)
    {
        uint32_t wBits;
        std::memcpy(&wBits, &w[3], 4);
        const int32_t limit = (wBits & 0x7F800000u) != 0u ? static_cast<int32_t>(wBits & 0x7FFFFFFFu) : 0x007FFFFF;
        auto exceeds = [limit](float value, uint32_t signMask)
        {
            uint32_t bits;
            std::memcpy(&bits, &value, 4);
            bits ^= signMask;
            int32_t ordered;
            std::memcpy(&ordered, &bits, 4);
            return ordered > limit;
        };
        uint32_t flags = 0u;
        if (exceeds(xyz[0], 0u)) flags |= 0x01u;
        if (exceeds(xyz[0], 0x80000000u)) flags |= 0x02u;
        if (exceeds(xyz[1], 0u)) flags |= 0x04u;
        if (exceeds(xyz[1], 0x80000000u)) flags |= 0x08u;
        if (exceeds(xyz[2], 0u)) flags |= 0x10u;
        if (exceeds(xyz[2], 0x80000000u)) flags |= 0x20u;
        vu.m_workingClip = ((vu.m_workingClip << 6) | (flags & 0x3Fu)) & 0xFFFFFFu;
        if (auto *entry = allocFlags(vu))
        {
            entry->clip = vu.m_workingClip;
            entry->writesClip = true;
        }
        else
        {
            vu.reportReservedInstruction(true, 0xFFFFFFFDu);
        }
    }
};

#endif
