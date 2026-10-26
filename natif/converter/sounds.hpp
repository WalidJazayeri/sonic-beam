// SPDX-License-Identifier: MIT
// Sonic's sounds from the player's own Frontiers banks (CRI ACB/HCA, decoded by vgmstream's clHCA): every
// waveform a cue reaches, random variants and layers alike, written as PCM16 WAV at its own rate and
// channel count. BeamNG's audio loops a whole file, so a looped waveform keeps only its HCA loop region,
// its seam crossfaded.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "../../src/frontiers/acb.hpp"
#include "sonic_data.hpp"
#include "text.hpp"

namespace sonicnative {

struct SoundFile {
    std::string file;                 // relative to the sounds folder
    std::vector<uint8_t> bytes;
    float seconds = 0, delay = 0;     // delay: when it starts after the cue (layers only)
};

struct SoundOut {
    std::string id, mode;             // mode: random (one file per play) | layers (all files together)
    bool loop = false;
    float volume = 1;
    std::vector<SoundFile> files;
};

// RIFF WAVE, PCM16 little-endian: frames [first, first + frames) of p.
inline std::vector<uint8_t> wav_pcm16(const frontiers::Pcm &p, size_t first, size_t frames)
{
    const uint32_t ch = p.channels, data = uint32_t(frames * ch * 2);
    std::vector<uint8_t> w(44 + size_t(data));
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&w[at], &v, 4); };
    auto put16 = [&](size_t at, uint16_t v) { std::memcpy(&w[at], &v, 2); };
    std::memcpy(&w[0], "RIFF", 4); put32(4, 36 + data); std::memcpy(&w[8], "WAVEfmt ", 8);
    put32(16, 16); put16(20, 1); put16(22, uint16_t(ch)); put32(24, p.rate); put32(28, p.rate * ch * 2);
    put16(32, uint16_t(ch * 2)); put16(34, 16); std::memcpy(&w[36], "data", 4); put32(40, data);
    if (data) std::memcpy(&w[44], p.samples.data() + first * ch, data);
    return w;
}

// The overlay's one-shot treatment (beamng/src/assets.cpp sounds): with a maxSeconds, cut to it and fade the
// last 0.12 s out linearly; maxSeconds 0 keeps the sound as it is.
inline void cut_and_fade(frontiers::Pcm &p, size_t frames, float max_seconds)
{
    p.samples.resize(frames * p.channels);
    if (max_seconds <= 0) return;
    frames = std::min(frames, size_t(double(max_seconds) * p.rate));
    p.samples.resize(frames * p.channels);
    const size_t fade = std::min(frames, size_t(sonic_data::kFadeSeconds * p.rate));
    for (size_t i = 0; i < fade; ++i)
        for (uint32_t c = 0; c < p.channels; ++c) {
            int16_t &s = p.samples[(frames - fade + i) * p.channels + c];
            s = int16_t(s * (1.0 - double(i) / fade));
        }
}

// A loop region cut out of a linear decode does not meet itself sample for sample (HCA blocks are lossy: up to
// 4600 of 32768 on drift's loop). BeamNG loops the whole file (p: frames [first, first + its length) of whole), so
// its last frames blend over a short equal-power crossfade into the frames just before first: the wrap from the
// last frame to the first is continuous, the file keeps the HCA loop's length and still starts on its first frame
// (an intro flows into it). With less than that before first (boost's two looped layers), the head takes over
// from the tail instead and the file gets that much shorter.
inline void blend_loop(frontiers::Pcm &p, const frontiers::Pcm &whole, size_t first)
{
    const size_t frames = p.samples.size() / p.channels;
    const size_t n = std::min(size_t(sonic_data::kLoopBlendSeconds * p.rate), frames / 4);
    if (n < 2) return;
    const bool before = first >= n;
    for (size_t i = 0; i < n; ++i) {
        const double t = 1.5707963267948966 * (double(i) + 0.5) / double(n), in = std::sin(t), out = std::cos(t);
        for (uint32_t c = 0; c < p.channels; ++c) {
            int16_t &s = before ? p.samples[(frames - n + i) * p.channels + c] : p.samples[i * p.channels + c];
            const double v = before ? out * s + in * whole.samples[(first - n + i) * p.channels + c]
                                    : in * s + out * p.samples[(frames - n + i) * p.channels + c];
            s = int16_t(std::clamp(std::lround(v), -32768L, 32767L));
        }
    }
    if (!before) p.samples.resize((frames - n) * p.channels);
}

// Decodes every row of sonic_data::kSounds. A missing bank, cue or waveform is a warning, as in the overlay.
inline void prepare_sounds(const std::wstring &frontiers_root, std::vector<SoundOut> &out)
{
    std::map<std::wstring, std::unique_ptr<frontiers::AcbBank>> banks;   // each bank file is read once
    size_t bytes = 0, files = 0;
    for (const sonic_data::SoundRow &row : sonic_data::kSounds) {
        std::string error;
        auto found = banks.find(row.bank);
        if (found == banks.end()) {
            auto bank = std::make_unique<frontiers::AcbBank>();
            if (!bank->load(frontiers_root + L"\\" + sonic_data::kSoundFolder + row.bank, error)) {
                std::printf("  warning: sound bank %s: %s - %s left out\n", narrow(row.bank).c_str(), error.c_str(), row.id);
                continue;
            }
            found = banks.emplace(row.bank, std::move(bank)).first;
        }
        std::vector<frontiers::CueVoice> voices;
        int kind = -1;
        if (!found->second->cue_voices(row.cue, voices, kind, error)) {
            std::printf("  warning: sound %s (%s): %s - left out\n", row.id, row.cue, error.c_str());
            continue;
        }
        SoundOut main, looped;
        main.id = row.id; main.loop = row.loop; main.volume = row.volume;
        looped.id = std::string(row.id) + "_loop"; looped.loop = true; looped.volume = row.volume;
        std::printf("  sound %-14s %-17s sequence type %2d, %zu waveform(s)\n", row.id, row.cue, kind, voices.size());
        for (const frontiers::CueVoice &v : voices) {
            frontiers::Pcm pcm;
            if (!found->second->decode_waveform_at(v.waveform, pcm, error) || !pcm.channels || !pcm.rate) {
                std::printf("    warning: waveform %u: %s - left out\n", v.waveform, error.c_str());
                continue;
            }
            const size_t frames = pcm.samples.size() / pcm.channels;
            const bool has_loop = pcm.loop_end > pcm.loop_start && pcm.loop_end <= frames;
            const double total = double(frames) / pcm.rate;
            auto add = [&](SoundOut &to, size_t first, size_t count, float max_seconds, float delay, const char *what) {
                frontiers::Pcm part;
                part.rate = pcm.rate; part.channels = pcm.channels;
                part.samples.assign(pcm.samples.begin() + first * pcm.channels, pcm.samples.begin() + (first + count) * pcm.channels);
                if (to.loop) blend_loop(part, pcm, first);
                else cut_and_fade(part, count, max_seconds);
                SoundFile f;
                f.file = to.id + "_" + std::to_string(to.files.size() + 1) + ".wav";
                f.seconds = float(double(part.samples.size() / part.channels) / part.rate);
                f.delay = delay;
                f.bytes = wav_pcm16(part, 0, part.samples.size() / part.channels);
                std::printf("    %-22s wave %3u  %s %5u Hz  %6.3f s of %6.3f  %s", f.file.c_str(), v.waveform, pcm.channels == 1 ? "mono  " : "stereo",
                            pcm.rate, f.seconds, total, what);
                if (delay > 0 && kind == 0) std::printf(", starts at %.3f s", delay);
                std::printf("\n");
                bytes += f.bytes.size(); ++files;
                to.files.push_back(std::move(f));
            };
            if (row.loop) {
                if (has_loop) add(main, pcm.loop_start, pcm.loop_end - pcm.loop_start, 0, v.delay, "loop region, seam blended");
                else add(main, 0, frames, 0, v.delay, "whole, looped, seam blended");
            } else if (has_loop) {
                // The intro is a one-shot layer (maxSeconds and fade, as the overlay), the loop starts at its place in
                // the cue: a timed hand-over, not a sample-continuous one (the intro fades out, or ends short of its loop).
                const double intro = double(pcm.loop_start) / pcm.rate;
                if (intro >= sonic_data::kMinIntroSeconds)
                    add(main, 0, pcm.loop_start, row.max_seconds, v.delay,
                        row.max_seconds <= 0 ? "intro before its loop, whole" :
                        intro > row.max_seconds ? "intro before its loop, cut short of it" : "intro before its loop, faded");
                add(looped, pcm.loop_start, pcm.loop_end - pcm.loop_start, 0, v.delay + float(intro), "loop region, seam blended");
            } else {
                add(main, 0, frames, row.max_seconds, v.delay, row.max_seconds > 0 ? "one-shot, maxSeconds + fade" : "one-shot, whole");
            }
            // more than one track starts this waveform: written once, at the first one's delay (in a random cue,
            // 2 to 4, that is only a repeated pick; drift starts wave 123 twice at once)
            if (v.plays > 1 && (kind < 2 || kind > 4))
                std::printf("    note: %u of the cue's tracks start wave %u, written once\n", v.plays, v.waveform);
        }
        if (main.files.empty()) std::printf("    warning: %s has no one-shot or looped file of its own\n", row.id);
        for (SoundOut *s : {&main, &looped}) {
            if (s->files.empty()) continue;
            s->mode = kind == 0 && s->files.size() > 1 ? "layers" : "random";
            if (s->mode == "random") for (auto &f : s->files) f.delay = 0;    // a pick starts at once
            out.push_back(std::move(*s));
        }
    }
    std::printf("sounds: %zu ids, %zu files, %.2f MB\n", out.size(), files, bytes / 1e6);
}

// The manifest's "sounds" object; prefix is the VFS folder of the files ("/mods/<data>/sounds/").
inline std::string sounds_json(const std::vector<SoundOut> &sounds, const std::string &prefix)
{
    std::string s = "{";
    for (size_t i = 0; i < sounds.size(); ++i) {
        const SoundOut &o = sounds[i];
        float longest = 0;
        bool delayed = false;
        std::string files, delays;
        for (size_t k = 0; k < o.files.size(); ++k) {
            files += (k ? ", " : "") + json_escape(prefix + o.files[k].file);
            delays += (k ? ", " : "") + json_num(o.files[k].delay);
            longest = std::max(longest, o.files[k].seconds);
            delayed = delayed || o.files[k].delay > 0.0005f;
        }
        s += (i ? ",\n    " : "\n    ") + json_escape(o.id) + ": {\"files\": [" + files + "], \"mode\": " + json_escape(o.mode) +
             ", \"loop\": " + (o.loop ? "true" : "false") + ", \"volume\": " + json_num(o.volume) + ", \"seconds\": " + json_num(longest);
        if (delayed) s += ", \"delays\": [" + delays + "]";
        s += "}";
    }
    return s + (sounds.empty() ? "}" : "\n  }");
}

} // namespace sonicnative
