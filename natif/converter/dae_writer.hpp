// SPDX-License-Identifier: MIT
// COLLADA 1.4.1 writer for a Rig, laid out like the game's own skinned shapes (scarecrow.dae):
// Z_UP, meters, base00 > start01 > {armature of JOINT nodes, one <object>2 node per mesh}.
// Joints carry <matrix sid="transform"> (bind local); animated joints get one <animation> of baked
// float4x4 locals on the shared timeline. Split modes:
//   clips  one <animation_clip name=...> per clip with Torque's cyclic extra
//   cs     no clips (Torque folds everything into 'ambient'); a TSShapeConstructor .cs splits it
//   bind   no animation at all (diagnostic)
#pragma once
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include "skin_math.hpp"
#include "text.hpp"

namespace sonicnative {

enum class Split { clips, cs, bind };

inline const char *split_name(Split s) { return s == Split::clips ? "clips" : s == Split::cs ? "cs" : "bind"; }

struct DaeInfo {
    std::string tool;          // authoring_tool text
    std::string armature = "sonicnative_armature";
    Split split = Split::clips;
};

namespace dae {

inline void matrix(Text &t, const float *m) { t.nums(m, 16, 16); }

inline void source_floats(Text &t, const std::string &id, const float *v, size_t count, size_t stride,
                          const char *const *params, size_t nparams, const char *type, size_t per_line, size_t indent = 8)
{
    const std::string pad(indent, ' ');
    t << pad << "<source id=\"" << id << "\">\n" << pad << "  <float_array id=\"" << id << "-array\" count=\"";
    t.u(count) << "\">";
    t.nums(v, count, per_line);
    t << "</float_array>\n" << pad << "  <technique_common><accessor source=\"#" << id << "-array\" count=\"";
    t.u(stride ? count / stride : 0) << "\" stride=\"";
    t.u(stride) << "\">";
    for (size_t i = 0; i < nparams; ++i) t << "<param name=\"" << params[i] << "\" type=\"" << type << "\"/>";
    t << "</accessor></technique_common>\n" << pad << "</source>\n";
}

inline void joint_tree(Text &t, const Rig &rig, int parent, int depth)
{
    for (size_t i = 0; i < rig.joints.size(); ++i) {
        const RigJoint &j = rig.joints[i];
        if (j.parent != parent) continue;
        const std::string pad(size_t(depth) * 2, ' ');
        t << pad << "<node id=\"j_" << j.sid << "\" name=\"" << xml_escape(j.name) << "\" sid=\"" << j.sid << "\" type=\"JOINT\">\n";
        t << pad << "  <matrix sid=\"transform\">";
        matrix(t, j.bind_local);
        t << "</matrix>\n";
        joint_tree(t, rig, int(i), depth + 1);
        t << pad << "</node>\n";
    }
}

} // namespace dae

inline std::string write_dae(const Rig &rig, const DaeInfo &info)
{
    Text t;
    t.s.reserve(16u << 20);
    SYSTEMTIME now;
    GetSystemTime(&now);
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%04u-%02u-%02uT%02u:%02u:%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    t << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
         "<COLLADA xmlns=\"http://www.collada.org/2005/11/COLLADASchema\" version=\"1.4.1\">\n"
         "  <asset>\n    <contributor><authoring_tool>" << xml_escape(info.tool) << "</authoring_tool></contributor>\n"
         "    <created>" << stamp << "</created>\n    <modified>" << stamp << "</modified>\n"
         "    <unit name=\"meter\" meter=\"1\"/>\n    <up_axis>Z_UP</up_axis>\n  </asset>\n";

    // Materials are bound by name: <material name> must equal the game material's mapTo.
    std::vector<std::string> materials;
    for (auto &m : rig.meshes) if (std::find(materials.begin(), materials.end(), m.material) == materials.end()) materials.push_back(m.material);
    t << "  <library_effects>\n";
    for (auto &m : materials)
        t << "    <effect id=\"" << m << "-effect\"><profile_COMMON><technique sid=\"common\"><phong>"
             "<diffuse><color sid=\"diffuse\">0.8 0.8 0.8 1</color></diffuse></phong></technique></profile_COMMON></effect>\n";
    t << "  </library_effects>\n  <library_materials>\n";
    for (auto &m : materials) t << "    <material id=\"" << m << "-material\" name=\"" << m << "\"><instance_effect url=\"#" << m << "-effect\"/></material>\n";
    t << "  </library_materials>\n";

    static const char *xyz[] = {"X", "Y", "Z"}, *st[] = {"S", "T"}, *transform[] = {"TRANSFORM"}, *weight[] = {"WEIGHT"}, *time[] = {"TIME"};
    t << "  <library_geometries>\n";
    for (auto &m : rig.meshes) {
        const std::string g = m.object + "-mesh";
        t << "    <geometry id=\"" << g << "\" name=\"" << m.object << "\">\n      <mesh>\n";
        dae::source_floats(t, g + "-positions", m.positions.data(), m.positions.size(), 3, xyz, 3, "float", 24);
        dae::source_floats(t, g + "-normals", m.normals.data(), m.normals.size(), 3, xyz, 3, "float", 24);
        dae::source_floats(t, g + "-map-0", m.uvs.data(), m.uvs.size(), 2, st, 2, "float", 24);
        t << "        <vertices id=\"" << g << "-vertices\"><input semantic=\"POSITION\" source=\"#" << g << "-positions\"/></vertices>\n";
        t << "        <triangles material=\"" << m.material << "-material\" count=\"";
        t.u(m.triangles.size() / 3) << "\">\n"
            "          <input semantic=\"VERTEX\" source=\"#" << g << "-vertices\" offset=\"0\"/>\n"
            "          <input semantic=\"NORMAL\" source=\"#" << g << "-normals\" offset=\"1\"/>\n"
            "          <input semantic=\"TEXCOORD\" source=\"#" << g << "-map-0\" offset=\"2\" set=\"0\"/>\n          <p>";
        for (size_t i = 0; i < m.triangles.size(); ++i) {
            if (i) t << ((i % 24) ? ' ' : '\n');
            t.u(m.triangles[i]) << ' ';
            t.u(m.triangles[i]) << ' ';
            t.u(m.triangles[i]);
        }
        t << "</p>\n        </triangles>\n      </mesh>\n    </geometry>\n";
    }
    t << "  </library_geometries>\n";

    // Skin: Name_array = joint sids, INV_BIND_MATRIX in the same order, weights indexed by byte value (k/255).
    float weights[256];
    for (int k = 0; k < 256; ++k) weights[k] = k / 255.0f;
    t << "  <library_controllers>\n";
    for (auto &m : rig.meshes) {
        const std::string c = m.object + "-skin";
        t << "    <controller id=\"" << c << "\" name=\"" << c << "\">\n      <skin source=\"#" << m.object << "-mesh\">\n"
             "        <bind_shape_matrix>1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</bind_shape_matrix>\n";
        t << "        <source id=\"" << c << "-joints\">\n          <Name_array id=\"" << c << "-joints-array\" count=\"";
        t.u(m.joints.size()) << "\">";
        for (size_t i = 0; i < m.joints.size(); ++i) t << (i ? " " : "") << rig.joints[size_t(m.joints[i])].sid;
        t << "</Name_array>\n          <technique_common><accessor source=\"#" << c << "-joints-array\" count=\"";
        t.u(m.joints.size()) << "\" stride=\"1\"><param name=\"JOINT\" type=\"name\"/></accessor></technique_common>\n        </source>\n";
        std::vector<float> ib;
        for (int j : m.joints) ib.insert(ib.end(), rig.joints[size_t(j)].inverse_bind, rig.joints[size_t(j)].inverse_bind + 16);
        dae::source_floats(t, c + "-bind_poses", ib.data(), ib.size(), 16, transform, 1, "float4x4", 16);
        dae::source_floats(t, c + "-weights", weights, 256, 1, weight, 1, "float", 16);
        t << "        <joints><input semantic=\"JOINT\" source=\"#" << c << "-joints\"/><input semantic=\"INV_BIND_MATRIX\" source=\"#" << c << "-bind_poses\"/></joints>\n";
        t << "        <vertex_weights count=\"";
        t.u(m.vcount.size()) << "\">\n          <input semantic=\"JOINT\" source=\"#" << c << "-joints\" offset=\"0\"/>\n"
            "          <input semantic=\"WEIGHT\" source=\"#" << c << "-weights\" offset=\"1\"/>\n          <vcount>";
        for (size_t i = 0; i < m.vcount.size(); ++i) { if (i) t << ((i % 48) ? ' ' : '\n'); t.u(m.vcount[i]); }
        t << "</vcount>\n          <v>";
        for (size_t i = 0; i < m.influences.size(); i += 2) { if (i) t << ((i % 32) ? ' ' : '\n'); t.u(m.influences[i]) << ' '; t.u(m.influences[i + 1]); }
        t << "</v>\n        </vertex_weights>\n      </skin>\n    </controller>\n";
    }
    t << "  </library_controllers>\n";

    if (info.split != Split::bind && rig.total_frames) {
        std::vector<float> times(rig.total_frames);
        for (uint32_t f = 0; f < rig.total_frames; ++f) times[f] = quantize(f / rig.fps);
        std::vector<float> samples(size_t(rig.total_frames) * 16);
        t << "  <library_animations>\n";
        for (size_t i = 0; i < rig.joints.size(); ++i) {
            const RigJoint &j = rig.joints[i];
            if (!j.animated) continue;
            const std::string a = "a_" + j.sid;
            for (uint32_t f = 0; f < rig.total_frames; ++f) std::memcpy(&samples[size_t(f) * 16], rig.local(f, i), 64);
            t << "    <animation id=\"" << a << "\" name=\"" << a << "\">\n";
            dae::source_floats(t, a + "-input", times.data(), times.size(), 1, time, 1, "float", 30, 6);
            dae::source_floats(t, a + "-output", samples.data(), samples.size(), 16, transform, 1, "float4x4", 16, 6);
            t << "      <source id=\"" << a << "-interpolation\">\n        <Name_array id=\"" << a << "-interpolation-array\" count=\"";
            t.u(rig.total_frames) << "\">";
            for (uint32_t f = 0; f < rig.total_frames; ++f) t << ((f == 0) ? "" : (f % 30) ? " " : "\n") << "LINEAR";
            t << "</Name_array>\n        <technique_common><accessor source=\"#" << a << "-interpolation-array\" count=\"";
            t.u(rig.total_frames) << "\" stride=\"1\"><param name=\"INTERPOLATION\" type=\"name\"/></accessor></technique_common>\n      </source>\n";
            t << "      <sampler id=\"" << a << "-sampler\">\n"
                 "        <input semantic=\"INPUT\" source=\"#" << a << "-input\"/>\n"
                 "        <input semantic=\"OUTPUT\" source=\"#" << a << "-output\"/>\n"
                 "        <input semantic=\"INTERPOLATION\" source=\"#" << a << "-interpolation\"/>\n"
                 "      </sampler>\n"
                 "      <channel source=\"#" << a << "-sampler\" target=\"j_" << j.sid << "/transform\"/>\n    </animation>\n";
        }
        t << "  </library_animations>\n";
        if (info.split == Split::clips) {
            t << "  <library_animation_clips>\n";
            for (auto &c : rig.clips) {
                t << "    <animation_clip id=\"clip_" << sanitize_sid(c.name) << "\" name=\"" << xml_escape(c.name) << "\" start=\"";
                t.num(quantize(c.first / rig.fps)) << "\" end=\"";
                t.num(quantize((c.first + c.frames - 1) / rig.fps)) << "\">\n";
                for (auto &j : rig.joints) if (j.animated) t << "      <instance_animation url=\"#a_" << j.sid << "\"/>\n";
                t << "      <extra><technique profile=\"Torque\"><cyclic>" << (c.loop ? "1" : "0") << "</cyclic></technique></extra>\n    </animation_clip>\n";
            }
            t << "  </library_animation_clips>\n";
        }
    }

    t << "  <library_visual_scenes>\n    <visual_scene id=\"Scene\" name=\"Scene\">\n"
         "      <node id=\"base00\" name=\"base00\" type=\"NODE\">\n        <matrix sid=\"transform\">1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</matrix>\n"
         "        <node id=\"start01\" name=\"start01\" type=\"NODE\">\n          <matrix sid=\"transform\">1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</matrix>\n"
         "          <node id=\"" << info.armature << "\" name=\"" << info.armature << "\" type=\"NODE\">\n"
         "            <matrix sid=\"transform\">1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</matrix>\n";
    dae::joint_tree(t, rig, -1, 6);
    t << "          </node>\n";
    for (auto &m : rig.meshes) {
        // Trailing number = detail size: <object>2 is drawn down to 2 px on screen.
        t << "          <node id=\"" << m.object << "2\" name=\"" << m.object << "2\" type=\"NODE\">\n"
             "            <matrix sid=\"transform\">1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</matrix>\n"
             "            <instance_controller url=\"#" << m.object << "-skin\">\n";
        // Joint sids are looked up under <skeleton>: the root joint, or the armature when the tree is flat.
        if (rig.flat) t << "              <skeleton>#" << info.armature << "</skeleton>\n";
        else for (auto &j : rig.joints) if (j.parent < 0) t << "              <skeleton>#j_" << j.sid << "</skeleton>\n";
        t << "              <bind_material><technique_common><instance_material symbol=\"" << m.material << "-material\" target=\"#"
          << m.material << "-material\"/></technique_common></bind_material>\n            </instance_controller>\n          </node>\n";
    }
    t << "        </node>\n      </node>\n    </visual_scene>\n  </library_visual_scenes>\n"
         "  <scene><instance_visual_scene url=\"#Scene\"/></scene>\n</COLLADA>\n";
    return std::move(t.s);
}

// Torque's COLLADA resampling (Torque3D tsShapeLoader / colladaShapeLoader): fps = clamp(1/minFrameTime + 0.5,
// 15, 60) over the written key times, keys = int(duration * fps + 0.5) + 1. A prediction for the log only.
inline uint32_t torque_keys(const Rig &rig, uint32_t first, uint32_t last, float &fps)
{
    float min_dt = 1000.0f;
    for (uint32_t f = 1; f < rig.total_frames; ++f) min_dt = std::min(min_dt, quantize(f / rig.fps) - quantize((f - 1) / rig.fps));
    fps = std::clamp(1.0f / min_dt + 0.5f, 15.0f, 60.0f);
    return uint32_t((quantize(last / rig.fps) - quantize(first / rig.fps)) * fps + 0.5f) + 1;
}

// --split cs: TSShapeConstructor next to the shape, like the game's greenlight.cs. Torque resamples the
// single 'ambient' sequence (not one key per frame), so onLoad scales each clip's frame range by ambient's
// real key count; ceil/floor keep a range inside its clip. The echo is the in-game probe.
inline std::string write_constructor(const Rig &rig, const std::string &ctor, const std::string &dae_file)
{
    const uint32_t last = std::max<uint32_t>(rig.total_frames, 2) - 1;
    Text t;
    t << "// Generated by sonicnative_convert: splits the 'ambient' timeline of " << dae_file << " into named sequences.\n"
      << "// Ranges are frames 0.." << std::to_string(last) << " of the converted timeline, scaled to ambient's keys.\n\n"
      << "function " << ctor << "::onLoad(%this)\n{\n"
      << "   %n = %this.getSequenceFrameCount(\"ambient\");\n"
      << "   %k = (%n - 1) / " << std::to_string(last) << ";\n"
      << "   echo(\"" << ctor << ": ambient has \" @ %n @ \" keys for " << std::to_string(last + 1) << " frames\");\n"
      << "   if (%n < 2) { error(\"" << ctor << ": no ambient sequence to split\"); return; }\n";
    for (auto &c : rig.clips) {
        t << "   %this.addSequence(\"ambient\", \"" << c.name << "\", ";
        if (c.first) { t << "mCeil("; t.u(c.first) << " * %k - 0.01)"; } else t << "0";
        t << ", mFloor(";
        t.u(c.first + c.frames - 1) << " * %k + 0.01), \"1\", \"0\");\n";
        t << "   %this.setSequenceCyclic(\"" << c.name << "\", \"" << (c.loop ? "1" : "0") << "\");\n";
    }
    t << "}\n\nsingleton TSShapeConstructor(" << ctor << ")\n{\n   baseShape = \"./" << dae_file << "\";\n};\n";
    return std::move(t.s);
}

} // namespace sonicnative
