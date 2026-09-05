#include "renderer_draw.h"
#include "renderer_draw_sites.h"
#include "detour.h"
#include "hdr.h"

#include <stdint.h>
#include <string.h>

namespace tq { namespace rendererdraw {
namespace {
struct Patch {
    BYTE* address;
    BYTE original[sites::kPatchSize];
    BYTE replacement[sites::kPatchSize];
};
Patch g_patches[2];
HMODULE g_renderer;
DrawFn g_draw;
DrawIndexedFn g_indexed;

bool matchesWindow(HMODULE module, DWORD rva, const BYTE* bytes, SIZE_T size) {
    BYTE* text = nullptr;
    SIZE_T textSize = 0;
    if (!tq::detour::moduleText(module, &text, &textSize)) return false;
    const uintptr_t begin = (uintptr_t)text;
    const uintptr_t end = begin + textSize;
    const uintptr_t at = (uintptr_t)module + rva;
    if (end < begin || at < begin || at > end || size > end - at) return false;
    tq::detour::Signature signature = {bytes, size, nullptr, 0};
    return tq::detour::matches(module, (void*)at, signature);
}

bool patch(Patch& record, BYTE* address, const void* callback) {
    memcpy(record.original, address, sites::kPatchSize);
    // mov ecx,[eax]; push count; push eax; call [ecx+slot]
    // becomes push count; push eax; call callback (same seven-byte extent).
    record.replacement[0] = record.original[2];
    record.replacement[1] = record.original[3];
    record.replacement[2] = 0xe8;
    const uint32_t relative = (uint32_t)(uintptr_t)callback
        - (uint32_t)(uintptr_t)(address + sites::kPatchSize);
    memcpy(record.replacement + 3, &relative, sizeof(relative));
    if (!tq::detour::writeBytes(address, record.original, record.replacement,
                               sites::kPatchSize)) return false;
    record.address = address;
    return true;
}
}  // namespace

bool installed() { return g_renderer != nullptr; }

void shutdown() {
    for (int i = 1; i >= 0; --i) {
        Patch& record = g_patches[i];
        if (record.address)
            tq::detour::writeBytes(record.address, record.replacement,
                                   record.original, sites::kPatchSize);
        record = {};
    }
    g_renderer = nullptr;
    g_draw = nullptr;
    g_indexed = nullptr;
}

bool install(HMODULE renderer, DrawFn draw, DrawIndexedFn indexed) {
    if (g_renderer)
        return renderer == g_renderer && draw == g_draw && indexed == g_indexed;
    // Validate both complete windows before making either callback reachable.
    if (!renderer || !draw || !indexed
        || !matchesWindow(renderer, sites::kDrawWindowRva, sites::kDrawWindow,
                           sizeof(sites::kDrawWindow))
        || !matchesWindow(renderer, sites::kIndexedWindowRva, sites::kIndexedWindow,
                           sizeof(sites::kIndexedWindow))) {
        tq::hdr::log("Renderer draw hooks rejected: unsupported submission sites\r\n");
        return false;
    }
    if (!patch(g_patches[0], (BYTE*)renderer + sites::kDrawWindowRva
               + sites::kDrawPatchOffset, (void*)draw)
        || !patch(g_patches[1], (BYTE*)renderer + sites::kIndexedWindowRva
                   + sites::kIndexedPatchOffset, (void*)indexed)) {
        shutdown();
        tq::hdr::log("Renderer draw hooks rolled back after patch failure\r\n");
        return false;
    }
    g_renderer = renderer;
    g_draw = draw;
    g_indexed = indexed;
    tq::hdr::log("Renderer draw hooks installed: Draw=%p DrawIndexed=%p\r\n",
                 g_patches[0].address, g_patches[1].address);
    return true;
}
} }
