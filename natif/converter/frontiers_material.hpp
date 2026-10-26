// SPDX-License-Identifier: MIT
// Every texture slot of a Hedgehog Engine 2 .material, built on the shared reader's chunk walker
// (frontiers::parse_material keeps only the diffuse). Layout as in src/frontiers/model.hpp:
// textures @12, alphaThreshold @16, noBackFaceCulling @17, texture count @23 of the Contexts block;
// per texture: name @0, texcoord @4, wrap @5/6, type @8.
#pragma once
#include <string>
#include <vector>
#include "../../src/frontiers/model.hpp"

namespace sonicnative {

// Wrap mode the overlay samples with CLAMP (beamng/src/render11.cpp); the others it samples with WRAP.
constexpr uint8_t kWrapClamp = 2;

struct MaterialSlot { std::string name, type; uint8_t texcoord = 0, wrap_u = 0, wrap_v = 0; };

struct MaterialTextures {
    std::vector<MaterialSlot> slots;
    uint8_t alpha_threshold = 0;
    bool double_sided = false;
    // First texture of a type ("diffuse", "normal", "specular", "reflection"), or empty.
    std::string first(const char *type) const {
        for (auto &s : slots) if (s.type == type) return s.name;
        return {};
    }
};

inline bool read_material(const frontiers::PacEntry &entry, MaterialTextures &out)
{
    frontiers::Bytes d{entry.data, entry.size};
    std::vector<frontiers::detail::SampleNode> nodes;
    if (!frontiers::detail::sample_nodes(d, 16, nodes)) return false;
    uint64_t context = 0;
    for (auto &n : nodes) if (n.name == "Contexts" && n.value == 3) context = n.pos + 16;
    if (!context) return false;
    auto pointer = [&](uint64_t at) -> uint64_t { const uint32_t v = d.be32(at); return v ? 16ull + v : 0; };
    out.alpha_threshold = d.u8(context + 16);
    out.double_sided = d.u8(context + 17) != 0;
    const uint64_t textures = pointer(context + 12);
    const uint8_t count = d.u8(context + 23);
    for (uint8_t i = 0; i < count; ++i) {
        const uint64_t tex = pointer(textures + 4ull * i);
        MaterialSlot s;
        s.name = d.cstr(pointer(tex));
        s.type = d.cstr(pointer(tex + 8));
        s.texcoord = d.u8(tex + 4); s.wrap_u = d.u8(tex + 5); s.wrap_v = d.u8(tex + 6);
        out.slots.push_back(s);
    }
    return d.ok;
}

} // namespace sonicnative
