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
    // The camera's orientation basis and position, from TheCamera.Cams[0]
    // (a fixed address, 0x6F4540) - the same vectors the port rotates for
    // vehicle mouse look, so they are the ones it can vouch for. The first
    // attempt read +0x78/+0x7C off the RenderWare camera, which turned out to
    // be a change detector (values like 2000) rather than alpha/beta, and a
    // +0x20 that is not a world position; the camera log caught both.
    float front[3] = {}; // look direction, unit length
    float up[3] = {};    // up vector, unit length
    float pos[3] = {};   // world position (CCam Source)
    float viewX = 0.0f, viewY = 0.0f; // tan(halfFov) wide, aspect-scaled tall
    bool valid = false;
};

extern Ps2CameraState g_ps2Camera;
// The presented frame the state was captured on, so the renderer can tell a
// fresh camera from one that has not moved since.
extern std::atomic<uint64_t> g_ps2CameraFrame;
