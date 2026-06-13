#pragma once

#include <d3d12.h>
#include <d3d9on12.h>

// D3D12 plumbing for the proxy. ONE shared D3D12 device + render queue back the whole stack.
// The engine makes several D3D9 devices; the FIRST (the active/presented one) runs On12 on the shared
// queue, so the side-renderer renders on that same queue and the unwrap/return path stays single-queue.
// Later devices get a throwaway separate queue (a second On12 device sharing the same queue null-derefs
// inside d3d9on12).
namespace wraith::d3d12
{
    ID3D12Device*       Device();   // shared, created on first use
    ID3D12CommandQueue* Queue();    // shared render queue = the first/active device's On12 queue

    // On12 args: first call uses the shared device+queue; later calls use the shared device + a fresh queue.
    D3D9ON12_ARGS MakeOn12Args();

    // Drain the D3D12 debug layer's stored validation messages to the log (no-op if the layer is absent).
    void DrainDebug();

    // Shared diagnostic log for the whole d3d9.dll.
    void Log(const char* fmt, ...);
}
