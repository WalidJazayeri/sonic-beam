// SPDX-License-Identifier: MIT
// Plays the Frontiers sound effects (decoded from the player's banks) through
// XAudio2 2.9, which ships with Windows 10 and 11.
#include "audio.hpp"
#include "common.hpp"
#include "testmode.hpp"
#include <xaudio2.h>
#include <cmath>
#include <cstring>
#include <vector>

namespace audio {

namespace {
IXAudio2 *engine = nullptr;
IXAudio2MasteringVoice *master = nullptr;
struct Voice { IXAudio2SourceVoice *voice = nullptr; uint32_t rate = 0, channels = 0; };
std::vector<Voice> voices;
const assets::Data *source = nullptr;
bool started = false;
float sound_gain = 1, steps_gain = 1;

float gain(float db) { return db <= -40 ? 0.0f : std::pow(10.0f, db / 20); }
bool is_step(size_t i) { return std::strncmp(sheets::sounds[i].id, "step_", 5) == 0; }
}

bool init(const assets::Data &data)
{
    if (started) return engine != nullptr;
    started = true;
    source = &data;
    HMODULE xa = LoadLibraryW(L"xaudio2_9.dll");
    using CreateFn = HRESULT(WINAPI *)(IXAudio2 **, UINT32, XAUDIO2_PROCESSOR);
    auto create = xa ? reinterpret_cast<CreateFn>(GetProcAddress(xa, "XAudio2Create")) : nullptr;
    if (!create || FAILED(create(&engine, 0, XAUDIO2_DEFAULT_PROCESSOR)) || !engine) { mod::log("audio: XAudio2 unavailable"); engine = nullptr; return false; }
    if (FAILED(engine->CreateMasteringVoice(&master))) { mod::log("audio: no output device"); engine->Release(); engine = nullptr; return false; }
    master->SetVolume(sheets::tuning::soundVolume * sound_gain);
    voices.resize(data.sounds.size());
    mod::log("audio: ready");
    return true;
}

void play(sheets::Sound sound, float pitch)
{
    if (!engine || !source) return;
    const size_t i = size_t(sound);
    if (i >= source->sounds.size() || source->sounds[i].samples.empty()) return;
    const frontiers::Pcm &pcm = source->sounds[i];
    Voice &v = voices[i];
    if (!v.voice) {
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = WORD(pcm.channels);
        format.nSamplesPerSec = pcm.rate;
        format.wBitsPerSample = 16;
        format.nBlockAlign = WORD(2 * pcm.channels);
        format.nAvgBytesPerSec = pcm.rate * format.nBlockAlign;
        if (FAILED(engine->CreateSourceVoice(&v.voice, &format))) { v.voice = nullptr; return; }
    }
    v.voice->SetVolume(sheets::sounds[i].volume * (is_step(i) ? steps_gain : 1.0f));
    // Restart from the top (a new ring cuts the previous ring sound, like in Frontiers).
    v.voice->Stop();
    v.voice->FlushSourceBuffers();
    v.voice->SetFrequencyRatio(pitch);
    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = UINT32(pcm.samples.size() * sizeof(int16_t));
    buffer.pAudioData = reinterpret_cast<const BYTE *>(pcm.samples.data());
    buffer.Flags = XAUDIO2_END_OF_STREAM;
    if (SUCCEEDED(v.voice->SubmitSourceBuffer(&buffer)) && SUCCEEDED(v.voice->Start())) testmode::report_sound_played();
}

void set_levels(float sound_db, float steps_db)
{
    const float s = gain(sound_db), f = gain(steps_db);
    if (s == sound_gain && f == steps_gain) return;
    sound_gain = s; steps_gain = f;
    if (master) master->SetVolume(sheets::tuning::soundVolume * sound_gain);
}

} // namespace audio
