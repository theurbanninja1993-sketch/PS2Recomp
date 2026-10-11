#include "runtime/ps2_mods.h"

#include "runtime/gs/gs_texture_replacements.h"
#include "runtime/ps2_host_settings.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <system_error>

namespace fs = std::filesystem;

namespace
{
    std::vector<PS2ModInfo> g_mods;

    fs::path executableDirectory()
    {
        std::error_code ec;
#if defined(_WIN32)
        return fs::current_path(ec);
#else
        const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
        return ec ? fs::current_path(ec) : exe.parent_path();
#endif
    }

    bool lowerEndsWith(const std::string &s, const char *suffix)
    {
        std::string lower = s;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const std::string tail(suffix);
        return lower.size() >= tail.size() && lower.compare(lower.size() - tail.size(), tail.size(), tail) == 0;
    }
}

std::string ps2ModsDirectory()
{
    static const std::string s_dir = []()
    {
        fs::path dir = std::getenv("PS2X_MODS_DIR") ? fs::path(std::getenv("PS2X_MODS_DIR")) : executableDirectory() / "mods";
        std::error_code ec;
        fs::create_directories(dir, ec);
        return dir.string();
    }();
    return s_dir;
}

const std::vector<PS2ModInfo> &ps2Mods()
{
    return g_mods;
}

void ps2ModsRefresh()
{
    const PS2HostSettings &settings = ps2HostSettings();
    std::vector<PS2ModInfo> mods;
    std::error_code ec;
    for (const fs::directory_entry &entry : fs::directory_iterator(ps2ModsDirectory(), ec))
    {
        if (!entry.is_directory(ec))
            continue;
        PS2ModInfo mod;
        mod.name = entry.path().filename().string();
        mod.path = entry.path().string();
        mod.enabled = std::find(settings.disabledMods.begin(), settings.disabledMods.end(), mod.name) == settings.disabledMods.end();
        mods.push_back(std::move(mod));
    }
    // Load order: alphabetical; a later mod wins where two replace the same thing.
    std::sort(mods.begin(), mods.end(), [](const PS2ModInfo &a, const PS2ModInfo &b) { return a.name < b.name; });

    auto index = std::make_shared<gsrepl::Index>();
    for (PS2ModInfo &mod : mods)
    {
        for (auto it = fs::recursive_directory_iterator(mod.path, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            if (ec)
                break;
            if (!it->is_regular_file(ec))
                continue;
            const std::string fileName = it->path().filename().string();
            if (const auto key = gsrepl::parseFileName(fileName))
            {
                ++mod.textures;
                if (mod.enabled)
                    index->files[*key] = it->path().string();
            }
            else if (lowerEndsWith(fileName, ".pnach"))
                ++mod.patches;
            else
                ++mod.otherFiles;
        }
        std::cerr << "[mods] " << mod.name << (mod.enabled ? "" : " (disabled)") << ": " << mod.textures
                  << " replacement textures" << std::endl;
    }
    g_mods = std::move(mods);
    gsrepl::setCurrent(settings.textureReplacement && !index->files.empty() ? std::shared_ptr<const gsrepl::Index>(index)
                                                                            : nullptr);
}

void ps2ModsSetEnabled(const std::string &name, bool enabled)
{
    PS2HostSettings &settings = ps2HostSettings();
    auto &disabled = settings.disabledMods;
    disabled.erase(std::remove(disabled.begin(), disabled.end(), name), disabled.end());
    if (!enabled)
        disabled.push_back(name);
    ps2HostSettingsSave();
    ps2ModsRefresh();
}

void ps2ModsOpenFolder()
{
    const std::string dir = ps2ModsDirectory();
#if defined(_WIN32)
    const std::string command = "explorer \"" + dir + "\"";
#elif defined(__APPLE__)
    const std::string command = "open \"" + dir + "\" &";
#else
    const std::string command = "xdg-open \"" + dir + "\" >/dev/null 2>&1 &";
#endif
    if (std::system(command.c_str()) != 0)
        std::cerr << "[mods] could not open " << dir << std::endl;
}
