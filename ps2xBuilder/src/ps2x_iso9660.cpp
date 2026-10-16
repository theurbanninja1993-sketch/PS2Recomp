#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

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
#if defined(_WIN32)
        if (m_device)
            CloseHandle(static_cast<HANDLE>(m_device));
#endif
        m_device = nullptr;
        m_entries.clear();
        m_readError.clear();
    }

    bool IsoImage::readSectors(uint32_t lba, uint32_t count, uint8_t *out)
    {
#if defined(_WIN32)
        if (m_device)
        {
            // Raw device reads must be whole, aligned sectors (out is sized in sectors).
            LARGE_INTEGER offset;
            offset.QuadPart = static_cast<LONGLONG>(uint64_t(lba) * 2048u);
            if (!SetFilePointerEx(static_cast<HANDLE>(m_device), offset, nullptr, FILE_BEGIN))
            {
                m_readError = "can't seek the disc (Windows error " + std::to_string(GetLastError()) + ")";
                return false;
            }
            DWORD read = 0;
            const DWORD bytes = count * 2048u;
            if (!ReadFile(static_cast<HANDLE>(m_device), out, bytes, &read, nullptr) || read != bytes)
            {
                const DWORD code = GetLastError();
                m_readError = (code == 21 || code == 1155)
                                  ? "the disc is not ready - it can need a few seconds after insertion"
                                  : "can't read the disc (Windows error " + std::to_string(code) + ")";
                return false;
            }
            return true;
        }
#endif
        bool ok = false;
        if (m_sectorSize == 2048u)
        {
            ok = seek64(m_file, uint64_t(lba) * 2048u) && std::fread(out, 2048u, count, m_file) == count;
        }
        else
        {
            ok = true;
            for (uint32_t i = 0; i < count && ok; ++i)
                ok = seek64(m_file, uint64_t(lba + i) * m_sectorSize + m_dataOffset) &&
                     std::fread(out + size_t(i) * 2048u, 2048u, 1, m_file) == 1;
        }
        if (!ok)
            m_readError = "can't read the image at sector " + std::to_string(lba);
        return ok;
    }

    bool IsoImage::open(const std::string &imagePath, std::string &error)
    {
        close();
#if defined(_WIN32)
        // "D:" (or "D:\\") names a drive: open the volume itself.
        if ((imagePath.size() == 2 || imagePath.size() == 3) && imagePath[1] == ':')
        {
            const std::wstring device = L"\\\\.\\" + std::wstring(1, static_cast<wchar_t>(imagePath[0])) + L":";
            HANDLE handle = CreateFileW(device.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
            {
                // The Windows error is the useful part: it separates "no disc"
                // from "not allowed", which otherwise look identical.
                const DWORD code = GetLastError();
                error = "can't open drive " + imagePath + " (Windows error " + std::to_string(code);
                if (code == 5)
                    error += ": access denied - try running this as administrator";
                else if (code == 2)
                    error += ": no such drive";
                else if (code == 21 || code == 1155)
                    error += ": the drive is not ready - no disc, or still spinning up";
                else
                    error += ": is a disc inserted?";
                error += ")";
                return false;
            }
            m_device = handle;
        }
        else
#endif
            m_file = std::fopen(imagePath.c_str(), "rb");
        if (!m_file && !m_device)
        {
            // errno is the difference between "no such file" and "not allowed".
            error = "can't open " + imagePath + " (" + std::strerror(errno) + ")";
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
            error = m_readError.empty() ? "not an ISO9660 disc image" : ("can't read the disc: " + m_readError);
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

    std::vector<std::string> opticalDrives()
    {
        std::vector<std::string> drives;
#if defined(_WIN32)
        const DWORD mask = GetLogicalDrives();
        for (int i = 0; i < 26; ++i)
        {
            if (!(mask & (1u << i)))
                continue;
            const wchar_t root[] = {static_cast<wchar_t>(L'A' + i), L':', L'\\', 0};
            if (GetDriveTypeW(root) == DRIVE_CDROM)
                drives.push_back(std::string(1, static_cast<char>('A' + i)) + ":");
        }
#else
        std::error_code ec;
        for (const auto &entry : std::filesystem::directory_iterator("/dev", ec))
        {
            const std::string name = entry.path().filename().string();
            if (name.size() > 2 && name.compare(0, 2, "sr") == 0 && std::isdigit(static_cast<unsigned char>(name[2])))
                drives.push_back(entry.path().string());
        }
        std::sort(drives.begin(), drives.end());
#endif
        return drives;
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
