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

    // M2 version gate: the stock loader accepts only the 264 version. Relaxing these two branches lets
    // it also accept the modern (MD21-era) inner MD20 versions.
    constexpr uintptr_t VersionGate_InitJA  = 0x0083CF51;   // version-too-high branch -> NOP x6
    constexpr uintptr_t VersionGate_AnimJBE = 0x0083C745;   // anim-parse branch        -> JMP short

    // .skin filename builder __cdecl(modelPathStem = CM2Model+0x3c, profileIndex, outBuf): copies the model
    // path into outBuf, strips the extension, appends "%02d.skin". outBuf is a fixed engine buffer (0x108).
    constexpr uintptr_t M2_BuildSkinPath = 0x00835A80;
    // CM2Model skin-profile loader __thiscall(model, profileIndex): builds the NN.skin path, opens+maps the
    // file, parses it, attaches the parsed profile at model+0x170 SYNCHRONOUSLY, then wires and schedules an
    // async finalize record at model+0xc. The finalize is idempotent: it skips when model+8 bit1 is already
    // set, so re-invoking this loader runs the finalize at most once.
    constexpr uintptr_t CM2_LoadSkinProfile = 0x0083CB40;
    // Size of the engine .skin path buffer the loader passes to M2_BuildSkinPath.
    constexpr uint32_t  M2_SkinPathBufSize = 0x108;

    // Storm file API. All __stdcall (callee-cleaned). Hooked by Features/Storage to serve reads from the host.
    // Open __stdcall(archiveOrNull, path, flag, &handle) -> nonzero, fills handle.
    constexpr uintptr_t Storage_FileOpen  = 0x00424B50;
    // Second open entry: thin SFileOpenFileEx __stdcall(archiveOrNull, name, flags, &handle) -> bool.
    constexpr uintptr_t Storage_FileOpen2 = 0x00422040;
    // Size __stdcall(handle, &sizeHigh) -> file size low dword.
    constexpr uintptr_t Storage_FileSize  = 0x004218C0;
    // Read __stdcall(handle, dst, len, &read|0, 0, 0) -> nonzero.
    constexpr uintptr_t Storage_FileRead  = 0x00422530;
    // Seek __stdcall(handle, distLow, distHigh, method) -> newPosLow; method 0=BEGIN,1=CURRENT,2=END.
    constexpr uintptr_t Storage_FileSeek  = 0x00421BB0;
    // Close __stdcall(handle).
    constexpr uintptr_t Storage_FileClose = 0x00422910;
    // Flag: open and load the whole file into the handle buffer.
    constexpr uint32_t  Storage_OpenFlag  = 0x20000;

    // --- M2 external .anim ---
    // External .anim read-completion callback __cdecl(node): runs once after the file bytes are read into
    // the load buffer and BEFORE the M2Track offsets are rebased against it. node+0x08 =
    // I/O record (record+0x04 = buffer ptr, record+0x08 = size), node+0x10 = seqIdx, node+0x0c = model.
    constexpr uintptr_t CM2_AnimLoadComplete = 0x0083D840;
    // External .anim loader __thiscall(model, seqIdx): follows the sequence alias chain to the real
    // (id, subId), builds "<stem>%04d-%02d.anim", opens it, allocates a buffer, and schedules the async
    // read whose completion (CM2_AnimLoadComplete) rebases the per-sequence track inner slots. Returns the
    // load node on success or 0 (logs "File not found") if the file is absent. The engine only calls this
    // lazily when an external sequence is set to play; for a model whose only played sequence is an embedded
    // stub it never fires, so the loader is driven explicitly at init for present external sequences.
    constexpr uintptr_t CM2_SequenceLoad = 0x0083DA10;
    // .anim filename builder __cdecl(stem = CM2Model+0x3c, id, subId, outBuf): copies the stem, strips the
    // extension, appends "%04d-%02d.anim". outBuf must be at least M2_SkinPathBufSize.
    constexpr uintptr_t M2_BuildAnimPath = 0x00835A20;
    // I/O record field offsets used by the rebase: the .anim buffer base and its size.
    constexpr uint32_t  AnimRecord_Buffer = 0x04;
    constexpr uint32_t  AnimRecord_Size   = 0x08;
    // Load node field: the I/O record pointer.
    constexpr uint32_t  AnimNode_Record   = 0x08;
    // Per-sequence track de-relocator __thiscall(model, seqIdx, buffer, size): validates the buffer holds an
    // inner MD20 (>=0x108) and rebases sequence seqIdx's track inner slots += buffer, then sets seq.flags
    // (clear 0x10, set 0x20). The size arg is consumed by the callee's stack cleanup but unused internally.
    // Restores its scratch globals before returning, so a direct synchronous call is self-contained. Returns
    // nonzero on success, 0 ("Corrupt model sequence data") on a malformed buffer.
    constexpr uintptr_t CM2_PerSeqDeReloc = 0x0083C6E0;
    // The .anim buffer allocator __cdecl(size, name, line): allocates size+0x10, returns a 16-byte aligned
    // pointer carrying a back-shift byte at [ptr-1]. The engine rebases track offsets relative to this base.
    constexpr uintptr_t M2_AnimBufferAlloc = 0x0083DE50;
    // CM2Model flags field: bit 2 selects the storage open flag for sibling files (.skin/.anim).
    constexpr uint32_t  Model_Flags          = 0x08;

    // --- M2 per-batch alpha ---
    // Shared per-batch alpha/material/cull setter (used by both the creature and doodad draw paths). It
    // chooses the alpha-test reference from the material blend mode and pushes it to the device.
    constexpr uintptr_t CM2_SetupBatchAlpha = 0x0081FE90;
    // Pushes the alpha-test reference to the device (pixel constant / D3DRS_ALPHAREF).
    constexpr uintptr_t CM2_PushAlphaRef    = 0x00873BA0;

    // --- M2 bone palette (per-frame skinning matrices; the hook point for bone-physics) ---
    // Per-instance bone-palette build __thiscall(instance, ...): fills the model-space 4x4 bone matrices
    // for one instance from the current animation pose. Runs each frame before the batch draw uploads the
    // palette to the vertex shader, so its return is the slot to adjust bones (e.g. physics). Not recursive.
    constexpr uintptr_t CM2_BuildBonePalette = 0x0082F0F0;
    // CM2Instance fields.
    constexpr uint32_t  Instance_Model       = 0x2C;   // -> CM2Model
    constexpr uint32_t  Instance_BonePalette = 0x98;   // -> bone matrices, row-major 4x4, stride 0x40
    // CM2Model fields: the model path stem (no extension; used to derive sibling files like .phys) and the
    // parsed M2 header.
    constexpr uint32_t  Model_PathStem       = 0x3C;
    constexpr uint32_t  Model_Header         = 0x150;
    // M2 header fields: global flags (bit 0x20 = the model carries physics) and the bone count.
    constexpr uint32_t  Header_GlobalFlags   = 0x10;
    constexpr uint32_t  Header_BoneCount     = 0x2C;
    constexpr uint32_t  Header_BoneArray     = 0x30;   // -> M2 bone records (post-fixup data ptr)
    constexpr uint32_t  BonePalette_Stride   = 0x40;
    // M2 bone record (file struct in Header_BoneArray): stride and flags field.
    constexpr uint32_t  M2Bone_Stride        = 0x58;
    constexpr uint32_t  M2Bone_Flags         = 0x04;   // u32 bone flags
    constexpr uint32_t  M2Bone_Parent        = 0x08;   // int16 parent bone index (0xFFFF = root)
    constexpr uint32_t  M2Bone_Pivot         = 0x4C;   // C3Vector pivot (bone origin in bind space)
    // M2 bone flags: 0x78 = billboard bits (spherical 0x8 + cylindrical-lock x/y/z 0x10/0x20/0x40).
    constexpr uint32_t  kBoneBillboardMask   = 0x78;

    // Vec3 M2Track evaluator __thiscall-ish(model, runtimeBone, track, out, baseValue): samples a
    // translation/scale track for the current animation index and writes the interpolated vec3 into out.
    // Reached per bone and per light each frame from CM2_BuildBonePalette. Hooked to guard against an
    // un-rebased external-.anim inner values pointer (degrade to the base pose instead of faulting).
    constexpr uintptr_t CM2Track_EvalVec3    = 0x0082B0A0;
    // M2TrackBase field offsets the guard reads: values outer M2Array {count,offset} at +0x0C/+0x10.
    constexpr uint32_t  M2Track_ValuesCount  = 0x0C;
    constexpr uint32_t  M2Track_ValuesPtr    = 0x10;
    // Timestamps outer M2Array {count,offset} at +0x04/+0x08; read before the values to find the keyframe.
    constexpr uint32_t  M2Track_TimestampsCount = 0x04;
    constexpr uint32_t  M2Track_TimestampsPtr   = 0x08;
    // Runtime-bone field: the current animation index used to pick the per-animation inner slot.
    constexpr uint32_t  RuntimeBone_AnimIdx  = 0x44;
    // One Vec3 value keyframe = 3 floats; one timestamp = a u32 ms.
    constexpr uint32_t  M2Track_Vec3Stride   = 0x0C;
    constexpr uint32_t  M2Track_TimestampStride = 0x04;

    // Quaternion M2Track evaluator __cdecl(model, runtimeBone, track, out, baseValue): samples a bone
    // rotation track for the current animation index and writes the interpolated quaternion into out[2..5].
    // The keyframe-search helper it calls is shared with the Vec3 path; reached per bone each frame from
    // CM2_BuildBonePalette. Hooked with the same guard against an un-rebased external-.anim inner pointer.
    constexpr uintptr_t CM2Track_EvalQuat    = 0x00828680;
    // One compressed quaternion value keyframe = 4 int16 = 8 bytes.
    constexpr uint32_t  M2Track_QuatStride   = 0x08;

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
