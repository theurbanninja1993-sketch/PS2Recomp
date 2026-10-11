#ifndef PS2_HOST_SETTINGS_H
#define PS2_HOST_SETTINGS_H

#include <atomic>
#include <string>

// Player-facing host settings (display, mouse), edited in the in-game
// settings menu (F2) and kept in <config>/ps2recomp/settings.ini.
// Owned by the window thread; the game thread only reads menuOpen.
struct PS2HostSettings
{
    enum class DisplayMode : int
    {
        Windowed = 0,
        Borderless = 1,  // borderless window covering the monitor
        Fullscreen = 2,  // exclusive fullscreen at the desktop resolution
    };
    enum class Aspect : int
    {
        Standard = 0,   // 4:3, as on a PS2 TV
        Widescreen = 1, // 16:9: pair with the game's own widescreen option
        Stretch = 2,    // fill the window
    };

    DisplayMode displayMode = DisplayMode::Windowed;
    int windowWidth = 960;
    int windowHeight = 720;
    Aspect aspect = Aspect::Standard;
    bool smoothFiltering = true; // bilinear scaling of the picture (off: sharp pixels)
    int internalScale = 1;       // GPU renderer resolution multiplier (1 = native 640x448)
    bool flickerFilter = false;  // the game's interlace flicker filter (a vertical blur)
    float mouseSensitivity = 1.0f;
    bool mouseInvertY = false;
};

PS2HostSettings &ps2HostSettings();
// Reads the settings file (missing file: defaults); PS2X_MOUSE_SENS and
// PS2X_MOUSE_INVERT_Y still override the mouse values.
void ps2HostSettingsLoad();
void ps2HostSettingsSave();
std::string ps2HostSettingsPath();

// Window thread, once per frame: applies display mode / window size changes
// (made in the menu or by loading) to the host window.
void ps2HostSettingsApplyWindow();
void ps2HostSettingsRequestWindowApply();
// Pushes the renderer settings (internal resolution, flicker filter) to the GS.
void ps2HostSettingsApplyRenderer();
// The GPU renderer is in use (internal resolution only applies to it).
bool ps2HostSettingsGpuRenderer();

// The settings menu is open: the game gets neutral input and the mouse is free.
extern std::atomic<bool> g_ps2SettingsMenuOpen;

#endif
