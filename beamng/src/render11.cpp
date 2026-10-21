// SPDX-License-Identifier: MIT
// Sonic Beam's renderer: the mod's Lua loads this DLL through LuaJIT FFI and feeds it a frame of
// BeamNG state (camera, Sonic's body, rings) every update. Present on BeamNG's D3D11 swap chain is
// hooked through the DXGI swap-chain method table; Sonic (GPU-skinned with his Frontiers clips),
// the rings and the ring counter are drawn into the finished frame with an isolated device-context
// state, so BeamNG's own pipeline state is never touched.
// build.ps1 passes the number from beamng/VERSION (e.g. 0.1.4); a bare 0.1.4 is one preprocessor token
#define SB_STRINGIFY2(x) #x
#define SB_STRINGIFY(x) SB_STRINGIFY2(x)
#ifdef SB_VERSION_NUMBER
#define SB_VERSION SB_STRINGIFY(SB_VERSION_NUMBER)
#else
#define SB_VERSION "dev"
#endif
#include "common.hpp"
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <cmath>
#include <cstring>
#include <vector>
#include "assets.hpp"
#include "audio.hpp"
#include "font.hpp"
#include "game.hpp"
#include "testmode.hpp"
#include "render12.hpp"
#include "shared_draw.hpp"

namespace {

int backend = 0;   // 0 unknown yet, 11 Direct3D 11, 12 Direct3D 12
int attach_tries = 0;

template <class T> void release(T *&p) { if (p) { p->Release(); p = nullptr; } }


using draw::DrawConstants;
using draw::FlatVertex;

struct GpuMesh {
    ID3D11Buffer *vb = nullptr, *ib = nullptr;
    std::vector<frontiers::Mesh> meshes;
    int texture_base = 0;
};

struct Renderer {
    ID3D11Device *device = nullptr;
    ID3D11Device1 *device1 = nullptr;
    ID3D11DeviceContext1 *context = nullptr;
    ID3DDeviceContextState *state = nullptr;
    IUnknown *chain = nullptr;                     // identity of the swap chain we draw into
    ID3D11RenderTargetView *rtv = nullptr;
    ID3D11Texture2D *depth = nullptr;
    ID3D11DepthStencilView *dsv = nullptr;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    int color_mode = 0;                            // 0 UNORM, 1 scRGB, 2 sRGB
    ID3D11VertexShader *vs_mesh = nullptr, *vs_flat = nullptr, *vs_aura = nullptr, *vs_post = nullptr;
    ID3D11PixelShader *ps_mesh = nullptr, *ps_flat = nullptr, *ps_aura = nullptr, *ps_post = nullptr, *ps_shadow = nullptr;
    ID3D11InputLayout *layout_mesh = nullptr, *layout_flat = nullptr;
    ID3D11Buffer *cb_draw = nullptr, *cb_bones = nullptr, *flat_vb = nullptr;
    UINT flat_capacity = 0;
    ID3D11SamplerState *samplers[3] = {};
    ID3D11BlendState *blend = nullptr, *blend_add = nullptr, *blend_off = nullptr;
    ID3D11DepthStencilState *depth_on = nullptr, *depth_off = nullptr, *depth_test = nullptr, *stencil_once = nullptr;
    ID3D11RasterizerState *raster = nullptr;
    std::vector<ID3D11ShaderResourceView *> textures;
    int font_texture = 0, white_texture = 0;
    GpuMesh sonic, ring, aura, ball, dome;
    bool has_aura = false, has_ball = false, has_dome = false;
    int aura_texture = -1, glow_texture = -1, wave_texture = -1;
    int trail_texture = -1, bolt_texture = -1, arc_texture = -1, orb_texture = -1;
    ID3D11Texture2D *copy = nullptr;               // copy of BeamNG's frame for the speed blur
    ID3D11ShaderResourceView *copy_srv = nullptr;
    UINT copy_w = 0, copy_h = 0;
    bool ready = false, disabled = false, uploaded = false;
    uint64_t frames_drawn = 0;
    game::Scene scene;
};

Renderer r;
SRWLOCK frame_lock = SRWLOCK_INIT;
SbFrame latest{};
std::vector<float> latest_rings;
bool have_frame = false;
int pending_picks = 0, pending_jumps = 0;   // events accumulate until a drawn frame consumes them
volatile LONG hooked = 0;
char status[160] = "starting";

using PresentFn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT);
using ResizeFn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
PresentFn real_present = nullptr;
ResizeFn real_resize = nullptr;
thread_local int present_depth = 0;

void set_status(const char *text) { std::snprintf(status, sizeof(status), "%s", text); }

bool create_pipeline()
{
    ID3DBlob *code[9] = {}, *errors = nullptr;
    const char *entries[9] = {"vsMesh", "psMesh", "vsFlat", "psFlat", "vsAura", "psAura", "vsPost", "psPost", "psShadow"};
    const char *targets[9] = {"vs_5_0", "ps_5_0", "vs_5_0", "ps_5_0", "vs_5_0", "ps_5_0", "vs_5_0", "ps_5_0", "ps_5_0"};
    for (int i = 0; i < 9; ++i) {
        if (FAILED(D3DCompile(draw::kShader, sizeof(draw::kShader) - 1, "sonic_beam", nullptr, nullptr, entries[i], targets[i], D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code[i], &errors))) {
            if (errors) mod::log("render: shader %s: %s", entries[i], static_cast<const char *>(errors->GetBufferPointer()));
            release(errors);
            for (auto *c : code) if (c) c->Release();
            return false;
        }
        release(errors);
    }
    bool ok = SUCCEEDED(r.device->CreateVertexShader(code[0]->GetBufferPointer(), code[0]->GetBufferSize(), nullptr, &r.vs_mesh)) &&
              SUCCEEDED(r.device->CreatePixelShader(code[1]->GetBufferPointer(), code[1]->GetBufferSize(), nullptr, &r.ps_mesh)) &&
              SUCCEEDED(r.device->CreateVertexShader(code[2]->GetBufferPointer(), code[2]->GetBufferSize(), nullptr, &r.vs_flat)) &&
              SUCCEEDED(r.device->CreatePixelShader(code[3]->GetBufferPointer(), code[3]->GetBufferSize(), nullptr, &r.ps_flat)) &&
              SUCCEEDED(r.device->CreateVertexShader(code[4]->GetBufferPointer(), code[4]->GetBufferSize(), nullptr, &r.vs_aura)) &&
              SUCCEEDED(r.device->CreatePixelShader(code[5]->GetBufferPointer(), code[5]->GetBufferSize(), nullptr, &r.ps_aura)) &&
              SUCCEEDED(r.device->CreateVertexShader(code[6]->GetBufferPointer(), code[6]->GetBufferSize(), nullptr, &r.vs_post)) &&
              SUCCEEDED(r.device->CreatePixelShader(code[7]->GetBufferPointer(), code[7]->GetBufferSize(), nullptr, &r.ps_post)) &&
              SUCCEEDED(r.device->CreatePixelShader(code[8]->GetBufferPointer(), code[8]->GetBufferSize(), nullptr, &r.ps_shadow));
    const D3D11_INPUT_ELEMENT_DESC mesh_input[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BLENDWEIGHT", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    const D3D11_INPUT_ELEMENT_DESC flat_input[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    ok = ok && SUCCEEDED(r.device->CreateInputLayout(mesh_input, 5, code[0]->GetBufferPointer(), code[0]->GetBufferSize(), &r.layout_mesh)) &&
         SUCCEEDED(r.device->CreateInputLayout(flat_input, 3, code[2]->GetBufferPointer(), code[2]->GetBufferSize(), &r.layout_flat));
    for (auto *c : code) if (c) c->Release();
    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = sizeof(DrawConstants);
    ok = ok && SUCCEEDED(r.device->CreateBuffer(&bd, nullptr, &r.cb_draw));
    bd.ByteWidth = 768 * 16;
    ok = ok && SUCCEEDED(r.device->CreateBuffer(&bd, nullptr, &r.cb_bones));
    for (int i = 0; i < 3; ++i) {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = i == 2 ? D3D11_FILTER_MIN_MAG_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = i == 0 ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX; sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        ok = ok && SUCCEEDED(r.device->CreateSamplerState(&sd, &r.samplers[i]));
    }
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO; blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    ok = ok && SUCCEEDED(r.device->CreateBlendState(&blend, &r.blend));
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;              // boost glow: additive
    ok = ok && SUCCEEDED(r.device->CreateBlendState(&blend, &r.blend_add));
    blend.RenderTarget[0].BlendEnable = FALSE;                      // speed blur: replaces the frame
    ok = ok && SUCCEEDED(r.device->CreateBlendState(&blend, &r.blend_off));
    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = TRUE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; ds.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;
    ok = ok && SUCCEEDED(r.device->CreateDepthStencilState(&ds, &r.depth_on));
    ds.DepthEnable = FALSE; ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    ok = ok && SUCCEEDED(r.device->CreateDepthStencilState(&ds, &r.depth_off));
    ds.DepthEnable = TRUE;                                          // the aura hides behind Sonic, writes no depth
    ok = ok && SUCCEEDED(r.device->CreateDepthStencilState(&ds, &r.depth_test));
    // Sonic's projected shadow: no depth, each pixel darkened once (stencil 0 -> 1)
    ds.DepthEnable = FALSE; ds.StencilEnable = TRUE; ds.StencilReadMask = 0xff; ds.StencilWriteMask = 0xff;
    ds.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_INCR_SAT, D3D11_COMPARISON_EQUAL};
    ds.BackFace = ds.FrontFace;
    ok = ok && SUCCEEDED(r.device->CreateDepthStencilState(&ds, &r.stencil_once));
    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_NONE; rs.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED(r.device->CreateRasterizerState(&rs, &r.raster));
    return ok;
}

ID3D11ShaderResourceView *make_texture(UINT w, UINT h, UINT mips, DXGI_FORMAT format, const std::vector<D3D11_SUBRESOURCE_DATA> &init)
{
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = mips; td.ArraySize = 1; td.Format = format; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *tex = nullptr;
    ID3D11ShaderResourceView *srv = nullptr;
    if (SUCCEEDED(r.device->CreateTexture2D(&td, init.data(), &tex))) r.device->CreateShaderResourceView(tex, nullptr, &srv);
    release(tex);
    return srv;
}

bool upload_mesh(const frontiers::Model &m, GpuMesh &g)
{
    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.ByteWidth = UINT(m.vertices.size() * sizeof(frontiers::Vertex));
    D3D11_SUBRESOURCE_DATA init{m.vertices.data(), 0, 0};
    if (FAILED(r.device->CreateBuffer(&bd, &init, &g.vb))) return false;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER; bd.ByteWidth = UINT(m.indices.size() * 2);
    init.pSysMem = m.indices.data();
    if (FAILED(r.device->CreateBuffer(&bd, &init, &g.ib))) return false;
    g.meshes = m.meshes;
    g.texture_base = int(r.textures.size());
    for (auto &t : m.textures) {
        std::vector<D3D11_SUBRESOURCE_DATA> mips;
        for (UINT i = 0; i < t.mips; ++i) {
            const UINT w = std::max(1u, t.width >> i);
            D3D11_SUBRESOURCE_DATA sd{};
            sd.pSysMem = t.data.data() + t.mip_offset[i];
            sd.SysMemPitch = t.compressed ? ((w + 3) / 4) * t.block_bytes : w * 4;
            mips.push_back(sd);
        }
        ID3D11ShaderResourceView *srv = make_texture(t.width, t.height, t.mips, DXGI_FORMAT(t.dxgi), mips);
        if (!srv) { mod::log("render: texture format %u not supported", t.dxgi); return false; }
        r.textures.push_back(srv);
    }
    return true;
}

ID3D11ShaderResourceView *texture_view(const frontiers::Texture &t)
{
    std::vector<D3D11_SUBRESOURCE_DATA> mips;
    for (UINT i = 0; i < t.mips; ++i) {
        const UINT w = std::max(1u, t.width >> i);
        D3D11_SUBRESOURCE_DATA sd{};
        sd.pSysMem = t.data.data() + t.mip_offset[i];
        sd.SysMemPitch = t.compressed ? ((w + 3) / 4) * t.block_bytes : w * 4;
        mips.push_back(sd);
    }
    return make_texture(t.width, t.height, t.mips, DXGI_FORMAT(t.dxgi), mips);
}

bool upload_assets(const assets::Data &d)
{
    if (!upload_mesh(d.sonic, r.sonic) || !upload_mesh(d.ring, r.ring)) return false;
    r.has_ball = d.ball_fx && upload_mesh(d.ball, r.ball);
    r.has_dome = upload_mesh(d.dome, r.dome);
    if (d.boost_fx && upload_mesh(d.aura, r.aura)) {
        const int base = int(r.textures.size());
        r.textures.push_back(texture_view(d.aura_texture));
        r.textures.push_back(texture_view(d.glow_texture));
        r.textures.push_back(texture_view(d.wave_texture));
        r.has_aura = r.textures[base] && r.textures[base + 1] && r.textures[base + 2];
        if (r.has_aura) { r.aura_texture = base; r.glow_texture = base + 1; r.wave_texture = base + 2; }
    }
    if (d.trail_fx) {
        const int base = int(r.textures.size());
        r.textures.push_back(texture_view(d.trail_texture));
        r.textures.push_back(texture_view(d.bolt_texture));
        r.textures.push_back(texture_view(d.arc_texture));
        r.textures.push_back(texture_view(d.orb_texture));
        if (r.textures[base] && r.textures[base + 1] && r.textures[base + 2] && r.textures[base + 3]) {
            r.trail_texture = base; r.bolt_texture = base + 1; r.arc_texture = base + 2; r.orb_texture = base + 3;
        }
    }
    static uint8_t atlas[font::atlas_w * font::atlas_h];
    font::build_atlas(atlas);
    r.font_texture = int(r.textures.size());
    r.textures.push_back(make_texture(font::atlas_w, font::atlas_h, 1, DXGI_FORMAT_R8_UNORM, {{atlas, font::atlas_w, 0}}));
    static const uint8_t white[4] = {255, 255, 255, 255};
    r.white_texture = int(r.textures.size());
    r.textures.push_back(make_texture(1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {{white, 4, 0}}));
    mod::log("render: Sonic and ring uploaded (%zu textures)", r.textures.size());
    return r.textures.back() != nullptr;
}

void release_targets()
{
    release(r.rtv); release(r.dsv); release(r.depth);
    release(r.copy_srv); release(r.copy); r.copy_w = r.copy_h = 0;
    r.width = r.height = 0;
}

bool ensure_targets(IDXGISwapChain *chain)
{
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(chain->GetDesc(&desc))) return false;
    if (r.rtv && r.width == desc.BufferDesc.Width && r.height == desc.BufferDesc.Height) return true;
    release_targets();
    ID3D11Texture2D *back = nullptr;
    if (FAILED(chain->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    D3D11_TEXTURE2D_DESC bd{};
    back->GetDesc(&bd);
    DXGI_FORMAT view = bd.Format;
    if (view == DXGI_FORMAT_R8G8B8A8_TYPELESS) view = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (view == DXGI_FORMAT_B8G8R8A8_TYPELESS) view = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (view == DXGI_FORMAT_R10G10B10A2_TYPELESS) view = DXGI_FORMAT_R10G10B10A2_UNORM;
    if (view == DXGI_FORMAT_R16G16B16A16_TYPELESS) view = DXGI_FORMAT_R16G16B16A16_FLOAT;
    D3D11_RENDER_TARGET_VIEW_DESC rv{};
    rv.Format = view; rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    const HRESULT hr = r.device->CreateRenderTargetView(back, &rv, &r.rtv);
    back->Release();
    if (FAILED(hr)) return false;
    r.color_mode = view == DXGI_FORMAT_R16G16B16A16_FLOAT ? 1 : (view == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || view == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) ? 2 : 0;
    D3D11_TEXTURE2D_DESC dd{};
    dd.Width = bd.Width; dd.Height = bd.Height; dd.MipLevels = 1; dd.ArraySize = 1; dd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    dd.SampleDesc.Count = 1; dd.Usage = D3D11_USAGE_DEFAULT; dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(r.device->CreateTexture2D(&dd, nullptr, &r.depth)) || FAILED(r.device->CreateDepthStencilView(r.depth, nullptr, &r.dsv))) return false;
    r.width = bd.Width; r.height = bd.Height; r.format = view;
    mod::log("render: targets %ux%u format %d", r.width, r.height, int(view));
    return true;
}

bool init_device(IDXGISwapChain *chain)
{
    ID3D11Device *device = nullptr;
    if (FAILED(chain->GetDevice(IID_PPV_ARGS(&device))) || !device) return false;   // not Direct3D 11
    r.device = device;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&r.device1)))) { mod::log("render: no D3D11.1 device"); return false; }
    ID3D11DeviceContext1 *ctx = nullptr;
    r.device1->GetImmediateContext1(&ctx);
    r.context = ctx;
    const D3D_FEATURE_LEVEL level = device->GetFeatureLevel();
    if (FAILED(r.device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device1), nullptr, &r.state))) {
        mod::log("render: cannot create an isolated device context state");
        return false;
    }
    chain->QueryInterface(IID_PPV_ARGS(&r.chain));
    if (!create_pipeline()) { mod::log("render: pipeline creation failed"); return false; }
    r.ready = true;
    mod::log("render: attached to BeamNG's D3D11 swap chain");
    set_status("attached");
    return true;
}

void bind_draw(const float *model_view, const float *proj, float alpha, float clamp, float glow, float mode)
{
    DrawConstants dc{};
    std::memcpy(dc.model_view, model_view, 64);
    std::memcpy(dc.proj, proj, 64);
    dc.tint[0] = dc.tint[1] = dc.tint[2] = 1; dc.tint[3] = alpha;
    dc.params[0] = mode; dc.params[1] = clamp; dc.params[2] = glow; dc.params[3] = float(r.color_mode);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(r.context->Map(r.cb_draw, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        std::memcpy(m.pData, &dc, sizeof(dc));
        r.context->Unmap(r.cb_draw, 0);
    }
}

void bind_bones(const float *data, size_t floats)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(r.context->Map(r.cb_bones, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        std::memset(m.pData, 0, 768 * 16);
        std::memcpy(m.pData, data, std::min<size_t>(floats * 4, 768 * 16));
        r.context->Unmap(r.cb_bones, 0);
    }
}

void mul(const float *a, const float *b, float *o)
{
    float t[16];
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) {
        float s = 0; for (int k = 0; k < 4; ++k) s += a[i * 4 + k] * b[k * 4 + j];
        t[i * 4 + j] = s;
    }
    std::memcpy(o, t, sizeof(t));
}

void draw_flat(const std::vector<FlatVertex> &v, const float *model_view, const float *proj, float mode, int texture)
{
    if (v.empty()) return;
    const UINT bytes = UINT(v.size() * sizeof(FlatVertex));
    if (bytes > r.flat_capacity) {
        release(r.flat_vb);
        D3D11_BUFFER_DESC bd{};
        bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.ByteWidth = std::max(bytes, 64u * 1024);
        if (FAILED(r.device->CreateBuffer(&bd, nullptr, &r.flat_vb))) return;
        r.flat_capacity = bd.ByteWidth;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(r.context->Map(r.flat_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    std::memcpy(m.pData, v.data(), bytes);
    r.context->Unmap(r.flat_vb, 0);
    bind_draw(model_view, proj, 1, 0, 0, mode);
    const UINT stride = sizeof(FlatVertex), offset = 0;
    r.context->IASetVertexBuffers(0, 1, &r.flat_vb, &stride, &offset);
    r.context->IASetInputLayout(r.layout_flat);
    r.context->VSSetShader(r.vs_flat, nullptr, 0);
    r.context->PSSetShader(r.ps_flat, nullptr, 0);
    r.context->OMSetDepthStencilState(r.depth_off, 0);
    r.context->PSSetShaderResources(0, 1, &r.textures[texture]);
    r.context->Draw(UINT(v.size()), 0);
}

void draw_mesh(const GpuMesh &g, const float *model, const float *view, const float *proj, float alpha, float glow)
{
    float mv[16];
    if (view) mul(model, view, mv); else std::memcpy(mv, model, sizeof(mv));
    const UINT stride = sizeof(frontiers::Vertex), offset = 0;
    r.context->IASetVertexBuffers(0, 1, &g.vb, &stride, &offset);
    r.context->IASetIndexBuffer(g.ib, DXGI_FORMAT_R16_UINT, 0);
    r.context->IASetInputLayout(r.layout_mesh);
    r.context->VSSetShader(r.vs_mesh, nullptr, 0);
    r.context->PSSetShader(r.ps_mesh, nullptr, 0);
    r.context->OMSetDepthStencilState(r.depth_on, 0);
    for (auto &m : g.meshes) {
        bind_draw(mv, proj, alpha, (m.wrap_u == 2 || m.wrap_v == 2) ? 1.0f : 0.0f, glow, 0);
        ID3D11ShaderResourceView *srv = r.textures[m.texture >= 0 ? g.texture_base + m.texture : r.white_texture];
        r.context->PSSetShaderResources(0, 1, &srv);
        r.context->DrawIndexed(m.index_count, m.first_index, INT(m.base_vertex));
    }
}

void bind_constants(const DrawConstants &dc)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(r.context->Map(r.cb_draw, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        std::memcpy(m.pData, &dc, sizeof(dc));
        r.context->Unmap(r.cb_draw, 0);
    }
}

// Boost aura shape: three nested bullets lying along the run, bright at the nose, fading back.
void draw_dome(const game::Scene &scene)
{
    static const std::vector<float> identity = draw::identity_bones();
    bind_bones(identity.data(), identity.size());
    const float factor[4] = {};
    r.context->OMSetBlendState(r.blend_add, factor, 0xffffffff);
    r.context->OMSetDepthStencilState(r.depth_test, 0);
    const UINT stride = sizeof(frontiers::Vertex), offset = 0;
    r.context->IASetVertexBuffers(0, 1, &r.dome.vb, &stride, &offset);
    r.context->IASetIndexBuffer(r.dome.ib, DXGI_FORMAT_R16_UINT, 0);
    r.context->IASetInputLayout(r.layout_mesh);
    r.context->VSSetShader(r.vs_aura, nullptr, 0);
    r.context->PSSetShader(r.ps_aura, nullptr, 0);
    r.context->PSSetShaderResources(0, 1, &r.textures[r.aura_texture]);
    for (int layer = 0; layer < 3; ++layer) {
        float model[16], mv[16];
        draw::layer_scale(scene.dome_model, draw::kDomeScale[layer], model);
        draw::mul(model, scene.view, mv);
        DrawConstants dc{};
        std::memcpy(dc.model_view, mv, 64); std::memcpy(dc.proj, scene.proj, 64);
        dc.tint[0] = 0.04f; dc.tint[1] = 0.28f; dc.tint[2] = 1.0f; dc.tint[3] = scene.aura * draw::kDomeAlpha[layer];
        dc.params[1] = scene.aura_time + layer * 0.37f; dc.params[2] = scene.dome_tail * (1 + 0.12f * layer); dc.params[3] = float(r.color_mode);
        bind_constants(dc);
        for (auto &m : r.dome.meshes) r.context->DrawIndexed(m.index_count, m.first_index, INT(m.base_vertex));
    }
    r.context->OMSetBlendState(r.blend, factor, 0xffffffff);
}

// Speed blur: copy BeamNG's finished frame, then draw it back streaked toward Sonic.
void draw_speed_blur(const game::Scene &scene)
{
    ID3D11Resource *back = nullptr;
    r.rtv->GetResource(&back);
    if (!back) return;
    if (!r.copy || r.copy_w != r.width || r.copy_h != r.height) {
        release(r.copy_srv); release(r.copy);
        D3D11_TEXTURE2D_DESC td{};
        static_cast<ID3D11Texture2D *>(back)->GetDesc(&td);
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.CPUAccessFlags = 0; td.MiscFlags = 0;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = r.format; sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
        if (FAILED(r.device->CreateTexture2D(&td, nullptr, &r.copy)) || FAILED(r.device->CreateShaderResourceView(r.copy, &sv, &r.copy_srv))) {
            release(r.copy_srv); release(r.copy); back->Release();
            mod::log("render: no copy of the frame for the speed blur");
            return;
        }
        r.copy_w = r.width; r.copy_h = r.height;
    }
    r.context->CopyResource(r.copy, back);
    back->Release();
    DrawConstants dc{};
    std::memcpy(dc.model_view, draw::kIdentity, 64); std::memcpy(dc.proj, draw::kIdentity, 64);
    dc.tint[0] = scene.blur_center[0]; dc.tint[1] = scene.blur_center[1]; dc.tint[2] = scene.blur * sheets::tuning::blurStrength;
    dc.params[3] = float(r.color_mode);
    bind_constants(dc);
    const float factor[4] = {};
    r.context->OMSetBlendState(r.blend_off, factor, 0xffffffff);
    r.context->OMSetDepthStencilState(r.depth_off, 0);
    r.context->IASetInputLayout(nullptr);
    r.context->VSSetShader(r.vs_post, nullptr, 0);
    r.context->PSSetShader(r.ps_post, nullptr, 0);
    r.context->PSSetShaderResources(0, 1, &r.copy_srv);
    r.context->Draw(3, 0);
    ID3D11ShaderResourceView *none = nullptr;
    r.context->PSSetShaderResources(0, 1, &none);
    r.context->OMSetBlendState(r.blend, factor, 0xffffffff);
}

// Frontiers' boost aura: the aura mesh on Sonic's pose (bones already bound), additive.
void draw_aura(const game::Scene &scene, int layer)
{
    float mv[16];
    draw::mul(scene.sonic_model, scene.view, mv);
    DrawConstants dc{};
    std::memcpy(dc.model_view, mv, 64); std::memcpy(dc.proj, scene.proj, 64);
    // deep electric blue (a sky blue added onto bright pixels turned white)
    dc.tint[0] = 0.04f; dc.tint[1] = 0.28f; dc.tint[2] = 1.0f; dc.tint[3] = scene.aura * (layer ? 0.35f : 0.6f);
    // a close shell and a wider halo, pushed out by the Lab's Aura thickness (the mesh is inside-out: its normals point in)
    dc.params[0] = (layer ? -0.085f : -0.03f) * scene.aura_thickness;
    dc.params[1] = scene.aura_time; dc.params[3] = float(r.color_mode);
    bind_constants(dc);
    const float factor[4] = {};
    r.context->OMSetBlendState(r.blend_add, factor, 0xffffffff);
    r.context->OMSetDepthStencilState(r.depth_test, 0);
    const UINT stride = sizeof(frontiers::Vertex), offset = 0;
    r.context->IASetVertexBuffers(0, 1, &r.aura.vb, &stride, &offset);
    r.context->IASetIndexBuffer(r.aura.ib, DXGI_FORMAT_R16_UINT, 0);
    r.context->IASetInputLayout(r.layout_mesh);
    r.context->VSSetShader(r.vs_aura, nullptr, 0);
    r.context->PSSetShader(r.ps_aura, nullptr, 0);
    r.context->PSSetShaderResources(0, 1, &r.textures[r.aura_texture]);
    for (auto &m : r.aura.meshes) r.context->DrawIndexed(m.index_count, m.first_index, INT(m.base_vertex));
    r.context->OMSetBlendState(r.blend, factor, 0xffffffff);
}

void draw_frame(IDXGISwapChain *chain)
{
    const double now = mod::now_seconds();
    const assets::Data *data = assets::ready();
    if (data) audio::init(*data);
    SbFrame frame{};
    std::vector<float> rings;
    bool have = false;
    AcquireSRWLockExclusive(&frame_lock);
    if (have_frame) {
        frame = latest; rings = latest_rings; have = true;
        frame.picked = pending_picks; frame.jumped = pending_jumps;
        pending_picks = pending_jumps = 0;
    }
    ReleaseSRWLockExclusive(&frame_lock);
    frame.ringPos = rings.data();
    DXGI_SWAP_CHAIN_DESC desc{};
    chain->GetDesc(&desc);
    const float aspect = desc.BufferDesc.Height ? float(desc.BufferDesc.Width) / desc.BufferDesc.Height : 16.0f / 9;
    game::update(data, have ? &frame : nullptr, now, aspect, r.scene);
    const game::Scene &scene = r.scene;
    if (!scene.sonic_visible && scene.rings.empty() && scene.texts.empty()) return;
    if (backend == 12) {
        bool drew = false;
        if (!r12::draw(scene, data, aspect, drew)) { r.disabled = true; set_status("d3d12 drawing failed"); mod::log("render12: drawing disabled"); return; }
        if (data) set_status("drawing (d3d12)");
        if (drew && ++r.frames_drawn == 300) { mod::log("render: Sonic drawn in 300 frames"); testmode::report_frames_drawn(); }
        return;
    }
    if (data && !r.uploaded) {
        r.uploaded = true;
        if (!upload_assets(*data)) { r.disabled = true; set_status("upload failed"); return; }
        set_status("drawing");
    }
    if (!r.uploaded || !ensure_targets(chain)) return;

    ID3DDeviceContextState *previous = nullptr;
    r.context->SwapDeviceContextState(r.state, &previous);
    r.context->OMSetRenderTargets(1, &r.rtv, r.dsv);
    r.context->ClearDepthStencilView(r.dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.0f, 0);
    D3D11_VIEWPORT vp{0, 0, float(r.width), float(r.height), 0, 1};
    r.context->RSSetViewports(1, &vp);
    r.context->RSSetState(r.raster);
    const float factor[4] = {};
    r.context->OMSetBlendState(r.blend, factor, 0xffffffff);
    r.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    r.context->VSSetConstantBuffers(0, 1, &r.cb_draw);
    r.context->VSSetConstantBuffers(1, 1, &r.cb_bones);
    r.context->PSSetConstantBuffers(0, 1, &r.cb_draw);
    r.context->PSSetSamplers(0, 3, r.samplers);
    static const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    bool drew_sonic = false;
    if (scene.camera && scene.blur > 0.01f) draw_speed_blur(scene);
    if (scene.camera) {
        if (scene.shadow) {
            std::vector<FlatVertex> v;
            const float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
            for (int k : {0, 1, 2, 0, 2, 3}) {
                const float *p = scene.shadow_corners[k];
                v.push_back({{p[0], p[1], p[2]}, {uv[k][0], uv[k][1]}, {0, 0, 0, scene.shadow_alpha}});
            }
            draw_flat(v, scene.view, scene.proj, 1, r.white_texture);
        }
        // Power Boost trail behind Sonic (drawn before him, so he stays in front of it)
        if (r.trail_texture >= 0 && (!scene.trail.empty() || !scene.arcs.empty())) {
            const float factor[4] = {};
            if (!scene.beam.empty()) draw_flat(draw::as_vertices(scene.beam), scene.view, scene.proj, 5, r.white_texture);
            r.context->OMSetBlendState(r.blend_add, factor, 0xffffffff);
            if (!scene.trail.empty()) draw_flat(draw::as_vertices(scene.trail), scene.view, scene.proj, 4, r.trail_texture);
            if (!scene.arcs.empty()) draw_flat(draw::as_vertices(scene.arcs), scene.view, scene.proj, 4, r.arc_texture);
            r.context->OMSetBlendState(r.blend, factor, 0xffffffff);
        }
        if (scene.sonic_visible && !scene.pose.empty()) {
            bind_bones(scene.pose.data(), scene.pose.size());
            if (scene.sonic_shadow) {
                float mv[16];
                draw::mul(scene.sonic_shadow_model, scene.view, mv);
                DrawConstants dc{};
                std::memcpy(dc.model_view, mv, 64); std::memcpy(dc.proj, scene.proj, 64);
                dc.tint[3] = scene.sonic_shadow_alpha; dc.params[3] = float(r.color_mode);
                bind_constants(dc);
                const UINT stride = sizeof(frontiers::Vertex), offset = 0;
                r.context->IASetVertexBuffers(0, 1, &r.sonic.vb, &stride, &offset);
                r.context->IASetIndexBuffer(r.sonic.ib, DXGI_FORMAT_R16_UINT, 0);
                r.context->IASetInputLayout(r.layout_mesh);
                r.context->VSSetShader(r.vs_mesh, nullptr, 0);
                r.context->PSSetShader(r.ps_shadow, nullptr, 0);
                r.context->OMSetDepthStencilState(r.stencil_once, 0);
                for (auto &m : r.sonic.meshes) r.context->DrawIndexed(m.index_count, m.first_index, INT(m.base_vertex));
            }
            if (!scene.sonic_hidden) draw_mesh(r.sonic, scene.sonic_model, scene.view, scene.proj, 1, 0);
            drew_sonic = true;
            if (r.has_aura && scene.aura > 0.01f) {
                if (scene.aura_dome && r.has_dome) draw_dome(scene);
                else { draw_aura(scene, 0); draw_aura(scene, 1); }
            }
            // Frontiers' spin ball shell around Sonic curled up in a jump (its own bones)
            if (r.has_ball && scene.ball_visible && !scene.ball_pose.empty()) {
                bind_bones(scene.ball_pose.data(), scene.ball_pose.size());
                draw_mesh(r.ball, scene.ball_model, scene.view, scene.proj, scene.ball_alpha, 0);
            }
        }
    }
    if (!scene.rings.empty()) {
        static std::vector<float> identity_bones = [] {
            std::vector<float> b(768 * 4, 0.0f);
            for (int i = 0; i < 256; ++i) { b[i * 12] = 1; b[i * 12 + 5] = 1; b[i * 12 + 10] = 1; }
            return b;
        }();
        bind_bones(identity_bones.data(), identity_bones.size());
        for (auto &inst : scene.rings) {
            if (inst.screen) draw_mesh(r.ring, inst.model, nullptr, kIdentity, inst.alpha, 0.25f);
            else if (scene.camera) draw_mesh(r.ring, inst.model, scene.view, scene.proj, inst.alpha, 0.25f);
        }
    }
    if (scene.camera && r.has_aura && !scene.sprites.empty()) {
        const float factor[4] = {};
        r.context->OMSetBlendState(r.blend_add, factor, 0xffffffff);
        draw_flat(draw::sprite_vertices(scene, 0), scene.view, scene.proj, 2, r.glow_texture);
        draw_flat(draw::sprite_vertices(scene, 1), scene.view, scene.proj, 3, r.wave_texture);
        draw_flat(draw::sprite_vertices(scene, 4), scene.view, scene.proj, 1, r.white_texture);   // jump ball glow: soft round, untextured
        if (r.trail_texture >= 0) {
            draw_flat(draw::sprite_vertices(scene, 2), scene.view, scene.proj, 4, r.bolt_texture);
            draw_flat(draw::sprite_vertices(scene, 3), scene.view, scene.proj, 4, r.orb_texture);
        }
        r.context->OMSetBlendState(r.blend, factor, 0xffffffff);
    }
    if (!scene.texts.empty()) {
        std::vector<FlatVertex> v;
        for (auto &t : scene.texts) {
            const float h = t.size, w = h * 5.0f / 7.0f / std::max(0.5f, aspect), advance = w * 1.25f;
            float x = t.x;
            for (char ch : t.text) {
                const int g = font::index_of(ch);
                if (g >= 0) {
                    const float u0 = float((g % font::columns) * font::cell_w) / font::atlas_w, v0 = float((g / font::columns) * font::cell_h) / font::atlas_h;
                    const float u1 = u0 + 5.0f / font::atlas_w, v1 = v0 + 7.0f / font::atlas_h;
                    const FlatVertex q[4] = {{{x, t.y, 0.5f}, {u0, v0}, {t.color[0], t.color[1], t.color[2], t.color[3]}},
                                             {{x + w, t.y, 0.5f}, {u1, v0}, {t.color[0], t.color[1], t.color[2], t.color[3]}},
                                             {{x + w, t.y - h, 0.5f}, {u1, v1}, {t.color[0], t.color[1], t.color[2], t.color[3]}},
                                             {{x, t.y - h, 0.5f}, {u0, v1}, {t.color[0], t.color[1], t.color[2], t.color[3]}}};
                    for (int k : {0, 1, 2, 0, 2, 3}) v.push_back(q[k]);
                }
                x += advance;
            }
        }
        draw_flat(v, kIdentity, kIdentity, 0, r.font_texture);
    }
    r.context->SwapDeviceContextState(previous, nullptr);
    release(previous);
    if (drew_sonic && ++r.frames_drawn == 300) {
        mod::log("render: Sonic drawn in 300 frames");
        testmode::report_frames_drawn();
    }
}

HRESULT STDMETHODCALLTYPE on_present(IDXGISwapChain *self, UINT interval, UINT flags)
{
    if (present_depth++ == 0 && !(flags & DXGI_PRESENT_TEST) && !r.disabled) {
        if (backend == 0) {
            if (init_device(self)) backend = 11;
            else if (r12::attach(self)) { backend = 12; r.ready = true; self->QueryInterface(IID_PPV_ARGS(&r.chain)); set_status("attached (d3d12)"); }
            else if (++attach_tries > 3000) { r.disabled = true; mod::log("render: BeamNG's renderer is neither Direct3D 11 nor 12 (Vulkan?): Sonic cannot be drawn"); set_status("unsupported renderer"); }
        }
        if (r.ready) {
            IUnknown *identity = nullptr;
            if (SUCCEEDED(self->QueryInterface(IID_PPV_ARGS(&identity)))) {
                if (identity == r.chain) draw_frame(self);
                identity->Release();
            }
        }
    }
    const HRESULT hr = real_present(self, interval, flags);
    --present_depth;
    return hr;
}

HRESULT STDMETHODCALLTYPE on_resize_buffers(IDXGISwapChain *self, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
{
    if (backend == 12) r12::before_resize(); else release_targets();
    return real_resize(self, count, width, height, format, flags);
}

// The DXGI swap-chain class is shared by every swap chain in the process: patching the method
// table of a throw-away swap chain reaches BeamNG's too.
bool hook_swapchain_class()
{
    WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"SonicBeamProbe";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 8, 8, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain *chain = nullptr;
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &chain, &device, nullptr, &ctx);
    if (FAILED(hr)) hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &chain, &device, nullptr, &ctx);
    bool ok = false;
    if (SUCCEEDED(hr) && chain) {
        void **table = *reinterpret_cast<void ***>(chain);
        DWORD protection = 0;
        if (VirtualProtect(&table[8], sizeof(void *) * 6, PAGE_READWRITE, &protection)) {
            real_present = reinterpret_cast<PresentFn>(table[8]);
            real_resize = reinterpret_cast<ResizeFn>(table[13]);
            table[8] = reinterpret_cast<void *>(on_present);
            table[13] = reinterpret_cast<void *>(on_resize_buffers);
            DWORD unused;
            VirtualProtect(&table[8], sizeof(void *) * 6, protection, &unused);
            ok = true;
        }
    }
    release(ctx); release(chain); release(device);
    if (hwnd) DestroyWindow(hwnd);
    return ok;
}

} // namespace

void start_link();   // link.cpp

// Started by the version.dll proxy in BeamNG's main process.
extern "C" int sb_start()
{
    if (InterlockedCompareExchange(&hooked, 1, 0) != 0) return 1;
    mod::log("Sonic Beam renderer %s started in BeamNG", SB_VERSION);
    start_link();
    testmode::start();
    assets::start_loading();
    return 1;
}

// Called by the link on the script's first frame: BeamNG's graphics are fully up by then, so
// probing DXGI cannot race the game's own device and swap-chain creation at startup.
void hook_graphics()
{
    static LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) != 0) return;
    // BeamNG's D3D12 device exists by now, so this probe reuses it instead of creating one first.
    if (!r12::hook_queue_class()) mod::log("render12: no D3D12 here, Direct3D 11 only");
    if (!hook_swapchain_class()) { mod::log("render: could not hook the swap chain"); set_status("hook failed"); return; }
    mod::log("render: swap chain hooked");
    set_status("hooked");
}

extern "C" void sb_frame(const SbFrame *frame)
{
    if (!frame) return;
    AcquireSRWLockExclusive(&frame_lock);
    latest = *frame;
    const int count = std::max(0, std::min(frame->ringCount, 512));
    latest_rings.assign(frame->ringPos, frame->ringPos ? frame->ringPos + 3 * count : frame->ringPos);
    latest.ringCount = frame->ringPos ? count : 0;
    pending_picks += frame->picked;
    pending_jumps += frame->jumped;
    have_frame = true;
    ReleaseSRWLockExclusive(&frame_lock);
}


