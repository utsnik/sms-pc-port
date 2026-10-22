# Galaxy JAudio2 ↔ the shared DSP HLE

Stage 2 Task 9. This compares the protocol Super Mario Galaxy's JAudio2 expects of the audio DSP task with the one `platform/audio` emulates for Sunshine (`platform/audio/PROTOCOL.md`). Where they differ, the HLE has an `#if defined(PORT_GAME_SMG)` branch, so Sunshine's build is unchanged (`sms-guard.sh`).

Galaxy sources: `games/smg/decomp/src/JSystem/JAudio2/` (`JASDSPInterface.cpp`, `JASAudioThread.cpp`, `JASDSPChannel.cpp`, `dspproc.cpp`, `dsptask.cpp`, plus `osdsp_task.cpp`, `JASAiCtrl.cpp`, `JASAramStream.cpp`). These five files are byte-identical to GalaxyQuest `wiiu-stage2`'s copies (`cmp`, 2026-10-22).

Earlier research for this task: `.superpowers/sdd/2026-10-22-smg-wiiu-stage2-boot-cemu/research-audio-protocol.md` and `research-ast-stream.md`. Where this page disagrees with them, this page is right; the corrections are listed at the end.

Checks:

- `tools/tests/test_smg_wiiu_audio_protocol.py` reads the facts below from the sources and checks each row's status against them. Every `difference` must name a fix (`path#tag`): the file must carry an `smg-audio: <tag>` marker, and in shared `platform/` code that marker must sit inside the guard.
- `platform/audio/tests/smg_galaxy_protocol_test` plays the retail title BGM through the HLE.

## Comparison

Status values:

- `match`: identical.
- `compatible`: the wire differs, but the existing HLE already handles it. The host test covers it.
- `difference`: fixed by an SMG-guarded branch.

| ID | Element | Sunshine (`PROTOCOL.md`, `dsp_hle.cpp`/`dsp_mixer.cpp`) | Galaxy evidence | Status | Fix |
| --- | --- | --- | --- | --- | --- |
| `mail-prefixes` | Task mails `0xDCD1000x`, request reply `0xF355xxxx` | `__DSPHandler`: `0xDCD10004` → `req_cb` reads `0xF355xxxx` | `osdsp_task.cpp:20-140` has the same state machine (`0xDCD10004` → `req_cb`, :104-106). The microcode in `dsptask.cpp` mails `0xdcd1`/`0xf355` | match | — |
| `boot-handshake` | Task start | `0xDCD10000` → `init_cb` = `DspHandShake` waits for one more mail | `dsptask.cpp:11-22` `DspHandShake`, the same shape | match | — |
| `send-framing` | `DSPSendCommands2` | count, `DSPAssertInt`, words; **count 0 = 2 words** | `dsptask.cpp:292-298`: count 0 = **1 word** | difference | `platform/audio/dsp_hle.cpp#D1` |
| `release-halt` | Voice release per subframe | One release carries the 64-bit map (2 words). The HLE renders and replies `0xF355FF00` on every release | `JASDSPChannel::updateAll` (`JASDSPChannel.cpp:198-206`) sends four `DSPReleaseHalt2(group)` per subframe, groups 0, 1, 2, 3. Each is one word: `group << 16 \| DSP_CreateMap2(group)` (`dspproc.cpp:5-10`, `JASDSPInterface.cpp:345-356`). `JASAudioThread` counts one `FF00` per subframe | difference | `platform/audio/dsp_hle.cpp#D2` |
| `acks` | Acknowledgements | `0xF355FF00` = subframe; other `0xF355xxxx` → `DspFinishWork(xxxx)` | `JASAudioThread::DSPCallback` (`JASAudioThread.cpp:86-101`): `(mail & 0xFF00) == 0xFF00` → `AUDIOMSG_DSP`, otherwise `finishWork(mail)` | match | — |
| `cmd-0x81` | `DsetupTable` | `0x81000000 \| n`, `CH_BUF`, `DSPRES_FILTER`, `DSPADPCM_FILTER`, `FX_BUF`; ack `0xF3558100` | `dspproc.cpp:20-33` sends the same 5 words. `JASDSPInterface.cpp:118` passes `n` = 0x40. Both tables are byte-identical to Sunshine's (on the BE target, `u8[64]` = `u32[16]`) | match | — |
| `cmd-0x82` | Frame sync | `0x82000000 \| sub << 16 \| level`, `outA`, `outB`; no ack | `DsyncFrame2ch` (`dspproc.cpp:42-50`) sends the same first 3 words, **padded to 5** (`0, 0`). `dsp_hle` reads `w[1]`/`w[2]` once `w.size() >= 3`, and a 5-word count completes the command | compatible | — |
| `cmd-0x8E` | `DsetVARAM(base)` | absent: falls into `default:` (ack only, payload dropped) | `dspproc.cpp:67-72` sends `{0x8E000000, base}` and spins until `DspFinishWork(0x8E00)`. Sent once by `JASDsp::boot` (`JASDSPInterface.cpp:28`) after `DspBoot` and **before** `0x81`. The base is `JKRHeap::getAltAramStartAdr()` | difference | `platform/audio/dsp_hle.cpp#D3`, `games/smg/platform/wiiu/runtime.cpp#D3` |
| `cmd-4ch` | `DsyncFrame4ch` (4 outputs) | absent | `dspproc.cpp:52-61`. Its only caller is `DsyncFrame3`, called only from `Dsp_Update_Request` case 2, and case 2 is set only inside `DsyncFrame3` (`osdsp_task.cpp:154-178`), so it is unreachable | compatible | — |
| `frame-size` | Frame / subframe | 560 samples = 7 × 80 | `JASAiCtrl.cpp:46` `sSubFrames = 7`, `:239-240` `getFrameSamples() = sSubFrames * 0x50` | match | — |
| `sample-rate` | DAC rate | 32028.5 Hz | `JASAiCtrl.cpp:44` `sDacRate = 32028.5f`. `initAI` (`:49`) always uses `OUTPUT_RATE_0`; `OUTPUT_RATE_1` (48 kHz, 10 subframes) has no caller | match | — |
| `output-order` | `outA`/`outB` | `outB = outA + frame`; DMA pairs `(outB, outA)` | `finishDSPFrame` passes `buf`, `buf + getFrameSamples()` (`JASAiCtrl.cpp:210`). `readDspBuffer` calls `imixcopy(endDacBuffer, dacBuffer, …)` (`:190-192`) | match | — |
| `voice-struct` | `TChannel` | 0x180 × 64 at the PROTOCOL.md offsets | `JASDspInterface.hpp:49-96` has the same offsets field for field, and the same 0x180 size | match | — |
| `mix-sends` | Send slots | 6 × {bus, target, current, level} at 0x010 | Every caller loops `i < 6` (`JASChannel.cpp:232,261,332,364`; `BUSOUT_CPUCH = 6`). The decomp's `_10[1][4]` "size unknown" is padding to 0x050 | match | — |
| `bus-table` | Voice send buses | `connect_table`: 0x0D00, 0x0D60, 0x0DC0, … 0x09A0 | `setBusConnect`'s `connect_table` (`JASDSPInterface.cpp:337-339`) is identical | match | — |
| `send-table` | FX-line return buses | `SEND_TABLE` = the bus list | Galaxy's `SEND_TABLE` (`JASDSPInterface.cpp:18-20`): 0x0D00, 0x0D60, **0x0DC8, 0x0E28, 0x0E88, 0x0EE8**, 0x0CA0, … The mixer's `bus_index` drops a return aimed at the +8 addresses | difference | `platform/audio/dsp_mixer.cpp#D6` |
| `fx-block` | `FxBuf` | 0x20 × 4: mode, length, buffer, 2 × (bus, gain), 8 taps | `JASDspInterface.hpp:19-29`, `setFXLine` (`JASDSPInterface.cpp:122-150`), the same layout | match | — |
| `format-codes` | `samplesSourceType` | 9 ADPCM4, 5 ADPCM2, 8 PCM8, 16 PCM16 | `COMP_BLOCKBYTES = {9, 5, 8, 0x10, …}` (`JASDSPInterface.cpp:204`). Streams use format 3 → 16 (`JASAramStream.cpp:652-661`); all 74 retail `.ast` files are PCM16. `0x21` (direct PCM) is never set | match | — |
| `dsp-cursor` | DSP-written read cursor | The mixer writes 0x068 (position) for waves and touches 0x074 only for `0x21` streams. A wave loops when the position reaches 0x114 | `JASAramStream::updateChannel` reads the DSP's **0x070 ARAM address** to see which ring block is playing and refill it (`:468-473`). It reads 0x074 + 0x064 as the samples left (`:412`, `:529`). It **moves the loop point by rewriting 0x074** (`:461`, `:498`, `:515`), so the DSP must loop when 0x074 runs out and then reload it from 0x114 − 0x110. These are the Zelda-microcode semantics (Dolphin's Zelda HLE; GalaxyQuest's `dsp_mixer.cpp` `downloadPcm`) | difference | `platform/audio/dsp_mixer.cpp#D4` |
| `start-offset` | PCM start | always sample 0 | `TChannel::setWaveInfo` sets 0x068 = skip for PCM8/PCM16 (`JASDSPInterface.cpp:229-233`; ADPCM moves 0x118 instead, `:221-227`), and the DSP starts there | difference | `platform/audio/dsp_mixer.cpp#D5` |
| `mixer-level` | Master level | Q15 word from `0x82`; SMS sends 0x5000 | `DSP_MIXERLEVEL` defaults to 0x4000; `setDSPMixerLevel` sends 4096 × 4 × level (`dspproc.cpp:36-40`, `JASDSPInterface.cpp:44-49`). The word is opaque to the HLE. Galaxy's actual level is not yet measured against retail | compatible | — |
| `aram-provider` | ARAM storage | `platform/ar/ar.cpp`: a separate 16 MiB buffer; `ARStartDMA(type, mainmem, aram)` | Wii virtual ARAM. `createRootHeap` reserves a 0xE00000 MEM2 window and names its low mark with `setAltAramStartAdr` (`HeapMemoryWatcher.cpp:127-129`). Real Dolphin `ARInit` starts allocation at offset 0x4000 and keeps `__AR_Size` as the total addressable size; `ARAlloc` checks the request against the remaining total (`decomp/libs/dolphin/src/ar/ar.c:62-78`). The SMG aralt therefore reports 0xE04000 (`0x4000` reserve + `0xE00000` usable), so `ARAlloc(0xE00000)` spans [0x4000, 0xE04000). `JKRAramPiece::startDMA` calls `ARStartDMA(dir, src, dst)` (`Overwrite.cpp:1072`), which is aralt's order (`aralt.c:26,31`) | difference | `games/smg/cmake/sources.cmake#aralt/aralt.c` |

## The fixes

**D1, count-0 framing** (`dsp_hle.cpp`, `port_audio_dsp_assert_int`). A count of 0 expects one word.
- Without it, every release swallows the next command's count mail and the mailbox desynchronises.

**D2, four releases per subframe** (`dsp_hle.cpp`, the release branch of `execute`).
- Releases for groups 0–2 are consumed silently.
- Group 3 renders the subframe and posts the single `0xDCD10004`/`0xF355FF00`.
- Without this the HLE renders and reports up to four times per subframe.

**D3, `0x8E DsetVARAM`** (`dsp_hle.cpp`, `case 0x8E` and `aram_lookup`).
- The handler stores the base and acks `0xF3558E00` (DsetVARAM busy-waits on that ack).
- From then on the mixer's ARAM resolver returns `port_varam_ptr(base + offset)`. Before `0x8E`, nothing resolves. The resolver accepts the reserved-offset range through `0xE04000`, while rejecting offsets below `ARGetBaseAddress()` and offsets beyond the `0xE00000` usable window.
- `port_varam_ptr` is provided by:
  - the Galaxy Wii U runtime (`games/smg/platform/wiiu/runtime.cpp`): a bounds-checked cast inside the MEM2 arena that holds the window;
  - the host test: its own MEM2 window at 0x90000000.
- The SMG build also drops `maybe_dump_aram`, which reads Sunshine's `port_aram_ptr`.

**D4, the DSP's cursor** (`dsp_mixer.cpp`: `start`, `pull`, `render_voice`). For every wave voice that is not an oscillator:
- At reset the remaining length starts at `endPosition − start`.
- A CPU rewrite of 0x074 is adopted, using the `written` bookkeeping the direct-PCM path already has.
- The voice loops when the remaining length reaches 0, then reloads it as `0x114 − 0x110` at that moment.
- After each subframe the mixer writes back 0x070 (the next ARAM byte to fetch: `base + 2·idx` for PCM16, `idx` for PCM8, ⌈idx/16⌉ × frame bytes for ADPCM) and 0x074.
- Without this:
  - the stream manager's block index is `(0 − wavePtr) / 10080`, its refill loop never settles, and the ring is never refilled;
  - its loop-end patch through 0x074 is ignored, so the voice plays past the track's loop end.

**D5, PCM start** (`dsp_mixer.cpp`, `start`). PCM8 and PCM16 start decoding at 0x068.

**D6, FX return buses** (`dsp_mixer.cpp`, `prepare_fx`). A return aimed at a bus address + 8 (0x0DC8, 0x0E28, 0x0E88, 0x0EE8) is mixed into that bus.
- **Assumed**: the 8-word offset is probably an 8-sample delay inside the DSP's 0x60-word bus area. It is not modelled.

**D7, ARAM provider** (`games/smg/cmake/sources.cmake`, `platform/wiiu/CMakeLists.txt`). Galaxy links RVL_SDK `aralt.c`, and `platform/ar/ar.cpp` moves to `SMG_WIIU_UNSELECTED_PROVIDER_PATHS`.

### Why aralt and not ar.cpp (the `0x8E` decision)

The decisive reason is that the CPU copy and the DSP lookup share the same
VARAM base: the DSP must read ARAM at the address where the CPU's DMA wrote it.

- **On the Wii, both use the same base.**
  - aralt's `ARInit` sets `__ARH_BaseAdr = MEM2ArenaLo`.
  - That is the value `createRootHeap` already gave `setAltAramStartAdr`, which `JASDsp::boot` sends with `0x8E`.
  - So ARAM offset *a* is main memory `VARAM + a` for both the copy and the DSP; offsets below 0x4000 are the ARAM allocator's reserved stack/header area, not audio heap data.
- **With ar.cpp the base would be meaningless.**
  - Offsets index a private buffer, so `port_aram_ptr(a)` is right and `VARAM + a` is wrong.
  - Galaxy does not currently reach direction-1 copies through
    `JKRAramPiece::startDMA`; the shared VARAM base remains the decisive
    compatibility requirement.
- **The donor port made the same choice.** GalaxyQuest `wiiu-stage2` links `aralt` (`CMakeLists.txt` `SDK_MODULES`) and resolves `varam + offset`.
- **Other effects:**
  - `test_smg_wiiu_source_boundary.py` (which expects `aralt.c`) passes again.
  - Galaxy's 14 MiB MEM2 reservation is used, instead of an extra 16 MiB `calloc`.

## Host test

`make -C platform/audio/tests smg_galaxy_protocol_test` builds `smg_galaxy_protocol_test.cc` with `dsp_hle.cpp` and `dsp_mixer.cpp` (`-DPORT_GAME_SMG`). Running `platform/audio/tests/smg_galaxy_protocol_test [DATA/files]` does the following.

**Galaxy's command layout.** The test sends Galaxy's own words, in order:
- the task boot;
- `{0x8E000000, 0x90000000}`;
- `0x81000040` with 32-bit (`MAP_32BIT`) `CH_BUF`/table/`FX_BUF` addresses;
- per frame, a 5-word `0x82`;
- per subframe, four one-word releases.

It checks:
- the acks `0x8E00` then `0x8100`;
- exactly one `FF00` per subframe, after group 3 only;
- that no ARAM lookup bypasses the VARAM base.

**The retail title BGM.** The test parses `AudioRes/Stream/SMG_title_strm.ast` from the disc at run time. The file is PCM16, 2 channels, 32000 Hz, 10080-byte blocks, loop [565526, 1421275), 282 blocks.
- It places the blocks VARAM-relative at ARAM 0x4000 (`ARGetBaseAddress`), in JASAramStream's ring layout.
- It starts two voices the way `channelStart` does (format 16, loop flag 0xFFFF, end = 3 blocks, pitch 0x0FFC, buses 1/2).

**The stream manager.** After every subframe the test runs `updateChannel`'s CB_PLAY arithmetic:
- the position counter from 0x074 + 0x064;
- the block index from 0x070, which triggers refills;
- the loop-end patch of 0x110/0x074.

**What it asserts.**
- **First BLCK block:** the first frame is non-silent, 0x070 = base + 2 × fetched, and 0x074 = ring − fetched.
- **Whole track:** every output sample of both channels equals the spliced track within 2 LSB, through 286 ring refills and the loop splice. The identity resampling taps and the Q14/Q15 unity gains make this exact.
- **Stream position:** the stream manager's own position equals the DSP's fetch count.
- **Ring growth:** an endPosition patch (0x114 and 0x074 + one block, `JASAramStream.cpp:497-517`) grows the ring mid-play, and the voice reloads 4 blocks.
- **PCM start offset:** a 0x068 skip is honoured.
- **FX return:** a return through `SEND_TABLE[2]` (0x0DC8) reaches bus 3.
- **Final line:** `audio-protocol-test: PASS: non-silent output …`.

If the disc is absent, the test prints `audio-protocol-test: SKIP: …` and exits 0. No game data is in the tree.

## Corrections to the research notes

- **Release halt is not identical.** `research-audio-protocol.md` §1 calls it identical. It is not: the count-0 framing is 1 word (D1), and there are four releases per subframe (D2).
- **Streams are not already handled.** `research-ast-stream.md` §2 says the mixer "already works" for streams. That is true only for 0x110/0x114. The stream manager also needs 0x070/0x074 (D4).
- **The FX block contents differ.** The FX block *layout* matches, but Galaxy's `SEND_TABLE` values differ (D6).
- **AST offsets are wrong.** The `.ast` `Header` is **0x40** bytes, not 0x48, and `BlockHeader` is **0x20**, not 0x1C. The first BLCK is at 0x40, channel 0's PCM at 0x60, and channel 1's at 0x60 + 0x2760.

## Left for later tasks

- **Task 10.** Do not add `platform/ar/ar.cpp` to smg: aralt is the AR provider. `port_varam_ptr` already exists in the SMG runtime. Linking `dsp_hle.cpp`/`dsp_mixer.cpp` into smg must keep `PORT_GAME_SMG` defined; `smg_wiiu_shared` already defines it.
- **Before any ARAM data can play on the Wii U** (blocks both wave banks and streams, so the title BGM stays silent): `JKRAramPiece::startDMA` (`games/smg/decomp/src/Game/System/Overwrite.cpp:1056-1074`) silently completes every command whose `mSrc < 0x80000000`, `mDst >= 0x04000000` or `length > 0xE00000`. On the Wii U every main-memory address is below 0x80000000: the RPX data and the static MEM1/MEM2 arenas sit at 0x10000000 and up. So every main-RAM → ARAM copy, and every ARAM → main-RAM copy, is dropped before reaching `ARStartDMA`.
- **Not implemented** (as for Sunshine):
  - per-voice FIR8/IIR/distance filters;
  - surround buses;
  - level calibration against a retail oracle trace.
