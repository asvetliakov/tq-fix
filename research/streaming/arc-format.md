# The `.arc` container, decoded

`tools/arcinfo.py` is the program that produced every number here.  It is
standard-library Python, reads only, and is re-runnable against any
installation:

```sh
G="$TQ_GAME_DIR"
research/streaming/tools/arcinfo.py --summary "$G/Resources"
research/streaming/tools/arcinfo.py "$G/Resources/Levels.arc"
```

The layout below was recovered by parsing the containers, and it is consistent
with the runtime structures `findings.md` recovered independently from
`Engine.dll`: entry record stride `0x44`, block descriptor stride `0xc`,
`entry[0] & 2` meaning compressed, and `archive[0x40]` holding the block size.

## Layout

```
header (0x800 bytes; file data begins at 0x800)
  +0x00  char magic[4]  "ARC\0"     +0x10  u32 partTableSize == partCount*12
  +0x04  u32  version   1           +0x14  u32 stringTableSize
  +0x08  u32  fileCount             +0x18  u32 tocOffset
  +0x0c  u32  partCount

toc, at tocOffset:
  [partCount x 12]  block records { u32 offset; u32 compressedSize; u32 uncompressedSize; }
  [stringTableSize] name blob, NUL-separated, lowercase, '/' separated
  [fileCount x 44]  file records
     +0x00 flags (bit 1 = compressed)   +0x1c blockCount
     +0x04 offset of first block        +0x20 firstBlockIndex
     +0x08 compressedSize               +0x24 nameLength
     +0x0c decompressedSize             +0x28 nameOffset
     +0x10..0x18 timestamps
```

Compressed blocks are zlib-wrapped deflate (`78 da`), which is why
`Archive::ReadFromFile`'s decompress step lands in `FUN_10065760` —
zlib `uncompress`, built `__fastcall`.

## The block size is 256 KiB, and it is set in code

`archive[0x40]` has exactly one writer:

```
1011ea94  c7 46 40 00 00 04 00   MOV dword ptr [ESI + 0x40], 0x40000
```

Every archive in the install that contains at least one entry spanning more
than one block reports a maximum block size of exactly `262144`.  (The
qualification is necessary: in an archive where no entry exceeds a single
block the largest block is just the largest entry, and reading that as a block
size is how a naive survey concludes that seventeen different block sizes
exist.  23 of the 135 archives are in that state.)

## What was checked exhaustively, and over what

**The scope matters, and the three headline properties are true of
`Resources/` — not of the whole install.**  `Resources/` is the tree the
runtime resource paths read from; `Audio/`, `Text/` and `Toolset/` are not
part of the texture or level path.

`Resources/`, 110 archives, **67,873 entries, 122,302 blocks**:

| property | result |
| --- | --- |
| entries stored uncompressed | **0** |
| archives with a gap or overlap in the data region | **0** |
| archives with a non-consecutive per-entry block run | **0** |
| block extents past end of file | **0** |
| block size, where revealed | **262144**, uniformly |
| `.tex` entries ≥ 2 MiB | **949** |

The whole install, 135 archives, 78,811 entries, 128,164 blocks, differs in
exactly two ways, and both are worth recording because they are the
counter-examples to claims that would otherwise read as universal:

- **6,880 entries are stored uncompressed** — every one an `.mp3`, all in the
  four `Audio/Dialog*.arc`.  Their file records carry `flags == 1`, i.e. the
  compressed bit clear.  So **the uncompressed branch of
  `Archive::ReadFromFile` is not dead code**; it is the voice-dialog path.  It
  is dead for textures and levels, which is the claim the streaming work
  actually needs, but the stronger phrasing is false.
- **`Toolset/Templates.arc` has 37 gaps** in its data region.  It is the only
  archive in the install that is not globally contiguous, and it is an editor
  asset that the game does not load.

Two consequences for the plan:

- A bounded compressed prefetch that over-reads past the requested block never
  leaves valid data *within `Resources/`*, because the region there is
  provably contiguous and every entry's blocks are consecutive record indices.
  It must still clamp against the file size at the last block, and it must not
  assume contiguity for an arbitrary `.arc` handed to it.
- Any change hooked to the compressed path must leave the uncompressed path
  alone rather than assume it is unreachable.

## `Resources/Levels.arc` is one entry

```
1 entries, 7646 blocks, 625.9 MiB on disk
largest entry  world/world01.map  2004303764 bytes uncompressed,
                                  625.8 MiB (656.2 MB) compressed, 7646 blocks
```

There is no loose `.map` anywhere in the install.  Every level of every act
comes out of that single entry.

Set against that: **the block cache is a single slot.**  `FUN_1011d240`'s cache
test is `CMP dword ptr [EBX],EAX` against one `cachedBlockIndex`.  So any read
into `world01.map` that crosses a 256 KiB boundary, or that revisits a block
the one-slot cache has since evicted, costs a complete 256 KiB inflate plus a
seek/read syscall pair under the per-archive lock.  A `Level::Load` that
alternates between two areas of that file re-inflates the same block
repeatedly.

It is also what bounds the prefetch design: "read the entry's whole compressed
extent in one `ReadFile`" is right for a 1 MiB texture and catastrophic here,
where the entry is 625.8 MiB.

## The largest textures

949 `.tex` entries in `Resources/` are ≥ 2 MiB uncompressed.  The largest:

| uncompressed | blocks | entry |
| --- | --- | --- |
| 21.68 MiB | 87 | `Menu.arc:main/tqragnaroklogo.tex` |
| 21.68 MiB | 87 | `XPack2/Menu.arc:main/tqragnaroklogo.tex` |
| 21.33 MiB | 86 | `XPack2/Scenery.arc:structure/setdress/sihailongwangbody_nrm.tex` |
| 21.33 MiB | 86 | `XPack2/Scenery.arc:atlantis/06garden/nature/trees/gardens_bigtree01normal.tex` |
| 21.33 MiB | 86 | `XPack2/Scenery.arc:atlantis/01gadir/nature/rocks/gadir_boulders01normal.tex` |

At one `SetFilePointerEx` + `ReadFile` pair and one full
`inflateInit_`/`inflate`/`inflateEnd` cycle per block, that is **86 syscall
pairs and 86 inflate cycles for one texture**, synchronously inside
`File::Lock`, on whichever thread touched it.

## Every terrain texture is a `TEX` container, not `DDS `

Decompressing the first block of 502 `.tex` entries drawn from
`TerrainTextures.arc`, `SceneryGreece.arc` and `XPack2/Scenery.arc` gives
`TEX\x01` for **every one of them**; not a single `DDS ` appeared.

`GraphicsTexture::Initialize` (`0x10194120`) has two branches.  The `"DDS "`
branch calls the render device once.  The `"TEX"` branch is a `while` loop
that calls it repeatedly, walking forward through the *same* buffer by a
per-sub-blob length prefix.  So the looping branch is the only one that
matters here, and any design that takes ownership of the source buffer at the
first `CreateTexture2D` is a use-after-free while the loader thread iterates.

## Provenance note

`Resources/Levels.arc` (mtime Aug 31) and `Resources/Shaders.arc` (Sep 1) are
both later than their siblings (Aug 29).  Both headers parse cleanly, all
7,646 of `Levels.arc`'s records are contiguous, and no block extent runs past
either file's end, so nothing here depends on their being pristine — but the
mtimes are recorded rather than passed over, because if something has been
written into them it changes what "the supported build" means for the archive
work.
