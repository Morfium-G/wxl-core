#include "Core/Mem.hpp"

#include <windows.h>
#include <cstring>

namespace wraith::mem
{
    void Patch(uintptr_t addr, const void* src, size_t len)
    {
        void* dst = reinterpret_cast<void*>(addr);
        DWORD old;
        VirtualProtect(dst, len, PAGE_EXECUTE_READWRITE, &old);
        memcpy(dst, src, len);
        VirtualProtect(dst, len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), dst, len);
    }

    void Fill(uintptr_t addr, uint8_t value, size_t len)
    {
        void* dst = reinterpret_cast<void*>(addr);
        DWORD old;
        VirtualProtect(dst, len, PAGE_EXECUTE_READWRITE, &old);
        memset(dst, value, len);
        VirtualProtect(dst, len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), dst, len);
    }
}
