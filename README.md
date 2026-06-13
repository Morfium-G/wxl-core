# Wraith Engine - Engine

**The client-side runtime.** This is the part that runs: the patcher that gets us into the
process, the hook layer, the modern-model backport, and the Direct3D 12 proxy that the native
renderer will grow out of.

Part of the [Wraith Engine](https://github.com/WraithEngine) project - bringing **Legion 7.3.5**
rendering to the **WotLK 3.3.5a (build 12340)** client without replacing the client, the data, or
the protocol. See the organization page for the vision and the two-phase roadmap.

> **Phase 1.** Today the engine *translates* modern structures into what the 3.3.5a client already
> knows how to draw - Proton-style. The native D3D12 path is being grown alongside it. This repo is
> a working skeleton under active development, not a finished product.

## What it does

Everything happens inside the running client, through runtime hooks - no patched data files.

- **Modern M2 models on the old loader.** Relaxes the client's M2 version gate and rebuilds, at
  load time, the material contract a modern (`MD21`-era) skin doesn't carry - so the stock
  creature/doodad pipeline draws models it was never meant to understand.
- **Faithful per-batch alpha.** Hooks the shared alpha/material setter so modern blend modes pick
  the correct alpha-test reference instead of falling back to the WotLK default.
- **Multi-texture ribbons.** Expands modern ribbon emitters and combines their per-layer textures,
  which the single-texture WotLK ribbon path can't represent on its own.
- **A real D3D12 device.** A drop-in `d3d9.dll` proxy forwards the genuine device while forcing
  **D3D9On12** onto a D3D12 device the engine owns - the foothold the native renderer is built on.

## Architecture

Three artifacts, each with one job:

| Artifact | Role |
|---|---|
| `WraithPatcher.exe` | Standalone tool. Adds `Wraith.dll` to the target executable's import table (PE surgery) so it loads at startup - no runtime injector. |
| `Wraith.dll` | The core. Bootstraps off the loader lock on its own thread, brings up the hook engine (MinHook), and installs the M2 and Ribbon features. |
| `d3d9.dll` | The proxy. Forwards to the real `d3d9`, forces D3D9On12 onto the engine's own D3D12 device, and hosts the D3D12-side rendering. *Must be named `d3d9.dll` - it's a search-order proxy, not branding.* |

```
src/
├── Core/        Hook · Logger · Mem        hook-engine bring-up, logging, memory helpers
├── Engine/      Offsets · Gx · View        client offsets + typed views over engine objects
├── Features/
│   ├── M2/      MD21 · Modern · AlphaScope  the modern-model backport
│   ├── Ribbon/  Ribbon                      multi-texture ribbon combine
│   └── D3D12/   Device · Capture · Proxy    the d3d9 proxy and D3D12 device  →  builds d3d9.dll
└── Wraith.cpp   entry point  →  builds Wraith.dll
Patcher/         Patcher.cpp  →  builds WraithPatcher.exe
```

All addresses the hooks rely on live in `src/Engine/Offsets.hpp`, named and annotated. The reasoning
behind each one belongs in the project's Documentation Wiki - the code follows it.

## Building

The target client is a 32-bit process, so everything builds **Win32**.

**Requirements**
- CMake ≥ 3.25
- A Win32 C++17 toolchain (Visual Studio 2022 recommended)
- A legally-obtained 3.3.5a (12340) client

```sh
cmake -B build -DCLIENT_PATH="C:/path/to/your/3.3.5a/client"
cmake --build build --config Release
```

Build outputs: `Wraith.dll`, `d3d9.dll`, `WraithPatcher.exe`. When `CLIENT_PATH` is set, all three
are copied into the client automatically after the build. Vendored MinHook builds with the project.

## Install

1. Run `WraithPatcher.exe` **once** against your `Wow.exe` to add the import entry.
2. Make sure `Wraith.dll` and `d3d9.dll` sit next to `Wow.exe`.
3. Launch. `Wraith.dll` writes a startup log on bootstrap - check it to confirm the chain is live.

> Patching modifies your client binary. Work on a **copy**, keep an untouched backup, and only
> point this at a client and server you are permitted to modify and connect to.

## Status

- ✅ Injection chain - patcher (import-table surgery) → `Wraith.dll` → hook engine
- ✅ M2 modern-model backport - version gate, material contract, per-batch alpha
- ✅ Multi-texture ribbon combine
- ✅ `d3d9.dll` → D3D9On12 → D3D12 device bridge
- 🚧 Widening the native D3D12 render path (Phase 2 groundwork)

## Legal

Wraith Engine is an **interoperability project**. It distributes no Blizzard code and no game
assets, and runs only against a client you supply and own, reading that client's own files at
runtime. Reverse-engineering is limited to what is necessary for interoperability.

World of Warcraft and Wrath of the Lich King are trademarks of Blizzard Entertainment. This project
is not affiliated with or endorsed by Blizzard.

## License

Released under the **GNU General Public License v3.0** - see [LICENSE](LICENSE).

Bundles [MinHook](https://github.com/TsudaKageyu/minhook) (© Tsuda Kageyu, BSD 2-Clause) under
`vendor/minhook`, with its license retained.