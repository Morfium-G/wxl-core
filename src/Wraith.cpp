#include <windows.h>

#include "Core/Logger.hpp"
#include "Core/Hook.hpp"
#include "Features/M2/MD21.hpp"
#include "Features/Ribbon/Ribbon.hpp"

extern "C" __declspec(dllexport) void Wraith() {}

namespace
{
    DWORD WINAPI Bootstrap(LPVOID)
    {
        wraith::log::Init();
        WLOG_INFO("WRAITH starting (build %s %s)", __DATE__, __TIME__);

        if (!wraith::hook::Init())
            return 1;

        wraith::features::m2::Install();
        wraith::features::ribbon::Install();

        if (!wraith::hook::EnableAll())
            return 1;

        WLOG_INFO("WRAITH ready.");
        return 0;
    }
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        // Defer real work off the loader lock.
        CloseHandle(CreateThread(nullptr, 0, &Bootstrap, nullptr, 0, nullptr));
    }
    return TRUE;
}
