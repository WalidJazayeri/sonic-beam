// SPDX-License-Identifier: MIT
// Sonic's behaviour in BeamNG: his walking body (moved by the mod's Lua) picks his Frontiers
// animation, and running like Sonic earns Frontiers rings. All numbers come from the sheets.
#include "game.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "audio.hpp"
#include "testmode.hpp"

namespace game {

namespace {
using namespace sheets;
constexpr float kPi = 3.14159265f;
constexpr float kDeg = kPi / 180;

struct V3 { float x = 0, y = 0, z = 0; };
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(V3 a) { return std::sqrt(dot(a, a)); }
V3 norm(V3 a) { const float l = length(a); return l > 1e-6f ? a * (1 / l) : V3{0, 1, 0}; }
V3 v3(const float *p) { return {p[0], p[1], p[2]}; }

// Row-vector 4x4 helpers: p' = p * M.
struct M4 { float m[16]; };
M4 identity() { M4 r{}; r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1; return r; }
M4 mul(const M4 &a, const M4 &b)
{
    M4 r{};
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) {
        float s = 0; for (int k = 0; k < 4; ++k) s += a.m[i * 4 + k] * b.m[k * 4 + j];
        r.m[i * 4 + j] = s;
    }
    return r;
}
M4 scale(float s) { M4 r = identity(); r.m[0] = r.m[5] = r.m[10] = s; return r; }
M4 scale3(float x, float y, float z) { M4 r = identity(); r.m[0] = x; r.m[5] = y; r.m[10] = z; return r; }
M4 rot_y(float a) { M4 r = identity(); const float c = std::cos(a), s = std::sin(a); r.m[0] = c; r.m[2] = -s; r.m[8] = s; r.m[10] = c; return r; }
M4 rot_z(float a) { M4 r = identity(); const float c = std::cos(a), s = std::sin(a); r.m[0] = c; r.m[1] = s; r.m[4] = -s; r.m[5] = c; return r; }
M4 translate(V3 t) { M4 r = identity(); r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z; return r; }
// rotation by `a` around the unit axis `n` through the point `at` (row vectors)
M4 rot_around(V3 n, float a, V3 at)
{
    const float c = std::cos(a), s = std::sin(a), t = 1 - c;
    M4 r = identity();
    // column-vector Rodrigues matrix, stored transposed for p' = p * M
    const float m[3][3] = {{t * n.x * n.x + c, t * n.x * n.y - s * n.z, t * n.x * n.z + s * n.y},
                           {t * n.x * n.y + s * n.z, t * n.y * n.y + c, t * n.y * n.z - s * n.x},
                           {t * n.x * n.z - s * n.y, t * n.y * n.z + s * n.x, t * n.z * n.z + c}};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) r.m[j * 4 + i] = m[i][j];
    return mul(mul(translate(at * -1.0f), r), translate(at));
}
// Rows are where model X, Y, Z go, then the position.
M4 basis(V3 x, V3 y, V3 z, V3 p)
{
    M4 r{};
    const V3 rows[4] = {x, y, z, p};
    for (int i = 0; i < 4; ++i) { r.m[i * 4] = rows[i].x; r.m[i * 4 + 1] = rows[i].y; r.m[i * 4 + 2] = rows[i].z; }
    r.m[15] = 1;
    return r;
}
void copy(const M4 &src, float *dst) { std::memcpy(dst, src.m, sizeof(src.m)); }

uint32_t rng_state = 12345;
float rnd() { rng_state = rng_state * 1664525u + 1013904223u; return (rng_state >> 8) * (1.0f / 16777216.0f); }

// ------------------------------------------------------------ animation
struct Player {
    int anim = int(Anim::idle), prev = -1;
    double time = 0, prev_time = 0, blend = 1, blend_len = 0.2;
    float rate = 1, prev_rate = 1;
};

void play(Player &p, Anim anim)
{
    if (int(anim) == p.anim) return;
    p.prev = p.anim; p.prev_time = p.time; p.prev_rate = p.rate;
    p.anim = int(anim); p.time = 0; p.blend = 0;
    p.blend_len = std::max(0.01f, animations[int(anim)].blendSeconds);
}

void sample(const frontiers::Clip &clip, bool loop, double time, float *out)
{
    const uint32_t frames = clip.frames, n = clip.nodes * 12;
    double f = time * clip.rate;
    if (loop && frames > 1) f = std::fmod(f, double(frames - 1)); else f = std::min(f, double(frames - 1));
    if (f < 0) f = 0;
    const uint32_t f0 = uint32_t(f), f1 = std::min(frames - 1, f0 + 1);
    const float w = float(f - f0);
    const float *a = clip.frame(f0), *b = clip.frame(f1);
    for (uint32_t i = 0; i < n; ++i) out[i] = a[i] + (b[i] - a[i]) * w;
}

struct Popup { V3 pos, vel; double born; };

struct State {
    Player player;
    int state = int(sheets::State::idle), candidate = -1, logged_state = -1;
    double candidate_since = 0;
    int trick_step = 0;
    // jump ball (Sonic Beam Lab, Jump): this airtime started with a jump; past the top of it
    bool jump_air = false, past_apex = false;
    double jump_at = -100, air_trick_until = 0;
    int air_trick = -1;
    float ball_angle = 0, ball_amount = 0;
    double event_until = 0;
    std::vector<Popup> popups;
    int rings = 0, plus = 0;
    double plus_time = -10, drift_time = 0, last_pay[int(RingRule::count)] = {-100, -100, -100, -100};
    bool was_boosting = false;
    float blur = 0, aura = 0;
    int step_anim = -1;
    float wind = 0;
    struct TrailPoint { V3 pos; double born; };
    std::vector<TrailPoint> trail;
    float trail_on = 0;
    struct Bolt { V3 pos, vel; double born; float life, size, spin; int kind, cell; int node = -1; };   // node: on Sonic's bone (pos is then an offset)
    std::vector<Bolt> bolts;
    double bolt_debt = 0, arc_reroll = 0, body_bolt_debt = 0, aura_clock = 0;
    int arc_column = 0;
    std::vector<V3> arc_jitter;
    double step_phase = 0;
    bool wanted_move = false;
    double burst_at = -100;
    V3 burst_pos;
    float speed_kmh = 0, shown_kmh = 0, heading = 0, turn_rate = 0, lean = 0;
    bool heading_set = false, air = false;
    double ground_lost = 0, air_time = 0, run_time = 0, last_frame = 0, first_shown = -1;
    std::vector<float> scratch;
} s;

// Frontiers footstep, landing sounds by ground: concrete, dirt, grass, sand, snow, water, metal
const Sound kWalk[7] = {Sound::step_walk_conc, Sound::step_walk_dirt, Sound::step_walk_gras, Sound::step_walk_sand, Sound::step_walk_snow, Sound::step_walk_watr, Sound::step_walk_mtlp};
const Sound kRun[7] = {Sound::step_run_conc, Sound::step_run_dirt, Sound::step_run_gras, Sound::step_run_sand, Sound::step_run_snow, Sound::step_run_watr, Sound::step_run_mtlp};
const Sound kLand[7] = {Sound::land, Sound::land_dirt, Sound::land_gras, Sound::land_sand, Sound::land_snow, Sound::land_watr, Sound::land_mtlp};

void pay(RingRule rule, int amount, double now, V3 origin)
{
    if (!tuning::ringsEnabled) return;   // rings are switched off for now (sheets/tuning.json)
    const RingRuleRow &row = ringRules[int(rule)];
    if (now - s.last_pay[int(rule)] < row.cooldownSeconds || amount <= 0) return;
    s.last_pay[int(rule)] = now;
    s.rings += amount;
    s.plus = (now - s.plus_time < 1.2) ? s.plus + amount : amount;
    s.plus_time = now;
    audio::play(row.sound);
    for (int i = 0; i < row.popup; ++i) {
        const float a = rnd() * 2 * kPi;
        s.popups.push_back({origin, {std::cos(a) * 2.2f, std::sin(a) * 2.2f, 3.0f + rnd() * 2}, now});
    }
}

void add_text(Scene &scene, float x, float y, float size, float r, float g, float b, float a, const std::string &text)
{
    scene.texts.push_back({x, y, size, {r, g, b, a}, text});
}
} // namespace

int ring_total() { return s.rings; }

void update(const assets::Data *d, const SbFrame *f, double now, float aspect, Scene &scene)
{
    scene.camera = false;
    scene.sonic_visible = false;
    scene.shadow = false;
    scene.sonic_shadow = false;
    scene.rings.clear();
    scene.texts.clear();
    scene.sprites.clear();
    scene.trail.clear();
    scene.beam.clear();
    scene.arcs.clear();
    scene.blur = 0;
    scene.aura = 0;
    scene.ball_visible = false;
    scene.sonic_hidden = false;
    std::memset(scene.clip_plane, 0, sizeof(scene.clip_plane));
    const double dt = s.last_frame ? std::min(0.1, now - s.last_frame) : 0.016;
    s.last_frame = now;
    if (!d) {
        if (assets::failed()) add_text(scene, -0.95f, 0.92f, 0.035f, 1, 0.4f, 0.3f, 1, "SONIC BEAM: SONIC FRONTIERS FILES NOT FOUND - SEE SONICBEAM LOG");
        return;
    }
    if (!f) return;

    // ---------------- camera: BeamNG's position, forward and up, vertical field of view
    const V3 cam = v3(f->camPos), fwd = norm(v3(f->camFwd));
    const V3 right = norm(cross(fwd, v3(f->camUp))), up = cross(right, fwd);
    M4 view = identity();
    const V3 axes[3] = {right, up, fwd};
    for (int c = 0; c < 3; ++c) {
        view.m[0 * 4 + c] = (&axes[c].x)[0];
        view.m[1 * 4 + c] = (&axes[c].x)[1];
        view.m[2 * 4 + c] = (&axes[c].x)[2];
        view.m[12 + c] = -dot(cam, axes[c]);
    }
    const float ys = 1 / std::tan(std::max(0.1f, f->fovY) * 0.5f), xs = ys / std::max(0.1f, aspect);
    const float proj[16] = {xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, 0, 1, 0, 0, tuning::nearClip, 0};
    copy(view, scene.view);
    std::memcpy(scene.proj, proj, sizeof(proj));
    scene.camera = true;

    if (!f->onFoot) {   // Sonic is in a car (BeamNG's F key): only the counter stays
        s.first_shown = s.first_shown < 0 ? -1 : s.first_shown;
    }

    // ---------------- motion of Sonic's body
    V3 vh = v3(f->vel); vh.z = 0;
    if (!f->onFoot || !(length(vh) < 1e6f)) vh = {};   // in a car, or a teleport: not Sonic running
    const float speed = length(vh) * 3.6f;
    s.speed_kmh += (speed - s.speed_kmh) * (1 - std::exp(-float(dt) / tuning::speedSmoothing));
    const float speed3d = f->onFoot && length(v3(f->vel)) < 1e6f ? length(v3(f->vel)) * 3.6f : 0;
    s.shown_kmh += (speed3d - s.shown_kmh) * (1 - std::exp(-float(dt) / 0.08f));
    if (length(vh) > 1.5f) {
        const float target = std::atan2(vh.y, vh.x);
        if (!s.heading_set) { s.heading = target; s.heading_set = true; }
        float delta = target - s.heading;
        while (delta > kPi) delta -= 2 * kPi;
        while (delta < -kPi) delta += 2 * kPi;
        // swimming: turn slowly (waves and pushes made him spin at a crawl)
        const float step = delta * std::min(1.0f, float(dt) * tuning::facingRate * (f->water == 2 ? 0.2f : 1.0f));
        s.heading += step;
        s.turn_rate += (step / float(std::max(dt, 1e-3)) - s.turn_rate) * std::min(1.0f, float(dt) * 6);
    } else {
        s.turn_rate *= 0.9f;
        if (!s.heading_set) { s.heading = std::atan2(fwd.y, fwd.x); s.heading_set = true; }
    }
    const bool was_air = s.air;
    if (f->grounded || f->water == 2) { s.ground_lost = 0; s.air = false; }   // swimming is not flying
    else { s.ground_lost += dt; if (s.ground_lost > 0.12) s.air = true; }
    if (s.air) s.air_time = was_air ? s.air_time + dt : s.ground_lost;
    const bool landed = was_air && !s.air;
    const float landed_air = landed ? float(s.air_time) : 0;
    if (!s.air && !landed) s.air_time = 0;
    if (f->jumped) audio::play(Sound::jump);
    // ---------------- jump ball (Frontiers: Sonic curls up into a spinning ball when he jumps)
    const int jump_style = std::max(0, std::min(2, int(f->jumpStyle)));
    if (f->jumped && f->onFoot) { s.jump_air = true; s.past_apex = false; s.jump_at = now; s.air_trick = -1; }
    if (landed || !f->onFoot || (s.jump_air && !s.air && now - s.jump_at > 0.4)) s.jump_air = false;
    if (s.jump_air && f->vel[2] < 0 && now - s.jump_at > 0.15) s.past_apex = true;
    // the ball holds the air tricks back: for the whole jump (0), up to the top (1), or until a direction is held (2)
    const bool ball_jump = s.jump_air && !(jump_style == 1 && s.past_apex);
    const int turn = s.turn_rate / kDeg > tuning::turnRateDegrees ? 1 : s.turn_rate / kDeg < -tuning::turnRateDegrees ? 2 : 0;

    // ---------------- state selection (sheets/states.json)
    int best = -1;
    for (int i = 0; i < int(sheets::State::count); ++i) {
        const StateRow &row = states[i];
        if (s.speed_kmh < row.minKmh || s.speed_kmh >= row.maxKmh) continue;
        if (row.air >= 0 && (row.air == 1) != s.air) continue;
        if (row.air == 1 && s.air_time < row.minAirSeconds) continue;
        if (ball_jump && i == int(sheets::State::trick)) continue;
        if (row.boost >= 0 && (row.boost == 1) != (f->boost != 0)) continue;
        if (row.turn >= 0 && row.turn != turn) continue;
        if (best < 0 || row.priority > states[best].priority) best = i;
    }
    if (best < 0) best = int(sheets::State::idle);
    if (best != s.candidate) { s.candidate = best; s.candidate_since = now; }
    const bool urgent = states[best].air == 1 || states[s.state].air == 1;
    if (best != s.state && (urgent || now - s.candidate_since >= 0.1)) {
        if (states[best].enterSound >= 0) audio::play(Sound(states[best].enterSound));
        if (best == int(sheets::State::trick)) s.trick_step = int(rnd() * 3);
        s.state = best;
    }
    const V3 feet = v3(f->feet);
    // Boost effects grow with Sonic's speed (Sonic Beam Lab): full at the reference speed (the O
    // menu's top speed, or 500 km/h), down to (1 - amount) when he is slow
    const float fx_ratio = std::max(0.0f, std::min(1.0f, s.speed_kmh / std::max(1.0f, f->fxRefKmh)));
    const float fx_speed = 1 - std::max(0.0f, std::min(1.0f, f->fxSpeedAmount)) * (1 - fx_ratio);
    const int surface = std::max(0, std::min(6, int(f->surface)));
    audio::set_levels(f->soundDb, f->stepsDb);
    const V3 origin = feet + V3{0, 0, 0.6f};

    // ---------------- events and rewards (sheets/events.json, rings.json)
    if (landed && landed_air >= landingEvent.minAirSeconds) {
        s.event_until = now + landingEvent.holdSeconds;
        if (landingEvent.sound >= 0) audio::play(kLand[surface]);
        const RingRuleRow &air = ringRules[int(RingRule::air)];
        if (landed_air >= air.everySeconds) pay(RingRule::air, int(std::round(air.rings * landed_air)), now, origin);
    }
    const bool drifting = s.state == int(sheets::State::drift_left) || s.state == int(sheets::State::drift_right);
    if (drifting) {
        s.drift_time += dt;
        const RingRuleRow &row = ringRules[int(RingRule::drift)];
        if (s.drift_time >= row.everySeconds) { s.drift_time -= row.everySeconds; pay(RingRule::drift, row.rings, now, origin); }
    } else s.drift_time = 0;
    const bool boosting = s.state == int(sheets::State::boost) || drifting;
    if (boosting && !s.was_boosting) {
        pay(RingRule::boost, ringRules[int(RingRule::boost)].rings, now, origin);
        s.burst_at = now; s.burst_pos = origin;
        if (f->boostBoomKmh > 0 && s.speed_kmh >= f->boostBoomKmh) audio::play(Sound::sonic_boom);   // boom on a fast boost
        // Power Boost start: a crackle of bolts and glowing orbs drifting away (Frontiers' pow_boost_impact)
        const float amount = (f->lightning > 0 ? f->lightning : 0) * fx_speed;
        for (int i = 0; i < int(10 * amount); ++i) {
            const float a = rnd() * 2 * kPi, r = 0.2f + 0.6f * rnd();
            s.bolts.push_back({origin + V3{std::cos(a) * r, std::sin(a) * r, (rnd() - 0.3f) * 0.9f}, {}, now + 0.03 * i, 0.14f, 0.7f + 0.6f * rnd(), rnd() * 6.28f, 2, int(rnd() * 16) & 15});
        }
        for (int i = 0; i < 9; ++i) {
            const float a = rnd() * 2 * kPi;
            s.bolts.push_back({origin, {std::cos(a) * (1.2f + rnd()), std::sin(a) * (1.2f + rnd()), 0.4f + rnd() * 1.4f}, now, 0.9f, 0.35f + 0.25f * rnd(), 0, 3, 0});
        }
    }
    s.was_boosting = boosting;
    if (boosting && s.speed_kmh > 100) testmode::report_boosting();
    for (int i = 0; i < f->picked; ++i) {
        s.last_pay[int(RingRule::pickup)] = -100;
        pay(RingRule::pickup, ringRules[int(RingRule::pickup)].rings, now, origin);
    }
    if ((s.state == int(sheets::State::run) || boosting) && s.speed_kmh > 25) {
        s.run_time += dt;
        if (s.run_time > 2.0) testmode::report_running();
    }

    // ---------------- animation
    Anim want = states[s.state].animation;
    // style 2: holding a direction in the air plays that trick, then Sonic curls up again
    bool ball_now = ball_jump;
    if (ball_jump && jump_style == 2) {
        if (s.air_trick < 0 && f->airInput > 0 && s.air) {
            s.air_trick = f->airInput == 1 ? int(Anim::trick_left) : f->airInput == 2 ? int(Anim::trick_right) : int(Anim::trick_up);
            s.air_trick_until = now + d->clips[s.air_trick].duration + 0.05;
            audio::play(Sound::trick);
        }
        if (s.air_trick >= 0 && now >= s.air_trick_until) s.air_trick = f->airInput > 0 ? -1 : -2;   // -2: wait for the release
        if (s.air_trick == -2 && f->airInput == 0) s.air_trick = -1;
        if (s.air_trick >= 0) { ball_now = false; want = Anim(s.air_trick); }
    }
    if (f->water == 2) {
        want = Anim::swim;   // out of his depth and too slow to run on the water
        ball_now = false;
    } else if (ball_now) {
        const double start = std::min(0.22, double(d->clips[int(Anim::jump_start)].duration));
        want = now - s.jump_at < start ? Anim::jump_start : Anim::ball;
    } else if (s.air_trick >= 0) {
    } else if (s.state == int(sheets::State::trick)) {
        static const Anim tricks[3] = {Anim::trick_up, Anim::trick_left, Anim::trick_right};
        const Anim current = tricks[s.trick_step % 3];
        if (s.player.anim == int(current) && s.player.time > d->clips[s.player.anim].duration + 0.12) ++s.trick_step;
        want = tricks[s.trick_step % 3];
    } else if (now < s.event_until && !s.air) {
        want = landingEvent.animation;
    }
    play(s.player, want);
    if (s.state != s.logged_state) {
        s.logged_state = s.state;
        mod::log("state: %s at %.0f km/h (turn %.0f deg/s, air %d %.2fs, boost %d)", states[s.state].id, s.speed_kmh,
                 s.turn_rate / kDeg, int(s.air), s.air_time, f->boost);
    }
    const AnimationRow &arow = animations[s.player.anim];
    // Like Frontiers: the run cycle speeds up with Sonic, then eases into its fastest rate so
    // his limbs never flail at absurd speeds; the rate itself changes smoothly.
    // Sonic Beam Lab: animation speed (multiplies the cadence) and limit (multiplies the ceiling)
    float want_rate = 1.0f;
    if (arow.speedRate) {
        const float speed_mul = f->animSpeed > 0 ? f->animSpeed : 1.0f, limit_mul = f->animLimit > 0 ? f->animLimit : 1.0f;
        const float top = std::max(arow.minRate, arow.maxRate * limit_mul);
        const float raw = s.speed_kmh / std::max(1.0f, arow.refKmh) * speed_mul;
        const float knee = arow.minRate + (top - arow.minRate) * 0.7f;
        want_rate = raw <= knee ? raw : knee + (top - knee) * (1 - std::exp(-(raw - knee) / std::max(0.01f, top - knee)));
        want_rate = std::max(arow.minRate, std::min(top, want_rate));
    }
    s.player.rate += (want_rate - s.player.rate) * std::min(1.0f, float(dt) * 8);
    s.player.time += dt * s.player.rate;
    if (s.player.prev >= 0) s.player.prev_time += dt * s.player.prev_rate;
    s.player.blend = std::min(1.0, s.player.blend + dt / s.player.blend_len);

    // ---------------- Sonic in the world
    const float spin = float(std::fmod(now * 3.0, 2 * kPi));
    if (f->onFoot) {
        // Drift clips already lean; only add our lean to the other states.
        const float target_lean = (drifting || f->water == 2) ? 0.0f : std::max(-1.0f, std::min(1.0f, (s.turn_rate / kDeg) / 90.0f)) * tuning::leanDegrees * kDeg;
        s.lean += (target_lean - s.lean) * std::min(1.0f, float(dt) * 8);
        const float k = tuning::sonicHeightMeters / d->sonic_height;
        const V3 face{std::cos(s.heading), std::sin(s.heading), 0}, upz{0, 0, 1};
        const V3 r = cross(face, upz);
        // swimming: cut at the water's surface (the renderer clips him there); his height is set below
        if (f->water == 2) {
            const float n[3] = {view.m[8], view.m[9], view.m[10]};   // world up (0, 0, 1) in view space: the view matrix's z row
            const V3 on{feet.x, feet.y, f->waterZ};
            const float q[3] = {dot(on, axes[0]) + view.m[12], dot(on, axes[1]) + view.m[13], dot(on, axes[2]) + view.m[14]};
            for (int a = 0; a < 3; ++a) scene.clip_plane[a] = n[a];
            scene.clip_plane[3] = -(n[0] * q[0] + n[1] * q[1] + n[2] * q[2]);
        }
        const V3 drawn_feet = feet;
        M4 sonic = mul(rot_z(-s.lean), basis(r * -k, upz * k, face * k, drawn_feet));
        // the jump ball: Sonic rolls forward around the middle of the ball, inside Frontiers' spin shell
        const bool rolling = ball_now && s.player.anim == int(Anim::ball);
        s.ball_amount += ((rolling ? 1.0f : 0.0f) - s.ball_amount) * std::min(1.0f, float(dt) * (rolling ? 14 : 10));
        if (rolling) s.ball_angle = float(std::fmod(s.ball_angle + dt * std::max(0.0f, f->ballSpin) * 2 * kPi, 2 * kPi));
        else s.ball_angle = 0;
        const V3 pivot = feet + upz * (tuning::sonicHeightMeters * 0.42f);
        if (rolling) sonic = mul(sonic, rot_around(r, s.ball_angle, pivot));
        copy(sonic, scene.sonic_model);
        if (s.ball_amount > 0.02f && d->ball_fx && !d->ball.nodes.empty()) {
            const V3 centre{(d->ball.min[0] + d->ball.max[0]) * 0.5f, (d->ball.min[1] + d->ball.max[1]) * 0.5f, (d->ball.min[2] + d->ball.max[2]) * 0.5f};
            M4 shell = mul(translate(centre * -1.0f), basis(r * -k, upz * k, face * k, pivot));
            shell = mul(shell, rot_around(r, s.ball_angle, pivot));
            copy(shell, scene.ball_model);
            scene.ball_pose.resize(size_t(d->ball_clip.nodes) * 12);
            sample(d->ball_clip, true, now, scene.ball_pose.data());
            scene.ball_alpha = s.ball_amount;
            scene.ball_visible = true;
            // like Frontiers, only the ball shows once it has formed: his shoes and quills poked through it
            scene.sonic_hidden = s.ball_amount > 0.5f;
        }
        // a soft, still, round blue glow (no texture and no spin: they made white dashes run round the ball)
        const float glow = std::max(0.0f, f->ballGlow) * s.ball_amount;
        if (glow > 0.01f) scene.sprites.push_back({{pivot.x, pivot.y, pivot.z}, 1.7f, std::min(1.0f, 0.5f * glow), 0, 4});
        scene.sonic_visible = true;
        const uint32_t n = d->clips[0].nodes * 12;
        scene.pose.resize(n);
        sample(d->clips[s.player.anim], animations[s.player.anim].loop, s.player.time, scene.pose.data());
        if (s.player.prev >= 0 && s.player.blend < 1) {
            s.scratch.resize(n);
            sample(d->clips[s.player.prev], animations[s.player.prev].loop, s.player.prev_time, s.scratch.data());
            const float w = float(s.player.blend);
            for (uint32_t i = 0; i < n; ++i) scene.pose[i] = s.scratch[i] + (scene.pose[i] - s.scratch[i]) * w;
        }
        // swimming: lift or sink him so the water line crosses his body at swimLine of its height
        // (whatever the pose), measured on his bones in this frame's pose
        if (f->water == 2 && !d->bone_rest.empty()) {
            float lo = 1e9f, hi = -1e9f;
            for (size_t b = 0; b * 3 + 2 < d->bone_rest.size() && b * 12 + 11 < scene.pose.size(); ++b) {
                const float *m = &scene.pose[b * 12], *rest = &d->bone_rest[b * 3];
                const float y = m[4] * rest[0] + m[5] * rest[1] + m[6] * rest[2] + m[7];   // model y is up
                lo = std::min(lo, y); hi = std::max(hi, y);
            }
            if (hi > lo) scene.sonic_model[14] = f->waterZ - k * (lo + (hi - lo) * tuning::swimLine);
        }
        // Shadow: Sonic flattened onto the ground under him along the sun (BeamNG's sky), on the
        // ground's slope, fading as he rises; a soft round one when the sun is low or at night.
        const float opacity = std::max(0.0f, std::min(1.0f, f->shadowOpacity));
        const V3 ground = f->hasGround ? v3(f->groundPos) : feet;
        V3 gn = f->hasGround ? norm(v3(f->groundNormal)) : V3{0, 0, 1};
        if (gn.z < 0.2f) gn = {0, 0, 1};
        const float height = std::max(0.0f, feet.z - ground.z);
        const float fade = std::max(0.0f, 1 - height / tuning::shadowFadeMeters);
        const V3 sun = norm(v3(f->toSun));
        const float sun_up = std::max(0.0f, std::min(1.0f, (sun.z - tuning::shadowSunMin) / 0.15f));
        if (sun_up > 0 && opacity > 0 && fade > 0) {
            // planar projection along the light: p' = p - L (n.p + dd) / (n.L), L = -sun
            const V3 L = sun * -1.0f;
            const float nl = dot(gn, L);
            const V3 on = ground + gn * tuning::shadowLift;
            const float dd = -dot(gn, on);
            M4 flat = identity();
            const float nn[3] = {gn.x, gn.y, gn.z}, ll[3] = {L.x, L.y, L.z};
            for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) flat.m[i * 4 + j] -= nn[i] * ll[j] / nl;
            for (int j = 0; j < 3; ++j) flat.m[12 + j] = -dd * ll[j] / nl;
            copy(mul(sonic, flat), scene.sonic_shadow_model);
            scene.sonic_shadow = true;
            scene.sonic_shadow_alpha = opacity * fade * sun_up;
        }
        const float soft = opacity * fade * (1 - sun_up) * 0.8f;
        if (soft > 0.01f) {
            scene.shadow = true;
            scene.shadow_alpha = soft;
            const float rr = 0.42f;
            const float c[4][2] = {{-rr, -rr}, {rr, -rr}, {rr, rr}, {-rr, rr}};
            for (int i = 0; i < 4; ++i) {
                scene.shadow_corners[i][0] = ground.x + c[i][0];
                scene.shadow_corners[i][1] = ground.y + c[i][1];
                scene.shadow_corners[i][2] = ground.z + 0.02f;
            }
        }
        if (s.first_shown < 0) s.first_shown = now;
    }

    // ---------------- Frontiers footsteps, on the ground BeamNG reports under Sonic (none while
    // boosting, like Frontiers: the boost has its own sound); a brake sound when he stops from speed
    {
        const int anim = s.player.anim;
        const bool stepping = !s.air && f->onFoot && (anim == int(Anim::walk) || anim == int(Anim::run)) &&
                              size_t(anim) < d->step_phases.size() && d->clips[anim].frames > 1;
        if (stepping) {
            const frontiers::Clip &clip = d->clips[anim];
            const double cycle = double(clip.frames - 1) / clip.rate;
            const double phase = std::fmod(s.player.time, cycle) / cycle;
            if (s.step_anim == anim) {
                for (float at : d->step_phases[anim]) {
                    const bool crossed = phase >= s.step_phase ? (at > s.step_phase && at <= phase) : (at > s.step_phase || at <= phase);
                    if (crossed) audio::play(anim == int(Anim::walk) ? kWalk[surface] : kRun[surface], 0.94f + 0.12f * rnd());
                }
            }
            s.step_phase = phase;
        }
        s.step_anim = stepping ? anim : -1;
        const bool wants = f->wantsMove != 0;
        if (s.wanted_move && !wants && !s.air && f->onFoot && s.speed_kmh > 50) audio::play(Sound::brake);
        s.wanted_move = wants;
    }

    // ---------------- Frontiers speed wind (sn_relative_wind): louder and higher with speed, pushed
    // by the boost
    {
        float want = 0;
        if (f->onFoot) {
            want = std::max(0.0f, std::min(1.0f, (s.speed_kmh - tuning::windStartKmh) / std::max(1.0f, tuning::windFullKmh - tuning::windStartKmh)));
            if (f->boost && s.speed_kmh > 30) want = std::max(want, tuning::windBoost);
        }
        s.wind += (want - s.wind) * std::min(1.0f, float(dt) * 3);
        audio::loop(Sound::wind, s.wind * s.wind, 0.8f + 0.6f * s.wind, f->windDb);
    }

    // ---------------- Power Boost trail (Frontiers' locus streaks) with lightning arcs along it and
    // bolts crackling around Sonic, while boosting; it recedes behind him when the boost ends
    {
        const float trail_len = std::max(0.1f, (f->trailLength > 0 ? f->trailLength : 1) * fx_speed);
        const float intensity = std::max(0.0f, f->trailIntensity) * fx_speed, lightning = std::max(0.0f, f->lightning) * fx_speed;
        const bool active = f->onFoot && f->boost && s.speed_kmh > 40;
        s.trail_on += ((active ? 1.0f : 0.0f) - s.trail_on) * std::min(1.0f, float(dt) * (active ? 10 : 3));
        const V3 travel = length(v3(f->vel)) > 1 ? norm(v3(f->vel)) : V3{std::cos(s.heading), std::sin(s.heading), 0};
        const V3 chest = feet + V3{0, 0, 0.55f} - travel * 0.35f;   // just behind Sonic's back
        const double keep = tuning::trailSeconds * trail_len;
        if (!f->onFoot) s.trail.clear();
        if (active && (s.trail.empty() || length(chest - s.trail.back().pos) > 0.25f)) s.trail.push_back({chest, now});
        if (!s.trail.empty() && length(chest - s.trail.back().pos) > 60) s.trail.clear();   // teleport
        s.trail.erase(std::remove_if(s.trail.begin(), s.trail.end(), [&](const State::TrailPoint &p) { return now - p.born > keep; }), s.trail.end());
        // the ribbon runs from Sonic's chest back along the points, facing the camera
        std::vector<State::TrailPoint> path;
        if (active) path.push_back({chest, now});
        for (auto it = s.trail.rbegin(); it != s.trail.rend(); ++it) path.push_back(*it);
        if (now >= s.arc_reroll) {   // lightning arcs re-strike a few times a second
            s.arc_reroll = now + 0.07;
            s.arc_column = int(rnd() * 4) & 3;
            s.arc_jitter.resize(64);
            for (auto &j : s.arc_jitter) j = {(rnd() - 0.5f) * 0.3f, (rnd() - 0.5f) * 0.3f, (rnd() - 0.5f) * 0.3f};
        }
        auto ribbon = [&](std::vector<float> &out, float width, float alpha, float u0, float u1, bool jitter, float v_scale, float v_speed, V3 color) {
            float along = 0;
            V3 prev_l{}, prev_r{}; float prev_v = 0, prev_a = 0; bool have = false;
            for (size_t i = 0; i < path.size(); ++i) {
                V3 p = path[i].pos;
                if (jitter && i > 0) p = p + s.arc_jitter[i % s.arc_jitter.size()];
                const V3 dir = i + 1 < path.size() ? path[i].pos - path[i + 1].pos : (i > 0 ? path[i - 1].pos - path[i].pos : fwd);
                if (i > 0) along += length(path[i - 1].pos - path[i].pos);
                const float age = float((now - path[i].born) / std::max(0.05, keep));
                // fades in over its first metres (Sonic Beam Lab: Trail start fade; 0 = no fade)
                const float head = f->trailFade > 0.001f ? std::min(1.0f, along / f->trailFade) : 1.0f;
                const float fade = std::pow(std::max(0.0f, 1 - age), 1.5f) * alpha * s.trail_on * head * head;
                // faces the camera, but turns flat (across the ground) where it points at the camera,
                // instead of spinning on edge into a thin line of changing width
                const V3 d = norm(dir);
                V3 facing = cross(d, norm(p - cam));
                const float edge = length(facing);
                V3 flat = cross(d, V3{0, 0, 1});
                if (length(flat) < 1e-3f) flat = right;
                flat = norm(flat);
                if (dot(facing, flat) < 0) flat = flat * -1.0f;
                const float k_face = std::min(1.0f, edge / 0.5f);
                V3 side = norm(norm(facing) * k_face + flat * (1 - k_face));
                side = side * (width * (0.15f + 0.85f * std::max(0.0f, 1 - age)) * 0.5f);
                const V3 l = p - side, r = p + side;
                const float v = along / v_scale - float(now) * v_speed;
                if (have) {
                    const float q[6][9] = {
                        {prev_l.x, prev_l.y, prev_l.z, u0, prev_v, color.x, color.y, color.z, prev_a}, {prev_r.x, prev_r.y, prev_r.z, u1, prev_v, color.x, color.y, color.z, prev_a},
                        {r.x, r.y, r.z, u1, v, color.x, color.y, color.z, fade},
                        {prev_l.x, prev_l.y, prev_l.z, u0, prev_v, color.x, color.y, color.z, prev_a}, {r.x, r.y, r.z, u1, v, color.x, color.y, color.z, fade},
                        {l.x, l.y, l.z, u0, v, color.x, color.y, color.z, fade}};
                    for (auto &vert : q) out.insert(out.end(), vert, vert + 9);
                }
                prev_l = l; prev_r = r; prev_v = v; prev_a = fade; have = true;
            }
        };
        if (s.trail_on > 0.01f && path.size() >= 2 && intensity > 0) {
            // electric-blue beam underneath (alpha blended, so it stays blue on bright ground),
            // Frontiers' locus streaks over it, and lightning arcs along it (both additive)
            ribbon(scene.beam, tuning::trailWidth * 1.3f, 0.75f * std::min(1.0f, intensity), 0, 1, false, 1.0f, 0, V3{0.08f, 0.38f, 1.0f});
            ribbon(scene.trail, tuning::trailWidth, intensity, 0, 1, false, 3.0f, 2.5f, V3{0.3f, 0.65f, 1.0f});
            if (lightning > 0) ribbon(scene.arcs, tuning::trailWidth * 0.6f, 0.9f * std::min(1.5f, lightning), s.arc_column / 4.0f, (s.arc_column + 1) / 4.0f, true, 2.0f, 0, V3{0.5f, 0.8f, 1.0f});
        }
        // bolts crackling on and around Sonic and along the trail
        if (active && lightning > 0) {
            s.bolt_debt += dt * tuning::boltsPerSecond * lightning;
            while (s.bolt_debt >= 1) {
                s.bolt_debt -= 1;
                const V3 at = (!s.trail.empty() && rnd() < 0.4f) ? s.trail[size_t(rnd() * s.trail.size()) % s.trail.size()].pos : chest;
                s.bolts.push_back({at + V3{(rnd() - 0.5f) * 0.8f, (rnd() - 0.5f) * 0.8f, (rnd() - 0.5f) * 0.9f}, {}, now, 0.08f + 0.07f * rnd(), 0.45f + 0.5f * rnd(), rnd() * 6.28f, 2, int(rnd() * 16) & 15});
            }
        } else s.bolt_debt = 0;
        // aura lightning: short arcs crackling on Sonic's arms, legs and quills while the aura is lit,
        // each one riding a bone so it moves with him (Sonic Beam Lab: Aura lightning)
        const float body_level = s.aura * fx_speed * std::max(0.0f, f->auraLightning) * (1 - s.ball_amount);
        if (f->onFoot && body_level > 0.02f && !d->bone_rest.empty()) {
            s.body_bolt_debt += dt * 26.0 * body_level;
            while (s.body_bolt_debt >= 1) {
                s.body_bolt_debt -= 1;
                const int node = int(rnd() * float(d->bone_rest.size() / 3)) % int(d->bone_rest.size() / 3);
                State::Bolt b{{(rnd() - 0.5f) * 0.16f, (rnd() - 0.5f) * 0.16f, (rnd() - 0.5f) * 0.16f}, {}, now, 0.06f + 0.08f * rnd(),
                              0.22f + 0.28f * rnd(), rnd() * 6.28f, 2, int(rnd() * 16) & 15};
                b.node = node;
                s.bolts.push_back(b);
            }
        } else s.body_bolt_debt = 0;
        s.bolts.erase(std::remove_if(s.bolts.begin(), s.bolts.end(), [&](const State::Bolt &b) { return now - b.born > b.life || !f->onFoot; }), s.bolts.end());
        for (auto &b : s.bolts) {
            const float age = float(now - b.born);
            if (age < 0) continue;
            const float k = age / b.life;
            V3 at = b.pos + b.vel * age;
            if (b.node >= 0) {   // on a bone of Sonic's current pose, then into the world
                if (!scene.sonic_visible || size_t(b.node) * 12 + 12 > scene.pose.size()) continue;
                const float *m = &scene.pose[size_t(b.node) * 12], *rest = &d->bone_rest[size_t(b.node) * 3];
                const float mx = m[0] * rest[0] + m[1] * rest[1] + m[2] * rest[2] + m[3];
                const float my = m[4] * rest[0] + m[5] * rest[1] + m[6] * rest[2] + m[7];
                const float mz = m[8] * rest[0] + m[9] * rest[1] + m[10] * rest[2] + m[11];
                const float *w = scene.sonic_model;
                at = V3{mx * w[0] + my * w[4] + mz * w[8] + w[12], mx * w[1] + my * w[5] + mz * w[9] + w[13], mx * w[2] + my * w[6] + mz * w[10] + w[14]} + b.pos;
            }
            Sprite sp{{at.x, at.y, at.z}, b.size, b.kind == 3 ? (1 - k) * (1 - k) : (1 - k), b.spin, b.kind};
            if (b.kind == 2) { const int c = b.cell & 15; sp.uv[0] = (c % 4) / 4.0f; sp.uv[1] = (c / 4) / 4.0f; sp.uv[2] = sp.uv[0] + 0.25f; sp.uv[3] = sp.uv[1] + 0.25f; }
            scene.sprites.push_back(sp);
        }
    }

    // ---------------- Frontiers boost effect: speed blur, aura, flash when a boost starts
    if (f->onFoot) {
        const float fast = std::max(0.0f, std::min(1.0f, (s.speed_kmh - tuning::blurStartKmh) / std::max(1.0f, tuning::blurFullKmh - tuning::blurStartKmh)));
        const double burst_age = now - s.burst_at;
        const float burst = burst_age < tuning::burstSeconds ? float(1 - burst_age / tuning::burstSeconds) : 0.0f;
        const float want_blur = std::min(1.0f, fast * (f->boost ? 1.0f : tuning::blurWithoutBoost) + burst * 0.6f);
        s.blur += (want_blur - s.blur) * std::min(1.0f, float(dt) * 6);
        const bool lit = boosting || (f->boost && s.speed_kmh > 30) || s.state == int(sheets::State::fast);
        const float rate = float(dt) / std::max(0.01f, lit ? tuning::auraFadeIn : tuning::auraFadeOut);
        s.aura = lit ? std::min(1.0f, s.aura + rate) : std::max(0.0f, s.aura - rate);
        scene.blur = s.blur * std::max(0.0f, f->blurStrength);
        scene.aura = s.aura * tuning::auraOpacity * fx_speed * std::max(0.0f, f->auraIntensity);
        scene.aura_thickness = std::max(0.0f, f->auraThickness);
        // boost aura shape (Sonic artwork): lying along the run, a round shield just ahead of his face,
        // streaming back past his feet and fading away; it points where he is really going (slopes and
        // jumps too), thickness sets its girth, length (and his speed) how far it streams back
        scene.aura_dome = f->auraBody == 0;
        if (scene.aura_dome) {
            const float H = tuning::sonicHeightMeters, size = 0.8f + 0.25f * scene.aura_thickness;
            const V3 vel = v3(f->vel);
            const V3 dir = length(vel) > 1.5f ? norm(vel) : V3{std::cos(s.heading), std::sin(s.heading), 0};
            const V3 upz{0, 0, 1};
            V3 up_axis = upz - dir * dot(upz, dir);
            up_axis = length(up_axis) > 1e-3f ? norm(up_axis) : V3{0, 1, 0};
            const V3 side = cross(dir, up_axis);
            const float radius = 0.62f * H * size, nose = 0.75f * radius;
            const V3 chest = feet + up_axis * (0.5f * H);
            const V3 base = chest + dir * (0.45f * H * size - nose);   // the nose's tip sits just ahead of his face
            copy(basis(side * radius, up_axis * radius, dir * nose, base), scene.dome_model);
            const float tail = (1.0f + 2.2f * fx_ratio) * H * std::max(0.0f, f->auraLength);
            scene.dome_tail = std::max(0.05f, tail / nose);
        }
        scene.aura *= 1 - s.ball_amount;   // the aura's bright rim streaked round the ball: the ball has its own glow
        // the aura's energy streams back faster as Sonic speeds up
        s.aura_clock = std::fmod(s.aura_clock + dt * (0.6 + 2.2 * fx_ratio), 1000.0);
        scene.aura_time = float(s.aura_clock);
        // streaks point at Sonic's chest on screen
        const V3 chest = feet + V3{0, 0, 0.6f};
        const float vx = dot(chest - cam, right), vy = dot(chest - cam, up), vz = dot(chest - cam, fwd);
        if (vz > 0.1f) {
            const float ndc_x = vx * xs / vz, ndc_y = vy * ys / vz;
            scene.blur_center[0] = std::max(0.25f, std::min(0.75f, ndc_x * 0.5f + 0.5f));
            scene.blur_center[1] = std::max(0.25f, std::min(0.75f, 0.5f - ndc_y * 0.5f));
        }
        if (burst > 0) {
            // follows Sonic a little, like Frontiers' boost_impact trailing him
            const V3 at = s.burst_pos + (origin - s.burst_pos) * 0.7f;
            const float grow = float(burst_age / tuning::burstSeconds);
            scene.sprites.push_back({{at.x, at.y, at.z}, 1.6f + 1.2f * grow, burst * burst, float(now * 2), 0});
            scene.sprites.push_back({{at.x, at.y, at.z}, 0.6f + tuning::burstMeters * grow, burst, float(-now * 1.5), 1});
        }
    } else {
        s.blur = s.aura = 0;
    }

    // ---------------- Frontiers rings placed by the mod's Lua on BeamNG's ground
    for (int i = 0; i < f->ringCount && f->ringPos; ++i) {
        const V3 p = v3(f->ringPos + 3 * i);
        Instance inst;
        // The ring model stands in X/Y (Frontiers Y up): turn it upright in BeamNG's Z-up world.
        copy(mul(mul(scale(1.5f), rot_y(spin)), basis({1, 0, 0}, {0, 0, 1}, {0, -1, 0}, p)), inst.model);
        scene.rings.push_back(inst);
    }
    for (auto &p : s.popups) {
        const float age = float(now - p.born);
        const V3 at = p.pos + p.vel * age + V3{0, 0, -6.0f * age * age};
        Instance inst;
        copy(mul(mul(scale(0.7f), rot_y(spin * 2)), basis({1, 0, 0}, {0, 0, 1}, {0, -1, 0}, at)), inst.model);
        inst.alpha = std::max(0.0f, 1 - age / 0.9f);
        scene.rings.push_back(inst);
    }
    s.popups.erase(std::remove_if(s.popups.begin(), s.popups.end(), [&](const Popup &p) { return now - p.born > 0.9; }), s.popups.end());

    // ---------------- HUD (Frontiers-style ring counter with the real ring as its icon)
    if (!f->hudHidden) {
        const bool rings_on = tuning::ringsEnabled != 0;
        if (rings_on) {
            const float icon = 0.055f / 0.296f;
            Instance hud;
            hud.screen = true;
            copy(mul(mul(scale3(icon / std::max(0.5f, aspect), icon, 0.05f), rot_y(spin)), basis({1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {-0.90f, 0.82f, 0.5f})), hud.model);
            scene.rings.push_back(hud);
            char count[32];
            std::snprintf(count, sizeof(count), "%03d", s.rings);
            add_text(scene, -0.84f, 0.855f, 0.07f, 0, 0, 0, 0.6f, count);
            add_text(scene, -0.845f, 0.86f, 0.07f, 1.0f, 0.86f, 0.15f, 1, count);
        }
        if (f->showSpeed) {
            // Speedometer: under the ring counter, or in its place while rings are off
            char speed_text[32];
            std::snprintf(speed_text, sizeof(speed_text), "%.0f KM/H", std::min(s.shown_kmh, 99999999.0f));
            const float y = rings_on ? 0.71f : 0.86f, size = rings_on ? 0.045f : 0.06f;
            add_text(scene, -0.935f, y - 0.005f, size, 0, 0, 0, 0.6f, speed_text);
            add_text(scene, -0.94f, y, size, 0.55f, 0.85f, 1.0f, 1, speed_text);
        }
        if (rings_on && now - s.plus_time < 1.2) {
            char plus[32];
            std::snprintf(plus, sizeof(plus), "+%d", s.plus);
            add_text(scene, -0.70f, 0.86f, 0.05f, 1, 1, 1, float(1 - (now - s.plus_time) / 1.2), plus);
        }
        if (s.first_shown >= 0 && now - s.first_shown < 25) {
            const float a = float(std::min(1.0, (25 - (now - s.first_shown)) / 2));
            add_text(scene, -0.62f, 0.70f, 0.036f, 1, 1, 1, a, "SONIC BEAM - YOU ARE SONIC: WASD RUN, SHIFT BOOST (NO LIMIT), SPACE JUMP");
            add_text(scene, -0.62f, 0.64f, 0.030f, 0.85f, 0.9f, 1, a, rings_on ? "MOUSE: CAMERA   F: CAR IN/OUT   O: SPEED MENU   RUN THROUGH THE RINGS"
                                                                              : "MOUSE: CAMERA   F: CAR IN/OUT   O: SPEED MENU");
        }
    }
    static double next_state = 0;
    if (now >= next_state) {
        next_state = now + 0.25;
        char json[384];
        std::snprintf(json, sizeof(json),
                      "{\"state\":\"%s\",\"speed\":%.1f,\"air\":%d,\"airTime\":%.2f,\"boost\":%d,\"rings\":%d,\"ringsAhead\":%d,\"onFoot\":%d,\"x\":%.1f,\"y\":%.1f,\"z\":%.1f}",
                      states[s.state].id, s.speed_kmh, int(s.air), s.air_time, f->boost, s.rings, f->ringCount, f->onFoot, feet.x, feet.y, feet.z);
        testmode::write_state(json);
    }
}

} // namespace game
