#include "runtime/ps2_pad.h"
#include "runtime/ps2_host_settings.h"
#include "ps2_host_backend.h"
#include <atomic>

extern std::atomic<uint64_t> g_ps2PresentedFrameCounter;
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
    constexpr uint8_t kPadStickCenter = 0x80;

}

bool PSPadBackend::readState(int /*port*/, int /*slot*/, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;

    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;
    data[2] = 0xFF;
    data[3] = 0xFF;
    data[4] = data[5] = data[6] = data[7] = kPadStickCenter;
    if (g_ps2SettingsMenuOpen.load(std::memory_order_relaxed))
        return true; // the settings menu has the keyboard and mouse

    uint16_t btns = 0xFFFFu;
    auto clearBit = [&btns](uint16_t mask)
    { btns &= ~mask; };
    auto press = [&clearBit](int ps2Button)
    {
        if (ps2Button >= 0 && ps2Button < ps2controls::kPs2ButtonCount)
            clearBit(static_cast<uint16_t>(1u << ps2Button));
    };

    const bool driving = m_vehicleProbe && m_rdram && m_vehicleProbe(m_rdram);
    static const bool s_debug = std::getenv("PS2X_PAD_DEBUG") != nullptr;
    static bool s_lastDriving = false;
    if (s_debug && driving != s_lastDriving)
    {
        s_lastDriving = driving;
        std::fprintf(stderr, "[pad] driving=%d\n", driving ? 1 : 0);
    }
    const PS2HostSettings &settings = ps2HostSettings();
    const int context = driving ? ps2controls::InVehicle : ps2controls::OnFoot;

    // Gamepad: each host button maps to a PS2 button per context (the
    // Classic / Modern / Custom layout in the settings), sticks as they are.
    constexpr int kGamepad = 0;
    if (IsGamepadAvailable(kGamepad))
    {
        static const int kRaylibButton[ps2controls::kHostButtonCount] = {
            GAMEPAD_BUTTON_RIGHT_FACE_DOWN, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT, GAMEPAD_BUTTON_RIGHT_FACE_LEFT,
            GAMEPAD_BUTTON_RIGHT_FACE_UP, GAMEPAD_BUTTON_LEFT_TRIGGER_1, GAMEPAD_BUTTON_RIGHT_TRIGGER_1,
            GAMEPAD_BUTTON_LEFT_TRIGGER_2, GAMEPAD_BUTTON_RIGHT_TRIGGER_2, GAMEPAD_BUTTON_LEFT_THUMB,
            GAMEPAD_BUTTON_RIGHT_THUMB, GAMEPAD_BUTTON_MIDDLE_LEFT, GAMEPAD_BUTTON_MIDDLE_RIGHT,
            GAMEPAD_BUTTON_LEFT_FACE_UP, GAMEPAD_BUTTON_LEFT_FACE_DOWN, GAMEPAD_BUTTON_LEFT_FACE_LEFT,
            GAMEPAD_BUTTON_LEFT_FACE_RIGHT};
        for (int b = 0; b < ps2controls::kHostButtonCount; ++b)
        {
            bool down = IsGamepadButtonDown(kGamepad, kRaylibButton[b]);
            // Analog triggers (rest at -1) on pads without trigger buttons.
            if (b == ps2controls::PadLT)
                down = down || GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_TRIGGER) > 0.0f;
            else if (b == ps2controls::PadRT)
                down = down || GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_TRIGGER) > 0.0f;
            if (down)
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
        // Menus and common actions.
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
        if (held(pc::Pause))
            press(Start);
        if (held(pc::EnterVehicle))
            press(Triangle);
        if (held(pc::CameraMode))
            press(Select);
        if (held(pc::LookBehind))
            press(R3);
        if (held(pc::Fire))
            press(Circle);

        // Left stick: WASD; in vehicles forward / back are the pedals.
        float moveX = 0.0f, moveY = 0.0f;
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
        if (moveX != 0.0f)
            data[6] = static_cast<uint8_t>(128 + moveX * 127);
        if (moveY != 0.0f)
            data[7] = static_cast<uint8_t>(128 + moveY * 127);

        // Weapon cycling: mouse wheel / the Next weapon key, as short R2
        // (next) / L2 (previous) presses.
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

    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
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
    if (IsKeyPressed(KEY_F7))
        std::fprintf(stderr, "[mark] frame=%llu\n", static_cast<unsigned long long>(g_ps2PresentedFrameCounter.load()));
    // Click in the window to capture the mouse for mouse look; Left Alt (or
    // losing focus) releases it.
    bool captured = m_mouseCaptured.load(std::memory_order_relaxed);
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
    else if (captured && (IsKeyPressed(KEY_LEFT_ALT) || !IsWindowFocused()))
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
