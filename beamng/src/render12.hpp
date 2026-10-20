// SPDX-License-Identifier: MIT
#pragma once
#include <dxgi.h>
#include "common.hpp"
#include "assets.hpp"
#include "game.hpp"

namespace r12 {
bool hook_queue_class();                       // learn BeamNG's D3D12 queue (call once at start)
bool attach(IDXGISwapChain *chain);            // false: not D3D12, or the queue isn't known yet
bool draw(const game::Scene &scene, const assets::Data *data, float aspect, bool &drew_sonic);  // false: give up
void before_resize();
}
