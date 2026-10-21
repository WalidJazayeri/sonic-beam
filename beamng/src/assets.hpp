// SPDX-License-Identifier: MIT
// Everything read from the player's Sonic Frontiers at start (sheets/assets.json,
// animations.json, sounds.json). Loaded on a background thread, never written out.
#pragma once
#include <vector>
#include "../../src/frontiers/model.hpp"
#include "../../src/frontiers/anim.hpp"
#include "../../src/frontiers/acb.hpp"
#include "gen/sheets.hpp"

namespace assets {

struct Data {
    frontiers::Model sonic, ring;
    std::vector<frontiers::Clip> clips;     // indexed by sheets::Anim
    std::vector<frontiers::Pcm> sounds;     // indexed by sheets::Sound (empty when a cue failed)
    float sonic_height = 1;                 // model units, feet to top
    std::vector<float> bone_rest;           // each node's rest position (model units, xyz): aura lightning sits on bones
    // Frontiers' boost effect (optional: Sonic still runs without it)
    bool boost_fx = false;
    frontiers::Model aura;                  // aura shell on Sonic's own skeleton
    frontiers::Model dome;                  // the boost aura's shape: a bullet along +z, nose tip at z = 1, tail to z = -1 (built here)
    frontiers::Texture aura_texture, glow_texture, wave_texture;
    // Power Boost trail and lightning (optional too)
    bool trail_fx = false;
    frontiers::Texture trail_texture, bolt_texture, arc_texture, orb_texture;
    // Frontiers' spin ball shell around Sonic curled up in a jump (optional too)
    bool ball_fx = false;
    frontiers::Model ball;
    frontiers::Clip ball_clip;
    // when each foot touches the ground in each clip, as a share of the loop (footstep sounds)
    std::vector<std::vector<float>> step_phases;   // indexed by sheets::Anim
};

void start_loading();
// Null until loading finished successfully.
const Data *ready();
bool failed();

} // namespace assets
