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
    float boostBoomKmh;                // Sonic Beam Lab: sonic boom on a boost started this fast (0: off)
    float toSun[3];                    // unit vector toward the sun (BeamNG's sky)
    float groundPos[3], groundNormal[3]; // ground under Sonic and its slope
    float shadowOpacity;               // Sonic Beam Lab: 0..1
    int32_t hasGround;                 // groundPos/groundNormal are valid
    float trailLength, trailIntensity, lightning;   // Sonic Beam Lab: boost trail multipliers
    float fxSpeedAmount, fxRefKmh;     // Sonic Beam Lab: how much the boost effects follow speed (0: not), full at this speed
    float auraThickness, auraIntensity, auraLightning;   // Sonic Beam Lab, Aura (1 = default)
    int32_t auraBody;                  // Sonic Beam Lab, Aura: 1 = the aura follows the body's shape, 0 = lying along the run
    float auraLength;                  // Sonic Beam Lab, Aura: length behind Sonic (x)
    int32_t water;                     // 1 running on water, 2 swimming
    float waterZ;                      // height of the water's surface under Sonic (when water != 0)
    float impact;                      // Sonic hit a vehicle this frame: closing speed (m/s), else 0
    int32_t wall;                      // 1 running up a wall, 2/3 along it (wall on his left/right), else 0
    float wallNormal[3];               // that wall's normal (away from it)
    float trailFade;                   // Sonic Beam Lab: metres over which the trail fades in behind Sonic (0: no fade)
    int32_t jumpStyle;                 // Sonic Beam Lab, Jump: 0 ball the whole jump, 1 ball up then tricks, 2 ball, tricks on a direction
    float ballSpin, ballGlow;          // Sonic Beam Lab, Jump: ball turns per second, blue glow (0 none, 1 default)
    float blurStrength;                // Sonic Beam Lab: speed blur multiplier (1 default)
    int32_t airInput;                  // direction held in the air: 0 none, 1 left, 2 right, 3 back
    int32_t ringCount;                 // world rings to draw
    const float *ringPos;              // ringCount * xyz
};
