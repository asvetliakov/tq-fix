#pragma once
#include <windows.h>

namespace tq { namespace resourcetrace {
void readOptions(const wchar_t* path);
bool install(HMODULE engine);
void shutdown();
bool enabled();
const void* setRenderOwner(const void* owner);
struct RenderScope {
    bool active;
    const void* prior;
    explicit RenderScope(const void* instance) : active(enabled()), prior(nullptr) {
        if (active) prior = setRenderOwner(*(void* const*)((const BYTE*)instance + 4));
    }
    ~RenderScope() { if (active) setRenderOwner(prior); }
};
unsigned beginDemand(const void* resource);
void finishDemand(unsigned ticket, unsigned elapsedUs);
void enqueue(const void* resource, int priority, int dependencies, int touch,
             unsigned stateBefore, bool queuedBefore, unsigned untilBefore, const void* caller);
void report();
#ifdef TQ_SELFTEST
bool test();
#endif
} }
