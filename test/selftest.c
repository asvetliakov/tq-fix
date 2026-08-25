/*
 * Off-game self-test: load our proxy and prove it forwards, with no game.
 *
 * Built for 32-bit Windows and run inside the bottle by
 * scripts/selftest-offgame.sh. It writes its findings to a file rather than
 * stdout, because a console under `cxstart` is not reliably capturable.
 *
 * What it proves, in order of what would hurt most if it were wrong:
 *
 *   1. Our winmm.dll loads at all as a 32-bit module.
 *   2. It is OUR module and not the system's - checked by the log line and by
 *      the module path, so a test that silently exercised the real winmm cannot
 *      pass.
 *   3. `timeGetTime` forwards and returns a real tick count, twice, increasing.
 *      This is the call the game makes constantly; if the stub mechanism were
 *      wrong it would hang or return zero here rather than in the game.
 *   4. Every one of the 186 exports resolves through GetProcAddress.
 */
#include <windows.h>
#include <stdio.h>
#include <float.h>
#include <d3d11.h>

static FILE* out;
static int   fails;

static void ck(int cond, const char* what) {
    fprintf(out, "%s  %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) fails++;
}

int main(int argc, char** argv) {
    const char* dll = (argc > 1) ? argv[1] : "winmm.dll";
    const char* rep = (argc > 2) ? argv[2] : "C:\\tqflicker-selftest.txt";

    out = fopen(rep, "w");
    if (!out) return 99;
    fprintf(out, "tqflicker off-game self-test\ndll: %s\n\n", dll);

    HMODULE m = LoadLibraryA(dll);
    ck(m != NULL, "LoadLibrary(our winmm.dll)");
    if (!m) {
        fprintf(out, "\nGetLastError=%lu\n", (unsigned long)GetLastError());
        fprintf(out, "RESULT: %d failure(s)\n", fails);
        fclose(out);
        return 1;
    }

    char path[MAX_PATH] = "";
    GetModuleFileNameA(m, path, MAX_PATH);
    fprintf(out, "loaded from: %s\n", path);
    /* If this loaded the system winmm instead of ours the whole test is
       meaningless, so make that a failure rather than a silent pass. */
    ck(strstr(path, "syswow64") == NULL && strstr(path, "system32") == NULL,
       "loaded OUR winmm, not the system's");

    FARPROC tgt = GetProcAddress(m, "timeGetTime");
    ck(tgt != NULL, "GetProcAddress(timeGetTime)");
    if (tgt) {
        DWORD (WINAPI *fn)(void) = (DWORD (WINAPI*)(void))tgt;
        DWORD a = fn();
        Sleep(30);
        DWORD b = fn();
        fprintf(out, "timeGetTime: %lu then %lu\n", (unsigned long)a, (unsigned long)b);
        /* Zero is what an unresolved slot returns, so it is the specific
           failure this is looking for. */
        ck(a != 0, "timeGetTime forwarded (non-zero: not the unresolved stub)");
        ck(b >= a, "timeGetTime advances");
    }

    /* Every export must resolve. A missing one is not cosmetic here: on i386 an
       unresolved slot that is actually called corrupts the stack, because the
       fallback cannot know how many bytes a __stdcall callee should pop. See
       the note on tq_winmm_unresolved in src/winmm_proxy.cpp. */
    static const char* const names[] = {
#define TQ_WINMM_NAME(n) n,
#include "winmm_names.inc"
#undef TQ_WINMM_NAME
    };
    int n = (int)(sizeof(names) / sizeof(names[0]));
    int missing = 0;
    const char* first = NULL;
    for (int i = 0; i < n; i++) {
        if (!GetProcAddress(m, names[i])) {
            if (!first) first = names[i];
            missing++;
        }
    }
    fprintf(out, "exports: %d checked, %d missing\n", n, missing);
    if (missing) fprintf(out, "first missing: %s\n", first);
    ck(missing == 0, "all exports resolve through our module");

    /* Stage 4: the vtable patches, exercised through DXMT with no game.
     *
     * This exe imports d3d11.dll statically, and the DLL was told
     * (TQFLICKER_D3D_HOST=selftest.exe) to hook THIS module's import of
     * D3D11CreateDeviceAndSwapChain instead of Direct3D11.dll's. So the device
     * created below goes through the same hook the game's does, gets the same
     * vtable patches, and the calls after it land in the same hooks - under
     * FEX, through the same 32-bit DXMT. What it cannot do is draw: there is
     * no pipeline here, and a Draw with nothing bound would test DXMT's
     * tolerance, not our hook. Present, CreateBuffer, Map, CreateSamplerState
     * are enough to prove the mechanism on every one of its shapes
     * (swapchain, context, device vtables). */
    Sleep(600);   /* the watcher thread installs the IAT hook asynchronously */
    {
        WNDCLASSA wc = {0};
        wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = "tqflicker-selftest";
        RegisterClassA(&wc);
        HWND hwnd = CreateWindowA("tqflicker-selftest", "tqflicker self-test", WS_OVERLAPPEDWINDOW,
                                  0, 0, 320, 240, NULL, NULL, wc.hInstance, NULL);
        ck(hwnd != NULL, "a window for the swapchain");

        DXGI_SWAP_CHAIN_DESC sd = {0};
        sd.BufferCount = 1; sd.BufferDesc.Width = 320; sd.BufferDesc.Height = 240;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = hwnd;
        sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0, got = 0;
        IDXGISwapChain* sc = NULL; ID3D11Device* dev = NULL; ID3D11DeviceContext* ctx = NULL;
        HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, &want, 1,
                                                   D3D11_SDK_VERSION, &sd, &sc, &dev, &got, &ctx);
        fprintf(out, "D3D11CreateDeviceAndSwapChain: hr 0x%08lx, feature level 0x%x\n",
                (unsigned long)hr, (unsigned)got);
        ck(SUCCEEDED(hr) && dev && ctx && sc, "a DXMT device, context and swapchain");
        if (SUCCEEDED(hr)) {
            /* a constant buffer, dynamic, then Map it a few times */
            D3D11_BUFFER_DESC bd = {0};
            bd.ByteWidth = 256; bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            ID3D11Buffer* cb = NULL;
            hr = dev->lpVtbl->CreateBuffer(dev, &bd, NULL, &cb);
            ck(SUCCEEDED(hr) && cb, "CreateBuffer(256-byte dynamic constant buffer) through the hook");

            /* the sampler DXMT warns about (O2): border colour -FLT_MAX */
            D3D11_SAMPLER_DESC sm = {0};
            sm.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
            sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
            sm.BorderColor[0] = -FLT_MAX; sm.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
            sm.MaxLOD = D3D11_FLOAT32_MAX;
            ID3D11SamplerState* ss = NULL;
            hr = dev->lpVtbl->CreateSamplerState(dev, &sm, &ss);
            ck(SUCCEEDED(hr) && ss, "CreateSamplerState(BORDER, -FLT_MAX) through the hook");

            int presented = 0, mapped = 0;
            for (int i = 0; i < 5; i++) {
                if (cb) {
                    D3D11_MAPPED_SUBRESOURCE m;
                    if (SUCCEEDED(ctx->lpVtbl->Map(ctx, (ID3D11Resource*)cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                        memset(m.pData, 0, 256);
                        ctx->lpVtbl->Unmap(ctx, (ID3D11Resource*)cb, 0);
                        mapped++;
                    }
                }
                if (SUCCEEDED(sc->lpVtbl->Present(sc, 1, 0))) presented++;
            }
            fprintf(out, "presented %d, mapped %d\n", presented, mapped);
            ck(presented == 5, "5 x Present through the hook");
            ck(mapped == 5, "5 x Map through the hook");

            if (ss) ss->lpVtbl->Release(ss);
            if (cb) cb->lpVtbl->Release(cb);
            ctx->lpVtbl->Release(ctx); sc->lpVtbl->Release(sc); dev->lpVtbl->Release(dev);
        }
        if (hwnd) DestroyWindow(hwnd);
    }

    /* Does FreeLibrary actually unload us here?
     *
     * Reported, not asserted: it is a fact about this substrate, not about our
     * correctness. It matters because DLL_PROCESS_DETACH takes two different
     * paths - an orderly unload puts every patch back, and process exit
     * deliberately does less (touching another module's memory while the address
     * space is being torn down is a crash on quit). If FreeLibrary never
     * unloads here, the orderly path never runs in this bottle, and a future
     * session should know that before it trusts the unpatch. */
    FreeLibrary(m);
    {
        char after[MAX_PATH] = "";
        DWORD got = GetModuleFileNameA(m, after, MAX_PATH);
        fprintf(out, "after FreeLibrary: %s\n",
                got ? "STILL LOADED (the orderly-detach path did not run)" : "unloaded");
    }
    fprintf(out, "\nRESULT: %d failure(s)\n", fails);
    fclose(out);
    return fails ? 1 : 0;
}
