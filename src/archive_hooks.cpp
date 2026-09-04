#include "engine_internal.h"

namespace tq { namespace engine { namespace detail {

ArchiveBlockFn g_archiveBlock;
Detour g_archiveBlockDetour;

// Walks the block routine's own address arithmetic -- the derivation spelled
// out above kArchiveBlockBytes -- to recover the five fields the cache keys on
// and the buffer the inflate would have written into.
//
// Every dereference is guarded, and the block size is checked against the
// archive rather than assumed, so a pointer that is not an Archive fails to
// describe rather than faulting. Refusing is free: the caller falls through to
// the engine's own read and inflate, which is what happens today.
bool describeBlock(void* self, unsigned entry, unsigned block,
                   void* blockBuffer, tq::arccache::Key& key, BYTE** dest) {
    const BYTE* archive = (const BYTE*)self;
    if (!tq::detour::readable(archive, kArchiveBlockSizeOffset + 4)) return false;
    if (*(const uint32_t*)(archive + kArchiveBlockSizeOffset)
        != tq::arccache::kSlotBytes)
        return false;

    // 0x400000 entries is sixty times the 67,873 the install actually has, and
    // it is what keeps entry * 0x44 inside 32 bits.
    if (entry >= 0x400000u || block >= 0x400000u) return false;

    const BYTE* entryTable =
        *(const BYTE* const*)(archive + kArchiveEntryTableOffset);
    const BYTE* record = entryTable + (SIZE_T)entry * kArchiveEntryStride;
    if (!tq::detour::readable(record, kArchiveEntryStride)) return false;

    const BYTE* descriptors =
        *(const BYTE* const*)(record + kArchiveEntryDescriptorsOffset);
    const BYTE* descriptor =
        descriptors + (SIZE_T)block * kArchiveDescriptorStride;
    if (!tq::detour::readable(descriptor, kArchiveDescriptorStride))
        return false;

    if (!tq::detour::readable(blockBuffer, 12)) return false;
    BYTE* destination = *(BYTE**)((BYTE*)blockBuffer + 8);

    key.archive = self;
    key.handle = *(void* const*)(archive + kArchiveHandleOffset);
    key.offset = ((const uint32_t*)descriptor)[0];
    key.compressed = ((const uint32_t*)descriptor)[1];
    key.uncompressed = ((const uint32_t*)descriptor)[2];
    if (!key.uncompressed || key.uncompressed > tq::arccache::kSlotBytes)
        return false;
    if (!tq::detour::readable(destination, key.uncompressed)) return false;
    *dest = destination;
    return true;
}

int __fastcall hookArchiveBlock(void* self, void* edx, unsigned entry,
                                unsigned block, void* blockBuffer) {
    if (!g_archiveBlock) return 0;

    // engine_arc_blocks counts what the engine asked for, hit or miss, so the
    // 1.8x and 2.3x amplification figures runs 10 and 17 measured stay the
    // same measurement in a cached run. engine_arc_inflate_us below then
    // covers only the blocks that were actually read and inflated.
    tq::probe::engineCount(tq::probe::CounterEngineArcBlocks);

    tq::arccache::Key key = {};
    BYTE* dest = nullptr;
    bool keyed = false;
    if (tq::arccache::running()) {
        keyed = describeBlock(self, entry, block, blockBuffer, key, &dest);
        // A refusal is safe -- the engine's own read and inflate run, exactly
        // as they do at archive_cache_mb=0 -- but it is not expected, so it
        // gets a column of its own rather than being silent.
        if (!keyed) tq::probe::engineCount(tq::probe::CounterArcCacheSkip);
    }
    if (keyed) {
        const int64_t looked = tq::probe::now();
        if (tq::arccache::lookup(key, dest)) {
            // Everything the original does that anyone downstream can observe:
            // FUN_1011d240's one-slot cache is told which block its scratch
            // buffer now holds, and AL comes back 1. What is skipped is the
            // seek, the read, the archive's own critical section and the
            // inflate -- see kArchiveBlockTailBytes.
            *(unsigned*)blockBuffer = block;
            tq::probe::engineCount(tq::probe::CounterArcCacheHitUs,
                                   tq::probe::microsecondsSince(looked));
            tq::arccache::report();
            return 1;
        }
    }

    const int64_t started = tq::probe::now();
    const int result = g_archiveBlock(self, edx, entry, block, blockBuffer);
    tq::probe::engineCount(tq::probe::CounterEngineArcInflateUs,
                           tq::probe::microsecondsSince(started));
    // The routine returns its bool in AL and leaves the rest of EAX holding
    // the inflated length, so this reads the byte the caller would.
    if (keyed && (result & 0xff)) tq::arccache::store(key, dest);
    if (tq::arccache::running()) tq::arccache::report();
    return result;
}

// The three windows a cache hit's correctness rests on, over and above the two
// that establish the hook's identity: the seek and the read, which name the
// handle and the descriptor's three fields in the operands of the syscalls
// that consume them, and the epilogue, which is the contract a hit reproduces.
// The block size has exactly one writer in the image, and that is checked too.
//
// These are required only when the cache is on. The instrument on its own
// needs the function to *be* the block routine; the cache additionally needs
// every offset it reads to be the offset the routine reads.
bool archiveStructureVerified(HMODULE engine) {
    struct Window {
        const char* what;
        DWORD rva;
        const BYTE* bytes;
        SIZE_T size;
        const Relocation* relocations;
        unsigned relocationCount;
    };
    const Window windows[] = {
        {"seek", kArchiveSeekWindowRva, kArchiveSeekWindowBytes,
         sizeof(kArchiveSeekWindowBytes), kArchiveSeekWindowRelocs, 1},
        {"read", kArchiveReadWindowRva, kArchiveReadWindowBytes,
         sizeof(kArchiveReadWindowBytes), kArchiveReadWindowRelocs, 1},
        {"epilogue", kArchiveBlockTailRva, kArchiveBlockTailBytes,
         sizeof(kArchiveBlockTailBytes), nullptr, 0},
        {"block size", kArchiveBlockSizeRva, kArchiveBlockSizeBytes,
         sizeof(kArchiveBlockSizeBytes), nullptr, 0},
    };
    for (unsigned i = 0; i < sizeof(windows) / sizeof(*windows); ++i) {
        const Window& w = windows[i];
        if (tq::detour::matches(engine, (BYTE*)engine + w.rva,
                                signature(w.bytes, w.size, w.relocations,
                                          w.relocationCount)))
            continue;
        tq::hdr::log("Archive cache: the %s window at %p does not match --"
                     " refusing to cache\r\n", w.what,
                     (void*)((BYTE*)engine + w.rva));
        return false;
    }
    return true;
}

bool installArchive(HMODULE engine, bool trace, bool cache) {
    if (trace) {
        void* target = resolve(engine, kReadFromFileName, kReadFromFileRva);
        if (target)
            tq::detour::attach(
                g_readFromFileDetour, engine, target,
                signature(kReadFromFileBytes, sizeof(kReadFromFileBytes)), 6,
                (const void*)&hookReadFromFile, (void**)&g_readFromFile);
        note("Archive::ReadFromFile", g_readFromFile != nullptr);
    }

    // The block routine is not exported, so identity rests entirely on the
    // bytes: its own forty-seven byte prologue and address arithmetic, and the
    // call to zlib's uncompress that is the only thing distinguishing it from
    // three functions with the same opening.
    BYTE* block = (BYTE*)engine + kArchiveBlockRva;
    const bool anchored = tq::detour::matches(
        engine, (BYTE*)engine + kArchiveInflateWindowRva,
        signature(kArchiveInflateWindowBytes,
                  sizeof(kArchiveInflateWindowBytes)));
    if (anchored)
        tq::detour::attach(
            g_archiveBlockDetour, engine, block,
            signature(kArchiveBlockBytes, sizeof(kArchiveBlockBytes)), 6,
            (const void*)&hookArchiveBlock, (void**)&g_archiveBlock);
    note("archive block inflate", g_archiveBlock != nullptr);

    // Nothing is committed until the site is proven. A refusal anywhere here
    // leaves the game byte-identical to archive_cache_mb=0, which is the
    // default, and says which check refused.
    if (cache) {
        if (!g_archiveBlock)
            tq::hdr::log("Archive cache: the block routine is not hooked --"
                         " nothing to cache in front of\r\n");
        else if (archiveStructureVerified(engine))
            tq::arccache::start();
    }
    return true;
}
} } }
