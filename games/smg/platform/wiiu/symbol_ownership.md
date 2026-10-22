# SMG Wii U Stage 1 symbol ownership

This is the source and ABI review manifest. A listed Petari path is a donor
candidate only; it is not permission to add a second provider. Task 3 must
turn every residual undefined symbol into exactly one shared provider or one
documented Galaxy stub.

## Provider table

| Symbol family | Stage 1 owner | Petari source disposition |
| --- | --- | --- |
| GX*, FIFO, display-list parsing, vertex emission | Sunshine `platform/gx/src/gx_api.cpp`, `gx_fifo.cpp`, `gx_vert.cpp`, and its Wii U integration | Exclude all `decomp/src/RVL_SDK/gx/*.c`, including `GXFifo.c` and `GXPerf.c` |
| GD* command builders | Sunshine Dolphin GD set: `GDBase.c`, `GDGeometry.c`, `GDLight.c`, `GDPixel.c`, `GDTev.c`, `GDTransform.c` | Exclude those duplicate Petari files; retain only `GDIndirect.c` and `GDTexture.c` when the symbol manifest proves Sunshine has no provider |
| OS*, OSThread, queues, mutexes, arenas, time, cache, reset | Sunshine `platform/wiiu/os_core.cpp`, `os_thread.cpp`, `runtime.cpp`, and explicit wut adapters | Exclude all Petari `os/*.c` and `mem/*.c`; do not link same-named wut/Revolution implementations |
| AI*, DSP*, `__DSP_*` | Sunshine `platform/audio/ai.cpp`, `dsp_hle.cpp`, `dsp_mixer.cpp`, `noaudio.cpp`, and the shared Wii U audio adapter | Exclude Petari `ai/*.c`, `dsp/*.c`, `ax/*.c`, and `axfx/*.c` |
| VI*, DVD*, PAD*, KPAD*, WPAD* | Sunshine Wii U `vi.cpp`, `dvd.cpp`, and `pad.cpp`; Galaxy-only KPAD/WPAD shape is a logged stub boundary | Exclude Petari `vi/*.c`, `dvd/*.c`, `pad/*.c`, `kpad/*.c`, and `wpad/*.c` |
| AR*, PSMTX*, and common platform math | Sunshine `platform/ar/ar.cpp`, `platform/mtx/mtx.cpp`, and shared math/misc | Exclude Petari `ar/` and `mtx/` duplicates; retain `aralt/aralt.c` only for absent ARQ/ARAM-alt symbols |
| ARC* and TPL* software readers | Petari `arc/arc.c`, `aralt/aralt.c`, and `tpl/TPL.c` where the manifest requires them | No Sunshine duplicate; `TPL.c` receives the native-BE policy guard in Task 4 |
| Galaxy game, JSystem/JAudio2, NW4R, Galaxy service calls | `games/smg/decomp/src/Game/**`, `JSystem/**`, `nw4r/**`, and the explicit stub boundary | Petari contribution; no Sunshine game objects |

## Selected Petari source set

`games/smg/cmake/sources.cmake` is the machine-readable source manifest. It
contains all Game, JSystem, and NW4R C/C++ sources, plus exactly:

- `RVL_SDK/arc/arc.c`
- `RVL_SDK/aralt/aralt.c`
- `RVL_SDK/tpl/TPL.c`
- `RVL_SDK/gd/GDIndirect.c`
- `RVL_SDK/gd/GDTexture.c`

The Petari GX, OS, MEM, AI, DSP, AX, AXFX, VI, DVD, PAD, KPAD, and WPAD
families are not in `smg_game`. The duplicate Petari GD files are not in
`smg_game`: `GDBase.c`, `GDGeometry.c`, `GDLight.c`, `GDPixel.c`, `GDTev.c`,
and `GDTransform.c`.

## Unresolved candidate families

These donor families are recorded for the symbol audit and are not selected
by the Stage 1 source manifest. Task 3 must assign each residual symbol to a
shared provider or to a finite, named stub; a wildcard stub is forbidden.

| Candidate family | Current status |
| --- | --- |
| NAND/FST/FS (`nand`, `fs`) | unresolved; no data access in Stage 1 |
| KPAD/WPAD (`kpad`, `wpad`) | unresolved; logged safe-failure boundary |
| optional VI/DVD service (`vi`, `dvd`) | unresolved residuals only; shared providers own core APIs |
| GX peek/performance (`gx`) | unresolved residuals only; shared GX owns core APIs |
| MEM (`mem`) | unresolved residuals only; shared wut/runtime ownership |
| OS/thread/queue/mutex (`os`) | unresolved residuals only; shared Sunshine adapters |
| AI/DSP/AX/AXFX (`ai`, `dsp`, `ax`, `axfx`) | unresolved residuals only; shared audio ownership |
| PAD (`pad`) | unresolved residuals only; shared Sunshine pad ownership |
| AR/MTX (`aralt`, `mtx`) | `aralt` selected above; MTX duplicate excluded |
| IPC, EXI, SI, USB, WUD (`ipc`, `exi`, `si`, `usb`, `wud`) | unresolved; later platform work only |
| NWC24, NET, RSO, SC, BTE (`nwc24`, `net`, `rso`, `sc`, `bte`) | unresolved; no Stage 1 service startup |
| base, DB, ESP, EUART, THP, VF (`base`, `db`, `esp`, `euart`, `thp`, `vf`) | unresolved; candidate symbols require explicit review |

## Rules

1. One strong provider per ABI symbol; link order must not resolve collisions.
2. Weak definitions are accepted only for symbols explicitly marked as a
   Stage 1 no-op in `sdk_stubs.cpp` and this manifest.
3. Stage 1 does not open DVD/FST, initialize NAND/save services, start game
   threads, or claim a renderer/audio implementation beyond the shared Wii U
   boundary.

## Task 3 provider set

`platform/wiiu/CMakeLists.txt` selects each shared Sunshine provider exactly
once in `SMG_WIIU_SHARED_PROVIDER_SOURCES`: GX API/FIFO/vertex emission, OS
core/thread/runtime, VI/DVD/PAD, AI/DSP/no-audio, AR, and MTX.  The Petari
source manifest supplies only the five software readers/builders listed above.

`games/smg/platform/wiiu/sdk_stubs.cpp` owns the finite
`SMG_STAGE1_STUB_SYMBOLS` list in its header.  It covers only residual
link-only NAND, KPAD/WPAD, GX peek/performance calls.  Each entry logs once
and returns the documented zero safe failure.  It is not a weak wildcard and
does not define a second GXInit, GXSet*, GD*, OS*, AI*, DSP*, VI*, DVD*, PAD*,
or AR* provider.
