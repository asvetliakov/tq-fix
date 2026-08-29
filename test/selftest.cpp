#include <windows.h>
#include <d3d11.h>

#include <stdio.h>
#include <stdlib.h>

#include "dxbc_patch.h"

namespace {

FILE* g_report;
int   g_failures;

void check(bool passed, const char* description) {
    fprintf(g_report, "%s  %s\n", passed ? "ok  " : "FAIL", description);
    if (!passed) ++g_failures;
}

void* readFile(const char* path, long* size) {
    *size = 0;
    FILE* file = fopen(path, "rb");
    if (!file) return nullptr;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    void* bytes = length > 0 ? malloc((size_t)length) : nullptr;
    if (!bytes || fread(bytes, 1, (size_t)length, file) != (size_t)length) {
        free(bytes);
        bytes = nullptr;
    } else {
        *size = length;
    }
    fclose(file);
    return bytes;
}

}  // namespace

int main(int argc, char** argv) {
    const char* dll = argc > 1 ? argv[1] : "winmm.dll";
    const char* report = argc > 2 ? argv[2] : "C:\\tqflicker-selftest.txt";
    g_report = fopen(report, "w");
    if (!g_report) return 99;

    HMODULE proxy = LoadLibraryA(dll);
    check(proxy != nullptr, "load the winmm proxy");
    if (proxy) {
        static const char* const names[] = {
#define TQ_WINMM_NAME(name) name,
#include "winmm_names.inc"
#undef TQ_WINMM_NAME
        };
        bool complete = true;
        for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
            if (!GetProcAddress(proxy, names[i])) complete = false;
        check(complete, "all winmm exports are present");

        FARPROC address = GetProcAddress(proxy, "timeGetTime");
        typedef DWORD(WINAPI* TimeGetTimeFn)();
        DWORD before = address ? ((TimeGetTimeFn)address)() : 0;
        Sleep(30);
        DWORD after = address ? ((TimeGetTimeFn)address)() : 0;
        check(address && before && after >= before, "timeGetTime forwards to the real winmm");
    }

    HMODULE host = LoadLibraryA("C:\\tqflicker-selftest\\Direct3D11.dll");
    check(host != nullptr, "load the production-path Direct3D11 host");
    typedef HRESULT (*MakeDeviceFn)(ID3D11Device**, ID3D11DeviceContext**);
    MakeDeviceFn makeDevice = host
        ? (MakeDeviceFn)(void*)GetProcAddress(host, "make_device") : nullptr;
    check(makeDevice != nullptr, "find the host's device-creation entry point");

    // The production DLL polls every 10 ms because the game's renderer is
    // loaded after winmm. Give the same path ample time in this off-game test.
    Sleep(250);
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HRESULT result = makeDevice ? makeDevice(&device, &context) : E_FAIL;
    check(SUCCEEDED(result) && device && context,
          "create a 32-bit DXMT D3D11 device through the hooked host");
    if (device && proxy) {
        void* createVertexShader = (*(void***)device)[12];
        MEMORY_BASIC_INFORMATION info = {};
        bool queried = VirtualQuery(createVertexShader, &info, sizeof(info)) != 0;
        check(queried && info.AllocationBase == proxy,
              "CreateVertexShader is redirected into the minimal proxy");
    }

    int transformed = 0;
    if (device) {
        for (int i = 3; i < argc; ++i) {
            long size;
            void* original = readFile(argv[i], &size);
            tq::dxbc::PatchResult patch = {};
            bool changed = original && tq::dxbc::clampBoneIndices(original, (SIZE_T)size, &patch);
            check(changed && patch.size == (SIZE_T)size + 40,
                  "transform one captured Titan Quest skinning shader");
            if (changed) {
                ID3D11VertexShader* shader = nullptr;
                HRESULT shaderResult = device->CreateVertexShader(
                    patch.data, patch.size, nullptr, &shader);
                check(SUCCEEDED(shaderResult) && shader,
                      "DXMT accepts the transformed shader");
                if (shader) shader->Release();
                ++transformed;
            }
            tq::dxbc::release(&patch);
            free(original);
        }
    }
    if (argc > 3)
        check(transformed == argc - 3, "all captured shader variants were transformed");

    if (context) context->Release();
    if (device) device->Release();
    // The proxy restores its IAT and vtable slots on an explicit unload, so the
    // host must remain mapped until after this call.
    if (proxy) FreeLibrary(proxy);
    if (host) FreeLibrary(host);

    fprintf(g_report, "\nRESULT: %d failure(s)\n", g_failures);
    fclose(g_report);
    return g_failures ? 1 : 0;
}
