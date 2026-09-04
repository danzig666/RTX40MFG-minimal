# RTX40MFG-minimal

DLSS Multi Frame Generation (3x / 4x) on RTX 40 series GPUs. One `.asi`, one
`.ini`. Works in D3D12 and Vulkan games that already ship DLSS Frame Generation.

**Confirmed working:** *007 First Light* (D3D12), *DOOM: The Dark Ages*
(Vulkan). Unsupported research software — it patches NVIDIA binaries in memory
on a path NVIDIA does not certify for Ada. Don't use it where anti-cheat is
watching.

## Usage

1. Download the latest release zip.
2. Copy `RTX40MFG.asi`, `RTX40MFG.ini` and `version.dll` next to the game's
   executable.
3. Launch. Enable Frame Generation in the game — its menu should now offer
   2x–6x. Pick one.

`version.dll` is [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader),
which loads the `.asi`. Games differ in which proxy name they load — if
nothing happens (no `RTX40MFG.log` appears), rename it. Known so far:

| Game | Loader name |
|---|---|
| *DOOM: The Dark Ages* | `version.dll` |
| *007 First Light* | `dinput8.dll` |

Use only one name at a time; two copies would load the mod twice.

If you use NVIDIA App's **DLSS Override** for the game, turn it off; it can
swap in a different Streamline plugin underneath the mod.

`RTX40MFG.ini`:

```ini
[MFG]
Multiplier=0   ; 0 = the game's own menu choice; 2/3/4 = force
Log=1          ; RTX40MFG.log beside the executable
```

## Requirements

- Windows x64, RTX 40 series GPU
- A game with working DLSS Frame Generation (Streamline)
- A supported DLSS-G provider (`nvngx_dlssg.dll` in the game folder, file
  version 310.x): 310.1.0, 310.2.0, 310.2.1, 310.3.0, 310.4.0, 310.5.0,
  310.5.2, 310.5.3, 310.6.0, 310.7.0, 310.7.128, 310.7.129, 310.8.0, 310.9.0

The provider ships **with the game**, not the driver. 310.x is the DLSS 4
generation; a 1.x or 3.x provider is DLSS 3 and has nothing for the mod to
work with. To swap one in, replace `nvngx_dlssg.dll` in the game folder with a
genuine NVIDIA-signed 310.x build (back up the original). The log prints the
detected version and the supported list whenever it rejects one.

## What it does

The frame generation itself is NVIDIA's. The mod changes four answers:

| Question | Stock RTX 40 | Here |
|---|---|---|
| Is this architecture allowed multi-frame? | no (arch id < Blackwell) | yes |
| How many frames may be requested? | 1 | 1–5 |
| Where in time does Ada evaluate them? | always `t = 0.5` | `t` and `1 - t` |
| How is output paced? | hardware flip metering (Ada has none) | software RSYNC |

```
sl.interposer.dll   slGetFeatureFunction → slDLSSGSetOptions observed / forced
                    slSetD3DDevice / slSetVulkanInfo → adapter verified (SM 8.9 via CUDA)
sl.dlss_g.dll       frame-count clamp removed; flip metering forced off
nvngx_dlssg.dll     arch-id compares 0x1b0 → 0x190; Ada SM89 PTX rebuilt so the
                    104 hardcoded 0.5 motion scales read t / (1-t); published
                    before feature creation
```

Everything is applied to the mapped image only — NGX verifies the provider's
signature at load, so patching the file on disk makes frame generation vanish.
Every step fails closed: Ada verification, provider version, three SHA-256
digests over source fatbin / source PTX / rebuilt fatbin, and exactly-one-match
on the byte patterns.

## Debugging

Every patch has a switch. They are **not** in the shipped ini; the defaults
are the working configuration. Add them under `[MFG]` only to bisect a
failure:

| Key | Default | Effect when 0 |
|---|---|---|
| `ArchGates` | 1 | leave the provider's arch-id compares; advertised max stays 1 |
| `AdaTemporalPatch` | 1 | stock Ada kernel; 3x+ collapses onto the midpoint |
| `FlipMetering` | 1 | leave hardware flip metering on; 3x+ freezes the image |
| `StreamlineMax` | 1 | leave the plugin's frame-count clamp |
| `NgxHooks` | **0** | *when 1:* inline detours on the provider's `CreateFeature`/`EvaluateFeature` to log NVIDIA's result codes. **Breaks feature creation** (PlatformError) on 310.7.128. Diagnostic only. |
| `VerifyAtCreate` | 1 | (only with `NgxHooks=1`) never verify the adapter inside `CreateFeature` |
| `LegacyNgxPatch` | 0 | the device-support NOP from upstream; not needed |

The log's second line lists the active switches. Useful lines:

```
DLSS-G provider found: version 310.7.128           the file the mod will patch
Arch gate: rewrote 2 of 2 arch-id comparisons      capability raised
[slSetD3DDevice] Ada temporal patch: ready=1       kernel published
[state] DLSSGState: status=eOk presented=4 max=5   frames actually presented
```

`presented` is what DLSS-G really put on screen per rendered frame; a non-`eOk`
`status` names why it did not (Reflex off, resolution, HDR format).

## Build

Only the Streamline SDK's `include/` is needed. MinHook is vendored.

```bash
cmake -B build -S . -A x64 -DSTREAMLINE_ROOT=<Streamline SDK>
cmake --build build --config Release
```

or `build.bat` with `STREAMLINE_ROOT` set. Static CRT; depends only on
`kernel32`, `bcrypt`, `version`. Clean at `/W4` on VS 2019 Build Tools.

## Lessons from getting it to work

Two things this port had simplified away from the upstream project turned out
to be why it didn't work, found by bisecting on hardware:

- **Adapter verification inside `CreateFeature`** — `cuInit` and a load/free
  of `nvcuda.dll` on the render thread while NVIDIA brings up its own CUDA
  context. Now done at `slSetD3DDevice`, as upstream does.
- **Inline detours on the provider's exports.** NVIDIA refuses feature
  creation while they are present. Upstream hooks those entry points through a
  deliberately careful atomic-hotpatch framework; the RenoDx fork never touches
  them. The kernel patch never needed them — it swaps a pointer in the
  provider's data section, and `slDLSSGSetOptions` fires right before feature
  creation.

## Layout

```
src/
├── loader.cpp        DllMain, config, module discovery, dispatch
├── config.*          RTX40MFG.ini
├── log.*             RTX40MFG.log
├── patches.*         NGX device-support NOP, Streamline clamp
├── gates.cpp         arch gates, flip metering
├── streamline.*      interposer hooks, SetOptions, state telemetry
├── ngx.*             provider registration, Ada patch trigger, optional detours
├── ada_patch.*       Ada SM89 temporal correction
└── provider_policy.* supported provider versions
```

~2,900 lines against upstream's 14,499.

## Credit and licence

MIT. See `LICENSE`.

- **Michael Robles**, [RTX40MFG-Unlock](https://github.com/dashdogy/RTX40MFG-Unlock) — the
  mechanisms, byte patterns, provider profiles and the entire Ada temporal
  correction. This is a reduction of that work.
- **mavismmg**, [MFGAdaUnlock-RenoDx](https://github.com/mavismmg/MFGAdaUnlock-RenoDx) — the
  arch-gate and flip-metering techniques and the analysis of why both are
  required.
- **ThirteenAG**, [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) —
  bundled as `version.dll`.
- MinHook — Tsuda Kageyu, BSD 2-clause.
