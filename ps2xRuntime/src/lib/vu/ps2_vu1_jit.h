#ifndef PS2_VU1_JIT_H
#define PS2_VU1_JIT_H

// Inline helpers for recompiled VU1 microprograms (tools/vu1_recomp.cpp).
// Generated code runs on VU1Interpreter's own state, so every helper here
// mirrors the interpreter's behaviour exactly (verified with vu1_replay);
// instruction fields are template/constant arguments.

#include "runtime/ps2_vu1.h"

#include <algorithm>
#include <cmath>
#include <emmintrin.h>
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
    __attribute__((always_inline)) static inline float N(float value)
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

    __attribute__((always_inline)) static inline int32_t floatToInt(float value, float scale)
    {
        const double scaled = static_cast<double>(value) * static_cast<double>(scale);
        if (scaled >= static_cast<double>(std::numeric_limits<int32_t>::max()))
            return std::numeric_limits<int32_t>::max();
        if (scaled <= static_cast<double>(std::numeric_limits<int32_t>::min()))
            return std::numeric_limits<int32_t>::min();
        return static_cast<int32_t>(scaled);
    }

    __attribute__((always_inline)) static inline float f32(uint32_t bits)
    {
        float value;
        std::memcpy(&value, &bits, 4);
        return value;
    }

    // ------------------------------------------------------------ cycles

    __attribute__((always_inline)) static inline uint64_t now(const V &vu) { return vu.m_cycle; }

    __attribute__((always_inline)) static inline bool atBudget(V &vu, uint64_t budgetEnd) { return vu.m_cycle >= budgetEnd || vu.m_stopRequested; }

    // Compiled code commits flag entries lazily (syncFlags); before handing
    // back to the interpreter its commit schedule must include them again.
    __attribute__((always_inline)) static inline void leave(V &vu)
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
    __attribute__((noinline, cold)) static void commitSlow(V &vu)
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

    __attribute__((always_inline)) static inline void endCycle(V &vu)
    {
        ++vu.m_cycle; // state.cycles is refreshed when run() returns
        commit(vu);
        if (vu.m_xgkick.active)
            vu.progressXgkick();
    }
    // Programs with FSSET/FCSET keep the interpreter's per-cycle flag commits
    // (FSSET replaces sticky bits, so its order against Q commits matters).
    __attribute__((always_inline)) static inline void endCycleEagerFlags(V &vu)
    {
        endCycle(vu);
        syncFlags(vu);
    }

    // The interpreter's stall loop; false when the budget ran out.
    template <typename Ready>
    __attribute__((always_inline)) static inline bool stall(V &vu, uint64_t budgetEnd, Ready ready)
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
    __attribute__((always_inline)) static inline uint64_t vf(const V &vu, uint64_t r)
    {
        if (vu.m_vfRegReady[reg] <= r)
            return r;
        for (uint32_t c = 0; c < 4u; ++c)
            if (lanes & lane(c))
                r = std::max(r, vu.m_vfReady[reg][c]);
        return r;
    }
    template <uint8_t reg>
    __attribute__((always_inline)) static inline uint64_t vi(const V &vu, uint64_t r) { return std::max(r, vu.m_viReady[reg]); }
    template <uint8_t lanes>
    __attribute__((always_inline)) static inline uint64_t acc(const V &vu, uint64_t r)
    {
        for (uint32_t c = 0; c < 4u; ++c)
            if (lanes & lane(c))
                r = std::max(r, vu.m_accReady[c]);
        return r;
    }
    __attribute__((always_inline)) static inline uint64_t fdiv(const V &vu, uint64_t r) { return vu.m_fdiv.valid ? std::max(r, vu.m_fdiv.readyCycle) : r; }
    __attribute__((always_inline)) static inline uint64_t efuResource(const V &vu, uint64_t r) { return std::max(r, vu.m_efuResourceReady); }
    __attribute__((always_inline)) static inline uint64_t waitP(const V &vu, uint64_t r)
    {
        for (const auto &entry : vu.m_efu)
            if (entry.valid)
                r = std::max(r, entry.readyCycle);
        return r;
    }
    __attribute__((always_inline)) static inline uint64_t xgkick(const V &vu, uint64_t r) { return vu.m_xgkick.active ? std::max(r, vu.m_cycle + 1u) : r; }

    template <uint8_t reg, uint8_t lanes, uint32_t latency>
    __attribute__((always_inline)) static inline void markVf(V &vu)
    {
        const uint64_t ready = vu.m_cycle + latency;
        for (uint32_t c = 0; c < 4u; ++c)
            if (lanes & lane(c))
                vu.m_vfReady[reg][c] = ready;
        vu.m_vfRegReady[reg] = std::max(vu.m_vfRegReady[reg], ready);
    }
    template <uint8_t reg, uint32_t latency>
    __attribute__((always_inline)) static inline void markVi(V &vu) { vu.m_viReady[reg] = vu.m_cycle + latency; }
    template <uint8_t lanes>
    __attribute__((always_inline)) static inline void markAcc(V &vu)
    {
        for (uint32_t c = 0; c < 4u; ++c)
            if (lanes & lane(c))
                vu.m_accReady[c] = vu.m_cycle + 1u; // kAccForwardLatency
    }

    // ------------------------------------------------------- VI branches

    template <uint8_t reg>
    __attribute__((always_inline)) static inline int32_t branchVi(const V &vu)
    {
        if (reg == 0u)
            return 0;
        if (vu.m_viBranchBackupValid && vu.m_viBranchBackupReg == reg)
            return vu.m_viBranchBackupValue;
        return vu.m_state.vi[reg];
    }
    __attribute__((always_inline)) static inline void clearBranchBackup(V &vu) { vu.m_viBranchBackupValid = false; }
    template <uint8_t reg>
    __attribute__((always_inline)) static inline void recordBranchBackup(V &vu, int32_t oldValue)
    {
        vu.m_viBranchBackupValue = oldValue;
        vu.m_viBranchBackupReg = reg;
        vu.m_viBranchBackupValid = true;
    }

    // ------------------------------------------------------------- FMAC

    template <uint8_t dest>
    __attribute__((always_inline)) static inline void applyDest(float *dst, const float *result)
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

    // ------------------------------------------------------------- SIMD
    // Four-lane versions of the FMAC helpers (SSE2). SSE arithmetic is the
    // same IEEE single precision under the same MXCSR rounding as the scalar
    // code, so results are identical; rare cases fall back to the scalar
    // exact path.

    // Lane bits in component order (bit c = component c) of a DEST mask.
    static constexpr int destBits(uint8_t dest)
    {
        return ((dest >> 3) & 1) | (((dest >> 2) & 1) << 1) | (((dest >> 1) & 1) << 2) | ((dest & 1) << 3);
    }
    // Component-order bits -> MAC lane order (x = bit 3).
    static constexpr uint32_t laneBits(int bits)
    {
        return static_cast<uint32_t>(((bits & 1) << 3) | ((bits & 2) << 1) | ((bits & 4) >> 1) | ((bits & 8) >> 3));
    }

    __attribute__((always_inline)) static inline __m128 load4(const float *p) { return _mm_loadu_ps(p); }

    __attribute__((always_inline)) static inline __m128 N4(__m128 v)
    {
        __m128i bits = _mm_castps_si128(v);
        const __m128i exponent = _mm_and_si128(bits, _mm_set1_epi32(0x7F800000));
        const __m128i sign = _mm_and_si128(bits, _mm_set1_epi32(static_cast<int>(0x80000000u)));
        const __m128i zero = _mm_cmpeq_epi32(exponent, _mm_setzero_si128());
        const __m128i inf = _mm_cmpeq_epi32(exponent, _mm_set1_epi32(0x7F800000));
        bits = _mm_or_si128(_mm_andnot_si128(zero, bits), _mm_and_si128(zero, sign));
        bits = _mm_or_si128(_mm_andnot_si128(inf, bits), _mm_and_si128(inf, _mm_or_si128(sign, _mm_set1_epi32(0x7F7FFFFF))));
        return _mm_castsi128_ps(bits);
    }

    // FTOIn: truncation of value * 2^n with saturation (floatToInt); the
    // power-of-two scale is exact in float, so no double is needed.
    __attribute__((always_inline)) static inline __m128 ftoi4(__m128 value, float scale)
    {
        const __m128 scaled = _mm_mul_ps(value, _mm_set1_ps(scale));
        const __m128i truncated = _mm_cvttps_epi32(scaled);
        // cvttps gives INT_MIN for |x| >= 2^31; positive overflow saturates up.
        const __m128 overflow = _mm_cmpge_ps(scaled, _mm_set1_ps(2147483648.0f));
        const __m128i result = _mm_or_si128(_mm_andnot_si128(_mm_castps_si128(overflow), truncated),
                                            _mm_and_si128(_mm_castps_si128(overflow), _mm_set1_epi32(0x7FFFFFFF)));
        return _mm_castsi128_ps(result);
    }
    // ITOFn: int -> float (MXCSR rounding, as static_cast) divided by 2^n.
    __attribute__((always_inline)) static inline __m128 itof4(__m128 bits, float divisor)
    {
        const __m128 value = _mm_cvtepi32_ps(_mm_castps_si128(bits));
        return divisor == 1.0f ? value : _mm_div_ps(value, _mm_set1_ps(divisor));
    }
    __attribute__((always_inline)) static inline __m128 abs4(__m128 value)
    {
        return _mm_and_ps(value, _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)));
    }

    // Component bits where either operand is zero (a zero product is exact).
    __attribute__((always_inline)) static inline int zeroOperands(__m128 a, __m128 b)
    {
        const __m128 zero = _mm_setzero_ps();
        return _mm_movemask_ps(_mm_or_ps(_mm_cmpeq_ps(a, zero), _mm_cmpeq_ps(b, zero)));
    }

    template <uint8_t dest>
    __attribute__((always_inline)) static inline void store4(float *dst, __m128 value)
    {
        if constexpr (dest == 0xFu)
        {
            _mm_storeu_ps(dst, value);
        }
        else
        {
            const __m128 mask = _mm_castsi128_ps(_mm_set_epi32((dest & 1) ? -1 : 0, (dest & 2) ? -1 : 0,
                                                               (dest & 4) ? -1 : 0, (dest & 8) ? -1 : 0));
            _mm_storeu_ps(dst, _mm_or_ps(_mm_and_ps(mask, value), _mm_andnot_ps(mask, _mm_loadu_ps(dst))));
        }
    }

    // Sticky product flags of a product-sum over DEST lanes (productFlags).
    template <uint8_t dest>
    __attribute__((always_inline)) static inline uint32_t productFlags4(__m128 a, __m128 b)
    {
        constexpr int dm = destBits(dest);
        const __m128i ab = _mm_castps_si128(a), bb = _mm_castps_si128(b);
        const __m128i ea = _mm_and_si128(_mm_srli_epi32(ab, 23), _mm_set1_epi32(0xFF));
        const __m128i eb = _mm_and_si128(_mm_srli_epi32(bb, 23), _mm_set1_epi32(0xFF));
        const __m128i zero = _mm_or_si128(_mm_cmpeq_epi32(ea, _mm_setzero_si128()), _mm_cmpeq_epi32(eb, _mm_setzero_si128()));
        const __m128i sum = _mm_add_epi32(ea, eb);
        const __m128i safe = _mm_and_si128(_mm_cmpgt_epi32(sum, _mm_set1_epi32(127)), _mm_cmplt_epi32(sum, _mm_set1_epi32(380)));
        const int unsafe = ~_mm_movemask_ps(_mm_castsi128_ps(_mm_or_si128(zero, safe))) & dm;
        if (__builtin_expect(unsafe != 0, 0))
        {
            alignas(16) float av[4], bv[4];
            _mm_store_ps(av, a);
            _mm_store_ps(bv, b);
            uint32_t flags = 0u;
            for (uint32_t c = 0; c < 4u; ++c)
                if (dm & (1 << c))
                    flags |= productFlags(av[c], bv[c]);
            return flags;
        }
        const int sign = _mm_movemask_ps(_mm_xor_ps(a, b)) & dm;
        const int zeroBits = _mm_movemask_ps(_mm_castsi128_ps(zero)) & dm;
        return (sign ? 0x2u : 0u) | (zeroBits ? 0x1u : 0u);
    }

    // fmacFinish for a four-lane result (exactZero in component bits).
    template <uint8_t dest>
    __attribute__((always_inline)) static inline void fmac4(V &vu, uint32_t instr, __m128 r, float *dst, uint32_t extraSticky,
                                                            int exactZero)
    {
        constexpr int dm = destBits(dest);
        const __m128i bits = _mm_castps_si128(r);
        const __m128i exponent = _mm_and_si128(_mm_srli_epi32(bits, 23), _mm_set1_epi32(0xFF));
        const __m128i normal = _mm_and_si128(_mm_cmpgt_epi32(exponent, _mm_set1_epi32(1)),
                                             _mm_cmplt_epi32(exponent, _mm_set1_epi32(254)));
        const int zeroBits = _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(_mm_slli_epi32(bits, 1), _mm_setzero_si128()))) &
                             exactZero & dm;
        const int normalBits = _mm_movemask_ps(_mm_castsi128_ps(normal)) & dm;
        if (__builtin_expect(((normalBits | zeroBits) ^ dm) != 0, 0))
        {
            alignas(16) float tmp[4];
            _mm_store_ps(tmp, r);
            fmacFinish<dest>(vu, instr, tmp, dst, extraSticky, static_cast<uint32_t>(exactZero));
            return;
        }
        const int signBits = _mm_movemask_ps(r) & dm;
        if constexpr (dest != 0u)
        {
            if (auto *entry = allocFlags(vu))
            {
                entry->mac = laneBits(zeroBits) | (laneBits(signBits) << 4);
                entry->status = (zeroBits ? 0x1u : 0u) | (signBits ? 0x2u : 0u);
                entry->extraSticky = extraSticky;
                entry->writesMac = true;
                entry->writesStatus = true;
            }
            else
            {
                vu.reportReservedInstruction(true, 0xFFFFFFFFu);
            }
        }
        store4<dest>(dst, r);
    }

    // ------------------------------------------------------- memory, misc

    __attribute__((always_inline)) static inline uint8_t *data(V &vu) { return vu.m_activeVuData; }
    __attribute__((always_inline)) static inline uint32_t dataSize(const V &vu) { return vu.m_activeVuDataSize; }

    __attribute__((always_inline)) static inline void store(V &vu, uint32_t address, const uint32_t words[4], uint8_t laneMask)
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
