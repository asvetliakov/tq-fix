#include "arc_cache.h"

#include <string.h>
#include <wchar.h>

#include "hdr.h"
#include "probe.h"

namespace tq {
namespace arccache {
namespace {

const unsigned kMaxMegabytes = 256;
const unsigned kSlotsPerMegabyte = (1u << 20) / kSlotBytes;   // 4
// Every 1,024 requests up to the eighth report, then every 8,192. The buffer
// tq::hdr::log appends into is a fixed 64 KiB that never resets, so a fixed
// cadence that is right for a hundred-second measurement run would fill it in
// under an hour of play -- and once it is full, a mismatch late in a long
// session would be silently dropped, which is the one line that must never be
// lost. Backing off keeps a multi-hour `verify` session inside about a
// kilobyte while a measurement run still gets the same eight reports it did.
const unsigned kReportEvery = 1024;
const unsigned kReportBackoffAfter = 8;
const unsigned kReportEveryLate = 8192;

const uint8_t kUsed = 1;
const uint8_t kReferenced = 2;

unsigned g_megabytes;
bool g_verify;

CRITICAL_SECTION g_lock;
bool g_lockReady;
BYTE* g_slab;
Key* g_keys;
uint32_t* g_tags;
uint8_t* g_state;
BYTE* g_metadata;
unsigned g_slots;
unsigned g_hand;

// Set the first time a stored block disagrees with what the engine produced.
// The cache stops serving and stops storing, permanently, and says so. There
// is no configuration in which continuing would be the right thing to do.
bool g_poisoned;

// All under g_lock.
unsigned g_requests;
unsigned g_hits;
unsigned g_stores;
unsigned g_evictions;
unsigned g_verified;
unsigned g_mismatches;

uint32_t hash(const Key& key) {
    // FNV-1a over the key's bytes. It only has to spread well enough that the
    // tag scan below rejects almost everything without touching g_keys.
    uint32_t h = 2166136261u;
    const BYTE* bytes = (const BYTE*)&key;
    for (unsigned i = 0; i < sizeof(Key); ++i) {
        h ^= bytes[i];
        h *= 16777619u;
    }
    return h;
}

bool sameKey(const Key& a, const Key& b) {
    return a.archive == b.archive && a.handle == b.handle
        && a.offset == b.offset && a.compressed == b.compressed
        && a.uncompressed == b.uncompressed;
}

// A linear scan, and deliberately. The slab holds at most 1,024 slots, the tag
// array is therefore at most 4 KiB, and the routine that calls this ran 7,491
// times in a 100-second session -- so the scan costs a microsecond at its
// worst against an inflate that costs five hundred. A hash table here would
// buy nothing and would need tombstones to survive clock eviction.
int find(const Key& key, uint32_t tag) {
    for (unsigned i = 0; i < g_slots; ++i)
        if (g_tags[i] == tag && (g_state[i] & kUsed) && sameKey(g_keys[i], key))
            return (int)i;
    return -1;
}

unsigned evict(bool* reused) {
    // Clock. One full sweep clears every reference bit, so the second is
    // guaranteed to find a victim; the bound is belt and braces.
    *reused = false;
    for (unsigned steps = 0; steps < 2 * g_slots + 2; ++steps) {
        const unsigned slot = g_hand;
        g_hand = g_hand + 1 < g_slots ? g_hand + 1 : 0;
        if (!(g_state[slot] & kUsed)) return slot;
        if (g_state[slot] & kReferenced) {
            g_state[slot] = (uint8_t)(g_state[slot] & ~kReferenced);
            continue;
        }
        *reused = true;
        ++g_evictions;
        return slot;
    }
    const unsigned slot = g_hand;
    g_hand = g_hand + 1 < g_slots ? g_hand + 1 : 0;
    *reused = true;
    ++g_evictions;
    return slot;
}

}  // namespace

void readOptions(const wchar_t* iniPath) {
    g_megabytes = 0;
    g_verify = false;
    if (!iniPath || !iniPath[0]) return;

    wchar_t value[32] = {};
    GetPrivateProfileStringW(L"performance", L"archive_cache_mb", L"0", value,
                             32, iniPath);

    // Leading digits are the size; an optional `verify` suffix asks for the
    // measurement boot. Anything else at all is refused rather than guessed
    // at, because the failure mode of guessing is a cache that is silently on.
    const wchar_t* p = value;
    while (*p == L' ' || *p == L'\t') ++p;
    unsigned megabytes = 0;
    bool digits = false;
    while (*p >= L'0' && *p <= L'9') {
        if (megabytes > kMaxMegabytes) megabytes = kMaxMegabytes + 1;
        else megabytes = megabytes * 10 + (unsigned)(*p - L'0');
        digits = true;
        ++p;
    }
    if (!digits) return;
    bool verify = false;
    if (*p) {
        if (_wcsicmp(p, L"verify") != 0) {
            tq::hdr::log("Archive cache: archive_cache_mb=%ls not understood,"
                         " staying off\r\n", value);
            return;
        }
        verify = true;
    }
    if (megabytes > kMaxMegabytes) megabytes = kMaxMegabytes;
    g_megabytes = megabytes;
    g_verify = verify && megabytes != 0;
}

unsigned megabytes() { return g_megabytes; }
bool verifying() { return g_verify; }
bool running() { return g_slab != nullptr && !g_poisoned; }

bool start() {
    if (g_slab) return true;
    if (!g_megabytes) return false;

    const unsigned slots = g_megabytes * kSlotsPerMegabyte;
    const SIZE_T slabBytes = (SIZE_T)slots * kSlotBytes;
    const SIZE_T metadataBytes =
        slots * (sizeof(Key) + sizeof(uint32_t) + sizeof(uint8_t));

    BYTE* metadata = (BYTE*)VirtualAlloc(nullptr, metadataBytes,
                                         MEM_COMMIT | MEM_RESERVE,
                                         PAGE_READWRITE);
    if (!metadata) {
        tq::hdr::log("Archive cache: could not commit %u bytes of metadata,"
                     " staying off\r\n", (unsigned)metadataBytes);
        return false;
    }
    BYTE* slab = (BYTE*)VirtualAlloc(nullptr, slabBytes,
                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!slab) {
        VirtualFree(metadata, 0, MEM_RELEASE);
        tq::hdr::log("Archive cache: could not commit %u MiB in a 32-bit"
                     " address space, staying off\r\n", g_megabytes);
        return false;
    }

    if (!g_lockReady) {
        InitializeCriticalSection(&g_lock);
        g_lockReady = true;
    }
    g_metadata = metadata;
    g_keys = (Key*)metadata;
    g_tags = (uint32_t*)(metadata + slots * sizeof(Key));
    g_state = (uint8_t*)(metadata + slots * (sizeof(Key) + sizeof(uint32_t)));
    g_slab = slab;
    g_slots = slots;
    g_hand = 0;
    g_poisoned = false;
    g_requests = g_hits = g_stores = g_evictions = g_verified = g_mismatches = 0;

    tq::hdr::log("Archive cache: %u MiB, %u slots of %u KiB%s\r\n", g_megabytes,
                 slots, (unsigned)(kSlotBytes >> 10),
                 g_verify ? ", VERIFY -- inflating anyway and comparing" : "");
    return true;
}

void stop() {
    if (!g_slab) return;
    if (g_lockReady) EnterCriticalSection(&g_lock);
    BYTE* slab = g_slab;
    BYTE* metadata = g_metadata;
    g_slab = nullptr;
    g_metadata = nullptr;
    g_keys = nullptr;
    g_tags = nullptr;
    g_state = nullptr;
    g_slots = 0;
    if (g_lockReady) LeaveCriticalSection(&g_lock);
    VirtualFree(slab, 0, MEM_RELEASE);
    VirtualFree(metadata, 0, MEM_RELEASE);
}

bool lookup(const Key& key, void* destination) {
    if (!g_slab || g_verify || g_poisoned || !destination) return false;
    if (!key.uncompressed || key.uncompressed > kSlotBytes) return false;

    const uint32_t tag = hash(key);
    EnterCriticalSection(&g_lock);
    const int slot = g_slab ? find(key, tag) : -1;
    if (slot < 0) {
        LeaveCriticalSection(&g_lock);
        return false;
    }
    memcpy(destination, g_slab + (SIZE_T)slot * kSlotBytes, key.uncompressed);
    g_state[slot] = (uint8_t)(g_state[slot] | kReferenced);
    ++g_hits;
    LeaveCriticalSection(&g_lock);
    tq::probe::engineCount(tq::probe::CounterArcCacheHit);
    return true;
}

void store(const Key& key, const void* source) {
    if (!g_slab || g_poisoned || !source) return;
    if (!key.uncompressed || key.uncompressed > kSlotBytes) return;

    const uint32_t tag = hash(key);
    EnterCriticalSection(&g_lock);
    if (!g_slab || g_poisoned) {
        LeaveCriticalSection(&g_lock);
        return;
    }
    int slot = find(key, tag);
    bool verified = false;
    bool mismatched = false;
    if (slot >= 0) {
        if (g_verify) {
            // The measurement the mode exists for: this is a request that
            // would have been served from the slab, and these are the bytes
            // the engine produced for it instead.
            if (memcmp(g_slab + (SIZE_T)slot * kSlotBytes, source,
                       key.uncompressed) == 0) {
                ++g_verified;
                verified = true;
            } else {
                g_poisoned = true;
                ++g_mismatches;
                mismatched = true;
            }
        }
        // Not verifying, the key is already resident and the block is
        // immutable, so there is nothing to write: two threads inflated the
        // same block at once. Keep it warm and leave it alone.
        if (!mismatched)
            g_state[slot] = (uint8_t)(g_state[slot] | kReferenced);
        LeaveCriticalSection(&g_lock);
        if (verified) tq::probe::engineCount(tq::probe::CounterArcCacheVerify);
        if (mismatched) {
            tq::probe::engineCount(tq::probe::CounterArcCacheBad);
            tq::hdr::log("Archive cache: MISMATCH on archive %p handle %p"
                         " offset %u csize %u usize %u -- cache disabled for"
                         " the rest of this session\r\n", key.archive,
                         key.handle, (unsigned)key.offset,
                         (unsigned)key.compressed, (unsigned)key.uncompressed);
        }
        return;
    }

    bool reused = false;
    slot = (int)evict(&reused);
    memcpy(g_slab + (SIZE_T)slot * kSlotBytes, source, key.uncompressed);
    g_keys[slot] = key;
    g_tags[slot] = tag;
    g_state[slot] = (uint8_t)(kUsed | kReferenced);
    ++g_stores;
    LeaveCriticalSection(&g_lock);
    tq::probe::engineCount(tq::probe::CounterArcCacheStore);
    if (reused) tq::probe::engineCount(tq::probe::CounterArcCacheEvict);
}

void report() {
    if (!g_lockReady) return;
    EnterCriticalSection(&g_lock);
    ++g_requests;
    const bool early = g_requests <= kReportBackoffAfter * kReportEvery;
    const bool due = early ? g_requests % kReportEvery == 0
                           : g_requests % kReportEveryLate == 0;
    const unsigned requests = g_requests, hits = g_hits, stores = g_stores;
    const unsigned evictions = g_evictions, verified = g_verified;
    const unsigned mismatches = g_mismatches, slots = g_slots;
    const bool poisoned = g_poisoned;
    LeaveCriticalSection(&g_lock);
    if (!due) return;
    tq::hdr::log("Archive cache: %u requests, %u hits (%u%%), %u stored,"
                 " %u evicted, %u slots%s\r\n", requests, hits,
                 requests ? hits * 100 / requests : 0, stores, evictions, slots,
                 poisoned ? " -- DISABLED after a mismatch" : "");
    if (g_verify)
        tq::hdr::log("Archive cache: verify -- %u blocks compared byte for"
                     " byte, %u mismatches\r\n", verified, mismatches);
}

#ifdef TQ_SELFTEST
void configureForTest(unsigned mb, bool verify) {
    stop();
    g_megabytes = mb > kMaxMegabytes ? kMaxMegabytes : mb;
    g_verify = verify && g_megabytes != 0;
    g_poisoned = false;
}
unsigned slotsForTest() { return g_slots; }
unsigned mismatchesForTest() { return g_mismatches; }
#endif

}  // namespace arccache
}  // namespace tq
