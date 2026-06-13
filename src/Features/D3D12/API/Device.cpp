#include "Features/D3D12/API/Device.hpp"

#include <cstdio>
#include <cstdarg>
#include <cstdlib>

namespace
{
    ID3D12Device*       g_device    = nullptr;
    ID3D12CommandQueue* g_queue     = nullptr;
    ID3D12InfoQueue*    g_infoQueue = nullptr;
    bool                g_firstUsed = false;
    FILE*               g_log       = nullptr;

    ID3D12CommandQueue* MakeQueue(ID3D12Device* dev)
    {
        D3D12_COMMAND_QUEUE_DESC desc = {};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12CommandQueue* q = nullptr;
        dev->CreateCommandQueue(&desc, IID_PPV_ARGS(&q));
        return q;
    }

    bool EnsureDevice()
    {
        if (g_device) return true;

#ifdef WRAITH_D3D12_DEBUG
        // The D3D12 debug layer validates EVERY call. Since On12 funnels all of the engine's D3D9 calls through
        // D3D12, leaving it on costs ~3-5x FPS. Build with -DWRAITH_D3D12_DEBUG only when chasing a GPU bug.
        ID3D12Debug* dbg = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) { dbg->EnableDebugLayer(); dbg->Release(); }
#endif

        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device)))) return false;
#ifdef WRAITH_D3D12_DEBUG
        g_device->QueryInterface(IID_PPV_ARGS(&g_infoQueue));   // null if the debug layer is absent
#endif
        g_queue = MakeQueue(g_device);
        return g_queue != nullptr;
    }
}

namespace wraith::d3d12
{
    ID3D12Device*       Device() { EnsureDevice(); return g_device; }
    ID3D12CommandQueue* Queue()  { EnsureDevice(); return g_queue; }

    D3D9ON12_ARGS MakeOn12Args()
    {
        D3D9ON12_ARGS args = {};
        args.Enable9On12 = TRUE;
        if (EnsureDevice())
        {
            args.pD3D12Device = g_device;
            if (!g_firstUsed) { args.ppD3D12Queues[0] = g_queue; g_firstUsed = true; }
            else              { args.ppD3D12Queues[0] = MakeQueue(g_device); }   // throwaway, kept by On12
            args.NumQueues = 1;
        }
        return args;
    }

    void DrainDebug()
    {
        if (!g_infoQueue) return;
        UINT64 n = g_infoQueue->GetNumStoredMessages();
        for (UINT64 i = 0; i < n && i < 30; ++i)
        {
            SIZE_T len = 0;
            g_infoQueue->GetMessage(i, nullptr, &len);
            if (!len) continue;
            D3D12_MESSAGE* m = static_cast<D3D12_MESSAGE*>(malloc(len));
            if (m && SUCCEEDED(g_infoQueue->GetMessage(i, m, &len)))
                Log("d3d12dbg[%d]: %s", static_cast<int>(m->Severity), m->pDescription);
            free(m);
        }
        g_infoQueue->ClearStoredMessages();
    }

    void Log(const char* fmt, ...)
    {
        if (!g_log) g_log = fopen("Logs\\d3d9proxy.log", "w");
        if (!g_log) return;
        va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
        fputc('\n', g_log); fflush(g_log);
    }
}
