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

// beamng/sheets/animations.json, every row in its order (id, clip, loop, refKmh, minRate, maxRate, blendSeconds).
// A 'fixed' rate row has refKmh 0 and both rates 1. 'swim' reuses walk_loop like the overlay but is a sequence
// of its own (fixed rate, slower blend). 'fall' is what the overlay plays airborne (states.json row airborne);
// jump_start, ball and land are the jump set; trick_* the air tricks; wall_* the wall run (game.cpp f->wall).
struct ClipRow { const char *id, *clip; bool loop; float ref_kmh, min_rate, max_rate, blend_seconds; };
constexpr ClipRow kClips[] = {
    {"idle",        "chr_sonic@idle_loop.anm.pxd",      true,  0,  1.0f, 1.0f, 0.25f},
    {"walk",        "chr_sonic@walk_loop.anm.pxd",      true,  6,  0.7f, 2.0f, 0.2f},
    {"run",         "chr_sonic@run_loop.anm.pxd",       true,  32, 0.9f, 3.2f, 0.2f},
    {"boost",       "chr_sonic@boost_loop.anm.pxd",     true,  60, 1.0f, 4.5f, 0.15f},
    {"boost_left",  "chr_sonic@boost_l_loop.anm.pxd",   true,  60, 1.0f, 4.5f, 0.2f},
    {"boost_right", "chr_sonic@boost_r_loop.anm.pxd",   true,  60, 1.0f, 4.5f, 0.2f},
    {"drift_left",  "chr_sonic@drift_l_loop.anm.pxd",   true,  0,  1.0f, 1.0f, 0.15f},
    {"drift_right", "chr_sonic@drift_r_loop.anm.pxd",   true,  0,  1.0f, 1.0f, 0.15f},
    {"fall",        "chr_sonic@jump_d_loop.anm.pxd",    true,  0,  1.0f, 1.0f, 0.15f},
    {"trick_up",    "chr_sonic@trick_u0.anm.pxd",       false, 0,  1.0f, 1.0f, 0.1f},
    {"trick_left",  "chr_sonic@trick_l0.anm.pxd",       false, 0,  1.0f, 1.0f, 0.1f},
    {"trick_right", "chr_sonic@trick_r0.anm.pxd",       false, 0,  1.0f, 1.0f, 0.1f},
    {"land",        "chr_sonic@jump_e.anm.pxd",         false, 0,  1.0f, 1.0f, 0.08f},
    {"jump_start",  "chr_sonic@jump_s.anm.pxd",         false, 0,  1.0f, 1.0f, 0.06f},
    {"ball",        "chr_sonic@ball_loop.anm.pxd",      true,  0,  1.0f, 1.0f, 0.08f},
    {"swim",        "chr_sonic@walk_loop.anm.pxd",      true,  0,  1.0f, 1.0f, 0.25f},
    {"wall_up",     "chr_sonic@wallrun_u_loop.anm.pxd", true,  32, 0.9f, 3.2f, 0.12f},
    {"wall_left",   "chr_sonic@wallrun_l_loop.anm.pxd", true,  32, 0.9f, 3.2f, 0.12f},
    {"wall_right",  "chr_sonic@wallrun_r_loop.anm.pxd", true,  32, 0.9f, 3.2f, 0.12f},
};
// Every animation the overlay plays (all of animations.json).
constexpr const char *kDefaultClips = "idle,walk,run,boost,boost_left,boost_right,drift_left,drift_right,fall,"
                                      "trick_up,trick_left,trick_right,land,jump_start,ball,swim,wall_up,wall_left,wall_right";

// beamng/sheets/assets.json rows ball_model, ball_skeleton and ball_clip: Frontiers' spin ball shell, drawn
// around Sonic curled up in the jump ball (beamng/src/game.cpp, jump ball). The overlay poses it with its clip
// (a still pose: 2 identical keys turning the shell 90 degrees about its middle), centres it on the middle of
// its bind box, scales it like Sonic and turns the whole shell about Sonic's right axis.
constexpr const char *kBallModelEntry = "chr_sonicspin.model";
constexpr const char *kBallSkeletonEntry = "chr_sonicspin.skl.pxd";
constexpr const char *kBallClipEntry = "chr_sonicspin@spin_normal_loop.anm.pxd";
constexpr const char *kBallMaterialPrefix = "chr_sonicspin_";
// The shell's middle sits this high above Sonic's feet: tuning.json sonicHeightMeters * 0.42 (game.cpp pivot).
constexpr float kBallPivotShare = 0.42f;
// Ball turns per second: the Lab's default (beamng/mod/lua/ge/extensions/sonicfrontiers/sonic.lua ballSpin = 3).
constexpr float kBallTurnsPerSecond = 3.0f;

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

// Boost effects (natif plan fx): beamng/sheets/assets.json rows boost_aura_model, boost_aura_texture and
// trail_texture (in kArchive), boost_glow_texture, boost_wave_texture, bolt_texture, arc_texture and orb_texture
// (in kEffectArchive, relative to the Frontiers folder like kArchive).
constexpr const wchar_t *kEffectArchive = L"image\\x64\\raw\\EffectCommon.pac";
constexpr const char *kAuraModelEntry = "chr_sonic_aura.model";
constexpr const char *kAuraTextureEntry = "ef_y02_so_aura01.dds";      // BC7, the aura's energy pattern
constexpr const char *kLocusEntry = "ef_y01_so_locus01.dds";           // BC6H, the trail's streaks
constexpr const char *kBoltEntry = "ef_i01_com_lightning02_anim.dds";  // BC7, 4x4 sheet of bolts
constexpr const char *kArcEntry = "ef_n01_com_lightning04.dds";        // BC7, 4 columns of arcs
constexpr const char *kOrbEntry = "ef_n01_com_glowlight13.dds";        // BC7, a glowing orb
constexpr const char *kGlowEntry = "ef_n01_com_glowlight04.dds";       // BC5, detail of the boost flash
constexpr const char *kRingEntry = "ef_a01_com_noisewave01_off.dds";   // BC5, detail of the shock ring

// Effect colours in the overlay's gamma space (beamng/src/game.cpp 585-587 and 590-686, render11.cpp draw_aura);
// the materials take them linear, pow(c, 2.2), since BeamNG blends in linear light.
constexpr float kSparkColour[3] = {0.45f, 0.75f, 1.0f};   // bolts, orbs, flash glow, shock ring
constexpr float kLocusColour[3] = {0.3f, 0.65f, 1.0f};
constexpr float kArcColour[3] = {0.5f, 0.8f, 1.0f};
constexpr float kBeamColour[3] = {0.08f, 0.38f, 1.0f};
// native only: the beam blended in linear light then tonemapped came out darker and greyer than the overlay's
// (game.cpp 585 over the finished image); this colour brings it back (natif-v0.7.0, fitted over three grounds)
constexpr float kBeamColourNative[3] = {0.2f, 0.48f, 1.0f};
constexpr float kAuraColour[3] = {0.04f, 0.28f, 1.0f};
constexpr float kGamma = 2.2f;

// Brightness ladder of every effect material: level k (1..kFxLevels) has alpha kFxAlphaTop * kFxAlphaStep^(k - kFxLevels),
// 0.065 to 2, 16 % apart. Lua draws a quad with the material of the level nearest its alpha (natif plan fx, section 3).
constexpr int kFxLevels = 24;
constexpr double kFxAlphaTop = 2.0, kFxAlphaStep = 1.16;
constexpr float fx_alpha(int k)
{
    double a = kFxAlphaTop;
    for (int i = k; i < kFxLevels; ++i) a /= kFxAlphaStep;
    return float(a);
}

// The aura shell, drawn twice like the overlay (render11.cpp draw_aura, shared_draw.hpp vsAura/psAura): pushed along
// its normal by `push` model units at Aura thickness 1 (the shell is inside-out, its normals point in: negative =
// outward), UVs scaled (psAura's second sample reads uv * 1.7) and a brightness share (tint.a 0.6 / 0.35). The scroll
// is psAura's a1 / a2 motion at fx_ratio 0.5 (pattern clock 0.6 + 2.2 * 0.5 = 1.7 /s; a1 (0, -1.3 t), a2 (0.4 t, -2.1 t)
// in uv * 1.7 space), with the overlay's signs: the DAE holds 1 - v but Torque's COLLADA import flips it back, so
// the shader samples Frontiers' own UVs, and BeamNG adds scrollDir * scrollSpeed * t to them (defaultMat.hlsl
// texureAnimM). The material names start with "base" so that setSkinName("lNN") swaps them for their ladder level lNN_<rest>.
struct AuraLayer { const char *object, *material; float push, uv_scale, alpha, scroll_u, scroll_v, scroll_speed; };
constexpr AuraLayer kAuraLayers[] = {
    {"aura_a",     "base_sonicnativeaura",     -0.03f,  1.0f, 0.6f,  0.0f,  -1.0f,  2.2f},
    {"aurahalo_a", "base_sonicnativeaurahalo", -0.085f, 1.7f, 0.35f, 0.19f, -0.98f, 3.6f},
};
// tuning.json auraOpacity: the base materials show the aura at this level (full aura, before speed and Lab).
constexpr float kAuraOpacity = 0.9f;

// psFlat modes 2 and 3 (shared_draw.hpp): the flash glow and shock ring add output(c (1 + 0.5 s)) * saturate(s * detail),
// in linear light c_lin (1 + 0.5 s)^2.2 * min(1, s * detail), at most 1.5^2.2 = 2.44. Their textures hold that divided
// by the peak, their materials multiply it back.
constexpr float kFlashPeak = 2.44f;
// psFlat mode 4 (bolts, orbs, locus, arcs) adds output(c (0.6 + 0.8 t)) * saturate(1.5 a max(t)): in linear light
// c_lin (0.6 + 0.8 t)^2.2 * 1.5 a t, 1.5 to 3.1 times c_lin a t on bright texels (2.1 at t = 1 when a = 1, where
// the alpha clamps), under 1 on faint ones. A material is linear in t (c_lin gain a t): one gain, for the bright cores.
constexpr float kAddGain = 2.0f;
// native only: the BC7 effect textures (bolts, orbs, arcs) are read as sRGB, so the GPU gives t^2.2 like the
// overlay's display-space t. That keeps t = 1 at 1 and darkens the mid-tones: 2.1 (= 1.4^2.2, the overlay's
// core above) matches the cores, 3 keeps the mid-tones (t 0.5 to 0.7) nearer the overlay's (natif-v0.7.0)
constexpr float kAddGainSrgb = 3.0f;

// One kind of effect quad: Lua draws sprites (camera-facing quads) and ribbons (the trail) with the materials
// sonicnative_fx_<kind>_<level>. made: its texture is copied from an archive (copy), baked from a Frontiers mask
// (glow, ring) or computed (beam). grid: tiles across and down its texture (0: one image).
enum class FxMade { copy, glow, ring, beam };
// srgb: a copied BC7 texture read as sRGB (its header switched to the _SRGB format). glow: BeamNG's glow buffer
// (its bloom halo); the overlay had none, the beam is kept out of it.
struct FxKind { const char *kind; bool ribbon; FxMade made; bool effect_archive; const char *entry; const float *colour; float gain; bool lerp; int grid_u, grid_v; bool srgb; bool glow; };
constexpr FxKind kFxKinds[] = {
    {"bolt",  false, FxMade::copy, true,  kBoltEntry,  kSparkColour,      kAddGainSrgb, false, 4, 4, true,  true},
    {"orb",   false, FxMade::copy, true,  kOrbEntry,   kSparkColour,      kAddGainSrgb, false, 0, 0, true,  true},
    {"glow",  false, FxMade::glow, true,  kGlowEntry,  kSparkColour,      kFlashPeak,   false, 0, 0, false, true},
    {"ring",  false, FxMade::ring, true,  kRingEntry,  kSparkColour,      kFlashPeak,   false, 0, 0, false, true},
    // the beam is alpha blended, not added, so it stays blue on a bright ground (game.cpp 585, psFlat mode 5)
    {"beam",  true,  FxMade::beam, false, nullptr,     kBeamColourNative, 1.0f,         true,  0, 0, false, false},
    // locus01 is BC6H, which has no sRGB variant: read linear, its gain unchanged
    {"locus", true,  FxMade::copy, false, kLocusEntry, kLocusColour,      kAddGain,     false, 0, 0, false, true},
    {"arc",   true,  FxMade::copy, true,  kArcEntry,   kArcColour,        kAddGainSrgb, false, 4, 1, true,  true},
};

// Joints the lightning on Sonic's body sits on (overlay: any joint, game.cpp 600-626; here a fixed set). Their
// model-space positions over every clip go to fx_bones_<hash>.json. Names from chr_sonic.skl.pxd: head, the six
// quills (second segment), spine, chest, hips, both arms (upper, fore, hand), both legs (thigh, calf, foot, toe).
constexpr const char *kFxBones[] = {
    "Head", "Needle1_U_L", "Needle1_U_C", "Needle1_U_R", "Needle1_B_L", "Needle1_B_C", "Needle1_B_R",
    "Spine", "Spine1", "Hips",
    "UpperArm_L", "UpperArm_R", "ForeArm_L", "ForeArm_R", "Hand_L", "Hand_R",
    "Thigh_L", "Thigh_R", "Calf_L", "Calf_R", "Foot_L", "Foot_R", "Toe_L", "Toe_R",
};

} // namespace sonic_data
