// SPDX-License-Identifier: MIT
// Off-game bench for Sonic Beam's renderer: plays BeamNG's part (a D3D11 swap chain and the
// frames the mod's Lua would send: camera behind Sonic, run, boost, jump, rings) and saves BMPs.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

struct SbFrame {
    float camPos[3], camFwd[3], camUp[3];
    float fovY, dt;
    float feet[3], vel[3];
    int32_t onFoot, grounded, boost, jumped, picked, hudHidden, ringCount;
    const float *ringPos;
};

static void save_bmp(const wchar_t *path, const uint8_t *bgra, UINT w, UINT h, UINT pitch)
{
    const UINT row = (w * 3 + 3) & ~3u;
    BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
    fh.bfType = 0x4d42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + row * h;
    ih.biSize = sizeof(ih); ih.biWidth = LONG(w); ih.biHeight = LONG(h); ih.biPlanes = 1; ih.biBitCount = 24;
    FILE *f = _wfopen(path, L"wb");
    if (!f) return;
    fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f);
    std::vector<uint8_t> line(row);
    for (UINT y = 0; y < h; ++y) {
        const uint8_t *src = bgra + size_t(h - 1 - y) * pitch;
        for (UINT x = 0; x < w; ++x) { line[x * 3] = src[x * 4 + 2]; line[x * 3 + 1] = src[x * 4 + 1]; line[x * 3 + 2] = src[x * 4]; }
        fwrite(line.data(), 1, row, f);
    }
    fclose(f);
}

int wmain(int argc, wchar_t **argv)
{
    const std::wstring out = argc > 1 ? argv[1] : L".";
    HMODULE dll = LoadLibraryW(L"sonic_beamng.dll");
    if (!dll) { std::printf("cannot load sonic_beamng.dll\n"); return 1; }
    auto sb_init = reinterpret_cast<int (*)(const char *)>(GetProcAddress(dll, "sb_init"));
    auto sb_frame = reinterpret_cast<void (*)(const SbFrame *)>(GetProcAddress(dll, "sb_frame"));
    auto sb_status = reinterpret_cast<const char *(*)()>(GetProcAddress(dll, "sb_status"));
    if (!sb_init || !sb_frame || !sb_init("bench")) { std::printf("sb_init failed\n"); return 1; }

    WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"SonicBeamBench";
    RegisterClassW(&wc);
    const UINT W = 1280, H = 720;
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Sonic Beam bench", WS_OVERLAPPEDWINDOW, 60, 60, W + 16, H + 39, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2; sd.BufferDesc.Width = W; sd.BufferDesc.Height = H; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_SEQUENTIAL; sd.BufferCount = 1;   // keeps the presented image for read-back
    IDXGISwapChain *chain = nullptr; ID3D11Device *device = nullptr; ID3D11DeviceContext *ctx = nullptr;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &chain, &device, nullptr, &ctx))) {
        std::printf("no D3D11 device\n"); return 1;
    }
    ID3D11Texture2D *back = nullptr; chain->GetBuffer(0, IID_PPV_ARGS(&back));
    ID3D11RenderTargetView *rtv = nullptr; device->CreateRenderTargetView(back, nullptr, &rtv);
    D3D11_TEXTURE2D_DESC td{}; back->GetDesc(&td); back->Release();
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
    ID3D11Texture2D *staging = nullptr; device->CreateTexture2D(&td, nullptr, &staging);

    struct Shot { double at; const wchar_t *name; } shots[] = {{3.0, L"1-idle"}, {7.0, L"2-run"}, {11.0, L"3-boost"}, {13.4, L"4-drift"}, {15.6, L"5-air"}, {19.5, L"6-rings"}};
    size_t next_shot = 0;
    double x = 0, y = 0, z = 100, heading = 1.5708, speed = 0, vz = 0, cam_yaw = 1.5708;
    std::vector<float> rings;
    for (int i = 0; i < 8; ++i) { rings.push_back(0.0f); rings.push_back(float(30 + i * 5)); rings.push_back(100.9f); }
    LARGE_INTEGER freq, t0, t; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&t0);
    double last = 0;
    for (int frame = 0;; ++frame) {
        MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        QueryPerformanceCounter(&t);
        const double now = double(t.QuadPart - t0.QuadPart) / freq.QuadPart, dt = std::max(1e-4, now - last); last = now;
        if (now > 21) break;
        const double target = now < 4 ? 0 : now < 9 ? 80 : now < 16 ? 190 : 85;   // km/h
        const bool boost = now >= 9 && now < 16;
        speed += (target / 3.6 - speed) * std::min(1.0, dt * 2);
        if (now >= 12 && now < 14) heading += dt * 1.6;                              // hard boosted turn
        int jumped = 0;
        if (now >= 14.6 && vz == 0 && z <= 100) { vz = 9; jumped = 1; }
        if (vz != 0 || z > 100) { vz -= 15 * dt; z += vz * dt; if (z <= 100) { z = 100; vz = 0; } }
        x += std::cos(heading) * speed * dt; y += std::sin(heading) * speed * dt;
        cam_yaw += (heading - cam_yaw) * std::min(1.0, dt * 2.5);
        // Camera 4.2 m behind and above Sonic, looking slightly down at his chest.
        const float cx = float(x - std::cos(cam_yaw) * 3.3), cy = float(y - std::sin(cam_yaw) * 3.3), cz = float(z + 1.4);
        float fx = float(x) - cx, fy = float(y) - cy, fz = float(z + 0.9) - cz;
        const float fl = std::sqrt(fx * fx + fy * fy + fz * fz); fx /= fl; fy /= fl; fz /= fl;
        // In the rings phase put a line in front of the run.
        if (now > 16.5 && rings.size() == 24 && rings[0] == 0.0f) {
            for (int i = 0; i < 8; ++i) {
                rings[i * 3] = float(x + std::cos(heading) * (15 + i * 5));
                rings[i * 3 + 1] = float(y + std::sin(heading) * (15 + i * 5));
                rings[i * 3 + 2] = 100.9f;
            }
        }
        int picked = 0;
        for (size_t i = 0; i < rings.size(); i += 3) {
            const double dx = rings[i] - x, dy = rings[i + 1] - y;
            if (rings[i] != 0.0f && dx * dx + dy * dy < 4) { rings.erase(rings.begin() + i, rings.begin() + i + 3); i -= 3; ++picked; }
        }
        SbFrame f{};
        f.camPos[0] = cx; f.camPos[1] = cy; f.camPos[2] = cz;
        f.camFwd[0] = fx; f.camFwd[1] = fy; f.camFwd[2] = fz;
        f.camUp[0] = 0; f.camUp[1] = 0; f.camUp[2] = 1;
        f.fovY = 70 * 3.14159f / 180; f.dt = float(dt);
        f.feet[0] = float(x); f.feet[1] = float(y); f.feet[2] = float(z);
        f.vel[0] = float(std::cos(heading) * speed); f.vel[1] = float(std::sin(heading) * speed); f.vel[2] = float(vz);
        f.onFoot = 1; f.grounded = z <= 100.01; f.boost = boost; f.jumped = jumped; f.picked = picked;
        f.ringCount = int(rings.size() / 3); f.ringPos = rings.data();
        sb_frame(&f);
        const float sky[4] = {0.45f, 0.62f, 0.85f, 1};
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->ClearRenderTargetView(rtv, sky);
        chain->Present(1, 0);
        if (next_shot < sizeof(shots) / sizeof(shots[0]) && now >= shots[next_shot].at) {
            ID3D11Texture2D *bb = nullptr; chain->GetBuffer(0, IID_PPV_ARGS(&bb));
            ctx->CopyResource(staging, bb);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
                std::vector<uint8_t> bgra(size_t(W) * H * 4);
                for (UINT yy = 0; yy < H; ++yy) for (UINT xx = 0; xx < W; ++xx) {
                    const uint8_t *p = static_cast<uint8_t *>(m.pData) + size_t(yy) * m.RowPitch + xx * 4;
                    uint8_t *o = &bgra[(size_t(yy) * W + xx) * 4];
                    o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = 255;   // RGBA, as save_bmp expects
                }
                ctx->Unmap(staging, 0);
                save_bmp((out + L"\\" + shots[next_shot].name + L".bmp").c_str(), bgra.data(), W, H, W * 4);
            }
            bb->Release();
            std::printf("snapshot %ls at %.1fs (status %s)\n", shots[next_shot].name, now, sb_status ? sb_status() : "?");
            ++next_shot;
        }
    }
    std::printf("bench done\n");
    return 0;
}
