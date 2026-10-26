// SPDX-License-Identifier: MIT
// What the native converter needs to know about Sonic, copied from the overlay's sheets and sources
// (beamng/ is never read at run time). Keep in step by hand when those sheets change.
#pragma once

namespace sonic_data {

// beamng/sheets/assets.json rows sonic_model and sonic_skeleton (archive is relative to the Frontiers folder).
constexpr const wchar_t *kArchive = L"image\\x64\\raw\\character\\sonic.pac";
constexpr const char *kModelEntry = "chr_sonic.model";
constexpr const char *kSkeletonEntry = "chr_sonic.skl.pxd";
// Frontiers material names start with the model name (chr_sonic_fur...); the native names drop it.
constexpr const char *kMaterialPrefix = "chr_sonic_";

// beamng/sheets/tuning.json sonicHeightMeters: Sonic's standing height in BeamNG's world.
// The overlay's scale is sonicHeightMeters / model height, the height being max.y - min.y of the bind
// mesh (beamng/src/assets.cpp sonic_height, beamng/src/game.cpp k); 1.0 / 1.082 = 0.924 for LOD0.
constexpr float kHeightMeters = 1.0f;

// beamng/sheets/animations.json (id, clip, loop, refKmh, minRate, maxRate). 'fall' is what the overlay
// plays airborne (beamng/sheets/states.json row airborne); jump_start, land and ball are the jump set.
struct ClipRow { const char *id, *clip; bool loop; float ref_kmh, min_rate, max_rate; };
constexpr ClipRow kClips[] = {
    {"idle",       "chr_sonic@idle_loop.anm.pxd", true,  0,  1.0f, 1.0f},
    {"walk",       "chr_sonic@walk_loop.anm.pxd", true,  6,  0.7f, 2.0f},
    {"run",        "chr_sonic@run_loop.anm.pxd",  true,  32, 0.9f, 3.2f},
    {"boost",      "chr_sonic@boost_loop.anm.pxd", true, 60, 1.0f, 4.5f},
    {"fall",       "chr_sonic@jump_d_loop.anm.pxd", true, 0, 1.0f, 1.0f},
    {"jump_start", "chr_sonic@jump_s.anm.pxd",    false, 0,  1.0f, 1.0f},
    {"land",       "chr_sonic@jump_e.anm.pxd",    false, 0,  1.0f, 1.0f},
    {"ball",       "chr_sonic@ball_loop.anm.pxd", true,  0,  1.0f, 1.0f},
};
// The overlay's ground and air set (states.json idle/walk/run/boost/airborne).
constexpr const char *kDefaultClips = "idle,walk,run,boost,fall";

// Plain PBR factors until the PRM maps are decoded (plan: constant factors, no PRM).
constexpr float kRoughness = 0.6f;
constexpr float kMetallic = 0.0f;

} // namespace sonic_data
