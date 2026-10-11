#include "runtime/ps2_pad.h"
#include "ps2_host_backend.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
    constexpr uint8_t kPadStickCenter = 0x80;

    constexpr uint16_t PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_DOWN = 0x0040u;
    constexpr uint16_t PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u;
    constexpr uint16_t PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_L3 = 0x0002u;
    constexpr uint16_t PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u;
    constexpr uint16_t PAD_CROSS = 0x4000u;
    constexpr uint16_t PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_L1 = 0x0400u;
    constexpr uint16_t PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L2 = 0x0100u;
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

    uint16_t btns = 0xFFFFu;
    constexpr int kGamepad = 0;
    const bool useGamepad = IsGamepadAvailable(kGamepad);
    auto clearBit = [&btns](uint16_t mask)
    { btns &= ~mask; };

    if (useGamepad)
    {
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_UP))
            clearBit(PAD_UP);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_DOWN))
            clearBit(PAD_DOWN);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_LEFT))
            clearBit(PAD_LEFT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT))
            clearBit(PAD_RIGHT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN))
            clearBit(PAD_CROSS);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT))
            clearBit(PAD_CIRCLE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_LEFT))
            clearBit(PAD_SQUARE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_UP))
            clearBit(PAD_TRIANGLE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_1))
            clearBit(PAD_L1);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_1))
            clearBit(PAD_R1);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_2))
            clearBit(PAD_L2);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_2))
            clearBit(PAD_R2);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_MIDDLE_RIGHT))
            clearBit(PAD_START);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_MIDDLE_LEFT))
            clearBit(PAD_SELECT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_THUMB))
            clearBit(PAD_L3);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_THUMB))
            clearBit(PAD_R3);

        float lx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_X);
        float ly = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_Y);
        float rx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_X);
        float ry = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_Y);
        data[6] = static_cast<uint8_t>(128 + lx * 127);
        data[7] = static_cast<uint8_t>(128 + ly * 127);
        data[4] = static_cast<uint8_t>(128 + rx * 127);
        data[5] = static_cast<uint8_t>(128 + ry * 127);
    }

    // Optional modern driving on a gamepad (PS2X_MODERN_DRIVING=1): right
    // trigger accelerates, left trigger brakes/reverses.
    static const bool s_modernDriving = std::getenv("PS2X_MODERN_DRIVING") != nullptr;
    const bool driving = m_vehicleProbe && m_rdram && m_vehicleProbe(m_rdram);
    static const bool s_debug = std::getenv("PS2X_PAD_DEBUG") != nullptr;
    static bool s_lastDriving = false;
    if (s_debug && driving != s_lastDriving)
    {
        s_lastDriving = driving;
        std::fprintf(stderr, "[pad] driving=%d\n", driving ? 1 : 0);
    }
    if (useGamepad && driving && s_modernDriving)
    {
        btns |= PAD_R2 | PAD_L2; // the triggers act as pedals, not look left/right
        if (GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_TRIGGER) > -0.5f &&
            IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_2))
            clearBit(PAD_CROSS);
        if (GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_TRIGGER) > -0.5f &&
            IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_2))
            clearBit(PAD_SQUARE);
    }

    // Keyboard + mouse, modern GTA (IV/V PC) style, merged with the gamepad:
    // raylib reports any joystick device as "gamepad available" (e.g. gaming
    // mouse dongles exposing /dev/input/js*), so the keyboard is always read.
    {
        // Menus and common actions.
        if (IsKeyDown(KEY_UP))
            clearBit(PAD_UP);
        if (IsKeyDown(KEY_DOWN))
            clearBit(PAD_DOWN);
        if (IsKeyDown(KEY_LEFT))
            clearBit(PAD_LEFT);
        if (IsKeyDown(KEY_RIGHT))
            clearBit(PAD_RIGHT);
        if (IsKeyDown(KEY_ENTER) || IsKeyDown(KEY_KP_ENTER))
            clearBit(PAD_CROSS); // accept
        if (IsKeyDown(KEY_BACKSPACE))
            clearBit(PAD_TRIANGLE); // back
        if (IsKeyDown(KEY_ESCAPE) || IsKeyDown(KEY_P))
            clearBit(PAD_START);
        if (IsKeyDown(KEY_E) || IsKeyDown(KEY_F))
            clearBit(PAD_TRIANGLE); // enter / exit vehicle
        if (IsKeyDown(KEY_V))
            clearBit(PAD_SELECT); // camera mode
        if (IsKeyDown(KEY_C))
            clearBit(PAD_R3); // look behind
        const bool captured = m_mouseCaptured.load(std::memory_order_relaxed);
        if (captured && IsMouseButtonDown(MOUSE_BUTTON_LEFT))
            clearBit(PAD_CIRCLE); // attack / fire

        // Left stick: WASD.
        float moveX = 0.0f, moveY = 0.0f;
        if (driving)
        {
            if (IsKeyDown(KEY_W))
                clearBit(PAD_CROSS); // accelerate
            if (IsKeyDown(KEY_S))
                clearBit(PAD_SQUARE); // brake / reverse
            if (IsKeyDown(KEY_SPACE))
                clearBit(PAD_R1); // handbrake
            if (IsKeyDown(KEY_H))
                clearBit(PAD_L1); // horn
            if (IsKeyDown(KEY_A))
                moveX -= 1.0f;
            if (IsKeyDown(KEY_D))
                moveX += 1.0f;
        }
        else
        {
            if (IsKeyDown(KEY_LEFT_SHIFT))
                clearBit(PAD_CROSS); // sprint
            if (IsKeyDown(KEY_SPACE))
                clearBit(PAD_SQUARE); // jump
            if (IsKeyDown(KEY_Q))
                clearBit(PAD_L1); // action
            if (captured && IsMouseButtonDown(MOUSE_BUTTON_RIGHT))
                clearBit(PAD_R1); // target / aim
            if (IsKeyDown(KEY_W))
                moveY -= 1.0f;
            if (IsKeyDown(KEY_S))
                moveY += 1.0f;
            if (IsKeyDown(KEY_A))
                moveX -= 1.0f;
            if (IsKeyDown(KEY_D))
                moveX += 1.0f;
        }
        if (moveX != 0.0f)
            data[6] = static_cast<uint8_t>(128 + moveX * 127);
        if (moveY != 0.0f)
            data[7] = static_cast<uint8_t>(128 + moveY * 127);

        // Weapon cycling: mouse wheel / Tab, as short R2 (next) / L2 (previous) presses.
        const int steps = m_wheelSteps.exchange(0, std::memory_order_relaxed);
        if (m_weaponPulse == 0 && steps != 0)
            m_weaponPulse = steps < 0 ? 4 : -4;
        if (m_weaponPulse == 0 && IsKeyPressed(KEY_TAB))
            m_weaponPulse = 4;
        if (!driving && m_weaponPulse > 0)
            clearBit(PAD_R2);
        if (!driving && m_weaponPulse < 0)
            clearBit(PAD_L2);
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
    static const bool s_parsed = std::sscanf(s_env, "%f,%f", &s_yaw, &s_pitch) >= 1;
    if (!s_parsed)
        return false;
    yaw += s_yaw;
    pitch += s_pitch;
    return true;
}

void PSPadBackend::updateHostInput()
{
    // Click in the window to capture the mouse for mouse look; Left Alt (or
    // losing focus) releases it.
    bool captured = m_mouseCaptured.load(std::memory_order_relaxed);
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
    // consumeMouseLook): PS2X_MOUSE_SENS scales, PS2X_MOUSE_INVERT_Y inverts,
    // '-' / '=' adjust the sensitivity while playing.
    static float s_sensitivity = []()
    {
        const char *env = std::getenv("PS2X_MOUSE_SENS");
        return env ? std::strtof(env, nullptr) : 1.0f;
    }();
    if (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_EQUAL))
    {
        s_sensitivity = std::clamp(s_sensitivity * (IsKeyPressed(KEY_EQUAL) ? 1.25f : 0.8f), 0.05f, 20.0f);
        std::fprintf(stderr, "[pad] mouse sensitivity %.2f (PS2X_MOUSE_SENS)\n", s_sensitivity);
    }
    const float s_radiansPerPixel = 0.0005f * s_sensitivity;
    static const float s_pitchSign = std::getenv("PS2X_MOUSE_INVERT_Y") ? -1.0f : 1.0f;
    const Vector2 delta = GetMouseDelta();
    if (delta.x != 0.0f || delta.y != 0.0f)
    {
        m_lookYaw.store(m_lookYaw.load(std::memory_order_relaxed) + delta.x * s_radiansPerPixel, std::memory_order_relaxed);
        m_lookPitch.store(m_lookPitch.load(std::memory_order_relaxed) - delta.y * s_radiansPerPixel * s_pitchSign,
                          std::memory_order_relaxed);
        static const bool s_debug = std::getenv("PS2X_PAD_DEBUG") != nullptr;
        if (s_debug)
            std::fprintf(stderr, "[pad] mouse d=(%.1f,%.1f)\n", delta.x, delta.y);
    }
    const float wheel = GetMouseWheelMove();
    if (wheel != 0.0f)
        m_wheelSteps.fetch_add(wheel > 0.0f ? 1 : -1, std::memory_order_relaxed);
}
