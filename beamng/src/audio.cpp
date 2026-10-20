// SPDX-License-Identifier: MIT
// Plays the Frontiers sound effects (decoded from the player's banks) through
// XAudio2 2.9, which ships with Windows 10 and 11.
#include "audio.hpp"
#include "common.hpp"
#include "testmode.hpp"
#include <xaudio2.h>
#include <vector>

namespace audio {

namespace {
IXAudio2 *engine = nullptr;
IXAudio2MasteringVoice *master = nullptr;
struct Voice { IXAudio2SourceVoice *voice = nullptr; uint32_t rate = 0, channels = 0; };
std::vector<Voice> voices;
const assets::Data *source = nullptr;
bool started = false;
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
    master->SetVolume(sheets::tuning::soundVolume);
    voices.resize(data.sounds.size());
    mod::log("audio: ready");
    return true;
}

void play(sheets::Sound sound)
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
        v.voice->SetVolume(sheets::sounds[i].volume);
    }
    // Restart from the top (a new ring cuts the previous ring sound, like in Frontiers).
    v.voice->Stop();
    v.voice->FlushSourceBuffers();
    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = UINT32(pcm.samples.size() * sizeof(int16_t));
    buffer.pAudioData = reinterpret_cast<const BYTE *>(pcm.samples.data());
    buffer.Flags = XAUDIO2_END_OF_STREAM;
    if (SUCCEEDED(v.voice->SubmitSourceBuffer(&buffer)) && SUCCEEDED(v.voice->Start())) testmode::report_sound_played();
}

} // namespace audio
