// Offline VU1 replay benchmark. Replays MSCALs captured with
// PS2X_VU1_CAPTURE=<file>,<startFrame>,<count> and checks that the final VU1
// state, VU1 data memory and XGKICK output match the capture bit for bit.
//
// usage: vu1_replay <capture.bin> [passes]
#define private public // markVU1CodeModified() is a private PS2Memory helper
#include "runtime/ps2_memory.h"
#undef private
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_vu1.h"
#include "ps2_runtime.h"

#include <csignal>
#include <sys/time.h>
#include <ucontext.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// The runtime library references the generated function table; the replay
// tool runs no guest code.
const uint32_t g_ps2RecompiledFunctionTableBase = 0;
const uint32_t g_ps2RecompiledFunctionTableEnd = 0;
const uint32_t g_ps2RecompiledFunctionTableSlotCount = 0;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[1] = {};

namespace
{
    // VU1_PROFILE=<file>: SIGPROF samples of the host RIP, written as
    // "offset-from-profileHandler" u32s (map with tools/rip_hist.py).
    std::vector<uint32_t> g_samples;
    void profileHandler(int, siginfo_t *, void *contextPtr)
    {
        const auto *context = static_cast<const ucontext_t *>(contextPtr);
        if (g_samples.size() < g_samples.capacity())
            g_samples.push_back(static_cast<uint32_t>(context->uc_mcontext.gregs[REG_RIP] -
                                                      reinterpret_cast<uint64_t>(&profileHandler)));
    }

    struct Record
    {
        uint32_t startPC = 0, top = 0, itop = 0;
        VU1State before{};
        std::vector<uint8_t> code, dataBefore, dataAfter;
        VU1State after{};
    };

    bool sameState(const VU1State &a, const VU1State &b)
    {
        return std::memcmp(a.vf, b.vf, sizeof(a.vf)) == 0 && std::memcmp(a.vi, b.vi, sizeof(a.vi)) == 0 &&
               std::memcmp(a.acc, b.acc, sizeof(a.acc)) == 0 && std::memcmp(&a.q, &b.q, 4) == 0 &&
               std::memcmp(&a.p, &b.p, 4) == 0 && std::memcmp(&a.i, &b.i, 4) == 0 && a.r == b.r &&
               a.mac == b.mac && a.clip == b.clip && a.status == b.status && a.pc == b.pc;
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <capture.bin> [passes]\n", argv[0]);
        return 2;
    }
    const int passes = argc > 2 ? std::atoi(argv[2]) : 1;

    std::vector<Record> records;
    if (FILE *f = std::fopen(argv[1], "rb"))
    {
        for (;;)
        {
            uint32_t header[4];
            if (std::fread(header, sizeof(header), 1, f) != 1)
                break;
            if (header[0] != 0x31555643u)
            {
                std::fprintf(stderr, "bad record magic\n");
                return 1;
            }
            Record r;
            r.startPC = header[1];
            r.top = header[2];
            r.itop = header[3];
            r.code.resize(PS2_VU1_CODE_SIZE);
            r.dataBefore.resize(PS2_VU1_DATA_SIZE);
            r.dataAfter.resize(PS2_VU1_DATA_SIZE);
            if (std::fread(&r.before, sizeof(VU1State), 1, f) != 1 ||
                std::fread(r.code.data(), 1, r.code.size(), f) != r.code.size() ||
                std::fread(r.dataBefore.data(), 1, r.dataBefore.size(), f) != r.dataBefore.size() ||
                std::fread(&r.after, sizeof(VU1State), 1, f) != 1 ||
                std::fread(r.dataAfter.data(), 1, r.dataAfter.size(), f) != r.dataAfter.size())
                break; // truncated tail
            records.push_back(std::move(r));
        }
        std::fclose(f);
    }
    std::printf("%zu records\n", records.size());

    PS2Memory memory;
    if (!memory.initialize())
        return 1;
    GS gs;
    uint64_t gifHash = 1469598103934665603ull;
    uint64_t gifBytes = 0;
    memory.setGifPacketCallback([&](const uint8_t *data, uint32_t size)
                                {
                                    gifBytes += size;
                                    for (uint32_t i = 0; i < size; ++i)
                                        gifHash = (gifHash ^ data[i]) * 1099511628211ull; });
    VU1Interpreter vu1(VU1Interpreter::Unit::VU1);

    const char *profilePath = std::getenv("VU1_PROFILE");
    if (profilePath)
    {
        g_samples.reserve(1u << 22);
        struct sigaction action{};
        action.sa_sigaction = profileHandler;
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(SIGPROF, &action, nullptr);
        itimerval timer{{0, 100}, {0, 100}};
        setitimer(ITIMER_PROF, &timer, nullptr);
    }

    size_t mismatches = 0;
    double seconds = 0.0;
    for (int pass = 0; pass < passes; ++pass)
    {
        for (size_t index = 0; index < records.size(); ++index)
        {
            const Record &r = records[index];
            if (std::memcmp(memory.getVU1Code(), r.code.data(), PS2_VU1_CODE_SIZE) != 0)
            {
                std::memcpy(memory.getVU1Code(), r.code.data(), PS2_VU1_CODE_SIZE);
                memory.markVU1CodeModified();
            }
            std::memcpy(memory.getVU1Data(), r.dataBefore.data(), PS2_VU1_DATA_SIZE);
            vu1.state() = r.before;

            const auto t0 = std::chrono::steady_clock::now();
            vu1.execute(memory.getVU1Code(), PS2_VU1_CODE_SIZE, memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                        gs, &memory, r.startPC, r.top, r.itop, 65536);
            seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

            if (pass == 0 && (!sameState(vu1.state(), r.after) ||
                              std::memcmp(memory.getVU1Data(), r.dataAfter.data(), PS2_VU1_DATA_SIZE) != 0))
            {
                if (mismatches < 10)
                    std::printf("mismatch: record %zu pc=0x%x\n", index, r.startPC);
                ++mismatches;
            }
        }
    }
    std::printf("mismatches=%zu time=%.3fs (%.1f us/record) gifBytes=%llu gifHash=%016llx\n", mismatches, seconds,
                seconds * 1e6 / static_cast<double>(records.size() * passes), static_cast<unsigned long long>(gifBytes / passes),
                static_cast<unsigned long long>(gifHash));
    if (profilePath)
    {
        itimerval off{};
        setitimer(ITIMER_PROF, &off, nullptr);
        if (FILE *f = std::fopen(profilePath, "wb"))
        {
            std::fwrite(g_samples.data(), sizeof(uint32_t), g_samples.size(), f);
            std::fclose(f);
        }
        // <file>.maps for tools/prof_analyze.py (samples in shared libraries).
        if (FILE *f = std::fopen((std::string(profilePath) + ".maps").c_str(), "w"))
        {
            std::fprintf(f, "handler %llx\n", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&profileHandler)));
            if (FILE *maps = std::fopen("/proc/self/maps", "r"))
            {
                char line[512];
                while (std::fgets(line, sizeof(line), maps))
                    std::fputs(line, f);
                std::fclose(maps);
            }
            std::fclose(f);
        }
    }
    return mismatches == 0 ? 0 : 1;
}
