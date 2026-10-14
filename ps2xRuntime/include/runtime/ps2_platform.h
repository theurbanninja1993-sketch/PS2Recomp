#ifndef PS2_PLATFORM_H
#define PS2_PLATFORM_H

#include <filesystem>
#include <string>

// Small OS-specific helpers (ps2_platform.cpp keeps <windows.h> out of the
// files that use raylib).

// Folder of the running executable (falls back to the current directory).
std::filesystem::path ps2ExecutableDirectory();

// Where the port keeps settings, generated prompt art and logs: normally the
// user's config folder. In portable mode - a "portable.txt" next to the
// executable, or PS2X_PORTABLE=1 - a "config" folder next to the executable, so
// a build on a USB stick keeps its settings with it and touches nothing else.
// The folder is created if missing.
std::filesystem::path ps2ConfigDirectory();

// Opens a folder in the desktop's file manager.
void ps2OpenFolder(const std::string &path);

// Reports a fatal startup error: stderr, plus a message box on Windows
// (where the game has no console window).
void ps2ShowFatalError(const std::string &message);

#endif
