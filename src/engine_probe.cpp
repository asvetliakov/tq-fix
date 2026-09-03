#include "engine_probe.h"

#include "detour.h"
#include "hdr.h"
#include "probe.h"

#include <stdint.h>
#include <string.h>

namespace tq {
namespace engineprobe {
namespace {

using tq::detour::CallPatch;
using tq::detour::Detour;
using tq::detour::Relocation;
using tq::detour::Signature;

// ---------------------------------------------------------------------------
// What the audit recorded, and what this file re-reads before it writes.
//
// research/streaming/findings.md §7 lists these, but that file is a record and
// not a licence: every byte below was read back out of the pinned Engine.dll
// (SHA-256 0aedbb18...f694f6) while this was written. The addresses are RVAs
// against the preferred base 0x10000000, and each exported target is resolved
// by decorated name first and then asserted against its RVA -- so the name is
// the lookup and the RVA is the identity check, which is the way round that
// survives a rebased image.

const DWORD kEngineImageSize = 0x0044b000;

// The thread the engine recorded as its own. `CMP EAX,[0x1041a5dc]` at
// 0x1014476b is what the engine itself compares against, so reading it costs
// one load and needs no hook.
const DWORD kMainThreadIdRva = 0x41a5dc;

// --- Region::LoadLevel. Returns immediately when the level is resident
// (`MOV EAX,[EBX+0x50]` / `TEST` / the AL=1 epilogue), so the duration this
// records *is* the cost of a load the renderer forced.
const DWORD kLoadLevelRva = 0x20bec0;
const char kLoadLevelName[] = "?LoadLevel@Region@GAME@@QAE_N_N@Z";
const BYTE kLoadLevelBytes[] = {
    0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8,        // push ebp; mov ebp,esp; and esp,-8
    0x83, 0xec, 0x0c,                          // sub esp,0xc
    0x53,                                      // push ebx
    0x8b, 0xd9,                                // mov ebx,ecx
    0x56,                                      // push esi
    0x8b, 0x43, 0x50,                          // mov eax,[ebx+0x50]
    0x57,                                      // push edi
    0x85, 0xc0,                                // test eax,eax
    0x74, 0x4c,                                // jz  the load path
    0x80, 0x7d, 0x08                           // cmp byte [ebp+8],...
};

// --- ResourceLoader::LoadResource. The duration includes its wait on the
// resource's own critical section, which is the stall worth naming.
const DWORD kLoadResourceRva = 0x213ed0;
const char kLoadResourceName[] =
    "?LoadResource@ResourceLoader@GAME@@QAEXPAVResource@2@@Z";
const BYTE kLoadResourceBytes[] = {
    0x6a, 0xff,                                // push -1
    0x68, 0, 0, 0, 0,                          // push <SEH handler>
    0x64, 0xa1, 0x00, 0x00, 0x00, 0x00,        // mov eax,fs:[0]
    0x50,                                      // push eax
    0x83, 0xec, 0x08,                          // sub esp,8
    0x53, 0x55, 0x56, 0x57,                    // push ebx,ebp,esi,edi
    0xa1                                       // mov eax,ds:[__security_cookie]
};
const Relocation kLoadResourceRelocs[] = {{3, 0x2a2db8}};

// --- Region::UnloadLevel.
const DWORD kUnloadLevelRva = 0x20e040;
const char kUnloadLevelName[] = "?UnloadLevel@Region@GAME@@QAEX_N_N@Z";
const BYTE kUnloadLevelBytes[] = {
    0x6a, 0xff,
    0x68, 0, 0, 0, 0,
    0x64, 0xa1, 0x00, 0x00, 0x00, 0x00,
    0x50,
    0x51, 0x56, 0x57,                          // push ecx,esi,edi
    0xa1, 0, 0, 0, 0                           // mov eax,ds:[__security_cookie]
};
const Relocation kUnloadLevelRelocs[] = {{3, 0x2a29a3}, {18, 0x36f000}};

// --- ResourceLoader::EnqueueResource. Only the same seven-byte SEH prologue
// as the two above, so the handler it pushes is what tells them apart.
const DWORD kEnqueueRva = 0x2145c0;
const char kEnqueueName[] =
    "?EnqueueResource@ResourceLoader@GAME@@QAEXPBVResource@2@"
    "W4ResourcePriority@2@_N2@Z";
const BYTE kEnqueueBytes[] = {
    0x6a, 0xff,
    0x68, 0, 0, 0, 0,
    0x64, 0xa1, 0x00, 0x00, 0x00, 0x00,
    0x50,
    0x83, 0xec, 0x14,                          // sub esp,0x14
    0x53, 0x55, 0x56, 0x57,
    0xa1
};
const Relocation kEnqueueRelocs[] = {{3, 0x2a2e86}};

// --- Archive::ReadFromFile. `?ReadFromFile@Archive@GAME@@QBE_NHPAEIIPAU
// BlockBuffer@12@@Z` and the disassembly agree on the argument layout, which
// settles the one thing the plan flagged as inferred: `size` is the fourth
// stack argument, read at [ebp+0x14].
const DWORD kReadFromFileRva = 0x11d320;
const char kReadFromFileName[] =
    "?ReadFromFile@Archive@GAME@@QBE_NHPAEIIPAUBlockBuffer@12@@Z";
const BYTE kReadFromFileBytes[] = {
    0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8,
    0x83, 0xec, 0x0c,
    0x83, 0x7d, 0x0c, 0x00,                    // cmp dword [ebp+0xc],0   dest
    0x53, 0x56,
    0x8b, 0xc1,                                // mov eax,ecx
    0x57,
    0x89, 0x44, 0x24, 0x10,
    0x0f, 0x84                                 // jz the bail-out
};

// --- One block read and inflated. Not exported, so identity rests entirely on
// the bytes: the prologue is the same six bytes as three other targets, and
// what makes this one itself is the call to zlib's uncompress 0xf6 in.
const DWORD kArchiveBlockRva = 0x11d0e0;
const BYTE kArchiveBlockBytes[] = {
    0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8,
    0x83, 0xec, 0x0c,
    0x8b, 0x55, 0x08,                          // mov edx,[ebp+8]   entry index
    0x8b, 0x41, 0x2c,                          // mov eax,[ecx+0x2c]
    0xc1, 0xe2, 0x04,                          // shl edx,4
    0x03, 0x55, 0x08,                          // add edx,[ebp+8]   -> index*0x11
    0x53,
    0x8b, 0x5d                                 // mov ebx,[ebp+0xc] block index
};
// CALL 0x10065760 -- zlib uncompress, built __fastcall. The displacement is
// relative and unrelocated, so comparing the five bytes at this exact address
// is the same statement as "this call goes to the inflate".
const DWORD kArchiveInflateCallRva = 0x11d1d6;
const BYTE kArchiveInflateCallBytes[] = {0xe8, 0x85, 0x85, 0xf4, 0xff};

// --- Region::WaitForLoadingToFinish. Seven bytes: `cmp byte [ecx+0x78],1`,
// `jz -6`, `ret`. The stolen bytes would contain that relative jump, so a
// trampoline is impossible and the replacement implements the spin itself.
const DWORD kWaitForLoadingRva = 0x20bde0;
const char kWaitForLoadingName[] = "?WaitForLoadingToFinish@Region@GAME@@QAEXXZ";
const BYTE kWaitForLoadingBytes[] = {0x80, 0x79, 0x78, 0x01, 0x74, 0xfa, 0xc3};

// --- The region lock, on the render path. Three sites with an identical
// twenty-eight byte shape:
//     ff 7? 08                push [region+8]
//     ff 15 <EnterCriticalSection>
//     8b 7? 50                mov esi,[region+0x50]
//     c7 4? 6c 00 00 00 00    mov dword [region+0x6c],0
//     ff 7? 08                push [region+8]
//     ff 15 <LeaveCriticalSection>
// The critical section covers two loads, so a hit here is the render thread
// waiting on whoever else holds the region -- which is the audit's §1b, and
// has never been measured.
const DWORD kEnterCriticalSectionSlotRva = 0x2ac17c;
const DWORD kLeaveCriticalSectionSlotRva = 0x2ac178;
const BYTE kRegionLockEbxBytes[] = {
    0xff, 0x73, 0x08,
    0xff, 0x15, 0, 0, 0, 0,
    0x8b, 0x73, 0x50,
    0xc7, 0x43, 0x6c, 0x00, 0x00, 0x00, 0x00,
    0xff, 0x73, 0x08,
    0xff, 0x15, 0, 0, 0, 0
};
const BYTE kRegionLockEdiBytes[] = {
    0xff, 0x77, 0x08,
    0xff, 0x15, 0, 0, 0, 0,
    0x8b, 0x77, 0x50,
    0xc7, 0x47, 0x6c, 0x00, 0x00, 0x00, 0x00,
    0xff, 0x77, 0x08,
    0xff, 0x15, 0, 0, 0, 0
};
const Relocation kRegionLockRelocs[] = {{5, kEnterCriticalSectionSlotRva},
                                        {24, kLeaveCriticalSectionSlotRva}};
static_assert(sizeof(kRegionLockEbxBytes) == sizeof(kRegionLockEdiBytes),
              "both region-lock windows are the same twenty-eight byte shape");
const unsigned kRegionLockCallOffset = 3;

struct LockSite {
    const char* owner;          // decorated name of the containing export
    DWORD ownerRva;             // asserted against what the name resolves to
    DWORD windowRva;
    const BYTE* bytes;
};
const LockSite kLockSites[] = {
    {"?GetEntitiesInFrustum@Region@GAME@@QBEXAAV?$vector@PAVEntity@GAME@@"
     "V?$allocator@PAVEntity@GAME@@@std@@@std@@ABVFrustum@2@_NPBV12@"
     "W4EntityListType@2@22@Z",
     0x209840, 0x209896, kRegionLockEbxBytes},
    {"?AddElementsInBox@GraphicsDeferredRendererX@GAME@@UAEXPAVRegion@2@"
     "ABVOBBox@2@ABVCoords@2@@Z",
     0x1677e0, 0x167869, kRegionLockEdiBytes},
    {"?AddElementsInBox@GraphicsForwardRenderer@GAME@@UAEXPAVRegion@2@"
     "ABVOBBox@2@ABVCoords@2@@Z",
     0x17d850, 0x17d8d9, kRegionLockEdiBytes},
};
const unsigned kLockSiteCount = sizeof(kLockSites) / sizeof(kLockSites[0]);

// --- Engine::Update. Two of the call-site groups live inside it, so its own
// RVA is asserted once and their windows are offsets into the same function.
const DWORD kEngineUpdateRva = 0x1443a0;
const char kEngineUpdateName[] = "?Update@Engine@GAME@@QAEXPBVWorldFrustum@2@_N@Z";
// It is also detoured whole, which is a different question from the call sites
// inside it. Run 10 named a fifth to a third of the hitch time and left the
// rest dark: 950 ms of its worst frame, and every millisecond of a class of
// 200-240 ms frames that showed zero in every other engine column. Bracketing
// the engine's own two halves says which half the dark time is in -- or that
// it is in neither, which would put it outside Engine.dll and rule out all of
// Stages 4 and 5 at once. Both run once a frame.
const BYTE kEngineUpdateBytes[] = {
    0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8,
    0x6a, 0xff,
    0x68, 0, 0, 0, 0,                          // push <SEH handler>
    0x64, 0xa1, 0x00, 0x00, 0x00, 0x00,
    0x50,
    0x81, 0xec, 0x70, 0x08                     // sub esp,0x870
};
const Relocation kEngineUpdateRelocs[] = {{9, 0x296a30}};

// --- Engine::Render. Aligns its frame to 64 rather than 8, so unlike almost
// everything else here its opening six bytes are already distinctive; the
// relocated dword is the QueryPerformanceCounter import it reads first.
const DWORD kEngineRenderRva = 0x143fe0;
const char kEngineRenderName[] = "?Render@Engine@GAME@@QAEXXZ";
const BYTE kEngineRenderBytes[] = {
    0x55, 0x8b, 0xec, 0x83, 0xe4, 0xc0,        // push ebp; mov ebp,esp; and esp,-64
    0x83, 0xec, 0x78,                          // sub esp,0x78
    0x56, 0x57,                                // push esi,edi
    0x8b, 0x3d, 0, 0, 0, 0,                    // mov edi,ds:[QueryPerformanceCounter]
    0x8d, 0x44, 0x24, 0x40,                    // lea eax,[esp+0x40]
    0x50,                                      // push eax
    0x8b, 0xf1                                 // mov esi,ecx
};
const Relocation kEngineRenderRelocs[] = {{13, 0x2ac1b8}};

// The loader fence. `push -1; push [g_fence]; call WaitForSingleObject`.
// §6.2 argues the event is normally already signalled, so a non-zero total
// here is exactly the evidence that argument is wrong.
const DWORD kFenceWindowRva = 0x14479a;
const BYTE kFenceWindowBytes[] = {
    0x6a, 0xff,                                // push -1  (INFINITE)
    0xff, 0x35, 0, 0, 0, 0,                    // push dword [fence]
    0xff, 0x15, 0, 0, 0, 0,                    // call [WaitForSingleObject]
    0xe8, 0x63, 0xab, 0xfd, 0xff               // call the fence close
};
const DWORD kWaitForSingleObjectSlotRva = 0x2ac188;
const Relocation kFenceWindowRelocs[] = {{4, 0x370258},
                                         {10, kWaitForSingleObjectSlotRva}};
const unsigned kFenceCallOffset = 8;

// The seven UnloadUnreferencedResources sweeps, in two contiguous runs.
const DWORD kSweepTargetRva = 0x120250;
const char kSweepTargetName[] =
    "?UnloadUnreferencedResources@BaseResourceManager@GAME@@QAEXXZ";
const BYTE kSweepWindowABytes[] = {
    0x8b, 0x4e, 0x28, 0xe8, 0xc7, 0xbd, 0xfd, 0xff,
    0x8b, 0x4e, 0x2c, 0xe8, 0xbf, 0xbd, 0xfd, 0xff,
    0x8b, 0x4e, 0x30, 0xe8, 0xb7, 0xbd, 0xfd, 0xff,
    0x8b, 0x4e, 0x24, 0xe8, 0xaf, 0xbd, 0xfd, 0xff
};
const BYTE kSweepWindowBBytes[] = {
    0x8b, 0x4e, 0x1c, 0xe8, 0xa7, 0xbd, 0xfd, 0xff,
    0x8b, 0x8b, 0x58, 0x01, 0x00, 0x00, 0xe8, 0x9c, 0xbd, 0xfd, 0xff,
    0x8b, 0x4b, 0x38, 0x8d, 0x89, 0x18, 0x05, 0x00, 0x00,
    0xe8, 0x8e, 0xbd, 0xfd, 0xff
};
const DWORD kSweepWindowARva = 0x144481;
const DWORD kSweepWindowBRva = 0x1444a1;
const unsigned kSweepCount = 7;

// Seven calls in two windows, so the window cannot be the thing each patch
// verifies: retargeting the first call rewrites four of the bytes the next
// one would compare. The window is checked once, whole, and then each call is
// patched against its own five bytes -- which is not a weaker check, because
// patchCall also requires the displacement to resolve to the sweep itself.
struct SweepSite {
    DWORD windowRva;
    const BYTE* window;
    unsigned windowLength;
    unsigned callOffset;
};
const SweepSite kSweepSites[kSweepCount] = {
    {kSweepWindowARva, kSweepWindowABytes, sizeof(kSweepWindowABytes), 3},
    {kSweepWindowARva, kSweepWindowABytes, sizeof(kSweepWindowABytes), 11},
    {kSweepWindowARva, kSweepWindowABytes, sizeof(kSweepWindowABytes), 19},
    {kSweepWindowARva, kSweepWindowABytes, sizeof(kSweepWindowABytes), 27},
    {kSweepWindowBRva, kSweepWindowBBytes, sizeof(kSweepWindowBBytes), 3},
    {kSweepWindowBRva, kSweepWindowBBytes, sizeof(kSweepWindowBBytes), 14},
    {kSweepWindowBRva, kSweepWindowBBytes, sizeof(kSweepWindowBBytes), 28},
};

// --- GameEngine::Update, in Game.dll rather than Engine.dll. It is here
// because run 11 closed the frame's accounting and found the answer was not
// in Engine.dll at all: 58% of the session is Engine::Render and 10% is
// Engine::Update, but 38% of the hitch time -- and eighteen of the thirty-two
// frames over 100 ms -- fall outside both, on frames that draw 400 to 1,600
// times with the mod completely idle. Against a normal frame's 0.21 ms
// outside, those spend 100 to 225 ms there. This is the one bracket large
// enough to hold it.
//
// `?Update@GameEngine@GAME@@QAEXH@Z` is __thiscall void(int): one stack
// argument, callee popped, confirmed by the `ret 4` that ends the body at
// +0x5ee.
const DWORD kGameImageSize = 0x0059a000;
const DWORD kGameUpdateRva = 0x19a230;
const char kGameUpdateName[] = "?Update@GameEngine@GAME@@QAEXH@Z";
const BYTE kGameUpdateBytes[] = {
    0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8,
    0x6a, 0xff,
    0x68, 0, 0, 0, 0,                          // push <SEH handler>
    0x64, 0xa1, 0x00, 0x00, 0x00, 0x00,
    0x50,
    0x81, 0xec, 0x70, 0x04                     // sub esp,0x470
};
const Relocation kGameUpdateRelocs[] = {{9, 0x2f1112}};

// --- TQ.exe's main loop. Not a patch at all: three entries of the
// executable's own import address table, so the redirect is scoped to the one
// module that has the stall in it and every other caller in the process --
// including the mod's own -- keeps the real function.
//
// The loop's entire vocabulary for blocking is these three. It imports no
// PeekMessage, so its pump is the *blocking* GetMessageA; and it imports
// GameEngine::NeedsSleep beside Sleep, so it is a frame limiter. Either can
// hand back an arbitrary amount of time on a host that is momentarily busy,
// which is the shape run 12 measured.
const DWORD kExecutableImageSize = 0x0036a000;

// ---------------------------------------------------------------------------
// Configuration. Bit 0 means "everything"; the rest select groups, so a run
// that misbehaves can be narrowed from the INI rather than from a rebuild.
const unsigned kGroupAll = 0x01;
const unsigned kGroupLoads = 0x02;
const unsigned kGroupArchive = 0x04;
const unsigned kGroupFence = 0x08;
const unsigned kGroupLock = 0x10;
const unsigned kGroupSweeps = 0x20;
const unsigned kGroupWait = 0x40;
const unsigned kGroupFrame = 0x80;
const unsigned kGroupGame = 0x100;
const unsigned kGroupLoop = 0x200;

unsigned g_traceMask = 1;

LONG g_installed;
unsigned g_installedHooks;

const volatile DWORD* g_mainThreadId;

bool onMainThread() {
    return g_mainThreadId && *g_mainThreadId == GetCurrentThreadId();
}

// ---------------------------------------------------------------------------
// Hooks. Titan Quest is __thiscall throughout; __fastcall with a second
// argument nobody reads matches it -- this in ecx, the rest on the stack,
// callee cleaned -- which is what src/grass.cpp already relies on. The bool
// arguments are declared int so the caller's whole pushed dword goes through
// unchanged.

typedef int (__fastcall* LoadLevelFn)(void* self, void* edx, int background);
typedef void (__fastcall* LoadResourceFn)(void* self, void* edx, void* resource);
typedef void (__fastcall* UnloadLevelFn)(void* self, void* edx, int a, int b);
typedef void (__fastcall* EnqueueFn)(void* self, void* edx, const void* resource,
                                     int priority, int a, int b);
typedef int (__fastcall* ReadFromFileFn)(void* self, void* edx, int entry,
                                         BYTE* dest, unsigned offset,
                                         unsigned size, void* blockBuffer);
typedef int (__fastcall* ArchiveBlockFn)(void* self, void* edx, unsigned entry,
                                         unsigned block, void* blockBuffer);
typedef void (__fastcall* SweepFn)(void* self, void* edx);
typedef void (__fastcall* EngineUpdateFn)(void* self, void* edx,
                                          const void* frustum, int flag);
typedef void (__fastcall* EngineRenderFn)(void* self, void* edx);
typedef void (__fastcall* GameUpdateFn)(void* self, void* edx, int delta);

LoadLevelFn g_loadLevel;
LoadResourceFn g_loadResource;
UnloadLevelFn g_unloadLevel;
EnqueueFn g_enqueue;
ReadFromFileFn g_readFromFile;
ArchiveBlockFn g_archiveBlock;
SweepFn g_sweep;
EngineUpdateFn g_engineUpdate;
EngineRenderFn g_engineRender;
GameUpdateFn g_gameUpdate;

Detour g_loadLevelDetour;
Detour g_loadResourceDetour;
Detour g_unloadLevelDetour;
Detour g_enqueueDetour;
Detour g_readFromFileDetour;
Detour g_archiveBlockDetour;
Detour g_waitForLoadingDetour;
Detour g_engineUpdateDetour;
Detour g_engineRenderDetour;
Detour g_gameUpdateDetour;
CallPatch g_lockPatches[kLockSiteCount];
CallPatch g_fencePatch;
CallPatch g_sweepPatches[kSweepCount];

int __fastcall hookLoadLevel(void* self, void* edx, int background) {
    if (!g_loadLevel) return 0;
    const int64_t started = tq::probe::now();
    const int result = g_loadLevel(self, edx, background);
    const uint32_t elapsed = tq::probe::microsecondsSince(started);
    tq::probe::engineCount(tq::probe::CounterEngineLevelLoad);
    tq::probe::engineCount(tq::probe::CounterEngineLevelLoadUs, elapsed);
    if (onMainThread()) {
        tq::probe::engineCount(tq::probe::CounterEngineLevelLoadMain);
        tq::probe::engineCount(tq::probe::CounterEngineLevelLoadMainUs, elapsed);
    }
    return result;
}

void __fastcall hookLoadResource(void* self, void* edx, void* resource) {
    if (!g_loadResource) return;
    const int64_t started = tq::probe::now();
    g_loadResource(self, edx, resource);
    const uint32_t elapsed = tq::probe::microsecondsSince(started);
    tq::probe::engineCount(tq::probe::CounterEngineResLoad);
    tq::probe::engineCount(tq::probe::CounterEngineResLoadUs, elapsed);
    if (onMainThread()) {
        tq::probe::engineCount(tq::probe::CounterEngineResLoadMain);
        tq::probe::engineCount(tq::probe::CounterEngineResLoadMainUs, elapsed);
    }
}

void __fastcall hookUnloadLevel(void* self, void* edx, int a, int b) {
    if (!g_unloadLevel) return;
    const int64_t started = tq::probe::now();
    g_unloadLevel(self, edx, a, b);
    tq::probe::engineCount(tq::probe::CounterEngineRegionUnload);
    tq::probe::engineCount(tq::probe::CounterEngineRegionUnloadUs,
                           tq::probe::microsecondsSince(started));
}

void __fastcall hookEnqueueResource(void* self, void* edx, const void* resource,
                                    int priority, int a, int b) {
    if (!g_enqueue) return;
    tq::probe::engineCount(tq::probe::CounterEngineResEnqueued);
    g_enqueue(self, edx, resource, priority, a, b);
}

int __fastcall hookReadFromFile(void* self, void* edx, int entry, BYTE* dest,
                                unsigned offset, unsigned size,
                                void* blockBuffer) {
    if (!g_readFromFile) return 0;
    // Counted, never timed. Rounded up so a short read still registers as one
    // KiB rather than disappearing.
    tq::probe::engineCount(tq::probe::CounterEngineArcRead);
    tq::probe::engineCount(tq::probe::CounterEngineArcKib, (size + 1023u) >> 10);
    return g_readFromFile(self, edx, entry, dest, offset, size, blockBuffer);
}

int __fastcall hookArchiveBlock(void* self, void* edx, unsigned entry,
                                unsigned block, void* blockBuffer) {
    if (!g_archiveBlock) return 0;
    const int64_t started = tq::probe::now();
    const int result = g_archiveBlock(self, edx, entry, block, blockBuffer);
    tq::probe::engineCount(tq::probe::CounterEngineArcBlocks);
    tq::probe::engineCount(tq::probe::CounterEngineArcInflateUs,
                           tq::probe::microsecondsSince(started));
    return result;
}

// Semantically what the seven replaced bytes did: spin while the region's
// loading flag is set. The null check is the one departure, and it turns a
// fault inside Engine.dll into a return -- a function a full .text scan finds
// no caller for is not worth crashing over.
void __fastcall hookWaitForLoadingToFinish(void* self, void* edx) {
    (void)edx;
    if (!self) return;
    tq::probe::engineCount(tq::probe::CounterEngineWaitLoading);
    const int64_t started = tq::probe::now();
    const volatile BYTE* loading = (const volatile BYTE*)self + 0x78;
    while (*loading == 1) {
        YieldProcessor();
        SwitchToThread();
    }
    tq::probe::engineCount(tq::probe::CounterEngineWaitLoadingUs,
                           tq::probe::microsecondsSince(started));
}

// Retargeted call sites reach these through the ordinary call frame, so they
// can be plain __stdcall functions: no hand-emitted thunk, and no chance of
// getting a callee-pop count wrong on a path that runs every frame.

// The uncontended case is one interlocked operation and a branch, and records
// nothing at all. That is what makes measuring a lock on a render-path call
// affordable; taking a timestamp per acquisition would not be.
void __stdcall hookEnterCriticalSection(LPCRITICAL_SECTION section) {
    if (TryEnterCriticalSection(section)) return;
    const int64_t started = tq::probe::now();
    EnterCriticalSection(section);
    tq::probe::engineCount(tq::probe::CounterEngineRegionLockHits);
    tq::probe::engineCount(tq::probe::CounterEngineRegionLockUs,
                           tq::probe::microsecondsSince(started));
}

DWORD __stdcall hookWaitForSingleObject(HANDLE handle, DWORD milliseconds) {
    const int64_t started = tq::probe::now();
    const DWORD result = WaitForSingleObject(handle, milliseconds);
    tq::probe::engineCount(tq::probe::CounterEngineFenceWait);
    tq::probe::engineCount(tq::probe::CounterEngineFenceWaitUs,
                           tq::probe::microsecondsSince(started));
    return result;
}

void __fastcall hookSweep(void* self, void* edx) {
    if (!g_sweep) return;
    const int64_t started = tq::probe::now();
    g_sweep(self, edx);
    tq::probe::engineCount(tq::probe::CounterEngineSweeps);
    tq::probe::engineCount(tq::probe::CounterEngineSweepUs,
                           tq::probe::microsecondsSince(started));
}

// The frame's two halves. Everything else in this file nests inside one of
// these, so their totals are the denominators the rest are read against.
void __fastcall hookEngineUpdate(void* self, void* edx, const void* frustum,
                                 int flag) {
    if (!g_engineUpdate) return;
    const int64_t started = tq::probe::now();
    g_engineUpdate(self, edx, frustum, flag);
    tq::probe::engineCount(tq::probe::CounterEngineUpdate);
    tq::probe::engineCount(tq::probe::CounterEngineUpdateUs,
                           tq::probe::microsecondsSince(started));
}

void __fastcall hookEngineRender(void* self, void* edx) {
    if (!g_engineRender) return;
    const int64_t started = tq::probe::now();
    g_engineRender(self, edx);
    tq::probe::engineCount(tq::probe::CounterEngineRender);
    tq::probe::engineCount(tq::probe::CounterEngineRenderUs,
                           tq::probe::microsecondsSince(started));
}

typedef void (WINAPI* SleepFn)(DWORD);
typedef BOOL (WINAPI* GetMessageFn)(LPMSG, HWND, UINT, UINT);
typedef DWORD (WINAPI* WaitFn)(HANDLE, DWORD);
SleepFn g_loopSleep;
GetMessageFn g_loopGetMessage;
WaitFn g_loopWait;
CallPatch g_loopSleepPatch;
CallPatch g_loopMessagePatch;
CallPatch g_loopWaitPatch;

void __stdcall hookLoopSleep(DWORD milliseconds) {
    if (!g_loopSleep) return;
    const int64_t started = tq::probe::now();
    g_loopSleep(milliseconds);
    tq::probe::engineCount(tq::probe::CounterLoopSleep);
    // Saturating rather than wrapping, so an INFINITE or a very long request
    // reads as "longer than this can express" instead of as a short one.
    const uint32_t requested = milliseconds > 4294967u ? 0xffffffffu
                                                       : milliseconds * 1000u;
    tq::probe::engineCount(tq::probe::CounterLoopSleepRequestedUs, requested);
    tq::probe::engineCount(tq::probe::CounterLoopSleepUs,
                           tq::probe::microsecondsSince(started));
}

BOOL __stdcall hookLoopGetMessage(LPMSG message, HWND window, UINT first,
                                  UINT last) {
    if (!g_loopGetMessage) return FALSE;
    const int64_t started = tq::probe::now();
    const BOOL result = g_loopGetMessage(message, window, first, last);
    tq::probe::engineCount(tq::probe::CounterLoopMessage);
    tq::probe::engineCount(tq::probe::CounterLoopMessageUs,
                           tq::probe::microsecondsSince(started));
    return result;
}

DWORD __stdcall hookLoopWait(HANDLE handle, DWORD milliseconds) {
    if (!g_loopWait) return WAIT_FAILED;
    const int64_t started = tq::probe::now();
    const DWORD result = g_loopWait(handle, milliseconds);
    tq::probe::engineCount(tq::probe::CounterLoopWait);
    tq::probe::engineCount(tq::probe::CounterLoopWaitUs,
                           tq::probe::microsecondsSince(started));
    return result;
}

void __fastcall hookGameUpdate(void* self, void* edx, int delta) {
    if (!g_gameUpdate) return;
    const int64_t started = tq::probe::now();
    g_gameUpdate(self, edx, delta);
    tq::probe::engineCount(tq::probe::CounterGameUpdate);
    tq::probe::engineCount(tq::probe::CounterGameUpdateUs,
                           tq::probe::microsecondsSince(started));
}

// ---------------------------------------------------------------------------
// Install.

bool wants(unsigned group) {
    return (g_traceMask & kGroupAll) != 0 || (g_traceMask & group) != 0;
}

Signature signature(const BYTE* bytes, SIZE_T length,
                    const Relocation* relocations = nullptr,
                    unsigned relocationCount = 0) {
    Signature result = {bytes, length, relocations, relocationCount};
    return result;
}

// Resolves by decorated name and asserts the RVA. Either half alone would be
// weaker: the name survives a rebase but not a renamed build, the RVA survives
// a renamed build but not a rebase, and requiring both is what makes "this is
// the audited Engine.dll" a statement rather than a hope.
void* resolve(HMODULE engine, const char* name, DWORD rva) {
    void* address = (void*)GetProcAddress(engine, name);
    if (!address) {
        tq::hdr::log("Engine trace: %s is not exported\r\n", name);
        return nullptr;
    }
    if (address != (void*)((BYTE*)engine + rva)) {
        tq::hdr::log("Engine trace: %s resolved to %p, expected %p\r\n", name,
                     address, (void*)((BYTE*)engine + rva));
        return nullptr;
    }
    return address;
}

void note(const char* what, bool ok) {
    if (ok) ++g_installedHooks;
    tq::hdr::log("Engine trace: %s %s\r\n", what, ok ? "installed" : "skipped");
}

// Every attach here hands the detour the global the hook calls through, so the
// trampoline is published before the entry is patched rather than after.
bool installLoads(HMODULE engine) {
    void* target = resolve(engine, kLoadLevelName, kLoadLevelRva);
    if (target)
        tq::detour::attach(g_loadLevelDetour, engine, target,
                           signature(kLoadLevelBytes, sizeof(kLoadLevelBytes)),
                           6, (const void*)&hookLoadLevel,
                           (void**)&g_loadLevel);
    note("Region::LoadLevel", g_loadLevel != nullptr);

    target = resolve(engine, kLoadResourceName, kLoadResourceRva);
    if (target)
        tq::detour::attach(
            g_loadResourceDetour, engine, target,
            signature(kLoadResourceBytes, sizeof(kLoadResourceBytes),
                      kLoadResourceRelocs, 1),
            7, (const void*)&hookLoadResource, (void**)&g_loadResource);
    note("ResourceLoader::LoadResource", g_loadResource != nullptr);

    target = resolve(engine, kUnloadLevelName, kUnloadLevelRva);
    if (target)
        tq::detour::attach(
            g_unloadLevelDetour, engine, target,
            signature(kUnloadLevelBytes, sizeof(kUnloadLevelBytes),
                      kUnloadLevelRelocs, 2),
            7, (const void*)&hookUnloadLevel, (void**)&g_unloadLevel);
    note("Region::UnloadLevel", g_unloadLevel != nullptr);

    target = resolve(engine, kEnqueueName, kEnqueueRva);
    if (target)
        tq::detour::attach(
            g_enqueueDetour, engine, target,
            signature(kEnqueueBytes, sizeof(kEnqueueBytes), kEnqueueRelocs, 1),
            7, (const void*)&hookEnqueueResource, (void**)&g_enqueue);
    note("ResourceLoader::EnqueueResource", g_enqueue != nullptr);
    return true;
}

bool installArchive(HMODULE engine) {
    void* target = resolve(engine, kReadFromFileName, kReadFromFileRva);
    if (target)
        tq::detour::attach(
            g_readFromFileDetour, engine, target,
            signature(kReadFromFileBytes, sizeof(kReadFromFileBytes)), 6,
            (const void*)&hookReadFromFile, (void**)&g_readFromFile);
    note("Archive::ReadFromFile", g_readFromFile != nullptr);

    // The block routine is not exported, so it is verified twice: its own
    // twenty-four byte prologue, and the call to zlib's uncompress that is the
    // only thing distinguishing it from three functions with the same opening.
    BYTE* block = (BYTE*)engine + kArchiveBlockRva;
    const bool anchored = tq::detour::matches(
        engine, (BYTE*)engine + kArchiveInflateCallRva,
        signature(kArchiveInflateCallBytes, sizeof(kArchiveInflateCallBytes)));
    if (anchored)
        tq::detour::attach(
            g_archiveBlockDetour, engine, block,
            signature(kArchiveBlockBytes, sizeof(kArchiveBlockBytes)), 6,
            (const void*)&hookArchiveBlock, (void**)&g_archiveBlock);
    note("archive block inflate", g_archiveBlock != nullptr);
    return true;
}

// The one check that can fail for a reason other than "different build": the
// call sites go through the import table, and patchCall compares what that
// slot holds against what kernel32 exports today. If a loader ever bound the
// slot to a forwarder those two differ, the patch is refused, and the column
// reads zero -- so say which pointer disagreed rather than only that the hook
// was skipped.
void* importedKernelFunction(HMODULE engine, const char* name, DWORD slotRva) {
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    void* exported = kernel ? (void*)GetProcAddress(kernel, name) : nullptr;
    void* const* slot = (void* const*)((BYTE*)engine + slotRva);
    void* bound = tq::detour::readable(slot, sizeof(*slot)) ? *slot : nullptr;
    if (exported && exported == bound) return exported;
    tq::hdr::log("Engine trace: %s import slot holds %p, kernel32 exports %p\r\n",
                 name, bound, exported);
    return nullptr;
}

bool installFence(HMODULE engine) {
    void* wait = importedKernelFunction(engine, "WaitForSingleObject",
                                        kWaitForSingleObjectSlotRva);
    const bool ok = resolve(engine, kEngineUpdateName, kEngineUpdateRva) && wait
        && tq::detour::patchCall(
               g_fencePatch, engine, (BYTE*)engine + kFenceWindowRva,
               signature(kFenceWindowBytes, sizeof(kFenceWindowBytes),
                         kFenceWindowRelocs, 2),
               kFenceCallOffset, wait, (const void*)&hookWaitForSingleObject);
    note("Engine::Update loader fence", ok);
    return ok;
}

bool installRegionLock(HMODULE engine) {
    void* enter = importedKernelFunction(engine, "EnterCriticalSection",
                                         kEnterCriticalSectionSlotRva);
    unsigned installed = 0;
    for (unsigned i = 0; enter && i < kLockSiteCount; ++i) {
        const LockSite& site = kLockSites[i];
        if (!resolve(engine, site.owner, site.ownerRva)) continue;
        if (tq::detour::patchCall(
                g_lockPatches[i], engine, (BYTE*)engine + site.windowRva,
                signature(site.bytes, sizeof(kRegionLockEbxBytes),
                          kRegionLockRelocs, 2),
                kRegionLockCallOffset, enter,
                (const void*)&hookEnterCriticalSection))
            ++installed;
    }
    tq::hdr::log("Engine trace: region lock %u/%u sites installed\r\n",
                 installed, kLockSiteCount);
    if (installed) ++g_installedHooks;
    return installed != 0;
}

bool installSweeps(HMODULE engine) {
    void* target = resolve(engine, kSweepTargetName, kSweepTargetRva);
    if (!target || !resolve(engine, kEngineUpdateName, kEngineUpdateRva)) {
        note("Engine::Update resource sweeps", false);
        return false;
    }
    // Both windows whole, before anything is written to either.
    if (!tq::detour::matches(
            engine, (BYTE*)engine + kSweepWindowARva,
            signature(kSweepWindowABytes, sizeof(kSweepWindowABytes)))
        || !tq::detour::matches(
            engine, (BYTE*)engine + kSweepWindowBRva,
            signature(kSweepWindowBBytes, sizeof(kSweepWindowBBytes)))) {
        note("Engine::Update resource sweeps", false);
        return false;
    }
    g_sweep = (SweepFn)target;
    unsigned installed = 0;
    for (unsigned i = 0; i < kSweepCount; ++i) {
        const SweepSite& site = kSweepSites[i];
        BYTE* call = (BYTE*)engine + site.windowRva + site.callOffset;
        if (tq::detour::patchCall(g_sweepPatches[i], engine, call,
                                  signature(site.window + site.callOffset, 5), 0,
                                  target, (const void*)&hookSweep))
            ++installed;
    }
    tq::hdr::log("Engine trace: resource sweeps %u/%u sites installed\r\n",
                 installed, kSweepCount);
    if (installed) ++g_installedHooks;
    else g_sweep = nullptr;
    return installed != 0;
}

// The same assertion install() makes about Engine.dll, for whichever module
// is being patched: a build with a different SizeOfImage is a different build,
// and one log line beats nine failed signature matches.
bool auditedImage(HMODULE module, DWORD expectedSize, const char* what) {
    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)module;
    if (!module || !tq::detour::readable(dos, sizeof(*dos))
        || dos->e_lfanew <= 0)
        return false;
    const IMAGE_NT_HEADERS* nt =
        (const IMAGE_NT_HEADERS*)((const BYTE*)module + dos->e_lfanew);
    if (tq::detour::readable(nt, sizeof(*nt))
        && nt->Signature == IMAGE_NT_SIGNATURE
        && nt->OptionalHeader.SizeOfImage == expectedSize)
        return true;
    tq::hdr::log("Engine trace: %s is not the audited build, nothing"
                 " installed from it\r\n", what);
    return false;
}

bool installGame() {
    HMODULE game = GetModuleHandleW(L"Game.dll");
    if (!game || !auditedImage(game, kGameImageSize, "Game.dll")) {
        note("GameEngine::Update", false);
        return false;
    }
    void* target = resolve(game, kGameUpdateName, kGameUpdateRva);
    if (target)
        tq::detour::attach(
            g_gameUpdateDetour, game, target,
            signature(kGameUpdateBytes, sizeof(kGameUpdateBytes),
                      kGameUpdateRelocs, 1),
            6, (const void*)&hookGameUpdate, (void**)&g_gameUpdate);
    note("GameEngine::Update", g_gameUpdate != nullptr);
    return g_gameUpdate != nullptr;
}

// One import slot, with the original captured before the slot is rewritten so
// the hook always has something to call through.
bool redirectImport(CallPatch& patch, HMODULE executable, const char* dll,
                    const char* name, void** original, const void* replacement) {
    HMODULE provider = GetModuleHandleA(dll);
    void* target = provider ? (void*)GetProcAddress(provider, name) : nullptr;
    if (!target) {
        tq::hdr::log("Engine trace: %s!%s is not resolvable\r\n", dll, name);
        return false;
    }
    *original = target;
    if (tq::detour::patchImport(patch, executable, dll, name, target,
                                replacement))
        return true;
    *original = nullptr;
    tq::hdr::log("Engine trace: TQ.exe's %s import does not hold %p\r\n",
                 name, target);
    return false;
}

bool installLoop() {
    HMODULE executable = GetModuleHandleW(nullptr);
    if (!executable
        || !auditedImage(executable, kExecutableImageSize, "TQ.exe")) {
        note("TQ.exe main loop", false);
        return false;
    }
    unsigned installed = 0;
    installed += redirectImport(g_loopSleepPatch, executable, "kernel32.dll",
                                "Sleep", (void**)&g_loopSleep,
                                (const void*)&hookLoopSleep) ? 1u : 0u;
    installed += redirectImport(g_loopMessagePatch, executable, "user32.dll",
                                "GetMessageA", (void**)&g_loopGetMessage,
                                (const void*)&hookLoopGetMessage) ? 1u : 0u;
    installed += redirectImport(g_loopWaitPatch, executable, "kernel32.dll",
                                "WaitForSingleObject", (void**)&g_loopWait,
                                (const void*)&hookLoopWait) ? 1u : 0u;
    tq::hdr::log("Engine trace: TQ.exe main loop %u/3 imports redirected\r\n",
                 installed);
    if (installed) ++g_installedHooks;
    return installed != 0;
}

bool installFrame(HMODULE engine) {
    void* target = resolve(engine, kEngineUpdateName, kEngineUpdateRva);
    if (target)
        tq::detour::attach(
            g_engineUpdateDetour, engine, target,
            signature(kEngineUpdateBytes, sizeof(kEngineUpdateBytes),
                      kEngineUpdateRelocs, 1),
            6, (const void*)&hookEngineUpdate, (void**)&g_engineUpdate);
    note("Engine::Update", g_engineUpdate != nullptr);

    target = resolve(engine, kEngineRenderName, kEngineRenderRva);
    if (target)
        tq::detour::attach(
            g_engineRenderDetour, engine, target,
            signature(kEngineRenderBytes, sizeof(kEngineRenderBytes),
                      kEngineRenderRelocs, 1),
            6, (const void*)&hookEngineRender, (void**)&g_engineRender);
    note("Engine::Render", g_engineRender != nullptr);
    return true;
}

bool installWait(HMODULE engine) {
    void* target = resolve(engine, kWaitForLoadingName, kWaitForLoadingRva);
    const bool ok = target
        && tq::detour::replace(
               g_waitForLoadingDetour, engine, target,
               signature(kWaitForLoadingBytes, sizeof(kWaitForLoadingBytes)),
               sizeof(kWaitForLoadingBytes),
               (const void*)&hookWaitForLoadingToFinish);
    note("Region::WaitForLoadingToFinish", ok);
    return ok;
}

}  // namespace

void readOptions(const wchar_t* iniPath) {
    // No INI is the shipping configuration, and the shipping configuration has
    // the probe off, so the default here only decides what a trace run gets.
    g_traceMask = iniPath && iniPath[0]
        ? (unsigned)GetPrivateProfileIntW(L"debug", L"engine_trace", 1, iniPath)
        : 1u;
}

bool install(HMODULE engine) {
    if (!engine || !tq::probe::enabled() || !g_traceMask) return false;
    if (InterlockedCompareExchange(&g_installed, 1, 0)) return false;

    if (!auditedImage(engine, kEngineImageSize, "Engine.dll")) {
        InterlockedExchange(&g_installed, 0);
        return false;
    }

    const volatile DWORD* mainThread =
        (const volatile DWORD*)((BYTE*)engine + kMainThreadIdRva);
    g_mainThreadId = tq::detour::readable((const void*)mainThread,
                                          sizeof(DWORD)) ? mainThread : nullptr;

    g_installedHooks = 0;
    if (wants(kGroupLoads)) installLoads(engine);
    if (wants(kGroupArchive)) installArchive(engine);
    if (wants(kGroupFence)) installFence(engine);
    if (wants(kGroupLock)) installRegionLock(engine);
    if (wants(kGroupSweeps)) installSweeps(engine);
    if (wants(kGroupWait)) installWait(engine);
    if (wants(kGroupFrame)) installFrame(engine);
    if (wants(kGroupGame)) installGame();
    if (wants(kGroupLoop)) installLoop();

    tq::hdr::log("Engine trace: mask=0x%x hooks=%u main thread id at %p\r\n",
                 g_traceMask, g_installedHooks, (const void*)g_mainThreadId);
    if (g_installedHooks) return true;
    InterlockedExchange(&g_installed, 0);
    return false;
}

void shutdown() {
    // Reverse of the install order, and each restore checks the site still
    // holds what we wrote before it puts the original back.
    tq::detour::restoreCall(g_loopWaitPatch);
    g_loopWait = nullptr;
    tq::detour::restoreCall(g_loopMessagePatch);
    g_loopGetMessage = nullptr;
    tq::detour::restoreCall(g_loopSleepPatch);
    g_loopSleep = nullptr;
    tq::detour::detach(g_gameUpdateDetour);
    g_gameUpdate = nullptr;
    tq::detour::detach(g_engineRenderDetour);
    g_engineRender = nullptr;
    tq::detour::detach(g_engineUpdateDetour);
    g_engineUpdate = nullptr;
    tq::detour::detach(g_waitForLoadingDetour);
    for (int i = (int)kSweepCount - 1; i >= 0; --i)
        tq::detour::restoreCall(g_sweepPatches[i]);
    g_sweep = nullptr;
    for (int i = (int)kLockSiteCount - 1; i >= 0; --i)
        tq::detour::restoreCall(g_lockPatches[i]);
    tq::detour::restoreCall(g_fencePatch);
    tq::detour::detach(g_archiveBlockDetour);
    g_archiveBlock = nullptr;
    tq::detour::detach(g_readFromFileDetour);
    g_readFromFile = nullptr;
    tq::detour::detach(g_enqueueDetour);
    g_enqueue = nullptr;
    tq::detour::detach(g_unloadLevelDetour);
    g_unloadLevel = nullptr;
    tq::detour::detach(g_loadResourceDetour);
    g_loadResource = nullptr;
    tq::detour::detach(g_loadLevelDetour);
    g_loadLevel = nullptr;
    g_mainThreadId = nullptr;
    g_installedHooks = 0;
    InterlockedExchange(&g_installed, 0);
}

#ifdef TQ_SELFTEST
unsigned installedForTest() { return g_installedHooks; }
void enterCriticalSectionForTest(LPCRITICAL_SECTION section) {
    hookEnterCriticalSection(section);
}
void setTraceMaskForTest(unsigned mask) { g_traceMask = mask; }
#endif

}  // namespace engineprobe
}  // namespace tq
