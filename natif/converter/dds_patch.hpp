// SPDX-License-Identifier: MIT
// DDS edits that keep the compressed blocks: mark an albedo sRGB (header only) and flip the green
// channel of a BC5 normal map inside its BC4 blocks (endpoints and indices remapped, no re-encode).
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "../../src/frontiers/model.hpp"

namespace sonicnative {

// UNORM -> UNORM_SRGB for the formats that have one; returns 0 when there is none.
inline uint32_t srgb_format(uint32_t dxgi)
{
    switch (dxgi) {
    case 71: case 72: return 72;    // BC1
    case 74: case 75: return 75;    // BC2
    case 77: case 78: return 78;    // BC3
    case 98: case 99: return 99;    // BC7
    case 28: case 29: return 29;    // R8G8B8A8
    case 87: case 91: return 91;    // B8G8R8A8
    default: return 0;
    }
}

inline const char *dxgi_name(uint32_t dxgi)
{
    switch (dxgi) {
    case 28: return "R8G8B8A8_UNORM"; case 29: return "R8G8B8A8_UNORM_SRGB";
    case 71: return "BC1_UNORM"; case 72: return "BC1_UNORM_SRGB";
    case 74: return "BC2_UNORM"; case 75: return "BC2_UNORM_SRGB";
    case 77: return "BC3_UNORM"; case 78: return "BC3_UNORM_SRGB";
    case 80: return "BC4_UNORM"; case 83: return "BC5_UNORM"; case 84: return "BC5_SNORM";
    case 87: return "B8G8R8A8_UNORM"; case 91: return "B8G8R8A8_UNORM_SRGB";
    case 98: return "BC7_UNORM"; case 99: return "BC7_UNORM_SRGB";
    default: return "other";
    }
}

inline bool dds_is_dx10(const uint8_t *d, size_t n) { return n >= 148 && !std::memcmp(d + 84, "DX10", 4); }

// Copies a DDS with its format switched to the _SRGB variant. A legacy FourCC header (DXT1...) gets a
// DX10 header inserted (20 bytes), since only DX10 can say sRGB; the payload is copied byte for byte.
inline bool dds_srgb(const uint8_t *d, size_t n, std::vector<uint8_t> &out, uint32_t &from, uint32_t &to, std::string &error)
{
    frontiers::Texture info;
    if (!frontiers::parse_dds(frontiers::PacEntry{d, uint32_t(n)}, info, error)) return false;
    from = info.dxgi;
    to = srgb_format(from);
    if (!to) { error = "no sRGB variant"; return false; }
    if (dds_is_dx10(d, n)) {
        out.assign(d, d + n);
        std::memcpy(out.data() + 128, &to, 4);
        return true;
    }
    out.assign(d, d + 128);
    const uint32_t fourcc_flag = 0x4;                       // DDPF_FOURCC
    std::memcpy(out.data() + 80, &fourcc_flag, 4);
    std::memcpy(out.data() + 84, "DX10", 4);
    std::memset(out.data() + 88, 0, 20);                    // bit count and masks
    const uint32_t dx10[5] = {to, 3 /* TEXTURE2D */, 0, 1 /* array size */, 0};
    out.insert(out.end(), reinterpret_cast<const uint8_t *>(dx10), reinterpret_cast<const uint8_t *>(dx10) + 20);
    out.insert(out.end(), d + 128, d + n);
    return true;
}

namespace bc4 {

inline void decode(const uint8_t *b, int out[16])
{
    int pal[8] = {b[0], b[1]};
    if (b[0] > b[1]) for (int i = 2; i < 8; ++i) pal[i] = ((8 - i) * b[0] + (i - 1) * b[1]) / 7;
    else { for (int i = 2; i < 6; ++i) pal[i] = ((6 - i) * b[0] + (i - 1) * b[1]) / 5; pal[6] = 0; pal[7] = 255; }
    uint64_t bits = 0;
    for (int k = 0; k < 6; ++k) bits |= uint64_t(b[2 + k]) << (8 * k);
    for (int t = 0; t < 16; ++t) out[t] = pal[(bits >> (3 * t)) & 7];
}

// v -> 255 - v. Swapping the endpoints as e0' = 255 - e1, e1' = 255 - e0 keeps the block's mode;
// 8-value mode: indices 0<->1, i -> 9 - i; 6-value mode: 0<->1, i -> 7 - i, and 6 (0) <-> 7 (255).
inline void flip(uint8_t *b)
{
    const uint8_t e0 = b[0], e1 = b[1];
    const bool eight = e0 > e1;
    uint64_t bits = 0, out = 0;
    for (int k = 0; k < 6; ++k) bits |= uint64_t(b[2 + k]) << (8 * k);
    for (int t = 0; t < 16; ++t) {
        const unsigned i = (bits >> (3 * t)) & 7;
        const unsigned j = i < 2 ? i ^ 1 : eight ? 9 - i : i < 6 ? 7 - i : i ^ 1;
        out |= uint64_t(j) << (3 * t);
    }
    b[0] = uint8_t(255 - e1);
    b[1] = uint8_t(255 - e0);
    for (int k = 0; k < 6; ++k) b[2 + k] = uint8_t(out >> (8 * k));
}

} // namespace bc4

// Flips green in every block of a BC5 DDS (all mips) and checks each block decodes to 255 - old (+-1).
inline bool dds_flip_green(std::vector<uint8_t> &dds, std::string &error)
{
    frontiers::Texture info;
    if (!frontiers::parse_dds(frontiers::PacEntry{dds.data(), uint32_t(dds.size())}, info, error)) return false;
    if (info.dxgi != 83) { error = "not BC5_UNORM"; return false; }
    const size_t payload = dds_is_dx10(dds.data(), dds.size()) ? 148 : 128;
    for (size_t at = payload; at + 16 <= payload + info.data.size(); at += 16) {
        uint8_t *green = dds.data() + at + 8;
        int before[16], after[16];
        bc4::decode(green, before);
        bc4::flip(green);
        bc4::decode(green, after);
        for (int t = 0; t < 16; ++t)
            if (std::abs(after[t] - (255 - before[t])) > 1) { error = "BC4 flip check failed"; return false; }
    }
    return true;
}

} // namespace sonicnative
