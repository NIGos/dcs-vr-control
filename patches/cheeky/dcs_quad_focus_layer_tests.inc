void dcs_quad_focus_policy_unit_test() {
    CheekyGazeSnapshotV1 s{};
    s.abi_version = CHEEKY_GAZE_ABI_VERSION; s.structure_size = sizeof(s); s.view_count = 2;
    s.session_generation = 7; s.predicted_display_time = 1; s.publication_qpc = 1000;
    s.status_flags = dcs_quad_focus_required_flags;
    DcsQuadLayoutV1 l{};
    l.structure_size = sizeof(l); l.valid = 1; l.session_generation = 7; l.publication_qpc = 1000;
    l.width = {1764, 1764, 1040, 1040}; l.height = {2480, 2480, 1028, 1028}; l.array_index = {0, 0, 0, 0};
    l.swapchain = {1, 2, 3, 4};
    for (unsigned eye = 0; eye < 2; ++eye) {
        auto& v = s.views[eye];
        v.flags = CHEEKY_GAZE_VIEW_RESOURCE_VALID | dcs_quad_focus_single_mip;
        v.resource_identity = 30 + eye; v.swapchain_identity = 3 + eye;
        v.image_rect_width = 1040; v.image_rect_height = 1028;
    }
    const auto no_copy = [](const CheekyGazeViewV1&) { return false; };
    DcsQuadHandleCensus census;
    const auto decide = [&](std::uint64_t handle, unsigned w, unsigned h, const CheekyGazeSnapshotV1& snap,
        const DcsQuadLayoutV1* layout, std::uint64_t now_ms, bool eyes = false) {
        return dcs_quad_focus_decide(handle, true, snap, 1001, 1000, 0x900 + handle, 0, 0, w, h, no_copy,
            layout, census, now_ms, eyes);
    };
    // Cold start: the census sees one feature only and refuses.
    auto d = decide(1, 1040, 1028, s, &l, 100);
    require(!d.allowed && d.size == DcsQuadSizeProof::handle_census, "Incomplete census accepted");
    decide(2, 1040, 1028, s, &l, 101); decide(3, 1764, 2480, s, &l, 102); decide(4, 1764, 2480, s, &l, 103);
    d = decide(1, 1040, 1028, s, &l, 110);
    require(d.allowed && std::string(d.route) == "size-bijection" && d.census.focus == 2 && d.census.peripheral == 2,
        "Complete focus/peripheral census rejected");
    require(!decide(3, 1764, 2480, s, &l, 111).allowed, "Peripheral accepted by size proof");
    // A fifth live feature of any size breaks the bijection.
    decide(5, 640, 480, s, &l, 112);
    require(decide(1, 1040, 1028, s, &l, 113).size == DcsQuadSizeProof::handle_census, "Extra feature ignored");
    // It expires after the census window.
    for (std::uint64_t h = 1; h <= 4; ++h) decide(h, h <= 2 ? 1040 : 1764, h <= 2 ? 1028 : 2480, s, &l, 500);
    require(decide(1, 1040, 1028, s, &l, 501).allowed, "Expired feature still counted");
    // Three focus-sized features (e.g. a peripheral rendered at focus size) are ambiguous.
    decide(6, 1040, 1028, s, &l, 502);
    require(!decide(1, 1040, 1028, s, &l, 503).allowed, "Three focus-sized features accepted");
    census.clear();
    for (std::uint64_t h = 1; h <= 4; ++h) decide(h, h <= 2 ? 1040 : 1764, h <= 2 ? 1028 : 2480, s, &l, 900);
    auto same = l; same.width = {1040, 1040, 1040, 1040}; same.height = {1028, 1028, 1028, 1028};
    require(decide(1, 1040, 1028, s, &same, 901).size == DcsQuadSizeProof::peripheral_equals_focus,
        "Equal peripheral/focus extents accepted");
    auto uneven = l; uneven.width[3] = 1041;
    require(decide(1, 1040, 1028, s, &uneven, 901).size == DcsQuadSizeProof::focus_pair_mismatch, "Uneven focus pair accepted");
    auto stale = l; stale.publication_qpc = 700;
    require(decide(1, 1040, 1028, s, &stale, 901).size == DcsQuadSizeProof::layout_stale, "Stale layout accepted");
    auto other_session = l; other_session.session_generation = 8;
    require(decide(1, 1040, 1028, s, &other_session, 901).size == DcsQuadSizeProof::layout_session, "Foreign layout accepted");
    auto moved = l; moved.swapchain[2] = 9;
    require(decide(1, 1040, 1028, s, &moved, 901).size == DcsQuadSizeProof::snapshot_layout_mismatch, "Layout/snapshot mismatch accepted");
    require(decide(1, 1040, 1028, s, nullptr, 901).size == DcsQuadSizeProof::layout_unavailable, "Missing layout accepted");
    require(decide(1, 1040, 1028, s, &l, 901, true).size == DcsQuadSizeProof::eye_identity_required, "Eye-dependent size proof accepted");
    require(decide(1, 1040, 1028, s, &l, 901).allowed, "Valid census rejected after edge cases");
    // Snapshot readiness reports exactly what is missing.
    auto unfocused = s; unfocused.status_flags &= ~CHEEKY_GAZE_STATUS_SESSION_FOCUSED;
    d = decide(1, 1040, 1028, unfocused, &l, 902);
    require(d.reason == DcsQuadFocusReason::snapshot_not_ready && d.missing_flags == CHEEKY_GAZE_STATUS_SESSION_FOCUSED &&
        (d.snapshot_problems & dcs_snapshot_missing_flags), "Missing focus flag not reported");
    auto late = s; late.publication_qpc = 700;
    require(decide(1, 1040, 1028, late, &l, 902).snapshot_problems == dcs_snapshot_stale, "Stale snapshot not reported");
    require(dcs_quad_focus_decide(1, false, s, 1001, 1000, 1, 0, 0, 1040, 1028, no_copy, &l, census, 902, false).reason ==
        DcsQuadFocusReason::snapshot_unavailable, "Missing layer not reported");
    // Exact identity still wins and is labelled; two exact matches stay ambiguous.
    d = dcs_quad_focus_decide(1, true, s, 1001, 1000, 30, 0, 0, 1040, 1028, no_copy, &l, census, 903, true);
    require(d.allowed && std::string(d.route) == "exact-resource", "Exact focus identity rejected");
    auto twin = s; twin.views[1].resource_identity = 30;
    d = dcs_quad_focus_decide(1, true, twin, 1001, 1000, 30, 0, 0, 1040, 1028, no_copy, &l, census, 903, false);
    require(!d.allowed && d.reason == DcsQuadFocusReason::ambiguous_match, "Duplicate exact identity accepted");
    // Transient-rejection hold: a size-proven focus handle stays accepted through
    // stale/census rejections for at most dcs_quad_focus_hold_ms after its last
    // proof; any definite counter-proof releases it immediately.
    {
        const auto prove = [&](std::uint64_t at, DcsQuadFocusHold& hold) {
            for (std::uint64_t h = 1; h <= 4; ++h) decide(h, h <= 2 ? 1040 : 1764, h <= 2 ? 1028 : 2480, s, &l, at);
            auto proven = decide(1, 1040, 1028, s, &l, at + 1);
            require(proven.allowed && std::string(proven.route) == "size-bijection" &&
                !dcs_quad_focus_apply_hold(proven, hold, s, &l, 1040, 1028, at + 1, false) && hold.valid &&
                hold.proven_ms == at + 1, "Size-proven focus not recorded for the hold");
        };
        const auto hold_of = [&](DcsQuadFocusDecision decision, DcsQuadFocusHold& hold, const CheekyGazeSnapshotV1& snap,
            const DcsQuadLayoutV1* layout, unsigned w, unsigned h, std::uint64_t at, bool eyes = false) {
            const bool held = dcs_quad_focus_apply_hold(decision, hold, snap, layout, w, h, at, eyes);
            require(held == decision.allowed || !held, "Hold result inconsistent");
            return std::pair{held, decision};
        };
        auto stale_snapshot = s; stale_snapshot.publication_qpc = 700;
        census.clear();
        DcsQuadFocusHold hold;
        prove(3000, hold);
        // Accepted -> 300 ms of stale snapshots -> still accepted, as a hold.
        auto stale_d = decide(1, 1040, 1028, stale_snapshot, &l, 3301);
        require(!stale_d.allowed && stale_d.reason == DcsQuadFocusReason::snapshot_not_ready && dcs_quad_focus_transient(stale_d),
            "Stale snapshot is not a transient rejection");
        auto [held, out] = hold_of(stale_d, hold, stale_snapshot, &l, 1040, 1028, 3301);
        require(held && out.allowed && std::string(out.route) == "transient-hold" && hold.holding,
            "Accepted focus released after a 300 ms stale snapshot");
        // Census incomplete (the other features' entries expired) and a stale layout are transient too.
        auto census_d = decide(1, 1040, 1028, s, &l, 3302);
        require(census_d.size == DcsQuadSizeProof::handle_census && hold_of(census_d, hold, s, &l, 1040, 1028, 3302).first,
            "Incomplete census released a held focus");
        auto old_layout = l; old_layout.publication_qpc = 700;
        auto layout_d = decide(1, 1040, 1028, s, &old_layout, 3303);
        require(layout_d.size == DcsQuadSizeProof::layout_stale && hold_of(layout_d, hold, s, &old_layout, 1040, 1028, 3303).first,
            "Stale layout released a held focus");
        // -> definite peripheral extent on the same handle: rejected, hold gone.
        auto peripheral_d = decide(1, 1764, 2480, s, &l, 3304);
        auto [peripheral_held, peripheral_out] = hold_of(peripheral_d, hold, s, &l, 1764, 2480, 3304);
        require(!peripheral_held && !peripheral_out.allowed && !hold.valid, "Peripheral extent accepted through the hold");
        require(!hold_of(decide(1, 1040, 1028, stale_snapshot, &l, 3305), hold, stale_snapshot, &l, 1040, 1028, 3305).first,
            "Hold survived a definite rejection");
        // Eye identity required never holds.
        prove(3400, hold);
        require(!hold_of(decide(1, 1040, 1028, stale_snapshot, &l, 3402, true), hold, stale_snapshot, &l, 1040, 1028, 3402, true).first &&
            !hold.valid, "Hold used while eye identity is required");
        // Session change or a layout whose peripheral now has the focus extent are definite.
        prove(3500, hold);
        auto next_session = stale_snapshot; next_session.session_generation = 8;
        require(!hold_of(decide(1, 1040, 1028, next_session, &l, 3502), hold, next_session, &l, 1040, 1028, 3502).first,
            "Hold crossed a session change");
        prove(3600, hold);
        auto equal_layout = old_layout; equal_layout.width[0] = equal_layout.width[1] = 1040;
        equal_layout.height[0] = equal_layout.height[1] = 1028;
        require(!hold_of(decide(1, 1040, 1028, s, &equal_layout, 3602), hold, s, &equal_layout, 1040, 1028, 3602).first,
            "Hold kept a handle whose extent is now peripheral");
        // Grace expiry: still held at the limit, rejected just after it.
        prove(4000, hold);
        require(hold_of(decide(1, 1040, 1028, stale_snapshot, &l, 4001 + dcs_quad_focus_hold_ms), hold, stale_snapshot, &l,
            1040, 1028, 4001 + dcs_quad_focus_hold_ms).first, "Hold ended before its grace period");
        auto expired = decide(1, 1040, 1028, stale_snapshot, &l, 4002 + dcs_quad_focus_hold_ms);
        auto [expired_held, expired_out] = hold_of(expired, hold, stale_snapshot, &l, 1040, 1028, 4002 + dcs_quad_focus_hold_ms);
        require(!expired_held && !expired_out.allowed && !hold.valid, "Hold outlived its grace period");
        // Exact-identity acceptance does not arm the size hold.
        auto exact = dcs_quad_focus_decide(1, true, s, 1001, 1000, 30, 0, 0, 1040, 1028, no_copy, &l, census, 4100, false);
        require(exact.allowed && !dcs_quad_focus_apply_hold(exact, hold, s, &l, 1040, 1028, 4100, false) && !hold.valid,
            "Exact route armed the size hold");
        census.clear();
    }

    // Trace limiter: first four, powers of two, reason changes at most once per second.
    DcsQuadFocusTrace trace;
    unsigned logged{};
    for (unsigned i = 0; i < 100; ++i) logged += trace.should_log(1, 0, 5000) ? 1U : 0U;
    require(logged == 8, "Trace limiter not power-of-two");  // 1,2,3,4,8,16,32,64
    require(trace.should_log(2, 0, 5001) == false && trace.should_log(3, 0, 6002), "Reason change limiter wrong");

    // Configurable DLSS-NR hotkey: snapshot parsing, edge detection, foreground and command format.
    bool nr{};
    require(cheeky::standalone::dcs_nr_enabled_from_snapshot(
        R"({"protocol":1,"settings":{"Enabled":false,"NrEnabled":true,"NrFoveated":false},"message":""})", nr) && nr,
        "NrEnabled=true not parsed");
    require(cheeky::standalone::dcs_nr_enabled_from_snapshot(R"({"settings":{"NrEnabled":false}})", nr) && !nr,
        "NrEnabled=false not parsed");
    require(!cheeky::standalone::dcs_nr_enabled_from_snapshot(R"({"settings":{"Enabled":true},"NrEnabled":true})", nr),
        "NrEnabled outside settings accepted");
    cheeky::standalone::DcsNrHotkeySpec spec{};
    require(cheeky::standalone::dcs_nr_parse_hotkey("123:5", spec) && spec.virtual_key == 123U && spec.modifiers == 5U, "Hotkey spec not parsed");
    require(!cheeky::standalone::dcs_nr_parse_hotkey("0:0", spec) && !cheeky::standalone::dcs_nr_parse_hotkey("12", spec) && !cheeky::standalone::dcs_nr_parse_hotkey("12:9", spec), "Invalid hotkey spec accepted");
    cheeky::standalone::dcs_nr_parse_hotkey("123:5", spec);
    const auto held = [](std::vector<unsigned> keys) { return [keys](unsigned vk) { for (auto k : keys) if (k == vk) return true; return false; }; };
    require(cheeky::standalone::dcs_nr_hotkey_down(spec, held({123U, 0x11U, 0x10U})), "Ctrl+Shift+F12 not detected");
    require(!cheeky::standalone::dcs_nr_hotkey_down(spec, held({123U, 0x11U, 0x10U, 0x12U})), "Extra Alt accepted");
    require(!cheeky::standalone::dcs_nr_hotkey_down(spec, held({123U, 0x11U})), "Missing Shift accepted");
    cheeky::standalone::DcsNrHotkey hotkey;
    std::string state = R"({"settings":{"Enabled":false,"NrEnabled":true}})", sent, logged_text;
    unsigned commands{};
    const auto snapshot_fn = [&](std::string& out) { out = state; return true; };
    const auto command_fn = [&](const char* text) {
        sent = text; ++commands;
        const bool on = sent.find("NrEnabled=1") != std::string::npos;
        state = std::string(R"({"settings":{"Enabled":false,"NrEnabled":)") + (on ? "true" : "false") + "}}";
        return true;
    };
    const auto log_fn = [&](const char* text) { logged_text = text; };
    using R = cheeky::standalone::DcsNrHotkey::Result;
    require(hotkey.poll(true, false, snapshot_fn, command_fn, log_fn) == R::idle && commands == 0, "Background window toggled NR");
    require(hotkey.poll(false, true, snapshot_fn, command_fn, log_fn) == R::idle, "Key release toggled NR");
    require(hotkey.poll(true, true, snapshot_fn, command_fn, log_fn) == R::toggled && commands == 1, "Hotkey press ignored");
    require(sent.starts_with("1\n") && sent.find("\nset_session\nNrEnabled=0\n") != std::string::npos, "Toggle command malformed");
    require(logged_text == "DLSS-NR hotkey toggled enabled=no", "Toggle not logged");
    require(hotkey.poll(true, true, snapshot_fn, command_fn, log_fn) == R::idle && commands == 1, "Held key repeated the toggle");
    hotkey.poll(false, true, snapshot_fn, command_fn, log_fn);
    require(hotkey.poll(true, true, snapshot_fn, command_fn, log_fn) == R::toggled && sent.find("NrEnabled=1") != std::string::npos &&
        logged_text == "DLSS-NR hotkey toggled enabled=yes", "Second press did not re-enable NR");
    hotkey.poll(false, true, snapshot_fn, command_fn, log_fn);
    require(hotkey.poll(true, true, [](std::string&) { return false; }, command_fn, log_fn) == R::snapshot_failed &&
        commands == 2, "Toggle without runtime settings");
}

void dcs_quad_focus_layer_test() {
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &context));
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 128; desc.Height = 128; desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR}; binding.device = device.Get();
    for (unsigned variant = 0; variant < 4; ++variant) {
        SetEnvironmentVariableW(L"DCSVR_QUAD_FOCUS", L"1");
        const bool array = variant != 0;
        desc.ArraySize = array ? 4 : 1;
        ComPtr<ID3D11Texture2D> texture; check(device->CreateTexture2D(&desc, nullptr, &texture));
        XRLayer layer(texture.Get(), 11, &binding, 128, desc.ArraySize, 128);
        XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
        begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO;
        require(layer.fn<PFN_xrBeginSession>("xrBeginSession")(layer.session, &begin) == XR_SUCCESS, "Quad session forwarding failed");
        layer.begin(); layer.release();
        std::array<XrCompositionLayerProjectionView, 4> views{};
        for (unsigned i = 0; i < 4; ++i) {
            views[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            views[i].pose.orientation.w = 1;
            views[i].fov = {-.5F, .5F, .5F, -.5F};
            views[i].subImage = {layer.chain,
                {{array ? 0 : static_cast<int>((i % 2) * 64), 0}, {i < 2 ? 32 : (array ? 128 : 64), 128}},
                array ? i : 0};
        }
        if (variant == 2) views[2].subImage.imageArrayIndex = 4;
        if (variant == 3) views[2].subImage.imageRect.offset.x = 1;
        XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        projection.viewCount = 4; projection.views = views.data();
        const auto* p = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
        XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO}; info.displayTime = 1000000; info.layerCount = 1; info.layers = &p;
        require(layer.fn<PFN_xrEndFrame>("xrEndFrame")(layer.session, &info) == XR_SUCCESS, "Four-view EndFrame forwarding failed");
        auto snapshot_fn = reinterpret_cast<CheekyOpenXRGetGazeSnapshotFn>(GetProcAddress(layer.module, "CheekyOpenXR_GetGazeSnapshot"));
        require(snapshot_fn != nullptr, "Gaze snapshot export missing");
        CheekyGazeSnapshotV1 snapshot{};
        require(snapshot_fn(CHEEKY_GAZE_ABI_VERSION, &snapshot, sizeof(snapshot)) != 0, "Quad snapshot unavailable");
        require(snapshot.view_count == 2 && (snapshot.status_flags & (1U << 12U)), "Focus pair adapter status missing");
        require(!(snapshot.status_flags & CHEEKY_GAZE_STATUS_UNSUPPORTED_VIEW_CONFIG), "Adapted quad marked unsupported");
        require(snapshot.views[0].image_rect_width == (array ? 128U : 64U) &&
            snapshot.views[1].image_rect_width == (array ? 128U : 64U),
            "Peripheral views were selected instead of focus views");
        if (!array)
            require(snapshot.views[0].image_rect_x == 0 && snapshot.views[1].image_rect_x == 64, "Focus eye order changed");
        else if (variant == 1) {
            require(snapshot.views[0].array_index == 2 && snapshot.views[1].array_index == 3, "Focus array order changed");
            require((snapshot.views[0].flags & (1U << 9U)) && (snapshot.views[1].flags & (1U << 9U)),
                "Array mip layout proof missing");
        }
        require(bool(snapshot.status_flags & CHEEKY_GAZE_STATUS_AMBIGUOUS_RESOURCE) == (variant >= 2),
            "Invalid focus array/rectangle was not rejected");
        if (variant >= 2)
            require(!(snapshot.status_flags & CHEEKY_GAZE_STATUS_MAPPING_READY), "Invalid focus layout was marked ready");
        require(projection.viewCount == 4 && views[0].subImage.imageRect.extent.width == 32,
            "Application submission was mutated");
        const auto layout_fn = reinterpret_cast<DcsQuadLayoutFn>(GetProcAddress(layer.module, dcs_quad_layout_export));
        require(layout_fn != nullptr, "Quad layout export missing");
        DcsQuadLayoutV1 quad_layout{};
        require(layout_fn(&quad_layout, sizeof(quad_layout)) != 0 && quad_layout.structure_size == sizeof(quad_layout) &&
            quad_layout.publication_qpc != 0, "Quad layout unavailable");
        require(quad_layout.session_generation == snapshot.session_generation, "Quad layout session mismatch");
        const unsigned focus_width = array ? 128U : 64U;
        if (variant < 2) {
            require(quad_layout.valid == 1 && quad_layout.width[0] == 32 && quad_layout.width[1] == 32 &&
                quad_layout.width[2] == focus_width && quad_layout.width[3] == focus_width &&
                quad_layout.height[0] == 128 && quad_layout.height[3] == 128, "Quad layout extents wrong");
            require(quad_layout.swapchain[2] == snapshot.views[0].swapchain_identity &&
                quad_layout.array_index[3] == snapshot.views[1].array_index, "Quad layout disagrees with focus snapshot");
            // Real layer output feeds the gate decision: only the census and
            // focus extent decide, and an eye-dependent mode is refused.
            auto ready = snapshot;
            ready.status_flags |= CHEEKY_GAZE_STATUS_SESSION_FOCUSED;
            if (!ready.predicted_display_time) ready.predicted_display_time = 1000000; // Fixture has no xrWaitFrame.
            const std::uint64_t now = (std::max)(ready.publication_qpc, quad_layout.publication_qpc) + 1;
            DcsQuadHandleCensus census;
            census.record(101, focus_width, 128, 1000); census.record(102, focus_width, 128, 1000);
            census.record(103, 32, 128, 1000); census.record(104, 32, 128, 1000);
            const auto no_copy = [](const CheekyGazeViewV1&) { return false; };
            auto d = dcs_quad_focus_decide(101, true, ready, now, 1000000, 0x77, 0, 0, focus_width, 128, no_copy,
                &quad_layout, census, 1001, false);
            require(d.allowed && std::string(d.route) == "size-bijection", (std::string("Size-bijection proof rejected real layout: ") +
                dcs_quad_focus_reason_name(d.reason) + " problems=" + std::to_string(d.snapshot_problems) +
                " size=" + dcs_quad_size_proof_name(d.size)).c_str());
            d = dcs_quad_focus_decide(103, true, ready, now, 1000000, 0x78, 0, 0, 32, 128, no_copy,
                &quad_layout, census, 1001, false);
            require(!d.allowed && d.size == DcsQuadSizeProof::output_not_focus_size, "Peripheral feature accepted");
            d = dcs_quad_focus_decide(101, true, ready, now, 1000000, 0x77, 0, 0, focus_width, 128, no_copy,
                &quad_layout, census, 1001, true);
            require(!d.allowed && d.size == DcsQuadSizeProof::eye_identity_required, "Eye-dependent mode used size proof");
        } else {
            require(!quad_layout.valid, "Invalid focus layout published as valid");
        }
        SetEnvironmentVariableW(L"DCSVR_QUAD_FOCUS", nullptr);
        layer.fn<PFN_xrEndFrame>("xrEndFrame")(layer.session, &info);
        snapshot_fn(CHEEKY_GAZE_ABI_VERSION, &snapshot, sizeof(snapshot));
        require(snapshot.status_flags & CHEEKY_GAZE_STATUS_UNSUPPORTED_VIEW_CONFIG, "Disabled adapter accepted four views");
    }
    dcs_quad_focus_policy_unit_test();
    std::cout << "PASS DCS quad focus: real layer DLL, WARP packed/array mapping, invalid layout rejection, unchanged submission, layout export, gate decisions, NR hotkey\n";
}
