#pragma once

#include <cstddef>
#include <cstdint>

// Raw memory patching of the target process code/data (handles the page protection).
namespace wraith::mem
{
    // Copy `len` bytes from `src` to `addr`.
    void Patch(uintptr_t addr, const void* src, size_t len);

    // Fill `len` bytes at `addr` with `value` (e.g. 0x90 = NOP).
    void Fill(uintptr_t addr, uint8_t value, size_t len);
}
