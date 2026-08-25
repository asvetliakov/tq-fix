#include "log.h"

#include <stdarg.h>
#include <string.h>

namespace tq {
namespace {

HANDLE  g_file = INVALID_HANDLE_VALUE;
wchar_t g_path[MAX_PATH] = L"(not open)";
bool    g_verbose = false;

// **Every line carries its pid, and it is stamped here so no caller can forget.**
//
// O17: `TQ.exe` runs as two processes and our DLL is loaded into both, so this
// one file has two writers. Without a pid on every line the log is ambiguous,
// and an ambiguous log becomes a wrong fact in docs/rev/ — which is exactly how
// O16 came to record "two feature-level blocks" as unexplained when it was
// simply two processes. Stage 4 will interleave per-frame draw counts from both;
// that log without a pid would be worse than no log at all.
DWORD g_pid;

// One lock around the write. Stage 4 hooks `Present` and the `Draw*` family,
// which D3D11 permits on more than one thread; two threads interleaving inside
// one `WriteFile` would produce a line that is a lie, and a lie in this file
// becomes a wrong fact in docs/rev/.
CRITICAL_SECTION g_lock;
bool             g_lockReady = false;

/** Try to open `path`; returns true and keeps it on success. */
bool tryOpen(const wchar_t* path) {
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    g_file = h;
    lstrcpynW(g_path, path, MAX_PATH);
    SetFilePointer(h, 0, NULL, FILE_END);
    return true;
}

void write(const char* s, int n) {
    if (g_file == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(g_file, s, (DWORD)n, &wrote, NULL);
    // Flushed on every line, deliberately. A buffered log loses exactly the
    // lines that matter when the process dies.
    FlushFileBuffers(g_file);
}

void emit(const char* fmt, va_list ap) {
    char line[1024];

    SYSTEMTIME t;
    GetLocalTime(&t);
    int n = _snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d  p%-5lu  ", t.wHour, t.wMinute,
                      t.wSecond, t.wMilliseconds, (unsigned long)g_pid);
    if (n < 0 || n >= (int)sizeof(line)) return;

    int m = _vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
    if (m < 0) m = (int)strlen(line + n);  // truncated; keep what we have
    n += m;
    if (n > (int)sizeof(line) - 2) n = (int)sizeof(line) - 2;
    line[n++] = '\r';
    line[n++] = '\n';

    if (g_lockReady) EnterCriticalSection(&g_lock);
    write(line, n);
    if (g_lockReady) LeaveCriticalSection(&g_lock);
}

}  // namespace

void logOpen(HINSTANCE self) {
    g_pid = GetCurrentProcessId();
    if (!g_lockReady) {
        InitializeCriticalSection(&g_lock);
        g_lockReady = true;
    }
    if (g_file != INVALID_HANDLE_VALUE) return;

    wchar_t path[MAX_PATH];

    // 1. An explicit override, which is how the off-game self-test reads the log
    //    without touching the developer's own.
    DWORD n = GetEnvironmentVariableW(L"TQFLICKER_LOG", path, MAX_PATH);
    if (n > 0 && n < MAX_PATH && tryOpen(path)) return;

    // 2. %TEMP%. This is the one every script and tool here reads.
    n = GetTempPathW(MAX_PATH, path);
    if (n > 0 && n < MAX_PATH - 20) {
        lstrcatW(path, L"tqflicker.log");
        if (tryOpen(path)) return;
    }

    // 3. Beside the DLL, if %TEMP% was somehow not writable.
    n = GetModuleFileNameW(self, path, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash && (slash - path) + 20 < MAX_PATH) {
            *(slash + 1) = 0;
            lstrcatW(path, L"tqflicker.log");
            if (tryOpen(path)) return;
        }
    }
}

void logClose() {
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    // The critical section is deliberately not deleted. At DLL_PROCESS_DETACH
    // another thread may still be inside a hook we installed, and deleting a
    // lock somebody is holding is a crash on the way out — which would look
    // exactly like our patches having broken the game.
}

const wchar_t* logPath() { return g_path; }

void logSetVerbose(bool on) { g_verbose = on; }
bool logIsVerbose() { return g_verbose; }

void tqlog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit(fmt, ap);
    va_end(ap);
}

void tqtrace(const char* fmt, ...) {
    if (!g_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    emit(fmt, ap);
    va_end(ap);
}

}  // namespace tq
