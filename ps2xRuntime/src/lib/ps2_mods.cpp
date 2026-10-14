#include "runtime/ps2_mods.h"
#include "runtime/ps2_platform.h"
#include "runtime/ps2_pad.h"

#include "runtime/gs/gs_texture_replacements.h"
#include "runtime/ps2_host_settings.h"
#include "runtime/ps2_memory.h" // PS2_RAM_SIZE
#include "runtime/ps2_prompt_art.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace
{
    std::vector<PS2ModInfo> g_mods;
    std::vector<PS2PnachEntry> g_pnach;

    bool lowerEndsWith(const std::string &s, const char *suffix)
    {
        std::string lower = s;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const std::string tail(suffix);
        return lower.size() >= tail.size() && lower.compare(lower.size() - tail.size(), tail.size(), tail) == 0;
    }

    std::string trim(std::string_view text)
    {
        size_t begin = 0;
        size_t end = text.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
            ++begin;
        while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
            --end;
        return std::string(text.substr(begin, end - begin));
    }
}

std::vector<PS2PnachEntry> ps2PnachParse(std::string_view text)
{
    std::vector<PS2PnachEntry> entries;
    std::istringstream lines{std::string(text)};
    std::string line;
    while (std::getline(lines, line))
    {
        const std::string stripped = trim(line);
        if (stripped.size() < 6 || stripped.compare(0, 6, "patch=") != 0)
            continue; // comments, "gametitle=...", blank lines
        // patch=ENABLE,CPU,ADDRESS,TYPE,VALUE
        std::string field;
        std::vector<std::string> fields;
        std::istringstream body(stripped.substr(6));
        while (std::getline(body, field, ','))
            fields.push_back(trim(field));
        if (fields.size() < 5 || fields[0] != "1")
            continue; // disabled patch
        // EE physical address: strip the 0x20xxxxxx uncached / 0x80xxxxxx /
        // 0xA0xxxxxx kernel mirrors, then require it to land in main RAM.
        const uint32_t address = static_cast<uint32_t>(std::strtoul(fields[2].c_str(), nullptr, 16)) & 0x1FFFFFFFu;
        if (address >= PS2_RAM_SIZE)
            continue; // MMIO registers / scratchpad
        uint8_t size = 0;
        const std::string &type = fields[3];
        if (type == "byte")
            size = 1;
        else if (type == "short")
            size = 2;
        else if (type == "word")
            size = 4;
        else
            continue; // "extended" / conditional codes are not supported
        if (address + size > PS2_RAM_SIZE)
            continue;
        const uint32_t value = static_cast<uint32_t>(std::strtoul(fields[4].c_str(), nullptr, 16));
        // Conditional/pointer code words (0xD0..0xDF, 0xE0..0xEF, 0x50..0x5F)
        // are program, not data; writing them would corrupt the game.
        if (size == 4 && ((value & 0xF0000000u) == 0xD0000000u || (value & 0xF0000000u) == 0xE0000000u ||
                          (value & 0xFF000000u) == 0x50000000u))
            continue;
        entries.push_back(PS2PnachEntry{address, size, value});
    }
    return entries;
}

std::string ps2ModsDirectory()
{
    static const std::string s_dir = []()
    {
        fs::path dir = std::getenv("PS2X_MODS_DIR") ? fs::path(std::getenv("PS2X_MODS_DIR")) : ps2ExecutableDirectory() / "mods";
        std::error_code ec;
        fs::create_directories(dir, ec);
        return dir.string();
    }();
    return s_dir;
}

std::string ps2PromptsDirectory()
{
    static const std::string s_dir = []()
    {
        fs::path dir = std::getenv("PS2X_PROMPTS_DIR") ? fs::path(std::getenv("PS2X_PROMPTS_DIR")) : ps2ExecutableDirectory() / "prompts";
        std::error_code ec;
        fs::create_directories(dir, ec); // so the Mods tab hint can point at it
        return dir.string();
    }();
    return s_dir;
}

namespace
{
    // Which prompts/<set>/ folder the setting resolves to ("" = none).
    std::string resolvedPromptSet(const PS2HostSettings &settings)
    {
        switch (settings.buttonPrompts)
        {
            case PS2HostSettings::ButtonPrompts::Off: return "";
            case PS2HostSettings::ButtonPrompts::Xbox: return "xbox";
            case PS2HostSettings::ButtonPrompts::Steam: return "steam";
            case PS2HostSettings::ButtonPrompts::Keyboard: return "keyboard";
            case PS2HostSettings::ButtonPrompts::DualSense: return "dualsense";
            case PS2HostSettings::ButtonPrompts::DualShock4: return "dualshock";
            case PS2HostSettings::ButtonPrompts::Switch: return "switch";
            case PS2HostSettings::ButtonPrompts::Auto: break;
        }
        switch (ps2InputDeviceKind())
        {
            case PS2InputDevice::Xbox: return "xbox";
            case PS2InputDevice::Steam: return "steam";
            case PS2InputDevice::DualSense: return "dualsense";
            case PS2InputDevice::DualShock: return "dualshock";
            case PS2InputDevice::Switch: return "switch";
            default: return "keyboard"; // the keyboard, or an unknown Xbox-style gamepad
        }
    }
} // namespace

namespace
{
    // The mods' replacement files, kept so the prompt layers can be rebuilt
    // without rescanning the (possibly huge) mod folders. That rescan on
    // every menu context flip was visible as the wrong prompt keys for about
    // a second when opening the map.
    std::shared_ptr<const gsrepl::Index> g_modsIndex;

    // The prompt set + runtime-generated art on top of the cached mods index.
    std::shared_ptr<const gsrepl::Index> ps2IndexWithPrompts()
    {
        const PS2HostSettings &settings = ps2HostSettings();
        std::error_code ec;
        auto index = std::make_shared<gsrepl::Index>();
        if (g_modsIndex)
            *index = *g_modsIndex;
        {
            const std::string set = resolvedPromptSet(settings);
            if (!set.empty())
            {
                const fs::path dir = fs::path(ps2PromptsDirectory()) / set;
                size_t count = 0, overridden = 0;
                for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
                     it != fs::recursive_directory_iterator(); it.increment(ec))
                {
                    if (ec)
                        break;
                    if (!it->is_regular_file(ec))
                        continue;
                    if (const auto key = gsrepl::parseFileName(it->path().filename().string()))
                    {
                        overridden += index->files.find(*key) != index->files.end() ? 1u : 0u;
                        index->files[*key] = it->path().string();
                        ++count;
                    }
                }
                if (count != 0u)
                    std::cerr << "[prompts] " << set << ": " << count << " textures (" << overridden
                              << " overriding mod packs)" << std::endl;
            }
        }
        {
            const std::string set = resolvedPromptSet(settings);
            if (!set.empty())
            {
                const fs::path dir = fs::path(ps2PromptArtDirectory()) / set;
                size_t count = 0;
                for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
                     it != fs::recursive_directory_iterator(); it.increment(ec))
                {
                    if (ec)
                        break;
                    if (!it->is_regular_file(ec))
                        continue;
                    if (const auto key = gsrepl::parseFileName(it->path().filename().string()))
                    {
                        index->files[*key] = it->path().string();
                        ++count;
                    }
                }
                if (count != 0u)
                    std::cerr << "[prompts] " << set << ": " << count << " runtime-generated textures" << std::endl;
            }
        }
        return index;
    }
} // namespace

void ps2ModsRefreshButtonPromptsIfNeeded()
{
    // Auto follows the active input device: rebuild the replacement index only
    // when the resolved set changes (rare; the rebuild rescan is expensive).
    static std::string s_last;
    const std::string resolved = resolvedPromptSet(ps2HostSettings());
    const bool setChanged = resolved != s_last;
    s_last = resolved;
    // Keyboard keycaps follow the live key bindings; regenerating writes the
    // runtime prompt-art folder, so the index must be rescanned too.
    const bool artChanged = ps2PromptArtRefreshIfNeeded();
    if (!setChanged && !artChanged)
        return;
    // Only the prompt layers changed: rebuild them over the cached mods index
    // instead of rescanning every mod folder (the full refresh runs at
    // startup, on Rescan and on mod toggles).
    auto index = ps2IndexWithPrompts();
    gsrepl::setCurrent(!index->files.empty() ? std::shared_ptr<const gsrepl::Index>(index) : nullptr);
}

std::string ps2CurrentPromptSet()
{
    return resolvedPromptSet(ps2HostSettings());
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

    auto index = std::make_shared<gsrepl::Index>(); // the mods layer; prompts load on top
    std::vector<PS2PnachEntry> pnach;
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
            {
                std::ifstream file(it->path(), std::ios::binary);
                if (!file)
                    continue;
                const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                std::vector<PS2PnachEntry> parsed = ps2PnachParse(text);
                mod.patches += parsed.size();
                if (mod.enabled)
                    pnach.insert(pnach.end(), parsed.begin(), parsed.end());
            }
            else
                ++mod.otherFiles;
        }
        std::cerr << "[mods] " << mod.name << (mod.enabled ? "" : " (disabled)") << ": " << mod.textures
                  << " replacement textures, " << mod.patches << " memory patches" << std::endl;
    }
    g_mods = std::move(mods);
    g_pnach = std::move(pnach);
    g_modsIndex = std::move(index);
    auto withPrompts = ps2IndexWithPrompts();
    gsrepl::setCurrent(!withPrompts->files.empty() ? std::shared_ptr<const gsrepl::Index>(withPrompts) : nullptr);
}

void ps2PnachApply(const std::vector<PS2PnachEntry> &entries, uint8_t *ram, size_t ramSize)
{
    if (ram == nullptr)
        return;
    for (const PS2PnachEntry &entry : entries)
    {
        if (entry.size == 0u || entry.address + entry.size > ramSize)
            continue;
        std::memcpy(ram + entry.address, &entry.value, entry.size);
    }
}

void ps2ModsApplyPatches(uint8_t *rdram)
{
    if (rdram == nullptr || g_pnach.empty() || !ps2HostSettings().pnachCheats)
        return;
    ps2PnachApply(g_pnach, rdram, PS2_RAM_SIZE);
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
    ps2OpenFolder(ps2ModsDirectory());
}
