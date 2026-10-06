// DCS VR Control: deferred DX11 game feature (dcs_deferred_feature.inc) on the
// generic DX11 host. The game creates, evaluates and releases through the NGX
// core (a fixture copy that forwards DX11 to the snippet, like NVIDIA's core),
// with DCSVR_QUAD_FOCUS=1 and the SR-replacing NR-before transport selected.
// Scenarios: 0 the deferred lifecycle (no OpenXR layer: bilinear hold, then
// DCS's own feature after the bound), 1 the startup path before the Quad Views
// proof (private D3D12 SR without NR, then NR on the same textures; the idle
// view's resources given back at the next mission's create), 2 DLSS 5
// toggled off in flight (private D3D12 SR without NR, never DCS's feature).
#include "dcs_quad_focus_policy.hpp"
std::uint32_t dcs_created_quality{~0U};
void dcs_observe_created(const cheeky::foveated_dlss::NgxParameters* created) {
    dcs_created_quality = cheeky::foveated_dlss::get_ui(created, "PerfQualityValue");
}

// What the private D3D12 SR feature (fixture snippet) was created with.
std::uint32_t dcs_private_quality{~0U}, dcs_private_preset{~0U};
void dcs_observe_private_created(const cheeky::foveated_dlss::NgxParameters* created) {
    dcs_private_quality = cheeky::foveated_dlss::get_ui(created, "PerfQualityValue");
    dcs_private_preset = cheeky::foveated_dlss::get_ui(created, "DLSS.Hint.Render.Preset.Quality");
}

float dcs_half_to_float(std::uint16_t h) {
    const unsigned exponent = (h >> 10U) & 31U, mantissa = h & 1023U;
    const float value = exponent == 0U ? std::ldexp(float(mantissa), -24)
        : exponent == 31U ? INFINITY : std::ldexp(float(mantissa | 1024U), int(exponent) - 25);
    return (h & 0x8000U) ? -value : value;
}
std::array<float, 4> dcs_read_pixel(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture,
    unsigned x, unsigned y) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    desc.Width = desc.Height = 1U; desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0U;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0U;
    ComPtr<ID3D11Texture2D> staging;
    require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)), "Readback texture");
    const D3D11_BOX box{x, y, 0U, x + 1U, y + 1U, 1U};
    context->CopySubresourceRegion(staging.Get(), 0U, 0U, 0U, 0U, texture, 0U, &box);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    require(SUCCEEDED(context->Map(staging.Get(), 0U, D3D11_MAP_READ, 0U, &mapped)), "Map readback");
    std::uint16_t half[4]{};
    std::memcpy(half, mapped.pData, sizeof(half));
    context->Unmap(staging.Get(), 0U);
    return {dcs_half_to_float(half[0]), dcs_half_to_float(half[1]), dcs_half_to_float(half[2]), dcs_half_to_float(half[3])};
}
void dcs_clear(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture, const float (&value)[4]) {
    ComPtr<ID3D11UnorderedAccessView> view;
    require(SUCCEEDED(device->CreateUnorderedAccessView(texture, nullptr, &view)), "Clear view");
    context->ClearUnorderedAccessViewFloat(view.Get(), value);
}
bool dcs_pixel_is(const std::array<float, 4>& pixel, const float (&value)[4]) {
    for (unsigned i = 0; i < 4; ++i) if (std::fabs(pixel[i] - value[i]) > 0.002F) return false;
    return true;
}

void verify_dcs_deferred(CheekyRuntimeCommandFn command, CheekyRuntimeSnapshotFn get,
    std::uint64_t attachment, ID3D11Device* device, ID3D11DeviceContext* context, HMODULE ngx,
    const std::filesystem::path& log_path, const std::filesystem::path& bin, int scenario) {
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
    // Transport textures (shared D3D11/D3D12) allocated so far.
    const auto allocations = [&] {
        std::ifstream log(log_path);
        unsigned count{};
        for (std::string line; std::getline(log, line);)
            count += line.find("Transport texture ") != std::string::npos && line.find(" shared via ") != std::string::npos ? 1U : 0U;
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
    // The game's render: one flat colour, so any upscale of it is that colour.
    constexpr float scene[4]{0.25F, 0.5F, 0.75F, 1.0F};
    constexpr float sentinel[4]{1.0F, 0.0F, 1.0F, 1.0F};
    dcs_clear(device, context, textures[0].Get(), scene);
    const auto output = textures[3].Get();
    const auto output_written = [&] { return !dcs_pixel_is(dcs_read_pixel(device, context, output, 128U, 128U), sentinel); };
    const auto creates0 = counter(core, "CheekyFakeCreates"), releases0 = counter(core, "CheekyFakeReleases");
    const auto snippet_creates0 = counter(ngx, "CheekyFakeCreates");
    // The DLSS-NR runtime fixture is loaded with the first NR evaluation.
    const auto nr_evaluates = [&] { const auto module = GetModuleHandleW(L"nvngx_dlssnr.dll"); return module ? counter(module, "CheekyFakeEvaluates") : 0U; };

    if (scenario != 0) {
        // The adapted OpenXR layer's two exports, as published after a mission load.
        const auto layer = LoadLibraryW((bin / "test-fixtures/dcs-layer/CheekyOpenXRLayer.dll").c_str());
        require(layer != nullptr, "Load the Quad Views layer fixture");
        const auto publish = proc<void(*)(const CheekyGazeSnapshotV1*, const DcsQuadLayoutV1*)>(layer, "CheekyFakeDcsLayer");
        LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
        const auto qpc = [] { LARGE_INTEGER now{}; QueryPerformanceCounter(&now); return static_cast<std::uint64_t>(now.QuadPart); };
        IUnknown* identity{};
        require(SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&identity))), "Output identity");
        const auto output_identity = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(identity));
        identity->Release();
        // Layout: peripheral 128x128 (views 0/1), focus 256x256 (views 2/3).
        DcsQuadLayoutV1 layout{};
        layout.structure_size = sizeof(layout); layout.valid = 1U; layout.session_generation = 1U; layout.sequence = 1U;
        layout.width = {128U, 128U, 256U, 256U}; layout.height = {128U, 128U, 256U, 256U};
        layout.swapchain = {1U, 2U, 3U, 4U};
        // proven=false: last published a second ago (DCS was loading). proven=true: this
        // frame's snapshot names this NGX output as the left focus view (exact resource).
        const auto publish_frame = [&](bool proven) {
            CheekyGazeSnapshotV1 sample{};
            sample.abi_version = CHEEKY_GAZE_ABI_VERSION; sample.structure_size = sizeof(sample);
            sample.status_flags = dcs_quad_focus_required_flags; sample.view_count = 2U;
            sample.session_generation = 1U; sample.predicted_display_time = 1;
            sample.publication_qpc = qpc() - (proven ? 0U : static_cast<std::uint64_t>(frequency.QuadPart));
            for (unsigned eye = 0; eye < 2U; ++eye) {
                auto& view = sample.views[eye];
                view.structure_size = sizeof(view); view.view_index = eye; view.flags = CHEEKY_GAZE_VIEW_RESOURCE_VALID;
                view.image_rect_width = view.image_rect_height = 256U;
                view.resource_identity = eye == 0U ? output_identity : 0x1234U;
                view.swapchain_identity = layout.swapchain[2U + eye];
            }
            auto published = layout;
            published.publication_qpc = sample.publication_qpc;
            publish(&sample, &published);
        };
        // DCS creates its feature with one quality and later reuses the shared bag with other values:
        // the private SR must keep DCS's creation-time quality and preset hint on every path.
        proc<void(*)(void(*)(const NgxParameters*))>(ngx, "CheekyFakeObserveCreated")(&dcs_observe_private_created);
        parameters.Set("DLSS.Hint.Render.Preset.Quality", 3U);
        const auto diverge_bag = [&] { parameters.Set("PerfQualityValue", 0); parameters.Set("DLSS.Hint.Render.Preset.Quality", 0U); };
        const auto private_mirrors_creation = [&] {
            int bag_quality{-1};
            return dcs_private_quality == 2U && dcs_private_preset == 3U &&
                ngx_succeeded(parameters.Get("PerfQualityValue", &bag_quality)) && bag_quality == 0;
        };
        const auto frame = [&](const NgxHandle* handle, bool proven) {
            publish_frame(proven);
            require(ngx_succeeded(evaluate(context, handle, &parameters, nullptr)), "Focus view frame");
            context->Flush();
        };
        if (scenario == 1) {
            // 1. Mission start: DCS evaluates its focus views before the Quad Views proof exists.
            publish_frame(false);
            NgxHandle* focus{};
            require(ngx_succeeded(create(context, 1, &parameters, &focus)) && focus, "Deferred focus create");
            require(log_count("focus=layout") >= 1U, "The focus extent is classified from the published layout");
            diverge_bag();
            dcs_clear(device, context, output, sentinel);
            const auto core_evaluates0 = counter(core, "CheekyFakeEvaluates");
            const auto snippet_evaluates0 = counter(ngx, "CheekyFakeEvaluates");
            const auto nr_evaluates0 = nr_evaluates();
            for (unsigned i = 0; i < 8; ++i) frame(focus, false);
            require(log_count("reason=snapshot-not-ready") + log_count("size_route=layout-stale") >= 1U, "The gate rejects the unproven frames");
            require(counter(core, "CheekyFakeCreates") == creates0 && counter(core, "CheekyFakeEvaluates") == core_evaluates0 &&
                log_count("DCS deferred DX11 DLSS materialized") == 0U, "Unproven focus frames never create or evaluate DCS's own feature");
            require(log_count("kept on the private D3D12 SR without NR") >= 1U && log_count("reason=focus-proof-pending") >= 1U,
                "Unproven focus frames run on the private D3D12 SR");
            require(counter(ngx, "CheekyFakeEvaluates") >= snippet_evaluates0 + 8U && nr_evaluates() == nr_evaluates0,
                "Private SR evaluated every unproven frame, NR never");
            require(contains(snapshot(get), "\"execution_path\":\"DX12 Transport\"") && output_written(), "The transport writes the focus output");
            require(private_mirrors_creation() && log_count("creation-time DLSS parameters") >= 1U,
                "Unproven frames: the private SR uses DCS's creation-time quality and preset; DCS's bag is restored");
            const auto sr_only_allocations = allocations();
            require(sr_only_allocations >= 4U, "The SR-only frames built the view's transport textures");
            // 2. The proof arrives: NR starts on the same feature and textures.
            bool active{};
            for (unsigned i = 0; i < 100 && !active; ++i) {
                frame(focus, true);
                active = contains(snapshot(get), "\"nr\":\"Active\"") && nr_evaluates() > nr_evaluates0;
            }
            for (unsigned i = 0; i < 6; ++i) frame(focus, true);
            if (!active) puts(snapshot(get).c_str());
            require(active && log_count("mapped focus accepted") >= 1U, "NR starts once the focus proof arrives");
            require(allocations() == sr_only_allocations, "NR resumes on the SR-only textures: nothing is rebuilt");
            require(private_mirrors_creation(), "With NR the private SR keeps DCS's creation-time quality and preset");
            require(counter(core, "CheekyFakeCreates") == creates0 && counter(core, "CheekyFakeEvaluates") == core_evaluates0,
                "The proven focus view never needed DCS's own feature");
            // 3. DCS never releases a mission's features: the next mission's create gives back the idle
            //    view's private resources, and the old handle still works should DCS ever use it again.
            Sleep(5200U);
            const auto idle_before = log_count("FEATURE_IDLE released private resources");
            NgxHandle* next_mission{};
            require(ngx_succeeded(create(context, 1, &parameters, &next_mission)) && next_mission, "Next mission's focus create");
            require(log_count("FEATURE_IDLE released private resources") == idle_before + 1U,
                "The next create releases the idle view's private resources");
            const auto released_allocations = allocations();
            dcs_clear(device, context, output, sentinel);
            for (unsigned i = 0; i < 6; ++i) frame(focus, true);
            require(output_written() && allocations() > released_allocations,
                "An idle-released view is rebuilt on its next evaluation");
            require(counter(core, "CheekyFakeCreates") == creates0, "Rebuilding never needs DCS's own feature");
            require(ngx_succeeded(release(next_mission)), "Release the next mission's focus");
            require(ngx_succeeded(release(focus)) && counter(core, "CheekyFakeReleases") == releases0, "Release without any NGX release");
            // 4. A view created before any layout that turns out peripheral gets DCS's own feature at once.
            publish(nullptr, nullptr);
            MockNgxParameters peripheral_parameters = parameters;
            peripheral_parameters.Set("Width", 64U); peripheral_parameters.Set("Height", 64U);
            peripheral_parameters.Set("OutWidth", 128U); peripheral_parameters.Set("OutHeight", 128U);
            NgxHandle* peripheral{};
            require(ngx_succeeded(create(context, 1, &peripheral_parameters, &peripheral)) && peripheral &&
                log_count("focus=unclassified") >= 1U, "Unclassifiable create is deferred");
            publish_frame(false);
            require(ngx_succeeded(evaluate(context, peripheral, &peripheral_parameters, nullptr)), "Peripheral frame");
            require(counter(core, "CheekyFakeCreates") == creates0 + 1U && log_count("reason=not-a-focus-extent") == 1U,
                "A peripheral extent creates DCS's own feature on its first frame");
            require(ngx_succeeded(release(peripheral)) && counter(core, "CheekyFakeReleases") == releases0 + 1U, "Peripheral release");
        } else {
            // DLSS 5 toggled off in flight on a proven focus view, then on again.
            publish_frame(true);
            NgxHandle* focus{};
            require(ngx_succeeded(create(context, 1, &parameters, &focus)) && focus, "Deferred focus create");
            diverge_bag();
            bool active{};
            for (unsigned i = 0; i < 100 && !active; ++i) {
                frame(focus, true);
                active = contains(snapshot(get), "\"nr\":\"Active\"");
            }
            for (unsigned i = 0; i < 4; ++i) frame(focus, true);
            require(active, "DLSS 5 active on the focus view");
            const auto with_nr = allocations();
            const auto core_evaluates0 = counter(core, "CheekyFakeEvaluates");
            require(command(attachment, "1\n41\nset_session\nNrEnabled=0"), "DLSS 5 key: off");
            const auto nr_evaluates_off = nr_evaluates();
            const auto snippet_evaluates_off = counter(ngx, "CheekyFakeEvaluates");
            dcs_clear(device, context, output, sentinel);
            for (unsigned i = 0; i < 8; ++i) frame(focus, true);
            require(counter(core, "CheekyFakeCreates") == creates0 && counter(core, "CheekyFakeEvaluates") == core_evaluates0 &&
                log_count("DCS deferred DX11 DLSS materialized") == 0U, "DLSS 5 off never creates or evaluates DCS's own feature");
            require(log_count("reason=dlss5-off") >= 1U && contains(snapshot(get), "\"execution_path\":\"DX12 Transport\""),
                "DLSS 5 off keeps the private D3D12 SR");
            require(counter(ngx, "CheekyFakeEvaluates") >= snippet_evaluates_off + 8U && nr_evaluates() == nr_evaluates_off,
                "Private SR every frame, no NR, while DLSS 5 is off");
            require(output_written() && allocations() == with_nr, "Valid output from the same transport textures");
            require(private_mirrors_creation(), "DLSS 5 off: the private SR keeps DCS's creation-time quality and preset");
            // A feature DCS creates while DLSS 5 is off (mission restart) is still deferred.
            NgxHandle* later{};
            const auto deferred_creates = log_count("DCS deferred DX11 DLSS create");
            require(ngx_succeeded(create(context, 1, &parameters, &later)) && later &&
                log_count("DCS deferred DX11 DLSS create") == deferred_creates + 1U &&
                counter(core, "CheekyFakeCreates") == creates0, "Creation with DLSS 5 off is deferred too");
            require(ngx_succeeded(release(later)) && counter(core, "CheekyFakeReleases") == releases0, "Release of the never-used feature");
            require(command(attachment, "1\n42\nset_session\nNrEnabled=1"), "DLSS 5 key: on");
            for (unsigned i = 0; i < 6; ++i) frame(focus, true);
            require(nr_evaluates() >= nr_evaluates_off + 6U && contains(snapshot(get), "\"nr\":\"Active\"") &&
                allocations() == with_nr, "DLSS 5 resumes on the same textures");
            require(ngx_succeeded(release(focus)) && counter(core, "CheekyFakeReleases") == releases0 &&
                counter(core, "CheekyFakeCreates") == creates0, "The focus view never created DCS's own feature");
        }
        context->Flush();
        return;
    }

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

    // 3. No Quad Views layout at all (no OpenXR layer): a bounded bilinear hold,
    //    then DCS's own feature, created with the creation-time values.
    NgxHandle* handle{};
    require(ngx_succeeded(create(context, 1, &parameters, &handle)) && handle, "Second deferred create");
    parameters.Set("PerfQualityValue", 0); // DCS reuses one bag; evaluation values differ from creation.
    dcs_clear(device, context, output, sentinel);
    const auto held_started = GetTickCount64();
    const auto hold_evaluates0 = counter(core, "CheekyFakeEvaluates");
    require(ngx_succeeded(evaluate(context, handle, &parameters, nullptr)), "Held frame without a layout");
    require(counter(core, "CheekyFakeCreates") == creates0 && counter(core, "CheekyFakeEvaluates") == hold_evaluates0 &&
        log_count("held with a bilinear upscale") == 1U, "No layout yet: a bilinear hold, no NGX feature");
    require(dcs_pixel_is(dcs_read_pixel(device, context, output, 128U, 128U), scene) &&
        dcs_pixel_is(dcs_read_pixel(device, context, output, 0U, 0U), scene) &&
        dcs_pixel_is(dcs_read_pixel(device, context, output, 255U, 255U), scene), "The held frame is the upscaled render");
    while (GetTickCount64() - held_started <= 3100U) Sleep(50);
    const auto core_evaluates0 = counter(core, "CheekyFakeEvaluates");
    require(ngx_succeeded(evaluate(context, handle, &parameters, nullptr)), "Fallback evaluation of the deferred feature");
    require(counter(core, "CheekyFakeCreates") == creates0 + 1U && counter(ngx, "CheekyFakeCreates") == snippet_creates0 + 1U,
        "Fallback materializes exactly one real feature");
    require(log_count("reason=layout-unknown-timeout") == 1U, "The bounded hold ends in DCS's own feature");
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
