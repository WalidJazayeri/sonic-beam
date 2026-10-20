// SPDX-License-Identifier: MIT
// Developer check: compile every entry point of the shared HLSL (shared_draw.hpp) the way the
// renderers do at run time, so a shader mistake shows up at build time instead of in BeamNG.
#include <windows.h>
#include <d3dcompiler.h>
#include <cstdio>
#include "../src/shared_draw.hpp"

int main()
{
    const char *entries[] = {"vsMesh", "psMesh", "vsFlat", "psFlat", "vsAura", "psAura", "vsPost", "psPost", "psShadow"};
    int failed = 0;
    for (const char *e : entries) {
        ID3DBlob *code = nullptr, *errors = nullptr;
        const char *target = e[0] == 'v' ? "vs_5_0" : "ps_5_0";
        if (FAILED(D3DCompile(draw::kShader, sizeof(draw::kShader) - 1, "sonic_beam", nullptr, nullptr, e, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
            std::printf("%s: %s\n", e, errors ? static_cast<const char *>(errors->GetBufferPointer()) : "failed");
            ++failed;
        }
        if (code) code->Release();
        if (errors) errors->Release();
    }
    if (failed) std::printf("%d shader(s) failed\n", failed);
    else std::printf("all %d shaders compile\n", int(sizeof(entries) / sizeof(*entries)));
    return failed ? 1 : 0;
}
