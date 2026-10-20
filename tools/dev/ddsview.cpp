// SPDX-License-Identifier: MIT
// Developer tool: decode one texture from a Frontiers PAC archive with the GPU (BC7 and the
// other block formats Windows cannot open) and save it as a BMP: colour on the left, alpha on
// the right. Reads the player's install; the image goes where the developer asks (scratch only).
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <string>
#include <vector>
#include "../../src/frontiers/pac.hpp"
#include "../../src/frontiers/model.hpp"

static const char kShader[] = R"(
Texture2D t : register(t0); SamplerState s : register(s0);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) { O o; float2 uv = float2((id << 1) & 2, id & 2); o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }
float4 ps(O i) : SV_Target { return t.SampleLevel(s, i.uv, 0); }
)";

int wmain(int argc, wchar_t **argv)
{
    if (argc < 4) { std::fprintf(stderr, "usage: ddsview <file.pac> <entry.dds> <out.bmp>\n"); return 2; }
    frontiers::Pac pac;
    std::string error, entry;
    for (wchar_t *c = argv[2]; *c; ++c) entry += char(*c);
    if (!pac.load(argv[1], error)) { std::fprintf(stderr, "pac: %s\n", error.c_str()); return 1; }
    const frontiers::PacEntry *e = pac.find(entry);
    frontiers::Texture tex;
    if (!e || !frontiers::parse_dds(*e, tex, error)) { std::fprintf(stderr, "dds: %s\n", e ? error.c_str() : "missing"); return 1; }
    std::printf("%s: %ux%u dxgi %u, %u mips\n", entry.c_str(), tex.width, tex.height, tex.dxgi, tex.mips);

    ID3D11Device *dev = nullptr; ID3D11DeviceContext *ctx = nullptr;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) return 1;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = tex.width; td.Height = tex.height; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT(tex.dxgi);
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{tex.data.data() + tex.mip_offset[0], tex.compressed ? ((tex.width + 3) / 4) * tex.block_bytes : tex.width * 4, 0};
    ID3D11Texture2D *src = nullptr; ID3D11ShaderResourceView *srv = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, &init, &src)) || FAILED(dev->CreateShaderResourceView(src, nullptr, &srv))) { std::fprintf(stderr, "texture\n"); return 1; }
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *rt = nullptr; ID3D11RenderTargetView *rtv = nullptr;
    dev->CreateTexture2D(&td, nullptr, &rt); dev->CreateRenderTargetView(rt, nullptr, &rtv);
    ID3DBlob *vsb = nullptr, *psb = nullptr;
    D3DCompile(kShader, sizeof(kShader) - 1, "v", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsb, nullptr);
    D3DCompile(kShader, sizeof(kShader) - 1, "p", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psb, nullptr);
    ID3D11VertexShader *vs = nullptr; ID3D11PixelShader *ps = nullptr;
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
    D3D11_SAMPLER_DESC sd{}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = 0;
    ID3D11SamplerState *smp = nullptr; dev->CreateSamplerState(&sd, &smp);
    D3D11_VIEWPORT vp{0, 0, float(tex.width), float(tex.height), 0, 1};
    ctx->RSSetViewports(1, &vp); ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs, nullptr, 0); ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &srv); ctx->PSSetSamplers(0, 1, &smp);
    ctx->Draw(3, 0);
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *stage = nullptr; dev->CreateTexture2D(&td, nullptr, &stage);
    ctx->CopyResource(stage, rt);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m))) return 1;

    // BMP: colour (left) and alpha as grey (right), bottom-up rows
    const uint32_t w = tex.width * 2, h = tex.height, row = w * 3 + (4 - (w * 3) % 4) % 4;
    std::vector<uint8_t> bmp(54 + size_t(row) * h, 0);
    auto put32 = [&](size_t at, uint32_t v) { for (int k = 0; k < 4; ++k) bmp[at + k] = uint8_t(v >> (8 * k)); };
    bmp[0] = 'B'; bmp[1] = 'M'; put32(2, uint32_t(bmp.size())); put32(10, 54); put32(14, 40); put32(18, w); put32(22, h);
    bmp[26] = 1; bmp[28] = 24; put32(34, uint32_t(row) * h);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t *s = static_cast<const uint8_t *>(m.pData) + size_t(y) * m.RowPitch;
        uint8_t *d = bmp.data() + 54 + size_t(h - 1 - y) * row;
        for (uint32_t x = 0; x < tex.width; ++x) {
            const uint8_t *p = s + x * 4;
            d[x * 3] = p[2]; d[x * 3 + 1] = p[1]; d[x * 3 + 2] = p[0];
            uint8_t *a = d + (tex.width + x) * 3; a[0] = a[1] = a[2] = p[3];
        }
    }
    FILE *f = _wfopen(argv[3], L"wb");
    if (!f) return 1;
    std::fwrite(bmp.data(), 1, bmp.size(), f); std::fclose(f);
    std::printf("saved colour + alpha\n");
    return 0;
}
