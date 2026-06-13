#pragma once

#include "Features/M2/M2.hpp"

#include <cstdint>

// The modern contract: how an MD21-chunked (post-264) model differs from the 264 contract.
// wowdev: inner MD20 version != expansion. Versions 272-274 span Legion/BfA/SL (and 272 is also MoP/WoD),
// so dispatch on the [kVersionMin, kVersionMax] range and trust the model, not a single number.
namespace wraith::m2::modern
{
    constexpr uint32_t kVersionMin = 272;
    constexpr uint32_t kVersionMax = 274;
    constexpr uint16_t kShaderMin  = 0x8000;   // a texunit shaderId >= this is a CM2Shared effect index

    // M2ParticleOld emitter stride. wowdev M2: 476 by default; 492 when version > 271 (or flag 0x200).
    // The native particle de-relocator hardcodes 476, so a v272 body's emitters must be slid down from 492
    // to 476 before native Init or its 2nd-emitter bounds-check reads garbage and rejects.
    constexpr uint32_t kParticleStride264    = 0x1dc;
    constexpr uint32_t kParticleStrideModern = 0x1ec;
    constexpr uint32_t kFlagParticleExt      = 0x200;   // global_flags bit forcing the 492-byte emitter

    // M2ParticleOld.textureId at emitter+0x16. 264 reads it as a flat uint16 index into header.textures.
    // The multi-texture form (emitter flag 0x10000) instead packs three 5-bit texture ids; read flat by 264
    // it is a multi-thousand index that overruns the texture-handle table. Keep the first id, drop the rest.
    constexpr uint32_t kParticleTextureIdOff   = 0x16;
    constexpr uint32_t kParticleFlagMultiTex   = 0x10000;
    constexpr uint16_t kParticleTextureIdMask  = 0x1F;

    // M2ParticleOld.blendingType at emitter+0x28 (u8). The 264 M2-blend table is stride-7 (modes 0..6);
    // Cata+ BlendAdd (7) indexes OOB. BlendAdd = (ONE, INV_SRC_ALPHA) = the engine's mode 3 (device blend
    // index 10), so 7 is remapped to 3 (faithful), not approximated.
    constexpr uint32_t kParticleBlendOff       = 0x28;

    // Compressed gravity: a Cata+ emitter with flags 0x800000 stores its gravity M2Track keys not as plain
    // floats but as packed {int8 x, int8 y, int16 z} direction+magnitude. 264 predates this and reads the
    // 4 raw bytes as an IEEE float, which can be NaN and poison the particle position. The gravity track
    // body is at emitter+0x84; its val M2Array {count,ofs} at +0x90/+0x94 (two-level: outer indexed by
    // animation, inner = the 4-byte keys). Magnitude unit per wowdev = 0.04238648 yd/key-unit.
    constexpr uint32_t kParticleFlagCompressedGravity = 0x800000;
    constexpr uint32_t kParticleGravityValCountOff    = 0x90;
    constexpr uint32_t kParticleGravityValOfsOff      = 0x94;
    constexpr float    kParticleGravityMagUnit        = 0.04238648f;

    // Flipbook atlas: textureRows@+0x30 / textureCols@+0x32 (u16) subdivide the particle texture into
    // rows*cols cells. The head/tail cell M2PartTrack { M2Array times; M2Array keys } are at +0x13c / +0x14c
    // (keys = int16 cell indices at FBlock+0x8 count / +0xc ofs). Cata wraps the sampled cell by rows*cols;
    // 264 does not (cell>=cols maps to row>=1 -> V off the atlas). Wrap the keyframes at load.
    constexpr uint32_t kParticleTexRowsOff     = 0x30;
    constexpr uint32_t kParticleTexColsOff     = 0x32;
    constexpr uint32_t kParticleHeadCellOff    = 0x13c;
    constexpr uint32_t kParticleTailCellOff    = 0x14c;

    // M2Ribbon stride. The native ribbon de-relocator and the ribbon draw case both stride by 0xb0.
    // Unlike particles, the Cata+ tail (priorityPlane/ribbonColorIndex/
    // textureTransformLookupIndex, 4 bytes) lands in the 264 layout's trailing padding (0xac..0xb0), so the
    // modern stride EQUALS 264: 0xb0. Both names are kept explicit so a future version that does grow the
    // ribbon body only edits kRibbonStrideModern and CompactRibbons starts sliding.
    constexpr uint32_t kRibbonStride264    = 0xb0;
    constexpr uint32_t kRibbonStrideModern = 0xb0;

#pragma pack(push, 1)
    // The explicit fov float was dropped and an FoV animation track appended. The 0x54 track body is
    // identical to the 264 contract and its sub-array offsets are MD20-relative, so it compacts to
    // m2::M2Camera in place with no offset cascade.
    struct M2Camera
    {
        uint32_t     type;           // 0x00
        float        farClip;        // 0x04
        float        nearClip;       // 0x08
        M2CameraBody body;           // 0x0C
        uint8_t      fovTrack[0x14]; // 0x60  (appended FoV M2Track)
    };
#pragma pack(pop)

    static_assert(sizeof(M2Camera) == 0x74, "modern::M2Camera");
}

// Load-time transforms that turn a de-chunked modern MD20 into the 264 contract.
//   - ApplyFixups: M2Camera 0x74 -> 0x64 (the dropped fov float restored to a default). Runs in Init.
//   - RebuildMaterials: re-derive the 264 material contract the native skin passes expect. A modern .skin
//     encodes each batch's material in its own shaderId and leaves header.textureUnitLookup (0x88) empty;
//     the WotLK skin's first shader-id pass indexes textureUnitLookup[batch.texCoordCombo] and NULL-derefs.
//     Runs at the FinalizeSkin entry, where both the header (CM2Model+0x150) and the pointer-fixed skin
//     (CM2Model+0x170) are live and BEFORE the shader-id passes size their parallel batch blocks.
// [slackBegin, slackEnd) is the spare buffer past the inner MD20, available for table synthesis.
namespace wraith::features::m2::modern
{
    void ApplyFixups(wraith::m2::M2Header* md, uint32_t slackBegin, uint32_t slackEnd);
    void RebuildMaterials(wraith::m2::M2Header* md, wraith::m2::M2SkinProfile* skin, const char* name);

    // Post-de-relocation ribbon fix. A modern ribbon can have textureIndices.count > materialIndices.count;
    // the native ribbon setup reads materialIndices[i] in parallel with textureIndices[i], so layers past
    // materialIndices.count read out of bounds. Modern content uses materialIndices[0] for every layer.
    // Expand each short materialIndices to textureIndices.count entries, all = materialIndices[0].
    // Runs on the ribbon array AFTER the de-relocator pointer-fixed each ribbon's M2Arrays to raw pointers;
    // ribbon base = ribbonArray + i*0xb0, with textureIndices/materialIndices offset fields now raw pointers.
    void ExpandRibbonMaterials(uint8_t* ribbonArray, uint32_t ribbonCount);
}
