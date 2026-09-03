#pragma once

#include <windows.h>
#include <d3d11.h>
#include <stdint.h>

namespace tq {
namespace probe {

// Frame-hitch attribution.
//
// The frame overlay answers "is this frame slow". This answers "why was *that*
// frame slow", which is a different question and needs a different instrument:
// a per-frame record of where the render thread spent its time, what it was
// asked to do, and what the GPU did, kept for the last few hundred frames and
// written out only for the frames that actually spiked -- together with how far
// each field sat from its own recent median, which is the part that names the
// cause.
//
// It is off unless [debug] performance_trace is set in tqflicker.ini: 1 writes
// one row per hitching frame and is cheap enough to leave on, `full` writes
// every frame for a measurement session. While it is off this module allocates
// nothing, measures nothing, writes nothing, and creates no device objects;
// the only cost that remains compiled in is one cached-bool branch at each
// instrumented call site.
//
// Deliberately not instrumented with timers: Map, Unmap and PSSetShaderResources
// run 2400-5000 times a frame, and a QueryPerformanceCounter pair on each would
// cost more than the work it measured. Those get counters, and their real cost
// is priced by turning the feature off in the INI instead.

// ---------------------------------------------------------------------------
// CPU phases. The order is the CSV column order and must not be reshuffled
// without regenerating any recorded run alongside it.
enum Phase {
    PhasePresent,        // our pre-Present callback, exclusive of the three
                         // phases below that it brackets
    PhaseGrassPresent,   // grass::onPresent -- completes a staged grass readback
    PhaseStreamStep,     // one progressive texture upload chunk
    PhaseOverlayRaster,  // overlay CPU rasterisation and its texture uploads
    PhaseOverlayDraw,    // overlay state save, draws, and restore
    PhaseSmaa,
    PhaseBloom,
    PhaseShaderCreate,   // CreateVertexShader/CreatePixelShader, incl. DXBC work
    PhaseTextureCreate,
    PhaseBufferCreate,
    PhaseGrassFill,      // grass unmap rewrite, accumulated over the frame
    PhaseGrassCross,     // the crossing draw, accumulated over the frame
    // The game's own Present call. When the GPU is the bottleneck this is
    // where the frame waits, so it is what separates "slow because of work on
    // this thread" from "slow because the GPU is behind".
    PhasePresentCall,
    PhaseCount
};

// Counts of things done in the frame. Cheap enough for the hot hooks that
// timing them would be dishonest.
enum Counter {
    CounterDraw,
    CounterDrawIndexed,
    CounterGrassDraw,
    CounterGrassCross,
    CounterMap,
    CounterUnmap,
    CounterGrassFill,
    CounterGrassRotate,
    CounterGrassAdopt,
    CounterGrassSeedQueued,
    CounterGrassSeedDone,
    CounterGrassSeedFailed,
    CounterGrassTwinCreate,
    CounterGrassTwinFail,
    CounterUploadSteps,
    CounterUploadKiB,
    // Progressive jobs started and finished, so a session's total upload
    // volume can be read against the number of textures it actually covered
    // rather than assumed to be one pass over each.
    CounterUploadJobsStarted,
    CounterUploadJobsDone,
    CounterUploadRejected,
    CounterShaderCreate,
    CounterTextureCreate,
    CounterBufferCreate,
    CounterSetRenderTargets,
    CounterShadowBind,
    CounterShadowFitChange,
    CounterPsSetSrv,

    // ---------------------------------------------------------------------
    // The engine channel. Everything from here down is written from the game's
    // loader thread or its main thread, never from the render thread, and so
    // must go through engineCount() rather than count(): countInternal drops
    // every write that is not the render thread's, which is exactly why the
    // texture-load counters this block replaces were reading zero.
    //
    // Durations here are named `_us`, and that is load-bearing rather than
    // stylistic. tools/frames.py builds "the mod's share" from every column
    // ending in `_ms`, so an engine `_ms` column would be silently charged to
    // the mod; `_us` falls into its counter bucket instead.
    CounterEngineFirst,
    // The three outcomes CounterUploadRejected used to conflate, plus the two
    // Stage 2 adds. Their sum is still reported as upload_rejected, so a run
    // recorded before the split compares on that column unchanged.
    CounterUploadRejectPool = CounterEngineFirst,  // no free job slot
    CounterUploadRejectBudget,                     // over the retained-byte cap
    CounterUploadRejectAlloc,                      // the retention buffer failed
    CounterUploadRejectScan,                       // no owning File on the stack
    // Which File class served the texture. The whole progressive uploader is
    // predicated on an answer to this, and until now nothing reported it.
    CounterUploadSrcArc,
    CounterUploadSrcLoose,
    CounterUploadSrcNone,
    // CreateTexture2D calls that did not arrive on the render thread -- i.e.
    // the game's own texture loads -- and how long they took. This is the
    // number that prices Stage 2's copy against the upload it replaces.
    CounterEngineTexCreateOff,
    CounterEngineTexCreateOffUs,
    // The retire path, split three ways. Run 4 showed 9 of its 10 worst frames
    // retiring a job against a 5.4% base rate, on chunks of 128-512 KiB -- so
    // the cost is not the upload, it is what happens when a job finishes. That
    // is two COM releases, which under DXMT free the texture's device memory,
    // and one UnmapViewOfFile over a view of the whole source file. These say
    // which, rather than leaving it to be argued.
    // upload_unmap_us is the worker's time; the _inline_ pair is what still
    // had to be paid on the render thread because the queue was full. Run 5
    // measured 1.03 s of UnmapViewOfFile inside Present, 92-98% of every worst
    // stream_step frame, so the inline columns are the ones that must stay at
    // zero for the move to have worked.
    CounterUploadUnmap,
    CounterUploadUnmapUs,
    CounterUploadUnmapInline,
    CounterUploadUnmapInlineUs,
    CounterUploadReleaseUs,
    // The loose-file size gate. A texture pack can ship assets far larger than
    // anything the game itself contains -- this install has 984 over 4096 on a
    // side, up to 16384x16384 -- and those are what make a mapped view
    // expensive to tear down and a synchronous create expensive to do at all.
    // Files the loose source served at all, so a run can tell "the gate never
    // saw a loose file" from "it saw them and declined to inspect them" --
    // the ambiguity that hid a bug in the gate's own class check for a whole
    // session.
    CounterLooseOpen,
    CounterLooseProbe,
    CounterLooseProbeUs,
    CounterLooseRejectOversize,
    // Opens the archive source served. A redirect is only correct if the file
    // the loose source refused turns up here instead, so this is the other
    // half of the evidence rather than a curiosity.
    CounterArcOpen,
    // Address space held by live mapping leases, sampled once a frame so the
    // column reads as a gauge rather than a total. The pool's only real limit
    // is a count -- 128 leases -- and a count says nothing about bytes: at the
    // measured median texture that is ~340 MiB of views, and at the 4K cap's
    // maximum it would be 2.7 GiB, in a process that has about 3.
    CounterUploadLeasedMib,

    // ---------------------------------------------------------------------
    // Engine.dll's own work, from src/engine_probe.cpp. Everything below is
    // written from inside the game's code on whichever thread reached it, and
    // exists to name the residual tools/frames.py has always had to report as
    // "the game's frame": ~7-8.7 s of hitch time a session that no mod column
    // accounts for.
    //
    // The `_us` totals nest. A resource load inside a level load is counted in
    // both, so they are a breakdown of where a hitch went and not a partition
    // to be summed.
    //
    // `_main` is the subset that ran on the thread the engine recorded as its
    // own at Engine+0x41a5dc -- i.e. the loads the game forced synchronously
    // rather than handing to its loader thread. Those are the ones that are
    // in the frame rather than beside it.
    CounterEngineLevelLoad,
    CounterEngineLevelLoadUs,
    CounterEngineLevelLoadMain,
    CounterEngineLevelLoadMainUs,
    CounterEngineResLoad,
    CounterEngineResLoadUs,
    CounterEngineResLoadMain,
    CounterEngineResLoadMainUs,
    CounterEngineRegionUnload,
    CounterEngineRegionUnloadUs,
    // Archive::ReadFromFile calls and the bytes they asked for. Counted, not
    // timed: the worst frame measured carries ~1,300 archive opens, and a
    // QueryPerformanceCounter pair on each would price the instrument rather
    // than the read.
    CounterEngineArcRead,
    CounterEngineArcKib,
    // One 256 KiB block read and inflated. This one is timed, because a single
    // inflate costs on the order of a millisecond and the pair that measures
    // it does not.
    CounterEngineArcBlocks,
    CounterEngineArcInflateUs,
    // Work handed to the loader thread. Rising here before a hitch is the
    // backlog forming; rising in the _main columns is the backlog being paid.
    CounterEngineResEnqueued,
    // Engine::Update's rendezvous with the loader fence, once per update, so
    // the count doubles as the number of engine updates the frame contained.
    CounterEngineFenceWait,
    CounterEngineFenceWaitUs,
    // The region lock, taken on the render path by both AddElementsInBox
    // overloads and by Region::GetEntitiesInFrustum. Only contended
    // acquisitions are counted or timed -- an uncontended one costs one
    // interlocked op and a branch and records nothing.
    CounterEngineRegionLockHits,
    CounterEngineRegionLockUs,
    // The seven UnloadUnreferencedResources sweeps Engine::Update runs every
    // update. A candidate for the class of hitch that carries no archive
    // opens and names nothing, which is exactly why it is measured before
    // anything is done about it.
    CounterEngineSweeps,
    CounterEngineSweepUs,
    // Region::WaitForLoadingToFinish, a spin on a load flag that a full .text
    // scan finds no caller for. Non-zero here would be genuine news.
    CounterEngineWaitLoading,
    CounterEngineWaitLoadingUs,
    // The frame, split in two at the engine's own seam. Run 10 named only a
    // fifth to a third of the hitch time: its worst frame spent 511 ms in a
    // forced level load and 950 ms somewhere nothing could see, and a whole
    // class of 200-240 ms frames showed *every* other column at zero. These
    // two brackets say which half of the game's frame that time is in --
    // update or render -- or whether it is in neither, which would put it
    // outside Engine.dll altogether. Both run once a frame, so they cost
    // nothing worth counting.
    CounterEngineUpdate,
    CounterEngineUpdateUs,
    CounterEngineRender,
    CounterEngineRenderUs,
    // Game.dll's simulation tick, which is neither of the two above. Run 11
    // closed the session's accounting to 100% and found 38% of the hitch time
    // -- and the majority of hitching frames -- outside Engine.dll entirely,
    // on frames that render normally with the mod idle. This is the only
    // candidate large enough to hold it; if it stays small, what is left is
    // TQ.exe's own loop or the layer below the process.
    CounterGameUpdate,
    CounterGameUpdateUs,
    // TQ.exe's main loop, reached through its import table rather than by
    // patching anything. Run 12 left 11.6% of the session and 44.7% of the
    // time in frames over 100 ms outside every bracket, arriving as bursts of
    // 50-398 ms events rather than as a steady tax -- and the loop's whole
    // vocabulary for blocking is these three imports. It has no PeekMessage
    // at all, so its message pump is the blocking GetMessageA, and it asks
    // GameEngine::NeedsSleep and then sleeps.
    //
    // The requested/actual pair on the sleep is the point: a loop that asks
    // for one millisecond and is handed two hundred is an environment
    // problem, not a game one, and the two columns say which immediately.
    CounterLoopSleep,
    CounterLoopSleepRequestedUs,
    CounterLoopSleepUs,
    CounterLoopMessage,
    CounterLoopMessageUs,
    CounterLoopWait,
    CounterLoopWaitUs,
    // Free address space in the process, sampled once a frame as a gauge.
    // The mapping-lease explanation for the stalls is already dead, but the
    // 32-bit address space is the one resource whose exhaustion would look
    // exactly like this, and one call a frame settles it rather than leaving
    // it as a second run.
    CounterProcAvailVaMib,
    // Engine::PresentSurface, and the collision fixup beside it. Reading
    // TQ.exe's main loop settled where the residual has to be: the loop runs
    // GameEngine::Update, then *PresentSurface*, then Engine::Render -- and
    // the probe's own frame boundary is the D3D Present *inside*
    // PresentSurface. So the head of that function, where a wait on the
    // swapchain or the GPU would live, has been in every frame's window and
    // inside none of the brackets. Both are reached through TQ.exe's import
    // table, so neither costs a byte of patched code.
    CounterEnginePresentSurface,
    CounterEnginePresentSurfaceUs,
    CounterGameCollisions,
    CounterGameCollisionsUs,
    // The rest of TQ.exe's main loop, which run 14 proved is where the
    // residual lives -- PresentSurface took only 4.6% of the hitch time and
    // the loop is 2,326 bytes, not the 700 first read. These are the six
    // calls in it that do work rather than return a pointer, in the order the
    // loop makes them:
    //
    //   THQNO_Process              the online/platform layer. libcurl sits
    //                              behind it, and bursty network or IPC work
    //                              is the exact shape of the measured stalls.
    //   GraphicsEngine::UpdateFromOptions
    //   Jukebox::Update            music, streamed
    //   SoundManager::Update       audio, streamed
    //   QuestRepository::FireTriggers
    //   EWindow::ProcessMessages   the real message pump, and the reason
    //                              TQ.exe's own GetMessageA read zero calls:
    //                              the pump is Engine.dll's, not the exe's.
    //                              Its return value is what ends the loop.
    CounterLoopPlatform,
    CounterLoopPlatformUs,
    CounterLoopGfxOptions,
    CounterLoopGfxOptionsUs,
    CounterLoopJukebox,
    CounterLoopJukeboxUs,
    CounterLoopSound,
    CounterLoopSoundUs,
    CounterLoopQuests,
    CounterLoopQuestsUs,
    CounterLoopPump,
    CounterLoopPumpUs,
    // Inside the pump. Run 15 found it: EWindow::ProcessMessages is 20.3% of
    // the hitch time, and the worst frames are 119-214 ms of pump with
    // everything else at nothing. The function itself is nine instructions --
    // PeekMessageA(PM_REMOVE), TranslateMessage, DispatchMessageA, looping
    // until the queue drains -- so there are exactly two places the time can
    // be, and they mean opposite things.
    //
    // Many cheap peeks means a message flood, and the count says so.  One
    // expensive peek means the host is not answering, which under CrossOver
    // is a wineserver round trip and not the game's problem at all.  Time in
    // dispatch means the game's own window procedure, which is the one case
    // that is the game's problem and the one that could be fixed here.
    //
    // These hook Engine.dll's import table rather than the executable's; the
    // pump belongs to Engine.dll, which is why TQ.exe's GetMessageA read zero.
    CounterPumpPeek,
    CounterPumpPeekUs,
    CounterPumpDispatch,
    CounterPumpDispatchUs,
    // The peek split by what it returned. PeekMessage does not block, so a
    // slow one is the call itself being slow, not a wait for a message to
    // arrive -- and if the slow ones are the peeks that found the queue
    // EMPTY, that is conclusive: the cost is the round trip to ask, not
    // anything the game or its window has to do.
    CounterPumpPeekMiss,
    CounterPumpPeekMissUs,
    CounterCount
};

// GPU regions, timed with timestamp queries. Non-blocking: results are read
// with D3D11_ASYNC_GETDATA_DONOTFLUSH several frames later and attributed to
// the frame they were opened in, so a frame's GPU columns may be blank.
//
// A region entered and left more than once in a frame -- the eight point-shadow
// passes -- measures from its first entry to its last exit, gaps included.
enum GpuPhase {
    GpuFrame,
    GpuShadowDirectional,
    GpuShadowPoint,
    GpuGrass,
    GpuSmaa,
    GpuBloom,
    GpuPhaseCount
};

// ---------------------------------------------------------------------------
namespace detail {
// Read once at install and never written again from the render thread, so the
// hot-path test is a plain load. Exposed only so enabled() can be inlined.
extern bool active;
}

inline bool enabled() { return detail::active; }

// Reads [debug] performance_trace from the INI beside the executable. Called
// once from the visual install, before anything else here. Taking the path
// rather than deriving it is what lets the off-game test drive this module.
void readOptions(const wchar_t* iniPath);

// Where the CSV is written. Defaults to tqflicker-frames.csv beside the
// executable; the off-game test redirects it. Must be called before the first
// completed frame, and only has an effect while enabled.
void setOutputPath(const wchar_t* csvPath);

// True when every frame is written, rather than only the frames that hitched.
bool logsEveryFrame();

// ---------------------------------------------------------------------------
// Timing.

// QueryPerformanceCounter, or 0 while disabled.
int64_t now();

// Adds the interval from `startTicks` to now into this frame's `phase` total.
// Phases and counters record the render thread only: the frame record is one
// unsynchronized struct, and the question the columns answer is where *this
// thread's* frame went -- a texture created concurrently on the game's loader
// thread is neither, and writing it here would tear the record.
void addPhaseInternal(Phase phase, int64_t startTicks);
inline void addPhase(Phase phase, int64_t startTicks) {
    if (detail::active) addPhaseInternal(phase, startTicks);
}

// Scoped form. Costs one predictable branch while the probe is off.
struct Scope {
    int64_t start;
    Phase phase;
    explicit Scope(Phase p) : start(enabled() ? now() : 0), phase(p) {}
    ~Scope() { if (start) addPhase(phase, start); }
};

// Counted on hooks that run thousands of times a frame, so the disabled path
// has to be a load and a branch rather than a call into another object file.
void countInternal(Counter counter, uint32_t amount);
inline void count(Counter counter, uint32_t amount = 1) {
    if (detail::active) countInternal(counter, amount);
}

// Whether the caller is the thread whose frames are being recorded. True
// before the first frame closes, when no render thread has been learned yet,
// so a call site that branches on this behaves the same way count() does.
bool isRenderThread();

// The counting channel for the game's own threads. Accumulates into an
// interlocked side array and folds into the frame record at endFrame, so the
// column, its median and the `unusual` attribution all work unchanged -- but
// unlike count() it cannot tear the record, because it never touches it.
//
// Legal for any counter, including the ones above CounterEngineFirst; a
// render-thread write lands in the frame that is closing, exactly as count()
// would. What is not legal is the reverse: nothing below CounterEngineFirst
// may be written with count(), or it silently reads zero.
void engineCountInternal(Counter counter, uint32_t amount);
inline void engineCount(Counter counter, uint32_t amount = 1) {
    if (detail::active) engineCountInternal(counter, amount);
}

// Elapsed microseconds since a now() reading, saturating at UINT32_MAX and 0
// while the probe is disabled. For the `_us` counters, whose whole point is to
// carry a duration through the integer channel.
uint32_t microsecondsSince(int64_t startTicks);

// ---------------------------------------------------------------------------
// GPU regions. Created on the shader-build worker rather than the render
// thread, for the same reason the overlay's resources are: DXMT does not want
// device objects created re-entrantly from inside a hooked call.
bool createResources(ID3D11Device* device);
void releaseResources();
void gpuBegin(ID3D11DeviceContext* context, GpuPhase phase);
void gpuEnd(ID3D11DeviceContext* context, GpuPhase phase);

// Scoped form, so a function with several returns cannot leave a region open
// -- an unclosed region is never resolved and its column silently reads blank.
struct GpuScope {
    ID3D11DeviceContext* context;
    GpuPhase phase;
    GpuScope(ID3D11DeviceContext* c, GpuPhase p) : context(c), phase(p) {
        gpuBegin(context, phase);
    }
    ~GpuScope() { gpuEnd(context, phase); }
};

// ---------------------------------------------------------------------------
// Frame boundary. beginFrame collects whatever GPU results have landed and
// opens the whole-frame region; endFrame closes the record with the frame time
// the overlay measured, decides whether it hitched, and queues a row.
void beginFrame(ID3D11DeviceContext* context);
// Closes the whole-frame GPU region on the context beginFrame was given, so it
// needs no context of its own.
void endFrame(float cpuFrameMilliseconds);

// The three phases with the largest recent mean, and the phase that dominated
// the most recent hitch, formatted for the overlay panel. Writes an empty
// string when there is nothing to report.
void summarize(char* out, size_t size);

// Flushes and stops the writer. Safe to call when disabled or never started.
void shutdown();

// Writes whatever the ring still holds, without touching the writer thread or
// any lock. For the process-termination path only, where every other thread is
// already gone and shutdown() is never reached: Titan Quest exits without an
// explicit unload, so this is what saves the tail of a run.
void flushOnExit();

#ifdef TQ_SELFTEST
// The frame the ring is about to write, so a test can drive frames and then
// assert on what was recorded without waiting for the writer.
unsigned frameCountForTest();
float phaseMillisecondsForTest(unsigned framesBack, Phase phase);
// Whether that frame's timestamp queries came back, so a test can assert the
// GPU side actually reports rather than merely not crashing.
bool gpuResolvedForTest(unsigned framesBack);
uint32_t counterForTest(unsigned framesBack, Counter counter);
void resetForTest();
#endif

}  // namespace probe
}  // namespace tq
