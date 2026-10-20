// SPDX-License-Identifier: MIT
// Melty test mode (sheets/tests.json): while %LOCALAPPDATA%\Melty\test\sonic-beam.json
// exists, the mod uses the test ring spawns and appends "pass <check>" lines.
#pragma once
#include <cstdint>
#include <string>

namespace testmode {
void start();                   // background watcher of the test file
void poll(double now);          // cheap; re-reads the test file every two seconds
bool active();
uint32_t seed();
void write_state(const std::string &json);   // values for Melty's test_look / test_input
void report_assets_loaded();
void report_frames_drawn();
void report_running();
void report_boosting();
void report_ring_collected();
void report_sound_played();
}
