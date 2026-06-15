#pragma once

#include <cstddef>
#include <cstdint>

#include "Engine/View.hpp"

// Typed views of the M2 model format and the engine's runtime model objects.
//
// The engine's native parser expects ONE on-disk layout: the 3.3.5 MD20 version 264. That layout is the
// host CONTRACT below (M2Header + M2Batch + M2SkinSection + ...). A modern model is made to
// satisfy that contract in memory before the engine parses it: the MD21 container is de-chunked and each
// version's source layout (declared in that version's contract, e.g. m2::modern) is transformed into it.
namespace wraith::m2
{
    constexpr uint32_t kMagicMD20 = 0x3032444D;
    constexpr uint32_t kMagicMD21 = 0x3132444D;

    // The MD20 version of the 264 contract. Newer versions declare their own (see m2::modern).
    constexpr uint32_t kVersionWrath = 264;

    // global_flags bits.
    constexpr uint32_t kFlagUseTextureCombinerCombos = 0x8;

#pragma pack(push, 1)

    // An (count, offset) pair. In a file the offset is MD20-relative; the loader rewrites it to a raw
    // pointer in place once parsed.
    struct M2Array
    {
        uint32_t count;
        uint32_t offset;
    };

    // ---- 264 contract (the host MD20 layout, version 264) --------------------------------------------
    // The exact on-disk layout the engine's native parser reads. Every supported version is transformed
    // into these structs before the engine sees the model.

    // The MD20 header.
    struct M2Header
    {
        uint32_t magic;                  // 0x00
        uint32_t version;                // 0x04
        M2Array  name;                   // 0x08
        uint32_t globalFlags;            // 0x10
        M2Array  globalLoops;            // 0x14
        M2Array  sequences;              // 0x1C
        M2Array  sequenceLookup;         // 0x24
        M2Array  bones;                  // 0x2C
        M2Array  boneLookup;             // 0x34
        M2Array  vertices;               // 0x3C
        uint32_t numSkinProfiles;        // 0x44
        M2Array  colors;                 // 0x48
        M2Array  textures;               // 0x50
        M2Array  textureWeights;         // 0x58
        M2Array  textureTransforms;      // 0x60
        M2Array  textureReplacements;    // 0x68
        M2Array  materials;              // 0x70  (render flags + blend mode)
        M2Array  boneCombos;             // 0x78
        M2Array  textureCombos;          // 0x80  (texunit -> texture index)
        M2Array  textureUnitLookup;      // 0x88  (texCoordCombos: per-texture UV set, -1 = env)
        M2Array  textureWeightCombos;    // 0x90
        M2Array  textureTransformCombos; // 0x98
        float    boundingBox[6];         // 0xA0
        float    boundingSphereRadius;   // 0xB8
        float    collisionBox[6];        // 0xBC
        float    collisionSphereRadius;  // 0xD4
        M2Array  collisionIndices;       // 0xD8
        M2Array  collisionPositions;     // 0xE0
        M2Array  collisionNormals;       // 0xE8
        M2Array  attachments;            // 0xF0
        M2Array  attachmentLookup;       // 0xF8
        M2Array  events;                 // 0x100
        M2Array  lights;                 // 0x108
        M2Array  cameras;                // 0x110
        M2Array  cameraLookup;           // 0x118
        M2Array  ribbonEmitters;         // 0x120
        M2Array  particleEmitters;       // 0x128
        M2Array  textureCombinerCombos;  // 0x130  (only if globalFlags & kFlagUseTextureCombinerCombos)

        uint8_t* base() { return reinterpret_cast<uint8_t*>(this); }
    };

    // One animation sequence, 0x40 bytes (the engine strides sequences by 0x40). The 12340 engine reads a
    // SINGLE duration at 0x04 (build 12340 already uses the modern per-animation timeline, not the pre-Wrath
    // start/end pair), flags at 0x0C, the replay (min,max) range at 0x14/0x18, variationNext/aliasNext at
    // 0x3C/0x3E, and modulos the play time by duration. These match the modern (Cata+) layout for every
    // field the engine consumes, so the record is NOT down-converted. blendTime at 0x1C is a single u32 in
    // 264; Cata+/Legion split it into blendTimeIn(u16)+blendTimeOut(u16), so the dword is masked to its low
    // u16 to restore 264 semantics. flags bit 0x20 = data embedded in the .m2; clear = streamed from .anim.
    struct M2Sequence
    {
        uint16_t id;             // 0x00  AnimationData.dbc id
        uint16_t variationIndex; // 0x02
        uint32_t duration;       // 0x04  milliseconds (play time is taken modulo this)
        float    movespeed;      // 0x08
        uint32_t flags;          // 0x0C  bit 0x20 = embedded; clear = external .anim
        int16_t  frequency;      // 0x10
        uint16_t _pad12;         // 0x12
        uint32_t replayMin;      // 0x14
        uint32_t replayMax;      // 0x18
        uint32_t blendTime;      // 0x1C  264: u32; Cata+: blendTimeIn(u16)|blendTimeOut(u16)
        uint8_t  _bounds[0x1C];  // 0x20  M2Bounds (CAaBox + radius)
        int16_t  variationNext;  // 0x3C
        uint16_t aliasNext;      // 0x3E
    };

    // One render batch (texunit), 0x18 bytes.
    struct M2Batch
    {
        uint8_t  flags;                     // 0x00
        uint8_t  priorityPlane;             // 0x01
        uint16_t shaderId;                  // 0x02
        uint16_t skinSectionIndex;          // 0x04
        uint16_t geosetIndex;               // 0x06
        uint16_t colorIndex;                // 0x08
        uint16_t materialIndex;             // 0x0A  (index into M2Header.materials = render flags)
        uint16_t materialLayer;             // 0x0C
        uint16_t textureCount;              // 0x0E
        uint16_t textureComboIndex;         // 0x10
        uint16_t textureCoordComboIndex;    // 0x12
        uint16_t textureWeightComboIndex;   // 0x14
        uint16_t textureTransformComboIndex;// 0x16
    };

    // One ribbon emitter, 0xb0 bytes (the engine de-relocator and the ribbon draw case both stride by
    // 0xb0). textureIndices/materialIndices index header.textures/materials.
    // The seven M2Track bodies (interp u16, gseq u16, then timestamps+values M2Arrays) are pointer-fixed
    // by the de-relocator. Cata+ appends priorityPlane/ribbonColorIndex/textureTransformLookupIndex into
    // the 264 layout's trailing padding (0xac..0xb0), so the stride is unchanged.
    struct M2Ribbon
    {
        uint32_t ribbonId;            // 0x00
        uint32_t boneIndex;           // 0x04
        float    position[3];         // 0x08
        M2Array  textureIndices;      // 0x14  into header.textures
        M2Array  materialIndices;     // 0x1c  into header.materials
        uint8_t  colorTrack[0x14];    // 0x24
        uint8_t  alphaTrack[0x14];    // 0x38
        uint8_t  heightAboveTrack[0x14]; // 0x4c
        uint8_t  heightBelowTrack[0x14]; // 0x60
        float    edgesPerSecond;      // 0x74
        float    edgeLifetime;        // 0x78
        float    gravity;             // 0x7c
        uint16_t textureRows;         // 0x80
        uint16_t textureCols;         // 0x82
        uint8_t  texSlotTrack[0x14];  // 0x84
        uint8_t  visibilityTrack[0x14]; // 0x98
        int16_t  priorityPlane;       // 0xac  Cata+ (264 padding)
        int8_t   ribbonColorIndex;    // 0xae  Cata+
        int8_t   textureTransformLookupIndex; // 0xaf  Cata+
    };

    // One submesh, 0x30 bytes. level > 0 = a (level<<16 | id) sub-batch the 264 engine does not handle.
    struct M2SkinSection
    {
        uint16_t skinSectionId;     // 0x00
        uint16_t level;             // 0x02
        uint16_t vertexStart;       // 0x04
        uint16_t vertexCount;       // 0x06
        uint16_t indexStart;        // 0x08
        uint16_t indexCount;        // 0x0A
        uint16_t boneCount;         // 0x0C
        uint16_t boneComboIndex;    // 0x0E
        uint16_t boneInfluences;    // 0x10
        uint16_t centerBoneIndex;   // 0x12
        float    centerPosition[3]; // 0x14
        float    sortCenterPos[3];  // 0x20
        float    sortRadius;        // 0x2C
    };

    // The engine's parsed skin profile, hung off model+0x170. This is the LIVE in-memory object, NOT the
    // on-disk .skin: the parse prepends a 4-byte leading field, so every array sits 4 bytes higher than the
    // file layout (file vertexLookup M2Array@0x00 -> live vertexCount@0x04 / vertexLookup ptr@0x08, ...).
    // Array offsets are rewritten to raw pointers before the skin is finalized.
    // vertexLookup[i] -> global M2Vertex index. indices[] are GLOBAL local-vertex indices (into vertexLookup),
    // in [section.vertexStart, +vertexCount); the index builder subtracts vertexStart at upload. bones[i]
    // (uint8[4]) is per local vertex and holds LOCAL indices into the section's boneCombos slice
    // (header.boneCombos[section.boneComboIndex + bones[i][k]] = the global bone); these ARE the on-GPU vertex
    // bone indices the c31 palette is addressed by, NOT M2Vertex.boneIndices.
    struct M2SkinProfile
    {
        uint32_t       _lead;        // 0x00  parse-prepended leading field (unread by the draw/finalize paths)
        uint32_t       vertexCount;  // 0x04
        uint16_t*      vertexLookup; // 0x08
        uint32_t       indexCount;   // 0x0C
        uint16_t*      indices;      // 0x10
        uint32_t       boneCount;    // 0x14  (= vertexCount; bones[] is per local vertex)
        uint8_t*       bones;        // 0x18  (uint8[4] per local vertex; local boneCombos-slice indices)
        uint32_t       submeshCount; // 0x1C
        M2SkinSection* submeshes;    // 0x20
        uint32_t       batchCount;   // 0x24  (texunits)
        M2Batch*       batches;      // 0x28
        uint32_t       boneCountMax; // 0x2C  per-draw bone budget seed (used as a divisor at skin finalize)
    };

    // Shared camera track body (position/target/roll tracks + their bases). Identical across versions.
    struct M2CameraBody { uint8_t trackData[0x54]; };

    // M2Camera: the 264 contract carries an explicit fov float at 0x04.
    struct M2Camera
    {
        uint32_t     type;     // 0x00
        float        fov;      // 0x04
        float        farClip;  // 0x08
        float        nearClip; // 0x0C
        M2CameraBody body;     // 0x10
    };

    // The MD21 (Legion+) container: magic, then the inner self-relative MD20's size, then the MD20. Every
    // chunked model is de-chunked through this before its per-version fixup runs.
    struct Md21Header
    {
        uint32_t magic;       // 0x00 'MD21'
        uint32_t chunkSize;   // 0x04 size of the inner MD20
    };

#pragma pack(pop)

    static_assert(sizeof(M2Array) == 8, "M2Array");
    static_assert(offsetof(M2Header, version) == 0x04, "version");
    static_assert(offsetof(M2Header, globalFlags) == 0x10, "globalFlags");
    static_assert(offsetof(M2Header, vertices) == 0x3C, "vertices");
    static_assert(offsetof(M2Header, textures) == 0x50, "textures");
    static_assert(offsetof(M2Header, materials) == 0x70, "materials");
    static_assert(offsetof(M2Header, textureCombos) == 0x80, "textureCombos");
    static_assert(offsetof(M2Header, textureUnitLookup) == 0x88, "textureUnitLookup");
    static_assert(offsetof(M2Header, cameras) == 0x110, "cameras");
    static_assert(offsetof(M2Header, particleEmitters) == 0x128, "particleEmitters");
    static_assert(offsetof(M2Header, textureCombinerCombos) == 0x130, "textureCombinerCombos");
    static_assert(sizeof(M2Sequence) == 0x40, "M2Sequence");
    static_assert(offsetof(M2Sequence, variationIndex) == 0x02, "variationIndex");
    static_assert(offsetof(M2Sequence, duration) == 0x04, "duration");
    static_assert(offsetof(M2Sequence, flags) == 0x0C, "flags");
    static_assert(offsetof(M2Sequence, replayMin) == 0x14, "replayMin");
    static_assert(offsetof(M2Sequence, blendTime) == 0x1C, "blendTime");
    static_assert(offsetof(M2Sequence, variationNext) == 0x3C, "variationNext");
    static_assert(offsetof(M2Sequence, aliasNext) == 0x3E, "aliasNext");
    static_assert(sizeof(M2Batch) == 0x18, "M2Batch");
    static_assert(offsetof(M2Batch, shaderId) == 0x02, "shaderId");
    static_assert(offsetof(M2Batch, materialIndex) == 0x0A, "materialIndex");
    static_assert(offsetof(M2Batch, textureCount) == 0x0E, "textureCount");
    static_assert(sizeof(M2Ribbon) == 0xB0, "M2Ribbon");
    static_assert(offsetof(M2Ribbon, textureIndices) == 0x14, "M2Ribbon.textureIndices");
    static_assert(offsetof(M2Ribbon, materialIndices) == 0x1C, "M2Ribbon.materialIndices");
    static_assert(offsetof(M2Ribbon, texSlotTrack) == 0x84, "M2Ribbon.texSlotTrack");
    static_assert(offsetof(M2Ribbon, priorityPlane) == 0xAC, "M2Ribbon.priorityPlane");
    static_assert(sizeof(M2SkinSection) == 0x30, "M2SkinSection");
    static_assert(offsetof(M2SkinSection, level) == 0x02, "level");
    // Live skin object offsets (the +4-shifted in-memory layout, not the on-disk .skin).
    static_assert(offsetof(M2SkinProfile, vertexCount) == 0x04, "skin.vertexCount");
    static_assert(offsetof(M2SkinProfile, vertexLookup) == 0x08, "skin.vertexLookup");
    static_assert(offsetof(M2SkinProfile, indexCount) == 0x0C, "skin.indexCount");
    static_assert(offsetof(M2SkinProfile, indices) == 0x10, "skin.indices");
    static_assert(offsetof(M2SkinProfile, bones) == 0x18, "skin.bones");
    static_assert(offsetof(M2SkinProfile, submeshCount) == 0x1C, "submeshCount");
    static_assert(offsetof(M2SkinProfile, submeshes) == 0x20, "submeshes");
    static_assert(offsetof(M2SkinProfile, batchCount) == 0x24, "batchCount");
    static_assert(offsetof(M2SkinProfile, batches) == 0x28, "batches");
    static_assert(offsetof(M2SkinProfile, boneCountMax) == 0x2C, "boneCountMax");
    static_assert(sizeof(M2SkinProfile) == 0x30, "M2SkinProfile");
    static_assert(sizeof(Md21Header) == 8, "Md21Header");
    static_assert(sizeof(M2Camera) == 0x64, "M2Camera");

    // ---- Engine runtime views -----------------------------------------------------------------------
    // The engine's in-memory objects once a model is loaded (not the on-disk format).

    // The engine's loaded model object (the model asset, shared by all its instances).
    struct CM2Model : EngineView
    {
        const char*    name() const { return at<const char>(0x3C); }   // INLINE full-path string
        M2Header*      fileData() const { return *at<M2Header*>(0x150); }
        uint32_t       fileSize() const { return *at<uint32_t>(0x16C); }
        void           setFileSize(uint32_t v) { *at<uint32_t>(0x16C) = v; }
        M2SkinProfile* skin() const { return *at<M2SkinProfile*>(0x170); }
    };

    // The live CM2 render instance (one per visible creature/character). Its bone palette + transform are
    // animated by the engine each frame; the draw context points to it.
    struct CM2Instance : EngineView
    {
        CM2Model*    model() const       { return *at<CM2Model*>(0x2C); }       // shared model asset
        const float* bonePalette() const { return *at<const float*>(0x98); }    // nBones x 4x4 row-major, live
        uint32_t     boneCount() const   { return model()->fileData()->bones.count; }
    };

    // The sorted render batch the color dispatcher hands per draw (one geoset of the model).
    struct M2RenderBatch : EngineView
    {
        M2SkinSection* geoset() const { return *at<M2SkinSection*>(0x2C); }     // the submesh this batch draws
    };

    // The per-batch render context the color body draw receives as 'this'.
    struct M2DrawContext : EngineView
    {
        CM2Instance*   instance() const { return *at<CM2Instance*>(0x60); }     // the instance being drawn
        M2RenderBatch* batch() const    { return *at<M2RenderBatch*>(0x50); }   // this draw's geoset batch
    };
}
