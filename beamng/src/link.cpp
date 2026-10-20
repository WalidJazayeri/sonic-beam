// SPDX-License-Identifier: MIT
// The mod's Lua talks to the renderer over a loopback UDP socket (BeamNG's Lua has LuaSocket but
// no native calls). Each packet is one frame: camera, Sonic's body, events and the rings; the
// renderer answers with whether a Melty test is running, so the Lua can use the test layout.
#include "common.hpp"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <cstring>
#include <vector>
#include "testmode.hpp"
#include "game.hpp"

extern "C" void sb_frame(const SbFrame *frame);   // render11.cpp
void hook_graphics();                             // render11.cpp

namespace {

constexpr uint16_t kPort = 47613;
constexpr uint32_t kFrameMagic = 0x31464253;     // "SBF1"
constexpr uint32_t kStatusMagic = 0x31534253;    // "SBS1"

#pragma pack(push, 4)
struct Packet {
    uint32_t magic;
    float camPos[3], camFwd[3], camUp[3];
    float fovY, dt;
    float feet[3], vel[3];
    int32_t onFoot, grounded, boost, jumped, picked, hudHidden, showSpeed, surface, wantsMove;
    float stepsDb, soundDb, animSpeed, animLimit, windDb, boostBoomKmh;
    float toSun[3], groundPos[3], groundNormal[3], shadowOpacity;
    int32_t hasGround;
    int32_t ringCount;
    // followed by ringCount * 3 floats
};
struct Status { uint32_t magic; int32_t testActive, rings; };
#pragma pack(pop)

DWORD WINAPI listener(void *)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { mod::log("link: no sockets"); return 0; }
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET; addr.sin_port = htons(kPort); addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        mod::log("link: cannot listen on 127.0.0.1:%u (error %d)", kPort, WSAGetLastError());
        return 0;
    }
    mod::log("link: listening for the Sonic Beam script on 127.0.0.1:%u", kPort);
    std::vector<char> buffer(65536);
    std::vector<float> rings;
    bool first = true;
    for (;;) {
        sockaddr_in from{};
        int from_len = sizeof(from);
        const int n = recvfrom(s, buffer.data(), int(buffer.size()), 0, reinterpret_cast<sockaddr *>(&from), &from_len);
        if (n < int(sizeof(Packet))) continue;
        Packet p;
        std::memcpy(&p, buffer.data(), sizeof(p));
        if (p.magic != kFrameMagic) continue;
        const int count = std::max(0, std::min(p.ringCount, (n - int(sizeof(Packet))) / 12));
        rings.assign(count * 3, 0.0f);
        if (count) std::memcpy(rings.data(), buffer.data() + sizeof(Packet), size_t(count) * 12);
        SbFrame f{};
        std::memcpy(f.camPos, p.camPos, 12); std::memcpy(f.camFwd, p.camFwd, 12); std::memcpy(f.camUp, p.camUp, 12);
        f.fovY = p.fovY; f.dt = p.dt;
        std::memcpy(f.feet, p.feet, 12); std::memcpy(f.vel, p.vel, 12);
        f.onFoot = p.onFoot; f.grounded = p.grounded; f.boost = p.boost; f.jumped = p.jumped; f.picked = p.picked;
        f.hudHidden = p.hudHidden; f.showSpeed = p.showSpeed; f.surface = p.surface; f.wantsMove = p.wantsMove; f.stepsDb = p.stepsDb; f.soundDb = p.soundDb; f.animSpeed = p.animSpeed; f.animLimit = p.animLimit; f.windDb = p.windDb; f.boostBoomKmh = p.boostBoomKmh;
        std::memcpy(f.toSun, p.toSun, 12); std::memcpy(f.groundPos, p.groundPos, 12); std::memcpy(f.groundNormal, p.groundNormal, 12);
        f.shadowOpacity = p.shadowOpacity; f.hasGround = p.hasGround; f.ringCount = count; f.ringPos = rings.data();
        sb_frame(&f);
        if (first) { first = false; mod::log("link: first frame from the Sonic Beam script"); hook_graphics(); }
        const Status reply{kStatusMagic, testmode::active() ? 1 : 0, game::ring_total()};
        sendto(s, reinterpret_cast<const char *>(&reply), sizeof(reply), 0, reinterpret_cast<sockaddr *>(&from), from_len);
    }
}
} // namespace

void start_link()
{
    if (HANDLE t = CreateThread(nullptr, 0, listener, nullptr, 0, nullptr)) CloseHandle(t);
}
