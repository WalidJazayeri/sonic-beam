// SPDX-License-Identifier: MIT
// Sonic Beam's Direct3D 12 renderer, for BeamNG running on its D3D12 back end. The command
// queue BeamNG presents with is learnt from ID3D12CommandQueue::ExecuteCommandLists (the queue
// class's method table, patched through a throw-away queue); Sonic, the rings and the HUD are
// recorded on our own command list and executed on that queue right before Present.
#include "render12.hpp"
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <cmath>
#include <vector>
#include "shared_draw.hpp"

namespace r12 {

namespace {

template <class T> void release(T *&p) { if (p) { p->Release(); p = nullptr; } }

using ExecuteFn = void(STDMETHODCALLTYPE *)(ID3D12CommandQueue *, UINT, ID3D12CommandList *const *);
ExecuteFn real_execute = nullptr;
ID3D12CommandQueue *volatile last_direct_queue = nullptr;

void STDMETHODCALLTYPE on_execute(ID3D12CommandQueue *self, UINT count, ID3D12CommandList *const *lists)
{
    if (self->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) InterlockedExchangePointer(reinterpret_cast<void *volatile *>(&last_direct_queue), self);
    real_execute(self, count, lists);
}

struct GpuMesh {
    ID3D12Resource *vb = nullptr, *ib = nullptr;
    D3D12_VERTEX_BUFFER_VIEW vbv{};
    D3D12_INDEX_BUFFER_VIEW ibv{};
    std::vector<frontiers::Mesh> meshes;
    int texture_base = 0;
};

struct Frame {
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12Resource *upload = nullptr;
    uint8_t *mapped = nullptr;
    UINT64 used = 0, fence = 0;
};
constexpr UINT64 kUploadBytes = 4u << 20;
constexpr int kFrames = 4;

struct Context {
    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    IDXGISwapChain3 *chain = nullptr;
    ID3D12Fence *fence = nullptr;
    HANDLE event = nullptr;
    UINT64 next_fence = 1;
    Frame frames[kFrames];
    unsigned frame_index = 0;
    ID3D12GraphicsCommandList *list = nullptr;
    std::vector<ID3D12Resource *> backbuffers;
    ID3D12DescriptorHeap *rtv = nullptr, *dsv = nullptr;
    ID3D12Resource *depth = nullptr;
    UINT rtv_stride = 0, width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    int color_mode = 0;
    ID3D12RootSignature *root = nullptr;
    ID3D12PipelineState *mesh = nullptr, *flat = nullptr;
    ID3D12PipelineState *aura = nullptr, *glow = nullptr, *post = nullptr;   // boost effect
    ID3D12PipelineState *shadow = nullptr;          // Sonic's projected shadow (stencil: once per pixel)
    DXGI_FORMAT pipeline_format = DXGI_FORMAT_UNKNOWN;
    bool uploaded = false;
    std::vector<ID3D12Resource *> textures, staging;
    UINT64 staging_fence = 0;
    ID3D12DescriptorHeap *srv = nullptr;
    UINT srv_stride = 0;
    int font_texture = 0, white_texture = 0;
    GpuMesh sonic, ring, aura_mesh, ball, dome;
    bool has_aura = false, has_ball = false, has_dome = false;
    int aura_texture = -1, glow_texture = -1, wave_texture = -1;
    int trail_texture = -1, bolt_texture = -1, arc_texture = -1, orb_texture = -1;
    int copy_slot = -1;                           // descriptor of the frame copy the speed blur reads
    ID3D12Resource *copy = nullptr;
    UINT copy_w = 0, copy_h = 0;
} c;

D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) { D3D12_HEAP_PROPERTIES h{}; h.Type = type; return h; }

ID3D12Resource *buffer(UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state)
{
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    auto h = heap(type);
    ID3D12Resource *r = nullptr;
    return SUCCEEDED(c.device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r))) ? r : nullptr;
}

void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r; b.Transition.StateBefore = from; b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    c.list->ResourceBarrier(1, &b);
}

void wait_idle()
{
    if (!c.fence || !c.queue) return;
    const UINT64 value = c.next_fence++;
    if (SUCCEEDED(c.queue->Signal(c.fence, value)) && c.fence->GetCompletedValue() < value && SUCCEEDED(c.fence->SetEventOnCompletion(value, c.event)))
        WaitForSingleObject(c.event, 2000);
}

void release_targets()
{
    for (auto *b : c.backbuffers) if (b) b->Release();
    c.backbuffers.clear();
    release(c.rtv); release(c.dsv); release(c.depth);
    release(c.copy); c.copy_w = c.copy_h = 0;
    c.width = c.height = 0;
}

bool init_context()
{
    if (FAILED(c.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.fence)))) return false;
    c.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    for (auto &f : c.frames) {
        if (FAILED(c.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator)))) return false;
        f.upload = buffer(kUploadBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        D3D12_RANGE none{0, 0};
        if (!f.upload || FAILED(f.upload->Map(0, &none, reinterpret_cast<void **>(&f.mapped)))) return false;
    }
    if (FAILED(c.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.frames[0].allocator, nullptr, IID_PPV_ARGS(&c.list)))) return false;
    c.list->Close();
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; params[0].Descriptor = {0, 0}; params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; params[1].Descriptor = {1, 0}; params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[2].DescriptorTable = {1, &range}; params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC samplers[3]{};
    for (int i = 0; i < 3; ++i) {
        auto &s = samplers[i];
        s.Filter = i == 2 ? D3D12_FILTER_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        s.AddressU = s.AddressV = s.AddressW = i == 0 ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        s.MaxLOD = D3D12_FLOAT32_MAX; s.ShaderRegister = i; s.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        s.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    }
    D3D12_ROOT_SIGNATURE_DESC rd{3, params, 3, samplers, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ID3DBlob *blob = nullptr, *errors = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    release(errors);
    if (FAILED(hr)) return false;
    hr = c.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&c.root));
    release(blob);
    return SUCCEEDED(hr);
}

bool build_pipelines(DXGI_FORMAT format)
{
    if (c.pipeline_format == format && c.mesh) return true;
    release(c.mesh); release(c.flat); release(c.aura); release(c.glow); release(c.post); release(c.shadow);
    ID3DBlob *code[9] = {}, *errors = nullptr;
    const char *entries[9] = {"vsMesh", "psMesh", "vsFlat", "psFlat", "vsAura", "psAura", "vsPost", "psPost", "psShadow"};
    const char *targets[9] = {"vs_5_0", "ps_5_0", "vs_5_0", "ps_5_0", "vs_5_0", "ps_5_0", "vs_5_0", "ps_5_0", "ps_5_0"};
    bool ok = true;
    for (int i = 0; i < 9 && ok; ++i) {
        ok = SUCCEEDED(D3DCompile(draw::kShader, sizeof(draw::kShader) - 1, "sonic_beam", nullptr, nullptr, entries[i], targets[i], D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code[i], &errors));
        if (!ok && errors) mod::log("render12: shader %s: %s", entries[i], static_cast<const char *>(errors->GetBufferPointer()));
        release(errors);
    }
    if (ok) {
        D3D12_INPUT_ELEMENT_DESC mesh_input[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"BLENDWEIGHT", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        D3D12_INPUT_ELEMENT_DESC flat_input[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = c.root;
        auto &blend = pd.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_SRC_ALPHA; blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA; blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ZERO; blend.DestBlendAlpha = D3D12_BLEND_ONE; blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
        pd.SampleMask = UINT_MAX;
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable = TRUE;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1; pd.RTVFormats[0] = format; pd.DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; pd.SampleDesc.Count = 1;
        pd.DepthStencilState.DepthEnable = TRUE; pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        pd.VS = {code[0]->GetBufferPointer(), code[0]->GetBufferSize()}; pd.PS = {code[1]->GetBufferPointer(), code[1]->GetBufferSize()};
        pd.InputLayout = {mesh_input, 5};
        ok = SUCCEEDED(c.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&c.mesh)));
        pd.DepthStencilState.DepthEnable = FALSE; pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pd.VS = {code[2]->GetBufferPointer(), code[2]->GetBufferSize()}; pd.PS = {code[3]->GetBufferPointer(), code[3]->GetBufferSize()};
        pd.InputLayout = {flat_input, 3};
        ok = ok && SUCCEEDED(c.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&c.flat)));
        // boost effect: additive glow (flash sprites, aura) and the full-screen speed blur
        blend.SrcBlend = D3D12_BLEND_SRC_ALPHA; blend.DestBlend = D3D12_BLEND_ONE;
        ok = ok && SUCCEEDED(c.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&c.glow)));
        pd.DepthStencilState.DepthEnable = TRUE;   // the aura hides behind Sonic's body, writes no depth
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        pd.VS = {code[4]->GetBufferPointer(), code[4]->GetBufferSize()}; pd.PS = {code[5]->GetBufferPointer(), code[5]->GetBufferSize()};
        pd.InputLayout = {mesh_input, 5};
        ok = ok && SUCCEEDED(c.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&c.aura)));
        pd.DepthStencilState.DepthEnable = FALSE;
        blend.BlendEnable = FALSE;
        pd.VS = {code[6]->GetBufferPointer(), code[6]->GetBufferSize()}; pd.PS = {code[7]->GetBufferPointer(), code[7]->GetBufferSize()};
        pd.InputLayout = {nullptr, 0};
        ok = ok && SUCCEEDED(c.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&c.post)));
        // shadow: Sonic's mesh flattened, alpha blended, each pixel darkened once (stencil 0 -> 1)
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_SRC_ALPHA; blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        pd.VS = {code[0]->GetBufferPointer(), code[0]->GetBufferSize()}; pd.PS = {code[8]->GetBufferPointer(), code[8]->GetBufferSize()};
        pd.InputLayout = {mesh_input, 5};
        pd.DepthStencilState.StencilEnable = TRUE;
        pd.DepthStencilState.StencilReadMask = 0xff; pd.DepthStencilState.StencilWriteMask = 0xff;
        pd.DepthStencilState.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_INCR_SAT, D3D12_COMPARISON_FUNC_EQUAL};
        pd.DepthStencilState.BackFace = pd.DepthStencilState.FrontFace;
        ok = ok && SUCCEEDED(c.device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&c.shadow)));
    }
    for (auto *b : code) if (b) b->Release();
    if (ok) c.pipeline_format = format;
    else mod::log("render12: pipeline creation failed for format %d", int(format));
    return ok;
}

bool ensure_targets()
{
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(c.chain->GetDesc(&desc))) return false;
    if (c.rtv && c.width == desc.BufferDesc.Width && c.height == desc.BufferDesc.Height && c.backbuffers.size() == desc.BufferCount) return true;
    release_targets();
    if (!desc.BufferDesc.Width || !desc.BufferDesc.Height || !desc.BufferCount || desc.BufferCount > 16) return false;
    DXGI_FORMAT view = desc.BufferDesc.Format;
    if (view == DXGI_FORMAT_R8G8B8A8_TYPELESS) view = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (view == DXGI_FORMAT_B8G8R8A8_TYPELESS) view = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (view == DXGI_FORMAT_R10G10B10A2_TYPELESS) view = DXGI_FORMAT_R10G10B10A2_UNORM;
    c.format = view;
    c.color_mode = view == DXGI_FORMAT_R16G16B16A16_FLOAT ? 1 : (view == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || view == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) ? 2 : 0;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, desc.BufferCount, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(c.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&c.rtv)))) return false;
    hd = {D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (FAILED(c.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&c.dsv)))) return false;
    c.rtv_stride = c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 0; i < desc.BufferCount; ++i) {
        ID3D12Resource *b = nullptr;
        if (FAILED(c.chain->GetBuffer(i, IID_PPV_ARGS(&b)))) { release_targets(); return false; }
        c.backbuffers.push_back(b);
        auto h = c.rtv->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(i) * c.rtv_stride;
        D3D12_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format = view; rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        c.device->CreateRenderTargetView(b, &rv, h);
    }
    D3D12_RESOURCE_DESC dd{};
    dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; dd.Width = desc.BufferDesc.Width; dd.Height = desc.BufferDesc.Height;
    dd.DepthOrArraySize = 1; dd.MipLevels = 1; dd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; dd.SampleDesc.Count = 1; dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE clear{}; clear.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; clear.DepthStencil.Depth = 0;
    auto h = heap(D3D12_HEAP_TYPE_DEFAULT);
    if (FAILED(c.device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&c.depth)))) return false;
    c.device->CreateDepthStencilView(c.depth, nullptr, c.dsv->GetCPUDescriptorHandleForHeapStart());
    c.width = desc.BufferDesc.Width; c.height = desc.BufferDesc.Height;
    mod::log("render12: targets %ux%u format %d, %u buffers", c.width, c.height, int(view), desc.BufferCount);
    return build_pipelines(view);
}

bool upload_assets(const assets::Data &d)
{
    struct Source { const frontiers::Texture *tex; const uint8_t *pixels; uint32_t w, h, dxgi, pitch; };
    std::vector<Source> sources;
    c.sonic.texture_base = 0;
    for (auto &t : d.sonic.textures) sources.push_back({&t, nullptr, 0, 0, 0, 0});
    c.ring.texture_base = int(sources.size());
    for (auto &t : d.ring.textures) sources.push_back({&t, nullptr, 0, 0, 0, 0});
    c.ball.texture_base = int(sources.size());
    if (d.ball_fx) for (auto &t : d.ball.textures) sources.push_back({&t, nullptr, 0, 0, 0, 0});
    static uint8_t atlas[font::atlas_w * font::atlas_h];
    font::build_atlas(atlas);
    c.font_texture = int(sources.size());
    sources.push_back({nullptr, atlas, font::atlas_w, font::atlas_h, DXGI_FORMAT_R8_UNORM, font::atlas_w});
    static const uint8_t white[4] = {255, 255, 255, 255};
    c.white_texture = int(sources.size());
    sources.push_back({nullptr, white, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 4});
    if (d.boost_fx) {
        c.aura_texture = int(sources.size()); sources.push_back({&d.aura_texture, nullptr, 0, 0, 0, 0});
        c.glow_texture = int(sources.size()); sources.push_back({&d.glow_texture, nullptr, 0, 0, 0, 0});
        c.wave_texture = int(sources.size()); sources.push_back({&d.wave_texture, nullptr, 0, 0, 0, 0});
    }
    if (d.trail_fx) {
        c.trail_texture = int(sources.size()); sources.push_back({&d.trail_texture, nullptr, 0, 0, 0, 0});
        c.bolt_texture = int(sources.size()); sources.push_back({&d.bolt_texture, nullptr, 0, 0, 0, 0});
        c.arc_texture = int(sources.size()); sources.push_back({&d.arc_texture, nullptr, 0, 0, 0, 0});
        c.orb_texture = int(sources.size()); sources.push_back({&d.orb_texture, nullptr, 0, 0, 0, 0});
    }
    c.copy_slot = int(sources.size());            // filled by ensure_copy
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, UINT(sources.size() + 1), D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(c.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&c.srv)))) return false;
    c.srv_stride = c.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto upload_buffer = [&](const void *data, UINT64 size, ID3D12Resource *&out) {
        out = buffer(size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        ID3D12Resource *stage = buffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!out || !stage) { release(stage); return false; }
        void *p = nullptr;
        D3D12_RANGE none{0, 0};
        stage->Map(0, &none, &p); std::memcpy(p, data, size_t(size)); stage->Unmap(0, nullptr);
        c.list->CopyBufferRegion(out, 0, stage, 0, size);
        c.staging.push_back(stage);
        return true;
    };
    auto mesh = [&](const frontiers::Model &m, GpuMesh &g) {
        const UINT64 vbytes = m.vertices.size() * sizeof(frontiers::Vertex), ibytes = m.indices.size() * 2;
        if (!upload_buffer(m.vertices.data(), vbytes, g.vb) || !upload_buffer(m.indices.data(), ibytes, g.ib)) return false;
        barrier(g.vb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        barrier(g.ib, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER);
        g.vbv = {g.vb->GetGPUVirtualAddress(), UINT(vbytes), sizeof(frontiers::Vertex)};
        g.ibv = {g.ib->GetGPUVirtualAddress(), UINT(ibytes), DXGI_FORMAT_R16_UINT};
        g.meshes = m.meshes;
        return true;
    };
    if (!mesh(d.sonic, c.sonic) || !mesh(d.ring, c.ring)) return false;
    c.has_aura = d.boost_fx && mesh(d.aura, c.aura_mesh);
    c.has_ball = d.ball_fx && mesh(d.ball, c.ball);
    c.has_dome = mesh(d.dome, c.dome);
    for (size_t i = 0; i < sources.size(); ++i) {
        const Source &s = sources[i];
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = s.tex ? s.tex->width : s.w; td.Height = s.tex ? s.tex->height : s.h;
        td.DepthOrArraySize = 1; td.MipLevels = UINT16(s.tex ? s.tex->mips : 1);
        td.Format = DXGI_FORMAT(s.tex ? s.tex->dxgi : s.dxgi); td.SampleDesc.Count = 1;
        auto h = heap(D3D12_HEAP_TYPE_DEFAULT);
        ID3D12Resource *tex = nullptr;
        if (FAILED(c.device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)))) {
            mod::log("render12: texture %zu (format %u) not supported", i, unsigned(td.Format)); return false;
        }
        c.textures.push_back(tex);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[16]; UINT rows[16]; UINT64 row_bytes[16], total = 0;
        c.device->GetCopyableFootprints(&td, 0, td.MipLevels, 0, fp, rows, row_bytes, &total);
        ID3D12Resource *stage = buffer(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!stage) return false;
        uint8_t *p = nullptr;
        D3D12_RANGE none{0, 0};
        stage->Map(0, &none, reinterpret_cast<void **>(&p));
        for (UINT mip = 0; mip < td.MipLevels; ++mip) {
            const uint8_t *src = s.tex ? s.tex->data.data() + s.tex->mip_offset[mip] : s.pixels;
            const UINT64 pitch = row_bytes[mip];
            for (UINT r = 0; r < rows[mip]; ++r) std::memcpy(p + fp[mip].Offset + UINT64(r) * fp[mip].Footprint.RowPitch, src + r * pitch, size_t(pitch));
            D3D12_TEXTURE_COPY_LOCATION dst{tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}}; dst.SubresourceIndex = mip;
            D3D12_TEXTURE_COPY_LOCATION from{stage, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}}; from.PlacedFootprint = fp[mip];
            c.list->CopyTextureRegion(&dst, 0, 0, 0, &from, nullptr);
        }
        stage->Unmap(0, nullptr);
        c.staging.push_back(stage);
        barrier(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = td.Format; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = td.MipLevels;
        auto handle = c.srv->GetCPUDescriptorHandleForHeapStart(); handle.ptr += i * c.srv_stride;
        c.device->CreateShaderResourceView(tex, &sv, handle);
    }
    mod::log("render12: Sonic and ring uploaded (%zu textures)", sources.size());
    return true;
}

D3D12_GPU_VIRTUAL_ADDRESS push(Frame &f, const void *data, UINT64 size, UINT64 align = 256)
{
    f.used = (f.used + align - 1) / align * align;
    if (f.used + size > kUploadBytes) return 0;
    std::memcpy(f.mapped + f.used, data, size_t(size));
    const D3D12_GPU_VIRTUAL_ADDRESS at = f.upload->GetGPUVirtualAddress() + f.used;
    f.used += size;
    return at;
}

D3D12_GPU_DESCRIPTOR_HANDLE srv_handle(int index)
{
    auto h = c.srv->GetGPUDescriptorHandleForHeapStart();
    h.ptr += UINT64(index) * c.srv_stride;
    return h;
}

D3D12_GPU_VIRTUAL_ADDRESS constants(Frame &f, const float *model_view, const float *proj, float alpha, float clamp, float glow, float mode)
{
    draw::DrawConstants dc{};
    std::memcpy(dc.model_view, model_view, 64); std::memcpy(dc.proj, proj, 64);
    dc.tint[0] = dc.tint[1] = dc.tint[2] = 1; dc.tint[3] = alpha;
    dc.params[0] = mode; dc.params[1] = clamp; dc.params[2] = glow; dc.params[3] = float(c.color_mode);
    std::memcpy(dc.clip, draw::active_clip, sizeof(dc.clip));
    return push(f, &dc, sizeof(dc));
}

void draw_flat(Frame &f, const std::vector<draw::FlatVertex> &v, const float *model_view, const float *proj, float mode, int texture,
               ID3D12PipelineState *pipeline = nullptr)
{
    if (v.empty() || texture < 0) return;
    const auto vb = push(f, v.data(), v.size() * sizeof(draw::FlatVertex), 16);
    const auto cb = constants(f, model_view, proj, 1, 0, 0, mode);
    if (!vb || !cb) return;
    c.list->SetPipelineState(pipeline ? pipeline : c.flat);
    c.list->SetGraphicsRootConstantBufferView(0, cb);
    c.list->SetGraphicsRootConstantBufferView(1, cb);
    c.list->SetGraphicsRootDescriptorTable(2, srv_handle(texture));
    D3D12_VERTEX_BUFFER_VIEW view{vb, UINT(v.size() * sizeof(draw::FlatVertex)), sizeof(draw::FlatVertex)};
    c.list->IASetVertexBuffers(0, 1, &view);
    c.list->DrawInstanced(UINT(v.size()), 1, 0, 0);
}

void draw_mesh(Frame &f, const GpuMesh &g, D3D12_GPU_VIRTUAL_ADDRESS bones, const float *model, const float *view, const float *proj, float alpha, float glow)
{
    float mv[16];
    if (view) draw::mul(model, view, mv); else std::memcpy(mv, model, sizeof(mv));
    c.list->SetPipelineState(c.mesh);
    c.list->SetGraphicsRootConstantBufferView(1, bones);
    c.list->IASetVertexBuffers(0, 1, &g.vbv);
    c.list->IASetIndexBuffer(&g.ibv);
    for (auto &m : g.meshes) {
        const auto cb = constants(f, mv, proj, alpha, (m.wrap_u == 2 || m.wrap_v == 2) ? 1.0f : 0.0f, glow, 0);
        if (!cb) return;
        c.list->SetGraphicsRootConstantBufferView(0, cb);
        c.list->SetGraphicsRootDescriptorTable(2, srv_handle(m.texture >= 0 ? g.texture_base + m.texture : c.white_texture));
        c.list->DrawIndexedInstanced(m.index_count, 1, m.first_index, INT(m.base_vertex), 0);
    }
}

// Boost aura shape: three nested bullets lying along the run, bright at the nose, fading back.
void draw_dome(Frame &f, const game::Scene &scene)
{
    static const std::vector<float> identity = draw::identity_bones();
    const auto bones = push(f, identity.data(), identity.size() * 4);
    if (!bones) return;
    c.list->SetPipelineState(c.aura);
    c.list->SetGraphicsRootConstantBufferView(1, bones);
    c.list->SetGraphicsRootDescriptorTable(2, srv_handle(c.aura_texture));
    c.list->IASetVertexBuffers(0, 1, &c.dome.vbv);
    c.list->IASetIndexBuffer(&c.dome.ibv);
    for (int layer = 0; layer < 3; ++layer) {
        float model[16], mv[16];
        draw::layer_scale(scene.dome_model, draw::kDomeScale[layer], model);
        draw::mul(model, scene.view, mv);
        draw::DrawConstants dc{};
        std::memcpy(dc.model_view, mv, 64); std::memcpy(dc.proj, scene.proj, 64);
        dc.tint[0] = 0.04f; dc.tint[1] = 0.28f; dc.tint[2] = 1.0f; dc.tint[3] = scene.aura * draw::kDomeAlpha[layer];
        dc.params[1] = scene.aura_time + layer * 0.37f; dc.params[2] = scene.dome_tail * (1 + 0.12f * layer); dc.params[3] = float(c.color_mode);
        const auto cb = push(f, &dc, sizeof(dc));
        if (!cb) return;
        c.list->SetGraphicsRootConstantBufferView(0, cb);
        for (auto &m : c.dome.meshes) c.list->DrawIndexedInstanced(m.index_count, 1, m.first_index, INT(m.base_vertex), 0);
    }
}

// Frontiers' boost aura: the aura mesh on Sonic's pose (same bones), additive.
void draw_aura(Frame &f, D3D12_GPU_VIRTUAL_ADDRESS bones, const game::Scene &scene, int layer)
{
    float mv[16];
    draw::mul(scene.sonic_model, scene.view, mv);
    draw::DrawConstants dc{};
    std::memcpy(dc.model_view, mv, 64); std::memcpy(dc.proj, scene.proj, 64);
    // deep electric blue (a sky blue added onto bright pixels turned white)
    dc.tint[0] = 0.04f; dc.tint[1] = 0.28f; dc.tint[2] = 1.0f; dc.tint[3] = scene.aura * (layer ? 0.35f : 0.6f);
    // a close shell and a wider halo, pushed out by the Lab's Aura thickness (the mesh is inside-out: its normals point in)
    dc.params[0] = (layer ? -0.085f : -0.03f) * scene.aura_thickness;
    dc.params[1] = scene.aura_time; dc.params[3] = float(c.color_mode);
    const auto cb = push(f, &dc, sizeof(dc));
    if (!cb) return;
    c.list->SetPipelineState(c.aura);
    c.list->SetGraphicsRootConstantBufferView(0, cb);
    c.list->SetGraphicsRootConstantBufferView(1, bones);
    c.list->SetGraphicsRootDescriptorTable(2, srv_handle(c.aura_texture));
    c.list->IASetVertexBuffers(0, 1, &c.aura_mesh.vbv);
    c.list->IASetIndexBuffer(&c.aura_mesh.ibv);
    for (auto &m : c.aura_mesh.meshes) c.list->DrawIndexedInstanced(m.index_count, 1, m.first_index, INT(m.base_vertex), 0);
}

// The texture the speed blur reads: a copy of the finished BeamNG frame, same size and format.
bool ensure_copy(ID3D12Resource *backbuffer)
{
    if (c.copy && c.copy_w == c.width && c.copy_h == c.height) return true;
    release(c.copy);
    D3D12_RESOURCE_DESC td = backbuffer->GetDesc();
    td.Flags = D3D12_RESOURCE_FLAG_NONE;
    auto h = heap(D3D12_HEAP_TYPE_DEFAULT);
    if (FAILED(c.device->CreateCommittedResource(&h, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&c.copy)))) {
        mod::log("render12: no copy of the frame for the speed blur");
        return false;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = c.format; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
    auto handle = c.srv->GetCPUDescriptorHandleForHeapStart(); handle.ptr += UINT64(c.copy_slot) * c.srv_stride;
    c.device->CreateShaderResourceView(c.copy, &sv, handle);
    c.copy_w = c.width; c.copy_h = c.height;
    return true;
}

} // namespace

bool hook_queue_class()
{
    ID3D12Device *device = nullptr;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) return false;
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue *queue = nullptr;
    bool ok = false;
    if (SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) {
        void **table = *reinterpret_cast<void ***>(queue);
        DWORD protection = 0;
        if (VirtualProtect(&table[10], sizeof(void *), PAGE_READWRITE, &protection)) {
            real_execute = reinterpret_cast<ExecuteFn>(table[10]);
            table[10] = reinterpret_cast<void *>(on_execute);
            DWORD unused;
            VirtualProtect(&table[10], sizeof(void *), protection, &unused);
            ok = true;
        }
        queue->Release();
    }
    device->Release();
    return ok;
}

bool attach(IDXGISwapChain *chain)
{
    // A D3D12 swap chain's "device" is the command queue it presents on.
    ID3D12CommandQueue *queue = nullptr;
    ID3D12Device *device = nullptr;
    if (SUCCEEDED(chain->GetDevice(IID_PPV_ARGS(&queue))) && queue) {
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || !device) { queue->Release(); return false; }
    } else {
        static int why = 0;   // log the reason a few times, not every frame
        const HRESULT hr = chain->GetDevice(IID_PPV_ARGS(&device));
        if (FAILED(hr) || !device) { if (why++ < 3) mod::log("render12: swap chain has no D3D12 device (0x%08lx)", hr); return false; }
        queue = static_cast<ID3D12CommandQueue *>(InterlockedCompareExchangePointer(reinterpret_cast<void *volatile *>(&last_direct_queue), nullptr, nullptr));
        if (!queue) { device->Release(); if (why++ < 3) mod::log("render12: BeamNG's queue not seen yet"); return false; }
        ID3D12Device *queue_device = nullptr;
        const bool same = SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&queue_device))) && queue_device == device;
        release(queue_device);
        if (!same) { device->Release(); if (why++ < 3) mod::log("render12: last queue belongs to another device"); return false; }
        queue->AddRef();
    }
    c.device = device;
    c.queue = queue;
    chain->QueryInterface(IID_PPV_ARGS(&c.chain));
    if (!c.chain || !init_context()) { mod::log("render12: could not set up"); return false; }
    mod::log("render12: attached to BeamNG's D3D12 swap chain");
    return true;
}

bool draw(const game::Scene &scene, const assets::Data *data, float aspect, bool &drew_sonic)
{
    drew_sonic = false;
    if (!ensure_targets()) return false;
    Frame &f = c.frames[c.frame_index % kFrames];
    if (c.fence->GetCompletedValue() < f.fence) return true;   // GPU still busy with this slot: skip, never stall BeamNG
    if (c.staging_fence && c.fence->GetCompletedValue() >= c.staging_fence) {
        for (auto *s : c.staging) s->Release();
        c.staging.clear(); c.staging_fence = 0;
    }
    const UINT index = c.chain->GetCurrentBackBufferIndex();
    if (index >= c.backbuffers.size()) return true;
    if (FAILED(f.allocator->Reset()) || FAILED(c.list->Reset(f.allocator, nullptr))) return false;
    f.used = 0;
    const bool need_upload = data && !c.uploaded;
    if (need_upload) {
        if (!upload_assets(*data)) { c.list->Close(); return false; }
        c.uploaded = true;
    }
    if (!c.uploaded) { c.list->Close(); return true; }
    ID3D12Resource *target = c.backbuffers[index];
    const bool blur = scene.camera && scene.blur > 0.01f && c.post && ensure_copy(target);
    if (blur) {
        barrier(target, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(c.copy, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        c.list->CopyResource(c.copy, target);
        barrier(c.copy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        barrier(target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    } else {
        barrier(target, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    auto rtv = c.rtv->GetCPUDescriptorHandleForHeapStart(); rtv.ptr += SIZE_T(index) * c.rtv_stride;
    auto dsv = c.dsv->GetCPUDescriptorHandleForHeapStart();
    c.list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    c.list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0, nullptr);
    D3D12_VIEWPORT vp{0, 0, float(c.width), float(c.height), 0, 1};
    D3D12_RECT scissor{0, 0, LONG(c.width), LONG(c.height)};
    c.list->RSSetViewports(1, &vp); c.list->RSSetScissorRects(1, &scissor);
    c.list->SetGraphicsRootSignature(c.root);
    ID3D12DescriptorHeap *heaps[] = {c.srv};
    c.list->SetDescriptorHeaps(1, heaps);
    c.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if (blur) {
        draw::DrawConstants dc{};
        std::memcpy(dc.model_view, draw::kIdentity, 64); std::memcpy(dc.proj, draw::kIdentity, 64);
        dc.tint[0] = scene.blur_center[0]; dc.tint[1] = scene.blur_center[1]; dc.tint[2] = scene.blur * sheets::tuning::blurStrength;
        dc.params[3] = float(c.color_mode);
        const auto cb = push(f, &dc, sizeof(dc));
        if (cb) {
            c.list->SetPipelineState(c.post);
            c.list->SetGraphicsRootConstantBufferView(0, cb);
            c.list->SetGraphicsRootConstantBufferView(1, cb);
            c.list->SetGraphicsRootDescriptorTable(2, srv_handle(c.copy_slot));
            c.list->DrawInstanced(3, 1, 0, 0);
        }
    }
    if (scene.camera && scene.shadow) draw_flat(f, draw::shadow_vertices(scene), scene.view, scene.proj, 1, c.white_texture);
    if (scene.camera && scene.sonic_visible && !scene.pose.empty()) {
        std::vector<float> bones(768 * 4, 0.0f);
        std::memcpy(bones.data(), scene.pose.data(), std::min(bones.size(), scene.pose.size()) * 4);
        const auto b = push(f, bones.data(), bones.size() * 4);
        if (b && scene.sonic_shadow && c.shadow) {
            float mv[16];
            draw::mul(scene.sonic_shadow_model, scene.view, mv);
            draw::DrawConstants dc{};
            std::memcpy(dc.model_view, mv, 64); std::memcpy(dc.proj, scene.proj, 64);
            dc.tint[3] = scene.sonic_shadow_alpha; dc.params[3] = float(c.color_mode);
            std::memcpy(dc.clip, scene.clip_plane, sizeof(dc.clip));
            const auto cb = push(f, &dc, sizeof(dc));
            if (cb) {
                c.list->SetPipelineState(c.shadow);
                c.list->OMSetStencilRef(0);
                c.list->SetGraphicsRootConstantBufferView(0, cb);
                c.list->SetGraphicsRootConstantBufferView(1, b);
                c.list->SetGraphicsRootDescriptorTable(2, srv_handle(c.white_texture));
                c.list->IASetVertexBuffers(0, 1, &c.sonic.vbv);
                c.list->IASetIndexBuffer(&c.sonic.ibv);
                for (auto &m : c.sonic.meshes) c.list->DrawIndexedInstanced(m.index_count, 1, m.first_index, INT(m.base_vertex), 0);
            }
        }
        // Power Boost trail behind Sonic (drawn before him, so he stays in front of it)
        if (!scene.beam.empty()) draw_flat(f, draw::as_vertices(scene.beam), scene.view, scene.proj, 5, c.white_texture);
        if (!scene.trail.empty()) draw_flat(f, draw::as_vertices(scene.trail), scene.view, scene.proj, 4, c.trail_texture, c.glow);
        if (!scene.arcs.empty()) draw_flat(f, draw::as_vertices(scene.arcs), scene.view, scene.proj, 4, c.arc_texture, c.glow);
        if (b) {
            std::memcpy(draw::active_clip, scene.clip_plane, sizeof(draw::active_clip));
            if (!scene.sonic_hidden) draw_mesh(f, c.sonic, b, scene.sonic_model, scene.view, scene.proj, 1, 0);
            std::memset(draw::active_clip, 0, sizeof(draw::active_clip));
            drew_sonic = true;
            if (c.has_aura && scene.aura > 0.01f) {
                if (scene.aura_dome && c.has_dome) draw_dome(f, scene);
                else { draw_aura(f, b, scene, 0); draw_aura(f, b, scene, 1); }
            }
        }
        // Frontiers' spin ball shell around Sonic curled up in a jump (its own bones)
        if (c.has_ball && scene.ball_visible && !scene.ball_pose.empty()) {
            std::vector<float> ball_bones(768 * 4, 0.0f);
            std::memcpy(ball_bones.data(), scene.ball_pose.data(), std::min(ball_bones.size(), scene.ball_pose.size()) * 4);
            if (const auto bb = push(f, ball_bones.data(), ball_bones.size() * 4)) draw_mesh(f, c.ball, bb, scene.ball_model, scene.view, scene.proj, scene.ball_alpha, 0);
        }
    }
    if (!scene.rings.empty()) {
        static const std::vector<float> identity = draw::identity_bones();
        const auto b = push(f, identity.data(), identity.size() * 4);
        for (auto &inst : scene.rings) {
            if (!b) break;
            if (inst.screen) draw_mesh(f, c.ring, b, inst.model, nullptr, draw::kIdentity, inst.alpha, 0.25f);
            else if (scene.camera) draw_mesh(f, c.ring, b, inst.model, scene.view, scene.proj, inst.alpha, 0.25f);
        }
    }
    if (scene.camera && !scene.sprites.empty()) {
        draw_flat(f, draw::sprite_vertices(scene, 0), scene.view, scene.proj, 2, c.glow_texture, c.glow);
        draw_flat(f, draw::sprite_vertices(scene, 1), scene.view, scene.proj, 3, c.wave_texture, c.glow);
        draw_flat(f, draw::sprite_vertices(scene, 4), scene.view, scene.proj, 1, c.white_texture, c.glow);   // jump ball glow: soft round, untextured
        draw_flat(f, draw::sprite_vertices(scene, 2), scene.view, scene.proj, 4, c.bolt_texture, c.glow);
        draw_flat(f, draw::sprite_vertices(scene, 3), scene.view, scene.proj, 4, c.orb_texture, c.glow);
    }
    if (!scene.texts.empty()) draw_flat(f, draw::text_vertices(scene, aspect), draw::kIdentity, draw::kIdentity, 0, c.font_texture);
    barrier(target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    if (FAILED(c.list->Close())) return false;
    ID3D12CommandList *lists[] = {c.list};
    real_execute(c.queue, 1, lists);
    const UINT64 value = c.next_fence++;
    c.queue->Signal(c.fence, value);
    f.fence = value;
    if (need_upload) c.staging_fence = value;
    ++c.frame_index;
    return true;
}

void before_resize()
{
    wait_idle();
    release_targets();
}

} // namespace r12
