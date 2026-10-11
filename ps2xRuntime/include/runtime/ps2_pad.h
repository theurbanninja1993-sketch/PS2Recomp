#ifndef PS2_PAD_H
#define PS2_PAD_H

#include <atomic>
#include <cstddef>
#include <cstdint>

class PSPadBackend
{
public:
    PSPadBackend() = default;
    ~PSPadBackend() = default;

    bool readState(int port, int slot, uint8_t *data, size_t size);

    // Host (window) thread, once per displayed frame: mouse capture and
    // motion for mouse look.
    void updateHostInput();

    // Game-specific "player is driving" check, used by the keyboard (W/S drive)
    // and the optional modern gamepad driving controls.
    // Mouse look: radians of yaw (positive = turn right) and pitch (positive =
    // look up) accumulated since the last call; a game hook applies them to
    // its camera. Returns false when the mouse is not captured.
    bool consumeMouseLook(float &yaw, float &pitch)
    {
        yaw = m_lookYaw.exchange(0.0f, std::memory_order_relaxed);
        pitch = m_lookPitch.exchange(0.0f, std::memory_order_relaxed);
        return m_mouseCaptured.load(std::memory_order_relaxed) || applyFakeMouse(yaw, pitch);
    }

    using VehicleProbe = bool (*)(const uint8_t *rdram);
    void setVehicleProbe(VehicleProbe probe, const uint8_t *rdram)
    {
        m_vehicleProbe = probe;
        m_rdram = rdram;
    }

private:
    static bool applyFakeMouse(float &yaw, float &pitch); // PS2X_MOUSE_FAKE=<yaw>,<pitch> per camera update (tests)
    VehicleProbe m_vehicleProbe = nullptr;
    const uint8_t *m_rdram = nullptr;
    std::atomic<bool> m_mouseCaptured{false};
    std::atomic<float> m_lookYaw{0.0f}, m_lookPitch{0.0f}; // radians not yet applied
    std::atomic<int> m_wheelSteps{0};
    int m_weaponPulse = 0; // pad reads left on a scripted weapon-cycle press (+ next, - previous)
};

#endif
