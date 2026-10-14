#include "runtime/ps2_pad.h"
#include "runtime/ps2_mods.h"
#include "runtime/ps2_prompt_art.h"

extern std::atomic<bool> g_ps2DumpRamRequest; // ps2_runtime.cpp
#include "runtime/ps2_host_settings.h"
#include "runtime/ps2_music_station.h"
#include "runtime/ps2_audio.h"
#include "ps2_host_backend.h"
#include <atomic>
#include <mutex>

extern std::atomic<uint64_t> g_ps2PresentedFrameCounter;
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

// GLFW's gamepad mappings (from SDL's database, generated into mappings.h at
// build time) only carry the device revisions it knew about. A pad whose
// firmware reports a different revision matches nothing: it is detected, but
// every button and axis reads zero. GLFW can be handed extra mappings at
// runtime, which is how those are fixed without rebuilding.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
    constexpr uint8_t kPadStickCenter = 0x80;

}

namespace
{
    // Select+Start together opens the settings menu, so neither reaches the
    // game as part of the combo: a lone Select / Start is passed on once it
    // has been held 50 ms (a shorter tap is replayed for 100 ms on release).
    void filterMenuCombo(bool &select, bool &start)
    {
        struct Held
        {
            double since = -1.0;    // pressed at (not yet passed on)
            bool passed = false;    // passed on to the game
            double pulseUntil = -1.0;
        };
        static Held s_held[2];
        static bool s_combo = false;
        const double now = GetTime();
        bool *buttons[2] = {&select, &start};
        if (select && start)
            s_combo = true;
        if (s_combo)
        {
            if (!select && !start)
                s_combo = false;
            for (Held &held : s_held)
                held = {};
            select = start = false;
            return;
        }
        constexpr double kGrace = 0.05, kPulse = 0.1;
        for (int i = 0; i < 2; ++i)
        {
            Held &held = s_held[i];
            bool &down = *buttons[i];
            if (down)
            {
                if (held.since < 0.0)
                    held.since = now;
                held.passed = held.passed || now - held.since >= kGrace;
                down = held.passed;
            }
            else
            {
                if (held.since >= 0.0 && !held.passed)
                    held.pulseUntil = now + kPulse; // a quick tap
                held.since = -1.0;
                held.passed = false;
                down = now < held.pulseUntil;
            }
        }
    }
}

namespace
{
    std::mutex g_preferredPadMutex;
    std::string g_preferredPad;
}

void ps2SetPreferredGamepad(const std::string &name)
{
    std::lock_guard lock(g_preferredPadMutex);
    g_preferredPad = name;
}

// Adds mappings GLFW does not ship, plus an optional user file. Must run with
// GLFW initialised - raylib does that in InitWindow - because
// glfwUpdateGamepadMappings only re-evaluates already-connected joysticks and
// returns GLFW_FALSE while GLFW is down. It is called again on every
// ps2ActiveGamepad() call until it takes, and explicitly right after
// InitWindow, because prompt setup runs earlier during boot.
void ps2ApplyGamepadMappings()
{
    static bool s_done = false;
    if (s_done)
        return;
    // Xbox One S pad over USB reporting firmware revision 0x0517: the database
    // has 0x0408 / 0x0301 / 0x0000 for product 0x02ea on Linux, so this one
    // matched nothing (owner's pad, 2026-10-14). Body copied from the closest
    // sibling ("Microsoft Xbox One S pad - Wired").
    static const char *kSupplement =
        "030000005e040000ea02000017050000,Microsoft X-Box One S pad,a:b0,b:b1,back:b6,dpdown:h0.4,dpleft:h0.8,"
        "dpright:h0.2,dpup:h0.1,guide:b8,leftshoulder:b4,leftstick:b9,lefttrigger:a2,leftx:a0,lefty:a1,rightshoulder:b5,"
        "rightstick:b10,righttrigger:a5,rightx:a3,righty:a4,start:b7,x:b2,y:b3,platform:Linux,\n"
        // A pad reporting no revision at all for the same product.
        "030000005e040000ea02000000000000,Xbox One S pad (no revision),a:b0,b:b1,back:b6,dpdown:h0.4,dpleft:h0.8,"
        "dpright:h0.2,dpup:h0.1,guide:b8,leftshoulder:b4,leftstick:b9,lefttrigger:a2,leftx:a0,lefty:a1,rightshoulder:b5,"
        "rightstick:b10,righttrigger:a5,rightx:a3,righty:a4,start:b7,x:b2,y:b3,platform:Linux,\n";
    // One mapping per line: GLFW splits on newlines, so two entries sharing a
    // line are parsed as a single (wrong) mapping. A false return means GLFW is
    // not up yet, so do not latch - retry on the next call.
    if (!glfwUpdateGamepadMappings(kSupplement))
        return;
    s_done = true;
    std::fprintf(stderr, "[pad] extra gamepad mappings loaded\n");
    // <config>/ps2recomp/gamecontrollerdb.txt: SDL-style lines to drop in for
    // pads GLFW does not know (any entry whose guid matches a connected pad).
    std::error_code error;
    const std::filesystem::path userFile =
        std::filesystem::path(ps2HostSettingsPath()).parent_path() / "gamecontrollerdb.txt";
    if (std::filesystem::exists(userFile, error))
    {
        if (FILE *f = std::fopen(userFile.string().c_str(), "rb"))
        {
            std::string text;
            char buffer[4096];
            size_t read = 0;
            while ((read = std::fread(buffer, 1, sizeof(buffer), f)) > 0)
                text.append(buffer, read);
            std::fclose(f);
            if (!text.empty())
                std::fprintf(stderr, "[pad] %s %s\n", userFile.string().c_str(),
                             glfwUpdateGamepadMappings(text.c_str()) ? "loaded" : "rejected");
        }
    }
}

// A device the game can actually read: present *and* recognised by GLFW's
// gamepad database. Keyboards and mice often expose phantom joystick nodes (the
// owner's Razer keyboard takes two slots) and those must never be auto-selected:
// they report no input at all, which looks exactly like a controller that is
// detected but dead. glfwJoystickIsGamepad is safe to call before/after init.
bool ps2IsRealGamepad(int pad)
{
    return IsGamepadAvailable(pad) && glfwJoystickIsGamepad(pad) != 0;
}

int ps2ActiveGamepad()
{
    ps2ApplyGamepadMappings();
    int active = -1;
    {
        std::lock_guard lock(g_preferredPadMutex);
        if (!g_preferredPad.empty())
        {
            // The pinned name first among real gamepads, then among any joystick
            // (an unrecognised pad is honoured, but say so: it will not respond
            // until it has a mapping).
            for (int i = 0; i < 4 && active < 0; ++i)
                if (ps2IsRealGamepad(i) && g_preferredPad == GetGamepadName(i))
                    active = i;
            for (int i = 0; i < 4 && active < 0; ++i)
                if (IsGamepadAvailable(i) && g_preferredPad == GetGamepadName(i))
                {
                    std::fprintf(stderr,
                                 "[pad] '%s' is connected but GLFW has no mapping for it; "
                                 "it will not respond\n",
                                 g_preferredPad.c_str());
                    active = i;
                }
        }
    }
    if (active < 0)
    {
        // Automatic: the controller last used. Only real gamepads count, so a
        // keyboard or mouse can never win a slot.
        static std::atomic<int> s_auto{-1};
        // Sticks count as used when moved from where they rested when first
        // seen (some devices rest at -1).
        static float s_rest[4][4];
        static bool s_restKnown[4]{};
        auto used = [](int pad)
        {
            if (!s_restKnown[pad])
            {
                for (int axis = 0; axis < 4; ++axis)
                    s_rest[pad][axis] = GetGamepadAxisMovement(pad, axis);
                s_restKnown[pad] = true;
            }
            for (int button = GAMEPAD_BUTTON_LEFT_FACE_UP; button <= GAMEPAD_BUTTON_RIGHT_THUMB; ++button)
                if (IsGamepadButtonDown(pad, button))
                    return true;
            for (int axis = 0; axis < 4; ++axis)
                if (std::fabs(GetGamepadAxisMovement(pad, axis) - s_rest[pad][axis]) > 0.5f)
                    return true;
            return false;
        };
        int current = s_auto.load(std::memory_order_relaxed);
        if (current < 0 || !ps2IsRealGamepad(current) || !used(current))
            for (int i = 0; i < 4; ++i)
                if (i != current && ps2IsRealGamepad(i) && used(i))
                {
                    current = i;
                    break;
                }
        if (current >= 0 && !ps2IsRealGamepad(current))
            current = -1;
        for (int i = 0; i < 4 && current < 0; ++i)
            if (ps2IsRealGamepad(i))
                current = i;
        s_auto.store(current, std::memory_order_relaxed);
        active = current;
    }
    static std::atomic<int> s_logged{-2};
    if (s_logged.exchange(active, std::memory_order_relaxed) != active)
    {
        std::string all;
        for (int i = 0; i < 4; ++i)
            if (IsGamepadAvailable(i))
                all += " [" + std::to_string(i) + "] " + GetGamepadName(i) +
                       (ps2IsRealGamepad(i) ? "" : " (not a gamepad - ignored)");
        if (active >= 0)
            std::fprintf(stderr, "[pad] using controller %d: %s (connected:%s)\n", active, GetGamepadName(active), all.c_str());
        else
            std::fprintf(stderr, "[pad] no controller (connected:%s)\n", all.empty() ? " none" : all.c_str());
    }
    return active;
}

// What a connected pad is, from its name: GLFW's own name when the pad is in
// its database, the name from our supplement otherwise ("Microsoft X-Box One
// S pad", hence the hyphenated spelling below).
static PS2InputDevice gamepadDeviceKind(int pad)
{
    if (pad < 0)
        return PS2InputDevice::Keyboard;
    std::string name = GetGamepadName(pad);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name.find("xbox") != std::string::npos || name.find("x-box") != std::string::npos)
        return PS2InputDevice::Xbox;
    if (name.find("steam") != std::string::npos)
        return PS2InputDevice::Steam;
    if (name.find("switch") != std::string::npos || name.find("pro controller") != std::string::npos)
        return PS2InputDevice::Switch;
    if (name.find("dualsense") != std::string::npos || name.find("ps5") != std::string::npos)
        return PS2InputDevice::DualSense;
    if (name.find("dualshock") != std::string::npos || name.find("playstation") != std::string::npos ||
        name.find("ps4") != std::string::npos || name.find("ps3") != std::string::npos)
        return PS2InputDevice::DualShock;
    return PS2InputDevice::Generic; // e.g. 8BitDo etc.: Xbox-style ABXY layout
}

// The device the player last used is what the automatic prompts follow: the
// keyboard while typing, the pad as soon as it is touched. -1 until something
// is used (then the active pad, or the keyboard with no pad at all).
std::atomic<int> g_lastInputDevice{-1};

PS2InputDevice ps2InputDeviceKind()
{
    const int last = g_lastInputDevice.load(std::memory_order_relaxed);
    if (last >= 0)
        return static_cast<PS2InputDevice>(last);
    return gamepadDeviceKind(ps2ActiveGamepad());
}

bool PSPadBackend::readState(int /*port*/, int /*slot*/, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;

    // Auto prompts follow the last used device, not the first connected one:
    // any bound key / mouse button / the wheel means the keyboard, a pad
    // button press or a stick moved past half way means that pad.
    {
        const PS2HostSettings &settings = ps2HostSettings();
        const int pad = ps2ActiveGamepad();
        bool padUsed = false;
        if (pad >= 0)
        {
            for (int b = GAMEPAD_BUTTON_LEFT_FACE_UP; b <= GAMEPAD_BUTTON_LEFT_THUMB && !padUsed; ++b)
                padUsed = IsGamepadButtonPressed(pad, static_cast<GamepadButton>(b));
            for (int a = GAMEPAD_AXIS_LEFT_X; a <= GAMEPAD_AXIS_RIGHT_Y && !padUsed; ++a)
                padUsed = std::fabs(GetGamepadAxisMovement(pad, static_cast<GamepadAxis>(a))) > 0.5f;
        }
        bool keyboardUsed = false;
        for (int a = 0; a < ps2controls::kActionCount && !keyboardUsed; ++a)
        {
            for (int slot = 0; slot < 2 && !keyboardUsed; ++slot)
            {
                const int binding = settings.keys[a][slot];
                if (binding == ps2controls::kNone)
                    continue;
                keyboardUsed = binding >= ps2controls::kMouseBase
                                   ? IsMouseButtonPressed(binding - ps2controls::kMouseBase)
                                   : IsKeyPressed(binding);
            }
        }
        if (!keyboardUsed && GetMouseWheelMove() != 0.0f)
            keyboardUsed = true;
        if (padUsed)
            g_lastInputDevice.store(static_cast<int>(gamepadDeviceKind(pad)), std::memory_order_relaxed);
        else if (keyboardUsed)
            g_lastInputDevice.store(static_cast<int>(PS2InputDevice::Keyboard), std::memory_order_relaxed);
    }

    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;
    data[2] = 0xFF;
    data[3] = 0xFF;
    data[4] = data[5] = data[6] = data[7] = kPadStickCenter;
    // Radio on/off: hold D-pad Down on the controller or "M" on the keyboard for
    // 0.75 s (the Mods settings toggle can disable it). Track skip: a single
    // press of D-pad Right / Left skips the music folder track (the keyboard
    // uses F10 for next). The D-pad is unmapped in-game, so neither ever
    // reaches the game.
    {
        const PS2HostSettings &settings = ps2HostSettings();
        static double s_offSince = 0.0;
        static bool s_offFired = false;
        static bool s_dpadPrev[2] = {};
        const int pad = ps2ActiveGamepad();
        const bool heldOff =
            settings.radioOffEnabled &&
            ((pad >= 0 && IsGamepadButtonDown(pad, GAMEPAD_BUTTON_LEFT_FACE_DOWN)) || IsKeyDown(KEY_M));
        if (heldOff)
        {
            const double now = GetTime();
            if (s_offSince == 0.0)
                s_offSince = now;
            else if (!s_offFired && now - s_offSince >= 0.75)
            {
                s_offFired = true;
                const bool off = !ps2MusicStationRadioOff();
                ps2MusicStationSetRadioOff(off);
                // The game is re-tuned to its silent slot by vcsFrameUpdate once,
                // on the first frame of the off state (see game_overrides.cpp).
                std::fprintf(stderr, "[pad] radio %s\n", off ? "off" : "on");
            }
        }
        else
        {
            s_offSince = 0.0;
            s_offFired = false;
        }
        if (pad >= 0)
        {
            const bool dpadNext = IsGamepadButtonDown(pad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT);
            const bool dpadPrev = IsGamepadButtonDown(pad, GAMEPAD_BUTTON_LEFT_FACE_LEFT);
            if (dpadNext && !s_dpadPrev[0])
                ps2MusicStationNextTrack();
            if (dpadPrev && !s_dpadPrev[1])
                ps2MusicStationPrevTrack();
            s_dpadPrev[0] = dpadNext;
            s_dpadPrev[1] = dpadPrev;
        }
        else
        {
            s_dpadPrev[0] = s_dpadPrev[1] = false;
        }
    }

    // Controller buttons still held when the menu closes (Circle / Options
    // closing it) stay with the menu until released.
    static bool s_padHeldByMenu = false;
    if (g_ps2SettingsMenuOpen.load(std::memory_order_relaxed))
    {
        s_padHeldByMenu = true;
        return true; // the settings menu has the keyboard, mouse and controller
    }

    uint16_t btns = 0xFFFFu;
    auto clearBit = [&btns](uint16_t mask)
    { btns &= ~mask; };
    auto press = [&clearBit](int ps2Button)
    {
        if (ps2Button >= 0 && ps2Button < ps2controls::kPs2ButtonCount)
            clearBit(static_cast<uint16_t>(1u << ps2Button));
    };

    const bool driving = m_vehicleProbe && m_rdram && m_vehicleProbe(m_rdram);
    const bool inMenu = m_menuProbe && m_menuRdram && m_menuProbe(m_menuRdram);
    static const bool s_debug = std::getenv("PS2X_PAD_DEBUG") != nullptr;
    static bool s_lastDriving = false, s_lastMenu = false;
    if (s_debug && (driving != s_lastDriving || inMenu != s_lastMenu))
    {
        s_lastDriving = driving;
        s_lastMenu = inMenu;
        std::fprintf(stderr, "[pad] driving=%d menu=%d\n", driving ? 1 : 0, inMenu ? 1 : 0);
    }
    const PS2HostSettings &settings = ps2HostSettings();
    // The game's pause menu / map uses its own fixed layout (classic PS2 by
    // default): gameplay remaps never change how the menus navigate, and the
    // menu layout never leaks into gameplay.
    const int context = inMenu ? ps2controls::Menus : (driving ? ps2controls::InVehicle : ps2controls::OnFoot);
    ps2controls::setCurrentContext(context);
    // The game's pause flag only becomes visible next frame, so the prompt
    // art would show the gameplay keys for one frame after pressing Start.
    // A fresh Start press predicts the toggle for the art only; the pad
    // mapping keeps the real context until the flag is seen. The prompt
    // layers are rebuilt here, early in the frame, so the new art lands
    // before this frame's draw.
    {
        const int pad = ps2ActiveGamepad();
        bool startPressed = pad >= 0 && IsGamepadButtonPressed(pad, GAMEPAD_BUTTON_MIDDLE_RIGHT);
        if (!startPressed)
        {
            auto pressedKey = [](int binding)
            { return binding != ps2controls::kNone && binding < ps2controls::kMouseBase && IsKeyPressed(binding); };
            startPressed = pressedKey(settings.keys[ps2controls::Pause][0]) ||
                           pressedKey(settings.keys[ps2controls::Pause][1]);
        }
        const int predicted = inMenu ? (driving ? ps2controls::InVehicle : ps2controls::OnFoot) : ps2controls::Menus;
        ps2PromptArtSetContext(startPressed ? predicted : -1);
    }
    ps2ModsRefreshButtonPromptsIfNeeded();

    // Gamepad: each host button maps to a PS2 button per context (the
    // Classic / Modern / Custom layout in the settings), sticks as they are.
    const int kGamepad = ps2ActiveGamepad();
    if (kGamepad >= 0)
    {
        static const int kRaylibButton[ps2controls::kHostButtonCount] = {
            GAMEPAD_BUTTON_RIGHT_FACE_DOWN, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT, GAMEPAD_BUTTON_RIGHT_FACE_LEFT,
            GAMEPAD_BUTTON_RIGHT_FACE_UP, GAMEPAD_BUTTON_LEFT_TRIGGER_1, GAMEPAD_BUTTON_RIGHT_TRIGGER_1,
            GAMEPAD_BUTTON_LEFT_TRIGGER_2, GAMEPAD_BUTTON_RIGHT_TRIGGER_2, GAMEPAD_BUTTON_LEFT_THUMB,
            GAMEPAD_BUTTON_RIGHT_THUMB, GAMEPAD_BUTTON_MIDDLE_LEFT, GAMEPAD_BUTTON_MIDDLE_RIGHT,
            GAMEPAD_BUTTON_LEFT_FACE_UP, GAMEPAD_BUTTON_LEFT_FACE_DOWN, GAMEPAD_BUTTON_LEFT_FACE_LEFT,
            GAMEPAD_BUTTON_LEFT_FACE_RIGHT};
        bool hostDown[ps2controls::kHostButtonCount]{};
        bool anyDown = false;
        for (int b = 0; b < ps2controls::kHostButtonCount; ++b)
        {
            bool down = IsGamepadButtonDown(kGamepad, kRaylibButton[b]);
            // Analog triggers (rest at -1) on pads without trigger buttons.
            if (b == ps2controls::PadLT)
                down = down || GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_TRIGGER) > 0.0f;
            else if (b == ps2controls::PadRT)
                down = down || GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_TRIGGER) > 0.0f;
            hostDown[b] = down;
            anyDown = anyDown || down;
        }
        filterMenuCombo(hostDown[ps2controls::PadBack], hostDown[ps2controls::PadStart]);
        if (s_padHeldByMenu && !anyDown)
            s_padHeldByMenu = false;
        if (!s_padHeldByMenu)
            for (int b = 0; b < ps2controls::kHostButtonCount; ++b)
            {
                if (!hostDown[b])
                    continue;
                // Every button follows the context's map: the menu context keeps
                // the classic layout, so the D-pad (and everything else)
                // navigates the game's menus under every gameplay layout.
                // D-pad Down also holds the radio on/off toggle (above).
                press(settings.gamepadMap[context][b]);
            }

        float lx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_X);
        float ly = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_Y);
        float rx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_X);
        float ry = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_Y);
        data[6] = static_cast<uint8_t>(128 + lx * 127);
        data[7] = static_cast<uint8_t>(128 + ly * 127);
        data[4] = static_cast<uint8_t>(128 + rx * 127);
        data[5] = static_cast<uint8_t>(128 + ry * 127);
    }

    // Keyboard + mouse (modern GTA PC style by default, rebindable), merged
    // with the gamepad: raylib reports any joystick device as "gamepad
    // available" (e.g. gaming mouse dongles exposing /dev/input/js*), so the
    // keyboard is always read.
    {
        namespace pc = ps2controls;
        const bool captured = m_mouseCaptured.load(std::memory_order_relaxed);
        auto bindingDown = [captured](int binding)
        {
            if (binding == pc::kNone)
                return false;
            if (binding >= pc::kMouseBase)
                return captured && IsMouseButtonDown(binding - pc::kMouseBase); // a click first captures the mouse
            return IsKeyDown(binding);
        };
        auto held = [&](int action)
        { return bindingDown(settings.keys[action][0]) || bindingDown(settings.keys[action][1]); };

        enum : int
        {
            Select = 0, L3, R3, Start, Up, Right, Down, Left, L2, R2, L1, R1, Triangle, Circle, Cross, Square
        };
        // Menus and common actions. The menu keys work in the game's menus
        // only (their own row of bindings), and the gameplay bindings do not
        // leak into the menus.
        const bool inMenu = context == ps2controls::Menus;
        if (inMenu)
        {
            if (held(pc::MenuUp))
                press(Up);
            if (held(pc::MenuDown))
                press(Down);
            if (held(pc::MenuLeft))
                press(Left);
            if (held(pc::MenuRight))
                press(Right);
            if (held(pc::MenuAccept))
                press(Cross);
            if (held(pc::MenuBack))
                press(Triangle);
            if (held(pc::MapTarget))
                press(Square); // the map's Target On/Off
        }
        if (held(pc::Pause))
            press(Start);
        // Cutscene skip: Cross, but only while a cutscene plays (when the
        // probe is wired), so it cannot fire during normal gameplay.
        const bool cutscene =
            m_cutsceneProbe == nullptr || (m_cutsceneRdram != nullptr && m_cutsceneProbe(m_cutsceneRdram));
        static bool s_lastCutscene = false;
        if (m_cutsceneProbe != nullptr && cutscene != s_lastCutscene)
        {
            s_lastCutscene = cutscene;
            std::fprintf(stderr, "[pad] cutscene=%d\n", cutscene ? 1 : 0);
        }
        if (held(pc::SkipCutscene) && cutscene)
            press(Cross);
        if (!inMenu)
        {
            if (held(pc::EnterVehicle))
                press(Triangle);
            if (held(pc::CameraMode))
                press(Select);
            if (held(pc::LookBehind))
                press(R3);
            if (held(pc::Fire))
                press(Circle);
        }

        // Left stick: WASD; in vehicles forward / back are the pedals. In the
        // game's menus the wheel zooms the map (R1 in, L1 out); navigation is
        // the menu keys above (arrows plus WASD).
        float moveX = 0.0f, moveY = 0.0f;
        if (inMenu)
        {
            int zoom = m_wheelSteps.exchange(0, std::memory_order_relaxed);
            if (zoom == 0)
            {
                // The wheel is also read by the mouse-look path when the mouse
                // is captured; either source gives at most one step per frame.
                const float wheel = GetMouseWheelMove();
                if (wheel != 0.0f)
                    zoom = wheel > 0.0f ? 1 : -1;
            }
            // Each notch holds the zoom button for a few frames; scrolling fast
            // queues several steps, so the hold (and the game's own repeat)
            // keeps up instead of one step per notch. The pad's L1/R1 buttons
            // are untouched.
            if (zoom != 0)
                m_menuZoomPulse = std::clamp(m_menuZoomPulse + zoom * 6, -48, 48);
            if (m_menuZoomPulse > 0)
                press(R1);
            else if (m_menuZoomPulse < 0)
                press(L1);
            m_menuZoomPulse += m_menuZoomPulse > 0 ? -1 : (m_menuZoomPulse < 0 ? 1 : 0);
        }
        else
        {
            if (held(pc::MoveLeft))
                moveX -= 1.0f;
            if (held(pc::MoveRight))
                moveX += 1.0f;
            if (driving)
            {
                if (held(pc::MoveForward))
                    press(Cross);
                if (held(pc::MoveBackward))
                    press(Square);
                if (held(pc::Handbrake))
                    press(R1);
                if (held(pc::Horn))
                    press(L1);
                // Bikes: the keyboard has no analog stick, so this pulls the
                // left stick back (hold accelerate for a wheelie) or forward
                // for a stoppie.
                if (held(pc::VehicleLeanBack))
                    moveY += 1.0f;
                if (held(pc::VehicleLeanForward))
                    moveY -= 1.0f;
            }
            else
            {
                if (held(pc::Sprint))
                    press(Cross);
                if (held(pc::Jump))
                    press(Square);
                if (held(pc::Interact))
                    press(L1);
                if (held(pc::Aim))
                    press(R1);
                if (held(pc::MoveForward))
                    moveY -= 1.0f;
                if (held(pc::MoveBackward))
                    moveY += 1.0f;
            }
        }
        if (moveX != 0.0f)
            data[6] = static_cast<uint8_t>(128 + moveX * 127);
        if (moveY != 0.0f)
            data[7] = static_cast<uint8_t>(128 + moveY * 127);

        // Weapon cycling: mouse wheel / the Next weapon key, as short R2
        // (next) / L2 (previous) presses. In the game's menus the wheel is the
        // map zoom instead (above).
        if (!inMenu)
        {
            const int steps = m_wheelSteps.exchange(0, std::memory_order_relaxed);
            if (m_weaponPulse == 0 && steps != 0)
                m_weaponPulse = steps < 0 ? 4 : -4;
            auto pressedNow = [](int binding)
            {
                if (binding == pc::kNone || binding >= pc::kMouseBase)
                    return binding >= pc::kMouseBase && IsMouseButtonPressed(binding - pc::kMouseBase);
                return IsKeyPressed(binding);
            };
            if (m_weaponPulse == 0 && (pressedNow(settings.keys[pc::NextWeapon][0]) || pressedNow(settings.keys[pc::NextWeapon][1])))
                m_weaponPulse = 4;
            if (!driving && m_weaponPulse > 0)
                press(R2);
            if (!driving && m_weaponPulse < 0)
                press(L2);
            m_weaponPulse += m_weaponPulse > 0 ? -1 : (m_weaponPulse < 0 ? 1 : 0);
        }
    }

    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    // PS2X_PAD_DEBUG=1: once a second, what a controller is doing - which pad
    // the port picked, whether the pinned name matched, the host buttons down
    // and the PS2 mask that resulted.
    static const bool s_debugPad = std::getenv("PS2X_PAD_DEBUG") != nullptr;
    if (s_debugPad)
    {
        static double s_next = 0.0;
        const double now = GetTime();
        if (now >= s_next)
        {
            s_next = now + 1.0;
            const int pad = ps2ActiveGamepad();
            std::string names;
            for (int i = 0; i < 4; ++i)
            {
                if (!IsGamepadAvailable(i))
                    continue;
                const char *nm = GetGamepadName(i);
                unsigned mask = 0;
                for (int b = GAMEPAD_BUTTON_LEFT_FACE_UP; b <= GAMEPAD_BUTTON_RIGHT_THUMB; ++b)
                    if (IsGamepadButtonDown(i, static_cast<GamepadButton>(b)))
                        mask |= 1u << b;
                char axes[64];
                std::snprintf(axes, sizeof(axes), "%d,%d,%d,%d", static_cast<int>(GetGamepadAxisMovement(i, 0) * 100.0f),
                              static_cast<int>(GetGamepadAxisMovement(i, 1) * 100.0f),
                              static_cast<int>(GetGamepadAxisMovement(i, 2) * 100.0f),
                              static_cast<int>(GetGamepadAxisMovement(i, 3) * 100.0f));
                names += " [" + std::to_string(i) + "]" + (nm ? nm : "?") +
                         "(mapped=" + (glfwJoystickIsGamepad(i) ? "1" : "0") + ",btn=" + std::to_string(mask) +
                         ",ax=" + axes + ")";
            }
            const char *name = pad >= 0 ? GetGamepadName(pad) : nullptr;
            // Also append to a log file, so the lines can be shared without
            // redirecting stderr: padlog.txt in the working directory (like the
            // F8 dumps), and the same name in the config folder so it can
            // always be found (~/.config/ps2recomp/padlog.txt).
            static FILE *s_file = std::fopen("padlog.txt", "a");
            static FILE *s_configFile = []() -> FILE *
            {
                const std::filesystem::path path =
                    std::filesystem::path(ps2HostSettingsPath()).parent_path() / "padlog.txt";
                return std::fopen(path.string().c_str(), "a");
            }();
            char line[1024];
            std::snprintf(line, sizeof(line),
                          "[pad] active=%d '%s' pinned='%s' connected:%s ps2mask=0x%04x axes=%u,%u,%u,%u\n", pad,
                          name ? name : "(none)", ps2HostSettings().controllerName.c_str(), names.c_str(),
                          static_cast<unsigned>(btns), data[6], data[7], data[4], data[5]);
            for (FILE *sink : {s_file, s_configFile})
            {
                if (sink != nullptr)
                {
                    std::fputs(line, sink);
                    std::fflush(sink);
                }
            }
            std::fprintf(stderr, "[pad] active=%d '%s' pinned='%s'\n", pad, name ? name : "(none)",
                         ps2HostSettings().controllerName.c_str());
        }
    }
    return true;
}

bool PSPadBackend::applyFakeMouse(float &yaw, float &pitch)
{
    static const char *s_env = std::getenv("PS2X_MOUSE_FAKE");
    if (!s_env)
        return false;
    static float s_yaw = 0.0f, s_pitch = 0.0f;
    static unsigned long long s_from = 0, s_to = ~0ull; // optional presented-frame window
    static const bool s_parsed = std::sscanf(s_env, "%f,%f,%llu,%llu", &s_yaw, &s_pitch, &s_from, &s_to) >= 1;
    const uint64_t frame = g_ps2PresentedFrameCounter.load(std::memory_order_relaxed);
    if (!s_parsed)
        return false;
    if (frame < s_from || frame >= s_to)
        return true; // captured, not moving
    yaw += s_yaw;
    pitch += s_pitch;
    return true;
}

void PSPadBackend::updateHostInput()
{
    // F7: a marker in the log (playtest notes: "it happened here").
    // F8: snapshot EE RAM to ram_f8_<frame>.bin (memory diffing while playing,
    // e.g. finding the radio station variable).
    if (IsKeyPressed(KEY_F8))
        g_ps2DumpRamRequest.store(true, std::memory_order_relaxed);
    if (IsKeyPressed(KEY_F7))
    {
        std::fprintf(stderr, "[mark] frame=%llu\n", static_cast<unsigned long long>(g_ps2PresentedFrameCounter.load()));
        if (FILE *f = std::fopen("boolwrites.log", "a"))
        {
            std::fprintf(f, "[mark] frame=%llu\n", static_cast<unsigned long long>(g_ps2PresentedFrameCounter.load()));
            std::fclose(f);
        }
    }
    // Click in the window to capture the mouse for mouse look; Left Alt (or
    // losing focus) releases it.
    bool captured = m_mouseCaptured.load(std::memory_order_relaxed);
    // The key (or mouse button) bound to "Release the mouse" - Left Alt by
    // default - gives the cursor back; a left click captures it again.
    auto releaseMousePressed = []()
    {
        const auto &bindings = ps2HostSettings().keys[ps2controls::ReleaseMouse];
        for (int slot = 0; slot < 2; ++slot)
        {
            const int binding = bindings[slot];
            if (binding == ps2controls::kNone)
                continue;
            if (binding >= ps2controls::kMouseBase)
            {
                if (IsMouseButtonPressed(binding - ps2controls::kMouseBase))
                    return true;
            }
            else if (IsKeyPressed(binding))
                return true;
        }
        return false;
    };
    if (g_ps2SettingsMenuOpen.load(std::memory_order_relaxed))
    {
        if (captured)
            EnableCursor();
        m_mouseCaptured.store(false, std::memory_order_relaxed);
        GetMouseDelta();
        return;
    }
    if (!captured && IsWindowFocused() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
    {
        DisableCursor();
        captured = true;
    }
    else if (captured && (releaseMousePressed() || !IsWindowFocused()))
    {
        EnableCursor();
        captured = false;
    }
    m_mouseCaptured.store(captured, std::memory_order_relaxed);
    if (!captured)
        return;
    // Direct mouse look (applied to the camera by a game hook, see
    // consumeMouseLook). Sensitivity / invert Y come from the settings (F2
    // menu, PS2X_MOUSE_SENS / PS2X_MOUSE_INVERT_Y); '-' / '=' adjust the
    // sensitivity while playing.
    PS2HostSettings &settings = ps2HostSettings();
    if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_EQUAL))
    {
        settings.mouseSensitivity = std::clamp(settings.mouseSensitivity * (IsKeyPressed(KEY_EQUAL) ? 1.25f : 0.8f), 0.05f, 20.0f);
        ps2HostSettingsSave();
        std::fprintf(stderr, "[pad] mouse sensitivity %.2f\n", settings.mouseSensitivity);
    }
    const float radiansPerPixel = 0.001f * settings.mouseSensitivity;
    const float pitchSign = settings.mouseInvertY ? -1.0f : 1.0f;
    const Vector2 delta = GetMouseDelta();
    if (delta.x != 0.0f || delta.y != 0.0f)
    {
        m_lookYaw.store(m_lookYaw.load(std::memory_order_relaxed) + delta.x * radiansPerPixel, std::memory_order_relaxed);
        m_lookPitch.store(m_lookPitch.load(std::memory_order_relaxed) - delta.y * radiansPerPixel * pitchSign,
                          std::memory_order_relaxed);
        static const bool s_debug = std::getenv("PS2X_PAD_DEBUG") != nullptr;
        if (s_debug)
            std::fprintf(stderr, "[pad] mouse d=(%.1f,%.1f)\n", delta.x, delta.y);
    }
    const float wheel = GetMouseWheelMove();
    if (wheel != 0.0f)
        m_wheelSteps.fetch_add(wheel > 0.0f ? 1 : -1, std::memory_order_relaxed);
}
