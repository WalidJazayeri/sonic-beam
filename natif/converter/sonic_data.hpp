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

// Footsteps: the overlay steps on the walk and run animations only (beamng/src/game.cpp footsteps), when a
// toe comes down through the bottom fifth of its height range (beamng/src/assets.cpp footsteps).
constexpr const char *kStepClips[] = {"walk", "run"};
constexpr const char *kToes[] = {"Toe_L", "Toe_R"};
constexpr float kStepThreshold = 0.2f;

// beamng/sheets/sounds.json (id, bank, cue, volume, maxSeconds); banks are relative to kSoundFolder.
// loop: the whole id loops (the overlay's only loop, wind). In the other ids, waveforms carrying an HCA loop
// (the boost and drift layers) go to an extra id <id>_loop, looped; their intro stays a one-shot layer of <id>.
constexpr const wchar_t *kSoundFolder = L"image\\x64\\raw\\sound\\";
struct SoundRow { const char *id; const wchar_t *bank; const char *cue; float volume, max_seconds; bool loop; };
constexpr const wchar_t *kSonicBank = L"sonic_sound\\se_rangers_player_sonic.acb";
constexpr SoundRow kSounds[] = {
    // ring (se_rangers_obj_common.acb, obj_ring, 0.55, 1.2) is left out: the native mode has no rings.
    {"boost",          kSonicBank, "sn_boost_run",     0.6f,  2.5f, false},
    {"drift",          kSonicBank, "sn_drift",         0.5f,  1.6f, false},
    {"jump",           kSonicBank, "sn_jump",          0.6f,  0.9f, false},
    {"trick",          kSonicBank, "sn_airtrick",      0.6f,  2.4f, false},
    {"land",           kSonicBank, "sn_land_conc",     0.6f,  0.7f, false},
    {"step_walk_conc", kSonicBank, "sn_walk_conc",     0.35f, 0.6f, false},
    {"step_walk_dirt", kSonicBank, "sn_walk_dirt",     0.35f, 0.6f, false},
    {"step_walk_gras", kSonicBank, "sn_walk_gras",     0.35f, 0.6f, false},
    {"step_walk_sand", kSonicBank, "sn_walk_sand",     0.35f, 0.6f, false},
    {"step_walk_snow", kSonicBank, "sn_walk_snow",     0.35f, 0.6f, false},
    {"step_walk_watr", kSonicBank, "sn_walk_watr",     0.35f, 0.6f, false},
    {"step_walk_mtlp", kSonicBank, "sn_walk_mtlp",     0.35f, 0.6f, false},
    {"step_run_conc",  kSonicBank, "sn_run_conc",      0.45f, 0.6f, false},
    {"step_run_dirt",  kSonicBank, "sn_run_dirt",      0.45f, 0.6f, false},
    {"step_run_gras",  kSonicBank, "sn_run_gras",      0.45f, 0.6f, false},
    {"step_run_sand",  kSonicBank, "sn_run_sand",      0.45f, 0.6f, false},
    {"step_run_snow",  kSonicBank, "sn_run_snow",      0.45f, 0.6f, false},
    {"step_run_watr",  kSonicBank, "sn_run_watr",      0.45f, 0.6f, false},
    {"step_run_mtlp",  kSonicBank, "sn_run_mtlp",      0.45f, 0.6f, false},
    {"land_dirt",      kSonicBank, "sn_land_dirt",     0.6f,  1.0f, false},
    {"land_gras",      kSonicBank, "sn_land_gras",     0.6f,  1.0f, false},
    {"land_sand",      kSonicBank, "sn_land_sand",     0.6f,  1.0f, false},
    {"land_snow",      kSonicBank, "sn_land_snow",     0.6f,  1.0f, false},
    {"land_watr",      kSonicBank, "sn_land_watr",     0.6f,  1.0f, false},
    {"land_mtlp",      kSonicBank, "sn_land_mtlp",     0.6f,  1.0f, false},
    {"brake",          kSonicBank, "sn_brake",         0.55f, 1.5f, false},
    {"wind",           kSonicBank, "sn_relative_wind", 0.7f,  0.0f, true},
    {"sonic_boom",     kSonicBank, "sn_sonic_boom",    0.9f,  0.0f, false},
};
// A one-shot with a maxSeconds is cut to it and fades out over its last 0.12 s (beamng/src/assets.cpp sounds).
constexpr double kFadeSeconds = 0.12;
// An HCA loop's intro shorter than this is dropped (boost's 48 kHz layer: 54 samples).
constexpr double kMinIntroSeconds = 0.1;
// A loop file's tail blends into the frames before its loop start over this long (or, with no room before it, its
// head takes over from its tail), so the whole-file wrap does not click.
constexpr double kLoopBlendSeconds = 0.02;

// Plain PBR factors until the PRM maps are decoded (plan: constant factors, no PRM).
constexpr float kRoughness = 0.6f;
constexpr float kMetallic = 0.0f;

} // namespace sonic_data
