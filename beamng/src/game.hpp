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

// Frontiers boost_impact flash: a camera-facing sprite (glow or shock wave) in the world.
struct Sprite {
    float pos[3];
    float size;                    // metres, full width
    float alpha;
    float spin;                    // radians
    int kind;                      // 0 glow flash, 1 shock wave, 2 lightning bolt, 3 glowing orb
    float uv[4] = {0, 0, 1, 1};    // texture rectangle (a cell of the bolt sheet)
};

struct Scene {
    bool camera = false;           // view/proj valid this frame
    float view[16], proj[16];      // row vectors, reversed infinite depth
    bool sonic_visible = false;
    float sonic_model[16];         // model -> world
    std::vector<float> pose;       // nodes * 12 skinning matrices
    bool shadow = false;           // soft round shadow (night, low sun)
    float shadow_corners[4][3];    // world space
    float shadow_alpha = 0.45f;
    bool sonic_shadow = false;     // Sonic flattened onto the ground along the sun
    float sonic_shadow_model[16];  // model -> world, flattened
    float sonic_shadow_alpha = 0;
    std::vector<Instance> rings;
    std::vector<Text> texts;
    // boost effect
    float blur = 0;                // 0..1 speed blur strength
    float blur_center[2] = {0.5f, 0.5f};   // where streaks point, 0..1 screen coordinates
    float aura = 0;                // 0..1 aura brightness (drawn on Sonic's pose)
    float aura_time = 0;           // seconds, scrolls the aura pattern
    std::vector<Sprite> sprites;
    // Power Boost trail: world-space triangles, 9 floats per vertex (position, uv, rgba)
    std::vector<float> trail, arcs, beam;
};

// frame: latest data from Lua (null before the first one). aspect: back buffer width / height.
void update(const assets::Data *data, const SbFrame *frame, double now, float aspect, Scene &scene);
int ring_total();

} // namespace game
