#pragma once

#include <cstdint>
#include <string>

// Generic FileDataID resolver. Modern assets (WMO/M2/textures) reference siblings by FileDataID; the
// Client engine references them by path. The host owns the DB2 tables that map a FileDataID to its on-disk
// path, so resolution is a host round trip. Results are memoized here on top of the IPC client's own cache.
// Returns "" when the id is unknown or the host is absent.
namespace wraith::features::fdid
{
    enum class Kind { Texture, Model };

    std::string Resolve(uint32_t fileDataId, Kind kind);

    inline std::string TexturePath(uint32_t fileDataId) { return Resolve(fileDataId, Kind::Texture); }
    inline std::string ModelPath(uint32_t fileDataId)   { return Resolve(fileDataId, Kind::Model); }
}
