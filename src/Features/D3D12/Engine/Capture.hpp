#pragma once

#include <d3d9.h>

// Captures the engine's D3D9 device by intercepting IDirect3D9::CreateDevice on the MAIN thread
namespace wraith::d3d12::capture
{
    // Called at each EndScene, on the main thread, after the device is captured. device = engine device.
    using FrameFn = void (*)(IDirect3DDevice9* device);

    // Wrap the real factory so CreateDevice/CreateDeviceEx are intercepted; returns a forwarding wrapper to
    // hand back to the engine in place of the real IDirect3D9/Ex.
    IDirect3D9*   Wrap(IDirect3D9* real);
    IDirect3D9Ex* WrapEx(IDirect3D9Ex* real);

    // Register the per-frame callback (call before the engine creates its device).
    void OnFrame(FrameFn fn);

    // The captured engine device, or null until the engine has called CreateDevice.
    IDirect3DDevice9* Device();
}
