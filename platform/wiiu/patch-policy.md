# Wii U Stage 1 patch policy

The target keeps `TARGET_PC` enabled so the existing patch and source
discovery pipeline remains shared with the host port. `PORT_HOST_BIG_ENDIAN`
selects the native Espresso behaviour at the helper boundary; it is not a
blanket byte swap. Textures, TLUTs and display lists remain in their retail
byte order for the later GX2 backend, while CPU-read arrays are covered by the
existing resource policy.

| Patch | Native-BE disposition | Basis |
| --- | --- | --- |
| `endian-01` | correct on BE unchanged | `port_be32`/`port_be16` assemble raw BE bytes |
| `endian-02` | correct on BE unchanged | raw magic/size reads use explicit BE helpers |
| `endian-03` | correct on BE unchanged | BMG raw fields use explicit BE helpers |
| `endian-04` | correct on BE unchanged | `JUTColor` uses the value-level RGBA contract |
| `endian-05` | correct on BE unchanged | `port_be32` preserves PRM's raw BE values |
| `endian-06` | correct on BE unchanged | SPC immediates are assembled by bytes |
| `endian-07` | correct on BE unchanged | stream header fields are already native BE |
| `endian-08` | correct on BE unchanged | scene stream reads use `port_be*` |
| `endian-09` | correct on BE unchanged | display-list vertex count uses `port_be16` |
| `endian-10` | correct on BE unchanged | sequence/FIR fields retain their BE contract |
| `endian-11` | identity on BE | `port_bswap16`/`port_bswap32` are identity on BE; `port_be16` reads raw card bytes |
| `endian-12` | correct on BE unchanged | THP header fields are native BE |
| `endian-13` | correct on BE unchanged | JSU length is read through the native BE path |
| `endian-14` | correct on BE unchanged | display-list indices use explicit BE reads |
| `endian-15` | correct on BE unchanged | retail BE sphere bytes remain registered for GX consumption |
| `endian-16` | correct on BE unchanged | native archive buffers need no host conversion |
| `endian-17` | identity on BE | `port_res_to_native_named` is a BE no-op |
| `endian-18` | correct on BE unchanged | failed-read preservation does not alter byte order |
| `endian-19` | correct on BE unchanged | Z24X8 CPU reads use `port_be16` |
| `0011` | guarded off on BE | all RARC/resource calls are under `!PORT_HOST_BIG_ENDIAN` |
| `0012` | correct on BE unchanged | `port_be32` assembles the Yaz0 length on either host |
| `0013` | correct on BE unchanged | its native branch is selected by the BE ABI |
| `0017` | correct on BE unchanged | the BP word helper assembles bytes explicitly |
| `audio-01` | guarded off on BE | `!PORT_HOST_BIG_ENDIAN` leaves ABI bitfield order native |
| `audio-02` | guarded off on BE | `!PORT_HOST_BIG_ENDIAN` leaves ABI bitfield order native |
| `thp-01` | deferred | host decoder replacement is outside the link-only runtime |
| `thp-02` | correct on BE unchanged | THP audio header is native BE; payload handling is deferred |
| `0016` | deferred | movie skipping is a Stage 1 runtime policy, not byte conversion |

The direct-bswap audit is intentionally narrow and repeatable:

```sh
rg -n "__builtin_bswap" decomp-patches platform
```

At Stage 1 the remaining direct uses are the card-fixup patch text and the
little-endian resource helper implementation; all runtime converter paths are
behind `PORT_HOST_BIG_ENDIAN` or use `port_bswap16`/`port_bswap32`.
