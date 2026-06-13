#include "Features/Ribbon/Ribbon.hpp"

#include "Engine/Offsets.hpp"
#include "Engine/Gx.hpp"
#include "Core/Hook.hpp"
#include "Core/Logger.hpp"

#include <windows.h>
#include <d3d9.h>
#include <cstdint>

using namespace wraith;

// A modern multi-texture ribbon layers 3 textures meant to combine as tex0*tex1*tex2*color*4. The host
// engine draws an N-texture ribbon as N sequential single-texture passes on sampler s0, which cannot
// reproduce that product. The product is expressible in fixed-function (MODULATE, MODULATE, MODULATE4X),
// so a 3+ layer ribbon is drawn in ONE pass: bind the 3 resolved textures to s0/s1/s2 and set the stage
// combine at the D3D9 draw call. The engine's own transforms place the strip and its additive blend is
// already set, so there is no shader swap and no constant push - only the texture-stage combine changes.
namespace
{
    // Set while a modern multi-texture ribbon draw is in flight; the draw override applies the 3-stage
    // combine only then, and saves/restores stage state so other draws are unaffected.
    volatile bool g_ribbonModern = false;

    void* GxDevice() { return *reinterpret_cast<void**>(offsets::GxDevicePtr); }

    // Sampler selectors for the engine bind path (s0 = 0x15, consecutive).
    constexpr uint32_t kSelS1 = 0x16;
    constexpr uint32_t kSelS2 = 0x17;

    // Engine texture-handle resolver: a texture handle -> the internal texture object the sampler bind
    // expects (the same resolve the native ribbon loop runs per layer).
    using TexResolveFn = void*(__cdecl*)(void*, int, int);
    TexResolveFn TexResolve = reinterpret_cast<TexResolveFn>(offsets::Ribbon_TexResolve);
    // Engine sampler bind: __thiscall(device, selector, resolvedTexture).
    using SamplerBindFn = void(__fastcall*)(void* self, void* edx, uint32_t selector, void* tex);
    SamplerBindFn SamplerBind = reinterpret_cast<SamplerBindFn>(offsets::Gx_SamplerBind);

    // --- D3D9 DrawIndexedPrimitive override (the last instant before the GPU draw) ---
    using DipFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
    DipFn g_origDip = nullptr;
    bool  g_dipPatched = false;

    HRESULT STDMETHODCALLTYPE HookDip(IDirect3DDevice9* dev, D3DPRIMITIVETYPE type, INT base, UINT minIdx,
                                      UINT numVerts, UINT startIdx, UINT primCount)
    {
        // Modern multi-texture ribbon: all 3 textures are bound (s0 by the engine's loop, s1/s2 pre-bound
        // by the draw hook). Apply tex0*tex1*tex2*color*4 in fixed-function: MODULATE(tex0,diffuse),
        // MODULATE(tex1,current), MODULATE4X(tex2,current). Stage state is saved and restored so the next
        // draw is unaffected; the additive frame blend is already set by the engine's ribbon draw.
        if (g_ribbonModern)
        {
            DWORD s[4][4];   // [stage][COLOROP, COLORARG1, COLORARG2, ALPHAOP] saved for stages 0..3
            for (DWORD st = 0; st < 4; ++st)
            {
                dev->GetTextureStageState(st, D3DTSS_COLOROP,   &s[st][0]);
                dev->GetTextureStageState(st, D3DTSS_COLORARG1, &s[st][1]);
                dev->GetTextureStageState(st, D3DTSS_COLORARG2, &s[st][2]);
                dev->GetTextureStageState(st, D3DTSS_ALPHAOP,   &s[st][3]);
            }

            const D3DTEXTUREOP op[3] = { D3DTOP_MODULATE, D3DTOP_MODULATE, D3DTOP_MODULATE4X };
            for (DWORD st = 0; st < 3; ++st)
            {
                dev->SetTextureStageState(st, D3DTSS_COLOROP,   op[st]);
                dev->SetTextureStageState(st, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                dev->SetTextureStageState(st, D3DTSS_COLORARG2, st == 0 ? D3DTA_DIFFUSE : D3DTA_CURRENT);
                dev->SetTextureStageState(st, D3DTSS_ALPHAOP,   op[st]);
                dev->SetTextureStageState(st, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
                dev->SetTextureStageState(st, D3DTSS_ALPHAARG2, st == 0 ? D3DTA_DIFFUSE : D3DTA_CURRENT);
            }
            dev->SetTextureStageState(3, D3DTSS_COLOROP, D3DTOP_DISABLE);   // stop after stage 2

            HRESULT hr = g_origDip(dev, type, base, minIdx, numVerts, startIdx, primCount);

            for (DWORD st = 0; st < 4; ++st)
            {
                dev->SetTextureStageState(st, D3DTSS_COLOROP,   s[st][0]);
                dev->SetTextureStageState(st, D3DTSS_COLORARG1, s[st][1]);
                dev->SetTextureStageState(st, D3DTSS_COLORARG2, s[st][2]);
                dev->SetTextureStageState(st, D3DTSS_ALPHAOP,   s[st][3]);
            }
            return hr;
        }
        return g_origDip(dev, type, base, minIdx, numVerts, startIdx, primCount);
    }

    // Patch this device's DrawIndexedPrimitive slot once. Under D3D9On12 each device has its own vtable,
    // so patch the live engine device the moment it exists.
    void PatchDip(IDirect3DDevice9* dev)
    {
        if (g_dipPatched || !dev) return;
        void** vt = *reinterpret_cast<void***>(dev);
        DWORD old = 0;
        VirtualProtect(&vt[offsets::VtDrawIndexedPrimitive], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
        g_origDip = reinterpret_cast<DipFn>(vt[offsets::VtDrawIndexedPrimitive]);
        vt[offsets::VtDrawIndexedPrimitive] = reinterpret_cast<void*>(&HookDip);
        VirtualProtect(&vt[offsets::VtDrawIndexedPrimitive], sizeof(void*), old, &old);
        g_dipPatched = true;
        WLOG_INFO("ribbon: DrawIndexedPrimitive hooked on engine device %p", dev);
    }

    // Bind layers 1 and 2 to s1/s2 through the engine (the native draw loop only binds s0, so these
    // survive into the single pass). The per-layer handle array is reached through the pointer at
    // Ribbon_TexHandlePtr. Only called with layerCount >= 3, so [1] and [2] are in bounds.
    bool BindExtraSamplers(void* gx, const uint8_t* emitter)
    {
        const void* const* arr =
            *reinterpret_cast<const void* const* const*>(emitter + offsets::Ribbon_TexHandlePtr);
        if (!arr) return false;
        void* h1 = const_cast<void*>(arr[1]);
        void* h2 = const_cast<void*>(arr[2]);
        if (!h1 || !h2) return false;
        void* t1 = TexResolve(h1, 0, 0);
        void* t2 = TexResolve(h2, 0, 0);
        if (!t1 || !t2) return false;
        SamplerBind(gx, nullptr, kSelS1, t1);
        SamplerBind(gx, nullptr, kSelS2, t2);
        return true;
    }

    // Log a given emitter's first bridged draw once (so the line is not spammed every frame).
    constexpr int kRibbonLoggedMax = 16;
    const void* g_ribbonLogged[kRibbonLoggedMax] = {};
    int         g_ribbonLoggedCount = 0;

    void LogRibbonOnce(const void* emitter, uint32_t layerCount)
    {
        for (int i = 0; i < g_ribbonLoggedCount; ++i)
            if (g_ribbonLogged[i] == emitter) return;
        if (g_ribbonLoggedCount >= kRibbonLoggedMax) return;
        g_ribbonLogged[g_ribbonLoggedCount++] = emitter;
        WLOG_INFO("ribbon: emitter %p drawn as single-pass 3-texture combine (%u textures)",
                  emitter, layerCount);
    }

    // Ribbon emitter draw: __thiscall(emitter, stateBlock). For a 3+ layer ribbon, pre-bind s1/s2, flag
    // the draw so the override applies the 3-stage combine, and clamp the layer count to 1 so the
    // engine's own draw runs exactly one pass; otherwise run the native N-pass draw untouched.
    using RibbonDrawFn = int(__fastcall*)(void* self, void* edx, void* stateBlock);
    RibbonDrawFn g_ribbonDraw = nullptr;

    // ON: 3+ layer ribbons use the single-pass combine. OFF: native multi-pass (additive) ribbon.
    constexpr bool kRibbonMultiTex = true;

    int __fastcall HookRibbonDraw(void* self, void* edx, void* stateBlock)
    {
        g_ribbonModern = false;

        uint8_t* emitter = static_cast<uint8_t*>(self);
        bool bridged = false;
        uint32_t savedLayerCount = 0;
        uint32_t* layerCountPtr = nullptr;

        if (emitter)
        {
            __try
            {
                layerCountPtr = reinterpret_cast<uint32_t*>(emitter + offsets::Ribbon_LayerCount);
                uint32_t layers = *layerCountPtr;
                // The only modern multi-tap ribbon is the 3-texture variant; engage the single-pass
                // combine only for >= 3 layers (also keeps the [1]/[2] handle reads in bounds). 1-2 layer
                // ribbons keep the native N-pass draw.
                if (kRibbonMultiTex && layers >= 3)
                {
                    void* gxDev = GxDevice();
                    if (gxDev)
                    {
                        PatchDip(reinterpret_cast<IDirect3DDevice9*>(
                            gx::device() ? gx::device()->d3d9() : nullptr));
                        if (BindExtraSamplers(gxDev, emitter))
                        {
                            g_ribbonModern = true;
                            savedLayerCount = layers;
                            *layerCountPtr = 1;     // run the engine's own draw as ONE pass
                            bridged = true;
                            LogRibbonOnce(emitter, layers);
                        }
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { bridged = false; g_ribbonModern = false; }
        }

        int r = g_ribbonDraw(self, edx, stateBlock);

        if (bridged && layerCountPtr)
            __try { *layerCountPtr = savedLayerCount; }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_ribbonModern = false;
        return r;
    }
}

namespace wraith::features::ribbon
{
    void Install()
    {
        hook::Install("CM2::RibbonDraw", offsets::CM2_RibbonDraw,
                      &HookRibbonDraw, reinterpret_cast<void**>(&g_ribbonDraw));
        WLOG_INFO("ribbon: multi-texture combine installed");
    }
}
