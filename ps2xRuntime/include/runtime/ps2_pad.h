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
    using VehicleProbe = bool (*)(const uint8_t *rdram);
    void setVehicleProbe(VehicleProbe probe, const uint8_t *rdram)
    {
        m_vehicleProbe = probe;
        m_rdram = rdram;
    }

private:
    VehicleProbe m_vehicleProbe = nullptr;
    const uint8_t *m_rdram = nullptr;
    std::atomic<bool> m_mouseCaptured{false};
    std::atomic<float> m_mouseDx{0.0f}, m_mouseDy{0.0f};
    std::atomic<int> m_wheelSteps{0};
    int m_weaponPulse = 0; // pad reads left on a scripted weapon-cycle press (+ next, - previous)
};

#endif
