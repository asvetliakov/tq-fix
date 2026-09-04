#pragma once

#include <windows.h>
#include <stdint.h>

namespace tq {
namespace arccache {

// A decompressed-block cache in front of the game's archive block routine.
//
// `Archive::ReadFromFile` serves a byte range out of an `.arc` entry by
// walking the 256 KiB blocks it spans, and its only cache is a single slot:
// `FUN_1011d240` tests one `cachedBlockIndex` per open `File`. Against
// `Resources/Levels.arc`, which is one 2 GB entry of 7,646 blocks holding
// every level of every act, that produces exactly the amplification two runs
// measured:
//
//     Eternal Embers  7,491 blocks, 1.88 GiB inflated to serve 1.03 GiB, 4,418 ms
//     Greece          4,560 blocks, 1.14 GiB inflated to serve 0.48 GiB, 2,131 ms
//
// A 256 KiB memcpy is on the order of 25 microseconds; the inflate it replaces
// measured 582 and 467 microseconds a block on those two runs.
//
// This is a fixed slab of 256 KiB slots with a clock victim. The measured
// default is `[performance] archive_cache_mb = 8`; `0` disables it.

// What a slot holds one of. Every field is read out of the block routine's own
// operands rather than out of a document -- src/engine_probe.cpp names the
// instruction each one comes from, and verify-sites.py compares those
// instructions to the installed Engine.dll.
//
// The key is deliberately over-specified. `{archive, handle, offset}` already
// identifies a block; carrying both sizes as well means a wrong hit would need
// a recycled archive pointer AND a recycled file handle AND the same byte
// offset AND the same compressed and decompressed lengths. `8verify` is what
// turns that from an argument into a measurement.
struct Key {
    const void* archive;      // Archive*, the routine's `this`
    void* handle;             // archive[0xc], the open .arc file
    uint32_t offset;          // descriptor[0], the block's offset in that file
    uint32_t compressed;      // descriptor[1]
    uint32_t uncompressed;    // descriptor[2], and the bytes a slot holds
};

// The engine's block size, written once in code at `1011ea94` as
// `MOV dword [ESI+0x40], 0x40000`. A slot is exactly one block, and a request
// whose descriptor claims more than this is refused rather than truncated.
const uint32_t kSlotBytes = 0x40000;

// Reads `[performance] archive_cache_mb`. `0` is off. A plain
// number is a size in MiB, clamped to 256 -- 1,024 slots, which is more than
// the 7,646 blocks of the largest entry would ever need resident at once.
// A `verify` suffix -- `8verify` -- is the measurement mode described below.
void readOptions(const wchar_t* iniPath);

unsigned megabytes();
inline bool configured() { return megabytes() != 0; }

// In verify mode nothing is ever served from the slab. Every request is read
// and inflated by the engine exactly as it would be without this file, and
// then compared against whatever the slab already holds for that key. So the
// run costs what an uncached run costs and answers the only question worth
// asking first: does a hit that *would* have been served ever differ from the
// bytes the engine actually produces.
bool verifying();

// Commits the slab. False when the cache is off or the allocation failed;
// either way the game is left exactly as it was.
bool start();
void stop();
bool running();

// Fills `destination` with `key.uncompressed` bytes on a hit. Always false
// while verifying.
//
// The lock is held across the lookup and the copy out of the slot -- a slot
// released before it is copied could be evicted and rewritten underneath the
// copy -- and across the insert. It is never held across the caller's
// `ReadFile` or its inflate, which is the whole point: a miss holds nothing.
bool lookup(const Key& key, void* destination);

// The block, as the engine just produced it. Verifying: compared against a
// slot that already holds this key, and any disagreement stops the cache and
// says so. Otherwise: installed, evicting by clock if the slab is full.
//
// "As the engine just produced it" is the whole standard here, and it is the
// right one. The block routine has no failure return: a short read or a bad
// inflate is logged through the engine's own reporter and the buffer is used
// anyway, and its one-slot cache records the block as resident either way. So
// storing whatever came back is behaving exactly as the engine behaves. What
// `verify` would catch is the case that would actually matter -- the same
// block inflating differently on two occasions.
void store(const Key& key, const void* source);

// One log line, on its own cadence. Titan Quest exits without unloading, so a
// summary written at teardown is a summary nobody ever reads; this is called
// once per request and prints every 1,024th.
void report();

#ifdef TQ_SELFTEST
void configureForTest(unsigned megabytes, bool verify);
unsigned slotsForTest();
unsigned mismatchesForTest();
#endif

}  // namespace arccache
}  // namespace tq
