#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_runtime_calls.h"
#include "ps2_stubs.h"
#include "Kernel/Stubs/MPEG.h"
#include "ps2_syscalls.h"
#include "ps2_log.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <sstream>
#include <utility>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <vector>

extern std::atomic<uint64_t> g_ps2PresentedFrameCounter;

namespace
{
    std::mutex &registryMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    std::vector<ps2_game_overrides::Descriptor> &descriptorRegistry()
    {
        static std::vector<ps2_game_overrides::Descriptor> registry;
        return registry;
    }

    bool equalsIgnoreCaseAscii(std::string_view lhs, std::string_view rhs)
    {
        if (lhs.size() != rhs.size())
        {
            return false;
        }

        for (size_t i = 0; i < lhs.size(); ++i)
        {
            const auto l = static_cast<unsigned char>(lhs[i]);
            const auto r = static_cast<unsigned char>(rhs[i]);
            if (std::tolower(l) != std::tolower(r))
            {
                return false;
            }
        }

        return true;
    }

    std::string basenameFromPath(const std::string &path)
    {
        std::error_code ec;
        const std::filesystem::path fsPath(path);
        const std::filesystem::path leaf = fsPath.filename();
        if (leaf.empty())
        {
            return path;
        }
        return leaf.string();
    }

    std::optional<PS2Runtime::RecompiledFunction> resolveHandlerByName(std::string_view handlerName)
    {
        const std::string_view resolvedSyscall = ps2_runtime_calls::resolveSyscallName(handlerName);
        if (!resolvedSyscall.empty())
        {
#define PS2_RESOLVE_SYSCALL(name)                   \
    if (resolvedSyscall == std::string_view{#name}) \
    {                                               \
        return &ps2_syscalls::name;                 \
    }
            PS2_SYSCALL_LIST(PS2_RESOLVE_SYSCALL)
#undef PS2_RESOLVE_SYSCALL
        }

        const std::string_view resolvedStub = ps2_runtime_calls::resolveStubName(handlerName);
        if (!resolvedStub.empty())
        {
#define PS2_RESOLVE_STUB(name)                   \
    if (resolvedStub == std::string_view{#name}) \
    {                                            \
        return &ps2_stubs::name;                 \
    }
            PS2_STUB_LIST(PS2_RESOLVE_STUB)
#undef PS2_RESOLVE_STUB
        }

        return std::nullopt;
    }
}
namespace ps2_game_overrides
{
    AutoRegister::AutoRegister(const Descriptor &descriptor)
    {
        registerDescriptor(descriptor);
    }

    void registerDescriptor(const Descriptor &descriptor)
    {
        if (!descriptor.apply)
        {
            std::cerr << "[game_overrides] ignoring descriptor with null apply callback." << std::endl;
            return;
        }

        std::lock_guard<std::mutex> lock(registryMutex());
        descriptorRegistry().push_back(descriptor);
    }

    bool bindAddressHandler(PS2Runtime &runtime, uint32_t address, std::string_view handlerName)
    {
        const auto resolved = resolveHandlerByName(handlerName);
        if (!resolved.has_value())
        {
            std::cerr << "[game_overrides] unresolved handler '" << handlerName
                      << "' for address 0x" << std::hex << address << std::dec << std::endl;
            return false;
        }

        return runtime.replaceFunction(address, resolved.value());
    }

    void applyMatching(PS2Runtime &runtime,
                       const std::string &elfPath,
                       uint32_t entry,
                       uint32_t fileCrc32,
                       bool fileCrcValid)
    {

        std::vector<Descriptor> descriptors;
        {
            std::lock_guard<std::mutex> lock(registryMutex());
            descriptors = descriptorRegistry();
        }

        if (descriptors.empty())
        {
            return;
        }

        const std::string elfName = basenameFromPath(elfPath);
        size_t appliedCount = 0;
        for (const Descriptor &descriptor : descriptors)
        {
            if (!descriptor.apply)
            {
                continue;
            }

            if (descriptor.elfName && descriptor.elfName[0] != '\0')
            {
                if (!equalsIgnoreCaseAscii(descriptor.elfName, elfName))
                {
                    continue;
                }
            }

            if (descriptor.entry != 0u && descriptor.entry != entry)
            {
                continue;
            }

            if (descriptor.crc32 != 0u)
            {
                if (!fileCrcValid || fileCrc32 != descriptor.crc32)
                {
                    continue;
                }
            }

            const char *name = (descriptor.name && descriptor.name[0] != '\0')
                                   ? descriptor.name
                                   : "unnamed";
            RUNTIME_LOG("[game_overrides] applying '" << name << "'");
            descriptor.apply(runtime);
            ++appliedCount;
        }

        if (appliedCount > 0)
        {
            RUNTIME_LOG("[game_overrides] applied " << appliedCount << " matching override(s).");
        }
    }
}

// ---------------------------------------------------------------------------
// Grand Theft Auto: Vice City Stories (NTSC-U, SLUS_215.90)
//
// Lives in this TU (rather than its own file) so the static registration is
// never dropped when ps2_runtime is linked as a static archive.
// ---------------------------------------------------------------------------
namespace
{
    uint32_t vcsRead32(const uint8_t *rdram, uint32_t addr)
    {
        uint32_t v = 0u;
        std::memcpy(&v, rdram + (addr & PS2_RAM_MASK), sizeof(v));
        return v;
    }

    uint64_t vcsRead64(const uint8_t *rdram, uint32_t addr)
    {
        uint64_t v = 0u;
        std::memcpy(&v, rdram + (addr & PS2_RAM_MASK), sizeof(v));
        return v;
    }

    void vcsWrite32(uint8_t *rdram, uint32_t addr, uint32_t v)
    {
        std::memcpy(rdram + (addr & PS2_RAM_MASK), &v, sizeof(v));
    }

    void vcsWrite64(uint8_t *rdram, uint32_t addr, uint64_t v)
    {
        std::memcpy(rdram + (addr & PS2_RAM_MASK), &v, sizeof(v));
    }

    // Exact libpkt semantics, translated from the game's own code. The generic
    // runtime stubs also rewrite the open DMA tag's QWC on every pointer move,
    // but this game closes DMA tags natively (0x448AB8: qwc += (cur-tag)/16 - 1),
    // so the generic stubs double-count and corrupt every GIF chain (e.g. the
    // movie's per-macroblock texture uploads).
    //
    // void sceGifPkAddGsAD(sceGifPacket *pkt, u_int addr, u_long data)
    void vcsGifPkAddGsAD(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t pkt = getRegU32(ctx, 4);
        const uint32_t cur = vcsRead32(rdram, pkt);
        vcsWrite64(rdram, cur, GPR_U64(ctx, 6));
        vcsWrite64(rdram, cur + 8u, static_cast<uint64_t>(getRegU32(ctx, 5)));
        vcsWrite32(rdram, pkt, cur + 16u);
        ctx->pc = getRegU32(ctx, 31);
    }

    // void sceGifPkCloseGifTag(sceGifPacket *pkt)
    void vcsGifPkCloseGifTag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t pkt = getRegU32(ctx, 4);
        const uint32_t tagAddr = vcsRead32(rdram, pkt + 12u);
        uint32_t cur = vcsRead32(rdram, pkt);
        if (tagAddr != 0u)
        {
            uint64_t tag = vcsRead64(rdram, tagAddr);
            uint32_t count = static_cast<uint32_t>(static_cast<int32_t>(cur - tagAddr) >> 3) - 2u;
            const uint32_t flg = static_cast<uint32_t>(tag >> 58) & 3u;
            if (flg != 1u)
            {
                count >>= 1u; // PACKED/IMAGE count qwords, REGLIST counts dwords
            }
            if (flg != 2u)
            {
                uint32_t nreg = static_cast<uint32_t>(tag >> 60);
                if (nreg == 0u)
                {
                    nreg = 16u;
                }
                count = (count + nreg - 1u) / nreg;
            }
            vcsWrite32(rdram, pkt + 12u, 0u);
            vcsWrite64(rdram, tagAddr, tag + count);
        }
        while ((cur & 0xCu) != 0u)
        {
            vcsWrite32(rdram, cur, 0u);
            cur += 4u;
        }
        vcsWrite32(rdram, pkt, cur);
        ctx->pc = getRegU32(ctx, 31);
    }

    // Movie player end-of-stream (0x31A5D0). The game streams exactly the PSS file's
    // byte size, skips its trailing program_end, and instead retries writing
    // 00 00 01 B7 into libmpeg's video ES ring until the IPU has drained room for it.
    // The HLE decoder never reads that ring (so it never drains), and doesn't need the
    // end code there: report it queued and finish the HLE playback instead. Without
    // this the player deadlocks after every movie.
    void vcsMoviePushEndCode(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_stubs::notifyMpegCdStreamEof(runtime);
        setReturnS32(ctx, 1);
        ctx->pc = getRegU32(ctx, 31);
    }

    // Movie player "finished?" (0x31A6A8: PSS ring drained && sceMpegIsEnd). The HLE
    // demuxer accepts data faster than the IPU would, so when the main loop stops
    // presenting, a few decoded fade-out pictures can still be owed to the decoder
    // thread, which then waits forever for a display slot nobody frees. Report done
    // once the HLE stream itself has ended; the undelivered tail frames are dropped.
    PS2Runtime::RecompiledFunction g_vcsOrigMovieFinished = nullptr;
    void vcsMovieFinished(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t player = getRegU32(ctx, 4);
        g_vcsOrigMovieFinished(rdram, ctx, runtime);
        if (getRegU32(ctx, 2) == 0u && ps2_stubs::isMpegStreamFinished(player))
        {
            setReturnS32(ctx, 1);
        }
    }

    // PS2X_COUNT_CALLS=0xA,0xB,...: debug wrapper that counts calls to up to 16 guest
    // functions and prints the counts (and the first calls' args) every 600 frames.
    constexpr int kVcsMaxCounted = 16;
    PS2Runtime::RecompiledFunction g_vcsCountOrig[kVcsMaxCounted]{};
    uint32_t g_vcsCountAddr[kVcsMaxCounted]{};
    std::atomic<uint64_t> g_vcsCountHits[kVcsMaxCounted]{};
    int g_vcsCountUsed = 0;

    void vcsReportCounts()
    {
        static uint64_t s_lastFrame = 0;
        const uint64_t frame = g_ps2PresentedFrameCounter.load(std::memory_order_relaxed);
        if (frame < s_lastFrame + 600u)
            return;
        s_lastFrame = frame;
        std::ostringstream out;
        out << "[calls] frame=" << frame;
        for (int i = 0; i < g_vcsCountUsed; ++i)
            out << " " << std::hex << g_vcsCountAddr[i] << std::dec << "=" << g_vcsCountHits[i].load();
        std::cerr << out.str() << std::endl;
    }

    template <int N>
    void vcsCountWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint64_t hit = g_vcsCountHits[N].fetch_add(1u, std::memory_order_relaxed);
        if (hit < 4u)
        {
            std::cerr << "[calls:first] fn=0x" << std::hex << g_vcsCountAddr[N] << " a0=0x" << getRegU32(ctx, 4)
                      << " a1=0x" << getRegU32(ctx, 5) << " a2=0x" << getRegU32(ctx, 6) << " a3=0x" << getRegU32(ctx, 7)
                      << " ra=0x" << getRegU32(ctx, 31) << std::dec << std::endl;
        }
        vcsReportCounts();
        g_vcsCountOrig[N](rdram, ctx, runtime);
    }

    template <int... Is>
    constexpr std::array<PS2Runtime::RecompiledFunction, sizeof...(Is)> vcsMakeCountWrappers(std::integer_sequence<int, Is...>)
    {
        return {&vcsCountWrapper<Is>...};
    }

    void vcsInstallCallCounters(PS2Runtime &runtime)
    {
        const char *env = std::getenv("PS2X_COUNT_CALLS");
        if (!env)
            return;
        static const auto wrappers = vcsMakeCountWrappers(std::make_integer_sequence<int, kVcsMaxCounted>{});
        const char *cursor = env;
        while (*cursor && g_vcsCountUsed < kVcsMaxCounted)
        {
            char *end = nullptr;
            const uint32_t addr = static_cast<uint32_t>(std::strtoul(cursor, &end, 0));
            if (end == cursor)
                break;
            if (PS2Runtime::RecompiledFunction orig = runtime.lookupFunction(addr))
            {
                const int slot = g_vcsCountUsed++;
                g_vcsCountOrig[slot] = orig;
                g_vcsCountAddr[slot] = addr;
                runtime.replaceFunction(addr, wrappers[slot]);
            }
            cursor = (*end == ',') ? end + 1 : end;
        }
    }

    void applyGtaVcsUs(PS2Runtime &runtime)
    {
        g_vcsOrigMovieFinished = runtime.lookupFunction(0x0031A6A8u);
        runtime.replaceFunction(0x0031A6A8u, &vcsMovieFinished);

        runtime.replaceFunction(0x0031A5D0u, &vcsMoviePushEndCode);

        runtime.replaceFunction(0x00448D70u, &vcsGifPkAddGsAD);
        runtime.replaceFunction(0x00448CC8u, &vcsGifPkCloseGifTag);

        // libcdvd sceCdLayerSearchFile/sceCdSearchFile (one function in this
        // build). Natively it binds the CDVDFSV search RPC (sid 0x80000597),
        // which no IOP module provides, so the game spins in sceSifBindRpc.
        ps2_game_overrides::bindAddressHandler(runtime, 0x0044E180u, "sceCdSearchFile");

        // libmpeg sceMpegIsEnd (`return mpeg->sys->active == 0`): the HLE decoder never
        // updates that internal field, so route it to the HLE end-of-stream check.
        ps2_game_overrides::bindAddressHandler(runtime, 0x00452F68u, "sceMpegIsEnd");

        vcsInstallCallCounters(runtime);
    }
}

PS2_REGISTER_GAME_OVERRIDE("GTA Vice City Stories (SLUS_215.90)", "SLUS_215.90", 0x001DD6A0u, 0u, &applyGtaVcsUs)
