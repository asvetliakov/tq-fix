#include "modules.h"

#include <winternl.h>

#include "log.h"

namespace tq {
namespace modules {

namespace {

// The first three fields of LDR_DATA_TABLE_ENTRY, which is all we read. Declared
// here because winternl.h ships a truncated version of it.
struct LdrEntry {
    LIST_ENTRY     InLoadOrderLinks;
    LIST_ENTRY     InMemoryOrderLinks;
    LIST_ENTRY     InInitializationOrderLinks;
    PVOID          DllBase;
    PVOID          EntryPoint;
    ULONG          SizeOfImage;
    UNICODE_STRING FullDllName;
    UNICODE_STRING BaseDllName;
};

/** Is this module one of Windows' own? The test is the path, not the name. */
bool isWindowsOwn(const LdrEntry* e) {
    int n = e->FullDllName.Length / 2;
    if (n < 11) return false;
    const wchar_t* p = e->FullDllName.Buffer;
    if (p[1] != L':' || (p[2] != L'\\' && p[2] != L'/')) return false;
    const wchar_t* w = L"windows";
    for (int i = 0; i < 7; i++) {
        wchar_t c = p[3 + i];
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c + 32);
        if (c != w[i]) return false;
    }
    return p[10] == L'\\' || p[10] == L'/';
}

}  // namespace

bool nap(HANDLE cancel, DWORD ms) {
    if (!cancel) { Sleep(ms); return true; }
    return WaitForSingleObject(cancel, ms) == WAIT_TIMEOUT;
}

HMODULE waitFor(const wchar_t* name, DWORD timeoutMs, HANDLE cancel, DWORD* waitedMs,
                bool* cancelled) {
    // 10ms, and the interval is the whole point of the design. `Engine.dll`
    // loads the renderer and then calls into it; every millisecond between the
    // module appearing and our hook landing is a millisecond in which the
    // device could be created without us. Polling `GetModuleHandleW` at 100Hz
    // costs a list walk in ntdll and nothing else.
    const DWORD kStep = 10;
    DWORD waited = 0;
    if (cancelled) *cancelled = false;
    for (;;) {
        HMODULE m = GetModuleHandleW(name);
        if (m) {
            if (waitedMs) *waitedMs = waited;
            return m;
        }
        if (waited >= timeoutMs) break;
        if (!nap(cancel, kStep)) {
            if (cancelled) *cancelled = true;
            break;
        }
        waited += kStep;
    }
    if (waitedMs) *waitedMs = waited;
    return NULL;
}

void logLoaded(const char* when) {
    PPEB peb = (PPEB)NtCurrentTeb()->ProcessEnvironmentBlock;
    if (!peb || !peb->Ldr) {
        tqlog("modules (%s): no PEB - cannot say what is loaded", when);
        return;
    }
    int total = 0, listed = 0;
    tqlog("modules (%s) - everything that is not from C:\\windows\\:", when);
    LIST_ENTRY* head = &peb->Ldr->InMemoryOrderModuleList;
    for (LIST_ENTRY* it = head->Flink; it && it != head; it = it->Flink) {
        const LdrEntry* e = (const LdrEntry*)((char*)it - sizeof(LIST_ENTRY));
        if (!e->DllBase) continue;
        total++;
        if (isWindowsOwn(e)) continue;
        listed++;
        tqlog("    %-28.*S  base %p  %.*S",
              (int)(e->BaseDllName.Length / 2), e->BaseDllName.Buffer, e->DllBase,
              (int)(e->FullDllName.Length / 2), e->FullDllName.Buffer);
    }
    tqlog("  %d modules loaded, %d of them not Windows' own", total, listed);
}

}  // namespace modules
}  // namespace tq
