// DCS VR Control: deferred DX11 game feature (dcs_deferred_feature.inc) on the
// generic DX11 host. The game creates, evaluates and releases through the NGX
// core (a fixture copy that forwards DX11 to the snippet, like NVIDIA's core),
// with DCSVR_QUAD_FOCUS=1 and the SR-replacing NR-before transport selected.
std::uint32_t dcs_created_quality{~0U};
void dcs_observe_created(const cheeky::foveated_dlss::NgxParameters* created) {
    dcs_created_quality = cheeky::foveated_dlss::get_ui(created, "PerfQualityValue");
}

void verify_dcs_deferred(CheekyRuntimeCommandFn command, CheekyRuntimeSnapshotFn get,
    std::uint64_t attachment, ID3D11Device* device, ID3D11DeviceContext* context, HMODULE ngx,
    const std::filesystem::path& log_path) {
    using namespace cheeky::foveated_dlss;
    using Init = NgxResult (*)(unsigned long long, const wchar_t*, ID3D11Device*, const void*, unsigned);
    using Create = NgxResult (*)(ID3D11DeviceContext*, unsigned, NgxParameters*, NgxHandle**);
    using Evaluate = NgxResult (*)(ID3D11DeviceContext*, const NgxHandle*, const NgxParameters*, NgxProgressCallback);
    using Release = NgxResult (*)(NgxHandle*);
    const auto core = GetModuleHandleW(L"_nvngx.dll");
    require(core != nullptr, "Fixture NGX core loaded");
    const auto log_count = [&](const char* text) {
        std::ifstream log(log_path);
        unsigned count{};
        for (std::string line; std::getline(log, line);) count += line.find(text) != std::string::npos ? 1U : 0U;
        return count;
    };
    // The fixture core also exports EvaluateFeature_C (NVIDIA's does not); both must be detoured.
    for (unsigned i = 0; i < 400 && log_count("Direct detour installed export=NVSDK_NGX_D3D11_EvaluateFeature_C") < 2U; ++i) Sleep(25);
    require(log_count("Direct detour installed export=NVSDK_NGX_D3D11_EvaluateFeature_C") >= 2U,
        "DCS quad focus installs the core DX11 evaluate detours");
    require(ngx_succeeded(proc<Init>(ngx, "NVSDK_NGX_D3D11_Init")(42, L".", device, nullptr, 1)), "Record DX11 initialization");
    require(command(attachment, "1\n40\nset\nEnabled=false\nNrEnabled=true\nNrFoveated=false\nPeripheralDlaa=false\n"
        "NrProcessingOrder=1\nD3D11D3D12Transport=true\nAutoStereoAlignment=false"), "Select NR before upscaling on the transport");
    const auto counter = [&](HMODULE module, const char* name) { return proc<unsigned(*)()>(module, name)(); };
    const auto create = proc<Create>(core, "NVSDK_NGX_D3D11_CreateFeature");
    const auto evaluate = proc<Evaluate>(core, "NVSDK_NGX_D3D11_EvaluateFeature");
    const auto release = proc<Release>(core, "NVSDK_NGX_D3D11_ReleaseFeature");
    proc<void(*)(void(*)(const NgxParameters*))>(core, "CheekyFakeObserveCreated")(&dcs_observe_created);

    MockNgxParameters parameters;
    parameters.Set("Width", 128U); parameters.Set("Height", 128U);
    parameters.Set("OutWidth", 256U); parameters.Set("OutHeight", 256U);
    parameters.Set("DLSS.Feature.Create.Flags", 2); parameters.Set("PerfQualityValue", 2);
    parameters.Set("MV.Scale.X", 1.F); parameters.Set("MV.Scale.Y", 1.F);
    const char* names[]{"Color", "Depth", "MotionVectors", "Output"};
    std::array<ComPtr<ID3D11Texture2D>, 4> textures;
    for (unsigned i = 0; i < textures.size(); ++i) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = i == 3 ? 256U : 128U;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = i == 1 ? DXGI_FORMAT_R32_FLOAT : i == 2 ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &textures[i])), "Game texture");
        parameters.Set(names[i], static_cast<ID3D11Resource*>(textures[i].Get()));
    }
    const auto creates0 = counter(core, "CheekyFakeCreates"), releases0 = counter(core, "CheekyFakeReleases");
    const auto snippet_creates0 = counter(ngx, "CheekyFakeCreates");

    // 1. Creation is deferred: neither the core nor the snippet allocates a feature.
    NgxHandle* never_used{};
    require(ngx_succeeded(create(context, 1, &parameters, &never_used)) && never_used, "Deferred create returns a handle");
    require(counter(core, "CheekyFakeCreates") == creates0 && counter(ngx, "CheekyFakeCreates") == snippet_creates0,
        "Deferred create allocates no NGX feature");
    require(log_count("DCS deferred DX11 DLSS create") == 1U, "Deferred create is logged");
    // 2. A never-evaluated feature releases nothing in NGX.
    require(ngx_succeeded(release(never_used)), "Release of a never-created feature succeeds");
    require(counter(core, "CheekyFakeReleases") == releases0, "Never-created feature makes no NGX release");
    require(log_count("FEATURE_RELEASE route=D3D11-core") == 1U && log_count("deferred=1") >= 1U,
        "Release of the deferred handle is in the feature ledger");

    // 3. The first evaluation of DCS's own feature creates it first, with the creation-time values.
    NgxHandle* handle{};
    require(ngx_succeeded(create(context, 1, &parameters, &handle)) && handle, "Second deferred create");
    parameters.Set("PerfQualityValue", 0); // DCS reuses one bag; evaluation values differ from creation.
    const auto core_evaluates0 = counter(core, "CheekyFakeEvaluates");
    // No OpenXR layer: the Quad Views gate rejects, the game's own DX11 DLSS must run.
    require(ngx_succeeded(evaluate(context, handle, &parameters, nullptr)), "Fallback evaluation of the deferred feature");
    require(counter(core, "CheekyFakeCreates") == creates0 + 1U && counter(ngx, "CheekyFakeCreates") == snippet_creates0 + 1U,
        "Fallback materializes exactly one real feature");
    require(counter(core, "CheekyFakeEvaluates") == core_evaluates0 + 1U, "Fallback evaluates the real feature once");
    require(dcs_created_quality == 2U, "Real feature created with the creation-time PerfQualityValue");
    int evaluation_quality{-1};
    require(ngx_succeeded(parameters.Get("PerfQualityValue", &evaluation_quality)) && evaluation_quality == 0,
        "Evaluation parameters restored after materialization");
    require(log_count("DCS deferred DX11 DLSS materialized") == 1U, "Materialization is logged");
    for (unsigned i = 0; i < 3; ++i) require(ngx_succeeded(evaluate(context, handle, &parameters, nullptr)), "Fallback frames");
    require(counter(core, "CheekyFakeCreates") == creates0 + 1U && counter(core, "CheekyFakeEvaluates") == core_evaluates0 + 4U,
        "Later fallbacks reuse the real feature");

    // 4. A focus view the transport owns from its first frame never creates DCS's
    //    own feature: no NGX create, evaluate or release for its whole lifetime.
    NgxHandle* transported{};
    require(ngx_succeeded(create(context, 1, &parameters, &transported)) && transported, "Third deferred create");
    SetEnvironmentVariableW(L"DCSVR_QUAD_FOCUS", nullptr); // Gate open (no Quad Views proof needed).
    const auto transport_core_evaluates = counter(core, "CheekyFakeEvaluates");
    bool transport{};
    for (unsigned frame = 0; frame < 100 && !transport; ++frame) {
        require(ngx_succeeded(evaluate(context, transported, &parameters, nullptr)), "Transport frame");
        context->Flush();
        transport = contains(snapshot(get), "\"execution_path\":\"DX12 Transport\"") && contains(snapshot(get), "\"nr\":\"Active\"");
        if (!transport) Sleep(25);
    }
    for (unsigned frame = 0; frame < 6; ++frame)
        require(ngx_succeeded(evaluate(context, transported, &parameters, nullptr)), "Steady transport frame");
    context->Flush();
    if (!transport) puts(snapshot(get).c_str());
    require(transport, "Deferred handle runs NR before upscaling on the transport");
    require(counter(core, "CheekyFakeEvaluates") == transport_core_evaluates && counter(core, "CheekyFakeCreates") == creates0 + 1U,
        "Transport frames neither create nor evaluate the game's feature");
    require(ngx_succeeded(release(transported)) && counter(core, "CheekyFakeReleases") == releases0,
        "A transport-only deferred feature is released without any NGX release");
    // The materialized feature keeps its real feature through transport frames (NGX would pool, not return, its VRAM).
    for (unsigned frame = 0; frame < 6; ++frame)
        require(ngx_succeeded(evaluate(context, handle, &parameters, nullptr)), "Transport frame of the materialized handle");
    context->Flush();
    require(counter(core, "CheekyFakeEvaluates") == transport_core_evaluates && counter(core, "CheekyFakeReleases") == releases0,
        "Transport frames neither evaluate nor release a materialized game feature");

    // 5. A later fallback reuses it; the game's release then releases it exactly once.
    SetEnvironmentVariableW(L"DCSVR_QUAD_FOCUS", L"1");
    require(ngx_succeeded(evaluate(context, handle, &parameters, nullptr)), "Fallback after the transport stretch");
    require(counter(core, "CheekyFakeCreates") == creates0 + 1U && counter(core, "CheekyFakeEvaluates") == transport_core_evaluates + 1U,
        "Fallback after transport frames reuses the real feature");
    require(ngx_succeeded(release(handle)), "Release materialized deferred feature");
    require(counter(core, "CheekyFakeReleases") == releases0 + 1U, "Game release frees the real feature exactly once");
    require(log_count("DCS deferred DX11 DLSS first evaluation") == 1U, "First evaluation of the real feature is logged with VRAM");
    require(log_count("FEATURE_VRAM first-evaluation") >= 2U, "First evaluations of game features are logged with VRAM");
    require(log_count("FEATURE_RELEASE route=D3D11-core") == 3U, "Every deferred release is in the feature ledger");
    context->Flush();
}
