#ifndef PS2_PLATFORM_H
#define PS2_PLATFORM_H

#include <filesystem>
#include <string>

// Small OS-specific helpers (ps2_platform.cpp keeps <windows.h> out of the
// files that use raylib).

// Folder of the running executable (falls back to the current directory).
std::filesystem::path ps2ExecutableDirectory();

// Opens a folder in the desktop's file manager.
void ps2OpenFolder(const std::string &path);

// Reports a fatal startup error: stderr, plus a message box on Windows
// (where the game has no console window).
void ps2ShowFatalError(const std::string &message);

#endif
