#include "Features/FDID/Fdid.hpp"

#include "Features/Ipc/ShmClient.hpp"

#include <mutex>
#include <unordered_map>

namespace wraith::features::fdid
{
    namespace
    {
        std::mutex g_lock;
        // Keyed by (id << 1 | kindBit). One map: both kinds share the space, kind disambiguated by the low bit.
        std::unordered_map<uint64_t, std::string> g_cache;

        uint64_t CacheKey(uint32_t id, Kind kind)
        {
            return (static_cast<uint64_t>(id) << 1) | (kind == Kind::Model ? 1u : 0u);
        }
    }

    std::string Resolve(uint32_t fileDataId, Kind kind)
    {
        if (fileDataId == 0)
            return std::string();

        const uint64_t key = CacheKey(fileDataId, kind);
        {
            std::lock_guard<std::mutex> g(g_lock);
            auto it = g_cache.find(key);
            if (it != g_cache.end())
                return it->second;
        }

        std::string path = (kind == Kind::Model) ? ipc::ModelPath(fileDataId) : ipc::TexturePath(fileDataId);

        {
            std::lock_guard<std::mutex> g(g_lock);
            g_cache[key] = path;
        }
        return path;
    }
}
