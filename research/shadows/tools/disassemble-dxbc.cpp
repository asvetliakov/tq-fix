#include <windows.h>
#include <d3dcompiler.h>

#include <cstdio>
#include <string>
#include <vector>

using DisassembleFn = HRESULT(WINAPI *)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob **);

static bool read_file(const std::string& path, std::vector<unsigned char>* bytes) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    std::fseek(file, 0, SEEK_END);
    const long length = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);
    if (length <= 0) {
        std::fclose(file);
        return false;
    }
    bytes->resize(static_cast<size_t>(length));
    const bool ok = std::fread(bytes->data(), 1, bytes->size(), file) == bytes->size();
    std::fclose(file);
    return ok;
}

static bool write_file(const std::string& path, const void* data, size_t size) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) return false;
    const bool ok = std::fwrite(data, 1, size, file) == size;
    std::fclose(file);
    return ok;
}

static std::string join(const std::string& directory, const std::string& name) {
    if (!directory.empty() && (directory.back() == '\\' || directory.back() == '/')) {
        return directory + name;
    }
    return directory + "\\" + name;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: disassemble-dxbc <input-directory> <output-directory>\n");
        return 2;
    }

    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!compiler) compiler = LoadLibraryW(L"d3dcompiler_43.dll");
    if (!compiler) {
        std::fprintf(stderr, "could not load d3dcompiler_47.dll or d3dcompiler_43.dll\n");
        return 3;
    }
    auto disassemble = reinterpret_cast<DisassembleFn>(
        GetProcAddress(compiler, "D3DDisassemble"));
    if (!disassemble) {
        std::fprintf(stderr, "D3DDisassemble export not found\n");
        FreeLibrary(compiler);
        return 3;
    }

    const std::string input = argv[1];
    const std::string output = argv[2];
    CreateDirectoryA(output.c_str(), nullptr);

    WIN32_FIND_DATAA found = {};
    HANDLE search = FindFirstFileA(join(input, "*.dxbc").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "no DXBC inputs found in %s\n", input.c_str());
        FreeLibrary(compiler);
        return 4;
    }

    unsigned count = 0;
    unsigned failures = 0;
    do {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::string name = found.cFileName;
        std::vector<unsigned char> bytecode;
        if (!read_file(join(input, name), &bytecode)) {
            std::fprintf(stderr, "failed to read %s\n", name.c_str());
            ++failures;
            continue;
        }
        ID3DBlob* text = nullptr;
        const HRESULT result = disassemble(
            bytecode.data(), bytecode.size(),
            D3D_DISASM_ENABLE_INSTRUCTION_NUMBERING, nullptr, &text);
        if (FAILED(result) || !text) {
            std::fprintf(stderr, "D3DDisassemble failed for %s: 0x%08lx\n",
                         name.c_str(), static_cast<unsigned long>(result));
            ++failures;
            continue;
        }
        std::string output_name = name.substr(0, name.size() - 5) + ".asm";
        if (!write_file(join(output, output_name),
                        text->GetBufferPointer(), text->GetBufferSize())) {
            std::fprintf(stderr, "failed to write %s\n", output_name.c_str());
            ++failures;
        } else {
            ++count;
        }
        text->Release();
    } while (FindNextFileA(search, &found));

    FindClose(search);
    FreeLibrary(compiler);
    std::printf("disassembled %u unique DXBC programs (%u failures)\n", count, failures);
    return failures == 0 ? 0 : 5;
}
