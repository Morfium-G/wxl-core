// MpqStore: asset-agnostic archive I/O over StormLib that serves raw bytes.
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

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Asset-agnostic archive I/O. Serves raw bytes; whether a file needs reshaping is the asset handlers'
// decision and the host stays format-blind. Reads are safe to run concurrently: StormLib handles are
// single-thread, so each mounted archive carries its own lock, and loose-folder reads take none. Two
// workers reading from two different archives (or from loose folders) no longer serialize each other.
namespace wxl::host::mpq
{
    /** @brief Which mounted source would serve a name (Locate). */
    enum class Source : uint8_t
    {
        None,     // not present anywhere
        Loose,    // loose override folder (host-only visibility)
        Extra,    // custom patch archive beyond the standard set (Patch-4.MPQ and up)
        Standard, // stock base/locale/patch archive the client also mounts natively
    };

    /** @brief Mounts the client archive set and serves raw file bytes from it. */
    class MpqStore
    {
    public:
        /**
         * @brief Mounts the client archive set: locale and base archives plus loose Patch*.MPQ override folders.
         * @param dataDir  client data root
         * @return true if at least one archive or loose root mounted
         */
        bool Mount(std::string_view dataDir);

        /**
         * @brief Reports whether `name` exists in any mounted archive or loose root.
         * @param name  file name to check
         * @return true if the file is present
         */
        bool Exists(std::string_view name) const;

        /**
         * @brief Reads the whole file into `out`.
         * @param name  file name to read
         * @param out   receives the file bytes
         * @return false if the file is absent
         */
        bool ReadAll(std::string_view name, std::vector<uint8_t>& out) const;

        /**
         * @brief Reads a byte range from the file.
         * @param name  file name to read
         * @param off   start offset
         * @param len   maximum number of bytes to read
         * @param out   receives the bytes read (clamped to file end)
         * @return false if the file is absent
         */
        bool ReadRange(std::string_view name, uint32_t off, uint32_t len, std::vector<uint8_t>& out) const;

        /**
         * @brief Resolves a missed item path to a mounted path carrying the same file name.
         * @param name  requested path whose exact location is absent
         * @return a serveable path with the same file name, or empty if none is indexed
         */
        std::string ResolveByFileName(std::string_view name) const;

        /**
         * @brief Reports which mounted source would serve `name`, without reading any bytes.
         *
         * Walks the same priority order as ReadAll (loose folders, then archives) but stops at the
         * presence test, so answering is a few attribute probes and hash-table lookups.
         * @param name  file name to locate
         * @return the winning source kind, or Source::None
         */
        Source Locate(std::string_view name) const;

        /**
         * @brief Locates and reads `name` in ONE walk of the mounted set.
         *
         * Locate-then-ReadAll walks (and locks) every archive twice per served file; this fuses
         * them: the first source that has the file wins, and its bytes are read in place — except
         * a Standard hit with readStandard=false, which reports the source without reading so the
         * caller can hand the open to the client's native archives (the native-skip fast path).
         * @param name          file name to read
         * @param readStandard  false to skip reading bytes from a Standard (stock) archive hit
         * @param out           receives the file bytes (untouched on None / unread Standard)
         * @return the winning source kind, or Source::None
         */
        Source LocateAndRead(std::string_view name, bool readStandard, std::vector<uint8_t>& out) const;

        /** @brief Closes all open archive handles. */
        ~MpqStore();

    private:
        void IndexLooseRoot(const std::string& root);
        void IndexArchiveListfile(void* archive);
        void AddIndexEntry(const std::string& path);
        const std::string* FindIndexed(const std::string& requestLower, const std::string& fileKey) const;

        // Highest priority first (search order). StormLib handles mutate on read, so mutable.
        mutable std::vector<void*> m_archives;     // StormLib HANDLEs
        std::vector<std::string>   m_archiveNames; // parallel to m_archives, for logging
        std::vector<bool>          m_archiveIsExtra; // parallel: custom patch beyond the standard set
        // One lock per archive: StormLib handles are single-thread, but two different archives (and any
        // loose-folder read) are independent, so readers only serialize on the same archive.
        mutable std::vector<std::unique_ptr<std::mutex>> m_archiveLocks;
        std::vector<std::string>   m_looseRoots;   // absolute folder paths, trailing slash
        std::string                m_locale;       // detected locale folder name

        // File-name lookup over the item subtree: lowercase file name -> mounted paths, priority order.
        std::unordered_map<std::string, std::vector<std::string>> m_itemIndex;
    };
}
