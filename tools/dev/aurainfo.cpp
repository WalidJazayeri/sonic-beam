// SPDX-License-Identifier: MIT
// Developer check: does Sonic Frontiers' boost aura mesh (chr_sonic_aura.model) share Sonic's
// skeleton? Reads the player's install; prints counts and differences only.
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include "../../src/frontiers/pac.hpp"
#include "../../src/frontiers/model.hpp"

int wmain(int argc, wchar_t **argv)
{
    std::wstring root = argc > 1 ? argv[1] : L"C:/games/Sonic Frontiers";
    std::string error;
    frontiers::Pac pac;
    if (!pac.load(root + L"/image/x64/raw/character/sonic.pac", error)) { std::printf("pac: %s\n", error.c_str()); return 1; }
    frontiers::Model sonic, aura;
    if (!frontiers::parse_model(pac, "chr_sonic.model", sonic, error)) { std::printf("sonic: %s\n", error.c_str()); return 1; }
    if (!frontiers::parse_model(pac, "chr_sonic_aura.model", aura, error)) { std::printf("aura: %s\n", error.c_str()); return 1; }
    std::printf("sonic: %zu nodes %zu vertices %zu meshes\n", sonic.nodes.size(), sonic.vertices.size(), sonic.meshes.size());
    std::printf("aura:  %zu nodes %zu vertices %zu meshes, bounds y %.2f..%.2f\n", aura.nodes.size(), aura.vertices.size(), aura.meshes.size(), aura.min[1], aura.max[1]);
    for (auto &m : aura.meshes) std::printf("  mesh %s idx %u tex %d\n", m.material.c_str(), m.index_count, m.texture);
    std::unordered_map<std::string, int> by_name;
    for (size_t i = 0; i < sonic.nodes.size(); ++i) by_name[sonic.nodes[i].name] = int(i);
    int missing = 0, same_index = 0; float worst = 0;
    for (size_t i = 0; i < aura.nodes.size(); ++i) {
        auto it = by_name.find(aura.nodes[i].name);
        if (it == by_name.end()) { if (missing++ < 5) std::printf("  aura node not in Sonic: %s\n", aura.nodes[i].name.c_str()); continue; }
        if (it->second == int(i)) ++same_index;
        for (int k = 0; k < 16; ++k) worst = std::max(worst, std::fabs(aura.nodes[i].inverse_bind[k] - sonic.nodes[it->second].inverse_bind[k]));
    }
    std::printf("aura nodes missing in Sonic: %d, same index: %d, worst inverse-bind difference: %g\n", missing, same_index, worst);
    // do the normals point out of the body? compare with the direction from the mesh's centre line
    const float cx = (aura.min[0] + aura.max[0]) / 2, cz = (aura.min[2] + aura.max[2]) / 2;
    int outward = 0, inward = 0; double len = 0;
    for (auto &v : aura.vertices) {
        const float dx = v.pos[0] - cx, dz = v.pos[2] - cz;
        const float d = dx * v.normal[0] + dz * v.normal[2];
        (d >= 0 ? outward : inward)++;
        len += std::sqrt(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] + v.normal[2] * v.normal[2]);
    }
    std::printf("aura normals: %d outward, %d inward, mean length %.3f\n", outward, inward, len / std::max<size_t>(1, aura.vertices.size()));
    int so = 0, si = 0;
    const float sx = (sonic.min[0] + sonic.max[0]) / 2, sz = (sonic.min[2] + sonic.max[2]) / 2;
    for (auto &v : sonic.vertices) ((v.pos[0] - sx) * v.normal[0] + (v.pos[2] - sz) * v.normal[2] >= 0 ? so : si)++;
    std::printf("sonic normals: %d outward, %d inward\n", so, si);
    for (size_t i = 0; i < sonic.nodes.size(); ++i) {
        const std::string &n = sonic.nodes[i].name;
        if (n.find("Foot") != std::string::npos || n.find("foot") != std::string::npos || n.find("Toe") != std::string::npos || n.find("toe") != std::string::npos)
            std::printf("  node %zu %s\n", i, n.c_str());
    }
    return 0;
}
