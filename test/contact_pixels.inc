// Execute the actual transformed receiver. Shader creation alone can defer
// compilation on DXMT, and cannot catch a wrong swizzle or reversed comparison.
void testContactPixels(ID3D11Device* device, ID3D11DeviceContext* context) {
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    typedef HRESULT(WINAPI* CompileFn)(LPCVOID, SIZE_T, LPCSTR,
        const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT,
        ID3DBlob**, ID3DBlob**);
    CompileFn compile = compiler ? (CompileFn)(void*)GetProcAddress(compiler, "D3DCompile") : nullptr;
    const char* vsSource =
        "struct O { float4 p:SV_Position; float2 uv:TEXCOORD0; };"
        "O main(uint i:SV_VertexID) { O o; o.uv=float2((i<<1)&2,i&2);"
        "o.p=float4(o.uv*float2(2,-2)+float2(-1,1),0,1); return o; }";
    ID3DBlob *code = nullptr, *errors = nullptr;
    ID3D11VertexShader* vs = nullptr;
    bool ok = compile && SUCCEEDED(compile(vsSource, strlen(vsSource), "contact-test",
        nullptr, nullptr, "main", "vs_5_0", 0, 0, &code, &errors));
    if (ok) ok = SUCCEEDED(device->CreateVertexShader(code->GetBufferPointer(),
        code->GetBufferSize(), nullptr, &vs));
    if (code) code->Release();
    if (errors) errors->Release();
    long size = 0;
    void* bytes = readFile("tq-dxbc-PS-deferred-shadow.dxbc", &size);
    tq::dxbc::PatchResult patch = {};
    ID3D11PixelShader *original = nullptr, *marched = nullptr;
    ok &= bytes && tq::dxbc::addContactShadowMarch(bytes, size, 8, &patch);
    if (ok) ok = SUCCEEDED(device->CreatePixelShader(bytes, size, nullptr, &original))
        && SUCCEEDED(device->CreatePixelShader(patch.data, patch.size, nullptr, &marched));
    tq::dxbc::release(&patch); free(bytes);
    // Private-data tags have shader lifetime and no fixed identity-table limit.
    check(original && marched && tq::contact::noteReceiver(original, false)
        && tq::contact::noteReceiver(marched, true)
        && tq::contact::receiverKind(original) == 1
        && tq::contact::receiverKind(marched) == 2,
        "shader lifetime tags distinguish baseline and marched receivers");
    float constants[48] = {};
    constants[0] = constants[1] = constants[2] = 1;
    constants[5] = 1; // light towards +Y, matching the synthetic upward normal
    constants[31] = 1; // world-to-shadow W; all shadow-map samples are lit
    const float inverse[16] = {1,0,0,0, 0,1,0,0, 0,0,0,1, 0,0,-1,1};
    memcpy(constants + 32, inverse, sizeof(inverse));
    float parameters[28] = {};
    parameters[19] = 1; // march increases clip W / view depth
    parameters[20] = 0.025f; parameters[21] = 0.02f;
    parameters[22] = 1.0f / 0.15f; parameters[23] = 0.5f;
    parameters[24] = -1; parameters[25] = 1;
    parameters[26] = 0.75f; parameters[27] = 1.0f / 8;
    ID3D11Buffer *cb = nullptr, *params = nullptr;
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(constants); bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA init = {constants, 0, 0};
    if (ok) ok = SUCCEEDED(device->CreateBuffer(&bd, &init, &cb));
    bd.ByteWidth = sizeof(parameters); init.pSysMem = parameters;
    if (ok) ok = SUCCEEDED(device->CreateBuffer(&bd, &init, &params));
    ID3D11Texture2D* textures[4] = {};
    ID3D11ShaderResourceView* views[4] = {};
    const float colors[4][4] = {{0.5f,1,0.5f,0}, {0,0,0,0}, {0.5f,0,0,0}, {1,1,1,1}};
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = td.Height = 1; td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    for (unsigned i = 0; i < 4 && ok; ++i) {
        init.pSysMem = colors[i]; init.SysMemPitch = 16;
        ok = SUCCEEDED(device->CreateTexture2D(&td, &init, &textures[i]))
            && SUCCEEDED(device->CreateShaderResourceView(textures[i], nullptr, &views[i]));
    }
    ID3D11Texture2D *output = nullptr, *staging = nullptr;
    ID3D11RenderTargetView* target = nullptr;
    td.Width = td.Height = 8; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (ok) ok = SUCCEEDED(device->CreateTexture2D(&td, nullptr, &output))
        && SUCCEEDED(device->CreateRenderTargetView(output, nullptr, &target));
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (ok) ok = SUCCEEDED(device->CreateTexture2D(&td, nullptr, &staging));
    ID3D11SamplerState* sampler = nullptr;
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX; sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    if (ok) ok = SUCCEEDED(device->CreateSamplerState(&sd, &sampler));
    check(ok, "create synthetic receiver pixel fixture");
    if (ok) {
        context->ClearState();
        D3D11_VIEWPORT viewport = {0,0,8,8,0,1};
        context->RSSetViewports(1, &viewport);
        context->OMSetRenderTargets(1, &target, nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs, nullptr, 0);
        context->PSSetConstantBuffers(0, 1, &cb);
        context->PSSetConstantBuffers(13, 1, &params);
        context->PSSetShaderResources(0, 4, views);
        ID3D11SamplerState* samplers[4] = {sampler,sampler,sampler,sampler};
        context->PSSetSamplers(0, 4, samplers);
        auto draw = [&](ID3D11PixelShader* shader) {
            context->UpdateSubresource(params, 0, nullptr, parameters, 0, 0);
            context->PSSetShader(shader, nullptr, 0);
            const float clear[4] = {-1,-1,-1,-1};
            context->ClearRenderTargetView(target, clear);
            context->Draw(3, 0);
            context->CopyResource(staging, output);
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            float pixel = -1;
            if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
                pixel = ((float*)((BYTE*)mapped.pData + 4 * mapped.RowPitch))[4 * 4];
                context->Unmap(staging, 0);
            }
            return pixel;
        };
        float baseline = draw(original);
        float contact = draw(marched);
        check(fabsf(baseline - 1.0f) < 1.0e-4f && contact < baseline - 0.05f
            && contact >= 0.5f, "actual march darkens synthetic contact within the strength bound");
        parameters[23] = 0;
        check(fabsf(draw(marched) - baseline) < 1.0e-5f, "zero strength exactly preserves original pixels");
        parameters[23] = 0.5f; parameters[26] = 1.1f;
        check(fabsf(draw(marched) - baseline) < 1.0e-5f, "rejected normal skips contact shading");
        parameters[26] = 0.75f; parameters[16] = 1000;
        check(fabsf(draw(marched) - baseline) < 1.0e-5f, "off-screen ray cannot pick up clamped edge depth");
        parameters[16] = 0; parameters[19] = -1000;
        check(fabsf(draw(marched) - baseline) < 1.0e-5f, "ray behind the camera remains neutral and finite");
        parameters[19] = 1; parameters[21] = 1;
        check(fabsf(draw(marched) - baseline) < 1.0e-5f, "bias rejects depth gaps below the threshold");
    }
    context->ClearState();
    for (auto view : views) if (view) view->Release();
    for (auto texture : textures) if (texture) texture->Release();
    if (sampler) sampler->Release();
    if (target) target->Release();
    if (output) output->Release();
    if (staging) staging->Release();
    if (cb) cb->Release();
    if (params) params->Release();
    if (original) original->Release();
    if (marched) marched->Release();
    if (vs) vs->Release();
    if (compiler) FreeLibrary(compiler);
}
