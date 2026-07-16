// Storage I/O hook: launch the host, then forward archive file opens to it (asset-agnostic).
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

#include "runtime/storage/StorageHook.hpp"

#include "runtime/storage/ShmClient.hpp"

#include "core/Hook.hpp"
#include "core/Logger.hpp"
#include "core/Mem.hpp"
#include "offsets/engine/Io.hpp"

#include <windows.h>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace io  = wxl::offsets::engine::io;
namespace ipc = wxl::runtime::ipc;

namespace
{
    // Marks a synthetic handle at +0x00 (a native handle holds a small kind there).
    constexpr uint32_t kHandleMagic = 0x464C5857; // 'WXLF'
    constexpr size_t   kMaxArchiveName = 512;     // upper bound when copying a name out of the native boundary

#pragma pack(push, 1)
    /**
     * @brief Synthetic file handle matching the native 0x30-byte file layout.
     *
     * The +0x14/+0x18/+0x1c fields match the native size/buffer/position fields the engine may read.
     */
    struct HostFile
    {
        uint32_t magic;        // +0x00  kHandleMagic
        uint32_t hostId;       // +0x04  host file handle (streaming mode; 0 when buffered)
        uint32_t reserved08;   // +0x08
        char*    shortName;    // +0x0c
        char*    fullName;     // +0x10
        uint32_t size;         // +0x14
        uint8_t* buffer;       // +0x18  whole-file bytes when buffered; the mapping when zero-copy; null streaming
        uint32_t position;     // +0x1c
        uint32_t reserved20;   // +0x20
        uint32_t reserved24;   // +0x24
        void*    mapView;      // +0x28  blob-section view (zero-copy); null otherwise
        void*    mapHandle;    // +0x2c  blob-section handle (zero-copy); null otherwise
    };
#pragma pack(pop)
    static_assert(sizeof(HostFile) == 0x30, "HostFile must match the native 0x30 file layout");

    /**
     * @brief Reports whether a handle is a synthetic host handle.
     * @param h  handle to test.
     * @return true when the handle carries kHandleMagic.
     */
    bool IsOurs(void* h)
    {
        return h && *reinterpret_cast<uint32_t*>(h) == kHandleMagic;
    }

    io::Storage_FileOpenFn  g_origOpen  = nullptr;
    io::Storage_FileSizeFn  g_origSize  = nullptr;
    io::Storage_FileReadFn  g_origRead  = nullptr;
    io::Storage_FileSeekFn  g_origSeek  = nullptr;
    io::Storage_FileCloseFn g_origClose = nullptr;
    io::MopaqOpenArchiveFn g_origMopaqOpenArchive = nullptr;
    io::InitializeWowConfigFn g_origInitializeWowConfig = nullptr;

    uint32_t g_served = 0; // files served from the host
    uint32_t g_missed = 0; // host connected but file not served (read natively)
    uint32_t g_opens  = 0; // intercept attempts

    // Names the host explicitly reported absent. Re-opening one skips the IPC round-trip and goes straight
    // native. Only a CONFIRMED host miss is recorded here -- never a timeout/desync -- so a transient failure
    // can never poison a servable file for the session. Capped so a pathological session stays bounded.
    std::mutex g_missMutex;
    std::unordered_set<std::string> g_knownMisses;
    constexpr size_t kKnownMissCap = 16384;

    std::vector<wxl::runtime::storage::ClientProvideFn>& ClientProviders()
    {
        static std::vector<wxl::runtime::storage::ClientProvideFn> v;
        return v;
    }

    std::vector<wxl::runtime::storage::ServeFilterFn>& ServeFilters()
    {
        static std::vector<wxl::runtime::storage::ServeFilterFn> v;
        return v;
    }

    /**
     * @brief Tests case-insensitively whether a string ends with a suffix.
     * @param s       string to test.
     * @param suffix  suffix to match.
     * @return true when s ends with suffix.
     */
    bool EndsWithCI(std::string_view s, const char* suffix)
    {
        size_t ls = s.size(), lf = strlen(suffix);
        if (lf > ls) return false;
        for (size_t i = 0; i < lf; ++i)
            if (tolower(static_cast<unsigned char>(s[ls - lf + i])) != suffix[i]) return false;
        return true;
    }

    /**
     * @brief Reports whether a name looks like a streamed-audio file.
     * @param name  file name to test.
     * @return true for .wav/.mp3/.ogg.
     */
    bool LooksLikeAudio(std::string_view name)
    {
        return EndsWithCI(name, ".wav") || EndsWithCI(name, ".mp3") || EndsWithCI(name, ".ogg");
    }

    /**
     * @brief Reports whether a name is routed to the host.
     *
     * Skips .pub/.url, which are existence probes rather than archive content. Skips the modern terrain
     * sidecars the client has no loader for: .tex (the per-map texture catalog) and _lod.adt (the
     * low-detail tile). Serving their bytes stalls or faults the terrain load, so the open is left to miss
     * natively and the loader proceeds without them. Audio (.wav/.mp3/.ogg) is served: glue-screen audio
     * (music/ambience/button clicks) confirmed working through a synthetic handle; in-world interface/
     * spell/creature sounds are currently silent in-game with no crash/hang, cause not yet found -- see
     * AudioDiag, which logs every audio open/read/close to characterize what actually happens. The name
     * is already validated/non-empty (CopyArchiveName).
     * @param name  file name to test.
     * @return true when the name should be served from the host.
     */
    bool ShouldIntercept(std::string_view name)
    {
        if (EndsWithCI(name, ".pub") || EndsWithCI(name, ".url")) return false;
        if (EndsWithCI(name, ".tex") || EndsWithCI(name, "_lod.adt")) return false;
        return true;
    }

    // Diagnostic-only: characterizes FMOD's actual access pattern against a synthetic handle now that
    // audio is no longer excluded from host serving. Capped so a long play session cannot flood the log.
    namespace adiag
    {
        std::atomic<uint32_t> g_opens{ 0 };
        std::atomic<uint32_t> g_reads{ 0 };
        constexpr uint32_t kMaxOpenLogs = 150;
        constexpr uint32_t kMaxReadLogs = 500;

        void LogOpen(const char* name, const char* mode)
        {
            if (g_opens.fetch_add(1, std::memory_order_relaxed) >= kMaxOpenLogs) return;
            WLOG_INFO("audio-diag: OPEN '%s' mode=%s thread=%lu", name, mode, GetCurrentThreadId());
        }

        void LogRead(const char* name, uint32_t position, uint32_t len, uint32_t got)
        {
            if (g_reads.fetch_add(1, std::memory_order_relaxed) >= kMaxReadLogs) return;
            WLOG_INFO("audio-diag: READ '%s' pos=%u len=%u got=%u thread=%lu",
                      name, position, len, got, GetCurrentThreadId());
        }
    }

    /**
     * @brief Returns a stable cache key for archive names that may vary by slash or case.
     * @param name  validated archive name.
     * @return lowercased name with forward slashes folded to backslashes.
     */
    std::string NameKey(std::string_view name)
    {
        std::string key(name);
        for (char& c : key)
            c = (c == '/') ? '\\' : static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return key;
    }

    /**
     * @brief Tests whether the host already reported this name absent.
     * @param key  normalized archive name key.
     * @return true when the name can go straight to native fallback.
     */
    bool KnownMiss(const std::string& key)
    {
        std::lock_guard<std::mutex> lock(g_missMutex);
        return g_knownMisses.find(key) != g_knownMisses.end();
    }

    /**
     * @brief Records a confirmed host miss, capped so pathological sessions do not grow unbounded.
     * @param key  normalized archive name key.
     */
    void RememberMiss(std::string&& key)
    {
        std::lock_guard<std::mutex> lock(g_missMutex);
        if (g_knownMisses.size() < kKnownMissCap)
            g_knownMisses.insert(std::move(key));
    }

    /**
     * @brief Copies a plausible archive path out of the native call boundary.
     *
     * The native open surface occasionally receives non-path sentinels or stale small integers whose bytes
     * look like a string (e.g. a lone 0x01). Keeping those out of the host IPC path stops one bogus open from
     * desynchronising later requests. Validates content only -- a wild pointer is assumed not to occur here.
     * @param name  native name pointer.
     * @param out   receives a bounded, validated copy.
     * @return true when the bytes look like a normal archive path.
     */
    bool CopyArchiveName(const char* name, std::string& out)
    {
        if (!name) return false;
        out.clear();
        for (size_t i = 0; i < kMaxArchiveName; ++i)
        {
            unsigned char c = static_cast<unsigned char>(name[i]);
            if (c == '\0') return !out.empty();
            if (c < 0x20 || c >= 0x7f) return false; // control/extended byte: not a normal archive path
            out.push_back(static_cast<char>(c));
        }
        return false; // unterminated within the bound: treat as bogus
    }

    /**
     * @brief Duplicates a null-terminated string into a malloc'd buffer.
     * @param s  source string.
     * @return the duplicated string, or null on allocation failure.
     */
    char* DupName(const char* s)
    {
        size_t n = strlen(s) + 1;
        char* p = static_cast<char*>(malloc(n));
        if (p) memcpy(p, s, n);
        return p;
    }

    bool MakeHostHandleFromOpenResult(const std::string& hostName, const std::string& handleName,
                                      uint32_t flags, ipc::FileOpenResult& r, void** out,
                                      const char* logSuffix)
    {
        auto* f = static_cast<HostFile*>(calloc(1, sizeof(HostFile)));
        if (!f) return false;

        f->magic = kHandleMagic;
        f->size = r.size;
        f->position = 0;
        f->fullName = DupName(handleName.c_str());
        f->shortName = f->fullName;

        bool wholeFile = (flags & io::kOpenWholeFile) != 0;
        const char* mode;
        bool ok = true;
        void* view = nullptr;
        void* mapHandle = nullptr;
        if (r.id == 0)
        {
            // Inline: bytes came back in the open response.
            f->buffer = static_cast<uint8_t*>(malloc(r.size ? r.size : 1));
            if (f->buffer && r.size) memcpy(f->buffer, r.inlineData.data(), r.size);
            ok = (f->buffer != nullptr);
            mode = "inline";
        }
        else if (ipc::MapBlob(r.id, r.size, view, mapHandle))
        {
            // Zero-copy: map the host's section read-only and read bytes straight from it.
            f->buffer = static_cast<uint8_t*>(view);
            f->mapView = view;
            f->mapHandle = mapHandle;
            f->hostId = r.id;
            mode = "map";
        }
        else if (wholeFile)
        {
            // Buffered: pull all bytes now, release the host handle.
            f->buffer = static_cast<uint8_t*>(malloc(r.size ? r.size : 1));
            uint32_t off = 0;
            while (f->buffer && off < r.size)
            {
                uint32_t n = ipc::FileReadChunk(r.id, off, f->buffer + off, r.size - off);
                if (n == 0) break;
                off += n;
            }
            ipc::FileClose(r.id);
            ok = (f->buffer != nullptr && off == r.size);
            mode = "whole";
        }
        else
        {
            // Streaming: keep the host handle, pull chunks on demand.
            f->buffer = nullptr;
            f->hostId = r.id;
            mode = "stream";
        }

        // Served-bytes filters: a module may record trailing side tables (ATSC/ATHB/ATL2, ...) and
        // trim them off so the native loader sees only the bytes it understands.
        if (ok && f->buffer && f->size)
            for (wxl::runtime::storage::ServeFilterFn filter : ServeFilters())
            {
                const uint32_t served = filter(hostName.c_str(), f->buffer, f->size);
                if (served < f->size) f->size = served;
            }

        if (ok)
        {
            if (out) *out = f;
            if (g_served < 60)
                WLOG_INFO("Storage: serve '%s' (%u B, %s) from host%s",
                          handleName.c_str(), r.size, mode, logSuffix ? logSuffix : "");
            ++g_served;
            if (LooksLikeAudio(handleName)) adiag::LogOpen(handleName.c_str(), mode);
            return true;
        }

        free(f->buffer);
        free(f->fullName);
        free(f);
        return false;
    }

    /**
     * @brief Attempts to serve an open from the host, building a synthetic handle on a hit.
     * @param archive  archive object; specific-archive opens (non-null) stay native except .anim
     *                 sibling loads, which need the host transform path.
     * @param name     file name.
     * @param flags    native open flags.
     * @param out      receives the synthetic handle on a host hit.
     * @return true on a host hit, false to let the native open run.
     */
    bool TryServe(void* archive, const char* name, uint32_t flags, void** out)
    {
        std::string safeName;
        if (!CopyArchiveName(name, safeName) || !ShouldIntercept(safeName)) return false;

        // Specific-archive opens usually name files the client wants from one concrete MPQ. External M2
        // sequence loads are the exception: modern .anim siblings need the same host normalization as
        // regular archive opens, otherwise AFM2/AFSB/raw modern payloads bypass wxl-modern-anim.
        const bool specificAnim = archive != nullptr && EndsWithCI(safeName, ".anim");
        if (archive != nullptr && !specificAnim)
        {
            if (LooksLikeAudio(safeName))
                WLOG_INFO("audio-diag: SPECIFIC-ARCHIVE open '%s' archive=%p -> native only (host never tried)",
                          safeName.c_str(), archive);
            return false;
        }

        if ((++g_opens % 2000) == 0)
            WLOG_INFO("Storage stats: opens=%u served=%u missed=%u", g_opens, g_served, g_missed);

        // Client-side virtual providers: checked before IPC to avoid a host round-trip.
        // A provider returns true and fills `provided` to claim the file.
        {
            std::vector<uint8_t> provided;
            for (auto fn : ClientProviders())
            {
                if (!fn(safeName.c_str(), provided)) continue;
                auto* f = static_cast<HostFile*>(calloc(1, sizeof(HostFile)));
                if (!f) break;
                f->magic     = kHandleMagic;
                f->size      = static_cast<uint32_t>(provided.size());
                f->buffer    = static_cast<uint8_t*>(malloc(f->size ? f->size : 1));
                f->fullName  = DupName(safeName.c_str());
                f->shortName = f->fullName;
                if (f->buffer && f->size) memcpy(f->buffer, provided.data(), f->size);
                if (out) *out = f;
                ++g_served;
                return true;
            }
        }

        std::string key = NameKey(safeName);
        // Skip the IPC round-trip for a name the host has already confirmed absent.
        if (KnownMiss(key)) return false;

        ipc::FileOpenResult r = ipc::FileOpen(safeName, flags);
        if (r.ok)
        {
            if (MakeHostHandleFromOpenResult(safeName, safeName, flags, r, out,
                                             specificAnim ? " (specific)" : nullptr))
                return true;
        }
        // The host resolves texture-component and helm suffix aliases internally. Keeping that logic there
        // collapses a miss-or-alias lookup into one IPC request instead of several client round-trips.

        if (r.hostMiss)
        {
            // The host answered and reported the file absent: cache it so the next open skips the IPC and
            // goes straight native. Only a CONFIRMED miss is cached -- a timeout/desync (r.hostMiss == false)
            // falls back to native for this open alone and is retried next time, never poisoning the name.
            RememberMiss(std::move(key));
            if (g_missed < 200)
            {
                if (specificAnim)
                    WLOG_INFO("Storage: anim MISS '%s' (specific) -> native archive", safeName.c_str());
                else
                    WLOG_INFO("Storage: MISS '%s' -> native archive", safeName.c_str());
            }
            ++g_missed;
        }
        return false;
    }

    /**
     * @brief Detours the per-file content open entry point, serving from the host when possible.
     * @param archive  archive object.
     * @param name     file name.
     * @param flags    native open flags.
     * @param out      receives the resulting handle.
     * @return 1 on a host hit, otherwise the native open result.
     */
    int __stdcall OpenDetour(void* archive, const char* name, uint32_t flags, void** out)
    {
        if (TryServe(archive, name, flags, out)) return 1;
        return g_origOpen(archive, name, flags, out);
    }

    /**
     * @brief Detours file size, returning the host file size for synthetic handles.
     * @param handle    file handle.
     * @param sizeHigh  receives the high 32 bits (always 0 for host handles).
     * @return the low 32 bits of the file size.
     */
    uint32_t __stdcall SizeDetour(void* handle, uint32_t* sizeHigh)
    {
        if (IsOurs(handle))
        {
            if (sizeHigh) *sizeHigh = 0;
            return reinterpret_cast<HostFile*>(handle)->size;
        }
        return g_origSize(handle, sizeHigh);
    }

    /**
     * @brief Detours file read, copying from the buffer or streaming chunks for synthetic handles.
     * @param handle  file handle.
     * @param dst     destination buffer.
     * @param len     requested byte count.
     * @param read    receives the bytes copied.
     * @param ovl     native overlapped parameter.
     * @param unk     native read parameter.
     * @return 1 when the full request was satisfied, otherwise 0.
     */
    int __stdcall ReadDetour(void* handle, void* dst, uint32_t len, uint32_t* read, void* ovl, uint32_t unk)
    {
        if (IsOurs(handle))
        {
            auto* f = reinterpret_cast<HostFile*>(handle);
            uint32_t avail = (f->position < f->size) ? (f->size - f->position) : 0;
            uint32_t want = (len < avail) ? len : avail;
            uint32_t got = 0;

            if (f->buffer)
            {
                if (want) memcpy(dst, f->buffer + f->position, want);
                got = want;
            }
            else
            {
                uint8_t* p = static_cast<uint8_t*>(dst);
                while (got < want)
                {
                    uint32_t n = ipc::FileReadChunk(f->hostId, f->position + got, p + got, want - got);
                    if (n == 0) break;
                    got += n;
                }
            }

            if (f->fullName && LooksLikeAudio(f->fullName))
                adiag::LogRead(f->fullName, f->position, want, got);

            f->position += got;
            if (read) *read = got;
            return (got == len) ? 1 : 0; // nonzero only when the full request was satisfied
        }
        return g_origRead(handle, dst, len, read, ovl, unk);
    }

    /**
     * @brief Detours file seek, clamping the position within the host file for synthetic handles.
     * @param handle    file handle.
     * @param distLow   signed seek distance.
     * @param distHigh  receives the high 32 bits of the resulting position (always 0).
     * @param method    seek origin: 0=begin, 1=current, 2=end.
     * @return the resulting position.
     */
    uint32_t __stdcall SeekDetour(void* handle, int32_t distLow, uint32_t* distHigh, uint32_t method)
    {
        if (IsOurs(handle))
        {
            auto* f = reinterpret_cast<HostFile*>(handle);
            int64_t base = (method == 1) ? f->position : (method == 2) ? f->size : 0; // 0=BEGIN,1=CURRENT,2=END
            int64_t pos = base + distLow;
            if (pos < 0) pos = 0;
            if (pos > f->size) pos = f->size;
            f->position = static_cast<uint32_t>(pos);
            if (distHigh) *distHigh = 0;
            return f->position;
        }
        return g_origSeek(handle, distLow, distHigh, method);
    }

    /**
     * @brief Detours file close, releasing the mapping, buffer and host id for synthetic handles.
     * @param handle  file handle.
     * @return 1 for host handles, otherwise the native close result.
     */
    int __stdcall CloseDetour(void* handle)
    {
        if (IsOurs(handle))
        {
            auto* f = reinterpret_cast<HostFile*>(handle);
            if (f->fullName && LooksLikeAudio(f->fullName))
                WLOG_INFO("audio-diag: CLOSE '%s' pos=%u size=%u thread=%lu",
                          f->fullName, f->position, f->size, GetCurrentThreadId());
            if (f->mapView)
            {
                // Zero-copy: buffer points into the mapping, so unmap (do not free) then release the section.
                ipc::UnmapBlob(f->mapView, f->mapHandle);
                ipc::FileClose(f->hostId);
            }
            else
            {
                if (!f->buffer && f->hostId) ipc::FileClose(f->hostId);
                free(f->buffer);
            }
            free(f->fullName);
            free(f);
            return 1;
        }
        return g_origClose(handle);
    }

    /**
     * @brief Detours the true archive/directory mount primitive, dropping the loose override
     *        directories the host owns.
     *
     * Every native mount call in the client -- the boot-time base+patch table, the per-patch nested
     * "alternate.MPQ" probe, and the runtime patch-download mounts -- funnels through this one
     * primitive, so hooking here closes every one of those paths at once.
     *
     * Real .MPQ archives mount natively, exactly like stock. Making the host the sole reader of real
     * archive content too was tried and reverted: each individual blocker hit along the way was fixable,
     * but the cumulative in-game result was not good enough, so real archives went back to mounting
     * natively -- the hooks/offsets stay in place since they may be useful for something else later.
     * A loose override that is a DIRECTORY (the modern/custom data the host serves) is still
     * skipped, so the client never indexes its huge tree into its 32-bit address space; the file-open
     * detour serves those files from the host instead, same as before this whole detour existed.
     * Returning 0 reads as an absent optional archive, which every caller already tolerates.
     * @param name      archive path the client is about to mount.
     * @param priority  search priority.
     * @param flags     mount flags.
     * @param out       receives the archive handle on a native mount.
     * @return the native mount result, or 0 when the directory is skipped.
     */
    char __cdecl MopaqOpenArchiveDetour(const char* name, int priority, uint32_t flags, void** out)
    {
        const DWORD attrs = name ? GetFileAttributesA(name) : INVALID_FILE_ATTRIBUTES;
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
        {
            WLOG_INFO("archive-mount: SKIP loose dir '%s' (host-owned)", name);
            if (out) *out = nullptr;
            return 0;
        }
        WLOG_INFO("archive-mount: keep '%s'", name ? name : "(null)");
        return g_origMopaqOpenArchive(name, priority, flags, out);
    }

    // Required-archive hard-fail gate (io::kRequiredArchiveGateJnz):
    // `0F 85 11 01 00 00` (jne 0x4061d4) -> `E9 12 01 00 00 90` (unconditional jmp + 1-byte NOP pad).
    // Both encodings are 6 bytes so nothing after the patch site needs to move. Kept applied as a harmless
    // defensive measure even now that real archives mount natively again (turns a corrupted/missing
    // required archive into a tolerated, logged skip instead of a hard "Failed to open archive" dialog +
    // exit); originally load-bearing for a since-reverted experiment that blocked native archive mounting.
    constexpr uint8_t kRequiredGateOriginal[6] = { 0x0F, 0x85, 0x11, 0x01, 0x00, 0x00 };
    constexpr uint8_t kRequiredGatePatched[6]  = { 0xE9, 0x12, 0x01, 0x00, 0x00, 0x90 };

    /**
     * @brief Detours InitializeWowConfig: runs the original archive-mount/signature-file/config sequence
     *        unmodified, then forces the expansion-content-present flag (io::kWotlkContentFlag) on.
     *
     * Originally load-bearing for a since-reverted experiment that blocked the expansion archives from
     * mounting natively (see MopaqOpenArchiveDetour's doc comment); kept installed since real
     * archives mount natively again now, so the native derivation already produces the same
     * fully-unlocked value on its own in the normal case -- this is a harmless, idempotent safety net,
     * not a required fix.
     */
    void __cdecl InitializeWowConfigDetour()
    {
        g_origInitializeWowConfig();
        *reinterpret_cast<uint8_t*>(io::kWotlkContentFlag) = 2;
    }
}

namespace wxl::runtime::storage
{
    /**
     * @brief Arms the archive-mount guard, dropping the host-owned loose directories at mount time, plus
     *        two harmless-by-default safety nets (the required-archive hard-fail gate, the
     *        expansion-content flag force) kept installed from a since-reverted full-native-mount-block
     *        experiment.
     *
     * Must run before the client builds its archive set (call it from the DLL entry, on the loader
     * thread, before the client's startup proceeds). Independent of the host connection.
     */
    void InstallArchiveGuard()
    {
        wxl::core::hook::Install("Mopaq_OpenArchive", io::kMopaqOpenArchive,
            reinterpret_cast<void*>(&MopaqOpenArchiveDetour),
            reinterpret_cast<void**>(&g_origMopaqOpenArchive));

        wxl::core::hook::Install("InitializeWowConfig", io::kInitializeWowConfig,
            reinterpret_cast<void*>(&InitializeWowConfigDetour),
            reinterpret_cast<void**>(&g_origInitializeWowConfig));

        auto* gate = reinterpret_cast<void*>(io::kRequiredArchiveGateJnz);
        if (memcmp(gate, kRequiredGateOriginal, sizeof kRequiredGateOriginal) == 0)
        {
            wxl::core::mem::Patch(gate, kRequiredGatePatched, sizeof kRequiredGatePatched);
            WLOG_INFO("Storage: required-archive gate patched (boot no longer hard-fails on a blocked mount)");
        }
        else
        {
            WLOG_INFO("Storage: required-archive gate bytes did not match expected -- NOT patched (build mismatch?)");
        }

        wxl::core::hook::EnableAll();
        WLOG_INFO("Storage: archive-mount guard armed");
    }

    /**
     * @brief Launches the host, connects best-effort, and installs the archive file-I/O detours.
     */
    void Install()
    {
        // Launch the host (if installed) and connect best-effort. Absent host: the hooks fall through to
        // native; a later request reconnects if the host comes up after this point.
        ipc::EnsureHostRunning();
        ipc::WaitForHost(3000);
        ipc::Connect();

        wxl::core::hook::Install("Storage_FileOpen",  io::kFileOpen,  reinterpret_cast<void*>(&OpenDetour),  reinterpret_cast<void**>(&g_origOpen));
        wxl::core::hook::Install("Storage_FileSize",  io::kFileSize,  reinterpret_cast<void*>(&SizeDetour),  reinterpret_cast<void**>(&g_origSize));
        wxl::core::hook::Install("Storage_FileRead",  io::kFileRead,  reinterpret_cast<void*>(&ReadDetour),  reinterpret_cast<void**>(&g_origRead));
        wxl::core::hook::Install("Storage_FileSeek",  io::kFileSeek,  reinterpret_cast<void*>(&SeekDetour),  reinterpret_cast<void**>(&g_origSeek));
        wxl::core::hook::Install("Storage_FileClose", io::kFileClose, reinterpret_cast<void*>(&CloseDetour), reinterpret_cast<void**>(&g_origClose));
        WLOG_INFO("Storage: hooks installed (host %s)", ipc::IsConnected() ? "connected" : "absent");
    }

    void RegisterClientProvider(ClientProvideFn fn)
    {
        if (fn) ClientProviders().push_back(fn);
    }

    void RegisterServeFilter(ServeFilterFn fn)
    {
        if (fn) ServeFilters().push_back(fn);
    }
}
