#include <windows.h>
#include <d3d11shader.h>

#include <stdio.h>
#include <stdlib.h>

namespace {

const GUID kShaderReflectionId = {
    0x8d536ca1, 0x0cca, 0x4956,
    {0xa8, 0x37, 0x78, 0x69, 0x63, 0x75, 0x55, 0x84}
};

void* readFile(const char* path, SIZE_T* size) {
    *size = 0;
    FILE* file = fopen(path, "rb");
    if (!file) return nullptr;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    void* data = length > 0 ? malloc((SIZE_T)length) : nullptr;
    if (!data || fread(data, 1, (SIZE_T)length, file) != (SIZE_T)length) {
        free(data);
        data = nullptr;
    } else {
        *size = (SIZE_T)length;
    }
    fclose(file);
    return data;
}

const char* className(D3D_SHADER_VARIABLE_CLASS value) {
    switch (value) {
        case D3D_SVC_SCALAR: return "scalar";
        case D3D_SVC_VECTOR: return "vector";
        case D3D_SVC_MATRIX_ROWS: return "matrix-rows";
        case D3D_SVC_MATRIX_COLUMNS: return "matrix-columns";
        case D3D_SVC_OBJECT: return "object";
        case D3D_SVC_STRUCT: return "struct";
        default: return "other";
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: reflect-dxbc.exe <shader.dxbc>\n");
        return 2;
    }
    SIZE_T size = 0;
    void* bytes = readFile(argv[1], &size);
    if (!bytes) return 3;
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!compiler) compiler = LoadLibraryW(L"d3dcompiler_43.dll");
    typedef HRESULT (WINAPI* ReflectFn)(LPCVOID, SIZE_T, REFIID, void**);
    ReflectFn reflect = compiler
        ? (ReflectFn)GetProcAddress(compiler, "D3DReflect") : nullptr;
    ID3D11ShaderReflection* shader = nullptr;
    HRESULT hr = reflect
        ? reflect(bytes, size, kShaderReflectionId, (void**)&shader)
        : E_NOINTERFACE;
    if (FAILED(hr) || !shader) {
        fprintf(stderr, "D3DReflect failed: 0x%08lx\n", (unsigned long)hr);
        free(bytes);
        if (compiler) FreeLibrary(compiler);
        return 4;
    }
    D3D11_SHADER_DESC shaderDesc = {};
    shader->GetDesc(&shaderDesc);
    for (UINT bufferIndex = 0; bufferIndex < shaderDesc.ConstantBuffers;
         ++bufferIndex) {
        ID3D11ShaderReflectionConstantBuffer* buffer =
            shader->GetConstantBufferByIndex(bufferIndex);
        D3D11_SHADER_BUFFER_DESC bufferDesc = {};
        if (FAILED(buffer->GetDesc(&bufferDesc))) continue;
        for (UINT variableIndex = 0; variableIndex < bufferDesc.Variables;
             ++variableIndex) {
            ID3D11ShaderReflectionVariable* variable =
                buffer->GetVariableByIndex(variableIndex);
            D3D11_SHADER_VARIABLE_DESC variableDesc = {};
            D3D11_SHADER_TYPE_DESC typeDesc = {};
            if (FAILED(variable->GetDesc(&variableDesc))
                || FAILED(variable->GetType()->GetDesc(&typeDesc)))
                continue;
            if (!strcmp(variableDesc.Name, "worldToShadowMatrix")) {
                printf("buffer=%s variable=%s offset=%u size=%u class=%s rows=%u columns=%u\n",
                       bufferDesc.Name, variableDesc.Name,
                       variableDesc.StartOffset, variableDesc.Size,
                       className(typeDesc.Class), typeDesc.Rows,
                       typeDesc.Columns);
            }
        }
    }
    shader->Release();
    free(bytes);
    FreeLibrary(compiler);
    return 0;
}
