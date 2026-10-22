# GRB-DLSS

NVIDIA DLSS Super Resolution for **Tom Clancy's Ghost Recon Breakpoint** (DirectX 11).

The game only ships temporal AA ("Temporal Injection"). This mod replaces that TAA pass with DLSS. It feeds DLSS the game's own low-resolution colour, depth, motion vectors and jitter, then writes the upscaled image back where the TAA output used to go. The rest of the frame (post-processing, UI) is untouched.

## Features

- DLSS Super Resolution / DLAA in place of the game's TAA.
- DLSS presets F, J, K, L and M (CNN, DLSS 4 and DLSS 4.5 transformer models).
- Custom internal render resolution at any percentage from 25% to 100%. This works by patching the engine's dynamic-resolution ratio, so the whole renderer follows it.
- In-game menu (**Home**) for render scale, preset and DLSS on/off. **End** switches between DLSS and the game's TAA live for comparison.
- Most `GRB_DLSS.ini` changes apply in-game as soon as you save the file.
- Works without other tools: it loads NVIDIA's NGX runtime from the driver. It also works alongside [OptiScaler](https://github.com/optiscaler/OptiScaler) (installed as `winmm.dll`), whose frame generation can then run on the mod's DLSS inputs.

## Requirements

- An NVIDIA RTX GPU and a current driver.
- Ghost Recon Breakpoint running on **DirectX 11**. Vulkan is not supported.
- In the game's graphics settings, **Anti-Aliasing = TAA** with Temporal Injection on. The mod hooks the TAA pass, so it does nothing when TAA is off.

## Install

1. Download [`bin/dinput8.dll`](bin/dinput8.dll) and [`bin/GRB_DLSS.ini`](bin/GRB_DLSS.ini) (prebuilt), or build them yourself (see below).
2. Copy both into the game folder, next to `GRB.exe`.
3. Optional: put a recent `nvngx_dlss.dll` (310.x or newer) in the game folder for the newer presets (J–M), or use the NVIDIA app's DLSS override.
4. Start the game, set Anti-Aliasing to TAA, and press **Home** for the menu.

To uninstall, delete `dinput8.dll`, `GRB_DLSS.ini` and `GRB_DLSS.log`.

> Breakpoint is an always-online game. The mod only touches rendering and does not change gameplay, but use it at your own risk, especially in PvP (Ghost War).

## Performance notes

DLSS cost depends on the **output** resolution, not the render resolution. Measured on an RTX 4060 Laptop GPU at 1440p output, compared with the game's TAA:

| Preset | Extra frame time |
|---|---|
| F (CNN) | ~1.5–2 ms |
| K (transformer) | ~2–3 ms |
| L (transformer, 4.5) | ~4–5 ms |

Preset K is the recommended default. Running DLSS through OptiScaler's DX11→DX12 bridge (`Dx11Upscaler=dlss_12`) adds roughly 20 ms per frame. If you use OptiScaler, set `Dx11Upscaler=dlss`.

## How it works

Breakpoint's renderer (AnvilNext) records the frame on D3D11 **deferred contexts**, so the TAA draw happens on a worker thread long before the GPU executes it. The mod handles this in four parts:

1. **Proxy and hooks.** `dinput8.dll` proxies the system DLL and hooks the D3D11 device and context vtables (`CreatePixelShader`, `PSSetShader`, draws, `Map`/`Unmap`, `FinishCommandList`, `ExecuteCommandList`).
2. **TAA detection.** The game's `TemporalAA_*` pixel shaders are identified when they are created. Their constant buffers (`cb_g_TemporalAA_Constants`, `cb_g_Pass`, `cb_g_Frame`) are shadowed on `Map`, which gives the mod jitter, sizes, the reprojection and the projection matrices.
3. **Command-list split.** At the TAA draw, the deferred command list is finished early (`FinishCommandList`). A "resolve" draw that copies the DLSS output into the TAA render target replaces the TAA shader. When the game submits the list with `ExecuteCommandList`, the mod executes the first part, runs a small compute pass to prepare colour, motion vectors and depth in the format NGX expects, evaluates DLSS on the immediate context, and then lets the rest of the frame run.
4. **Render scale.** `renderscale.cpp` patches the single instruction that stores the dynamic-resolution ratio (plus a secondary store), so the engine renders at the chosen percentage everywhere.

The in-game menu is drawn with D3D11 into the back buffer from a hook on the swap chain's `Present`. The font is a baked DejaVu Sans Mono atlas.

`slcam.cpp` (OptiScaler frame-generation camera data) and `probe.cpp` (memory probe used to find the render-scale global) are optional or developer-only. They are off by default.

## Build

The DLL is cross-built from Linux with clang and lld against the mingw-w64 headers. It has no static CRT and imports only Win32 and `msvcrt.dll`.

```sh
sudo apt install clang lld mingw-w64          # Debian/Ubuntu
./build.sh                                     # -> out/dinput8.dll
# or, with headers/libs elsewhere:
SYSROOT=/path/to/prefix ./build.sh             # expects $SYSROOT/x86_64-w64-mingw32/include
```

## Configuration

See [`config/GRB_DLSS.ini`](config/GRB_DLSS.ini). Every key is documented inline.

## Credits

- NVIDIA DLSS / NGX. No NVIDIA binaries are included; the runtime comes from the driver.
- DejaVu fonts, used for the menu.
- OptiScaler, for compatibility testing.

## License

MIT. See [LICENSE](LICENSE).
