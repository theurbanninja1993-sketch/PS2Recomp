#include "ps2x_iso9660.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>

namespace ps2x
{
    namespace
    {
        uint32_t le32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

        bool seek64(std::FILE *f, uint64_t offset)
        {
#if defined(_WIN32)
            return _fseeki64(f, static_cast<long long>(offset), SEEK_SET) == 0;
#else
            return fseeko(f, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
        }

        std::string upper(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            return s;
        }
    }

    IsoImage::~IsoImage()
    {
        close();
    }

    void IsoImage::close()
    {
        if (m_file)
            std::fclose(m_file);
        m_file = nullptr;
        m_entries.clear();
    }

    bool IsoImage::readSectors(uint32_t lba, uint32_t count, uint8_t *out)
    {
        if (m_sectorSize == 2048u)
            return seek64(m_file, uint64_t(lba) * 2048u) && std::fread(out, 2048u, count, m_file) == count;
        for (uint32_t i = 0; i < count; ++i)
        {
            if (!seek64(m_file, uint64_t(lba + i) * m_sectorSize + m_dataOffset) ||
                std::fread(out + size_t(i) * 2048u, 2048u, 1, m_file) != 1)
                return false;
        }
        return true;
    }

    bool IsoImage::open(const std::string &imagePath, std::string &error)
    {
        close();
        m_file = std::fopen(imagePath.c_str(), "rb");
        if (!m_file)
        {
            error = "can't open " + imagePath;
            return false;
        }
        // Find the primary volume descriptor (sector 16) for each layout.
        struct Layout
        {
            uint32_t sectorSize, dataOffset;
        };
        const Layout layouts[] = {{2048u, 0u}, {2352u, 16u}, {2352u, 24u}};
        uint8_t pvd[2048];
        bool found = false;
        for (const Layout &layout : layouts)
        {
            m_sectorSize = layout.sectorSize;
            m_dataOffset = layout.dataOffset;
            if (readSectors(16u, 1u, pvd) && pvd[0] == 1u && std::memcmp(pvd + 1, "CD001", 5) == 0)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            close();
            error = "not an ISO9660 disc image";
            return false;
        }
        m_volumeId.assign(reinterpret_cast<const char *>(pvd + 40), 32);
        m_volumeId.erase(m_volumeId.find_last_not_of(' ') + 1);
        const uint8_t *root = pvd + 156;
        if (!readDirectory("", le32(root + 2), le32(root + 10), 0))
        {
            close();
            error = "can't read the disc's directories";
            return false;
        }
        return true;
    }

    bool IsoImage::readDirectory(const std::string &prefix, uint32_t lba, uint32_t size, int depth)
    {
        if (depth > 32 || size > 64u * 1024u * 1024u)
            return false;
        const uint32_t sectors = (size + 2047u) / 2048u;
        std::vector<uint8_t> data(size_t(sectors) * 2048u);
        if (!readSectors(lba, sectors, data.data()))
            return false;
        uint32_t pos = 0;
        while (pos < size)
        {
            const uint8_t length = data[pos];
            if (length == 0u)
            {
                pos = (pos / 2048u + 1u) * 2048u; // records don't cross sectors
                continue;
            }
            if (pos + length > data.size() || length < 34u)
                return false;
            const uint8_t *record = data.data() + pos;
            const uint8_t nameLength = record[32];
            const bool directory = (record[25] & 0x02u) != 0u;
            std::string name(reinterpret_cast<const char *>(record + 33), nameLength);
            pos += length;
            if (nameLength == 1u && (name[0] == '\0' || name[0] == '\1'))
                continue; // "." and ".."
            if (const auto semicolon = name.find(';'); semicolon != std::string::npos)
                name.resize(semicolon);
            if (!name.empty() && name.back() == '.')
                name.pop_back();
            IsoEntry entry;
            entry.path = prefix + "/" + name;
            entry.lba = le32(record + 2);
            entry.size = le32(record + 10);
            entry.directory = directory;
            m_entries.push_back(entry);
            if (directory && !readDirectory(entry.path, entry.lba, static_cast<uint32_t>(entry.size), depth + 1))
                return false;
        }
        return true;
    }

    const IsoEntry *IsoImage::find(const std::string &path) const
    {
        std::string wanted = upper(path);
        if (wanted.empty() || wanted[0] != '/')
            wanted = "/" + wanted;
        for (const IsoEntry &entry : m_entries)
            if (upper(entry.path) == wanted)
                return &entry;
        return nullptr;
    }

    bool IsoImage::readFile(const IsoEntry &entry, std::vector<uint8_t> &out, std::string &error)
    {
        out.assign(size_t((entry.size + 2047u) / 2048u) * 2048u, 0u);
        if (!readSectors(entry.lba, static_cast<uint32_t>(out.size() / 2048u), out.data()))
        {
            error = "read error in " + entry.path;
            return false;
        }
        out.resize(static_cast<size_t>(entry.size));
        return true;
    }

    bool IsoImage::extractFile(const IsoEntry &entry, const std::string &destPath, std::string &error,
                               const std::function<void(uint64_t)> &progress)
    {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(destPath).parent_path(), ec);
        std::FILE *out = std::fopen(destPath.c_str(), "wb");
        if (!out)
        {
            error = "can't write " + destPath;
            return false;
        }
        constexpr uint32_t kChunkSectors = 2048u; // 4 MB
        std::vector<uint8_t> buffer(size_t(kChunkSectors) * 2048u);
        uint64_t done = 0;
        bool ok = true;
        while (done < entry.size && ok)
        {
            const uint64_t left = entry.size - done;
            const uint32_t sectors = static_cast<uint32_t>(std::min<uint64_t>(kChunkSectors, (left + 2047u) / 2048u));
            ok = readSectors(entry.lba + static_cast<uint32_t>(done / 2048u), sectors, buffer.data());
            const size_t bytes = static_cast<size_t>(std::min<uint64_t>(left, uint64_t(sectors) * 2048u));
            ok = ok && std::fwrite(buffer.data(), 1, bytes, out) == bytes;
            done += bytes;
            if (progress)
                progress(done);
        }
        ok = std::fclose(out) == 0 && ok;
        if (!ok)
            error = "error extracting " + entry.path;
        return ok;
    }

    uint64_t IsoImage::totalFileBytes() const
    {
        uint64_t total = 0;
        for (const IsoEntry &entry : m_entries)
            if (!entry.directory)
                total += entry.size;
        return total;
    }

    std::string isoBootFile(IsoImage &image)
    {
        const IsoEntry *cnf = image.find("/SYSTEM.CNF");
        std::vector<uint8_t> data;
        std::string error;
        if (!cnf || !image.readFile(*cnf, data, error))
            return {};
        const std::string text(data.begin(), data.end());
        const auto boot = text.find("BOOT2");
        if (boot == std::string::npos)
            return {};
        const auto colon = text.find(':', boot);
        if (colon == std::string::npos)
            return {};
        size_t start = colon + 1;
        while (start < text.size() && (text[start] == '\\' || text[start] == '/'))
            ++start;
        size_t end = start;
        while (end < text.size() && text[end] != ';' && text[end] != '\r' && text[end] != '\n')
            ++end;
        return text.substr(start, end - start);
    }
}
