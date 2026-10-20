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
    // Frontiers' boost effect (optional: Sonic still runs without it)
    bool boost_fx = false;
    frontiers::Model aura;                  // aura shell on Sonic's own skeleton
    frontiers::Texture aura_texture, glow_texture, wave_texture;
    // when each foot touches the ground in each clip, as a share of the loop (footstep sounds)
    std::vector<std::vector<float>> step_phases;   // indexed by sheets::Anim
};

void start_loading();
// Null until loading finished successfully.
const Data *ready();
bool failed();

} // namespace assets
