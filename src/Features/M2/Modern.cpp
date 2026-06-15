#include "Features/M2/Modern.hpp"

#include "Core/Logger.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace sdk = wraith::m2;

namespace
{
    constexpr float kCameraFov = 0.7853982f;   // 45deg, substituted for the fov float modern cameras drop

    // Compact each 0x74 modern camera onto the 0x64 contract in place. dst stride (0x64) < src stride
    // (0x74), so the forward walk never overwrites a camera before it is read.
    void CompactCameras(sdk::M2Header* md, float defaultFov)
    {
        if (!md->cameras.count || !md->cameras.offset) return;
        uint8_t* arr = md->base() + md->cameras.offset;
        for (uint32_t i = 0; i < md->cameras.count; ++i)
        {
            auto* src = reinterpret_cast<sdk::modern::M2Camera*>(arr + i * sizeof(sdk::modern::M2Camera));
            auto* dst = reinterpret_cast<sdk::M2Camera*>(arr + i * sizeof(sdk::M2Camera));

            uint32_t type     = src->type;
            float    farClip  = src->farClip;
            float    nearClip = src->nearClip;
            memmove(&dst->body, &src->body, sizeof(sdk::M2CameraBody));
            dst->type     = type;
            dst->fov      = defaultFov;
            dst->farClip  = farClip;
            dst->nearClip = nearClip;
        }
    }

    // Compact each modern particle emitter (0x1ec) onto the 264 emitter (0x1dc) in place. The native
    // de-relocator strides by 0x1dc, so an un-slid modern body parses later emitters from the wrong bytes
    // and the load fails its sub-array bounds-check. dst stride < src stride, so the forward walk never
    // overwrites an emitter before it is read.
    // Assumes the 16 added Cata+ bytes sit at the tail (interior layout unverified); the leading 0x1dc
    // bytes carry every field the engine reads.
    void CompactParticles(sdk::M2Header* md)
    {
        if (!md->particleEmitters.count || !md->particleEmitters.offset) return;
        uint8_t* arr = md->base() + md->particleEmitters.offset;
        for (uint32_t i = 0; i < md->particleEmitters.count; ++i)
        {
            uint8_t* src = arr + i * sdk::modern::kParticleStrideModern;
            uint8_t* dst = arr + i * sdk::modern::kParticleStride264;
            if (dst != src) memmove(dst, src, sdk::modern::kParticleStride264);

            // A multi-texture emitter packs three 5-bit texture ids into textureId; 264 reads the field flat
            // and uses it directly into header.textures, so the packed value overruns the texture-handle
            // table (a wild AddRef in the native particle setup). Collapse it to the first id.
            uint32_t flags  = *reinterpret_cast<uint32_t*>(dst + 0x4);
            uint16_t* texId = reinterpret_cast<uint16_t*>(dst + sdk::modern::kParticleTextureIdOff);
            if (flags & sdk::modern::kParticleFlagMultiTex)
                *texId &= sdk::modern::kParticleTextureIdMask;
            // Robustness: any textureId past the table (malformed or unforeseen packing) parks at id 0
            // rather than letting the native loop deref a wild texture-handle slot.
            if (md->textures.count && *texId >= md->textures.count)
                *texId = 0;

            // blendingType @+0x28: the 264 M2-blend table is stride-7 (modes 0..6), so Cata+ BlendAdd (7)
            // indexes out of its row. BlendAdd = (src ONE, dst INV_SRC_ALPHA) = the engine's M2 mode 3
            // (device blend index 10), so 7 maps to 3 exactly; any other unknown mode >7 falls back to Add (4).
            uint8_t* blend = dst + sdk::modern::kParticleBlendOff;
            if (*blend == 7)      *blend = 3;            // faithful BlendAdd (device blend index 10)
            else if (*blend > 7)  *blend = 4;

            // Flipbook: wrap the head/tail cell keyframes into [0, rows*cols). Cata wraps the sampled cell
            // by the atlas cell count; 264 does not, so a keyframe >= cols maps to row >= 1 and samples V
            // off the atlas. Wrapping at load keeps every keyframe in-atlas.
            uint16_t rows = *reinterpret_cast<uint16_t*>(dst + sdk::modern::kParticleTexRowsOff);
            uint16_t cols = *reinterpret_cast<uint16_t*>(dst + sdk::modern::kParticleTexColsOff);
            int16_t cells = static_cast<int16_t>(rows * cols);
            if (cells > 1)
            {
                const uint32_t cellTracks[2] = { sdk::modern::kParticleHeadCellOff,
                                                 sdk::modern::kParticleTailCellOff };
                for (uint32_t track : cellTracks)
                {
                    // M2PartTrack keys = M2Array{count,ofs} at FBlock+0x8; ofs is md-relative (pre-de-reloc).
                    uint32_t keyCount = *reinterpret_cast<uint32_t*>(dst + track + 0x8);
                    uint32_t keyOfs   = *reinterpret_cast<uint32_t*>(dst + track + 0xc);
                    if (keyCount == 0 || keyCount > 0x1000 || keyOfs == 0) continue;
                    int16_t* keys = reinterpret_cast<int16_t*>(md->base() + keyOfs);
                    for (uint32_t k = 0; k < keyCount; ++k)
                        if (keys[k] >= cells) keys[k] %= cells;
                }
            }

            // Compressed gravity (flags 0x800000): the gravity M2Track keys are packed
            // {int8 x, int8 y, int16 z}, not floats. Decompress each to the plain 264 float scalar
            // (+downward) and clear the flag; otherwise the 4 packed bytes read as a float can form a NaN
            // and poison the particle position. Two-level track: val outer {count,ofs} at emitter+0x90/+0x94
            // (md-relative, pre-de-reloc); the outer array has one inner M2Array per animation index.
            // Per-animation inner offsets are split like every other track: an EMBEDDED animation's keys are
            // .m2-relative (decompress in place), an EXTERNAL animation's keys are .anim-relative and the
            // .anim is not loaded here, so adding the .m2 base would land in unrelated .m2 data and corrupt
            // it. Decompress only embedded slots (sequence flag 0x20 set), matching the engine's track split.
            if (flags & sdk::modern::kParticleFlagCompressedGravity)
            {
                uint32_t outerCount = *reinterpret_cast<uint32_t*>(dst + sdk::modern::kParticleGravityValCountOff);
                uint32_t outerOfs   = *reinterpret_cast<uint32_t*>(dst + sdk::modern::kParticleGravityValOfsOff);
                uint8_t* seqs       = (md->sequences.count && md->sequences.offset) ? md->base() + md->sequences.offset : nullptr;
                if (outerOfs && outerCount && outerCount <= 0x1000)
                {
                    uint8_t* outer = md->base() + outerOfs;
                    for (uint32_t o = 0; o < outerCount; ++o)
                    {
                        // Skip external animations: their gravity keys live in the .anim, not the .m2.
                        if (seqs && o < md->sequences.count &&
                            (*reinterpret_cast<uint32_t*>(seqs + o * 0x40 + 0x0c) & 0x20) == 0)
                            continue;
                        uint32_t innerCount = *reinterpret_cast<uint32_t*>(outer + o * 8 + 0x0);
                        uint32_t innerOfs   = *reinterpret_cast<uint32_t*>(outer + o * 8 + 0x4);
                        if (!innerOfs || !innerCount || innerCount > 0x1000) continue;
                        uint8_t* keys = md->base() + innerOfs;
                        for (uint32_t k = 0; k < innerCount; ++k)
                        {
                            uint8_t* key = keys + k * 4;
                            float dx = static_cast<int8_t>(key[0]) / 128.0f;
                            float dy = static_cast<int8_t>(key[1]) / 128.0f;
                            int16_t zraw = *reinterpret_cast<int16_t*>(key + 2);
                            float planar = dx * dx + dy * dy;
                            float zc  = std::sqrt(planar < 1.0f ? 1.0f - planar : 0.0f);
                            float mag = zraw * sdk::modern::kParticleGravityMagUnit;
                            if (mag < 0.0f) { zc = -zc; mag = -mag; }
                            *reinterpret_cast<float*>(key) = -(zc * mag);   // +downward 264 scalar
                        }
                    }
                }
                *reinterpret_cast<uint32_t*>(dst + 0x4) &= ~sdk::modern::kParticleFlagCompressedGravity;
            }
        }
    }

    // Slide each modern ribbon emitter onto the 264 stride and clamp its texture/material indices into the
    // header tables. The native ribbon de-relocator and the ribbon draw both stride by kRibbonStride264
    // (0xb0); modern ribbons keep that exact stride because the Cata+ tail
    // (priorityPlane/ribbonColorIndex/textureTransformLookupIndex) occupies the 264 layout's trailing
    // padding (m2::M2Ribbon at 0xac..0xb0), so the slide is a no-op while strideModern == stride264. It is
    // written against the named strides so a version that grows the ribbon body only edits kRibbonStrideModern.
    // Forward walk is safe whenever dst stride <= src stride: a destination element never overwrites a
    // source element not yet read.
    //
    // The textureIndices/materialIndices M2Arrays are still file-relative (count,offset) pairs here; the
    // slide leaves their fields in place at +0x14/+0x1c so the de-relocator's pointer-fix reads them
    // correctly. The draw case derefs materialIndices[0] flat into header.materials, and the ribbon
    // emitter binds textureIndices[i] into header.textures; an out-of-range entry (e.g. an unforeseen
    // modern packing) would index past those tables, so any index >= the table count is parked at 0. This is
    // the ribbon analog of the particle textureId overrun guard.
    void CompactRibbons(sdk::M2Header* md)
    {
        if (!md->ribbonEmitters.count || !md->ribbonEmitters.offset) return;
        static_assert(sdk::modern::kRibbonStrideModern >= sdk::modern::kRibbonStride264,
                      "ribbon forward slide requires dst stride <= src stride");

        uint8_t* arr = md->base() + md->ribbonEmitters.offset;
        for (uint32_t i = 0; i < md->ribbonEmitters.count; ++i)
        {
            uint8_t* src = arr + i * sdk::modern::kRibbonStrideModern;
            uint8_t* dst = arr + i * sdk::modern::kRibbonStride264;
            if (dst != src) memmove(dst, src, sdk::modern::kRibbonStride264);

            auto* rb = reinterpret_cast<sdk::M2Ribbon*>(dst);
            if (md->textures.count)
            {
                auto* texIdx = reinterpret_cast<uint16_t*>(md->base() + rb->textureIndices.offset);
                for (uint32_t t = 0; t < rb->textureIndices.count; ++t)
                    if (texIdx[t] >= md->textures.count) texIdx[t] = 0;
            }
            if (md->materials.count)
            {
                auto* matIdx = reinterpret_cast<uint16_t*>(md->base() + rb->materialIndices.offset);
                for (uint32_t m = 0; m < rb->materialIndices.count; ++m)
                    if (matIdx[m] >= md->materials.count) matIdx[m] = 0;
            }
        }
    }

    // Per ribbon: if materialIndices is shorter than textureIndices, allocate a textureIndices.count u16
    // array filled with materialIndices[0] and repoint the ribbon's materialIndices (count+ptr) at it. The
    // native ribbon setup reads materialIndices[i] for i in [0, textureIndices.count); a short array OOBs.
    // Repeating [0] is exactly what a 1-material ribbon means (Legion's whole-ribbon material). Operates on
    // raw pointers (post-de-reloc): rb->materialIndices.offset / textureIndices.offset hold raw addresses.
    // The new array is leaked for the model's lifetime (same pattern as RebuildSkinMaterials).
    void ExpandRibbonMaterialsImpl(uint8_t* ribbonArray, uint32_t ribbonCount)
    {
        for (uint32_t i = 0; i < ribbonCount; ++i)
        {
            auto* rb = reinterpret_cast<sdk::M2Ribbon*>(ribbonArray + i * sdk::modern::kRibbonStride264);
            uint32_t texCount = rb->textureIndices.count;
            uint32_t matCount = rb->materialIndices.count;
            if (matCount >= texCount || matCount == 0 || rb->materialIndices.offset == 0)
                continue;

            uint16_t first = *reinterpret_cast<uint16_t*>(rb->materialIndices.offset);
            auto* buf = static_cast<uint16_t*>(malloc(texCount * sizeof(uint16_t)));
            if (!buf) continue;
            for (uint32_t m = 0; m < texCount; ++m) buf[m] = first;

            rb->materialIndices.count  = texCount;
            rb->materialIndices.offset = reinterpret_cast<uint32_t>(buf);
        }
    }

    // Index of the first sequence whose id equals anim, or -1.
    int16_t AnimationIndex(sdk::M2Sequence* seqs, uint32_t count, uint16_t anim)
    {
        for (uint32_t i = 0; i < count; ++i)
            if (seqs[i].id == anim) return static_cast<int16_t>(i);
        return -1;
    }

    // Point sequenceLookup[new_id] at new_pos if it still holds old_pos, else rewrite the first lookup
    // entry that holds old_pos. A -1 old_pos (AnimationIndex miss) matches nothing and is a no-op.
    void ReplaceAnimLookup(int16_t* lookup, uint32_t lookupCount, int16_t oldPos, uint16_t newId,
                           int16_t newPos)
    {
        if (newId < lookupCount && lookup[newId] == oldPos)
        {
            lookup[newId] = newPos;
            return;
        }
        for (uint32_t i = 0; i < lookupCount; ++i)
            if (lookup[i] == oldPos) { lookup[i] = newPos; break; }
    }

    // Two transforms over the file-relative sequence array:
    //  - EVERY sequence: mask blendTime to its low u16. Cata+ split that u32 into blendTimeIn|blendTimeOut;
    //    read whole by the 264 engine it is a huge blend duration so transitions never complete (stuck or
    //    sliding animations).
    //  - id > 505 (WotLK max anim id): remap a curated swim/jump set to a 264 id and patch sequenceLookup so
    //    the engine still resolves them.
    void FixAnimations(sdk::M2Header* md)
    {
        if (!md->sequences.count || !md->sequences.offset) return;
        auto* seqs = reinterpret_cast<sdk::M2Sequence*>(md->base() + md->sequences.offset);
        uint32_t count = md->sequences.count;

        int16_t* lookup     = nullptr;
        uint32_t lookupCount = 0;
        if (md->sequenceLookup.count && md->sequenceLookup.offset)
        {
            lookup      = reinterpret_cast<int16_t*>(md->base() + md->sequenceLookup.offset);
            lookupCount = md->sequenceLookup.count;
        }

        for (uint32_t i = 0; i < count; ++i)
        {
            uint16_t id = seqs[i].id;
            if (id > 505)
            {
                uint16_t anim = id;
                switch (id)
                {
                case 564: anim = 37;  break;
                case 548: anim = 41;  break;
                case 556: anim = 42;  break;
                case 552: anim = 43;  break;
                case 554: anim = 44;  break;
                case 562: anim = 45;  break;
                case 572: anim = 39;  break;
                case 574: anim = 187; break;
                }
                if (id != anim && lookup)
                {
                    ReplaceAnimLookup(lookup, lookupCount, AnimationIndex(seqs, count, anim), anim,
                                      static_cast<int16_t>(i));
                    seqs[i].id = anim;
                }
            }

            seqs[i].blendTime &= 0xFFFF;
        }
    }

    // ---- 264 material contract rebuild ----------------------------------------------------------------
    // A modern .skin encodes each batch's material in its own shaderId (Cata+ scheme) and leaves the
    // header textureUnitLookup (0x88) empty. The WotLK skin's first shader-id pass instead indexes
    // header.textureUnitLookup[batch.texCoordCombo]; with that array NULL it derefs 0x0. The transform
    // re-derives the 264 per-batch fields (shaderId/texCoordCombo/textureCount) and synthesises the header
    // textureUnitLookup + textureCombinerCombos the native passes expect.

    // Append entry, returning the index of an existing single-entry match or the new slot.
    uint16_t LookupSingle(std::vector<int16_t>& lookup, int16_t v)
    {
        for (size_t n = 0; n < lookup.size(); ++n)
            if (lookup[n] == v) return static_cast<uint16_t>(n);
        lookup.push_back(v);
        return static_cast<uint16_t>(lookup.size() - 1);
    }

    // Append an adjacent pair, returning the base index. Reuses an overlapping tail so the two consecutive
    // entries [base], [base+1] equal a, b.
    uint16_t LookupPair(std::vector<int16_t>& lookup, int16_t a, int16_t b)
    {
        for (size_t n = 0; n + 1 < lookup.size(); ++n)
            if (lookup[n] == a && lookup[n + 1] == b) return static_cast<uint16_t>(n);
        size_t sz = lookup.size();
        if (sz > 1 && lookup[sz - 1] == a)
        {
            lookup.push_back(b);
            return static_cast<uint16_t>(sz - 1);
        }
        lookup.push_back(a);
        lookup.push_back(b);
        return static_cast<uint16_t>(sz);
    }

    // Find/append a (blend1, blend2) pair in textureCombinerCombos, returning its base index.
    uint16_t BlendOverride(std::vector<uint16_t>& combos, uint16_t b1, uint16_t b2)
    {
        for (size_t n = 0; n + 1 < combos.size(); n += 2)
            if (combos[n] == b1 && combos[n + 1] == b2) return static_cast<uint16_t>(n);
        uint16_t base = static_cast<uint16_t>(combos.size());
        combos.push_back(b1);
        combos.push_back(b2);
        return base;
    }

    // SM3 vertex-constant ceiling: the bone palette starts at c31, 3 registers per bone matrix, so
    // (256 - 31) / 3 = 75 bones per draw. The splitter keeps every drawn section under this; the FixSubmeshes
    // clamp is the safety net when a split is skipped or impossible.
    constexpr uint16_t kMaxBonesPerDraw = 75;

    // A skin->batchCount/submeshCount past this is treated as malformed; the env split can up to double the
    // batch count and the splitter grows submeshes, so cap the commit well under any value that would
    // overflow the native +0x188 sizing (batchCount*4).
    constexpr uint32_t kMaxBatches = 0x4000;

    // ---- per-draw bone splitter ----------------------------------------------------------------------
    // A modern .skin can declare a submesh whose per-draw bone palette (boneCombos[boneComboIndex ..
    // +boneCount)) exceeds 75. Split each such drawn submesh into N sub-sections of <=75 unique bones, 
    // each with its own compact boneCombos slice, like the Client exporter's skin partitioner. 
    //
    // The skin geometry arrays (vertexLookup, indices, bones) and header.boneCombos are rebuilt into owned 
    // buffers.
    //
    // Skinning model: skin.indices[] are GLOBAL local-vertex indices
    // into vertexLookup, range [section.vertexStart, +vertexCount); the GPU index builder subtracts
    // vertexStart. skin.bones[localVertex] (uint8[4]) holds LOCAL indices into the section's boneCombos
    // slice; header.boneCombos[section.boneComboIndex + bones[k]] = the global bone, and bones[k] is the
    // on-GPU vertex bone index the c31 palette is addressed by. So a sub-section needs: its own boneCombos
    // slice of the unique global bones its triangles touch, each vertex's bones[] remapped to that slice,
    // and its own contiguous vertex/index range (shared vertices are duplicated per sub-section).

    // u16 ceilings of the skin format: vertexStart/Count, indexStart/Count and every index value are u16.
    constexpr uint32_t kSkinU16Max = 0xFFFF;

    // Upper bound on boneCombos count; a source above this is rejected before the bulk copy.
    constexpr uint32_t kSplitMaxBoneCombos = 0x10000;

    // One rebuilt sub-section plus the original submesh it came from (so its batches can be re-pointed).
    struct SplitSection
    {
        sdk::M2SkinSection section;
        uint16_t           origSubmesh;
    };

    // The contiguous run of new sub-section indices one original submesh became.
    struct SplitRun { uint16_t first; uint16_t count; };

    // Greedy triangle bin-packer. Accumulate triangles into the current sub-section while the union of
    // unique global bones their vertices reference stays <= 75; otherwise start a new sub-section. Each
    // sub-section emits a compact boneCombos slice (sorted unique globals), a deduplicated vertex block
    // with remapped bones[], and a global-indexed triangle block. splitMap[origSubmesh] = the run of new
    // section indices it became. Returns false (no commit) if any rebuilt count would overflow a u16 field,
    // leaving the caller on the clamp path.
    bool SplitSubmeshes(sdk::M2Header* md, sdk::M2SkinProfile* skin, std::vector<SplitSection>& outSections,
                        std::vector<SplitRun>& splitMap, uint32_t& splitCount, const char* name)
    {
        if (!md->boneCombos.count || !md->boneCombos.offset) return false;
        if (!skin->vertexLookup || !skin->indices || !skin->bones) return false;

        // Header M2Arrays are de-relocated by model init before the skin is finalized, so .offset already
        // holds a raw pointer here.
        uint32_t boneComboCount = md->boneCombos.count;
        auto* boneCombos = reinterpret_cast<uint16_t*>(md->boneCombos.offset);
        if (boneComboCount > kSplitMaxBoneCombos || !boneCombos)
        {
            WLOG_WARN("MD21: '%s' boneCombos count=%u ptr=%p out of range, skipping bone split", name,
                      boneComboCount, boneCombos);
            return false;
        }

        // Rebuild only when a drawn submesh exceeds the SM3 ceiling. Models that already fit keep the native
        // skin untouched (the FixSubmeshes clamp path), avoiding an unnecessary geometry rebuild.
        bool needsSplit = false;
        for (uint32_t si = 0; si < skin->submeshCount; ++si)
        {
            const sdk::M2SkinSection& s = skin->submeshes[si];
            if (s.level == 0 && s.boneCount > kMaxBonesPerDraw) { needsSplit = true; break; }
        }
        if (!needsSplit) return false;

        std::vector<uint16_t> newVtxLookup;
        std::vector<uint8_t>  newBones;          // 4 bytes per new local vertex
        std::vector<uint16_t> newIndices;
        std::vector<uint16_t> newBoneCombos(boneCombos, boneCombos + boneComboCount);
        newVtxLookup.reserve(skin->vertexCount);
        newBones.reserve(skin->vertexCount * 4);
        newIndices.reserve(skin->indexCount);

        splitCount = 0;

        for (uint32_t si = 0; si < skin->submeshCount; ++si)
        {
            sdk::M2SkinSection src = skin->submeshes[si];

            // A level>0 submesh is a (level<<16|id) sub-batch the 264 engine cannot draw. Pass it through as
            // a single zeroed placeholder so the batch re-point stays 1:1; its batch is later skipped.
            if (src.level > 0)
            {
                src.level = 0; src.vertexStart = 0; src.vertexCount = 0; src.indexStart = 0;
                src.indexCount = 0; src.boneComboIndex = 0; src.centerBoneIndex = 0; src.boneCount = 1;
                reinterpret_cast<uint8_t*>(&src)[0x11] = 0;
                outSections.push_back({ src, static_cast<uint16_t>(si) });
                continue;
            }

            // Bound the section index window to the live skin index buffer before the triangle walk derefs
            // skin->indices[src.indexStart + t*3 + k].
            if (static_cast<uint32_t>(src.indexStart) + src.indexCount > skin->indexCount)
            {
                WLOG_WARN("MD21: '%s' submesh %u index window [%u,+%u) past skin indexCount %u, skipping "
                          "bone split", name, si, src.indexStart, src.indexCount, skin->indexCount);
                return false;
            }

            uint32_t triCount = src.indexCount / 3;
            uint32_t comboBase = src.boneComboIndex;

            // Walk triangles, packing into sub-sections.
            std::vector<uint16_t> curGlobals;    // unique global bones in the current sub-section
            uint32_t curTriStart = 0;
            uint32_t emittedSections = 0;

            auto emit = [&](uint32_t triFrom, uint32_t triTo, std::vector<uint16_t>& globals) -> bool
            {
                if (triFrom >= triTo) return true;
                // sort globals for a stable compact slice, build global->local map
                std::sort(globals.begin(), globals.end());
                uint32_t comboIndex = static_cast<uint32_t>(newBoneCombos.size());
                if (comboIndex > kSkinU16Max) return false;
                for (uint16_t g : globals) newBoneCombos.push_back(g);

                uint32_t secVertStart = static_cast<uint32_t>(newVtxLookup.size());
                uint32_t secIndexStart = static_cast<uint32_t>(newIndices.size());
                if (secVertStart > kSkinU16Max || secIndexStart > kSkinU16Max) return false;

                // dedup vertices within this sub-section
                std::unordered_map<uint16_t, uint16_t> vmap;
                for (uint32_t t = triFrom; t < triTo; ++t)
                {
                    for (uint32_t k = 0; k < 3; ++k)
                    {
                        uint16_t lv = skin->indices[src.indexStart + t * 3 + k];
                        if (lv >= skin->vertexCount) return false;   // bad index -> abort, fall to clamp
                        auto it = vmap.find(lv);
                        uint16_t nv;
                        if (it == vmap.end())
                        {
                            uint32_t idx = static_cast<uint32_t>(newVtxLookup.size());
                            if (idx > kSkinU16Max) return false;
                            nv = static_cast<uint16_t>(idx);
                            vmap.emplace(lv, nv);
                            newVtxLookup.push_back(skin->vertexLookup[lv]);
                            const uint8_t* infl = skin->bones + lv * 4;
                            for (uint32_t j = 0; j < 4; ++j)
                            {
                                uint32_t comboIdx = comboBase + infl[j];
                                uint16_t g = comboIdx < boneComboCount ? boneCombos[comboIdx] : globals[0];
                                // local index in this sub-section's sorted slice
                                auto lo = std::lower_bound(globals.begin(), globals.end(), g);
                                uint16_t local = (lo != globals.end() && *lo == g)
                                               ? static_cast<uint16_t>(lo - globals.begin()) : 0;
                                newBones.push_back(static_cast<uint8_t>(local));
                            }
                        }
                        else nv = it->second;
                        newIndices.push_back(nv);
                    }
                }

                uint32_t secVertCount = static_cast<uint32_t>(newVtxLookup.size()) - secVertStart;
                uint32_t secIndexCount = static_cast<uint32_t>(newIndices.size()) - secIndexStart;
                if (secVertCount > kSkinU16Max || secIndexCount > kSkinU16Max) return false;

                sdk::M2SkinSection sec = src;
                sec.vertexStart    = static_cast<uint16_t>(secVertStart);
                sec.vertexCount    = static_cast<uint16_t>(secVertCount);
                sec.indexStart     = static_cast<uint16_t>(secIndexStart);
                sec.indexCount     = static_cast<uint16_t>(secIndexCount);
                sec.boneCount      = static_cast<uint16_t>(globals.size());
                sec.boneComboIndex = static_cast<uint16_t>(comboIndex);
                outSections.push_back({ sec, static_cast<uint16_t>(si) });
                ++emittedSections;
                return true;
            };

            for (uint32_t t = 0; t < triCount; ++t)
            {
                uint16_t g[12]; int gn = 0;
                for (uint32_t k = 0; k < 3; ++k)
                {
                    uint16_t lv = skin->indices[src.indexStart + t * 3 + k];
                    if (lv >= skin->vertexCount) return false;   // bad index -> abort, fall to clamp
                    const uint8_t* infl = skin->bones + lv * 4;
                    for (uint32_t j = 0; j < 4; ++j)
                    {
                        uint32_t comboIdx = comboBase + infl[j];
                        uint16_t gg = comboIdx < boneComboCount ? boneCombos[comboIdx] : 0;
                        bool seen = false;
                        for (int e = 0; e < gn; ++e) if (g[e] == gg) { seen = true; break; }
                        if (!seen && gn < 12) g[gn++] = gg;
                    }
                }
                // union size if this triangle joined the current sub-section
                size_t unionSize = curGlobals.size();
                for (int e = 0; e < gn; ++e)
                    if (std::find(curGlobals.begin(), curGlobals.end(), g[e]) == curGlobals.end())
                        ++unionSize;

                if (unionSize > kMaxBonesPerDraw && t > curTriStart)
                {
                    if (!emit(curTriStart, t, curGlobals)) return false;
                    curGlobals.clear();
                    curTriStart = t;
                }
                for (int e = 0; e < gn; ++e)
                    if (std::find(curGlobals.begin(), curGlobals.end(), g[e]) == curGlobals.end())
                        curGlobals.push_back(g[e]);
            }
            if (!emit(curTriStart, triCount, curGlobals)) return false;
            if (emittedSections == 0)
            {
                // empty (degenerate) submesh: keep a placeholder section so batch re-point stays 1:1.
                sdk::M2SkinSection sec = src;
                sec.vertexCount = 0; sec.indexCount = 0; sec.boneCount = 1;
                outSections.push_back({ sec, static_cast<uint16_t>(si) });
            }
            else if (emittedSections > 1)
            {
                splitCount += emittedSections - 1;
            }
        }

        if (newVtxLookup.size() > kSkinU16Max || outSections.size() > kMaxBatches) return false;

        // Build origSubmesh -> new-section run map (sections are emitted in original-submesh order, each as a
        // contiguous run). Default each original to a 1:1 self-mapping so an absent origSubmesh stays valid.
        splitMap.assign(skin->submeshCount, SplitRun{ 0, 0 });
        for (uint16_t i = 0; i < outSections.size(); ++i)
        {
            uint16_t orig = outSections[i].origSubmesh;
            if (orig >= splitMap.size()) continue;
            if (splitMap[orig].count == 0) splitMap[orig].first = i;
            ++splitMap[orig].count;
        }

        // Commit the rebuilt geometry into owned buffers (leaked for the model's lifetime, same pattern as
        // the header arrays). skin->* were file-mapped; the engine never per-array frees them.
        auto* vl = static_cast<uint16_t*>(malloc(newVtxLookup.size() * sizeof(uint16_t)));
        auto* bn = static_cast<uint8_t*>(malloc(newBones.size()));
        auto* ix = static_cast<uint16_t*>(malloc(newIndices.size() * sizeof(uint16_t)));
        auto* bc = static_cast<uint16_t*>(malloc(newBoneCombos.size() * sizeof(uint16_t)));
        auto* sm = static_cast<sdk::M2SkinSection*>(malloc(outSections.size() * sizeof(sdk::M2SkinSection)));
        if (!vl || !bn || !ix || !bc || !sm)
        {
            free(vl); free(bn); free(ix); free(bc); free(sm);
            return false;
        }
        memcpy(vl, newVtxLookup.data(), newVtxLookup.size() * sizeof(uint16_t));
        memcpy(bn, newBones.data(), newBones.size());
        memcpy(ix, newIndices.data(), newIndices.size() * sizeof(uint16_t));
        memcpy(bc, newBoneCombos.data(), newBoneCombos.size() * sizeof(uint16_t));
        for (size_t i = 0; i < outSections.size(); ++i) sm[i] = outSections[i].section;

        skin->vertexLookup = vl;
        skin->vertexCount  = static_cast<uint32_t>(newVtxLookup.size());
        skin->bones        = bn;
        skin->boneCount    = static_cast<uint32_t>(newVtxLookup.size());
        skin->indices      = ix;
        skin->indexCount   = static_cast<uint32_t>(newIndices.size());
        skin->submeshes    = sm;
        skin->submeshCount = static_cast<uint32_t>(outSections.size());

        // Store a raw pointer: header M2Arrays are de-relocated by skin finalize time and the native
        // finalize/draw read boneCombos as a pointer. Matches the textureUnitLookup/textureCombinerCombos commits.
        md->boneCombos.count  = static_cast<uint32_t>(newBoneCombos.size());
        md->boneCombos.offset = reinterpret_cast<uint32_t>(bc);
        return true;
    }

    // Park a level>0 submesh (a level<<16|id sub-batch the 264 engine cannot draw) by zeroing its geometry
    // and marking badSubmesh; keep boneCount >= 1 (native skin finalize divides boneCountMax by every submesh
    // boneCount). Clamp a drawn (level 0) submesh's boneCount to the SM3 ceiling and the boneCombos bounds
    // (>= 1), with a >= 1 bone-influence floor. A zero-geometry section is marked bad.
    void FixSubmeshes(sdk::M2Header* md, sdk::M2SkinProfile* skin, std::vector<uint8_t>& badSubmesh)
    {
        badSubmesh.assign(skin->submeshCount, 0);
        for (uint32_t i = 0; i < skin->submeshCount; ++i)
        {
            auto* s = &skin->submeshes[i];
            if (s->level > 0)
            {
                s->level           = 0;
                s->vertexStart     = 0;
                s->vertexCount     = 0;
                s->indexStart      = 0;
                s->indexCount      = 0;
                s->boneComboIndex  = 0;
                s->centerBoneIndex = 0;
            }

            if (s->indexCount == 0)
            {
                if (s->boneCount < 1) s->boneCount = 1;
                badSubmesh[i] = 1;
            }
            else
            {
                uint16_t cap = kMaxBonesPerDraw;
                uint16_t byCombo = md->boneCombos.count > s->boneComboIndex
                                 ? static_cast<uint16_t>(md->boneCombos.count - s->boneComboIndex) : 1;
                if (byCombo < cap) cap = byCombo;
                if (cap < 1)       cap = 1;
                if (s->boneCount > cap) s->boneCount = cap;
                if (s->boneCount < 1)   s->boneCount = 1;
                if (s->boneInfluences == 0) s->boneInfluences = 1;
            }
            reinterpret_cast<uint8_t*>(s)[0x11] = 0;
        }
    }

    // The < 0x8000 shaderId tail: blend-bit decode + texUnitLookup synthesis, applied in place to a batch
    // that already holds the down-converted shaderId. Used by the normal path and (after the env split
    // forces single-texture) the follower's primary.
    void DecodeBlendBits(sdk::M2Batch* b, uint16_t shaderId, uint16_t textureCount,
                         std::vector<int16_t>& texUnitLookup, std::vector<uint16_t>& blendOverride)
    {
        uint16_t blend1 = (shaderId >> 4) & 0x7;
        uint16_t blend2 = shaderId & 0x7;

        bool twoTex = textureCount > 1 && (shaderId & 0x4000) && blend1 != 0 && blend2 != 0;
        uint16_t shaderToSave = 0;
        if (twoTex)
            shaderToSave = BlendOverride(blendOverride, blend1, blend2);
        else
            textureCount = 1;

        b->flags &= 0x10;
        b->shaderId = shaderToSave;

        if (textureCount == 1)
        {
            int16_t t0 = (shaderId & 0x80) ? -1 : 0;
            b->textureCoordComboIndex = LookupSingle(texUnitLookup, t0);
        }
        else
        {
            int16_t t0, t1;
            if (shaderId & 0x80)
            {
                t0 = -1;
                t1 = (shaderId & 0x8) ? -1 : 0;
            }
            else
            {
                t0 = 0;
                t1 = (shaderId & 0x8) ? -1 : ((shaderId & 0x4000) ? 1 : 0);
            }
            b->textureCoordComboIndex = LookupPair(texUnitLookup, t0, t1);
        }

        b->textureCount = textureCount < 2 ? textureCount : 2;
    }

    // One modern env batch splits into a primary plus a follower (a 2nd render pass over the SAME
    // geometry). The follower copies the primary's fields, then carries material-layer 1 and renderflags
    // index +1 so the engine binds it as the layered 2nd pass over the diffuse below it. Three case
    // families differ only in blend override + tex-coord lookup, keyed off the shaderId >= 0x8000 code.
    // out gets primary then follower. Returns false if this low code is not an env split (caller falls
    // through to the in-place decode).
    bool EnvSplit(uint16_t low, uint16_t shaderId, sdk::M2Batch primary, uint32_t nTransparencyLookup,
                  std::vector<sdk::M2Batch>& out, std::vector<int16_t>& texUnitLookup,
                  std::vector<uint16_t>& blendOverride)
    {
        sdk::M2Batch follower = primary;
        follower.materialIndex  = static_cast<uint16_t>(primary.materialIndex + 1);
        follower.materialLayer  = 1;
        follower.textureCount   = 1;

        switch (low)
        {
        case 0: case 3: case 9: case 17: case 24:
        {
            primary.textureCount = 2;
            uint16_t blendIdx = BlendOverride(blendOverride, 1, 4);
            uint16_t tc = LookupPair(texUnitLookup, 0, -1);
            primary.shaderId               = blendIdx;
            primary.textureCoordComboIndex = tc;
            follower.shaderId               = blendIdx;
            follower.textureCoordComboIndex = tc;
            out.push_back(primary);
            out.push_back(follower);
            return true;
        }
        case 1: case 15:
        {
            primary.textureCount   = 1;
            primary.shaderId       = 0;   // no blend override on this family
            follower.shaderId      = 0;
            follower.textureComboIndex = static_cast<uint16_t>(primary.textureComboIndex + 1);
            // transparency-combo only advances when a +1 slot exists in the header weight lookup
            if (static_cast<uint32_t>(primary.textureWeightComboIndex) + 1 < nTransparencyLookup)
                follower.textureWeightComboIndex = static_cast<uint16_t>(primary.textureWeightComboIndex + 1);

            int16_t t1 = (shaderId == 0x8001) ? -1 : 1;
            uint16_t tc = LookupPair(texUnitLookup, 0, t1);
            primary.textureCoordComboIndex  = tc;
            follower.textureCoordComboIndex = static_cast<uint16_t>(tc + 1);
            out.push_back(primary);
            out.push_back(follower);
            return true;
        }
        case 2:
        {
            uint16_t blendIdx = BlendOverride(blendOverride, 1, 3);
            uint16_t tc = LookupPair(texUnitLookup, 0, -1);
            primary.shaderId               = blendIdx;
            primary.textureCoordComboIndex = tc;
            follower.shaderId               = blendIdx;
            follower.textureCoordComboIndex = tc;
            out.push_back(primary);
            out.push_back(follower);
            return true;
        }
        default:
            return false;
        }
    }

    // Builds the down-converted batch array. Most batches map 1:1; env effects split into a primary +
    // follower, so the array grows. The result is committed to skin->batches / skin->batchCount at the
    // skin finalize entry, before the native passes size their parallel +0x188 block from skin->batchCount,
    // so the grow is seen and the block is sized correctly.
    // Down-convert one batch into 'piece' (1 batch, or primary+follower for an env split). Does not touch
    // skinSectionIndex; the caller re-points it per target sub-section.
    void DownConvertBatch(sdk::M2Batch b, uint32_t nTransparencyLookup, std::vector<sdk::M2Batch>& piece,
                          std::vector<int16_t>& texUnitLookup, std::vector<uint16_t>& blendOverride)
    {
        uint16_t shaderId = b.shaderId;
        uint16_t textureCount = b.textureCount;
        b.flags &= 0x10;

        if (shaderId >= sdk::modern::kShaderMin)
        {
            // Modern shader-effect indices [0..2] are Diffuse_T1_Env env effects the engine renders
            // natively, re-based to engine index = legionIdx+1 (index 0 is reserved for "no shader").
            // Emit ONE 2-texture Diffuse_T1_Env batch (T1 + env coord), matching the native env merges;
            // do NOT route these through the EnvSplit heuristic.
            uint16_t legionIdx = shaderId & 0x7fff;
            if (legionIdx <= 2)
            {
                b.shaderId               = static_cast<uint16_t>(sdk::modern::kShaderMin | (legionIdx + 1));
                b.textureCount           = 2;
                b.textureCoordComboIndex = LookupPair(texUnitLookup, 0, -1);
                b.flags                 &= 0x10;
                piece.push_back(b);
                return;
            }

            uint16_t low = shaderId & 0xFF;
            if (EnvSplit(low, shaderId, b, nTransparencyLookup, piece, texUnitLookup, blendOverride))
                return;
            switch (low)
            {
            case 5: case 8: case 10: case 12: case 16: case 23:
                shaderId = 0; textureCount = 1; break;
            case 21:  // Combiners_Mod_Mod
                shaderId = 0x4011; textureCount = 2; break;
            default:  // Combiners_Mod
                shaderId = 0x0010; textureCount = 1; break;
            }
        }

        if (shaderId < sdk::modern::kShaderMin)
            DecodeBlendBits(&b, shaderId, textureCount, texUnitLookup, blendOverride);
        else
            b.textureCount = textureCount < 2 ? textureCount : 2;

        piece.push_back(b);
    }

    // Build the down-converted batch array. Each original batch is processed once, then emitted for every
    // sub-section its original submesh became (skinSectionIndex re-pointed). A batch on a bad (zero-geometry)
    // section is reduced to a no-draw 0x8000 batch. With no split, splitMap is empty and every batch maps 1:1.
    void FixTexUnits(sdk::M2SkinProfile* skin, const std::vector<uint8_t>& badSubmesh,
                     const std::vector<SplitRun>& splitMap, std::vector<sdk::M2Batch>& out,
                     std::vector<int16_t>& texUnitLookup, std::vector<uint16_t>& blendOverride,
                     uint32_t nTransparencyLookup)
    {
        out.reserve(skin->batchCount);
        for (uint32_t i = 0; i < skin->batchCount; ++i)
        {
            sdk::M2Batch b = skin->batches[i];   // by value: edits build the new array, originals untouched

            // The batch's skinSectionIndex still names an ORIGINAL submesh; resolve its new-section run.
            SplitRun run{ b.skinSectionIndex, 1 };
            if (b.skinSectionIndex < splitMap.size()) run = splitMap[b.skinSectionIndex];

            std::vector<sdk::M2Batch> piece;
            DownConvertBatch(b, nTransparencyLookup, piece, texUnitLookup, blendOverride);

            for (uint16_t s = 0; s < run.count; ++s)
            {
                uint16_t sectionIdx = static_cast<uint16_t>(run.first + s);
                bool bad = sectionIdx < badSubmesh.size() && badSubmesh[sectionIdx];
                for (const sdk::M2Batch& p : piece)
                {
                    sdk::M2Batch nb = p;
                    nb.skinSectionIndex = sectionIdx;
                    if (bad) nb.shaderId = 0x8000;   // pass-1 skips 0x8000; the parked submesh draws nothing
                    out.push_back(nb);
                }
            }
        }
    }

    // Clamp a Cata+ material blend mode the 264 blend table cannot index (it has modes 0..6) to Add (4),
    // and strip flags above bit 5. The mesh path uses Add (4) rather than the faithful BlendAdd remap
    // (7->3 = device blend index 10) that the particle path uses: premultiplied "over" is too strong on
    // mesh materials.
    void FixRenderFlags(sdk::M2Header* md)
    {
        if (!md->materials.count) return;
        auto* mats = reinterpret_cast<uint16_t*>(md->materials.offset);  // raw pointer post-parse
        for (uint32_t i = 0; i < md->materials.count; ++i)
        {
            uint16_t& flag  = mats[i * 2 + 0];
            uint16_t& blend = mats[i * 2 + 1];
            if (blend > 6) { blend = 4; flag |= 0x5; }
            flag &= 0x1F;
        }
    }

    void RebuildSkinMaterials(sdk::M2Header* md, sdk::M2SkinProfile* skin, const char* name)
    {
        // Clamp an aberrant batchCount and continue the conversion (never skip): the native finalize needs a
        // down-converted skin. Batches past the cap are not drawn.
        if (skin->batchCount > kMaxBatches)
        {
            WLOG_WARN("MD21: '%s' skin batchCount=%u exceeds cap %u, clamping", name, skin->batchCount,
                      kMaxBatches);
            skin->batchCount = kMaxBatches;
        }

        std::vector<uint8_t> badSubmesh;
        std::vector<int16_t> texUnitLookup;
        std::vector<uint16_t> blendOverride;
        std::vector<sdk::M2Batch> batches;
        uint32_t nTransparencyLookup = md->textureWeightCombos.count;   // transparency-lookup count (header +0x90)

        // Partition any drawn submesh whose per-draw bone palette exceeds the SM3 ceiling into <=75-bone
        // sub-sections, rebuilding the skin geometry + header.boneCombos and re-pointing batches per
        // sub-section. On any failure (overflow, OOM, missing arrays) the skin is left untouched and the
        // FixSubmeshes clamp below is the safety net.
        std::vector<SplitSection> sections;
        std::vector<SplitRun> splitMap;
        uint32_t splitCount = 0;
        if (SplitSubmeshes(md, skin, sections, splitMap, splitCount, name) && splitCount > 0)
            WLOG_INFO("MD21: '%s' bone-splitter produced %u extra sub-draw(s)", name, splitCount);

        FixSubmeshes(md, skin, badSubmesh);
        FixTexUnits(skin, badSubmesh, splitMap, batches, texUnitLookup, blendOverride, nTransparencyLookup);

        // Commit the grown batch array into an owned heap buffer and repoint the skin BEFORE the native
        // skin finalize (g_finalizeOriginal) sizes its +0x188 block from skin->batchCount. skin->batches was
        // file-mapped memory the engine never per-array frees, so leaking the old pointer is fine; the new
        // buffer is leaked for the model's lifetime (same pattern as the header arrays below).
        if (!batches.empty())
        {
            auto* buf = static_cast<sdk::M2Batch*>(malloc(batches.size() * sizeof(sdk::M2Batch)));
            memcpy(buf, batches.data(), batches.size() * sizeof(sdk::M2Batch));
            skin->batches    = buf;
            skin->batchCount = static_cast<uint32_t>(batches.size());
        }

        // Commit the synthesised header arrays into owned heap buffers; post-parse the header array fields
        // are raw pointers, so the native passes read them directly. Leaked for the model's lifetime.
        if (!texUnitLookup.empty())
        {
            auto* buf = static_cast<int16_t*>(malloc(texUnitLookup.size() * sizeof(int16_t)));
            memcpy(buf, texUnitLookup.data(), texUnitLookup.size() * sizeof(int16_t));
            md->textureUnitLookup.count  = static_cast<uint32_t>(texUnitLookup.size());
            md->textureUnitLookup.offset = reinterpret_cast<uint32_t>(buf);
        }

        if (!blendOverride.empty())
        {
            auto* buf = static_cast<uint16_t*>(malloc(blendOverride.size() * sizeof(uint16_t)));
            memcpy(buf, blendOverride.data(), blendOverride.size() * sizeof(uint16_t));
            md->textureCombinerCombos.count  = static_cast<uint32_t>(blendOverride.size());
            md->textureCombinerCombos.offset = reinterpret_cast<uint32_t>(buf);
            md->globalFlags |= sdk::kFlagUseTextureCombinerCombos;
        }

        FixRenderFlags(md);
    }
}

namespace wraith::features::m2::modern
{
    void ApplyFixups(sdk::M2Header* md, uint32_t /*slackBegin*/, uint32_t /*slackEnd*/)
    {
        CompactCameras(md, kCameraFov);
        CompactParticles(md);
        CompactRibbons(md);
        FixAnimations(md);
    }

    void RebuildMaterials(sdk::M2Header* md, sdk::M2SkinProfile* skin, const char* name)
    {
        RebuildSkinMaterials(md, skin, name);
    }

    // SEH-guarded: derefs raw post-de-reloc ribbon pointers; a malformed model must not crash Init.
    void ExpandRibbonMaterials(uint8_t* ribbonArray, uint32_t ribbonCount)
    {
        if (!ribbonArray || !ribbonCount) return;
        __try { ExpandRibbonMaterialsImpl(ribbonArray, ribbonCount); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}
