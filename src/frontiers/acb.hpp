// SPDX-License-Identifier: MIT
// CRI ACB cue banks (@UTF tables + embedded AFS2 wave archive) and HCA decoding
// through vgmstream's clHCA (ISC). Used to play Sonic Frontiers' own sound
// effects from the player's installed banks. Decoded audio stays in memory.
#pragma once
#include <string>
#include <vector>
#include "bytes.hpp"
extern "C" {
#include "../third_party/clhca/clhca.h"
}

namespace frontiers {

struct Pcm {
    uint32_t rate = 0, channels = 0;
    std::vector<int16_t> samples;      // interleaved
    uint32_t loop_start = 0, loop_end = 0;   // HCA loop in frames of `samples`, [start, end); end 0: no loop
};

// One waveform a cue starts: its index in the WaveformTable, when it starts after the cue, and from how many of
// the cue's tracks (a layered cue may start the same waveform twice at once).
struct CueVoice { uint32_t waveform = 0; float delay = 0; uint32_t plays = 1; };

class UtfTable {
public:
    bool open(const uint8_t *data, size_t size)
    {
        d_ = Bytes{data, size};
        if (size < 32 || std::memcmp(data, "@UTF", 4)) return false;
        rows_offset_ = 8 + d_.be16(10);
        strings_ = 8ull + d_.be32(12);
        data_ = 8ull + d_.be32(16);
        const uint16_t columns = d_.be16(24);
        row_width_ = d_.be16(26);
        rows_ = d_.be32(28);
        uint64_t p = 32;
        uint32_t row_offset = 0;
        static const uint8_t sizes[16] = {1, 1, 2, 2, 4, 4, 8, 8, 4, 8, 4, 8, 16, 0, 0, 0};
        for (uint16_t c = 0; c < columns && d_.ok; ++c) {
            Column col;
            const uint8_t flag = d_.u8(p);
            col.type = flag & 0x0f;
            col.storage = flag & 0xf0;
            col.name = d_.cstr(strings_ + d_.be32(p + 1));
            p += 5;
            const uint8_t size = sizes[col.type];
            if (!size) return false;
            if (col.storage == 0x30) { col.at = p; p += size; }
            else if (col.storage == 0x50) { col.at = row_offset; row_offset += size; }
            columns_.push_back(col);
        }
        return d_.ok;
    }
    uint32_t rows() const { return rows_; }
    bool has(const char *name) const { return find(name) != nullptr; }
    uint64_t u(uint32_t row, const char *name)
    {
        const Column *c = find(name);
        if (!c) return 0;
        const uint64_t at = where(*c, row);
        switch (c->type) {
        case 0: case 1: return d_.u8(at);
        case 2: case 3: return d_.be16(at);
        case 4: case 5: return d_.be32(at);
        case 6: case 7: return uint64_t(d_.be32(at)) << 32 | d_.be32(at + 4);
        default: return 0;
        }
    }
    std::string s(uint32_t row, const char *name)
    {
        const Column *c = find(name);
        if (!c || c->type != 0xA) return {};
        return d_.cstr(strings_ + d_.be32(where(*c, row)));
    }
    bool blob(uint32_t row, const char *name, const uint8_t *&data, size_t &size)
    {
        const Column *c = find(name);
        if (!c || c->type != 0xB) return false;
        const uint64_t at = where(*c, row);
        const uint64_t offset = data_ + d_.be32(at), length = d_.be32(at + 4);
        if (!length || !d_.has(offset, length)) return false;
        data = d_.p + offset; size = size_t(length);
        return true;
    }

private:
    struct Column { std::string name; uint8_t type = 0, storage = 0; uint64_t at = 0; };
    const Column *find(const char *name) const
    {
        for (auto &c : columns_) if (c.name == name && (c.storage == 0x30 || c.storage == 0x50)) return &c;
        return nullptr;
    }
    uint64_t where(const Column &c, uint32_t row) const
    {
        return c.storage == 0x30 ? c.at : rows_offset_ + uint64_t(row) * row_width_ + c.at;
    }
    Bytes d_;
    uint64_t rows_offset_ = 0, strings_ = 0, data_ = 0;
    uint32_t rows_ = 0, row_width_ = 0;
    std::vector<Column> columns_;
};

class AcbBank {
public:
    bool load(const std::wstring &path, std::string &error)
    {
        if (!read_file(path, file_)) { error = "cannot read bank"; return false; }
        if (!root_.open(file_.data(), file_.size())) { error = "bank is not an ACB table"; return false; }
        return true;
    }

    // Finds the first waveform a cue plays and decodes it.
    bool decode_cue(const std::string &cue, Pcm &out, std::string &error)
    {
        UtfTable names, cues;
        if (!sub(root_, "CueNameTable", names) || !sub(root_, "CueTable", cues)) { error = "bank without cue tables"; return false; }
        for (uint32_t r = 0; r < names.rows(); ++r) {
            if (names.s(r, "CueName") != cue) continue;
            const uint32_t index = uint32_t(names.u(r, "CueIndex"));
            if (index >= cues.rows()) break;
            int waveform = -1;
            if (!follow(uint32_t(cues.u(index, "ReferenceType")), uint32_t(cues.u(index, "ReferenceIndex")), waveform, 0) || waveform < 0) {
                error = "cue has no waveform"; return false;
            }
            return decode_waveform(uint32_t(waveform), out, error);
        }
        error = "cue not found";
        return false;
    }

    // Every waveform a cue reaches (all random variants, all layers), in table order, each once: reaching one
    // again only counts in its plays (the first delay stays).
    // kind: the Type of the cue's sequence (0 layers played together, 1 sequential, 2/3/4 random
    // picks, as vgmstream reads them), -1 when the cue is not a sequence.
    bool cue_voices(const std::string &cue, std::vector<CueVoice> &voices, int &kind, std::string &error)
    {
        voices.clear();
        kind = -1;
        UtfTable names, cues;
        if (!sub(root_, "CueNameTable", names) || !sub(root_, "CueTable", cues)) { error = "bank without cue tables"; return false; }
        for (uint32_t r = 0; r < names.rows(); ++r) {
            if (names.s(r, "CueName") != cue) continue;
            const uint32_t index = uint32_t(names.u(r, "CueIndex"));
            if (index >= cues.rows()) break;
            const uint32_t type = uint32_t(cues.u(index, "ReferenceType")), ref = uint32_t(cues.u(index, "ReferenceIndex"));
            UtfTable sequence;
            if (type == 3 && sub(root_, "SequenceTable", sequence) && ref < sequence.rows()) kind = int(sequence.u(ref, "Type"));
            collect(type, ref, 0, voices, 0);
            if (voices.empty()) { error = "cue has no waveform"; return false; }
            return true;
        }
        error = "cue not found";
        return false;
    }

    bool decode_waveform_at(uint32_t index, Pcm &out, std::string &error) { return decode_waveform(index, out, error); }

private:
    // follow() that keeps going: every synth item, every track and every note-on. A track's 2001 command
    // reads as a wait in milliseconds before the next note-on (INFERRED from the data: Sonic's boost
    // starts its last layer at 1000, the wind its second layer at 3400 or 1182).
    void collect(uint32_t type, uint32_t index, float delay, std::vector<CueVoice> &voices, int depth)
    {
        if (depth > 8) return;
        if (type == 1) {
            for (auto &v : voices) if (v.waveform == index) { ++v.plays; return; }
            voices.push_back({index, delay, 1});
            return;
        }
        if (type == 2) {
            UtfTable synth;
            if (!sub(root_, "SynthTable", synth) || index >= synth.rows()) return;
            const uint8_t *items; size_t size;
            if (!synth.blob(index, "ReferenceItems", items, size)) return;
            Bytes b{items, size};
            for (size_t k = 0; k + 4 <= size; k += 4) collect(b.be16(k), b.be16(k + 2), delay, voices, depth + 1);
            return;
        }
        if (type == 3 || type == 8) {
            UtfTable sequence, tracks, events;
            if (!sub(root_, type == 3 ? "SequenceTable" : "BlockSequenceTable", sequence) || index >= sequence.rows()) return;
            if (!sub(root_, "TrackTable", tracks)) return;
            if (!sub(root_, "TrackEventTable", events) && !sub(root_, "CommandTable", events)) return;
            const uint8_t *list; size_t size;
            if (!sequence.blob(index, "TrackIndex", list, size)) return;
            Bytes b{list, size};
            for (size_t k = 0; k + 2 <= size; k += 2) {
                const uint32_t track = b.be16(k);
                if (track >= tracks.rows()) continue;
                const uint32_t event = uint32_t(tracks.u(track, "EventIndex"));
                const uint8_t *cmd; size_t cmd_size;
                if (event >= events.rows() || !events.blob(event, "Command", cmd, cmd_size)) continue;
                Bytes c{cmd, cmd_size};
                float at = delay;
                for (size_t p = 0; p + 3 <= cmd_size;) {
                    const uint16_t code = c.be16(p);
                    const uint8_t len = c.u8(p + 2);
                    if ((code == 2000 || code == 2003) && len >= 4) collect(c.be16(p + 3), c.be16(p + 5), at, voices, depth + 1);
                    if (code == 2001 && len >= 4) at += float(c.be32(p + 3)) / 1000.0f;
                    p += 3 + len;
                }
            }
        }
    }

    bool sub(UtfTable &parent, const char *column, UtfTable &table)
    {
        const uint8_t *data; size_t size;
        return parent.blob(0, column, data, size) && table.open(data, size);
    }

    // type: 1 waveform, 2 synth, 3 sequence, 8 block sequence.
    bool follow(uint32_t type, uint32_t index, int &waveform, int depth)
    {
        if (depth > 8) return false;
        if (type == 1) { waveform = int(index); return true; }
        if (type == 2) {
            UtfTable synth;
            if (!sub(root_, "SynthTable", synth) || index >= synth.rows()) return false;
            const uint8_t *items; size_t size;
            if (!synth.blob(index, "ReferenceItems", items, size)) return false;
            Bytes b{items, size};
            for (size_t k = 0; k + 4 <= size; k += 4)
                if (follow(b.be16(k), b.be16(k + 2), waveform, depth + 1)) return true;
            return false;
        }
        if (type == 3 || type == 8) {
            UtfTable sequence, tracks, events;
            if (!sub(root_, type == 3 ? "SequenceTable" : "BlockSequenceTable", sequence) || index >= sequence.rows()) return false;
            if (!sub(root_, "TrackTable", tracks)) return false;
            if (!sub(root_, "TrackEventTable", events) && !sub(root_, "CommandTable", events)) return false;
            const uint8_t *list; size_t size;
            if (!sequence.blob(index, "TrackIndex", list, size)) return false;
            Bytes b{list, size};
            for (size_t k = 0; k + 2 <= size; k += 2) {
                const uint32_t track = b.be16(k);
                if (track >= tracks.rows()) continue;
                const uint32_t event = uint32_t(tracks.u(track, "EventIndex"));
                const uint8_t *cmd; size_t cmd_size;
                if (event >= events.rows() || !events.blob(event, "Command", cmd, cmd_size)) continue;
                Bytes c{cmd, cmd_size};
                for (size_t p = 0; p + 3 <= cmd_size;) {
                    const uint16_t code = c.be16(p);
                    const uint8_t len = c.u8(p + 2);
                    if ((code == 2000 || code == 2003) && len >= 4)
                        if (follow(c.be16(p + 3), c.be16(p + 5), waveform, depth + 1)) return true;
                    p += 3 + len;
                }
            }
            return false;
        }
        return false;
    }

    bool decode_waveform(uint32_t index, Pcm &out, std::string &error)
    {
        UtfTable waves;
        if (!sub(root_, "WaveformTable", waves) || index >= waves.rows()) { error = "no waveform table"; return false; }
        if (waves.has("Streaming") && waves.u(index, "Streaming") == 1) { error = "streamed waveform"; return false; }
        const uint32_t id = uint32_t(waves.has("MemoryAwbId") ? waves.u(index, "MemoryAwbId") : waves.u(index, "Id"));
        const uint8_t *awb; size_t awb_size;
        if (!root_.blob(0, "AwbFile", awb, awb_size)) { error = "bank without memory AWB"; return false; }
        Bytes a{awb, awb_size};
        if (awb_size < 16 || std::memcmp(awb, "AFS2", 4)) { error = "bad AWB"; return false; }
        const uint8_t offset_size = a.u8(5);
        const uint32_t id_size = a.u8(6) ? a.u8(6) : 2;
        const uint32_t count = a.u32(8), align = a.u16(12) ? a.u16(12) : 1;
        if (count > 100000 || (offset_size != 2 && offset_size != 4)) { error = "bad AWB header"; return false; }
        const uint64_t offsets = 16 + uint64_t(count) * id_size;
        auto offset_at = [&](uint32_t i) -> uint64_t { return offset_size == 4 ? a.u32(offsets + 4ull * i) : a.u16(offsets + 2ull * i); };
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t entry_id = id_size == 4 ? a.u32(16 + 4ull * i) : a.u16(16 + 2ull * i);
            if (entry_id != id) continue;
            uint64_t start = offset_at(i);
            start = (start + align - 1) / align * align;
            const uint64_t end = offset_at(i + 1);
            if (!a.ok || end <= start || end > awb_size) { error = "AWB entry out of bounds"; return false; }
            return decode_hca(awb + start, size_t(end - start), out, error);
        }
        error = "waveform id not in AWB";
        return false;
    }

    static bool decode_hca(const uint8_t *data, size_t size, Pcm &out, std::string &error)
    {
        const int header = clHCA_isOurFile(data, unsigned(size));
        if (header <= 0 || size_t(header) > size) { error = "not HCA"; return false; }
        std::vector<uint8_t> buf(data, data + size);   // decoder may modify blocks
        clHCA *hca = clHCA_new();
        clHCA_stInfo info{};
        bool ok = hca && clHCA_DecodeHeader(hca, buf.data(), unsigned(header)) == 0 && clHCA_getInfo(hca, &info) == 0;
        if (ok && info.encryptionEnabled) { error = "encrypted HCA"; ok = false; }
        if (ok && (info.channelCount < 1 || info.channelCount > 2 || info.samplesPerBlock != 1024)) { error = "unsupported HCA layout"; ok = false; }
        if (ok) {
            out.rate = info.samplingRate; out.channels = info.channelCount;
            std::vector<int16_t> block(1024 * info.channelCount);
            for (unsigned b = 0; b < info.blockCount && ok; ++b) {
                const size_t at = size_t(header) + size_t(b) * info.blockSize;
                if (at + info.blockSize > buf.size()) break;
                if (clHCA_DecodeBlock(hca, buf.data() + at, info.blockSize) < 0) { ok = b > 0; break; }
                clHCA_ReadSamples16(hca, block.data());
                out.samples.insert(out.samples.end(), block.begin(), block.end());
            }
            const size_t skip = size_t(info.encoderDelay) * info.channelCount;
            const size_t keep = size_t(info.sampleCount) * info.channelCount;
            if (skip < out.samples.size()) out.samples.erase(out.samples.begin(), out.samples.begin() + skip);
            if (keep && keep < out.samples.size()) out.samples.resize(keep);
            if (out.samples.empty()) { error = "HCA decoded to nothing"; ok = false; }
            // clHCA gives the loop after the encoder delay, the same frames as the trimmed samples
            if (ok && info.loopEnabled && info.loopEndSample > info.loopStartSample) {
                out.loop_start = info.loopStartSample;
                out.loop_end = std::min<uint32_t>(info.loopEndSample, uint32_t(out.samples.size() / info.channelCount));
            }
        }
        if (hca) clHCA_delete(hca);
        return ok;
    }

    std::vector<uint8_t> file_;
    UtfTable root_;
};

} // namespace frontiers
