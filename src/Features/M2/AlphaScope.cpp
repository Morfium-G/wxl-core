#include "Features/M2/MD21.hpp"

#include "Engine/Offsets.hpp"
#include "Features/M2/M2.hpp"
#include "Features/M2/Modern.hpp"
#include "Core/Hook.hpp"
#include "Core/Logger.hpp"

#include <windows.h>

using namespace wraith;

// Scopes the lowered alpha-key reference (0.5) to modern (inner MD20 >= 272) models. Stock content keeps
// its vanilla 0.878 cutoff.
//
// The shared per-batch alpha/material setter is called by BOTH the creature triangle path and the doodad
// path with the same draw context, so hooking it covers trees (doodads) and creatures in one place. It
// derives the alpha-key ref from the material blend mode and pushes it to the device (ref = matAlpha *
// 0.878 for mode 1). After the original runs we re-push 0.5 for modern mode-1 batches, overwriting the
// same value (pixel constant / D3DRS_ALPHAREF). Mode 0 (test off) and mode >= 2 (1/255) are left as set.
namespace
{
    using SetupAlphaFn = void(__fastcall*)(void* ctx);   // shared per-batch alpha/material setter, __thiscall(M2DrawContext*)
    SetupAlphaFn g_setupAlphaOriginal = nullptr;

    using PushAlphaRefFn = void(__cdecl*)(float ref);     // push the alpha-test reference (pixel constant / ALPHAREF)
    auto g_pushAlphaRef = reinterpret_cast<PushAlphaRefFn>(offsets::CM2_PushAlphaRef);

    // The Legion alpha-key cutoff: the A2C coverage midpoint, where modern leaf/foliage coverage-alpha sits.
    constexpr float kModernAlphaKeyRef = 0.5f;

    // True if the batch's model is a modern (>= 272) one and its material is blend mode 1 (alpha key).
    // ctx+0x60 -> instance, instance+0x2c -> model, model header holds version; the live material is
    // ctx+0x98 (set by the caller before the setter), blend mode = *(u16*)(mat+0x02).
    bool IsModernAlphaKeyBatch(void* ctx)
    {
        bool hit = false;
        __try
        {
            auto* dc = static_cast<m2::M2DrawContext*>(ctx);
            m2::CM2Instance* inst = dc->instance();
            m2::CM2Model* model = inst ? inst->model() : nullptr;
            m2::M2Header* md = model ? model->fileData() : nullptr;
            if (!md || md->magic != m2::kMagicMD20 || md->version < m2::modern::kVersionMin)
                return false;

            void* mat = *reinterpret_cast<void**>(static_cast<char*>(ctx) + 0x98);
            if (!mat) return false;
            hit = (*reinterpret_cast<uint16_t*>(static_cast<char*>(mat) + 0x02) == 1);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { hit = false; }
        return hit;
    }

    void __fastcall SetupAlphaDetour(void* ctx)
    {
        g_setupAlphaOriginal(ctx);
        if (IsModernAlphaKeyBatch(ctx))
            g_pushAlphaRef(kModernAlphaKeyRef);   // overwrite the 0.878 ref the original just pushed
    }
}

namespace wraith::features::m2
{
    void AlphaScope_Install()
    {
        hook::Install("CM2::SetupBatchAlpha", offsets::CM2_SetupBatchAlpha,
                      &SetupAlphaDetour, reinterpret_cast<void**>(&g_setupAlphaOriginal));
        WLOG_INFO("M2/AlphaScope: installed (modern alpha-key ref scoped to >= %u)",
                  wraith::m2::modern::kVersionMin);
    }
}
