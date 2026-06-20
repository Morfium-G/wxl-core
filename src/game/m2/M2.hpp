// M2 game bindings: typed wrappers over the client's model functions, curated by hand.
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

#pragma once

#include <cstddef>
#include <cstdint>

#include "game/Binding.hpp"
#include "offsets/game/M2.hpp"
#include "structure/m2/M2Format.hpp"

// Curated M2 bindings. A module writes wxl::game::m2::ResolveTexture(h) instead of casting an address.
// The wrappers are inline typed calls (zero overhead); RegisterCatalog() adds the names to the
// enumerable catalog for tooling and the future scripting bridge. This is the worked template for the
// other formats' binding sets.
namespace wxl::game::m2
{
    namespace off = wxl::offsets::game::m2;

    // Resolve a texture handle to the internal texture object a sampler bind expects.
    inline void* ResolveTexture(void* handle)
    {
        return Native<off::M2_TexResolveFn>(off::kTexResolve)(handle, 0, 0);
    }

    // Bind a resolved texture to a device sampler selector.
    inline void BindSampler(void* device, uint32_t selector, void* resolvedTexture)
    {
        Native<off::M2_SamplerBindFn>(off::kSamplerBind)(device, nullptr, selector, resolvedTexture);
    }

    // Push the alpha-test reference to the device.
    inline void PushAlphaRef(float ref)
    {
        Native<off::M2_PushAlphaRefFn>(off::kPushAlphaRef)(ref);
    }

    // --- raw .m2 file buffer (valid at parse time, e.g. from OnModelLoadPre) ---
    // The whole .m2 file is read into a heap buffer at model+0x150. The byte size is at model+0x16c.
    inline void*    FileBuffer(void* model) { return *reinterpret_cast<void**>   (reinterpret_cast<char*>(model) + off::kOffModelHeader); }
    inline uint32_t FileSize  (void* model) { return *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(model) + off::kOffModelFileSize); }

    // The parsed model header (the .m2 buffer is parsed in place, so the buffer base IS the header). Valid
    // once the model is parsed (e.g. from OnModelLoad / OnM2SkinFinalize). Header M2Arrays hold RAW
    // POINTERS by this point, not file offsets.
    inline wxl::structure::m2::M2Header* Header(void* model)
    {
        return reinterpret_cast<wxl::structure::m2::M2Header*>(FileBuffer(model));
    }

    // The engine's live parsed skin profile, hung off the model object. NOT the on-disk skin: the parse
    // prepends a 4-byte leading field, so the arrays sit 4 bytes higher than the file layout, and the
    // count/pointer pairs are raw pointers. Valid at/after skin finalize (OnM2SkinFinalize). Indices are
    // global into the model vertex/index pools.
#pragma pack(push, 1)
    struct M2SkinProfile
    {
        uint32_t                          _lead;        // 0x00  parse-prepended leading field
        uint32_t                          vertexCount;  // 0x04
        uint16_t*                         vertexLookup; // 0x08
        uint32_t                          indexCount;   // 0x0C
        uint16_t*                         indices;      // 0x10
        uint32_t                          boneCount;    // 0x14
        uint8_t*                          bones;        // 0x18
        uint32_t                          submeshCount; // 0x1C
        wxl::structure::m2::M2SkinSection* submeshes;   // 0x20
        uint32_t                          batchCount;   // 0x24  (texunits)
        wxl::structure::m2::M2Batch*      batches;      // 0x28
        uint32_t                          boneCountMax; // 0x2C  per-draw bone budget seed
    };
#pragma pack(pop)
    static_assert(sizeof(M2SkinProfile) == 0x30, "M2SkinProfile");
    static_assert(offsetof(M2SkinProfile, submeshes) == 0x20, "M2SkinProfile.submeshes");
    static_assert(offsetof(M2SkinProfile, batches) == 0x28, "M2SkinProfile.batches");

    // The live parsed skin profile for a model (model+0x170), or null before it is attached.
    inline M2SkinProfile* Skin(void* model)
    {
        return *reinterpret_cast<M2SkinProfile**>(reinterpret_cast<char*>(model) + off::kOffModelSkin);
    }

    // Allocate / free a buffer with the SAME allocator the .m2 load buffer uses, so a replacement buffer is
    // freed correctly by the model destructor (it relies on the back-shift byte at [ptr-1]). The caller
    // passes its own debug tag so the core stays version-agnostic (each support module supplies its name).
    inline void* AllocBuffer(uint32_t size, const char* tag)
    {
        return Native<off::M2_BufferAllocFn>(off::kBufferAlloc)(size, tag, 0);
    }
    inline void FreeBuffer(void* ptr)
    {
        Native<off::M2_BufferFreeFn>(off::kBufferFree)(ptr);
    }

    // Point the model at a different .m2 image (buffer + byte size). The parser reads these on the next call.
    inline void ReplaceBuffer(void* model, void* buffer, uint32_t size)
    {
        *reinterpret_cast<void**>   (reinterpret_cast<char*>(model) + off::kOffModelHeader)   = buffer;
        *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(model) + off::kOffModelFileSize) = size;
    }

    // Add the M2 bindings to the enumerable catalog (cold, at startup).
    inline void RegisterCatalog()
    {
        Register({ "M2::ResolveTexture", off::kTexResolve,   "void*(handle)" });
        Register({ "M2::BindSampler",    off::kSamplerBind,  "void(device, selector, tex)" });
        Register({ "M2::PushAlphaRef",   off::kPushAlphaRef, "void(float ref)" });
    }
}
