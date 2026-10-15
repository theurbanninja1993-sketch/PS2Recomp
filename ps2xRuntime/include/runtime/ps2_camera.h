#pragma once

#include <atomic>
#include <cstdint>

// The main camera, published once per emulated frame for the reprojection pass
// (a modern motion blur, and later frame interpolation above 60 fps).
//
// Written by the game_overrides camera hooks, which is the only place the port
// knows the camera: orientation at +0x78/+0x7C, position at +0x20, and the view
// window (tan(halfFov) wide) from RwCameraSetViewWindow. Read by the renderer at
// present time, which runs on another thread, so a reader may see a value one
// frame old; for a blur that is invisible and not worth a lock.
struct Ps2CameraState
{
    float alpha = 0.0f, beta = 0.0f; // yaw, pitch
    float pos[3] = {};
    float viewX = 0.0f, viewY = 0.0f;
    bool valid = false;
};

extern Ps2CameraState g_ps2Camera;
// The presented frame the state was captured on, so the renderer can tell a
// fresh camera from one that has not moved since.
extern std::atomic<uint64_t> g_ps2CameraFrame;
