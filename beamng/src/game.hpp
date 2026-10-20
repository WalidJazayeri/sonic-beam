// SPDX-License-Identifier: MIT
// What Sonic does each frame in BeamNG (beamng/sheets/states.json, rings.json, events.json,
// tuning.json), turned into a scene for the renderer.
#pragma once
#include <string>
#include <vector>
#include "common.hpp"
#include "assets.hpp"

namespace game {

struct Instance {
    float model[16];               // row vectors. world space, or straight to clip space when screen
    float alpha = 1;
    bool screen = false;
};

struct Text {
    float x, y, size;              // clip space
    float color[4];
    std::string text;
};

struct Scene {
    bool camera = false;           // view/proj valid this frame
    float view[16], proj[16];      // row vectors, reversed infinite depth
    bool sonic_visible = false;
    float sonic_model[16];         // model -> world
    std::vector<float> pose;       // nodes * 12 skinning matrices
    bool shadow = false;
    float shadow_corners[4][3];    // world space
    std::vector<Instance> rings;
    std::vector<Text> texts;
};

// frame: latest data from Lua (null before the first one). aspect: back buffer width / height.
void update(const assets::Data *data, const SbFrame *frame, double now, float aspect, Scene &scene);
int ring_total();

} // namespace game
