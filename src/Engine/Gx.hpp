#pragma once

#include "Engine/View.hpp"
#include "Engine/Offsets.hpp"

struct IDirect3DDevice9;
struct IDirect3DSurface9;

namespace wraith::gx
{
    // Typed view over the engine's CGxDeviceD3d: the D3D9 device + the On12-correct cached surfaces it draws to.
    struct CGxDeviceD3d : EngineView
    {
        IDirect3DDevice9*  d3d9()       const { return *at<IDirect3DDevice9*>(offsets::GxD3DDeviceField); }
        IDirect3DSurface9* backbuffer() const { return *at<IDirect3DSurface9*>(offsets::GxD3DBackbufferField); }
        IDirect3DSurface9* worldDepth() const { return *at<IDirect3DSurface9*>(offsets::GxD3DDepthField); }
    };

    // The global engine device (null until the engine has created it).
    inline CGxDeviceD3d* device()
    {
        return *reinterpret_cast<CGxDeviceD3d**>(offsets::GxDevicePtr);
    }
}
