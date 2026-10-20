// SPDX-License-Identifier: MIT
// Reader for Sonic Frontiers PACx403 archives, straight from the player's own
// installed copy. Layout follows HedgeLib (MIT, Radfordhound). Everything stays
// in memory; nothing read here is ever written back to disk.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include "bytes.hpp"

namespace frontiers {

struct PacEntry {
    const uint8_t *data = nullptr;
    uint32_t size = 0;
};

class Pac {
public:
    bool load(const std::wstring &path, std::string &error);
    const PacEntry *find(const std::string &name) const {
        auto it = files_.find(name);
        return it == files_.end() ? nullptr : &it->second;
    }
    const std::unordered_map<std::string, PacEntry> &files() const { return files_; }

private:
    std::vector<uint8_t> source_;
    std::vector<std::vector<uint8_t>> roots_;
    std::unordered_map<std::string, PacEntry> files_;
    bool add_entries(const std::vector<uint8_t> &root, std::string &error);
};

// Bounded LZ4 block decoder, overlapping matches included.
inline bool lz4_block(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size)
{
    size_t i = 0, o = 0;
    while (i < in_size) {
        const uint8_t token = in[i++];
        size_t length = token >> 4;
        if (length == 15) {
            uint8_t extra;
            do { if (i >= in_size) return false; extra = in[i++]; length += extra; } while (extra == 255);
        }
        if (i + length > in_size || o + length > out_size) return false;
        std::memcpy(out + o, in + i, length);
        i += length; o += length;
        if (i == in_size) break;
        if (i + 2 > in_size) return false;
        const size_t distance = in[i] | (in[i + 1] << 8);
        i += 2;
        length = (token & 15) + 4;
        if ((token & 15) == 15) {
            uint8_t extra;
            do { if (i >= in_size) return false; extra = in[i++]; length += extra; } while (extra == 255);
        }
        if (!distance || distance > o || o + length > out_size) return false;
        for (size_t k = 0; k < length; ++k, ++o) out[o] = out[o - distance];
    }
    return o == out_size;
}

inline bool pac_decompress(const std::vector<uint8_t> &source, uint64_t offset, uint32_t compressed,
                           uint32_t expected, const std::vector<std::pair<uint32_t, uint32_t>> &chunks,
                           std::vector<uint8_t> &out)
{
    if (offset + compressed > source.size()) return false;
    out.resize(expected);
    if (compressed == expected) {
        std::memcpy(out.data(), source.data() + offset, expected);
        return true;
    }
    uint64_t in_total = 0, out_total = 0;
    for (auto [comp, uncomp] : chunks) {
        if (in_total + comp > compressed || out_total + uncomp > expected) return false;
        if (!lz4_block(source.data() + offset + in_total, comp, out.data() + out_total, uncomp)) return false;
        in_total += comp; out_total += uncomp;
    }
    return in_total == compressed && out_total == expected;
}

inline bool Pac::add_entries(const std::vector<uint8_t> &root, std::string &error)
{
    Bytes r{root.data(), root.size()};
    const uint32_t data_count = r.u32(52);
    const uint64_t nodes = r.u64(56), indices = r.u64(64);
    for (uint32_t j = 0; j < data_count; ++j) {
        const int32_t n = r.i32(indices + 4ull * j);
        const uint64_t type_node = nodes + uint64_t(n) * 40;
        const uint64_t tree = r.u64(type_node + 8);
        const uint64_t file_nodes = r.u64(tree + 8);
        char path[512] = {};
        // Iterative depth-first walk with an explicit stack.
        std::vector<int32_t> stack{0};
        int guard = 0;
        while (!stack.empty()) {
            if (++guard > 1000000 || !r.ok) { error = "PAC tree walk out of bounds"; return false; }
            const int32_t idx = stack.back(); stack.pop_back();
            const uint64_t node = file_nodes + uint64_t(idx) * 40;
            const uint64_t name = r.u64(node), data = r.u64(node + 8), children = r.u64(node + 16);
            const uint16_t child_count = r.u16(node + 36);
            const uint8_t has_data = r.u8(node + 38), start = r.u8(node + 39);
            if (has_data) {
                const uint32_t size = r.u32(data + 4);
                const uint64_t data_offset = r.u64(data + 16);
                const uint64_t extension = r.u64(data + 32);
                const uint16_t flags = r.u16(data + 40);
                std::string filename(path, path + start);
                const std::string ext = r.cstr(extension);
                if (!ext.empty()) filename += "." + ext;
                if (!(flags & 1)) {
                    if (data_offset + size > root.size()) { error = "PAC entry out of bounds"; return false; }
                    files_[filename] = PacEntry{root.data() + data_offset, size};
                }
            } else if (name) {
                const std::string text = r.cstr(name);
                if (start + text.size() + 1 > sizeof(path)) { error = "PAC name too long"; return false; }
                std::memcpy(path + start, text.data(), text.size());
                path[start + text.size()] = 0;
            }
            // Push in reverse so children are visited in order (names build on parents).
            for (int k = int(child_count) - 1; k >= 0; --k) stack.push_back(r.i32(children + 4ull * k));
        }
    }
    return r.ok || (error = "PAC tree out of bounds", false);
}

inline bool Pac::load(const std::wstring &path, std::string &error)
{
    if (!read_file(path, source_)) { error = "cannot read archive"; return false; }
    Bytes s{source_.data(), source_.size()};
    if (source_.size() < 48 || std::memcmp(source_.data(), "PACx403L", 8) != 0) { error = "not a PACx403 archive"; return false; }
    if (s.u32(12) != source_.size()) { error = "PAC size mismatch"; return false; }
    const uint32_t offset = s.u32(16), comp = s.u32(20), uncomp = s.u32(24);
    const uint32_t parents_size = s.u32(32);
    const uint64_t table = 48ull + parents_size;
    const uint32_t chunk_count = s.u32(table);
    if (chunk_count > 65536) { error = "PAC chunk table invalid"; return false; }
    std::vector<std::pair<uint32_t, uint32_t>> chunks;
    for (uint32_t i = 0; i < chunk_count; ++i) chunks.push_back({s.u32(table + 4 + 8ull * i), s.u32(table + 8 + 8ull * i)});
    if (!s.ok) { error = "PAC header out of bounds"; return false; }
    roots_.emplace_back();
    if (!pac_decompress(source_, offset, comp, uncomp, chunks, roots_.back())) { error = "PAC root decompression failed"; return false; }
    {
        const auto &root = roots_.back();
        Bytes r{root.data(), root.size()};
        const uint32_t trees_size = r.u32(16), dep_count = r.u32(44);
        if (dep_count) {
            const uint64_t count = r.u64(48ull + trees_size), ptr = r.u64(56ull + trees_size);
            if (!r.ok || count > 4096) { error = "PAC dependency table invalid"; return false; }
            std::vector<std::vector<uint8_t>> deps;
            for (uint64_t i = 0; i < count; ++i) {
                const uint64_t d = ptr + i * 32;
                const uint32_t dcomp = r.u32(d + 8), duncomp = r.u32(d + 12), pos = r.u32(d + 16), n = r.u32(d + 20);
                const uint64_t chunk_ptr = r.u64(d + 24);
                if (n > 65536) { error = "PAC dependency chunks invalid"; return false; }
                std::vector<std::pair<uint32_t, uint32_t>> dchunks;
                for (uint32_t j = 0; j < n; ++j) dchunks.push_back({r.u32(chunk_ptr + 8ull * j), r.u32(chunk_ptr + 8ull * j + 4)});
                if (!r.ok) { error = "PAC dependency out of bounds"; return false; }
                deps.emplace_back();
                if (!pac_decompress(source_, pos, dcomp, duncomp, dchunks, deps.back())) { error = "PAC dependency decompression failed"; return false; }
            }
            for (auto &d : deps) roots_.push_back(std::move(d));
        }
    }
    for (const auto &root : roots_) if (!add_entries(root, error)) return false;
    return true;
}

} // namespace frontiers
