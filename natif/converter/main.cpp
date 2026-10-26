// SPDX-License-Identifier: MIT
// sonicnative_convert: turns Sonic from the player's own Sonic Frontiers install into a skinned,
// animated COLLADA shape (every clip the overlay mode plays, with his boost aura shell), a still shape of his
// spin ball shell, BeamNG materials, DDS textures, the boost effects' materials and bone tracks, and WAV sounds
// inside the BeamNG user folder (<userpath>\current\mods\sonicnative_data).
// Nothing it writes belongs in the repository or a package.
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
#include "sounds.hpp"
#include "fx.hpp"
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

// A shape text above this makes BeamNG's first load (COLLADA parse, then the .cdae cache) slow.
constexpr double kShapeWarnBytes = 40.0 * 1024 * 1024;

struct Options {
    std::wstring userpath, frontiers, out;
    std::string clips = sonic_data::kDefaultClips;
    Split split = Split::clips;
    bool fixture = false, bind_only = false, flip_green = false, srgb = true, flat = false, uv_clamp = true, sounds = true, ball = true;
    bool aura = true, fx = true, locus_fallback = false;
};

void usage()
{
    std::printf("sonicnative_convert %s\n"
                "usage: sonicnative_convert --userpath <BeamNG user folder> [--frontiers <Sonic Frontiers folder>]\n"
                "                           [--clips <ids>] [--split clips|cs] [--bind-only] [--flat] [--flip-green] [--no-srgb]\n"
                "                           [--no-uv-clamp] [--no-sounds] [--no-ball] [--no-aura] [--no-fx] [--locus-fallback]\n"
                "                           [--fixture] [--out <folder>]\n"
                "  --userpath    the folder BeamNG runs with (-userpath); output goes to <it>\\current\\mods\\%s\n"
                "  --frontiers   default: frontiers= line of %%LOCALAPPDATA%%\\SonicBeam\\settings.ini\n"
                "  --clips       comma-separated clip ids, default all of them:\n                %s\n",
                kVersion, kDataFolder, sonic_data::kDefaultClips);
    std::printf("  --split       clips: one <animation_clip> per clip (default); cs: one 'ambient' + a TSShapeConstructor .cs\n"
                "  --bind-only   no animation (diagnostic: bind pose only)\n"
                "  --flat        every joint under the armature with model-space locals (no inherited scale or shear)\n"
                "  --flip-green  invert the green channel of the BC5 normal maps (lossless)\n"
                "  --no-srgb     keep the albedo headers as they are (UNORM)\n"
                "  --no-uv-clamp keep UVs outside [0,1] on clamp-addressed textures (eyes) as they are\n"
                "  --no-sounds   leave Sonic's sounds out (default: every waveform of his sound cues as WAV into %s\\sounds)\n"
                "  --no-ball     leave the spin ball shell out (default: a second, still shape sonic\\ball_<hash>.dae)\n"
                "  --no-aura     leave the boost aura shell out of Sonic's shape (default: meshes aura_a and aurahalo_a)\n"
                "  --no-fx       leave every boost effect out: aura, effect textures and materials, bone tracks (implies --no-aura)\n"
                "  --locus-fallback  computed streaks instead of Frontiers' locus01 (BC6H) for the boost trail\n"
                "  --fixture     synthetic 2-joint test shape (clips idle, lean) into %s\\fixture, no Frontiers data, no sounds\n"
                "  --out         write the package into this folder instead of the user folder, for inspection: no BeamNG\n"
                "                cache is touched, so it also runs while BeamNG does (--userpath is then not needed);\n"
                "                never a mods\\%s folder nor one inside the repository\n",
                kDataFolder, kDataFolder, kDataFolder);
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

// A folder's absolute, resolved spelling, lower-cased (Windows paths ignore case), for comparisons.
std::wstring folder_key(const fs::path &dir)
{
    std::error_code ec;
    const fs::path abs = fs::absolute(dir, ec);
    fs::path c = fs::weakly_canonical(abs, ec);
    if (ec || c.empty()) c = abs.lexically_normal();
    std::wstring s = c.make_preferred().wstring();
    if (s.compare(0, 4, L"\\\\?\\") == 0) s.erase(0, 4);
    for (auto &ch : s) ch = wchar_t(towlower(ch));
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    return s;
}

// The repository the converter was built in (<repo>\natif\build\sonicnative_convert.exe), empty when it runs from elsewhere.
std::wstring repo_key()
{
    wchar_t exe[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, exe, DWORD(sizeof(exe) / sizeof(exe[0])));
    if (!n || n >= sizeof(exe) / sizeof(exe[0])) return {};
    const fs::path repo = fs::path(exe).parent_path().parent_path().parent_path();
    std::error_code ec;
    if (!fs::is_regular_file(repo / L"natif" / L"converter" / L"main.cpp", ec)) return {};
    return folder_key(repo);
}

// Why --out may not write into this folder (empty: it may). The live package is rewritten only through
// --userpath (BeamNG closed, shape cache cleared), and nothing from Frontiers ever lands in the repository.
std::string out_refusal(const std::wstring &out)
{
    const std::wstring key = folder_key(out);
    const std::wstring live = L"\\mods\\" + widen(kDataFolder);
    for (size_t at = key.find(live); at != std::wstring::npos; at = key.find(live, at + 1))
        if (at + live.size() == key.size() || key[at + live.size()] == L'\\')
            return std::string("a BeamNG mods\\") + kDataFolder + " folder: convert into it with --userpath, BeamNG closed";
    const std::wstring repo = repo_key();
    if (!repo.empty() && key.compare(0, repo.size(), repo) == 0 && (key.size() == repo.size() || key[repo.size()] == L'\\' || repo.back() == L'\\'))
        return "inside the repository, where nothing from Sonic Frontiers belongs";
    return {};
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
    std::vector<TextureOut> textures;    // Sonic's and the ball's (tex/sonicnative_*)
    float model_height = 1;
    Rig rig;
    bool with_sounds = false;        // the sounds step ran: sounds/ is rewritten and the manifest lists it
    std::vector<SoundOut> sounds;
    // The spin ball shell: a second, rigid shape in the same folder (ball_<hash>.dae), its own materials
    bool with_ball = false;
    Rig ball;
    std::string ball_materials_json;
    float ball_radius = 0;           // model units, around the shell's middle (its origin)
    // Boost effects (natif plan fx): the aura shell is in rig (meshes + Sonic's materials); the effect quads'
    // materials go to their own file, the bone tracks to fx_bones_<hash>.json
    bool with_aura = false, with_fx = false;
    std::string fx_materials_json, bones_json;
    std::vector<const sonic_data::FxKind *> fx_kinds;   // kinds with a texture and materials, in kFxKinds order
};

const char *const kBallStem = "ball";
const char *const kBallMaterialsFile = "sonicnative_ball.materials.json";
const char *const kBallDatablocksFile = "sonicnative_ball.datablocks.json";
const char *const kBallDatablock = "sonicnative_ball_db";
const char *const kFxMaterialsFile = "sonicnative_fx.materials.json";
const char *const kBonesStem = "fx_bones";
const char *const kAuraTextureFile = "tex/sonicnative_fx_aura.dds";

std::string vfs(const Package &p, const std::string &file) { return std::string("/mods/") + kDataFolder + "/" + p.folder + "/" + file; }
std::string sounds_vfs() { return std::string("/mods/") + kDataFolder + "/sounds/"; }

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
         "Torque plays a cyclic clip of n keys over seqSeconds = (n-1)/fps; Frontiers loops last loopSeconds = n/fps. "
         "A one-shot (loop false) keeps all its keys and ends on its last pose. blendSeconds is the overlay's cross-fade into the clip.\",\n";
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
            std::string steps;
            for (float at : c.steps) steps += (steps.empty() ? "" : ", ") + json_num(at);
            s += "{\"name\": " + json_escape(c.name) + ", \"frames\": " + std::to_string(c.frames) + ", \"fps\": " + json_num(rig.fps) +
                 ", \"loop\": " + (c.loop ? "true" : "false") + ", \"refKmh\": " + json_num(c.ref_kmh) + ", \"minRate\": " + json_num(c.min_rate) +
                 ", \"maxRate\": " + json_num(c.max_rate) + ", \"blendSeconds\": " + json_num(c.blend_seconds) + ", \"first\": " + std::to_string(c.first) +
                 ", \"seqSeconds\": " + json_num((c.frames - 1) / rig.fps) + ", \"loopSeconds\": " + json_num((c.loop ? c.frames : c.frames - 1) / rig.fps) +
                 ", \"steps\": [" + steps + "]}";
        }
        if (!rig.clips.empty()) s += "\n  ";
    }
    s += "]";
    // The spin ball shell: origin on its middle, Sonic's axes and scale; the overlay sets its middle pivotHeight
    // meters above Sonic's feet and turns it about Sonic's right axis once per spinSeconds; Sonic is hidden inside.
    if (p.with_ball) {
        const double scale = sonic_data::kHeightMeters / p.model_height;
        size_t bverts = 0, btris = 0;
        for (auto &m : p.ball.meshes) { bverts += m.positions.size() / 3; btris += m.triangles.size() / 3; }
        s += ",\n  \"ball\": {\"shape\": " + json_escape(vfs(p, std::string(kBallStem) + "_" + hash + ".dae")) +
             ", \"materials\": " + json_escape(vfs(p, kBallMaterialsFile)) + ", \"datablocks\": " + json_escape(vfs(p, kBallDatablocksFile)) +
             ",\n    \"datablock\": " + json_escape(kBallDatablock) + ", \"scale\": " + json_num(scale) +
             ", \"spinSeconds\": " + json_num(1.0 / sonic_data::kBallTurnsPerSecond) +
             ", \"pivotHeight\": " + json_num(sonic_data::kHeightMeters * sonic_data::kBallPivotShare) +
             ", \"radius\": " + json_num(p.ball_radius * scale) +
             ", \"vertices\": " + std::to_string(bverts) + ", \"triangles\": " + std::to_string(btris) + "}";
    }
    // Boost effects: the material of kind K at ladder level k (1-based in alphas) is <material>_<k>; the aura's
    // meshes are in the shape, its base materials in Sonic's materials, its ladder levels are setSkinName(skins % k).
    if (p.with_fx) {
        std::string alphas, sprites, ribbons;
        for (int k = 1; k <= sonic_data::kFxLevels; ++k) alphas += (k > 1 ? ", " : "") + json_num(sonic_data::fx_alpha(k));
        for (const sonic_data::FxKind *kind : p.fx_kinds) {
            std::string &list = kind->ribbon ? ribbons : sprites;
            list += (list.empty() ? "" : ", ") + json_escape(kind->kind) + ": {\"material\": " + json_escape(std::string("sonicnative_fx_") + kind->kind);
            if (kind->grid_u) list += ", \"grid\": [" + std::to_string(kind->grid_u) + ", " + std::to_string(kind->grid_v) + "]";
            list += "}";
        }
        s += ",\n  \"fx\": {\"version\": 1, \"materials\": " + json_escape(vfs(p, kFxMaterialsFile)) + ",\n    \"alphas\": [" + alphas + "],\n" +
             "    \"sprites\": {" + sprites + "},\n    \"ribbons\": {" + ribbons + "}";
        if (p.with_aura) {
            std::string meshes, materials, alpha;
            for (auto &L : sonic_data::kAuraLayers) {
                meshes += (meshes.empty() ? "" : ", ") + json_escape(L.object);
                materials += (materials.empty() ? "" : ", ") + json_escape(L.material);
                alpha += (alpha.empty() ? "" : ", ") + json_num(L.alpha);
            }
            s += ",\n    \"aura\": {\"meshes\": [" + meshes + "], \"materials\": [" + materials + "], \"skins\": \"l%02d\", \"alpha\": [" + alpha +
                 "], \"colour\": " + json_vec(sonic_data::kAuraColour, 3) + "}";
        }
        if (!p.bones_json.empty()) s += ",\n    \"bones\": {\"file\": " + json_escape(vfs(p, std::string(kBonesStem) + "_" + hash + ".json")) + "}";
        s += "}";
    }
    // Sounds: random = one file per play, layers = all files together; a loop loops each whole file.
    if (p.with_sounds) s += ",\n  \"sounds\": " + sounds_json(p.sounds, sounds_vfs());
    s += "\n}\n";
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

// data: <userpath>\current\mods\sonicnative_data, or the --out folder; cache: BeamNG's shape cache for it
// (<userpath>\current\temp\mods\sonicnative_data), empty with --out.
bool write_package(const fs::path &data, const fs::path &cache, const Package &p, const Options &o, const std::string &hash)
{
    const fs::path dir = data / widen(p.folder);
    const std::string dae_file = p.stem + "_" + hash + ".dae";
    const std::string ball_file = std::string(kBallStem) + "_" + hash + ".dae";
    const fs::path manifest = o.fixture ? dir / L"manifest.json" : data / L"manifest.json";
    std::error_code ec;
    // The manifest goes first and comes back last: a run that fails in between leaves no manifest
    // (the mod then asks for a conversion), never one naming files the cleanup removed.
    if (!fs::remove(manifest, ec) && ec) { std::printf("error: cannot remove %s: %s\n", narrow(manifest.wstring()).c_str(), ec.message().c_str()); return false; }
    // Stale shapes and caches next: BeamNG may prefer an old .cdae to a new .dae. An old ball goes even when
    // this run has none, so no shape outlives the manifest that named it.
    const fs::path sounds = data / L"sounds";
    const size_t stale = remove_stale(dir, widen(p.stem + "_"), {L".dae", L".cdae", L".cs"}) +
                         remove_stale(dir, widen(std::string(kBallStem) + "_"), {L".dae", L".cdae"}) +
                         remove_stale(dir, widen(std::string(kBonesStem) + "_"), {L".json"}) +
                         remove_stale(dir / L"tex", L"sonicnative_", {L".dds"});
    std::vector<const char *> unused;
    if (!p.with_ball) { unused.push_back(kBallMaterialsFile); unused.push_back(kBallDatablocksFile); }
    if (!p.with_fx) unused.push_back(kFxMaterialsFile);
    for (const char *f : unused) {
        const fs::path old = dir / widen(f);
        std::error_code rec;
        if (!fs::remove(old, rec) && rec) { std::printf("error: cannot remove %s: %s\n", narrow(old.wstring()).c_str(), rec.message().c_str()); return false; }
    }
    const size_t stale_sounds = p.with_sounds ? remove_stale(sounds, L"", {L".wav"}) : 0;   // the sounds folder is ours
    const bool had_cache = !cache.empty() && fs::exists(cache, ec);
    if (!cache.empty()) {
        fs::remove_all(cache, ec);
        if (ec) { std::printf("error: cannot remove %s: %s\n", narrow(cache.wstring()).c_str(), ec.message().c_str()); return false; }
    }
    std::printf("cleanup: %zu stale file(s) removed from %s", stale, narrow(dir.wstring()).c_str());
    if (p.with_sounds) std::printf(", %zu from %s", stale_sounds, narrow(sounds.wstring()).c_str());
    if (cache.empty()) std::printf("; no BeamNG cache (--out)\n");
    else std::printf("; cache %s %s\n", narrow(cache.wstring()).c_str(), had_cache ? "removed" : "absent");
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
    {   // BeamNG parses the whole COLLADA text on the first load (then keeps a .cdae): its size is the load-time risk
        const double bytes = double(texts.back().second.size());
        size_t animated = 0;
        for (auto &j : p.rig.joints) animated += j.animated;
        std::printf("shape size: %.1f MB for %u frames x %zu animated joints (%zu clips)%s\n", bytes / (1024.0 * 1024.0), p.rig.total_frames, animated,
                    p.rig.clips.size(), bytes > kShapeWarnBytes ? "; warning: above 40 MB, BeamNG's first load of it will be slow" : "");
    }
    if (info.split == Split::cs) texts.push_back({dir / widen(p.stem + "_" + hash + ".cs"), write_constructor(p.rig, p.ctor, dae_file)});
    texts.push_back({dir / widen(p.materials_file), p.materials_json});
    texts.push_back({dir / widen(p.datablocks_file), "{\n  " + json_escape(p.datablock) + ": {\"name\": " + json_escape(p.datablock) +
                     ", \"class\": \"StaticShapeData\", \"shapeFile\": " + json_escape(vfs(p, dae_file)) + "}\n}\n"});
    if (p.with_ball) {
        DaeInfo still;
        still.tool = info.tool;
        still.split = Split::bind;   // no joints, no animation: the overlay's shell pose is baked in
        texts.push_back({dir / widen(ball_file), write_dae(p.ball, still)});
        texts.push_back({dir / widen(kBallMaterialsFile), p.ball_materials_json});
        texts.push_back({dir / widen(kBallDatablocksFile), "{\n  " + json_escape(kBallDatablock) + ": {\"name\": " + json_escape(kBallDatablock) +
                         ", \"class\": \"StaticShapeData\", \"shapeFile\": " + json_escape(vfs(p, ball_file)) + "}\n}\n"});
    }
    if (p.with_fx) texts.push_back({dir / widen(kFxMaterialsFile), p.fx_materials_json});
    if (p.with_fx && !p.bones_json.empty()) texts.push_back({dir / widen(std::string(kBonesStem) + "_" + hash + ".json"), p.bones_json});
    for (auto &t : texts) {
        if (!write_file(t.first.wstring(), t.second)) { std::printf("error: cannot write %s\n", narrow(t.first.wstring()).c_str()); return false; }
        std::printf("wrote %s (%zu bytes)\n", narrow(t.first.wstring()).c_str(), t.second.size());
    }
    for (auto &t : p.textures) {
        const fs::path path = (dir / widen(t.file)).make_preferred();
        if (!write_file(path.wstring(), t.bytes.data(), t.bytes.size())) { std::printf("error: cannot write %s\n", narrow(path.wstring()).c_str()); return false; }
        std::printf("wrote %s (%zu bytes)\n", narrow(path.wstring()).c_str(), t.bytes.size());
    }
    if (p.with_sounds) {
        fs::create_directories(sounds, ec);
        if (ec) { std::printf("error: cannot create %s: %s\n", narrow(sounds.wstring()).c_str(), ec.message().c_str()); return false; }
        size_t files = 0, bytes = 0;
        for (auto &s : p.sounds)
            for (auto &f : s.files) {
                const fs::path path = sounds / widen(f.file);
                if (!write_file(path.wstring(), f.bytes.data(), f.bytes.size())) { std::printf("error: cannot write %s\n", narrow(path.wstring()).c_str()); return false; }
                ++files; bytes += f.bytes.size();
            }
        std::printf("wrote %zu sound files into %s (%zu bytes)\n", files, narrow(sounds.wstring()).c_str(), bytes);
    }
    // Manifest last: it only ever names files this run wrote.
    const std::string m = manifest_json(p, o, hash, dae_file);
    if (!write_file(manifest.wstring(), m)) { std::printf("error: cannot write %s\n", narrow(manifest.wstring()).c_str()); return false; }
    std::printf("wrote %s (%zu bytes)\n", narrow(manifest.wstring()).c_str(), m.size());
    return true;
}

// Footsteps of a looping clip, as the overlay finds them (beamng/src/assets.cpp footsteps): a toe's model-space
// height comes down through the bottom fifth of its range. Phase f / (frames - 1) is key f of the kept cycle,
// the same position in Torque's cyclic sequence (n keys over 0..1) and in Frontiers' loop.
std::vector<float> step_phases(const frontiers::Model &model, const frontiers::Clip &clip)
{
    std::vector<float> out;
    if (clip.frames < 4 || clip.nodes != model.nodes.size()) return out;
    const uint32_t loop = clip.frames - 1;
    for (const char *name : sonic_data::kToes) {
        int toe = -1;
        for (size_t n = 0; n < model.nodes.size(); ++n) if (model.nodes[n].name == name) toe = int(n);
        float bind[16];
        if (toe < 0 || !frontiers::math4::inverse_affine(model.nodes[size_t(toe)].inverse_bind, bind)) { std::printf("    warning: no joint %s for footsteps\n", name); continue; }
        const float bx = bind[3], by = bind[7], bz = bind[11];
        std::vector<float> y(clip.frames);
        for (uint32_t f = 0; f < clip.frames; ++f) {
            const float *s = clip.frame(f) + size_t(toe) * 12;
            y[f] = s[4] * bx + s[5] * by + s[6] * bz + s[7];
        }
        const float lo = *std::min_element(y.begin(), y.end()), hi = *std::max_element(y.begin(), y.end());
        if (hi - lo < 1e-3f) continue;
        const float threshold = lo + sonic_data::kStepThreshold * (hi - lo);
        for (uint32_t f = 0; f < loop; ++f)
            if (y[f] <= threshold && y[(f + loop - 1) % loop] > threshold) out.push_back(float(f) / loop);
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Game materials of one Frontiers model: albedo (sRGB header) + BC5 normal per group of meshes sharing them,
// named <name_prefix><their common short name>, the short name being the Frontiers material minus prefix.
// Fills naming (DAE object and material per mesh), json (the .materials.json) and p.textures.
bool convert_materials(const frontiers::Pac &pac, const frontiers::Model &model, const Options &o, Package &p, const std::string &prefix,
                       const std::string &name_prefix, std::vector<MeshNaming> &naming, std::string &json)
{
    std::string error;
    std::vector<MaterialGroup> groups;
    std::vector<int> group_of(model.meshes.size());
    naming.assign(model.meshes.size(), MeshNaming());
    std::vector<std::string> objects;
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
        std::string name = name_prefix + common_prefix(g.meshes);
        for (int k = 2; std::find(used.begin(), used.end(), name) != used.end(); ++k) name = name_prefix + common_prefix(g.meshes) + std::to_string(k);
        used.push_back(name);
        g.name = name;
    }
    for (size_t m = 0; m < model.meshes.size(); ++m) naming[m].material = groups[size_t(group_of[m])].name;

    json = "{\n";
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
        json += material_json(g.name, stage, g.double_sided) + (k + 1 < groups.size() ? ",\n" : "\n");
    }
    json += "}\n";
    return true;
}

// Frontiers' spin ball shell, optional like the overlay's (beamng/src/assets.cpp, jump ball): a missing or
// unreadable shell only leaves it out. Its clip's still pose is baked into the vertices, which are centred on
// the middle of the bind box (the overlay's pivot, beamng/src/game.cpp), so the shape's origin is that middle.
bool prepare_ball(const frontiers::Pac &pac, const Options &o, Package &p)
{
    std::string error;
    frontiers::Model model;
    frontiers::Skeleton skeleton;
    frontiers::Clip clip;
    const frontiers::PacEntry *skl = pac.find(sonic_data::kBallSkeletonEntry), *anim = pac.find(sonic_data::kBallClipEntry);
    if (!frontiers::parse_model(pac, sonic_data::kBallModelEntry, model, error) ||
        !skl || !frontiers::parse_skeleton(*skl, skeleton, error) ||
        !anim || !frontiers::bake_clip(*anim, skeleton, model, clip, error)) {
        std::printf("  warning: spin ball left out (%s)\n", (!skl || !anim) && error.empty() ? "skeleton or clip missing" : error.c_str());
        return false;
    }
    float still = 0;   // the overlay samples the clip over time: say how far its keys stray from the first
    for (uint32_t f = 1; f < clip.frames; ++f) still = std::max(still, max_diff(clip.frame(f), clip.frame(0), int(clip.nodes * 12)));
    const float centre[3] = {(model.min[0] + model.max[0]) * 0.5f, (model.min[1] + model.max[1]) * 0.5f, (model.min[2] + model.max[2]) * 0.5f};
    if (!pose_vertices(model, clip, 0, centre, error)) { std::printf("  warning: spin ball left out (%s)\n", error.c_str()); return false; }
    float radius = 0;
    for (auto &v : model.vertices) radius = std::max(radius, std::sqrt(v.pos[0] * v.pos[0] + v.pos[1] * v.pos[1] + v.pos[2] * v.pos[2]));
    std::printf("spin ball: %s, %zu nodes, %zu meshes, %zu vertices, %zu triangles; pose = key 0 of %s (%u keys, max drift %.3g), "
                "centred on (%.4f, %.4f, %.4f), radius %.4f\n", sonic_data::kBallModelEntry, model.nodes.size(), model.meshes.size(), model.vertices.size(),
                model.indices.size() / 3, sonic_data::kBallClipEntry, clip.frames, still, centre[0], centre[1], centre[2], radius);
    const size_t textures = p.textures.size();
    std::vector<MeshNaming> naming;
    Rig rig;
    if (!convert_materials(pac, model, o, p, sonic_data::kBallMaterialPrefix, "sonicnative_ball_", naming, p.ball_materials_json) ||
        !build_rigid(model, naming, rig, error)) {
        if (!error.empty()) std::printf("  warning: %s\n", error.c_str());
        std::printf("  warning: spin ball left out\n");
        p.textures.resize(textures);
        p.ball_materials_json.clear();
        return false;
    }
    for (auto &m : rig.meshes)
        std::printf("  mesh %-10s -> %-24s %6zu vertices %6zu triangles (rigid); winding %zu agree / %zu opposite / %zu degenerate%s\n",
                    (m.object + "2").c_str(), m.material.c_str(), m.positions.size() / 3, m.triangles.size() / 3, m.agreeing, m.flipped, m.degenerate,
                    m.reversed ? " -> reversed" : "");
    p.ball = std::move(rig);
    p.ball_radius = radius;
    p.with_ball = true;
    return true;
}

// Adds entries to a materials file text ("{\n<entries>\n}\n").
void append_materials(std::string &json, const std::string &entries)
{
    if (entries.empty()) return;
    const size_t close = json.rfind('}');
    if (close == std::string::npos) { json = "{\n" + entries + "\n}\n"; return; }
    json.resize(close);
    while (!json.empty() && (json.back() == '\n' || json.back() == ' ')) json.pop_back();
    json += (json.size() > 1 ? ",\n" : "\n") + entries + "\n}\n";
}

// Frontiers' boost aura shell as two more skinned meshes of Sonic (natif plan fx): it has his joints, so it follows
// every clip with no data of its own; Lua shows it with setMeshHidden. Its materials go into Sonic's own file (they
// bind when the shape loads): base_<name> at the full aura, and lNN_<name> for each ladder level, which
// setSkinName("lNN") picks. Optional like the ball: a missing or different shell, or its texture, only leaves it out.
bool prepare_aura(const frontiers::Pac &pac, const frontiers::Model &model, Package &p)
{
    std::string error;
    frontiers::Model aura;
    if (!frontiers::parse_model(pac, sonic_data::kAuraModelEntry, aura, error)) { std::printf("  warning: boost aura left out (%s)\n", error.c_str()); return false; }
    if (!same_skeleton(aura, model, error)) { std::printf("  warning: boost aura left out (%s: %s)\n", sonic_data::kAuraModelEntry, error.c_str()); return false; }
    const frontiers::PacEntry *tex = pac.find(sonic_data::kAuraTextureEntry);
    frontiers::Texture info;
    if (!tex || !frontiers::parse_dds(*tex, info, error)) {
        std::printf("  warning: boost aura left out (%s: %s)\n", sonic_data::kAuraTextureEntry, tex ? error.c_str() : "missing");
        return false;
    }
    std::vector<RigMesh> meshes;
    for (auto &L : sonic_data::kAuraLayers) {
        RigMesh m;
        if (!build_aura_layer(aura, L, m, error)) { std::printf("  warning: boost aura left out (%s)\n", error.c_str()); return false; }
        meshes.push_back(std::move(m));
    }
    std::printf("boost aura: %s, %zu nodes (Sonic's), %zu mesh(es), %zu vertices, %zu triangles; texture %s %s %ux%u, %u mip(s), copied as is\n",
                sonic_data::kAuraModelEntry, aura.nodes.size(), aura.meshes.size(), aura.vertices.size(), aura.indices.size() / 3,
                sonic_data::kAuraTextureEntry, dxgi_name(info.dxgi), info.width, info.height, info.mips);
    if (aura.meshes.size() > 1) std::printf("  note: the aura has %zu meshes, only the first is used\n", aura.meshes.size());
    TextureOut t;
    t.file = kAuraTextureFile;
    t.bytes.assign(tex->data, tex->data + tex->size);   // an energy mask: UNORM, no sRGB header
    p.textures.push_back(std::move(t));
    const std::string texture = vfs(p, kAuraTextureFile);
    std::string entries;
    for (size_t i = 0; i < meshes.size(); ++i) {
        const sonic_data::AuraLayer &L = sonic_data::kAuraLayers[i];
        std::printf("  aura layer %-10s push %.3f, uv x%.2f, share %.2f -> %s + %d levels %s..%s\n", L.object, L.push, L.uv_scale, L.alpha,
                    L.material, sonic_data::kFxLevels, aura_level_name(L.material, 1).c_str(), aura_level_name(L.material, sonic_data::kFxLevels).c_str());
        entries += (entries.empty() ? "" : ",\n") + aura_material(L.material, L, sonic_data::kAuraOpacity, texture);
        for (int k = 1; k <= sonic_data::kFxLevels; ++k) entries += ",\n" + aura_material(aura_level_name(L.material, k), L, sonic_data::fx_alpha(k), texture);
        p.rig.meshes.push_back(std::move(meshes[i]));
    }
    append_materials(p.materials_json, entries);
    p.with_aura = true;
    return true;
}

// The textures and materials of the effect quads Lua draws (natif plan fx): Frontiers' BC7 / BC6H textures
// copied block for block, the flash glow and shock ring baked from their BC5 masks, the beam's edge computed.
// A texture that cannot be made only leaves its kind out (the manifest then lacks it).
void prepare_fx(const Options &o, const frontiers::Pac &pac, Package &p)
{
    std::string error;
    frontiers::Pac effects;
    const std::wstring archive = o.frontiers + L"\\" + sonic_data::kEffectArchive;
    const bool have_effects = effects.load(archive, error);
    if (have_effects) std::printf("boost effects from %s and %s\n", narrow(archive).c_str(), narrow(sonic_data::kArchive).c_str());
    else std::printf("  warning: %s: %s: the effects drawn with its textures are left out\n", narrow(archive).c_str(), error.c_str());
    std::string json;
    for (const sonic_data::FxKind &kind : sonic_data::kFxKinds) {
        const std::string file = std::string("tex/sonicnative_fx_") + kind.kind + ".dds";
        TextureOut t;
        t.file = file;
        std::string how;
        const frontiers::PacEntry *entry = nullptr;
        frontiers::Texture info;
        error.clear();
        if (kind.entry) {
            if (kind.effect_archive && !have_effects) { std::printf("  warning: %s left out (no %s)\n", kind.kind, narrow(sonic_data::kEffectArchive).c_str()); continue; }
            entry = (kind.effect_archive ? effects : pac).find(kind.entry);
            if (!entry || !frontiers::parse_dds(*entry, info, error)) {
                std::printf("  warning: %s left out (%s: %s)\n", kind.kind, kind.entry, entry ? error.c_str() : "missing");
                continue;
            }
        }
        if (kind.made == sonic_data::FxMade::beam) {
            t.bytes = beam_texture();
            how = "computed (soft edge), B8G8R8A8 256x4";
        } else if (kind.made == sonic_data::FxMade::copy && kind.entry == sonic_data::kLocusEntry && o.locus_fallback) {
            t.bytes = locus_fallback_texture();
            how = std::string("computed streaks (--locus-fallback, not ") + kind.entry + "), B8G8R8A8 256x256";
        } else if (kind.made == sonic_data::FxMade::copy) {
            t.bytes.assign(entry->data, entry->data + entry->size);
            char b[96];
            std::snprintf(b, sizeof(b), "%s %ux%u, %u mip(s), copied as is", info.dxgi == 95 ? "BC6H_UF16" : info.dxgi == 96 ? "BC6H_SF16" : dxgi_name(info.dxgi),
                          info.width, info.height, info.mips);
            how = std::string(kind.entry) + " " + b;
        } else {
            std::vector<float> mask;
            if (!bc5_mask(info, mask, error)) { std::printf("  warning: %s left out (%s: %s)\n", kind.kind, kind.entry, error.c_str()); continue; }
            const bool ring = kind.made == sonic_data::FxMade::ring;
            t.bytes = flash_texture(mask, info.width, info.height, ring);
            how = std::string(kind.entry) + (ring ? " -> shock ring" : " -> flash glow") + " baked from its mask, B8G8R8A8 256x256";
        }
        std::printf("  %-6s %-32s %s\n", kind.kind, file.c_str(), how.c_str());
        json += (json.empty() ? "" : ",\n") + effect_kind_materials(kind, vfs(p, file));
        p.textures.push_back(std::move(t));
        p.fx_kinds.push_back(&kind);
    }
    p.fx_materials_json = "{\n" + json + (json.empty() ? "" : "\n") + "}\n";
    std::printf("  %zu effect kinds x %d levels in %s\n", p.fx_kinds.size(), sonic_data::kFxLevels, kFxMaterialsFile);
    p.with_fx = true;
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

    std::vector<MeshNaming> naming;
    if (!convert_materials(pac, model, o, p, sonic_data::kMaterialPrefix, "sonicnative_", naming, p.materials_json)) return false;
    if (o.ball) prepare_ball(pac, o, p);

    if (!build_bind(model, naming, o.flat, p.rig, error)) { std::printf("error: %s\n", error.c_str()); return false; }
    // Boost effects: the aura shell joins Sonic's meshes (his joints), then the effect textures and materials
    if (o.fx && o.aura) prepare_aura(pac, model, p);
    if (o.fx) prepare_fx(o, pac, p);
    if (o.bind_only) return true;
    std::vector<ClipSource> clips;
    for (const std::string &id : clip_ids(o.clips)) {
        const sonic_data::ClipRow *row = nullptr;
        for (auto &r : sonic_data::kClips) if (id == r.id) row = &r;
        if (!row) { std::printf("error: unknown clip '%s'\n", id.c_str()); return false; }
        for (auto &c : clips) if (c.name == id) { std::printf("error: clip '%s' listed twice\n", id.c_str()); return false; }
        ClipSource c;
        c.name = row->id; c.loop = row->loop; c.ref_kmh = row->ref_kmh; c.min_rate = row->min_rate; c.max_rate = row->max_rate;
        c.blend_seconds = row->blend_seconds;
        const frontiers::PacEntry *entry = pac.find(row->clip);
        if (!entry || !frontiers::bake_clip(*entry, skeleton, model, c.clip, error)) { std::printf("error: clip %s: %s\n", row->clip, entry ? error.c_str() : "missing"); return false; }
        std::printf("  clip %-11s %-34s %3u frames @ %.0f fps (%.3f s)\n", c.name.c_str(), row->clip, c.clip.frames, c.clip.rate, c.clip.duration);
        for (const char *s : sonic_data::kStepClips) {
            if (c.name != s || !c.loop) continue;
            c.steps = step_phases(model, c.clip);
            std::printf("    footsteps at");
            for (float at : c.steps) std::printf(" %.3f", at);
            std::printf("%s of the cycle\n", c.steps.empty() ? " none" : "");
        }
        clips.push_back(std::move(c));
    }
    if (clips.empty()) { std::printf("error: no clips (use --bind-only for a bind pose)\n"); return false; }
    if (!build_animation(model, clips, p.rig, error)) { std::printf("error: %s\n", error.c_str()); return false; }
    if (o.fx) {   // the lightning on Sonic's body follows these joints through every clip
        std::vector<std::string> found, missing;
        p.bones_json = bones_json(model, clips, p.rig, found, missing);
        uint32_t frames = 0;
        for (auto &c : p.rig.clips) frames += c.frames;
        std::printf("fx bones: %zu joints x %u frames over %zu clips (%zu bytes)\n", found.size(), frames, p.rig.clips.size(), p.bones_json.size());
        for (auto &m : missing) std::printf("  warning: no joint %s for the fx bones\n", m.c_str());
        if (found.empty()) p.bones_json.clear();
    }
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
        else if (a == L"--no-sounds") o.sounds = false;
        else if (a == L"--no-ball") o.ball = false;
        else if (a == L"--no-aura") o.aura = false;
        else if (a == L"--no-fx") o.fx = false;
        else if (a == L"--locus-fallback") o.locus_fallback = true;
        else if (a == L"--out" && more) o.out = trim_folder(argv[++i]);
        else if (a == L"--help" || a == L"-h" || a == L"/?") { usage(); return 0; }
        else { std::printf("error: unknown argument '%s'\n", narrow(a).c_str()); usage(); return 2; }
    }
    std::printf("sonicnative_convert %s\n", kVersion);
    if (o.userpath.empty() && o.out.empty()) { usage(); return 2; }
    {   // one spelling of the clip list for the lookup and the hash
        std::string list;
        for (auto &id : clip_ids(o.clips)) list += (list.empty() ? "" : ",") + id;
        o.clips = list;
    }
    // Where the package goes: the user folder (BeamNG closed: it keeps the shape and its cache open), or --out.
    fs::path data, cache;
    std::error_code ec;
    if (o.out.empty()) {
        if (beamng_running()) {
            std::printf("BeamNG.drive.x64.exe is running: close the game before converting (it keeps the shape and its cache open).\n");
            return 3;
        }
        const fs::path user(o.userpath);
        if (!fs::is_directory(user / L"current", ec)) {
            std::printf("error: %s has no 'current' folder: not a BeamNG user folder (run the game once with it)\n", narrow(o.userpath).c_str());
            return 2;
        }
        data = user / L"current" / L"mods" / widen(kDataFolder);
        cache = user / L"current" / L"temp" / L"mods" / widen(kDataFolder);
    } else {
        const std::string refusal = out_refusal(o.out);
        if (!refusal.empty()) { std::printf("error: --out %s: %s\n", narrow(o.out).c_str(), refusal.c_str()); return 2; }
        data = fs::path(o.out);
        fs::create_directories(data, ec);
        if (ec) { std::printf("error: cannot create %s: %s\n", narrow(o.out).c_str(), ec.message().c_str()); return 2; }
        std::printf("--out: writing into %s (not a BeamNG user folder)\n", narrow(o.out).c_str());
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
        // Sounds never touch the shape: the hash stays the same with or without them.
        if (o.sounds) {
            std::printf("sounds from %s\n", narrow(o.frontiers + L"\\" + sonic_data::kSoundFolder).c_str());
            p.with_sounds = true;
            prepare_sounds(o.frontiers, p.sounds);
        }
    }
    hash_input += std::string("|clips=") + (o.fixture ? "idle,lean" : o.clips) + "|split=" + split_name(o.bind_only ? Split::bind : o.split) +
                  "|flip=" + (o.flip_green ? "1" : "0") + "|srgb=" + (o.srgb ? "1" : "0") + "|flat=" + (o.flat ? "1" : "0") +
                  "|uvclamp=" + (o.uv_clamp ? "1" : "0");
    // The aura changes the shape and the fx add the bone tracks named after the hash: what this run made counts
    if (!o.fixture) hash_input += std::string("|aura=") + (p.with_aura ? "1" : "0") + "|fx=" + (p.with_fx ? "1" : "0");
    const std::string hash = hex8(fnv1a(hash_input));
    p.ctor = "sonicnative_" + p.stem + "_" + hash + "Dae";
    print_rig(p.rig);
    if (!write_package(data, cache, p, o, hash)) return 1;
    std::printf("done: %s, split %s, hash %s%s%s%s\n", p.folder.c_str(), split_name(o.bind_only ? Split::bind : o.split), hash.c_str(),
                o.fixture ? "" : p.with_ball ? ", spin ball" : ", no spin ball", o.fixture ? "" : p.with_aura ? ", boost aura" : ", no boost aura",
                o.fixture ? "" : p.with_fx ? ", boost effects" : ", no boost effects");
    return 0;
}
