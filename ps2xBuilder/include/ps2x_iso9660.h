#pragma once

// Minimal read-only ISO9660 reader for PS2 disc images: plain 2048-byte
// sector images (.iso) and raw 2352-byte CD images (.bin, mode 1 / mode 2).
// Used by the builder to verify and extract the player's own disc.

#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace ps2x
{
    struct IsoEntry
    {
        std::string path; // "/MODULES/IOPRP300.IMG" (no ";1" version suffix)
        uint32_t lba = 0;
        uint64_t size = 0;
        bool directory = false;
    };

    class IsoImage
    {
    public:
        IsoImage() = default;
        ~IsoImage();
        IsoImage(const IsoImage &) = delete;
        IsoImage &operator=(const IsoImage &) = delete;

        bool open(const std::string &imagePath, std::string &error);
        void close();

        // Every file and directory, depth first (directories before their contents).
        const std::vector<IsoEntry> &entries() const { return m_entries; }
        const IsoEntry *find(const std::string &path) const; // case-insensitive

        bool readFile(const IsoEntry &entry, std::vector<uint8_t> &out, std::string &error);
        // Streams a file to disk. progress(bytesDone) is called every few MB.
        bool extractFile(const IsoEntry &entry, const std::string &destPath, std::string &error,
                         const std::function<void(uint64_t)> &progress = {});

        uint64_t totalFileBytes() const;
        const std::string &volumeId() const { return m_volumeId; }

    private:
        bool readSectors(uint32_t lba, uint32_t count, uint8_t *out);
        bool readDirectory(const std::string &prefix, uint32_t lba, uint32_t size, int depth);

        std::FILE *m_file = nullptr;
        void *m_device = nullptr;       // Windows: HANDLE of an optical drive (sector-aligned reads)
        uint32_t m_sectorSize = 2048;   // bytes per sector in the image file
        uint32_t m_dataOffset = 0;      // user data offset inside a raw sector
        std::vector<IsoEntry> m_entries;
        std::string m_volumeId;
    };

    // Optical drives that can be passed to IsoImage::open: "/dev/sr0" on
    // Linux, "D:" on Windows. A PS2 DVD in a PC drive reads as plain ISO9660.
    std::vector<std::string> opticalDrives();

    // The PS2 boot file named by SYSTEM.CNF ("BOOT2 = cdrom0:\SLUS_215.90;1"),
    // or empty.
    std::string isoBootFile(IsoImage &image);
}
