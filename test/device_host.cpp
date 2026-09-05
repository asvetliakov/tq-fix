#include <windows.h>
#include <d3d11.h>
#include <string.h>
#include "renderer_draw_sites.h"

// Leave executable padding at the audited game RVAs. The fixture verifies it
// is padding before writing, so linker changes cannot overwrite real code.
asm(".text\n"
    ".space 0x60000, 0xcc\n");

extern "C" {
void* fixture_draw_entry;
void* fixture_indexed_entry;
}

extern "C" __declspec(dllexport) BOOL prepare_draw_sites() {
    static bool ready;
    if (ready) return TRUE;
    using namespace tq::rendererdraw::sites;
    BYTE* module = (BYTE*)GetModuleHandleW(L"Direct3D11.dll");
    if (!module) return FALSE;
    struct Window { DWORD rva; const BYTE* bytes; SIZE_T size; };
    const Window windows[] = {
        {kDrawWindowRva, kDrawWindow, sizeof(kDrawWindow)},
        {kIndexedWindowRva, kIndexedWindow, sizeof(kIndexedWindow)}
    };
    // These snippets run after the synthetic caller saves EBX, ESI, EDI.
    const BYTE finish[] = {0x5f, 0x5e, 0x5b, 0xc3};
    for (const Window& w : windows) {
        for (SIZE_T i = 0; i < w.size + sizeof(finish); ++i)
            if (module[w.rva + i] != 0xcc) return FALSE;
    }
    for (const Window& w : windows) {
        DWORD old;
        if (!VirtualProtect(module + w.rva, w.size + sizeof(finish),
                             PAGE_EXECUTE_READWRITE, &old)) return FALSE;
        memcpy(module + w.rva, w.bytes, w.size);
        memcpy(module + w.rva + w.size, finish, sizeof(finish));
        DWORD ignored;
        VirtualProtect(module + w.rva, w.size + sizeof(finish), old, &ignored);
        FlushInstructionCache(GetCurrentProcess(), module + w.rva,
                               w.size + sizeof(finish));
    }
    // Skip the preceding topology setter, retaining all native draw arguments,
    // the exact patch window, and the renderer's following counter updates.
    fixture_draw_entry = module + kDrawWindowRva + 10;
    fixture_indexed_entry = module + kIndexedWindowRva + 10;
    ready = true;
    return TRUE;
}

extern "C" __attribute__((naked)) void invoke_draw(void*, UINT) {
    asm("push %ebx\n push %esi\n push %edi\n"
        "mov 16(%esp), %esi\n mov 20(%esp), %edi\n xor %ebx,%ebx\n"
        "jmp *_fixture_draw_entry\n");
}
extern "C" __attribute__((naked)) void invoke_indexed(void*, INT, UINT, UINT, UINT) {
    asm("push %ebx\n push %esi\n push %edi\n"
        "mov 16(%esp), %esi\n mov 32(%esp), %ebx\n xor %edi,%edi\n"
        "jmp *_fixture_indexed_entry\n");
}

extern "C" __declspec(dllexport) void submit_draw(ID3D11DeviceContext* context, UINT count) {
    BYTE renderer[0xc0] = {};
    *(ID3D11DeviceContext**)(renderer + 0x2c) = context;
    invoke_draw(renderer, count);
}
extern "C" __declspec(dllexport) void submit_indexed(
    ID3D11DeviceContext* context, UINT count, UINT start, INT base) {
    BYTE renderer[0xc0] = {};
    *(ID3D11DeviceContext**)(renderer + 0x2c) = context;
    invoke_indexed(renderer, base, 0, start, count);
}

extern "C" __declspec(dllexport) HRESULT make_device(
    ID3D11Device** device, ID3D11DeviceContext** context) {
    if (!prepare_draw_sites()) return E_FAIL;
    D3D_FEATURE_LEVEL wanted = D3D_FEATURE_LEVEL_11_0;
    return D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                             &wanted, 1, D3D11_SDK_VERSION, device, nullptr, context);
}
