// The client LoadLibrary's "d3d9.dll" from its own folder first, so this proxy loads instead of the system one.

#include <windows.h>
#include <d3d9.h>
#include "Features/D3D12/API/Device.hpp"
#include "Features/D3D12/Engine/Capture.hpp"

using wraith::d3d12::Log;

namespace
{
    using Create9Fn       = IDirect3D9* (WINAPI*)(UINT);
    using Create9ExFn     = HRESULT     (WINAPI*)(UINT, IDirect3D9Ex**);
    using Create9On12Fn   = IDirect3D9* (WINAPI*)(UINT, D3D9ON12_ARGS*, UINT);
    using Create9On12ExFn = HRESULT     (WINAPI*)(UINT, D3D9ON12_ARGS*, UINT, IDirect3D9Ex**);
    Create9Fn       g_realCreate9       = nullptr;
    Create9ExFn     g_realCreate9Ex     = nullptr;
    Create9On12Fn   g_realCreate9On12   = nullptr;
    Create9On12ExFn g_realCreate9On12Ex = nullptr;

    // Load the real d3d9 from the system directory. A loaded module is keyed by full path, so the system
    // d3d9.dll is a distinct module from this proxy even though both share the base name, and no renamed
    // copy needs shipping. Falls back to a local d3d9_real.dll only if the system load fails.
    HMODULE LoadRealD3D9()
    {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);
        if (n != 0 && n < MAX_PATH - 16)
        {
            lstrcatA(path, "\\d3d9.dll");
            if (HMODULE r = LoadLibraryA(path)) return r;
        }
        return LoadLibraryA("d3d9_real.dll");
    }

    void EnsureReal()
    {
        if (g_realCreate9) return;
        HMODULE r = LoadRealD3D9();
        if (!r) { Log("d3d9proxy: FAILED to load the system d3d9.dll"); return; }
        g_realCreate9       = reinterpret_cast<Create9Fn>(GetProcAddress(r, "Direct3DCreate9"));
        g_realCreate9Ex     = reinterpret_cast<Create9ExFn>(GetProcAddress(r, "Direct3DCreate9Ex"));
        g_realCreate9On12   = reinterpret_cast<Create9On12Fn>(GetProcAddress(r, "Direct3DCreate9On12"));
        g_realCreate9On12Ex = reinterpret_cast<Create9On12ExFn>(GetProcAddress(r, "Direct3DCreate9On12Ex"));
        Log("d3d9proxy: system d3d9 loaded (9=%p Ex=%p On12=%p On12Ex=%p)",
            g_realCreate9, g_realCreate9Ex, g_realCreate9On12, g_realCreate9On12Ex);
    }
}

extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT sdkVersion)
{
    EnsureReal();
    if (g_realCreate9On12)
    {
        D3D9ON12_ARGS args = wraith::d3d12::MakeOn12Args();
        IDirect3D9* d3d = g_realCreate9On12(sdkVersion, &args, 1);
        Log("d3d9proxy: Direct3DCreate9On12 -> %p (D3D12 %p queue %p)", d3d, args.pD3D12Device, args.ppD3D12Queues[0]);
        if (d3d) return wraith::d3d12::capture::Wrap(d3d);   // intercept CreateDevice to capture the device
    }
    Log("d3d9proxy: native Direct3DCreate9 fallback");
    return g_realCreate9 ? g_realCreate9(sdkVersion) : nullptr;
}

extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex** out)
{
    EnsureReal();
    if (g_realCreate9On12Ex)
    {
        D3D9ON12_ARGS args = wraith::d3d12::MakeOn12Args();
        HRESULT hr = g_realCreate9On12Ex(sdkVersion, &args, 1, out);
        Log("d3d9proxy: Direct3DCreate9On12Ex -> hr=0x%08lx (D3D12 %p queue %p)", hr, args.pD3D12Device, args.ppD3D12Queues[0]);
        if (SUCCEEDED(hr))
        {
            if (out && *out) *out = wraith::d3d12::capture::WrapEx(*out);   // intercept CreateDeviceEx
            return hr;
        }
    }
    Log("d3d9proxy: native Direct3DCreate9Ex fallback");
    return g_realCreate9Ex ? g_realCreate9Ex(sdkVersion, out) : E_NOINTERFACE;
}

// Exposed to Wraith.dll: the shared D3D12 device + queue that back On12. Wraith's M2 D3D12 backend must render
// on THIS device/queue (the one On12 uses) so the unwrap/return of the engine's surfaces is valid.
extern "C" __declspec(dllexport) ID3D12Device* WraithD3D12Device()
{
    return wraith::d3d12::Device();
}
extern "C" __declspec(dllexport) ID3D12CommandQueue* WraithD3D12Queue()
{
    return wraith::d3d12::Queue();
}
// Diagnostic: flush the D3D12 debug-layer validation messages to the proxy log (no-op unless built -DWRAITH_D3D12_DEBUG).
extern "C" __declspec(dllexport) void WraithD3D12DrainDebug()
{
    wraith::d3d12::DrainDebug();
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        EnsureReal();
    return TRUE;
}
