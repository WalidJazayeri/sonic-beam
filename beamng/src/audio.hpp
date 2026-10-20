// SPDX-License-Identifier: MIT
#pragma once
#include "assets.hpp"

namespace audio {
bool init(const assets::Data &data);
void play(sheets::Sound sound);
}
