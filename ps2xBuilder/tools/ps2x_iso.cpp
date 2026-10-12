// ps2x_iso: inspect / extract a PS2 disc image (test tool for the builder).
//   ps2x_iso info <image>              volume, boot file, file count, size
//   ps2x_iso list <image>              every entry
//   ps2x_iso hash <image> [path]       XXH3-128 of the boot file (or path)
//   ps2x_iso extract <image> <outdir>  every file, with progress
#include "ps2x_iso9660.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s info|list|hash|extract <image> [path|outdir]\n", argv[0]);
        return 2;
    }
    const std::string command = argv[1];
    ps2x::IsoImage image;
    std::string error;
    if (!image.open(argv[2], error))
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    if (command == "info")
    {
        size_t files = 0;
        for (const auto &entry : image.entries())
            files += entry.directory ? 0u : 1u;
        std::printf("volume: %s\nboot: %s\nfiles: %zu\nbytes: %" PRIu64 "\n", image.volumeId().c_str(),
                    ps2x::isoBootFile(image).c_str(), files, image.totalFileBytes());
        return 0;
    }
    if (command == "list")
    {
        for (const auto &entry : image.entries())
            std::printf("%s%s  %" PRIu64 "\n", entry.path.c_str(), entry.directory ? "/" : "", entry.size);
        return 0;
    }
    if (command == "hash")
    {
        const std::string path = argc > 3 ? argv[3] : "/" + ps2x::isoBootFile(image);
        const ps2x::IsoEntry *entry = image.find(path);
        std::vector<uint8_t> data;
        if (!entry || !image.readFile(*entry, data, error))
        {
            std::fprintf(stderr, "can't read %s\n", path.c_str());
            return 1;
        }
        const XXH128_hash_t h = XXH3_128bits(data.data(), data.size());
        std::printf("%016" PRIx64 "%016" PRIx64 "  %s\n", h.high64, h.low64, path.c_str());
        return 0;
    }
    if (command == "extract" && argc > 3)
    {
        const std::filesystem::path out = argv[3];
        const uint64_t total = image.totalFileBytes();
        uint64_t before = 0;
        int lastPercent = -1;
        for (const auto &entry : image.entries())
        {
            const auto dest = out / entry.path.substr(1);
            if (entry.directory)
            {
                std::error_code ec;
                std::filesystem::create_directories(dest, ec); // empty ones too (FRONTEND/EN/MC1, ...)
                continue;
            }
            if (!image.extractFile(entry, dest.string(), error,
                                   [&](uint64_t done)
                                   {
                                       const int percent = total ? static_cast<int>((before + done) * 100u / total) : 100;
                                       if (percent != lastPercent)
                                       {
                                           lastPercent = percent;
                                           std::fprintf(stderr, "\r%3d%%", percent);
                                       }
                                   }))
            {
                std::fprintf(stderr, "\n%s\n", error.c_str());
                return 1;
            }
            before += entry.size;
        }
        std::fprintf(stderr, "\rdone\n");
        return 0;
    }
    std::fprintf(stderr, "unknown command %s\n", command.c_str());
    return 2;
}
