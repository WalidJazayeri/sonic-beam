// SPDX-License-Identifier: MIT
// Text helpers for the converter: one number format shared by the DAE and the self-check,
// XML/JSON escaping, FNV-1a and UTF-8 <-> UTF-16 for Windows paths.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace sonicnative {

// Every float the DAE holds goes through this format; quantize() returns what a reader gets back,
// so the self-check runs on the numbers as written.
inline int format_float(float v, char *buf, size_t size)
{
    if (!(std::fabs(v) >= 5e-8f)) v = 0;       // also drops -0 and NaN (never written: checked upstream)
    return std::snprintf(buf, size, "%.7g", v);
}
inline float quantize(float v)
{
    char buf[32];
    format_float(v, buf, sizeof(buf));
    return std::strtof(buf, nullptr);
}

struct Text {
    std::string s;
    Text &operator<<(const char *t) { s += t; return *this; }
    Text &operator<<(const std::string &t) { s += t; return *this; }
    Text &operator<<(char c) { s += c; return *this; }
    Text &num(float v) { char b[32]; s.append(b, size_t(format_float(v, b, sizeof(b)))); return *this; }
    Text &u(uint64_t v) { char b[24]; s.append(b, size_t(std::snprintf(b, sizeof(b), "%llu", (unsigned long long)v))); return *this; }
    // Space-separated list, wrapped so the file stays readable in an editor.
    Text &nums(const float *v, size_t n, size_t per_line = 16) {
        for (size_t i = 0; i < n; ++i) { if (i) s += (i % per_line) ? ' ' : '\n'; num(v[i]); }
        return *this;
    }
};

inline std::string xml_escape(const std::string &in)
{
    std::string out;
    for (char c : in) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

inline std::string json_escape(const std::string &in)
{
    std::string out = "\"";
    for (unsigned char c : in) {
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
        else out += char(c);
    }
    return out + "\"";
}

inline std::string json_num(double v)
{
    char b[32];
    std::snprintf(b, sizeof(b), "%.6g", std::fabs(v) < 1e-9 ? 0.0 : v);
    return b;
}

inline uint32_t fnv1a(const std::string &s, uint32_t h = 2166136261u)
{
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h;
}

inline std::wstring widen(const std::string &s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

inline std::string narrow(const std::wstring &w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

inline bool write_file(const std::wstring &path, const void *data, size_t size)
{
    FILE *f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    const bool ok = std::fwrite(data, 1, size, f) == size;
    return std::fclose(f) == 0 && ok;
}
inline bool write_file(const std::wstring &path, const std::string &text) { return write_file(path, text.data(), text.size()); }

} // namespace sonicnative
