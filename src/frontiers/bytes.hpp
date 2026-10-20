// SPDX-License-Identifier: MIT
// Bounds-checked little/big-endian readers. A read past the end returns 0 and
// clears `ok`, so parsers check once after a block of reads.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace frontiers {

struct Bytes {
    const uint8_t *p = nullptr;
    size_t n = 0;
    bool ok = true;

    bool has(uint64_t at, uint64_t len) { if (at > n || len > n - at) { ok = false; return false; } return true; }
    uint8_t u8(uint64_t at) { return has(at, 1) ? p[at] : 0; }
    uint16_t u16(uint64_t at) { uint16_t v = 0; if (has(at, 2)) std::memcpy(&v, p + at, 2); return v; }
    uint32_t u32(uint64_t at) { uint32_t v = 0; if (has(at, 4)) std::memcpy(&v, p + at, 4); return v; }
    int32_t i32(uint64_t at) { return int32_t(u32(at)); }
    int16_t i16(uint64_t at) { return int16_t(u16(at)); }
    uint64_t u64(uint64_t at) { uint64_t v = 0; if (has(at, 8)) std::memcpy(&v, p + at, 8); return v; }
    float f32(uint64_t at) { float v = 0; if (has(at, 4)) std::memcpy(&v, p + at, 4); return v; }
    uint16_t be16(uint64_t at) { return has(at, 2) ? uint16_t(p[at] << 8 | p[at + 1]) : 0; }
    uint32_t be32(uint64_t at) {
        return has(at, 4) ? uint32_t(p[at]) << 24 | uint32_t(p[at + 1]) << 16 | uint32_t(p[at + 2]) << 8 | p[at + 3] : 0;
    }
    int32_t bei32(uint64_t at) { return int32_t(be32(at)); }
    float bef32(uint64_t at) { uint32_t v = be32(at); float f; std::memcpy(&f, &v, 4); return f; }
    std::string cstr(uint64_t at) {
        if (!at || at >= n) { if (at) ok = false; return {}; }
        size_t end = at;
        while (end < n && p[end]) ++end;
        if (end == n) { ok = false; return {}; }
        return std::string(reinterpret_cast<const char *>(p + at), end - at);
    }
};

inline float half_to_float(uint16_t h)
{
    const uint32_t sign = uint32_t(h & 0x8000) << 16;
    uint32_t exponent = (h >> 10) & 31, mantissa = h & 1023, bits;
    if (exponent == 0) {
        if (!mantissa) bits = sign;
        else {
            exponent = 127 - 15 + 1;
            while (!(mantissa & 1024)) { mantissa <<= 1; --exponent; }
            bits = sign | exponent << 23 | (mantissa & 1023) << 13;
        }
    } else if (exponent == 31) bits = sign | 0x7f800000 | mantissa << 13;
    else bits = sign | (exponent + 127 - 15) << 23 | mantissa << 13;
    float f; std::memcpy(&f, &bits, 4); return f;
}

inline bool read_file(const std::wstring &path, std::vector<uint8_t> &out)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (1ll << 31);
    if (ok) {
        out.resize(size_t(size.QuadPart));
        size_t done = 0;
        while (ok && done < out.size()) {
            DWORD got = 0;
            const DWORD want = DWORD(std::min<size_t>(out.size() - done, 1u << 24));
            ok = ReadFile(file, out.data() + done, want, &got, nullptr) && got > 0;
            done += got;
        }
    }
    CloseHandle(file);
    return ok;
}

} // namespace frontiers
