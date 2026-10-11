#ifndef PS2_MODS_H
#define PS2_MODS_H

#include <cstddef>
#include <string>
#include <vector>

// Drop-in mods: every folder in the mods folder is a mod; what it contains
// decides what it does. Supported so far: PCSX2 texture packs (replacement
// images named after the texture's hash, in any sub-folder).
struct PS2ModInfo
{
    std::string name; // folder name
    std::string path;
    bool enabled = true;
    size_t textures = 0;    // replacement textures
    size_t patches = 0;     // .pnach files (not applied yet)
    size_t otherFiles = 0;
};

// <mods folder>: PS2X_MODS_DIR, or "mods" next to the executable (created).
std::string ps2ModsDirectory();
// Rescans the mods folder (enabled state from the settings) and applies
// the enabled mods.
void ps2ModsRefresh();
const std::vector<PS2ModInfo> &ps2Mods();
// Enables / disables a mod (persisted in the settings) and re-applies.
void ps2ModsSetEnabled(const std::string &name, bool enabled);
// Opens the mods folder in the desktop file manager.
void ps2ModsOpenFolder();

#endif
