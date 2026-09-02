#include "frustum_fix.h"
#include "hdr.h"

#include <d3d11.h>
#include <string.h>

namespace tq {
namespace frustum {
namespace {

const char kViewportCtorName[] = "??0Viewport@GAME@@QAE@HHHH@Z";
const char kWorldFrustumName[] =
    "?GetFrustum@WorldCamera@GAME@@QBE?AVWorldFrustum@2@ABVViewport@2@@Z";
const char kEngineGlobalName[] = "?gEngine@GAME@@3PAVEngine@1@A";
const char kGetGraphicsEngineName[] =
    "?GetGraphicsEngine@Engine@GAME@@QBEPAVGraphicsEngine@2@XZ";
const char kGetWidthName[] = "?GetWidth@GraphicsEngine@GAME@@QBEHXZ";
const char kGetHeightName[] = "?GetHeight@GraphicsEngine@GAME@@QBEHXZ";

typedef void* (__thiscall* ViewportCtorFn)(void*, int, int, int, int);
typedef void* (__thiscall* GetGraphicsEngineFn)(void*);
typedef int (__thiscall* GetDimensionFn)(void*);

ViewportCtorFn g_viewportCtor;
GetGraphicsEngineFn g_getGraphicsEngine;
GetDimensionFn g_getWidth;
GetDimensionFn g_getHeight;
void** g_engineGlobal;
const void* g_targetReturn;
void** g_viewportSlot;
LONG g_installed;
bool g_enabled = true;

bool readable(const void* address) {
    MEMORY_BASIC_INFORMATION info;
    if (!address || !VirtualQuery(address, &info, sizeof(info))) return false;
    DWORD protection = info.Protect & 0xff;
    return info.State == MEM_COMMIT && !(info.Protect & PAGE_GUARD)
        && protection != PAGE_NOACCESS;
}

bool belongsTo(HMODULE module, const void* address) {
    MEMORY_BASIC_INFORMATION info;
    return module && address && VirtualQuery(address, &info, sizeof(info))
        && info.AllocationBase == module;
}

void** importSlot(HMODULE module, const char* importedDll, const char* importedName) {
    if (!module) return nullptr;
    BYTE* base = (BYTE*)module;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (!readable(dos) || dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (!readable(nt) || nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;

    IMAGE_DATA_DIRECTORY& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress || !directory.Size) return nullptr;
    IMAGE_IMPORT_DESCRIPTOR* descriptor =
        (IMAGE_IMPORT_DESCRIPTOR*)(base + directory.VirtualAddress);
    for (; readable(descriptor) && descriptor->Name; ++descriptor) {
        const char* dllName = (const char*)(base + descriptor->Name);
        if (!readable(dllName) || _stricmp(dllName, importedDll)) continue;
        if (!descriptor->OriginalFirstThunk) return nullptr;
        IMAGE_THUNK_DATA* names =
            (IMAGE_THUNK_DATA*)(base + descriptor->OriginalFirstThunk);
        IMAGE_THUNK_DATA* addresses =
            (IMAGE_THUNK_DATA*)(base + descriptor->FirstThunk);
        for (; readable(names) && readable(addresses) && names->u1.AddressOfData;
             ++names, ++addresses) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME* name =
                (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (readable(name) && !strcmp((const char*)name->Name, importedName))
                return (void**)&addresses->u1.Function;
        }
        return nullptr;
    }
    return nullptr;
}

bool executableSection(HMODULE module, const BYTE** code, SIZE_T* size) {
    if (!module || !code || !size) return false;
    BYTE* base = (BYTE*)module;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (!readable(dos) || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (!readable(nt) || nt->Signature != IMAGE_NT_SIGNATURE) return false;
    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (!readable(section)) return false;
        if (!(section->Characteristics & IMAGE_SCN_CNT_CODE)
            || !(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        SIZE_T sectionSize = section->Misc.VirtualSize;
        if (!sectionSize || section->VirtualAddress >= nt->OptionalHeader.SizeOfImage
            || sectionSize > nt->OptionalHeader.SizeOfImage - section->VirtualAddress)
            return false;
        *code = base + section->VirtualAddress;
        *size = sectionSize;
        return readable(*code) && readable(*code + sectionSize - 1);
    }
    return false;
}

bool readOptions() {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return true;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return true;
    lstrcpyW(slash + 1, L"tqflicker.ini");
    wchar_t value[32];
    GetPrivateProfileStringW(L"graphics", L"edge_updates", L"expanded",
                             value, 32, path);
    return _wcsicmp(value, L"original") != 0;
}

bool writePointer(void** slot, void* value, void** oldValue = nullptr) {
    if (!slot || !value || !readable(slot)) return false;
    DWORD oldProtection;
    if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &oldProtection)) return false;
    void* previous = InterlockedExchangePointer((PVOID volatile*)slot, value);
    DWORD ignored;
    VirtualProtect(slot, sizeof(*slot), oldProtection, &ignored);
    if (oldValue) *oldValue = previous;
    return true;
}

void liveDimensions(int* width, int* height) {
    *width = *height = 0;
    if (!g_engineGlobal || !g_getGraphicsEngine || !g_getWidth || !g_getHeight) return;
    void* engine = *g_engineGlobal;
    if (!engine) return;
    void* graphics = g_getGraphicsEngine(engine);
    if (!graphics) return;
    *width = g_getWidth(graphics);
    *height = g_getHeight(graphics);
}

__attribute__((noinline))
void* __thiscall hookViewportCtor(void* viewport, int left, int top,
                                  int width, int height) {
    const void* returnAddress = __builtin_return_address(0);
    const bool targetCall = returnAddress == g_targetReturn;
    int liveWidth = 0, liveHeight = 0, selectedWidth, selectedHeight;
    if (targetCall) liveDimensions(&liveWidth, &liveHeight);
    selectViewportSize(g_enabled, targetCall,
                       width, height, liveWidth, liveHeight,
                       &selectedWidth, &selectedHeight);
    return g_viewportCtor(viewport, left, top, selectedWidth, selectedHeight);
}

}  // namespace

const BYTE* findUpdateViewportCall(const BYTE* code, SIZE_T size,
                                   uintptr_t viewportCtorSlot,
                                   uintptr_t worldFrustumSlot,
                                   unsigned* matchCount) {
    static const BYTE prefix[] = {
        0x68, 0x00, 0x03, 0x00, 0x00,       // push 768
        0x68, 0x00, 0x04, 0x00, 0x00,       // push 1024
        0x6a, 0x00, 0x6a, 0x00,             // push 0; push 0
        0x8d, 0x4c, 0x24, 0x18,             // lea ecx,[esp+18h]
        0xff, 0x15                          // call [Viewport::Viewport]
    };
    static const BYTE middle[] = {
        0x8d, 0x44, 0x24, 0x08, 0x50,
        0x8d, 0x84, 0x24, 0x5c, 0x06, 0x00, 0x00, 0x50,
        0x8d, 0x4c, 0x24, 0x20,
        0xff, 0x15                          // call [WorldCamera::GetFrustum]
    };
    static const BYTE suffix[] = {
        0xb9, 0x02, 0x01, 0x00, 0x00,       // mov ecx,102h
        0x8b, 0xf0, 0xf3, 0xa5              // copy returned WorldFrustum
    };
    const SIZE_T total = sizeof(prefix) + sizeof(uint32_t) + sizeof(middle)
                       + sizeof(uint32_t) + sizeof(suffix);
    const BYTE* found = nullptr;
    unsigned count = 0;
    if (code && size >= total && viewportCtorSlot <= UINT32_MAX
        && worldFrustumSlot <= UINT32_MAX) {
        const uint32_t viewportAddress = (uint32_t)viewportCtorSlot;
        const uint32_t frustumAddress = (uint32_t)worldFrustumSlot;
        for (SIZE_T i = 0; i <= size - total; ++i) {
            const BYTE* p = code + i;
            if (memcmp(p, prefix, sizeof(prefix))) continue;
            p += sizeof(prefix);
            if (memcmp(p, &viewportAddress, sizeof(viewportAddress))) continue;
            p += sizeof(viewportAddress);
            if (memcmp(p, middle, sizeof(middle))) continue;
            p += sizeof(middle);
            if (memcmp(p, &frustumAddress, sizeof(frustumAddress))) continue;
            p += sizeof(frustumAddress);
            if (memcmp(p, suffix, sizeof(suffix))) continue;
            ++count;
            found = code + i + sizeof(prefix) + sizeof(viewportAddress);
        }
    }
    if (matchCount) *matchCount = count;
    return count == 1 ? found : nullptr;
}

bool selectViewportSize(bool enabled, bool targetCall,
                        int requestedWidth, int requestedHeight,
                        int liveWidth, int liveHeight,
                        int* selectedWidth, int* selectedHeight) {
    if (!selectedWidth || !selectedHeight) return false;
    *selectedWidth = requestedWidth;
    *selectedHeight = requestedHeight;
    const int maxDimension = D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION;
    if (!enabled || !targetCall || requestedWidth != 1024 || requestedHeight != 768
        || liveWidth <= 0 || liveHeight <= 0
        || liveWidth > maxDimension || liveHeight > maxDimension
        || (int64_t)liveWidth * 3 <= (int64_t)liveHeight * 4)
        return false;
    *selectedWidth = liveWidth;
    *selectedHeight = liveHeight;
    return true;
}

void install(HMODULE gameModule) {
    if (!gameModule || InterlockedCompareExchange(&g_installed, 1, 0)) return;
    g_enabled = readOptions();
    if (!g_enabled) {
        tq::hdr::log("Frustum hook disabled by configuration\r\n");
        return;
    }

    HMODULE engineModule = GetModuleHandleW(L"Engine.dll");
    void** viewportSlot = importSlot(gameModule, "Engine.dll", kViewportCtorName);
    void** frustumSlot = importSlot(gameModule, "Engine.dll", kWorldFrustumName);
    if (!engineModule || !viewportSlot || !frustumSlot || !readable(*viewportSlot)
        || !belongsTo(engineModule, *viewportSlot) || !belongsTo(engineModule, *frustumSlot)) {
        tq::hdr::log("Frustum hook skipped: incompatible Game/Engine imports\r\n");
        return;
    }

    const BYTE* code = nullptr;
    SIZE_T codeSize = 0;
    unsigned matches = 0;
    const BYTE* target = executableSection(gameModule, &code, &codeSize)
        ? findUpdateViewportCall(code, codeSize, (uintptr_t)viewportSlot,
                                 (uintptr_t)frustumSlot, &matches)
        : nullptr;
    if (!target || matches != 1) {
        tq::hdr::log("Frustum hook skipped: viewport signature matches=%u\r\n", matches);
        return;
    }

    void** engineGlobal = (void**)GetProcAddress(engineModule, kEngineGlobalName);
    GetGraphicsEngineFn getGraphicsEngine = (GetGraphicsEngineFn)(void*)
        GetProcAddress(engineModule, kGetGraphicsEngineName);
    GetDimensionFn getWidth = (GetDimensionFn)(void*)
        GetProcAddress(engineModule, kGetWidthName);
    GetDimensionFn getHeight = (GetDimensionFn)(void*)
        GetProcAddress(engineModule, kGetHeightName);
    if (!belongsTo(engineModule, engineGlobal)
        || !belongsTo(engineModule, (const void*)getGraphicsEngine)
        || !belongsTo(engineModule, (const void*)getWidth)
        || !belongsTo(engineModule, (const void*)getHeight)) {
        tq::hdr::log("Frustum hook skipped: incompatible Engine exports\r\n");
        return;
    }

    void* original = *viewportSlot;
    if (!original || !belongsTo(engineModule, original)) {
        tq::hdr::log("Frustum hook skipped: invalid viewport constructor\r\n");
        return;
    }

    // Publish every dependency, including the call-through, before atomically
    // making the hook reachable from the game's update thread.
    g_engineGlobal = engineGlobal;
    g_getGraphicsEngine = getGraphicsEngine;
    g_getWidth = getWidth;
    g_getHeight = getHeight;
    g_targetReturn = target;
    g_viewportSlot = viewportSlot;
    g_viewportCtor = (ViewportCtorFn)original;
    if (!writePointer(viewportSlot, (void*)&hookViewportCtor)) {
        g_viewportSlot = nullptr;
        g_viewportCtor = nullptr;
        tq::hdr::log("Frustum hook failed while patching the import slot\r\n");
        return;
    }
    tq::hdr::log("Frustum hook installed\r\n");
}

void shutdown() {
    if (g_viewportSlot && readable(g_viewportSlot)
        && *g_viewportSlot == (void*)&hookViewportCtor && g_viewportCtor)
        writePointer(g_viewportSlot, (void*)g_viewportCtor);
    g_viewportSlot = nullptr;
    g_viewportCtor = nullptr;
    g_engineGlobal = nullptr;
    g_getGraphicsEngine = nullptr;
    g_getWidth = g_getHeight = nullptr;
    g_targetReturn = nullptr;
    InterlockedExchange(&g_installed, 0);
}

}  // namespace frustum
}  // namespace tq
