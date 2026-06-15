#include "Features/M2/MD21.hpp"
#include "Features/M2/Modern.hpp"

#include "Engine/Offsets.hpp"
#include "Features/M2/M2.hpp"
#include "Core/Hook.hpp"
#include "Core/Mem.hpp"
#include "Core/Logger.hpp"

#include <windows.h>
#include <cstring>

using namespace wraith;
using m2::M2Header;
using m2::CM2Model;

namespace modern = wraith::features::m2::modern;

namespace
{
    constexpr uint32_t kMagicSFID = 0x44494653;   // 'SFID' top-level chunk: array of skin FileDataIDs

    // Top-level MD21 chunk header: [4-byte magic][u32 size][data].
    struct ChunkHeader
    {
        uint32_t magic;
        uint32_t size;
    };

    using InitFn = int(__fastcall*)(void*);  // model init hook __thiscall(this)
    InitFn g_initOriginal = nullptr;

    using FinalizeFn = void(__fastcall*)(void*);  // skin finalize hook __thiscall(this)
    FinalizeFn g_finalizeOriginal = nullptr;

    // Ribbon de-relocator __cdecl(base, fileSize, ctx, &header.ribbonEmitters): pointer-fixes each ribbon's M2Arrays.
    using RibbonDeRelocFn = int(__cdecl*)(int base, unsigned int fileSize, int ctx, unsigned int* ribbons);
    RibbonDeRelocFn g_ribbonDeRelocOriginal = nullptr;

    // Skin finalize entry: the skin (model+0x170) is attached and pointer-fixed and the header
    // (model+0x150) is live, BEFORE the native shader-id passes run. Rebuild the 264 material contract a
    // modern skin lacks so the first shader-id pass does not NULL-deref header.textureUnitLookup.
    void __fastcall FinalizeDetour(void* self)
    {
        auto* model = static_cast<CM2Model*>(self);
        M2Header* md = model->fileData();
        m2::M2SkinProfile* skin = model->skin();
        if (md && skin && md->magic == m2::kMagicMD20 &&
            md->version >= m2::modern::kVersionMin && md->version <= m2::modern::kVersionMax)
        {
            modern::RebuildMaterials(md, skin, model->name());
        }
        g_finalizeOriginal(self);
    }

    // M2 ribbon de-relocator: after the native pass turns each ribbon's textureIndices/materialIndices into
    // raw pointers, expand any materialIndices shorter than textureIndices so every layer reads
    // materialIndices[0], removing the out-of-bounds material read on a multi-texture ribbon. When the
    // counts are equal the expand is a no-op. param_4 = &header.ribbonEmitters; after the original,
    // param_4[0] = ribbon count, param_4[1] = raw ribbon-array pointer (0 when count is 0).
    int __cdecl RibbonDeRelocDetour(int base, unsigned int fileSize, int ctx, unsigned int* ribbons)
    {
        int result = g_ribbonDeRelocOriginal(base, fileSize, ctx, ribbons);
        if (result != 0 && ribbons && ribbons[0] != 0 && ribbons[1] != 0)
            modern::ExpandRibbonMaterials(reinterpret_cast<uint8_t*>(ribbons[1]), ribbons[0]);
        return result;
    }

    // Model init hook: de-chunk the MD21 container, dispatch the per-version fixups, run the engine parse.
    // The container and engine integration are version-agnostic; per-version deltas live in each version
    // contract (e.g. modern::ApplyFixups).
    int __fastcall InitDetour(void* self)
    {
        auto* model = static_cast<CM2Model*>(self);
        M2Header* md = model->fileData();
        uint32_t origSize = model->fileSize();
        uint32_t md20Size = origSize;

        if (md && md->magic == m2::kMagicMD21)
        {
            // Slide the inner MD20 to the buffer start (its offsets are already self-relative). fileSize
            // stays origSize, so [md20Size, origSize) becomes spare slack for table synthesis.
            auto* wrapper = reinterpret_cast<m2::Md21Header*>(md);
            md20Size = wrapper->chunkSize;
            memmove(md, reinterpret_cast<uint8_t*>(md) + sizeof(m2::Md21Header), md20Size);
        }

        uint32_t version = (md && md->magic == m2::kMagicMD20) ? md->version : 0;
        if (version >= m2::modern::kVersionMin && version <= m2::modern::kVersionMax)
            modern::ApplyFixups(md, md20Size, origSize);

        int result = g_initOriginal(self);

        if (version > m2::kVersionWrath)   // only log the modern (post-264) models, not stock 264
            WLOG_INFO("MD21: '%s' version=%u result=%d", model->name(), version, result);
        return result;
    }
}

namespace wraith::features::m2
{
    void Install()
    {
        // Widen the M2 version gate (264 only) to also accept the MD21-era inner MD20 versions.
        mem::Fill(offsets::VersionGate_InitJA, 0x90, 6);        // NOP the JA (ver>264)
        const uint8_t jmpShort = 0xEB;
        mem::Patch(offsets::VersionGate_AnimJBE, &jmpShort, 1); // JBE -> JMP short

        // The alpha-key reference is lowered to 0.5 only for modern models, scoped at draw time
        // (AlphaScope), so stock 264 content keeps its vanilla 0.878 cutoff.
        AlphaScope_Install();

        hook::Install("CM2Model::Init", offsets::CM2Model_Init, &InitDetour, reinterpret_cast<void**>(&g_initOriginal));
        hook::Install("CM2Model::FinalizeSkin", offsets::CM2Model_FinalizeSkin, &FinalizeDetour, reinterpret_cast<void**>(&g_finalizeOriginal));
        hook::Install("M2Ribbon::DeRelocate", offsets::M2Ribbon_DeRelocate, &RibbonDeRelocDetour, reinterpret_cast<void**>(&g_ribbonDeRelocOriginal));

        WLOG_INFO("MD21: installed");
    }
}
