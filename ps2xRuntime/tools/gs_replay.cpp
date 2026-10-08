// Offline GS rasterizer benchmark. Replays a stream recorded with
// PS2X_GS_CAPTURE=<file>,<startFrame>,<frames> into a fresh GS (threaded CPU
// rasterizer, PS2X_GS_THREADS / PS2X_GS_SYNC as at runtime), times it, and
// prints a hash of the final local memory so rasterizer changes can be checked
// for identical output.
//
// usage: gs_replay <capture.bin> [passes]
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_memory.h"
#include "ps2_runtime.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

// The runtime library references the generated function table; no guest code runs here.
const uint32_t g_ps2RecompiledFunctionTableBase = 0;
const uint32_t g_ps2RecompiledFunctionTableEnd = 0;
const uint32_t g_ps2RecompiledFunctionTableSlotCount = 0;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[1] = {};

namespace
{
    struct Record
    {
        uint32_t type = 0;
        std::vector<uint8_t> data;
    };

    uint64_t fnv1a(const uint8_t *data, size_t size)
    {
        uint64_t hash = 1469598103934665603ull;
        for (size_t i = 0; i < size; ++i)
            hash = (hash ^ data[i]) * 1099511628211ull;
        return hash;
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
    std::vector<uint8_t> initialVram;
    if (FILE *f = std::fopen(argv[1], "rb"))
    {
        for (;;)
        {
            uint32_t header[2];
            if (std::fread(header, sizeof(header), 1, f) != 1)
                break;
            Record r;
            r.type = header[0];
            r.data.resize(header[1]);
            if (header[1] && std::fread(r.data.data(), 1, header[1], f) != header[1])
                break;
            if (r.type == 4u)
                initialVram = std::move(r.data);
            else
                records.push_back(std::move(r));
        }
        std::fclose(f);
    }
    size_t packets = 0, presents = 0;
    for (const Record &r : records)
    {
        packets += r.type == 1u;
        presents += r.type == 3u;
    }
    std::printf("%zu records (%zu GIF packets, %zu presents), initial VRAM %zu bytes\n", records.size(), packets,
                presents, initialVram.size());

    double total = 0.0;
    uint64_t hash = 0;
    for (int pass = 0; pass < passes; ++pass)
    {
        std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
        if (!initialVram.empty())
            std::memcpy(vram.data(), initialVram.data(), std::min(vram.size(), initialVram.size()));
        GSRegisters regs{};
        auto gs = std::make_unique<GS>();
        gs->init(vram.data(), static_cast<uint32_t>(vram.size()), &regs);

        const auto t0 = std::chrono::steady_clock::now();
        // GS_REPLAY_LIMIT=<n>: replay only the first n records (divergence bisection).
        static const size_t s_limit = std::getenv("GS_REPLAY_LIMIT") ? std::strtoull(std::getenv("GS_REPLAY_LIMIT"), nullptr, 10) : records.size();
        size_t replayed = 0;
        for (const Record &r : records)
        {
            if (replayed++ >= s_limit)
                break;
            switch (r.type)
            {
            case 1u:
                gs->processGIFPacket(r.data.data(), static_cast<uint32_t>(r.data.size()));
                break;
            case 2u:
                if (r.data.size() == 9u)
                {
                    uint64_t value = 0;
                    std::memcpy(&value, r.data.data() + 1, 8);
                    gs->writeRegister(r.data[0], value);
                }
                break;
            case 3u:
                if (r.data.size() == sizeof(GSRegisters))
                    std::memcpy(static_cast<void *>(&regs), r.data.data(), sizeof(GSRegisters));
                gs->latchHostPresentationFrame();
                break;
            default:
                break;
            }
        }
        gs.reset(); // workers finish every queued command before exiting
        total += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        hash = fnv1a(vram.data(), vram.size());
        if (const char *dump = std::getenv("GS_REPLAY_DUMP"))
            if (FILE *out = std::fopen(dump, "wb"))
            {
                std::fwrite(vram.data(), 1, vram.size(), out);
                std::fclose(out);
            }
        std::printf("pass %d: vramHash=%016llx\n", pass, static_cast<unsigned long long>(hash));
    }
    std::printf("time=%.3fs per pass=%.3fs (%.2f ms/present)\n", total, total / passes,
                presents ? 1000.0 * total / passes / static_cast<double>(presents) : 0.0);
    return 0;
}
