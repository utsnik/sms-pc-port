# Galaxy Wii U Stage 1 ABI and endian policy

`PORT_HOST_BIG_ENDIAN` is derived from the compiler byte-order macros in
`games/smg/include/port/compat.h`. `TARGET_PC` remains enabled for the
decompilation's existing port hooks, but it is never used as an endian test.
The Wii U target is native Espresso big-endian, so retail command streams,
textures, TLUTs, display lists, bitfields, and four-byte `PTR32` fields are
read in identity order.

## Executable boundaries

- `__lwbrx`, `__lhbrx`, `__stwbrx`, and `__sthbrx`: guarded byte-reversal;
  reverse only on a native-BE host, because the host implementation emulates
  the PowerPC byte-reversed operation.
- `__PortGXFifo_u32`: guarded-off host swap on native BE; little-endian hosts
  swap the emitted word so the recorded FIFO remains retail BE.
- `StringUtil.cpp` cooked UTF-16 conversion: guarded little-endian conversion;
  native BE reads the retail code-unit bytes directly.
- `JKRUnitHeap.cpp` bitmap words: native-BE identity/no-op; little-endian
  hosts use `__builtin_bswap32` to preserve the bitmap's MSB-first scan order.
- `TPL.c` pointer relocation: guarded by actual host endian. Both hosts keep
  the file's 32-bit offset fields as `u32`; relocation adds the palette base
  and converts to a host pointer only at a dereference. No host pointer is
  stored in a four-byte retail field.

## ABI audit dispositions

- `uintptr_t` and pointer subtraction are native-BE/host-address calculations
  only; any `u32` assignment is a deliberate retail 32-bit slot or a deferred
  data-loader boundary. No 64-bit pointer is written into a game structure.
- `u32` pointer slots and `PTR32(T)` fields are native-BE four-byte fields.
  Their runtime conversion is deferred until the Stage 2 resource loader.
- 64-bit fields are retained where the host API requires them; no
  `std::atomic<uint64_t>` or `-latomic` is selected for Stage 1.
- TLS and thread-local storage are guarded-off from the Galaxy Wii U runtime;
  Stage 1 uses wut-owned thread state only.
- Host allocator and pointer relocation assumptions are deferred: Stage 1
  does not open or cook game data, and the fixed aligned runtime allocator is
  the only entry-boundary allocation policy.
- Direct swaps are limited to the four byte-reversed intrinsics, the FIFO
  writer, and the JKR bitmap scan. All are classified above; no blanket
  `TARGET_PC` endian branch remains.
- Existing Sunshine SMS-only renderer and generic endian utility swaps shown
  by the repository-wide audit (`platform/wiiu/gx2_renderer.cpp`,
  `platform/gx/src/`, `platform/endian/endian_util.h`, and the host-only
  `tools/mods/eclipse/fixup_sources.py`) are not selected by `PORT_GAME=smg`;
  they are shared-repository deferred/guarded-off code, not Galaxy ABI
  boundaries.

This policy intentionally leaves data loading, resource relocation, and any
runtime ABI not exercised by the no-data Stage 1 entry point as deferred work.
