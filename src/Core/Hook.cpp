#include "Core/Hook.hpp"
#include "Core/Logger.hpp"

#include <MinHook.h>

namespace wraith::hook
{
    bool Init()
    {
        MH_STATUS s = MH_Initialize();
        if (s != MH_OK)
        {
            WLOG_ERROR("hook: MH_Initialize failed (%s)", MH_StatusToString(s));
            return false;
        }
        return true;
    }

    bool Install(const char* name, void* target, void* detour, void** original)
    {
        MH_STATUS s = MH_CreateHook(target, detour, original);
        if (s != MH_OK)
        {
            WLOG_ERROR("hook: create '%s' @0x%p failed (%s)", name, target, MH_StatusToString(s));
            return false;
        }
        WLOG_INFO("hook: installed '%s' @0x%p", name, target);
        return true;
    }

    bool EnableAll()
    {
        MH_STATUS s = MH_EnableHook(MH_ALL_HOOKS);
        if (s != MH_OK)
        {
            WLOG_ERROR("hook: enable all failed (%s)", MH_StatusToString(s));
            return false;
        }
        return true;
    }
}
