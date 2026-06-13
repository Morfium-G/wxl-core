#pragma once

#include <cstdint>

namespace wraith::offsets
{
    // Absolute addresses / structure-field offsets in the target client process that the in-memory hooks
    // and typed views below need. Each name is the engine routine or field the value refers to.

    // --- M2 load / setup ---
    // CM2Model::Init: parses an M2 file and builds the runtime model.
    constexpr uintptr_t CM2Model_Init = 0x0083CF00;
    // CM2Model skin-profile finalizer: runs once after the skin sub-arrays are resolved and before the
    // shader-id passes size their parallel batch blocks. The point to rebuild the material contract a
    // modern skin does not carry (see Features/M2/Modern).
    constexpr uintptr_t CM2Model_FinalizeSkin = 0x00837A40;

    // M2 version gate: the stock loader accepts only the WotLK version. Relaxing these two branches lets
    // it also accept the modern (MD21-era) inner MD20 versions.
    constexpr uintptr_t VersionGate_InitJA  = 0x0083CF51;   // version-too-high branch -> NOP x6
    constexpr uintptr_t VersionGate_AnimJBE = 0x0083C745;   // anim-parse branch        -> JMP short

    // --- M2 per-batch alpha ---
    // Shared per-batch alpha/material/cull setter (used by both the creature and doodad draw paths). It
    // chooses the alpha-test reference from the material blend mode and pushes it to the device.
    constexpr uintptr_t CM2_SetupBatchAlpha = 0x0081FE90;
    // Pushes the alpha-test reference to the device (pixel constant / D3DRS_ALPHAREF).
    constexpr uintptr_t CM2_PushAlphaRef    = 0x00873BA0;

    // --- M2 ribbon ---
    // Ribbon-emitter de-relocator: pointer-fixes each ribbon's sub-array offsets (textureIndices,
    // materialIndices), stride 0xb0. Hooked to expand a modern ribbon's short materialIndices so every
    // layer reads materialIndices[0] (otherwise a per-layer parallel read runs out of bounds).
    constexpr uintptr_t M2Ribbon_DeRelocate = 0x0083A460;
    // Ribbon emitter draw: __thiscall(emitter, stateBlock). Builds the strip and binds one texture per
    // layer to sampler s0. Hooked by the multi-texture ribbon combine (Features/Ribbon).
    constexpr uintptr_t CM2_RibbonDraw = 0x00980B70;
    // CRibbonEmitter field offsets, relative to the emitter base.
    constexpr uint32_t  Ribbon_LayerCount   = 0x118;   // layer count (the draw-loop bound)
    constexpr uint32_t  Ribbon_TexHandlePtr = 0x12c;   // pointer to the per-layer texture-handle array (stride 4)

    // --- engine texture / sampler helpers (used by the ribbon combine) ---
    // Resolve a texture handle to the internal texture object the sampler bind expects.
    constexpr uintptr_t Ribbon_TexResolve = 0x004B6CB0;
    // Bind a texture to a sampler selector: __thiscall(device, selector, resolvedTexture). s0 = 0x15.
    constexpr uintptr_t Gx_SamplerBind    = 0x00685F50;

    // --- graphics device ---
    // Pointer to the engine graphics device; the IDirect3DDevice9* lives at the device + GxD3DDeviceField.
    constexpr uintptr_t GxDevicePtr          = 0x00C5DF88;
    constexpr uint32_t  GxD3DDeviceField     = 0x397C;
    // Cached backbuffer + world depth surfaces on the engine device.
    constexpr uint32_t  GxD3DBackbufferField = 0x3B3C;
    constexpr uint32_t  GxD3DDepthField      = 0x3B40;

    // Standard IDirect3DDevice9 vtable index for DrawIndexedPrimitive.
    constexpr int VtDrawIndexedPrimitive = 82;
}
