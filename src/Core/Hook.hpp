#pragma once

#include <cstdint>

// Named hooking over MinHook. A feature installs a hook by NAME and address; the original (trampoline)
// is returned through `original`.
namespace wraith::hook
{
    // Initialise the hooking engine once at startup.
    bool Init();

    // Install one detour. `name` is for logging/diagnostics. `target` is the engine function address.
    // `detour` is our replacement. `original` receives the trampoline to call the real function.
    bool Install(const char* name, void* target, void* detour, void** original);

    inline bool Install(const char* name, uintptr_t target, void* detour, void** original)
    {
        return Install(name, reinterpret_cast<void*>(target), detour, original);
    }

    // Enable every installed hook. Call after all features have registered.
    bool EnableAll();
}
