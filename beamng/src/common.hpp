// SPDX-License-Identifier: MIT
// Shared pieces of Sonic Beam's renderer (sonic_beamng.dll, loaded by the mod's Lua through FFI).
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <string>

namespace mod {

void log(const char *format, ...);
std::wstring data_dir();               // %LOCALAPPDATA%\SonicBeam, created on first use
double now_seconds();

// %LOCALAPPDATA%\SonicBeam\settings.ini; Melty writes `frontiers=<Sonic Frontiers folder>` before every Play.
std::wstring setting(const wchar_t *key);
void save_setting(const wchar_t *key, const std::wstring &value);

} // namespace mod

// One frame of BeamNG state, filled by the mod's Lua (sonicfrontiers/sonic.lua) and passed through FFI.
// Keep in sync with the ffi.cdef there.
extern "C" struct SbFrame {
    float camPos[3], camFwd[3], camUp[3];
    float fovY;                        // radians, vertical
    float dt;
    float feet[3], vel[3];             // Sonic's body: point on the ground under him, velocity (m/s)
    int32_t onFoot;                    // Sonic is the player's body right now
    int32_t grounded;
    int32_t boost;                     // boost key held
    int32_t jumped;                    // a jump started this frame
    int32_t picked;                    // rings picked up this frame
    int32_t hudHidden;                 // menus / photo mode: hide our HUD
    int32_t showSpeed;                 // speedometer on (Sonic Beam menu)
    int32_t surface;                   // ground under Sonic: 0 concrete, 1 dirt, 2 grass, 3 sand, 4 snow, 5 water, 6 metal
    int32_t wantsMove;                 // the stick / WASD is pushed
    float stepsDb, soundDb;            // Sonic Beam Lab: footsteps and all sounds, decibels (-40 mutes)
    float animSpeed, animLimit;        // Sonic Beam Lab: run-cycle speed and ceiling multipliers
    float windDb;                      // Sonic Beam Lab: speed wind, decibels (-40 mutes)
    float barrierBoomKmh, boostBoomKmh; // Sonic Beam Lab: sonic boom at this speed / on a boost started this fast (0: off)
    int32_t ringCount;                 // world rings to draw
    const float *ringPos;              // ringCount * xyz
};
