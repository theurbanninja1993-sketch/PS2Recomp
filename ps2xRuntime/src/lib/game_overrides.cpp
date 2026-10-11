#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "ps2_runtime_calls.h"
#include "ps2_stubs.h"
#include "Kernel/Stubs/MPEG.h"
#include "ps2_syscalls.h"
#include "ps2_log.h"
#include "runtime/ps2_host_settings.h"
#include <algorithm>
#include <cmath>
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

    // Whether the player is driving: the player ped (found through one of a
    // few static pointers, validated by the back-pointer) points to its vehicle
    // at +0x480, and the vehicle points back to its driver at +0x384.
    bool vcsPlayerInVehicle(const uint8_t *rdram)
    {
        auto read32 = [rdram](uint32_t address) -> uint32_t
        {
            uint32_t value = 0;
            if (address >= 0x00100000u && address + 4u <= PS2_RAM_SIZE)
                std::memcpy(&value, rdram + address, 4);
            return value;
        };
        static constexpr uint32_t kPlayerPedPointers[] = {0x004E4910u, 0x00517834u, 0x006F4820u, 0x00744AC8u};
        for (const uint32_t pointer : kPlayerPedPointers)
        {
            const uint32_t ped = read32(pointer);
            if (ped == 0u)
                continue;
            const uint32_t vehicle = read32(ped + 0x480u);
            if (vehicle != 0u && read32(vehicle + 0x384u) == ped)
                return true;
        }
        return false;
    }

    // Direct mouse look on foot (follow camera 0x29DAB0, a0 = CCam). The
    // hook converts exactly the mouse motion since the last camera update
    // into the camera's own controls (see below), so there is no stick
    // ramp-up, dead zone, lag or drift.
    PS2Runtime::RecompiledFunction g_vcsOrigFollowPedCam = nullptr;
    PS2Runtime::RecompiledFunction g_vcsOrigFollowCarCam = nullptr;

    void vcsRunCameraWithMouse(PS2Runtime::RecompiledFunction original, uint8_t *rdram, R5900Context *ctx,
                               PS2Runtime *runtime)
    {
        // Mouse motion an update did not use (some camera calls change nothing).
        static float s_carryYaw = 0.0f, s_carryPitch = 0.0f;
        float yaw = 0.0f, pitch = 0.0f;
        const bool captured = runtime->padBackend().consumeMouseLook(yaw, pitch);
        yaw += s_carryYaw;
        pitch += s_carryPitch;
        s_carryYaw = s_carryPitch = 0.0f;
        const uint32_t cam = getRegU32(ctx, 4);
        const bool valid = cam >= 0x00100000u && cam + 0x240u <= PS2_RAM_SIZE;
        const bool apply = captured && (yaw != 0.0f || pitch != 0.0f) && valid;
        auto readF = [&](uint32_t off)
        {
            float v;
            std::memcpy(&v, rdram + cam + off, 4);
            return v;
        };
        auto writeF = [&](uint32_t off, float v) { std::memcpy(rdram + cam + off, &v, 4); };
        float frontBefore[3] = {};
        if (valid)
            std::memcpy(frontBefore, rdram + cam + 0x10u, 12);
        if (apply)
        {
            // Horizontal: one update turns by ~1.15x the speed (+0x140,
            // positive = left).
            constexpr float kAnglePerSpeed = 1.15f;
            writeF(0x140u, -yaw / kAnglePerSpeed);
            // Vertical: the stick moves a target camera height (+0x228/+0x238)
            // that the camera position (+0x28, copy +0x98) eases towards,
            // which lags. Moving both by the same amount makes it immediate;
            // looking up by a small angle lowers the camera by about its
            // horizontal distance to the target times the angle.
            uint32_t target = 0;
            std::memcpy(&target, rdram + cam + 0x70u, 4);
            if (pitch != 0.0f && target >= 0x00100000u && target + 0x40u <= PS2_RAM_SIZE)
            {
                float centre[3];
                std::memcpy(centre, rdram + target + 0x30u, 12);
                const float dx = readF(0x20u) - centre[0], dy = readF(0x24u) - centre[1];
                const float distance = std::sqrt(dx * dx + dy * dy);
                const float dz = -pitch * distance;
                if (std::isfinite(dz))
                    for (uint32_t off : {0x28u, 0x98u, 0x228u, 0x238u})
                        writeF(off, readF(off) + dz);
            }
        }
        float before[2] = {};
        static const bool s_debug = std::getenv("PS2X_PAD_DEBUG") != nullptr;
        if (s_debug && valid)
            std::memcpy(before, rdram + cam + 0x78u, 8);
        original(rdram, ctx, runtime);
        if (!valid)
            return;
        if (s_debug)
        {
            float after[2];
            std::memcpy(after, rdram + cam + 0x78u, 8);
            std::fprintf(stderr, "[mouselook] f=%llu want yaw %.4f pitch %.4f got beta %.4f alpha %.4f (alpha %.4f)\n",
                         (unsigned long long)g_ps2PresentedFrameCounter.load(), yaw, pitch, std::remainder(after[1] - before[1], 6.2831853f),
                         after[0] - before[0], after[0]);
        }
        if (apply)
        {
            writeF(0x140u, 0.0f);
            // A call that left the camera untouched did not turn it either.
            if (std::memcmp(frontBefore, rdram + cam + 0x10u, 12) == 0)
            {
                s_carryYaw = yaw;
                s_carryPitch = pitch;
            }
        }
    }

    void vcsFollowPedCam(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        vcsRunCameraWithMouse(g_vcsOrigFollowPedCam, rdram, ctx, runtime);
    }

    // Vehicle cameras have no stick look on the PS2, so the mouse adds a view
    // offset instead (like GTA IV/V): only the RenderWare camera is rotated
    // about the vehicle (see vcsFrameUpdate), so game logic never sees it.
    // (Wrapping CCamera::Process is unreliable: thread switches inside it
    // resume past a wrapper.) The offset eases back behind the vehicle once
    // the mouse rests.
    struct VcsCarLook
    {
        float yaw = 0.0f, pitch = 0.0f; // radians; yaw positive = look right
        uint32_t idleUpdates = 0;
        uint32_t vehicleCam = 0;        // CCam of the last vehicle-camera update
        uint64_t vehicleFrame = 0;      // presented frame of that update
    };
    VcsCarLook g_vcsCarLook;

    void vcsRotate(float v[3], const float axis[3], float angle)
    {
        // Rodrigues rotation about a unit axis.
        const float c = std::cos(angle), sn = std::sin(angle);
        const float dot = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
        const float cross[3] = {axis[1] * v[2] - axis[2] * v[1], axis[2] * v[0] - axis[0] * v[2], axis[0] * v[1] - axis[1] * v[0]};
        for (int i = 0; i < 3; ++i)
            v[i] = v[i] * c + cross[i] * sn + axis[i] * dot * (1.0f - c);
    }

    void vcsFollowCarCam(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        VcsCarLook &look = g_vcsCarLook;
        look.vehicleCam = getRegU32(ctx, 4);
        look.vehicleFrame = g_ps2PresentedFrameCounter.load(std::memory_order_relaxed);
        float yaw = 0.0f, pitch = 0.0f;
        const bool captured = runtime->padBackend().consumeMouseLook(yaw, pitch);
        if (captured && (yaw != 0.0f || pitch != 0.0f))
        {
            look.yaw = std::remainder(look.yaw + yaw, 6.2831853f);
            look.pitch = std::clamp(look.pitch + pitch, -0.6f, 0.9f);
            look.idleUpdates = 0;
        }
        else if (++look.idleUpdates > 45u) // ~1.5 s at the game's 30 Hz
        {
            look.yaw *= 0.9f;
            look.pitch *= 0.9f;
        }
        if (std::fabs(look.yaw) < 1.0e-3f && std::fabs(look.pitch) < 1.0e-3f)
            look.yaw = look.pitch = 0.0f;
        g_vcsOrigFollowCarCam(rdram, ctx, runtime);
    }

    // Rotates the RenderWare camera frame's modelling matrix (right +0x10,
    // up +0x20, at +0x30, pos +0x40) by the vehicle look offset.
    void vcsApplyCarLook(uint8_t *rdram, uint32_t frame)
    {
        VcsCarLook &look = g_vcsCarLook;
        const uint64_t now = g_ps2PresentedFrameCounter.load(std::memory_order_relaxed);
        if (now > look.vehicleFrame + 4u)
            look.yaw = look.pitch = 0.0f; // left the vehicle
        if (look.yaw == 0.0f && look.pitch == 0.0f)
            return;
        const uint32_t cam = look.vehicleCam;
        uint32_t target = 0;
        if (cam < 0x00100000u || cam + 0x80u > PS2_RAM_SIZE || frame < 0x00100000u || frame + 0x50u > PS2_RAM_SIZE)
            return;
        std::memcpy(&target, rdram + cam + 0x70u, 4);
        if (target < 0x00100000u || target + 0x40u > PS2_RAM_SIZE)
            return;
        float *m = reinterpret_cast<float *>(rdram + frame + 0x10u);
        float pivot[3];
        std::memcpy(pivot, rdram + target + 0x30u, 12);
        float right[3] = {m[0], m[1], m[2]};
        float up[3] = {m[4], m[5], m[6]};
        float at[3] = {m[8], m[9], m[10]};
        float offset[3] = {m[12] - pivot[0], m[13] - pivot[1], m[14] - pivot[2]};
        float pitchAxis[3] = {at[1], -at[0], 0.0f}; // horizontal, to the right
        const float axisLength = std::sqrt(pitchAxis[0] * pitchAxis[0] + pitchAxis[1] * pitchAxis[1]);
        if (!std::isfinite(axisLength) || axisLength < 1.0e-4f || !std::isfinite(offset[0] + offset[1] + offset[2]))
            return;
        pitchAxis[0] /= axisLength;
        pitchAxis[1] /= axisLength;
        const float zAxis[3] = {0.0f, 0.0f, 1.0f};
        // Pitch about the horizontal right axis, then yaw about Z (Z up:
        // looking right is clockwise seen from above).
        for (float *v : {right, up, at, offset})
        {
            vcsRotate(v, pitchAxis, look.pitch);
            vcsRotate(v, zAxis, -look.yaw);
        }
        for (int i = 0; i < 3; ++i)
        {
            m[i] = right[i];
            m[4 + i] = up[i];
            m[8 + i] = at[i];
            m[12 + i] = pivot[i] + offset[i];
        }
    }

    // Field of view: RwCameraSetViewWindow(camera, &viewWindow) (0x18F2D8;
    // view window = tan(FOV / 2) wide, at camera + 0x60) gets the game's
    // request scaled by settings FOV / 70, so zooms keep their relative
    // changes (cutscenes are left as they are). The game only sets it when its FOV changes:
    // the last request is kept to re-apply a new setting.
    PS2Runtime::RecompiledFunction g_vcsOrigSetViewWindow = nullptr;
    struct VcsFov
    {
        uint32_t camera = 0;
        float gameX = 0.0f, gameY = 0.0f; // the game's last request
        float appliedFov = 70.0f;
    };
    VcsFov g_vcsFov;

    // Cutscenes keep the original framing: TheCamera.Cams[0].Mode
    // (0x6F4540) is MODE_FLYBY (17) while one plays (4 = on foot).
    bool vcsCutsceneCamera(const uint8_t *rdram)
    {
        uint16_t mode = 0;
        std::memcpy(&mode, rdram + 0x006F4540u, sizeof(mode));
        return mode == 17u;
    }

    float vcsFovSetting(const uint8_t *rdram)
    {
        return vcsCutsceneCamera(rdram) ? 70.0f : std::clamp(ps2HostSettings().fieldOfView, 40.0f, 120.0f);
    }

    void vcsSetViewWindow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t camera = getRegU32(ctx, 4), viewWindow = getRegU32(ctx, 5);
        if (viewWindow >= 0x00100000u && viewWindow + 8u <= PS2_RAM_SIZE)
        {
            float vw[2];
            std::memcpy(vw, rdram + viewWindow, sizeof(vw));
            g_vcsFov.camera = camera;
            g_vcsFov.gameX = vw[0];
            g_vcsFov.gameY = vw[1];
            const float setting = vcsFovSetting(rdram);
            g_vcsFov.appliedFov = setting;
            if (setting != 70.0f && vw[0] > 0.0f && std::isfinite(vw[0]) && std::isfinite(vw[1]))
            {
                constexpr float kMaxHalfFov = 1.48f; // ~170 degrees
                const float half = std::min(std::atan(vw[0]) * (setting / 70.0f), kMaxHalfFov);
                const float x = std::tan(half);
                vw[1] *= x / vw[0];
                vw[0] = x;
                std::memcpy(rdram + viewWindow, vw, sizeof(vw));
            }
        }
        g_vcsOrigSetViewWindow(rdram, ctx, runtime);
    }

    // Draw distance (far part): RwCameraSetFarClipPlane(camera, far)
    // (0x18F400, far at camera + 0x7C) on the main camera gets the game's
    // value times the setting; kept for re-applying, like the FOV.
    PS2Runtime::RecompiledFunction g_vcsOrigSetFarClip = nullptr;
    float g_vcsGameFarClip = 0.0f, g_vcsAppliedDrawDistance = 1.0f;

    float vcsDrawDistanceSetting()
    {
        return std::clamp(ps2HostSettings().drawDistance, 0.5f, 4.0f);
    }

    void vcsSetFarClip(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t camera = getRegU32(ctx, 4);
        if (camera != 0u && camera == g_vcsFov.camera && std::isfinite(ctx->f[12]) && ctx->f[12] > 0.0f)
        {
            g_vcsGameFarClip = ctx->f[12];
            g_vcsAppliedDrawDistance = vcsDrawDistanceSetting();
            ctx->f[12] = g_vcsGameFarClip * g_vcsAppliedDrawDistance;
        }
        g_vcsOrigSetFarClip(rdram, ctx, runtime);
    }

    // Draw distance (detail part): CCam::Process (0x294540) ends with the
    // LOD distance multiplier, tan(35 deg) / tan(FOV / 2), at CCam + 0x138;
    // for TheCamera.Cams[0] (0x6F4540, the one in use) that is 0x6F4678,
    // read by the renderer after CCamera::Process. Only a freshly computed
    // value is scaled (an unchanged one was ours: it must not compound).
    float g_vcsLodWritten = 0.0f;

    void vcsScaleLodDistance(uint8_t *rdram)
    {
        constexpr uint32_t kLodMultiplier = 0x006F4678u;
        float value = 0.0f;
        std::memcpy(&value, rdram + kLodMultiplier, sizeof(value));
        if (value == g_vcsLodWritten || !std::isfinite(value) || value <= 0.0f || value > 100.0f)
            return;
        value *= vcsDrawDistanceSetting();
        std::memcpy(rdram + kLodMultiplier, &value, sizeof(value));
        g_vcsLodWritten = value;
    }

    // RwFrameUpdateObjects; CCamera::Process calls it (returning to
    // 0x37AE5C) right after copying TheCamera's matrix into the RenderWare
    // camera's frame, so the look offset reaches rendering only.
    PS2Runtime::RecompiledFunction g_vcsOrigFrameUpdate = nullptr;

    void vcsFrameUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (getRegU32(ctx, 31) == 0x0037AE5Cu)
        {
            vcsApplyCarLook(rdram, getRegU32(ctx, 4));
            vcsScaleLodDistance(rdram);
            // A new FOV setting: re-issue the game's last view window.
            const uint32_t camera = g_vcsFov.camera;
            if (g_vcsOrigSetViewWindow && camera >= 0x00100000u && camera + 0x70u <= PS2_RAM_SIZE &&
                g_vcsFov.appliedFov != vcsFovSetting(rdram))
            {
                const R5900Context saved = *ctx;
                const float vw[2] = {g_vcsFov.gameX, g_vcsFov.gameY};
                std::memcpy(rdram + camera + 0x60u, vw, sizeof(vw));
                SET_GPR_U32(ctx, 4, camera);
                SET_GPR_U32(ctx, 5, camera + 0x60u);
                vcsSetViewWindow(rdram, ctx, runtime);
                *ctx = saved;
            }
            if (g_vcsOrigSetFarClip && camera >= 0x00100000u && g_vcsGameFarClip > 0.0f &&
                g_vcsAppliedDrawDistance != vcsDrawDistanceSetting())
            {
                const R5900Context saved = *ctx;
                SET_GPR_U32(ctx, 4, camera);
                ctx->f[12] = g_vcsGameFarClip;
                vcsSetFarClip(rdram, ctx, runtime);
                *ctx = saved;
            }
        }
        g_vcsOrigFrameUpdate(rdram, ctx, runtime);
    }

    void applyGtaVcsUs(PS2Runtime &runtime)
    {
        if ((g_vcsOrigFollowPedCam = runtime.lookupFunction(0x0029DAB0u)))
            runtime.replaceFunction(0x0029DAB0u, &vcsFollowPedCam);
        if ((g_vcsOrigFollowCarCam = runtime.lookupFunction(0x0029FAF0u)))
            runtime.replaceFunction(0x0029FAF0u, &vcsFollowCarCam);
        if ((g_vcsOrigFrameUpdate = runtime.lookupFunction(0x001B4688u)))
            runtime.replaceFunction(0x001B4688u, &vcsFrameUpdate);
        if ((g_vcsOrigSetViewWindow = runtime.lookupFunction(0x0018F2D8u)))
            runtime.replaceFunction(0x0018F2D8u, &vcsSetViewWindow);
        if ((g_vcsOrigSetFarClip = runtime.lookupFunction(0x0018F400u)))
            runtime.replaceFunction(0x0018F400u, &vcsSetFarClip);

        runtime.padBackend().setVehicleProbe(&vcsPlayerInVehicle, runtime.memory().getRDRAM());

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
