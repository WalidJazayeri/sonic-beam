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
};

void start_loading();
// Null until loading finished successfully.
const Data *ready();
bool failed();

} // namespace assets
