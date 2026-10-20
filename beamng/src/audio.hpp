// SPDX-License-Identifier: MIT
#pragma once
#include "assets.hpp"

namespace audio {
bool init(const assets::Data &data);
void play(sheets::Sound sound, float pitch = 1.0f);   // pitch: frequency ratio
// Sonic Beam Lab levels in decibels (-40 or lower mutes): every sound, and footsteps on top
void set_levels(float sound_db, float steps_db);
// A looping sound (Frontiers' speed wind): level 0..1 (0 stops it), pitch as a frequency ratio,
// extra_db on top of the sheet volume (Sonic Beam Lab).
void loop(sheets::Sound sound, float level, float pitch, float extra_db);
}
