#include "runtime/ps2_host_settings.h"

#include "raylib.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_gpu_backend.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

std::atomic<bool> g_ps2SettingsMenuOpen{false};

namespace
{
    PS2HostSettings g_settings;
    bool g_windowApplyPending = false;

    int clampMode(int value) { return std::clamp(value, 0, 2); }
}

PS2HostSettings &ps2HostSettings()
{
    return g_settings;
}

std::string ps2HostSettingsPath()
{
    std::filesystem::path base;
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        base = xdg;
    else if (const char *appData = std::getenv("APPDATA"); appData && *appData)
        base = appData;
    else if (const char *home = std::getenv("HOME"); home && *home)
        base = std::filesystem::path(home) / ".config";
    else
        base = ".";
    return (base / "ps2recomp" / "settings.ini").string();
}

void ps2HostSettingsLoad()
{
    PS2HostSettings &s = g_settings;
    std::ifstream in(ps2HostSettingsPath());
    std::string line;
    while (std::getline(in, line))
    {
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == '#' || line[0] == ';' || eq == std::string::npos)
            continue;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        const char *v = value.c_str();
        if (key == "display_mode")
            s.displayMode = static_cast<PS2HostSettings::DisplayMode>(clampMode(std::atoi(v)));
        else if (key == "window_width")
            s.windowWidth = std::clamp(std::atoi(v), 320, 16384);
        else if (key == "window_height")
            s.windowHeight = std::clamp(std::atoi(v), 240, 16384);
        else if (key == "aspect")
            s.aspect = static_cast<PS2HostSettings::Aspect>(clampMode(std::atoi(v)));
        else if (key == "smooth_filtering")
            s.smoothFiltering = std::atoi(v) != 0;
        else if (key == "mouse_sensitivity")
            s.mouseSensitivity = std::clamp(std::strtof(v, nullptr), 0.05f, 20.0f);
        else if (key == "internal_scale")
            s.internalScale = std::clamp(std::atoi(v), 1, 4);
        else if (key == "flicker_filter")
            s.flickerFilter = std::atoi(v) != 0;
        else if (key == "mouse_invert_y")
            s.mouseInvertY = std::atoi(v) != 0;
    }
    if (const char *env = std::getenv("PS2X_MOUSE_SENS"))
        s.mouseSensitivity = std::clamp(std::strtof(env, nullptr), 0.05f, 20.0f);
    if (std::getenv("PS2X_MOUSE_INVERT_Y"))
        s.mouseInvertY = true;
    g_windowApplyPending = true;
    ps2HostSettingsApplyRenderer();
}

void ps2HostSettingsApplyRenderer()
{
    // PS2X_GS_SCALE (tests) wins over the saved value at startup.
    static const char *s_envScale = std::getenv("PS2X_GS_SCALE");
    static bool s_first = true;
    if (!(s_first && s_envScale))
        g_gsGpuScaleRequest.store(static_cast<uint32_t>(g_settings.internalScale), std::memory_order_relaxed);
    s_first = false;
    g_gsFlickerFilter.store(g_settings.flickerFilter, std::memory_order_relaxed);
}

bool ps2HostSettingsGpuRenderer()
{
    static const bool s_gpu = std::getenv("PS2X_GS_GPU") != nullptr;
    return s_gpu;
}

void ps2HostSettingsSave()
{
    const PS2HostSettings &s = g_settings;
    const std::string path = ps2HostSettingsPath();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    if (!out)
    {
        std::fprintf(stderr, "[settings] cannot write %s\n", path.c_str());
        return;
    }
    out << "# PS2Recomp host settings (also editable in game: F2)\n"
        << "display_mode=" << static_cast<int>(s.displayMode) << "\n"
        << "window_width=" << s.windowWidth << "\n"
        << "window_height=" << s.windowHeight << "\n"
        << "aspect=" << static_cast<int>(s.aspect) << "\n"
        << "smooth_filtering=" << (s.smoothFiltering ? 1 : 0) << "\n"
        << "internal_scale=" << s.internalScale << "\n"
        << "flicker_filter=" << (s.flickerFilter ? 1 : 0) << "\n"
        << "mouse_sensitivity=" << s.mouseSensitivity << "\n"
        << "mouse_invert_y=" << (s.mouseInvertY ? 1 : 0) << "\n";
}

void ps2HostSettingsRequestWindowApply()
{
    g_windowApplyPending = true;
}

void ps2HostSettingsApplyWindow()
{
    PS2HostSettings &s = g_settings;
    if (!g_windowApplyPending)
    {
        // Remember a window resized by hand.
        if (s.displayMode == PS2HostSettings::DisplayMode::Windowed && !IsWindowFullscreen() &&
            !IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE) && IsWindowResized())
        {
            s.windowWidth = GetScreenWidth();
            s.windowHeight = GetScreenHeight();
        }
        return;
    }
    g_windowApplyPending = false;

    // Leave the current mode first, then enter the requested one.
    if (IsWindowFullscreen() && s.displayMode != PS2HostSettings::DisplayMode::Fullscreen)
        ToggleFullscreen();
    if (IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE) && s.displayMode != PS2HostSettings::DisplayMode::Borderless)
        ToggleBorderlessWindowed();

    switch (s.displayMode)
    {
    case PS2HostSettings::DisplayMode::Windowed:
    {
        const int monitor = GetCurrentMonitor();
        const int width = std::min(s.windowWidth, std::max(320, GetMonitorWidth(monitor)));
        const int height = std::min(s.windowHeight, std::max(240, GetMonitorHeight(monitor)));
        SetWindowSize(width, height);
        const Vector2 monitorPos = GetMonitorPosition(monitor);
        SetWindowPosition(static_cast<int>(monitorPos.x) + (GetMonitorWidth(monitor) - width) / 2,
                          static_cast<int>(monitorPos.y) + (GetMonitorHeight(monitor) - height) / 2);
        break;
    }
    case PS2HostSettings::DisplayMode::Borderless:
        if (!IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE))
            ToggleBorderlessWindowed();
        break;
    case PS2HostSettings::DisplayMode::Fullscreen:
        if (!IsWindowFullscreen())
        {
            const int monitor = GetCurrentMonitor();
            SetWindowSize(GetMonitorWidth(monitor), GetMonitorHeight(monitor));
            ToggleFullscreen();
        }
        break;
    }
}
