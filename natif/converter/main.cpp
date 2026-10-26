// SPDX-License-Identifier: MIT
// sonicnative_convert: turns Sonic from the player's own Sonic Frontiers install into a skinned,
// animated COLLADA shape, BeamNG materials and DDS textures inside the BeamNG user folder
// (<userpath>\current\mods\sonicnative_data). Nothing it writes belongs in the repository or a package.
#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>
#include "../../src/frontiers/pac.hpp"
#include "../../src/frontiers/model.hpp"
#include "../../src/frontiers/anim.hpp"
#include "sonic_data.hpp"
#include "text.hpp"
#include "skin_math.hpp"
#include "dae_writer.hpp"
#include "dds_patch.hpp"
#include "frontiers_material.hpp"
#include "fixture.hpp"
#include <tlhelp32.h>   // after windows.h (text.hpp)

#ifndef SONICNATIVE_VERSION
#define SONICNATIVE_VERSION 0.0.0
#endif
#define SN_STR2(x) #x
#define SN_STR(x) SN_STR2(x)

namespace fs = std::filesystem;
using namespace sonicnative;

namespace {

const char *const kVersion = SN_STR(SONICNATIVE_VERSION);
const char *const kDataFolder = "sonicnative_data";     // <userpath>\current\mods\<this>, VFS /mods/<this>

struct Options {
    std::wstring userpath, frontiers;
    std::string clips = sonic_data::kDefaultClips;
    Split split = Split::clips;
    bool fixture = false, bind_only = false, flip_green = false, srgb = true, flat = false, uv_clamp = true;
};

void usage()
{
    std::printf("sonicnative_convert %s\n"
                "usage: sonicnative_convert --userpath <BeamNG user folder> [--frontiers <Sonic Frontiers folder>]\n"
                "                           [--clips %s] [--split clips|cs] [--bind-only] [--flat] [--flip-green] [--no-srgb]\n"
                "                           [--no-uv-clamp] [--fixture]\n"
                "  --userpath    the folder BeamNG runs with (-userpath); output goes to <it>\\current\\mods\\%s\n"
                "  --frontiers   default: frontiers= line of %%LOCALAPPDATA%%\\SonicBeam\\settings.ini\n"
                "  --clips       clip ids (", kVersion, sonic_data::kDefaultClips, kDataFolder);
    for (size_t i = 0; i < sizeof(sonic_data::kClips) / sizeof(sonic_data::kClips[0]); ++i) std::printf("%s%s", i ? ", " : "", sonic_data::kClips[i].id);
    std::printf(")\n"
                "  --split       clips: one <animation_clip> per clip (default); cs: one 'ambient' + a TSShapeConstructor .cs\n"
                "  --bind-only   no animation (diagnostic: bind pose only)\n"
                "  --flat        every joint under the armature with model-space locals (no inherited scale or shear)\n"
                "  --flip-green  invert the green channel of the BC5 normal maps (lossless)\n"
                "  --no-srgb     keep the albedo headers as they are (UNORM)\n"
                "  --no-uv-clamp keep UVs outside [0,1] on clamp-addressed textures (eyes) as they are\n"
                "  --fixture     synthetic 2-joint test shape (clips idle, lean) into %s\\fixture, no Frontiers data\n", kDataFolder);
}

bool beamng_running()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        found = !_wcsicmp(pe.szExeFile, L"BeamNG.drive.x64.exe");
    CloseHandle(snap);
    return found;
}

std::wstring trim_folder(std::wstring dir)
{
    for (auto &c : dir) if (c == L'/') c = L'\\';
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L' ' || dir.back() == L'\r' || dir.back() == L'"')) dir.pop_back();
    while (!dir.empty() && (dir.front() == L' ' || dir.front() == L'"')) dir.erase(dir.begin());
    return dir;
}

// The overlay's settings file names the Frontiers folder (frontiers=C:/games/Sonic Frontiers).
std::wstring settings_frontiers()
{
    wchar_t local[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    std::vector<uint8_t> raw;
    if (!frontiers::read_file(std::wstring(local) + L"\\SonicBeam\\settings.ini", raw)) return {};
    std::string text(raw.begin(), raw.end());
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF) line.erase(0, 3);   // UTF-8 BOM
        if (line.compare(0, 10, "frontiers=") == 0) return trim_folder(widen(line.substr(10)));
        at = end + 1;
    }
    return {};
}

std::string hex8(uint32_t v) { char b[12]; std::snprintf(b, sizeof(b), "%08x", v); return b; }

// "idle, run,boost" -> {idle, run, boost}
std::vector<std::string> clip_ids(const std::string &list)
{
    std::vector<std::string> out;
    std::string id;
    for (size_t i = 0; i <= list.size(); ++i) {
        const char c = i < list.size() ? list[i] : ',';
        if (c == ',') { if (!id.empty()) out.push_back(id); id.clear(); }
        else if (c != ' ' && c != '\t') id += c;
    }
    return out;
}

std::string common_prefix(const std::vector<std::string> &names)
{
    std::string p = names.empty() ? std::string() : names[0];
    for (auto &n : names) {
        size_t k = 0;
        while (k < p.size() && k < n.size() && p[k] == n[k]) ++k;
        p.resize(k);
    }
    while (!p.empty() && p.back() == '_') p.pop_back();
    return p.empty() ? names[0] : p;
}

// One game material: the Frontiers meshes sharing albedo, normal map and culling share it.
struct MaterialGroup {
    std::string name, albedo, normal;
    bool double_sided = false;
    std::vector<std::string> meshes;
    std::string albedo_file, normal_file;     // relative to the package folder ("tex/...")
};

struct TextureOut { std::string file; std::vector<uint8_t> bytes; };

// Everything one conversion writes, prepared before any file is touched.
struct Package {
    std::string folder;              // sonic | fixture
    std::string stem;                // <stem>_<hash>.dae
    std::string materials_file, datablocks_file, datablock, ctor;
    std::string materials_json;
    std::vector<TextureOut> textures;
    float model_height = 1;
    Rig rig;
};

std::string vfs(const Package &p, const std::string &file) { return std::string("/mods/") + kDataFolder + "/" + p.folder + "/" + file; }

std::string material_json(const std::string &name, const std::string &stage, bool double_sided)
{
    std::string s = "  " + json_escape(name) + ": {\n    \"name\": " + json_escape(name) + ", \"mapTo\": " + json_escape(name) +
                    ", \"class\": \"Material\", \"version\": 1.5,\n    \"Stages\": [{" + stage + "}, {}, {}, {}]";
    if (double_sided) s += ",\n    \"doubleSided\": true";
    return s + "\n  }";
}

void print_rig(const Rig &rig)
{
    size_t verts = 0, tris = 0, animated = 0;
    for (auto &j : rig.joints) animated += j.animated;
    std::printf("rig: %zu joints (%zu animated, %s hierarchy), %zu meshes\n", rig.joints.size(), animated, rig.flat ? "flat" : "Frontiers", rig.meshes.size());
    for (auto &m : rig.meshes) {
        size_t most = 0;
        for (auto c : m.vcount) most = std::max<size_t>(most, c);
        verts += m.vcount.size(); tris += m.triangles.size() / 3;
        std::printf("  mesh %-10s -> %-20s %6zu vertices %6zu triangles, %3zu joints, <= %zu influences; winding %zu agree / %zu opposite / %zu degenerate%s\n",
                    (m.object + "2").c_str(), m.material.c_str(), m.vcount.size(), m.triangles.size() / 3, m.joints.size(), most,
                    m.agreeing, m.flipped, m.degenerate, m.reversed ? " -> reversed" : "");
        if (m.clamp_u || m.clamp_v)
            std::printf("    uv clamp %s%s: %zu vertices brought into [0,1]\n", m.clamp_u ? "u" : "", m.clamp_v ? "v" : "", m.uv_clamped);
    }
    std::printf("  totals: %zu vertices, %zu triangles\n", verts, tris);
    std::printf("self-check: bind %.3g (limit %.0e)\n", rig.bind_check, kSelfCheckLimit);
    for (auto &c : rig.clips)
        std::printf("  clip %-10s frames %3u..%3u (%u kept of %u) %s  check %.3g  max|scale-1| %.3g  shear %.3g  min det %.3g  loop seam %.3g  root motion %.3f m\n",
                    c.name.c_str(), c.first, c.first + c.frames - 1, c.frames, c.source_frames, c.loop ? "loop    " : "one-shot",
                    c.check, c.max_scale, c.max_shear, c.min_det, c.seam, c.root_motion);
    float scale = 0, shear = 0, det = 1;
    for (auto &c : rig.clips) { scale = std::max(scale, c.max_scale); shear = std::max(shear, c.max_shear); det = std::min(det, c.min_det); }
    if (det <= 0) std::printf("  warning: a joint local is mirrored or degenerate (det %.3g): Torque cannot decompose it\n", det);
    if (scale > 0.01f || shear > 0.01f)
        std::printf("  note: joint locals carry scale (%.2f) and shear (%.2f): exact only through Torque's arbitrary-scale path;"
                    " if limbs look wrong in game, convert again with --flat\n", scale, shear);
}

std::string manifest_json(const Package &p, const Options &o, const std::string &hash, const std::string &dae_file)
{
    const Rig &rig = p.rig;
    size_t verts = 0, tris = 0, animated = 0, clamped = 0;
    for (auto &m : rig.meshes) { verts += m.vcount.size(); tris += m.triangles.size() / 3; clamped += m.uv_clamped; }
    for (auto &j : rig.joints) animated += j.animated;
    const Split split = o.bind_only ? Split::bind : o.split;
    std::string s = "{\n";
    s += "  \"about\": \"Written by sonicnative_convert from the player's Sonic Frontiers install. Z up, +Y forward, meters. "
         "Torque plays a cyclic clip of n keys over seqSeconds = (n-1)/fps; Frontiers loops last loopSeconds = n/fps.\",\n";
    s += "  \"converter\": " + json_escape(kVersion) + ",\n  \"hash\": " + json_escape(hash) + ",\n";
    s += "  \"shape\": " + json_escape(vfs(p, dae_file)) + ",\n";
    s += "  \"materials\": " + json_escape(vfs(p, p.materials_file)) + ",\n";
    s += "  \"datablocks\": " + json_escape(vfs(p, p.datablocks_file)) + ",\n";
    s += "  \"datablock\": " + json_escape(p.datablock) + ",\n";
    if (split == Split::cs) s += "  \"constructor\": " + json_escape(vfs(p, p.stem + "_" + hash + ".cs")) + ",\n";
    s += "  \"modelHeight\": " + json_num(p.model_height) + ",\n";
    s += "  \"scale\": " + json_num(o.fixture ? 1.0 : sonic_data::kHeightMeters / p.model_height) + ",\n";
    s += "  \"split\": " + json_escape(split_name(split)) + ", \"hierarchy\": " + (rig.flat ? "\"flat\"" : "\"tree\"") + ",\n";
    s += "  \"fps\": " + json_num(rig.fps) + ",\n";
    s += "  \"joints\": " + std::to_string(rig.joints.size()) + ", \"animatedJoints\": " + std::to_string(animated) +
         ", \"meshes\": " + std::to_string(rig.meshes.size()) + ", \"vertices\": " + std::to_string(verts) + ", \"triangles\": " + std::to_string(tris) + ",\n";
    s += std::string("  \"srgbAlbedo\": ") + (o.srgb ? "true" : "false") + ", \"flipGreen\": " + (o.flip_green ? "true" : "false") +
         ", \"uvClamped\": " + std::to_string(clamped) + ",\n";
    s += "  \"clips\": [";
    if (split != Split::bind) {
        for (size_t i = 0; i < rig.clips.size(); ++i) {
            const RigClip &c = rig.clips[i];
            s += i ? ",\n    " : "\n    ";
            s += "{\"name\": " + json_escape(c.name) + ", \"frames\": " + std::to_string(c.frames) + ", \"fps\": " + json_num(rig.fps) +
                 ", \"loop\": " + (c.loop ? "true" : "false") + ", \"refKmh\": " + json_num(c.ref_kmh) + ", \"minRate\": " + json_num(c.min_rate) +
                 ", \"maxRate\": " + json_num(c.max_rate) + ", \"first\": " + std::to_string(c.first) +
                 ", \"seqSeconds\": " + json_num((c.frames - 1) / rig.fps) + ", \"loopSeconds\": " + json_num((c.loop ? c.frames : c.frames - 1) / rig.fps) + "}";
        }
        if (!rig.clips.empty()) s += "\n  ";
    }
    s += "]\n}\n";
    return s;
}

// Removes <prefix>*<ext> files we wrote earlier (and their caches) from one folder.
size_t remove_stale(const fs::path &dir, const std::wstring &prefix, std::initializer_list<const wchar_t *> exts)
{
    size_t count = 0;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return 0;
    for (auto &e : fs::directory_iterator(dir, ec)) {
        const std::wstring name = e.path().filename().wstring();
        if (!e.is_regular_file(ec) || name.compare(0, prefix.size(), prefix) != 0) continue;
        std::wstring ext = e.path().extension().wstring();
        for (auto &c : ext) c = wchar_t(towlower(c));
        for (const wchar_t *x : exts) if (ext == x && fs::remove(e.path(), ec)) { ++count; break; }
    }
    return count;
}

bool write_package(const fs::path &user, const Package &p, const Options &o, const std::string &hash)
{
    const fs::path data = user / L"current" / L"mods" / widen(kDataFolder);
    const fs::path dir = data / widen(p.folder);
    const fs::path cache = user / L"current" / L"temp" / L"mods" / widen(kDataFolder);
    const std::string dae_file = p.stem + "_" + hash + ".dae";
    const fs::path manifest = o.fixture ? dir / L"manifest.json" : data / L"manifest.json";
    std::error_code ec;
    // The manifest goes first and comes back last: a run that fails in between leaves no manifest
    // (the mod then asks for a conversion), never one naming files the cleanup removed.
    if (!fs::remove(manifest, ec) && ec) { std::printf("error: cannot remove %s: %s\n", narrow(manifest.wstring()).c_str(), ec.message().c_str()); return false; }
    // Stale shapes and caches next: BeamNG may prefer an old .cdae to a new .dae.
    const size_t stale = remove_stale(dir, widen(p.stem + "_"), {L".dae", L".cdae", L".cs"}) +
                         remove_stale(dir / L"tex", L"sonicnative_", {L".dds"});
    const bool had_cache = fs::exists(cache, ec);
    fs::remove_all(cache, ec);
    if (ec) { std::printf("error: cannot remove %s: %s\n", narrow(cache.wstring()).c_str(), ec.message().c_str()); return false; }
    std::printf("cleanup: %zu stale file(s) removed from %s; cache %s %s\n", stale, narrow(dir.wstring()).c_str(),
                narrow(cache.wstring()).c_str(), had_cache ? "removed" : "absent");
    fs::create_directories(p.textures.empty() ? dir : dir / L"tex", ec);
    if (ec) { std::printf("error: cannot create %s: %s\n", narrow(dir.wstring()).c_str(), ec.message().c_str()); return false; }

    DaeInfo info;
    info.tool = std::string("sonicnative_convert ") + kVersion;
    info.split = o.bind_only ? Split::bind : o.split;
    if (info.split != Split::bind && p.rig.total_frames > 1) {
        float fps = 0;
        std::string keys;
        if (info.split == Split::cs) keys = "ambient " + std::to_string(torque_keys(p.rig, 0, p.rig.total_frames - 1, fps)) + "/" + std::to_string(p.rig.total_frames);
        else for (auto &c : p.rig.clips)
            keys += (keys.empty() ? "" : ", ") + c.name + " " + std::to_string(torque_keys(p.rig, c.first, c.first + c.frames - 1, fps)) + "/" + std::to_string(c.frames);
        std::printf("torque resampling (predicted, keys/frames): %.3g fps; %s\n", fps, keys.c_str());
    }
    std::vector<std::pair<fs::path, std::string>> texts;
    texts.push_back({dir / widen(dae_file), write_dae(p.rig, info)});
    if (info.split == Split::cs) texts.push_back({dir / widen(p.stem + "_" + hash + ".cs"), write_constructor(p.rig, p.ctor, dae_file)});
    texts.push_back({dir / widen(p.materials_file), p.materials_json});
    texts.push_back({dir / widen(p.datablocks_file), "{\n  " + json_escape(p.datablock) + ": {\"name\": " + json_escape(p.datablock) +
                     ", \"class\": \"StaticShapeData\", \"shapeFile\": " + json_escape(vfs(p, dae_file)) + "}\n}\n"});
    for (auto &t : texts) {
        if (!write_file(t.first.wstring(), t.second)) { std::printf("error: cannot write %s\n", narrow(t.first.wstring()).c_str()); return false; }
        std::printf("wrote %s (%zu bytes)\n", narrow(t.first.wstring()).c_str(), t.second.size());
    }
    for (auto &t : p.textures) {
        const fs::path path = (dir / widen(t.file)).make_preferred();
        if (!write_file(path.wstring(), t.bytes.data(), t.bytes.size())) { std::printf("error: cannot write %s\n", narrow(path.wstring()).c_str()); return false; }
        std::printf("wrote %s (%zu bytes)\n", narrow(path.wstring()).c_str(), t.bytes.size());
    }
    // Manifest last: it only ever names files this run wrote.
    const std::string m = manifest_json(p, o, hash, dae_file);
    if (!write_file(manifest.wstring(), m)) { std::printf("error: cannot write %s\n", narrow(manifest.wstring()).c_str()); return false; }
    std::printf("wrote %s (%zu bytes)\n", narrow(manifest.wstring()).c_str(), m.size());
    return true;
}

// Sonic from the player's install.
bool prepare_sonic(const Options &o, Package &p, std::string &hash_input)
{
    const std::wstring archive = o.frontiers + L"\\" + sonic_data::kArchive;
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!GetFileAttributesExW(archive.c_str(), GetFileExInfoStandard, &attr)) { std::printf("error: %s not found\n", narrow(archive).c_str()); return false; }
    const uint64_t size = uint64_t(attr.nFileSizeHigh) << 32 | attr.nFileSizeLow;
    const uint64_t mtime = uint64_t(attr.ftLastWriteTime.dwHighDateTime) << 32 | attr.ftLastWriteTime.dwLowDateTime;
    hash_input += "|sonic|" + std::to_string(size) + "|" + std::to_string(mtime);
    std::printf("reading %s (%llu bytes)\n", narrow(archive).c_str(), (unsigned long long)size);

    std::string error;
    frontiers::Pac pac;
    frontiers::Model model;
    frontiers::Skeleton skeleton;
    if (!pac.load(archive, error)) { std::printf("error: %s\n", error.c_str()); return false; }
    if (!frontiers::parse_model(pac, sonic_data::kModelEntry, model, error)) { std::printf("error: model: %s\n", error.c_str()); return false; }
    const frontiers::PacEntry *skl = pac.find(sonic_data::kSkeletonEntry);
    if (!skl || !frontiers::parse_skeleton(*skl, skeleton, error)) { std::printf("error: skeleton: %s\n", skl ? error.c_str() : "missing"); return false; }
    p.model_height = std::max(0.1f, model.max[1] - model.min[1]);
    std::printf("model: %zu nodes, %zu meshes, %zu vertices, %zu triangles, height %.4f m (y %.4f..%.4f); skeleton %zu bones\n",
                model.nodes.size(), model.meshes.size(), model.vertices.size(), model.indices.size() / 3, p.model_height,
                model.min[1], model.max[1], skeleton.bones.size());

    // Materials: albedo + BC5 normal per group, named after the Frontiers mesh materials.
    std::vector<MaterialGroup> groups;
    std::vector<int> group_of(model.meshes.size());
    std::vector<MeshNaming> naming(model.meshes.size());
    std::vector<std::string> objects;
    const std::string prefix = sonic_data::kMaterialPrefix;
    for (size_t m = 0; m < model.meshes.size(); ++m) {
        const std::string &mat = model.meshes[m].material;
        std::string short_name = mat.compare(0, prefix.size(), prefix) == 0 ? mat.substr(prefix.size()) : mat;
        short_name = sanitize_sid(short_name);
        const frontiers::PacEntry *entry = pac.find(mat + ".material");
        MaterialTextures mt;
        if (!entry || !read_material(*entry, mt)) { std::printf("error: material %s unreadable\n", mat.c_str()); return false; }
        std::printf("  material %-18s", mat.c_str());
        for (auto &s : mt.slots) {
            std::printf(" %s:%s", s.type.c_str(), s.name.c_str());
            if (s.wrap_u || s.wrap_v) std::printf("[wrap %u/%u]", s.wrap_u, s.wrap_v);
        }
        std::printf("  alphaThreshold %u%s\n", mt.alpha_threshold, mt.double_sided ? "  double-sided" : "");
        const std::string albedo = mt.first("diffuse"), normal = mt.first("normal");
        if (albedo.empty()) { std::printf("error: material %s has no diffuse texture\n", mat.c_str()); return false; }
        // The diffuse slot's addressing decides the UVs (build_bind clamps them for a clamped texture).
        for (auto &s : mt.slots)
            if (s.type == "diffuse") { naming[m].clamp_u = o.uv_clamp && s.wrap_u == kWrapClamp; naming[m].clamp_v = o.uv_clamp && s.wrap_v == kWrapClamp; break; }
        int g = -1;
        for (size_t k = 0; k < groups.size(); ++k)
            if (groups[k].albedo == albedo && groups[k].normal == normal && groups[k].double_sided == mt.double_sided) g = int(k);
        if (g < 0) { g = int(groups.size()); groups.push_back({}); groups.back().albedo = albedo; groups.back().normal = normal; groups.back().double_sided = mt.double_sided; }
        groups[size_t(g)].meshes.push_back(short_name);
        group_of[m] = g;
        std::string object = short_name + "_a";
        for (int k = 2; std::find(objects.begin(), objects.end(), object) != objects.end(); ++k) object = short_name + std::to_string(k) + "_a";
        objects.push_back(object);
        naming[m].object = object;
    }
    std::vector<std::string> used;
    for (auto &g : groups) {
        std::string name = "sonicnative_" + common_prefix(g.meshes);
        for (int k = 2; std::find(used.begin(), used.end(), name) != used.end(); ++k) name = "sonicnative_" + common_prefix(g.meshes) + std::to_string(k);
        used.push_back(name);
        g.name = name;
    }
    for (size_t m = 0; m < model.meshes.size(); ++m) naming[m].material = groups[size_t(group_of[m])].name;

    p.materials_json = "{\n";
    for (size_t k = 0; k < groups.size(); ++k) {
        MaterialGroup &g = groups[k];
        const frontiers::PacEntry *albedo = pac.find(g.albedo + ".dds");
        if (!albedo) { std::printf("error: texture %s.dds missing\n", g.albedo.c_str()); return false; }
        TextureOut b;
        b.file = "tex/" + g.name + "_b.dds";
        uint32_t from = 0, to = 0;
        if (o.srgb && dds_srgb(albedo->data, albedo->size, b.bytes, from, to, error)) {
            std::printf("  texture %-14s -> %-28s %s -> %s (header only)\n", g.albedo.c_str(), b.file.c_str(), dxgi_name(from), dxgi_name(to));
        } else {
            if (o.srgb) std::printf("  warning: %s kept as is (%s)\n", g.albedo.c_str(), error.c_str());
            b.bytes.assign(albedo->data, albedo->data + albedo->size);
            std::printf("  texture %-14s -> %-28s copied as is\n", g.albedo.c_str(), b.file.c_str());
        }
        g.albedo_file = b.file;
        p.textures.push_back(std::move(b));
        std::string stage = "\"baseColorMap\": " + json_escape(vfs(p, g.albedo_file));
        if (!g.normal.empty()) {
            const frontiers::PacEntry *normal = pac.find(g.normal + ".dds");
            frontiers::Texture info;
            if (normal && frontiers::parse_dds(*normal, info, error) && info.dxgi == 83) {
                TextureOut nm;
                nm.file = "tex/" + g.name + "_nm.dds";
                nm.bytes.assign(normal->data, normal->data + normal->size);
                if (o.flip_green && !dds_flip_green(nm.bytes, error)) { std::printf("error: %s: %s\n", g.normal.c_str(), error.c_str()); return false; }
                std::printf("  texture %-14s -> %-28s BC5_UNORM %ux%u, %u mips%s\n", g.normal.c_str(), nm.file.c_str(), info.width, info.height, info.mips,
                            o.flip_green ? ", green flipped" : ", as is");
                g.normal_file = nm.file;
                p.textures.push_back(std::move(nm));
                stage += ", \"normalMap\": " + json_escape(vfs(p, g.normal_file));
            } else {
                std::printf("  note: normal %s is %s, not BC5: not used\n", g.normal.c_str(), normal ? dxgi_name(info.dxgi) : "missing");
            }
        }
        stage += ", \"roughnessFactor\": " + json_num(sonic_data::kRoughness) + ", \"metallicFactor\": " + json_num(sonic_data::kMetallic);
        p.materials_json += material_json(g.name, stage, g.double_sided) + (k + 1 < groups.size() ? ",\n" : "\n");
    }
    p.materials_json += "}\n";

    if (!build_bind(model, naming, o.flat, p.rig, error)) { std::printf("error: %s\n", error.c_str()); return false; }
    if (o.bind_only) return true;
    std::vector<ClipSource> clips;
    for (const std::string &id : clip_ids(o.clips)) {
        const sonic_data::ClipRow *row = nullptr;
        for (auto &r : sonic_data::kClips) if (id == r.id) row = &r;
        if (!row) { std::printf("error: unknown clip '%s'\n", id.c_str()); return false; }
        for (auto &c : clips) if (c.name == id) { std::printf("error: clip '%s' listed twice\n", id.c_str()); return false; }
        ClipSource c;
        c.name = row->id; c.loop = row->loop; c.ref_kmh = row->ref_kmh; c.min_rate = row->min_rate; c.max_rate = row->max_rate;
        const frontiers::PacEntry *entry = pac.find(row->clip);
        if (!entry || !frontiers::bake_clip(*entry, skeleton, model, c.clip, error)) { std::printf("error: clip %s: %s\n", row->clip, entry ? error.c_str() : "missing"); return false; }
        std::printf("  clip %-10s %-34s %3u frames @ %.0f fps (%.3f s)\n", c.name.c_str(), row->clip, c.clip.frames, c.clip.rate, c.clip.duration);
        clips.push_back(std::move(c));
    }
    if (clips.empty()) { std::printf("error: no clips (use --bind-only for a bind pose)\n"); return false; }
    if (!build_animation(model, clips, p.rig, error)) { std::printf("error: %s\n", error.c_str()); return false; }
    return true;
}

// The synthetic rig (no Frontiers data).
bool prepare_fixture(const Options &o, Package &p)
{
    frontiers::Model model;
    std::vector<ClipSource> clips;
    build_fixture(model, clips);
    p.model_height = model.max[1] - model.min[1];
    std::vector<MeshNaming> naming = {{"column_a", "sonicnative_fixture"}};
    // Blue, so it never passes for the orange no-material look.
    p.materials_json = "{\n" + material_json("sonicnative_fixture", "\"baseColorFactor\": [0.1, 0.35, 1, 1], \"roughnessFactor\": 0.5, \"metallicFactor\": 0", false) + "\n}\n";
    std::string error;
    if (!build_bind(model, naming, o.flat, p.rig, error) || (!o.bind_only && !build_animation(model, clips, p.rig, error))) {
        std::printf("error: %s\n", error.c_str());
        return false;
    }
    return true;
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        const bool more = i + 1 < argc;
        if (a == L"--userpath" && more) o.userpath = trim_folder(argv[++i]);
        else if (a == L"--frontiers" && more) o.frontiers = trim_folder(argv[++i]);
        else if (a == L"--clips" && more) o.clips = narrow(argv[++i]);
        else if (a == L"--split" && more) {
            const std::wstring s = argv[++i];
            if (s == L"clips") o.split = Split::clips;
            else if (s == L"cs") o.split = Split::cs;
            else { std::printf("error: --split takes clips or cs\n"); return 2; }
        }
        else if (a == L"--fixture") o.fixture = true;
        else if (a == L"--bind-only") o.bind_only = true;
        else if (a == L"--flip-green") o.flip_green = true;
        else if (a == L"--flat") o.flat = true;
        else if (a == L"--no-srgb") o.srgb = false;
        else if (a == L"--no-uv-clamp") o.uv_clamp = false;
        else if (a == L"--help" || a == L"-h" || a == L"/?") { usage(); return 0; }
        else { std::printf("error: unknown argument '%s'\n", narrow(a).c_str()); usage(); return 2; }
    }
    std::printf("sonicnative_convert %s\n", kVersion);
    if (o.userpath.empty()) { usage(); return 2; }
    {   // one spelling of the clip list for the lookup and the hash
        std::string list;
        for (auto &id : clip_ids(o.clips)) list += (list.empty() ? "" : ",") + id;
        o.clips = list;
    }
    if (beamng_running()) {
        std::printf("BeamNG.drive.x64.exe is running: close the game before converting (it keeps the shape and its cache open).\n");
        return 3;
    }
    const fs::path user(o.userpath);
    std::error_code ec;
    if (!fs::is_directory(user / L"current", ec)) {
        std::printf("error: %s has no 'current' folder: not a BeamNG user folder (run the game once with it)\n", narrow(o.userpath).c_str());
        return 2;
    }

    Package p;
    std::string hash_input = std::string("sonicnative_convert ") + kVersion;
    if (o.fixture) {
        p.folder = "fixture"; p.stem = "fixture";
        p.materials_file = "sonicnative_fixture.materials.json"; p.datablocks_file = "sonicnative_fixture.datablocks.json";
        p.datablock = "sonicnative_fixture_db";
        hash_input += "|fixture";
        if (!prepare_fixture(o, p)) return 1;
    } else {
        if (o.frontiers.empty()) o.frontiers = settings_frontiers();
        if (o.frontiers.empty()) { std::printf("error: no Sonic Frontiers folder: pass --frontiers or set frontiers= in %%LOCALAPPDATA%%\\SonicBeam\\settings.ini\n"); return 2; }
        p.folder = "sonic"; p.stem = "sonic";
        p.materials_file = "sonicnative.materials.json"; p.datablocks_file = "sonicnative.datablocks.json";
        p.datablock = "sonicnative_db";
        if (!prepare_sonic(o, p, hash_input)) return 1;
    }
    hash_input += std::string("|clips=") + (o.fixture ? "idle,lean" : o.clips) + "|split=" + split_name(o.bind_only ? Split::bind : o.split) +
                  "|flip=" + (o.flip_green ? "1" : "0") + "|srgb=" + (o.srgb ? "1" : "0") + "|flat=" + (o.flat ? "1" : "0") +
                  "|uvclamp=" + (o.uv_clamp ? "1" : "0");
    const std::string hash = hex8(fnv1a(hash_input));
    p.ctor = "sonicnative_" + p.stem + "_" + hash + "Dae";
    print_rig(p.rig);
    if (!write_package(user, p, o, hash)) return 1;
    std::printf("done: %s, split %s, hash %s\n", p.folder.c_str(), split_name(o.bind_only ? Split::bind : o.split), hash.c_str());
    return 0;
}
