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
