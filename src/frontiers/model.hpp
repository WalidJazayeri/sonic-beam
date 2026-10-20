// SPDX-License-Identifier: MIT
// Hedgehog Engine 2 (Needle / Mirage) model, material and DDS readers for the
// player's installed Sonic Frontiers files. Formats documented by HedgeLib
// (MIT, Radfordhound) and HedgeNeedle. Results stay in memory.
#pragma once
#include <cmath>
#include <string>
#include <vector>
#include "bytes.hpp"
#include "pac.hpp"

namespace frontiers {

struct Vertex {            // GPU layout, 40 bytes
    float pos[3];
    float normal[3];
    float uv[2];
    uint8_t joints[4];     // model node indices
    uint8_t weights[4];    // UNORM8, sum 255
};
static_assert(sizeof(Vertex) == 40, "vertex layout");

struct Texture {
    uint32_t width = 0, height = 0, dxgi = 0, mips = 0, block_bytes = 0;
    bool compressed = true;            // false: 4 bytes per pixel (block_bytes = 4)
    std::vector<uint8_t> data;         // all mips, tightly packed BC blocks
    std::vector<uint64_t> mip_offset;
};

struct Mesh {
    std::string material;
    uint32_t first_index = 0, index_count = 0, base_vertex = 0;
    int texture = -1;                  // index into Model::textures
    uint8_t wrap_u = 0, wrap_v = 0;
    bool alpha_test = false;
};

struct Node {
    std::string name;
    int parent = -1;
    float inverse_bind[16];            // row-major, acts on column vectors
};

struct Model {
    std::vector<Node> nodes;
    std::vector<Vertex> vertices;
    std::vector<uint16_t> indices;
    std::vector<Mesh> meshes;
    std::vector<Texture> textures;
    std::vector<std::string> texture_names;
    float min[3] = {1e9f, 1e9f, 1e9f}, max[3] = {-1e9f, -1e9f, -1e9f};
};

namespace detail {

struct SampleNode { uint64_t pos; uint32_t value; std::string name; };

inline bool sample_nodes(Bytes &d, uint64_t p, std::vector<SampleNode> &out, int depth = 0)
{
    if (depth > 16) return false;
    for (int guard = 0; guard < 4096; ++guard) {
        const uint32_t flags = d.be32(p), value = d.be32(p + 4);
        if (!d.has(p + 8, 8)) return false;
        std::string name(reinterpret_cast<const char *>(d.p + p + 8), 8);
        while (!name.empty() && (name.back() == ' ' || name.back() == 0)) name.pop_back();
        const uint32_t size = flags & 0x1fffffff;
        if (size < 16 || p + size > d.n) return false;
        out.push_back({p, value, name});
        if (!(flags & 0x20000000) && !sample_nodes(d, p + 16, out, depth + 1)) return false;
        if (flags & 0xc0000000) return true;
        p += size;
    }
    return false;
}

inline void dec3(uint32_t word, float out[3])
{
    for (int k = 0; k < 3; ++k) {
        int v = (word >> (10 * k)) & 1023;
        if (v & 512) v -= 1024;
        out[k] = std::max(v / 511.0f, -1.0f);
    }
}

} // namespace detail

inline bool parse_dds(const PacEntry &entry, Texture &t, std::string &error)
{
    Bytes d{entry.data, entry.size};
    if (entry.size < 128 || std::memcmp(entry.data, "DDS ", 4) || d.u32(4) != 124) { error = "bad DDS"; return false; }
    t.height = d.u32(12); t.width = d.u32(16); t.mips = std::max<uint32_t>(1, d.u32(28));
    char fourcc[5] = {};
    std::memcpy(fourcc, entry.data + 84, 4);
    uint64_t payload = 128;
    if (!std::strcmp(fourcc, "DX10")) { t.dxgi = d.u32(128); payload = 148; }
    else if (!std::strcmp(fourcc, "DXT1")) t.dxgi = 71;
    else if (!std::strcmp(fourcc, "DXT3")) t.dxgi = 74;
    else if (!std::strcmp(fourcc, "DXT5")) t.dxgi = 77;
    else if (!std::strcmp(fourcc, "ATI2")) t.dxgi = 83;
    else if (!std::strcmp(fourcc, "ATI1")) t.dxgi = 80;
    else if (!fourcc[0] && d.u32(88) == 32) {          // uncompressed 32-bit
        t.dxgi = d.u32(92) == 0xff0000 ? 87 : 28;
        t.compressed = false;
    }
    if (!t.compressed) t.block_bytes = 4;
    else switch (t.dxgi) {
    case 70: case 71: case 72: case 79: case 80: case 81: t.block_bytes = 8; break;
    case 73: case 74: case 75: case 76: case 77: case 78: case 82: case 83: case 84:
    case 94: case 95: case 96: case 97: case 98: case 99: t.block_bytes = 16; break;
    default: error = "unsupported DDS format"; return false;
    }
    if (!t.width || !t.height || t.width > 8192 || t.height > 8192 || t.mips > 14) { error = "bad DDS size"; return false; }
    uint64_t total = 0;
    for (uint32_t m = 0; m < t.mips; ++m) {
        const uint32_t w = std::max(1u, t.width >> m), h = std::max(1u, t.height >> m);
        t.mip_offset.push_back(total);
        total += t.compressed ? uint64_t((w + 3) / 4) * ((h + 3) / 4) * t.block_bytes : uint64_t(w) * h * 4;
    }
    if (payload + total > entry.size) { error = "DDS payload short"; return false; }
    t.data.assign(entry.data + payload, entry.data + payload + total);
    return true;
}

struct MaterialInfo { std::string diffuse; uint8_t wrap_u = 0, wrap_v = 0; bool alpha_test = false; };

inline bool parse_material(const PacEntry &entry, MaterialInfo &out)
{
    Bytes d{entry.data, entry.size};
    std::vector<detail::SampleNode> nodes;
    if (!detail::sample_nodes(d, 16, nodes)) return false;
    uint64_t context = 0;
    for (auto &n : nodes) if (n.name == "Contexts" && n.value == 3) context = n.pos + 16;
    if (!context) return false;
    auto pointer = [&](uint64_t at) -> uint64_t { const uint32_t v = d.be32(at); return v ? 16ull + v : 0; };
    out.alpha_test = d.u8(context + 16) != 0;
    const uint64_t textures = pointer(context + 12);
    const uint8_t count = d.u8(context + 23);
    for (uint8_t i = 0; i < count; ++i) {
        const uint64_t tex = pointer(textures + 4ull * i);
        const std::string type = d.cstr(pointer(tex + 8));
        if (type == "diffuse") {
            out.diffuse = d.cstr(pointer(tex));
            out.wrap_u = d.u8(tex + 5); out.wrap_v = d.u8(tex + 6);
            break;
        }
    }
    return d.ok && !out.diffuse.empty();
}

// Reads `name` (a .model in `pac`) into `model`, with its diffuse textures.
inline bool parse_model(const Pac &pac, const std::string &name, Model &model, std::string &error)
{
    const PacEntry *entry = pac.find(name);
    if (!entry) { error = "missing " + name; return false; }
    Bytes raw{entry->data, entry->size};
    const uint8_t *payload = entry->data;
    size_t payload_size = entry->size;
    bool relative = false;
    if (entry->size > 16 && !std::memcmp(entry->data, "NEDARCV1", 8)) {
        const uint64_t end = uint64_t(raw.be32(8)) + 8;
        uint64_t pos = (13 + raw.cstr(12).size() + 3) & ~3ull;
        bool found = false;
        while (pos < end && raw.ok) {
            const std::string ext = raw.cstr(pos + 8);
            const uint64_t size_pos = (pos + 9 + ext.size() + 3) & ~3ull;
            const uint32_t size = raw.be32(size_pos);
            if (!raw.has(size_pos + 4, size)) break;
            if (!std::memcmp(entry->data + pos, "NEDMDLV5", 8)) {   // first = LOD0
                payload = entry->data + size_pos + 4; payload_size = size; relative = true; found = true; break;
            }
            pos = size_pos + 4 + size;
        }
        if (!found) { error = "no LOD0 model in Needle archive"; return false; }
    }
    Bytes d{payload, payload_size};
    auto pointer = [&](uint64_t at) -> uint64_t {
        if (relative) { const int32_t v = d.i32(at); return v ? uint64_t(int64_t(at) + v) : 0; }
        const uint32_t v = d.be32(at); return v ? 16ull + v : 0;
    };
    std::vector<detail::SampleNode> props;
    if (!detail::sample_nodes(d, 16, props)) { error = "model chunk tree invalid"; return false; }
    uint64_t context = 0; uint32_t version = 0, topology = 4;
    for (auto &n : props) {
        if (n.name == "Contexts") { context = n.pos + 16; version = n.value; }
        if (n.name == "Topology") topology = n.value;
    }
    if (!context || (version != 5 && version != 6)) { error = "unsupported model version"; return false; }
    if (topology != 3) { error = "model is not a triangle list"; return false; }
    const uint32_t groups = d.be32(context), node_count = d.be32(context + 16);
    const uint64_t groups_ptr = pointer(context + 4), node_ptr = pointer(context + 20), matrices = pointer(context + 24);
    if (node_count > 255) { error = "too many skeleton nodes"; return false; }
    for (uint32_t i = 0; i < node_count; ++i) {
        const uint64_t p = pointer(node_ptr + 4ull * i);
        Node node;
        node.parent = d.bei32(p);
        node.name = d.cstr(pointer(p + 4));
        for (int k = 0; k < 16; ++k) node.inverse_bind[k] = d.bef32(matrices + 64ull * i + 4ull * k);
        model.nodes.push_back(node);
    }
    std::vector<std::string> materials;
    for (uint32_t g = 0; g < groups; ++g) {
        const uint64_t group = pointer(groups_ptr + 4ull * g);
        struct Slot { uint32_t count; uint64_t meshes; bool alpha; };
        std::vector<Slot> slots;
        for (int j = 0; j < 3; ++j) slots.push_back({d.be32(group + 8ull * j), pointer(group + 8ull * j + 4), j > 0});
        const uint32_t special = d.be32(group + 24);
        if (special) {
            const uint64_t counts = pointer(group + 32), meshes = pointer(group + 36);
            for (uint32_t j = 0; j < special && j < 64; ++j) slots.push_back({d.be32(pointer(counts + 4ull * j)), pointer(meshes + 4ull * j), true});
        }
        for (auto &slot : slots) {
            for (uint32_t j = 0; j < slot.count && j < 1024; ++j) {
                const uint64_t mesh = pointer(slot.meshes + 4ull * j);
                Mesh out;
                out.material = d.cstr(pointer(mesh));
                const uint32_t face_count = d.be32(mesh + 4), vertex_count = d.be32(mesh + 12), stride = d.be32(mesh + 16);
                const uint64_t face_ptr = pointer(mesh + 8), vertex_ptr = pointer(mesh + 20), decl_ptr = pointer(mesh + 24);
                const uint32_t palette_count = d.be32(mesh + 28);
                const uint64_t palette_ptr = pointer(mesh + 32);
                if (!d.ok || face_count % 3 || !d.has(face_ptr, face_count * 2ull) || !d.has(vertex_ptr, uint64_t(vertex_count) * stride)) {
                    error = "mesh buffer out of bounds"; return false;
                }
                int pos_off = -1, nrm_off = -1, uv_off = -1, w_off = -1, j_off = -1;
                for (int k = 0; k < 32; ++k) {
                    const uint64_t e = decl_ptr + 12ull * k;
                    const uint32_t fmt = d.be32(e + 4);
                    if (fmt == 0xffffffff) break;
                    const int offset = d.be16(e + 2), usage = d.u8(e + 9), index = d.u8(e + 10);
                    if (usage == 0 && fmt == 0x2a23b9) pos_off = offset;
                    else if (usage == 3 && fmt == 0x2a2187 && index == 0) nrm_off = offset;
                    else if (usage == 5 && fmt == 0x2c235f && index == 0) uv_off = offset;
                    else if (usage == 1 && fmt == 0x1a2086) w_off = offset;
                    else if (usage == 2 && fmt == 0x1a2286) j_off = offset;
                }
                if (pos_off < 0) { error = "mesh without float3 positions"; return false; }
                std::vector<uint32_t> palette;
                for (uint32_t k = 0; k < palette_count; ++k)
                    palette.push_back(version == 5 ? d.u8(palette_ptr + k) : d.be16(palette_ptr + 2ull * k));
                out.base_vertex = uint32_t(model.vertices.size());
                for (uint32_t v = 0; v < vertex_count; ++v) {
                    const uint64_t base = vertex_ptr + uint64_t(v) * stride;
                    Vertex vx{};
                    for (int a = 0; a < 3; ++a) {
                        vx.pos[a] = d.bef32(base + pos_off + 4 * a);
                        if (!std::isfinite(vx.pos[a])) { error = "non-finite vertex"; return false; }
                        model.min[a] = std::min(model.min[a], vx.pos[a]);
                        model.max[a] = std::max(model.max[a], vx.pos[a]);
                    }
                    if (nrm_off >= 0) detail::dec3(d.be32(base + nrm_off), vx.normal); else vx.normal[1] = 1;
                    if (uv_off >= 0) { vx.uv[0] = half_to_float(d.be16(base + uv_off)); vx.uv[1] = half_to_float(d.be16(base + uv_off + 2)); }
                    if (w_off >= 0 && j_off >= 0 && !palette.empty()) {
                        unsigned sum = 0;
                        for (int k = 0; k < 4; ++k) sum += d.u8(base + w_off + k);
                        if (!sum) { vx.weights[0] = 255; vx.joints[0] = uint8_t(palette[0]); }
                        else {
                            unsigned acc = 0;
                            for (int k = 0; k < 4; ++k) {
                                const unsigned w = d.u8(base + w_off + k), ref = d.u8(base + j_off + k);
                                if (!w) continue;
                                if (ref >= palette.size() || palette[ref] >= node_count) { error = "skin index out of range"; return false; }
                                vx.joints[k] = uint8_t(palette[ref]);
                                vx.weights[k] = uint8_t(w * 255 / sum);
                                acc += vx.weights[k];
                            }
                            for (int k = 0; k < 4; ++k) if (vx.weights[k]) { vx.weights[k] += uint8_t(255 - acc); break; }
                        }
                    } else {
                        vx.weights[0] = 255;   // rigid: bound to the first node (identity when unanimated)
                    }
                    model.vertices.push_back(vx);
                }
                out.first_index = uint32_t(model.indices.size());
                out.index_count = face_count;
                for (uint32_t k = 0; k < face_count; ++k) {
                    const uint16_t idx = d.be16(face_ptr + 2ull * k);
                    if (idx >= vertex_count) { error = "mesh index out of range"; return false; }
                    model.indices.push_back(idx);
                }
                out.alpha_test = slot.alpha;
                model.meshes.push_back(out);
            }
        }
    }
    if (!d.ok) { error = "model out of bounds"; return false; }
    // Materials -> first diffuse texture of each, loaded once.
    for (auto &mesh : model.meshes) {
        const PacEntry *mat = pac.find(mesh.material + ".material");
        MaterialInfo info;
        if (!mat || !parse_material(*mat, info)) continue;
        mesh.wrap_u = info.wrap_u; mesh.wrap_v = info.wrap_v;
        mesh.alpha_test = mesh.alpha_test || info.alpha_test;
        for (size_t t = 0; t < model.texture_names.size(); ++t) if (model.texture_names[t] == info.diffuse) mesh.texture = int(t);
        if (mesh.texture < 0) {
            const PacEntry *dds = pac.find(info.diffuse + ".dds");
            Texture tex;
            std::string tex_error;
            if (dds && parse_dds(*dds, tex, tex_error)) {
                mesh.texture = int(model.textures.size());
                model.textures.push_back(std::move(tex));
                model.texture_names.push_back(info.diffuse);
            }
        }
    }
    return !model.meshes.empty() || (error = "model has no meshes", false);
}

} // namespace frontiers
