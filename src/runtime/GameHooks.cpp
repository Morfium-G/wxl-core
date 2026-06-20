// Game-logic detours: publish OnModelLoad (and other non-render events as their RE lands).
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#include "runtime/GameHooks.hpp"

#include "core/Hook.hpp"
#include "core/Logger.hpp"
#include "events/Event.hpp"
#include "offsets/engine/Gx.hpp"
#include "offsets/game/ADT.hpp"
#include "offsets/game/Doodad.hpp"
#include "offsets/game/M2.hpp"
#include "offsets/game/World.hpp"

#include <windows.h>

#include <cstdint>

namespace
{
    namespace ev    = wxl::events;
    namespace m2    = wxl::offsets::game::m2;
    namespace gxoff = wxl::offsets::engine::gx;
    namespace adt   = wxl::offsets::game::adt;
    namespace dd    = wxl::offsets::game::doodad;
    namespace wld   = wxl::offsets::game::world;

    m2::M2_InitFn              g_origM2Init       = nullptr;
    m2::M2_FinalizeSkinFn      g_origFinalizeSkin = nullptr;
    m2::M2_SetupBatchAlphaFn   g_origSetupAlpha   = nullptr;
    dd::SpawnFromMDDFFn        g_origDoodadSpawn  = nullptr;
    gxoff::TextureUpdateFn     g_origTexUpdate    = nullptr;
    adt::Map_ChunkBuildFn      g_origChunkBuild   = nullptr;
    wld::World_EnterFn         g_origWorldEnter   = nullptr;

    // Model init parses the file into the runtime model (this-in-ECX). OnModelLoadPre fires at entry, while
    // the raw .m2 bytes (model+0x150 / size +0x16c)
    // the buffer. OnModelLoad fires after, with the now-parsed model.
    int __fastcall hkM2Init(void* model)
    {
        ev::ModelLoadArgs pre{ model };
        ev::Emit(ev::Event::OnModelLoadPre, &pre);
        const int r = g_origM2Init(model);
        ev::ModelLoadArgs a{ model };
        ev::Emit(ev::Event::OnModelLoad, &a);
        return r;
    }

    // Skin finalize (this-in-ECX = model). The skin profile is attached and pointer-fixed and the header
    // is live, BEFORE the native finalize sizes its parallel batch blocks from skin->batchCount. Publish
    // OnM2SkinFinalize here so a subscriber can rebuild a material/texunit contract a modern skin omits.
    void __fastcall hkFinalizeSkin(void* model)
    {
        ev::M2SkinFinalizeArgs a{ model };
        ev::Emit(ev::Event::OnM2SkinFinalize, &a);
        g_origFinalizeSkin(model);
    }

    // Per-batch alpha/material setup (this-in-ECX = draw context). After the native setter chooses the
    // alpha-test reference from the blend mode, resolve the model being drawn and its blend mode and
    // publish them, so a subscriber can re-push a different reference for the content it recognizes. The
    // draw-context reads are guarded: a malformed context must never fault the render thread.
    void __fastcall hkSetupBatchAlpha(void* ctx)
    {
        g_origSetupAlpha(ctx);

        void*    model = nullptr;
        uint16_t blend = 0;
        __try
        {
            void* inst = *reinterpret_cast<void**>(reinterpret_cast<char*>(ctx) + m2::kOffDrawCtxInstance);
            void* mat  = *reinterpret_cast<void**>(reinterpret_cast<char*>(ctx) + m2::kOffDrawCtxMaterial);
            if (inst) model = *reinterpret_cast<void**>(reinterpret_cast<char*>(inst) + m2::kOffInstModel);
            if (mat)  blend = *reinterpret_cast<uint16_t*>(reinterpret_cast<char*>(mat) + m2::kOffMaterialBlend);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { model = nullptr; }

        if (model)
        {
            ev::M2SetupBatchAlphaArgs a{ model, blend };
            ev::Emit(ev::Event::OnM2SetupBatchAlpha, &a);
        }
    }

    // Doodad spawn: build the CMapDoodad, then publish it (returned in EAX) so a module can track the
    // placement without walking chunk lists.
    void* __cdecl hkDoodadSpawn(const char* modelName, void* mddf, void* tileOrigin)
    {
        void* doodad = g_origDoodadSpawn(modelName, mddf, tileOrigin);
        ev::DoodadSpawnArgs a{ doodad };
        ev::Emit(ev::Event::OnDoodadSpawn, &a);
        return doodad;
    }

    // Texture upload: fired before the device update. Full-surface uploads pass x=y=0, so width=x2-x and
    // height=y2-y also cover sub-rect (dirty-region) uploads.
    void __cdecl hkTexUpdate(void* tex, int x, int y, int x2, int y2, int flag)
    {
        ev::TextureUploadArgs a{ tex, static_cast<uint32_t>(x2 - x), static_cast<uint32_t>(y2 - y) };
        ev::Emit(ev::Event::OnTextureUpload, &a);
        g_origTexUpdate(tex, x, y, x2, y2, flag);
    }

    // CMapChunk::Build (this-in-ECX): after it returns the sub-chunk pointers + layer units are populated,
    // so the texture-layer count is readable. Publish OnAdtChunkBuild with the chunk and its layer count.
    void __fastcall hkChunkBuild(void* chunk, void* edx, void* rawMcnk, int param2)
    {
        g_origChunkBuild(chunk, edx, rawMcnk, param2);
        uint32_t layers = 0;
        void* header = *reinterpret_cast<void**>(reinterpret_cast<char*>(chunk) + adt::kOffChunkMcnkHeader);
        if (header) layers = *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(header) + adt::kOffMcnkNLayers);
        ev::AdtChunkArgs a{ chunk, layers };
        ev::Emit(ev::Event::OnAdtChunkBuild, &a);
    }

    // CWorld::Enter: at entry the old world is still intact (leave); after it returns the new world is
    // resident (enter). One hook gives the leave/enter pair per transition.
    void __cdecl hkWorldEnter(int worldTime, int withLoadingScreen)
    {
        ev::WorldLeaveArgs leave{ 0 };
        ev::Emit(ev::Event::OnWorldLeave, &leave);
        g_origWorldEnter(worldTime, withLoadingScreen);
        ev::WorldEnterArgs enter{ 0 };
        ev::Emit(ev::Event::OnWorldEnter, &enter);
    }
}

namespace wxl::runtime::game
{
    void Install()
    {
        wxl::core::hook::Install("M2Init", m2::kInit,
                                 reinterpret_cast<void*>(&hkM2Init),
                                 reinterpret_cast<void**>(&g_origM2Init));
        wxl::core::hook::Install("M2FinalizeSkin", m2::kFinalizeSkin,
                                 reinterpret_cast<void*>(&hkFinalizeSkin),
                                 reinterpret_cast<void**>(&g_origFinalizeSkin));
        wxl::core::hook::Install("M2SetupBatchAlpha", m2::kSetupBatchAlpha,
                                 reinterpret_cast<void*>(&hkSetupBatchAlpha),
                                 reinterpret_cast<void**>(&g_origSetupAlpha));
        wxl::core::hook::Install("DoodadSpawn", dd::kSpawnFromMDDF,
                                 reinterpret_cast<void*>(&hkDoodadSpawn),
                                 reinterpret_cast<void**>(&g_origDoodadSpawn));
        wxl::core::hook::Install("TextureUpdate", gxoff::kTextureUpdate,
                                 reinterpret_cast<void*>(&hkTexUpdate),
                                 reinterpret_cast<void**>(&g_origTexUpdate));
        wxl::core::hook::Install("ChunkBuild", adt::kChunkBuild,
                                 reinterpret_cast<void*>(&hkChunkBuild),
                                 reinterpret_cast<void**>(&g_origChunkBuild));
        wxl::core::hook::Install("CWorldEnter", wld::kEnter,
                                 reinterpret_cast<void*>(&hkWorldEnter),
                                 reinterpret_cast<void**>(&g_origWorldEnter));

        WLOG_INFO("game: hooks installed (M2Init, M2FinalizeSkin, M2SetupBatchAlpha, DoodadSpawn, TextureUpdate, ChunkBuild, CWorldEnter)");
    }
}
