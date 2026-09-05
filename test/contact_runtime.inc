#include "contact_pixels.inc"

// Separate process: the production configuration reader intentionally caches
// settings. Real D3D11 resources exercise copies, uploads, and binding lifetime;
// only the WAS_STILL_DRAWING result is injected to make GPU backlog repeatable.
int testContactRuntime() {
    g_report = fopen("report.txt", "w");
    if (!g_report) return 99;
    FILE* ini = fopen("tqflicker.ini", "w");
    if (!ini) return 99;
    // Leave graphics unspecified to exercise the shipped contact defaults.
    fputs("[debug]\nperformance_trace=full\n", ini);
    fclose(ini);
    wchar_t iniPath[MAX_PATH];
    GetFullPathNameW(L"tqflicker.ini", MAX_PATH, iniPath, nullptr);
    tq::probe::readOptions(iniPath);
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL wanted = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        &wanted, 1, D3D11_SDK_VERSION, &device, nullptr, &context);
    check(SUCCEEDED(hr) && device && context, "create contact-shadow runtime device");
    if (!device || !context) return 98;
    tq::contact::install(device, context);
    check(tq::contact::ready(), "contact resources have initialized neutral parameters");
    tq::probe::createResources(device);

    // Match the live game's allocation, not just the shader's declared size.
    // Poison the unused tail so accepting a large buffer must still copy the
    // correct 192-byte prefix into our smaller staging resource.
    float constants[512] = {};
    for (unsigned i = 48; i < 512; ++i) constants[i] = 12345.0f;
    const float eye[3] = {0, 32, 0};
    float vp[16];
    makeViewProjection(eye, vp);
    tq::contact::invertRowMatrix(vp, constants + 32);
    constants[4] = 0.3f; constants[5] = 1.0f; constants[6] = 0.2f;
    memcpy(constants + 12, eye, sizeof(eye));
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = sizeof(constants); desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3D11_SUBRESOURCE_DATA initial = {constants, 0, 0};
    ID3D11Buffer *source = nullptr, *sentinel = nullptr;
    check(SUCCEEDED(device->CreateBuffer(&desc, &initial, &source)),
          "create the game's 2048-byte dynamic receiver constant buffer");
    desc.ByteWidth = 16;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.CPUAccessFlags = 0;
    check(SUCCEEDED(device->CreateBuffer(&desc, nullptr, &sentinel)), "create b13 sentinel");
    context->PSSetConstantBuffers(0, 1, &source);
    context->PSSetConstantBuffers(13, 1, &sentinel);
    D3D11_VIEWPORT viewport = {0, 0, 64, 64, 0, 1};
    context->RSSetViewports(1, &viewport);

    auto frame = [&](bool marched) {
        tq::probe::beginFrame(context);
        tq::contact::onPresent();
        tq::contact::beforeReceiverDraw(context, marched);
        ID3D11Buffer* bound = nullptr;
        context->PSGetConstantBuffers(13, 1, &bound);
        bool boundCorrectly = marched ? bound && bound != sentinel : bound == sentinel;
        if (bound) bound->Release();
        tq::contact::afterReceiverDraw(context);
        context->PSGetConstantBuffers(13, 1, &bound);
        bool restored = bound == sentinel;
        if (bound) bound->Release();
        tq::probe::endFrame(16.0f);
        return boundCorrectly && restored;
    };
    auto uploadedStrength = [&]() {
        tq::contact::beforeReceiverDraw(context, true);
        ID3D11Buffer *bound = nullptr, *staging = nullptr;
        context->PSGetConstantBuffers(13, 1, &bound);
        float strength = -1;
        if (bound) {
            D3D11_BUFFER_DESC copy = {};
            bound->GetDesc(&copy);
            copy.Usage = D3D11_USAGE_STAGING; copy.BindFlags = 0;
            copy.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (SUCCEEDED(device->CreateBuffer(&copy, nullptr, &staging))) {
                context->CopyResource(staging, bound);
                D3D11_MAPPED_SUBRESOURCE mapped = {};
                // Blocking is confined to the test's observation of the GPU.
                if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
                    strength = ((const float*)mapped.pData)[23];
                    context->Unmap(staging, 0);
                }
                staging->Release();
            }
            bound->Release();
        }
        tq::contact::afterReceiverDraw(context);
        return strength;
    };
    check(frame(false) && tq::contact::stateForTest().pending == 0,
          "unmarched baseline preserves b13 and issues no readback");
    tq::contact::forceReadbackBusyForTest(true);
    bool bindings = true;
    for (unsigned i = 0; i < 12; ++i) bindings &= frame(true);
    auto state = tq::contact::stateForTest();
    check(bindings, "every marched draw binds parameters and restores the exact prior b13");
    check(state.pending == 4 && state.updates == 0 && state.nonblocking,
          "busy GPU fills a bounded ring without overwriting or waiting");
    check(uploadedStrength() == 0, "startup keeps GPU strength neutral during backlog");
    tq::contact::forceReadbackBusyForTest(false);
    for (unsigned i = 0; i < 40 && !tq::contact::stateForTest().valid; ++i) {
        context->Flush(); Sleep(5); frame(true);
    }
    check(tq::contact::stateForTest().valid && uploadedStrength() == 0.70f,
          "2048-byte dynamic source produces usable parameters from its 192-byte prefix");
    tq::contact::forceReadbackBusyForTest(true);
    for (unsigned i = 0; i < 12; ++i) frame(true);
    check(!tq::contact::stateForTest().valid && uploadedStrength() == 0,
          "history older than eight frames disables the uploaded effect without a blocking map");
    tq::contact::forceReadbackBusyForTest(false);
    for (unsigned i = 0; i < 40 && !tq::contact::stateForTest().valid; ++i) {
        context->Flush(); Sleep(5); frame(true);
    }
    check(tq::contact::stateForTest().valid, "contact shadows recover when GPU progress resumes");
    tq::contact::invalidateHistory();
    tq::contact::forceReadbackBusyForTest(true);
    frame(true);
    check(uploadedStrength() == 0, "resize invalidation neutralizes camera history");
    tq::contact::forceReadbackBusyForTest(false);

    // An undersized allocation must still fail, then the exact-size allocation
    // used by the original fixture must recover without a device reinstall.
    const unsigned sourceSizes[] = {176, 192};
    for (unsigned bytes : sourceSizes) {
        ID3D11Buffer* replacement = nullptr;
        desc.ByteWidth = bytes;
        bool replaced = SUCCEEDED(device->CreateBuffer(&desc, &initial, &replacement));
        check(replaced, "create alternate receiver allocation size");
        if (!replaced) continue;
        context->PSSetConstantBuffers(0, 1, &replacement);
        if (source) source->Release();
        source = replacement;
        tq::contact::invalidateHistory();
        for (unsigned i = 0; i < 12; ++i) { context->Flush(); Sleep(5); frame(true); }
        check(bytes == 192 ? tq::contact::stateForTest().valid && uploadedStrength() == 0.70f
                           : !tq::contact::stateForTest().valid && uploadedStrength() == 0,
              bytes == 192 ? "exact-size receiver constants remain supported"
                           : "undersized receiver constants stay neutral and are never copied");
    }

    // An unsupported inverse W must not be treated as axis-aligned depth.
    constants[44] = 0.1f;
    context->UpdateSubresource(source, 0, nullptr, constants, 0, 0);
    tq::contact::invalidateHistory();
    for (unsigned i = 0; i < 12; ++i) { context->Flush(); Sleep(5); frame(true); }
    check(!tq::contact::stateForTest().valid && uploadedStrength() == 0,
          "off-axis inverse depth is rejected rather than rendered with wrong linearization");

    testContactPixels(device, context);
    tq::contact::shutdown();
    check(!tq::contact::ready(), "shutdown drops all contact device resources");
    tq::contact::install(device, context);
    check(tq::contact::ready() && !tq::contact::stateForTest().valid,
          "reinstallation starts with fresh neutral history");
    tq::contact::shutdown();
    tq::probe::shutdown();
    context->ClearState();
    if (source) source->Release();
    if (sentinel) sentinel->Release();
    check(device->GetDeviceRemovedReason() == S_OK, "contact runtime preserves device health");
    context->Release(); device->Release();
    fprintf(g_report, "\nRESULT: %d failure(s)\n", g_failures);
    fclose(g_report);
    return g_failures ? 1 : 0;
}
