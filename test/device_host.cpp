#include <windows.h>
#include <d3d11.h>

extern "C" __declspec(dllexport) HRESULT make_device(
    ID3D11Device** device, ID3D11DeviceContext** context) {
    D3D_FEATURE_LEVEL wanted = D3D_FEATURE_LEVEL_11_0;
    return D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                             &wanted, 1, D3D11_SDK_VERSION, device, nullptr, context);
}
