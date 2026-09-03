#include "probe.h"

#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace tq {
namespace probe {

namespace detail {
bool active;
bool drawTiming;
}

namespace {

// Enough history for a hitch to be compared against a steady baseline, and for
// the GPU results of a frame to land before that frame is written out.
const unsigned kRecordCount = 256;
const unsigned kBaselineFrames = 60;
// A frame is written no sooner than this, and is held further until its
// timestamp queries retire -- but never longer than kGpuPatience, so a device
// that resolves nothing still produces rows, with the GPU columns blank.
const unsigned kEmitDelay = 8;
const unsigned kGpuPatience = 24;
const unsigned kGpuSlotCount = 16;
const unsigned kLogBytes = 64 * 1024;

enum Mode { ModeOff, ModeHitch, ModeFull };

struct FrameRecord {
    unsigned index;
    float milliseconds;
    float phaseMs[PhaseCount];
    uint32_t counters[CounterCount];
    float gpuMs[GpuPhaseCount];
    bool gpuResolved;
};

struct GpuSlot {
    ID3D11Query* disjoint;
    ID3D11Query* begin[GpuPhaseCount];
    ID3D11Query* end[GpuPhaseCount];
    bool opened[GpuPhaseCount];
    bool closed[GpuPhaseCount];
    unsigned frameIndex;
    bool pending;
};

// No explicit bound on any of these four: with one, `sizeof / sizeof` is the
// bound rather than the initializer count, so the assertions below would hold
// however many names were actually written and a phase added without a name
// would ship with an empty CSV column. Without it they count what is there.
const char* const kPhaseNames[] = {
    "present", "grass_present", "stream_step", "overlay_raster", "overlay_draw",
    "smaa", "bloom", "shader_create", "texture_create", "buffer_create",
    "grass_fill", "grass_cross", "present_call", "draw_submit", "map_resource"
};

const char* const kCounterNames[] = {
    "draw", "draw_indexed", "grass_draw", "grass_cross", "map", "unmap",
    "grass_fill", "grass_rotate", "grass_adopt", "grass_seed_queued",
    "grass_seed_done", "grass_seed_failed", "grass_twin_create",
    "grass_twin_fail", "upload_steps", "upload_kib", "upload_jobs_started",
    "upload_jobs_done", "upload_rejected", "shader_create",
    "texture_create", "buffer_create", "set_render_targets", "shadow_bind",
    "shadow_fit_change", "ps_set_srv",
    // The engine channel; see the enum. Durations are `_us`, deliberately.
    "upload_reject_pool", "upload_reject_budget", "upload_reject_alloc",
    "upload_reject_scan", "upload_src_arc", "upload_src_loose",
    "upload_src_none", "engine_tex_create_off", "engine_tex_create_off_us",
    "upload_unmap", "upload_unmap_us", "upload_unmap_inline",
    "upload_unmap_inline_us", "upload_release_us",
    "loose_open", "loose_probe", "loose_probe_us", "loose_reject_oversize",
    "arc_open", "upload_leased_mib",
    "arc_cache_hit", "arc_cache_hit_us", "arc_cache_store", "arc_cache_evict",
    "arc_cache_verify", "arc_cache_bad", "arc_cache_skip",
    // Engine.dll's own work. Durations are `_us` here too, and for the same
    // reason: frames.py must not charge the game's loading to the mod.
    "engine_level_load", "engine_level_load_us", "engine_level_load_main",
    "engine_level_load_main_us",
    "engine_res_load", "engine_res_load_us", "engine_res_load_main",
    "engine_res_load_main_us",
    "engine_region_unload", "engine_region_unload_us",
    "engine_arc_read", "engine_arc_kib",
    "engine_arc_blocks", "engine_arc_inflate_us",
    "engine_res_enqueued",
    "engine_fence_wait", "engine_fence_wait_us",
    "engine_region_lock_hits", "engine_region_lock_us",
    "engine_sweeps", "engine_sweep_us",
    "engine_wait_loading", "engine_wait_loading_us",
    "engine_update", "engine_update_us", "engine_render", "engine_render_us",
    "game_update", "game_update_us",
    "loop_sleep", "loop_sleep_req_us", "loop_sleep_us",
    "loop_message", "loop_message_us", "loop_wait", "loop_wait_us",
    "proc_avail_va_mib",
    "engine_present_surface", "engine_present_surface_us",
    "game_collisions", "game_collisions_us",
    "loop_platform", "loop_platform_us", "loop_gfx_options",
    "loop_gfx_options_us", "loop_jukebox", "loop_jukebox_us",
    "loop_sound", "loop_sound_us", "loop_quests", "loop_quests_us",
    "loop_pump", "loop_pump_us",
    "pump_peek", "pump_peek_us", "pump_dispatch", "pump_dispatch_us",
    "pump_peek_miss", "pump_peek_miss_us",
    "pump_timer_full", "pump_timer_split",
    "engine_heap_alloc", "engine_heap_alloc_us", "engine_heap_alloc_kib",
    "engine_heap_big", "engine_heap_big_us",
    "engine_heap_free", "engine_heap_free_us",
    "engine_io_seek", "engine_io_seek_us",
    "engine_io_read", "engine_io_read_us", "engine_io_read_kib",
    "engine_cs_wait", "engine_cs_wait_us",
    "engine_obj_wait", "engine_obj_wait_us",
    "engine_sleep", "engine_sleep_us",
    "engine_cs_wait_main", "engine_cs_wait_main_us",
    "engine_obj_wait_main", "engine_obj_wait_main_us",
    "engine_sleep_main", "engine_sleep_main_us", "engine_sleep_main_req_us",
    "engine_async_load", "engine_async_sync",
    "engine_portal_async_load", "engine_portal_async_sync"
};
static_assert(sizeof(kCounterNames) / sizeof(kCounterNames[0]) == CounterCount,
              "every counter needs a CSV column name");
static_assert(sizeof(kPhaseNames) / sizeof(kPhaseNames[0]) == PhaseCount,
              "every phase needs a CSV column name");

// The panel's 3x5 font has A-Z, 0-9, space, '.', ':', '-', '/' and '>' and
// nothing else, so the CSV's snake_case names cannot be drawn as they stand.
const char* const kPhaseShortNames[] = {
    "PRESENT", "GRASS-PRES", "STREAM", "OVL-RAST", "OVL-DRAW", "SMAA", "BLOOM",
    "SHADER", "TEXTURE", "BUFFER", "GRASS-FILL", "GRASS-CROSS", "PRESENT-CALL",
    "DRAW", "MAP"
};
static_assert(sizeof(kPhaseShortNames) / sizeof(kPhaseShortNames[0])
                  == PhaseCount,
              "every phase needs an overlay-panel name");

const char* const kGpuNames[] = {
    "gpu_frame", "gpu_shadow_dir", "gpu_shadow_point", "gpu_grass", "gpu_smaa",
    "gpu_bloom"
};
static_assert(sizeof(kGpuNames) / sizeof(kGpuNames[0]) == GpuPhaseCount,
              "every GPU phase needs a CSV column name");

Mode g_mode = ModeOff;
// Held separately from detail::drawTiming so the header can record what was
// asked for even on a boot where the probe then failed to arm.
bool g_drawTimingRequested = false;
// What counts as a hitch. Fixed rather than configurable: it exists to make
// rows self-selecting, not to be tuned per run.
const float g_hitchMs = 20.0f;
wchar_t g_csvPath[MAX_PATH];

FrameRecord* g_records;
// The engine channel's accumulator. Written with InterlockedExchangeAdd from
// the game's threads and drained with InterlockedExchange at the top of
// endFrame, so a value belongs to the frame that was open when it was counted
// and no write can ever land in the middle of a record being copied out.
volatile LONG g_engineCounters[CounterCount];
// The thread whose frames are being recorded, learned from the Present
// callback. Device-creation hooks run on whatever thread the game calls them
// from, and a write from any other thread would race endFrame's reset of the
// current record -- and would not belong in it anyway.
DWORD g_renderThread;
unsigned g_frameIndex;        // frames completed; the next record is this one
unsigned g_emitCursor;        // the next frame index still to be written out
FrameRecord g_current;        // the frame being accumulated
LARGE_INTEGER g_frequency;

GpuSlot g_gpu[kGpuSlotCount];
unsigned g_gpuCursor;
GpuSlot* g_gpuCurrent;
// Kept from beginFrame so endFrame can close the frame's region and its
// disjoint query without being handed a context of its own.
ID3D11DeviceContext* g_gpuContext;
// Published from the shader-build worker and read on the render thread, so the
// queries behind it are never seen half-created.
volatile LONG g_gpuReady;

// How the timestamp path actually behaved, reported once at shutdown: a device
// that never retires a query has to say so rather than leave blank columns to
// be read as "the GPU did nothing".
unsigned g_gpuResolvedFrames;
unsigned g_gpuTimedOutFrames;

char* g_log;
unsigned g_logBytes;
// Set only on the process-termination path, where every other thread has
// already been killed -- possibly while holding g_logLock. Taking that lock
// then would hang the game's exit, so the exit flush runs lock-free instead:
// single-threaded by definition, best-effort by design.
bool g_exiting;

unsigned g_logDropped;
SRWLOCK g_logLock = SRWLOCK_INIT;
HANDLE g_logThread;
HANDLE g_logFlush;
HANDLE g_logStop;
volatile LONG g_logStarted;
bool g_headerWritten;

// The last hitch's dominant phase, for the overlay line.
int g_lastHitchPhase = -1;
float g_lastHitchDelta;

FrameRecord* recordAt(unsigned index) {
    if (!g_records || index >= g_frameIndex) return nullptr;
    if (g_frameIndex - index > kRecordCount) return nullptr;
    return &g_records[index % kRecordCount];
}

// ---------------------------------------------------------------------------
// Writer. Its own sink rather than tq::hdr::log, which is a 64 KiB append-only
// buffer that drops silently once full and rewrites the whole file on every
// flush -- fine for a handful of one-shot lines, fatal for per-frame rows.

// `reserved` bytes are held back from the ring, so the shutdown summary can
// always be written even when a burst has just overrun the buffer.
void appendLogReserved(const char* text, unsigned reserved) {
    if (!g_log || !text) return;
    unsigned length = (unsigned)strlen(text);
    if (!g_exiting) AcquireSRWLockExclusive(&g_logLock);
    if (g_logBytes + length + reserved < kLogBytes) {
        memcpy(g_log + g_logBytes, text, length);
        g_logBytes += length;
    } else {
        ++g_logDropped;
    }
    if (!g_exiting) ReleaseSRWLockExclusive(&g_logLock);
    if (g_logFlush && !g_exiting) SetEvent(g_logFlush);
}

void appendLog(const char* text) { appendLogReserved(text, 512); }

void flushLog() {
    char* pending = nullptr;
    unsigned bytes = 0;
    if (!g_exiting) AcquireSRWLockExclusive(&g_logLock);
    if (g_logBytes) {
        pending = (char*)malloc(g_logBytes);
        if (pending) {
            memcpy(pending, g_log, g_logBytes);
            bytes = g_logBytes;
        }
        g_logBytes = 0;
    }
    if (!g_exiting) ReleaseSRWLockExclusive(&g_logLock);
    if (!pending || !bytes) { free(pending); return; }

    HANDLE file = CreateFileW(g_csvPath, FILE_APPEND_DATA, FILE_SHARE_READ,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, pending, bytes, &written, nullptr);
        CloseHandle(file);
    }
    free(pending);
}

DWORD WINAPI logThread(void*) {
    HANDLE waits[2] = {g_logStop, g_logFlush};
    for (;;) {
        DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 250);
        flushLog();
        if (wait == WAIT_OBJECT_0) return 0;
    }
}

void ensureWriter() {
    if (InterlockedCompareExchange(&g_logStarted, 1, 0)) return;
    g_log = (char*)malloc(kLogBytes);
    g_logStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_logFlush = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_log || !g_logStop || !g_logFlush) return;
    g_logThread = CreateThread(nullptr, 0, logThread, nullptr, 0, nullptr);
}

void writeHeader() {
    if (g_headerWritten) return;
    g_headerWritten = true;
    // A fresh file per session: an appended run would silently share columns
    // with a build that may have had different ones.
    DeleteFileW(g_csvPath);
    // Sized for the whole column set with room to add to it. The overrun this
    // guards against is silent: snprintf returns the length it *would* have
    // written, so `n += snprintf(...)` walks past the end and the header is
    // emitted truncated and unterminated, with no error anywhere. The self-test
    // asserts the header's comma count against a row's for exactly this reason.
    char line[4096];
    // Named so the summarizer knows whether these rows are the whole session
    // or only its hitches, instead of guessing from index contiguity.
    snprintf(line, sizeof(line), "# performance_trace=%s\r\n",
             g_mode == ModeFull ? "full" : "hitch");
    appendLog(line);
    // Its own line, so the performance_trace marker above keeps parsing as it
    // did. draw_submit_ms and map_resource_ms read 0 either way, and a reader
    // has to be able to tell "the game did not spend time there" from "nobody
    // was holding a stopwatch".
    snprintf(line, sizeof(line), "# draw_timing=%s\r\n",
             detail::drawTiming ? "1" : "0");
    appendLog(line);
    int n = snprintf(line, sizeof(line), "frame,ms");
    for (unsigned i = 0; i < PhaseCount && n > 0 && n < (int)sizeof(line); ++i)
        n += snprintf(line + n, sizeof(line) - n, ",%s_ms", kPhaseNames[i]);
    for (unsigned i = 0; i < GpuPhaseCount && n > 0 && n < (int)sizeof(line); ++i)
        n += snprintf(line + n, sizeof(line) - n, ",%s_ms", kGpuNames[i]);
    for (unsigned i = 0; i < CounterCount && n > 0 && n < (int)sizeof(line); ++i)
        n += snprintf(line + n, sizeof(line) - n, ",%s", kCounterNames[i]);
    if (n > 0 && n < (int)sizeof(line))
        snprintf(line + n, sizeof(line) - n, ",unusual\r\n");
    appendLog(line);
}

// ---------------------------------------------------------------------------
// Baseline. The median of the preceding frames rather than their mean, so a
// run of consecutive hitches cannot quietly become the thing a hitch is
// compared against.

float medianOf(float* values, unsigned count) {
    if (!count) return 0.0f;
    unsigned middle = count / 2;
    std::nth_element(values, values + middle, values + count);
    return values[middle];
}

float phaseBaseline(unsigned frame, Phase phase, float* scratch) {
    unsigned n = 0;
    for (unsigned back = 1; back <= kBaselineFrames && back <= frame; ++back) {
        FrameRecord* record = recordAt(frame - back);
        if (record) scratch[n++] = record->phaseMs[phase];
    }
    return medianOf(scratch, n);
}

float counterBaseline(unsigned frame, Counter counter, float* scratch) {
    unsigned n = 0;
    for (unsigned back = 1; back <= kBaselineFrames && back <= frame; ++back) {
        FrameRecord* record = recordAt(frame - back);
        if (record) scratch[n++] = (float)record->counters[counter];
    }
    return medianOf(scratch, n);
}

// What was unusual about this frame, most unusual first. This is the whole
// point of the instrument: a hitch row that only says "38 ms" names nothing.
void describeUnusual(const FrameRecord& record, char* out, size_t size,
                     int* topPhase, float* topDelta) {
    out[0] = 0;
    if (topPhase) *topPhase = -1;
    if (topDelta) *topDelta = 0.0f;
    float scratch[kBaselineFrames];
    struct Item { const char* name; float delta; bool integral; };
    Item items[(unsigned)PhaseCount + (unsigned)CounterCount];
    unsigned count = 0;

    for (unsigned i = 0; i < PhaseCount; ++i) {
        float delta = record.phaseMs[i]
                    - phaseBaseline(record.index, (Phase)i, scratch);
        if (delta < 0.5f) continue;
        items[count++] = {kPhaseNames[i], delta, false};
        if (topDelta && delta > *topDelta) {
            *topDelta = delta;
            if (topPhase) *topPhase = (int)i;
        }
    }
    for (unsigned i = 0; i < CounterCount; ++i) {
        float delta = (float)record.counters[i]
                    - counterBaseline(record.index, (Counter)i, scratch);
        if (delta >= 1.0f) items[count++] = {kCounterNames[i], delta, true};
    }
    // Phases and counters are ranked separately below, so the comparison only
    // ever orders like against like.
    std::stable_sort(items, items + count, [](const Item& a, const Item& b) {
        if (a.integral != b.integral) return !a.integral;
        return a.delta > b.delta;
    });

    size_t used = 0;
    unsigned printed = 0;
    for (unsigned i = 0; i < count && printed < 8 && used + 1 < size; ++i) {
        int n = snprintf(out + used, size - used, "%s%s:+%.*f",
                         printed ? " " : "", items[i].name,
                         items[i].integral ? 0 : 2, items[i].delta);
        if (n <= 0 || (size_t)n >= size - used) break;
        used += (size_t)n;
        ++printed;
    }
}

void emitRecord(const FrameRecord& record, bool flushWhenFull = false) {
    char line[8192];
    int n = snprintf(line, sizeof(line), "%u,%.3f", record.index,
                     record.milliseconds);
    for (unsigned i = 0; i < PhaseCount && n > 0 && n < (int)sizeof(line); ++i)
        n += snprintf(line + n, sizeof(line) - n, ",%.3f", record.phaseMs[i]);
    for (unsigned i = 0; i < GpuPhaseCount && n > 0 && n < (int)sizeof(line); ++i) {
        if (record.gpuResolved)
            n += snprintf(line + n, sizeof(line) - n, ",%.3f", record.gpuMs[i]);
        else
            n += snprintf(line + n, sizeof(line) - n, ",");
    }
    for (unsigned i = 0; i < CounterCount && n > 0 && n < (int)sizeof(line); ++i)
        n += snprintf(line + n, sizeof(line) - n, ",%u", record.counters[i]);
    char unusual[512];
    unusual[0] = 0;
    int topPhase = -1;
    float topDelta = 0.0f;
    // Only a hitch row earns the baseline pass: computing 39 medians-of-60 on
    // the render thread for every ordinary frame of a full-mode run would put
    // the instrument inside its own measurements.
    if (record.milliseconds > g_hitchMs)
        describeUnusual(record, unusual, sizeof(unusual), &topPhase, &topDelta);
    if (n > 0 && n < (int)sizeof(line))
        snprintf(line + n, sizeof(line) - n, ",%s\r\n", unusual);
    if (flushWhenFull && g_logBytes + strlen(line) + 512 >= kLogBytes)
        flushLog();
    appendLog(line);

    // Remembered for the overlay, so the panel does no ranking of its own.
    if (record.milliseconds > g_hitchMs && topPhase >= 0) {
        g_lastHitchPhase = topPhase;
        g_lastHitchDelta = topDelta;
    }
}

void emitDue() {
    if (!g_records || g_frameIndex <= kEmitDelay) return;
    unsigned limit = g_frameIndex - kEmitDelay;
    while (g_emitCursor < limit) {
        FrameRecord* record = recordAt(g_emitCursor);
        if (!record) { ++g_emitCursor; continue; }
        // Hold the row while its GPU timings might still land, but never at the
        // cost of the row itself.
        if (!record->gpuResolved && g_gpuReady
            && g_frameIndex - g_emitCursor < kGpuPatience)
            return;
        if (record->gpuResolved) ++g_gpuResolvedFrames;
        else if (g_gpuReady) ++g_gpuTimedOutFrames;
        ++g_emitCursor;
        if (g_mode == ModeFull || record->milliseconds > g_hitchMs) {
            writeHeader();
            emitRecord(*record);
        }
    }
}

// ---------------------------------------------------------------------------
// GPU regions.

void resolveSlot(ID3D11DeviceContext* context, GpuSlot& slot) {
    if (!slot.pending || !slot.disjoint) return;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint = {};
    if (context->GetData(slot.disjoint, &disjoint, sizeof(disjoint),
                         D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
        return;
    FrameRecord* record = recordAt(slot.frameIndex);
    for (unsigned i = 0; i < GpuPhaseCount; ++i) {
        if (!slot.opened[i] || !slot.closed[i]) continue;
        UINT64 begin = 0, end = 0;
        if (context->GetData(slot.begin[i], &begin, sizeof(begin),
                             D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK
            || context->GetData(slot.end[i], &end, sizeof(end),
                                D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            continue;
        if (disjoint.Disjoint || !disjoint.Frequency || end < begin) continue;
        if (record) {
            record->gpuMs[i] = (float)((double)(end - begin) * 1000.0
                                     / (double)disjoint.Frequency);
            record->gpuResolved = true;
        }
    }
    slot.pending = false;
    memset(slot.opened, 0, sizeof(slot.opened));
    memset(slot.closed, 0, sizeof(slot.closed));
}

}  // namespace

// ---------------------------------------------------------------------------

void readOptions(const wchar_t* iniPath) {
    detail::active = false;
    detail::drawTiming = false;
    g_mode = ModeOff;
    g_drawTimingRequested = false;
    if (!iniPath) return;
    wchar_t value[32];
    GetPrivateProfileStringW(L"debug", L"performance_trace", L"0", value, 32,
                             iniPath);
    // 1 is the leave-it-on mode: one row per hitching frame, a few hundred
    // bytes a minute. `full` records every frame for a measurement session.
    g_mode = (!_wcsicmp(value, L"full") || !_wcsicmp(value, L"2")) ? ModeFull
           : (!_wcsicmp(value, L"1") || !_wcsicmp(value, L"on")
              || !_wcsicmp(value, L"hitch")) ? ModeHitch : ModeOff;
    if (g_mode == ModeOff) return;

    // A clock pair on a hook that runs 1500-2700 times a frame, so it is asked
    // for rather than assumed. It cannot arm without the probe: there is no
    // frame record to add a phase to.
    GetPrivateProfileStringW(L"debug", L"draw_timing", L"0", value, 32,
                             iniPath);
    g_drawTimingRequested = !_wcsicmp(value, L"1") || !_wcsicmp(value, L"on");

    if (!g_csvPath[0]) {
        // Beside the executable, like every other file this mod writes.
        wchar_t path[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        wchar_t* slash = n && n < MAX_PATH ? wcsrchr(path, L'\\') : nullptr;
        if (!slash) { g_mode = ModeOff; return; }
        lstrcpyW(slash + 1, L"tqflicker-frames.csv");
        lstrcpynW(g_csvPath, path, MAX_PATH);
    }

    g_records = (FrameRecord*)calloc(kRecordCount, sizeof(FrameRecord));
    if (!g_records) { g_mode = ModeOff; return; }
    QueryPerformanceFrequency(&g_frequency);
    if (!g_frequency.QuadPart) {
        free(g_records);
        g_records = nullptr;
        g_mode = ModeOff;
        return;
    }
    memset(&g_current, 0, sizeof(g_current));
    ensureWriter();
    detail::active = true;
    detail::drawTiming = g_drawTimingRequested;
}

void setOutputPath(const wchar_t* csvPath) {
    if (!csvPath) return;
    lstrcpynW(g_csvPath, csvPath, MAX_PATH);
    g_headerWritten = false;
}

bool logsEveryFrame() { return g_mode == ModeFull; }

int64_t now() {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

void addPhaseInternal(Phase phase, int64_t startTicks) {
    if (!detail::active || !startTicks || phase >= PhaseCount) return;
    if (g_renderThread && GetCurrentThreadId() != g_renderThread) return;
    int64_t elapsed = now() - startTicks;
    if (elapsed <= 0) return;
    g_current.phaseMs[phase] +=
        (float)((double)elapsed * 1000.0 / (double)g_frequency.QuadPart);
}

void countInternal(Counter counter, uint32_t amount) {
    if (!detail::active || counter >= CounterCount) return;
    if (g_renderThread && GetCurrentThreadId() != g_renderThread) return;
    g_current.counters[counter] += amount;
}

bool isRenderThread() {
    return !g_renderThread || GetCurrentThreadId() == g_renderThread;
}

void engineCountInternal(Counter counter, uint32_t amount) {
    if (!detail::active || counter >= CounterCount || !amount) return;
    InterlockedExchangeAdd(&g_engineCounters[counter], (LONG)amount);
}

uint32_t microsecondsSince(int64_t startTicks) {
    if (!detail::active || !startTicks || !g_frequency.QuadPart) return 0;
    int64_t elapsed = now() - startTicks;
    if (elapsed <= 0) return 0;
    double microseconds =
        (double)elapsed * 1000000.0 / (double)g_frequency.QuadPart;
    // Saturating rather than wrapping: a column that reads 4294967295 says
    // "longer than this can express", where a wrapped one would read as a fast
    // load and quietly hide the worst case in the file.
    if (microseconds >= 4294967295.0) return 0xffffffffu;
    return (uint32_t)microseconds;
}

bool createResources(ID3D11Device* device) {
    if (!detail::active || !device || g_gpuReady) return g_gpuReady != 0;
    D3D11_QUERY_DESC disjoint = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    D3D11_QUERY_DESC stamp = {D3D11_QUERY_TIMESTAMP, 0};
    bool ok = true;
    for (unsigned slot = 0; slot < kGpuSlotCount && ok; ++slot) {
        ok &= SUCCEEDED(device->CreateQuery(&disjoint, &g_gpu[slot].disjoint));
        for (unsigned i = 0; i < GpuPhaseCount && ok; ++i) {
            ok &= SUCCEEDED(device->CreateQuery(&stamp, &g_gpu[slot].begin[i]));
            ok &= SUCCEEDED(device->CreateQuery(&stamp, &g_gpu[slot].end[i]));
        }
    }
    if (!ok) { releaseResources(); return false; }
    InterlockedExchange(&g_gpuReady, 1);
    return true;
}

void releaseResources() {
    InterlockedExchange(&g_gpuReady, 0);
    g_gpuCurrent = nullptr;
    g_gpuContext = nullptr;
    for (unsigned slot = 0; slot < kGpuSlotCount; ++slot) {
        if (g_gpu[slot].disjoint) g_gpu[slot].disjoint->Release();
        g_gpu[slot].disjoint = nullptr;
        for (unsigned i = 0; i < GpuPhaseCount; ++i) {
            if (g_gpu[slot].begin[i]) g_gpu[slot].begin[i]->Release();
            if (g_gpu[slot].end[i]) g_gpu[slot].end[i]->Release();
            g_gpu[slot].begin[i] = nullptr;
            g_gpu[slot].end[i] = nullptr;
        }
        g_gpu[slot].pending = false;
    }
}

void gpuBegin(ID3D11DeviceContext* context, GpuPhase phase) {
    if (!detail::active || !g_gpuReady || !context || !g_gpuCurrent) return;
    if (phase >= GpuPhaseCount || g_gpuCurrent->opened[phase]) return;
    context->End(g_gpuCurrent->begin[phase]);
    g_gpuCurrent->opened[phase] = true;
}

void gpuEnd(ID3D11DeviceContext* context, GpuPhase phase) {
    if (!detail::active || !g_gpuReady || !context || !g_gpuCurrent) return;
    if (phase >= GpuPhaseCount || !g_gpuCurrent->opened[phase]) return;
    // A timestamp query records the most recent End, so a region entered and
    // left several times in one frame -- the eight point-shadow passes, say --
    // measures from its first entry to its last exit. That span includes
    // whatever ran between them, which the CSV column name has to admit.
    context->End(g_gpuCurrent->end[phase]);
    g_gpuCurrent->closed[phase] = true;
}

void beginFrame(ID3D11DeviceContext* context) {
    if (!detail::active) return;
    if (!g_gpuReady || !context) return;
    for (unsigned i = 0; i < kGpuSlotCount; ++i) resolveSlot(context, g_gpu[i]);
    GpuSlot& slot = g_gpu[g_gpuCursor % kGpuSlotCount];
    // Still waiting on the GPU: skip a frame rather than discard a pending
    // result or stall waiting for it.
    if (slot.pending) { g_gpuCurrent = nullptr; return; }
    ++g_gpuCursor;
    slot.frameIndex = g_frameIndex;
    memset(slot.opened, 0, sizeof(slot.opened));
    memset(slot.closed, 0, sizeof(slot.closed));
    context->Begin(slot.disjoint);
    g_gpuCurrent = &slot;
    g_gpuContext = context;
    gpuBegin(context, GpuFrame);
}

void endFrame(float cpuFrameMilliseconds) {
    if (!detail::active || !g_records) return;
    g_renderThread = GetCurrentThreadId();
    // Fold the game's threads in first, so their counts belong to the frame
    // that is closing rather than to whichever one happens to close next. The
    // exchange is what makes this exact: a write landing between the read and
    // the store is not lost, it is carried into the following frame.
    for (unsigned i = 0; i < CounterCount; ++i) {
        LONG pending = InterlockedExchange(&g_engineCounters[i], 0);
        if (pending > 0) g_current.counters[i] += (uint32_t)pending;
    }
    if (g_gpuCurrent) {
        // The disjoint query has to be ended, not merely begun. Without this
        // GetData never returns S_OK for it, and since every timestamp in the
        // slot is gated on the disjoint result, the whole GPU side reports
        // nothing at all -- which is exactly what two full runs did.
        if (g_gpuContext) {
            gpuEnd(g_gpuContext, GpuFrame);
            g_gpuContext->End(g_gpuCurrent->disjoint);
        }
        g_gpuCurrent->pending = true;
        g_gpuCurrent = nullptr;
    }
    // Two scopes nest inside others, and a nested phase left inclusive would
    // double-count and outrank its parent: the overlay's rasterisation runs
    // inside its draw scope, and PhasePresent brackets the whole callback
    // containing grass, streaming and the overlay. Each column is therefore
    // made exclusive here, once, so every reader downstream can simply sum.
    float raster = g_current.phaseMs[PhaseOverlayRaster];
    g_current.phaseMs[PhaseOverlayDraw] =
        g_current.phaseMs[PhaseOverlayDraw] > raster
            ? g_current.phaseMs[PhaseOverlayDraw] - raster : 0.0f;
    float nested = g_current.phaseMs[PhaseGrassPresent]
                 + g_current.phaseMs[PhaseStreamStep]
                 + g_current.phaseMs[PhaseOverlayDraw]
                 + raster;
    g_current.phaseMs[PhasePresent] =
        g_current.phaseMs[PhasePresent] > nested
            ? g_current.phaseMs[PhasePresent] - nested : 0.0f;
    g_current.index = g_frameIndex;
    g_current.milliseconds = cpuFrameMilliseconds;
    g_records[g_frameIndex % kRecordCount] = g_current;
    ++g_frameIndex;
    memset(&g_current, 0, sizeof(g_current));
    emitDue();
}

void summarize(char* out, size_t size) {
    if (!out || !size) return;
    out[0] = 0;
    if (!detail::active || !g_records || !g_frameIndex) return;
    unsigned window = g_frameIndex < kRecordCount ? g_frameIndex : kRecordCount;
    double totals[PhaseCount] = {};
    for (unsigned back = 1; back <= window; ++back) {
        FrameRecord* record = recordAt(g_frameIndex - back);
        if (!record) continue;
        for (unsigned i = 0; i < PhaseCount; ++i) totals[i] += record->phaseMs[i];
    }
    unsigned order[PhaseCount];
    for (unsigned i = 0; i < PhaseCount; ++i) order[i] = i;
    std::stable_sort(order, order + PhaseCount, [&](unsigned a, unsigned b) {
        return totals[a] > totals[b];
    });
    size_t used = 0;
    for (unsigned i = 0; i < 3 && used + 1 < size; ++i) {
        double mean = totals[order[i]] / window;
        if (mean < 0.005) break;
        int n = snprintf(out + used, size - used, "%s%s %.2f",
                         used ? "  " : "", kPhaseShortNames[order[i]], mean);
        if (n <= 0 || (size_t)n >= size - used) break;
        used += (size_t)n;
    }
    if (g_lastHitchPhase >= 0 && used + 1 < size)
        snprintf(out + used, size - used, "%sLAST %s %.1f", used ? "  " : "",
                 kPhaseShortNames[g_lastHitchPhase], g_lastHitchDelta);
}

// Writes everything the ring still holds, then the run summary. One policy
// for what a teardown writes, shared by both teardown paths so they cannot
// drift; `flushAsItGoes` keeps the buffer from overflowing when no worker is
// draining it.
void drainRing(bool flushAsItGoes) {
    if (g_records) {
        while (g_emitCursor < g_frameIndex) {
            FrameRecord* record = recordAt(g_emitCursor);
            ++g_emitCursor;
            if (!record) continue;
            if (g_mode == ModeFull || record->milliseconds > g_hitchMs) {
                writeHeader();
                emitRecord(*record, flushAsItGoes);
            }
        }
    }
    if (g_headerWritten) {
        char line[192];
        snprintf(line, sizeof(line),
                 "# gpu timings: %u frames resolved, %u timed out; "
                 "%u rows dropped\r\n",
                 g_gpuResolvedFrames, g_gpuTimedOutFrames, g_logDropped);
        appendLogReserved(line, 0);
    }
}

void flushOnExit() {
    if (!detail::active || !g_log) return;
    // At process exit there is no later frame to wait for, no worker to
    // signal, and no other thread left alive -- but one may have been killed
    // while holding g_logLock, so from here on nothing takes it.
    g_exiting = true;
    drainRing(true);
    flushLog();
}

void shutdown() {
    if (g_logStarted) {
        // Anything the ring still holds belongs in the file: a session that is
        // being closed is exactly the one whose tail matters.
        drainRing(false);
        if (g_logStop) SetEvent(g_logStop);
        if (g_logThread) {
            WaitForSingleObject(g_logThread, 2000);
            CloseHandle(g_logThread);
            g_logThread = nullptr;
        }
        flushLog();
        if (g_logStop) { CloseHandle(g_logStop); g_logStop = nullptr; }
        if (g_logFlush) { CloseHandle(g_logFlush); g_logFlush = nullptr; }
        free(g_log);
        g_log = nullptr;
        InterlockedExchange(&g_logStarted, 0);
    }
    detail::active = false;
    g_mode = ModeOff;
    free(g_records);
    g_records = nullptr;
    for (unsigned i = 0; i < CounterCount; ++i)
        InterlockedExchange(&g_engineCounters[i], 0);
    g_frameIndex = g_emitCursor = 0;
    g_renderThread = 0;
    g_logBytes = g_logDropped = 0;
    g_headerWritten = false;
    g_exiting = false;
    g_gpuResolvedFrames = g_gpuTimedOutFrames = 0;
    g_lastHitchPhase = -1;
    g_lastHitchDelta = 0.0f;
}

#ifdef TQ_SELFTEST
unsigned frameCountForTest() { return g_frameIndex; }

float phaseMillisecondsForTest(unsigned framesBack, Phase phase) {
    FrameRecord* record = framesBack < g_frameIndex
                        ? recordAt(g_frameIndex - 1 - framesBack) : nullptr;
    return record && phase < PhaseCount ? record->phaseMs[phase] : -1.0f;
}

bool gpuResolvedForTest(unsigned framesBack) {
    FrameRecord* record = framesBack < g_frameIndex
                        ? recordAt(g_frameIndex - 1 - framesBack) : nullptr;
    return record && record->gpuResolved;
}

uint32_t counterForTest(unsigned framesBack, Counter counter) {
    FrameRecord* record = framesBack < g_frameIndex
                        ? recordAt(g_frameIndex - 1 - framesBack) : nullptr;
    return record && counter < CounterCount ? record->counters[counter] : 0u;
}

float phaseForTest(unsigned framesBack, Phase phase) {
    FrameRecord* record = framesBack < g_frameIndex
                        ? recordAt(g_frameIndex - 1 - framesBack) : nullptr;
    return record && phase < PhaseCount ? record->phaseMs[phase] : 0.0f;
}

void resetForTest() {
    shutdown();
    g_csvPath[0] = 0;
}
#endif

}  // namespace probe
}  // namespace tq
