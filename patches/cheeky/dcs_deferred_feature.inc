// DCS VR Control: deferred DX11 DLSS game feature for Quad Views focus views.
// Included inside the anonymous namespace of hooks.cpp, after the DX11
// evaluation pipeline (evaluate_d3d11_impl, evaluate_with_eye_calibration).
//
// With the D3D11->D3D12 transport, a focus view is upscaled by a private D3D12
// SR feature, so DCS's own DX11 SR feature for that view sits unused in VRAM.
// DCS calls NVSDK_NGX_D3D11_CreateFeature on the NGX core (_nvngx.dll); under
// DCSVR_QUAD_FOCUS=1 and an SR-replacing transport configuration the core hook
// returns an opaque handle instead and stores the creation parameters. The real
// core feature is created only when something evaluates DCS's feature:
//   * every evaluation of an opaque handle enters the core EvaluateFeature hook,
//     which runs the normal DX11 pipeline with a runtime whose "original"
//     evaluate materializes the real feature first (fallback, disabled
//     passthrough, native-SR NR, private-path fallback: all of them call it);
//   * ReleaseFeature releases the real feature only if it exists;
//   * D3D11 Shutdown forgets real features the core destroys itself.
// Once created, the real feature lives until DCS releases its handle: NGX's
// DX11 runtime does not return a released feature's VRAM while the game's other
// DX11 DLSS features (the peripheral views) stay alive (measured: 0 MiB back
// for a 254.5 MiB focus feature), so releasing it early would only add a create
// stall to the next fallback. The saving is the feature never evaluated:
// ~3.4 MiB when only created, ~254.5 MiB per 2076x2048 focus view once evaluated.

using NgxD3D11ShutdownFn = NgxResult (*)();
using NgxD3D11Shutdown1Fn = NgxResult (*)(ID3D11Device*);
std::atomic<EvaluateD3D11Fn> real_core_evaluate_d3d11{};
std::atomic<EvaluateD3D11CFn> real_core_evaluate_d3d11_c{};
std::atomic<NgxD3D11ShutdownFn> real_core_shutdown_d3d11{};
std::atomic<NgxD3D11Shutdown1Fn> real_core_shutdown_d3d11_1{};

constexpr unsigned dcs_deferred_magic = 0x44435346U; // "DCSF"

[[nodiscard]] bool dcs_quad_focus_env() noexcept {
    wchar_t value[2]{};
    return GetEnvironmentVariableW(L"DCSVR_QUAD_FOCUS", value, 2) == 1 && value[0] == L'1';
}

// What DCS holds instead of a core handle. NVSDK_NGX_Handle is { unsigned Id; }.
struct DcsDeferredHandle { unsigned id; unsigned magic; };

// Creation-time values replayed when the real feature is created. DCS shares
// one parameter object between its features and changes sizes between calls,
// so evaluation-time values are not the creation values.
struct DcsCreateValue { const char* name; bool is_int; bool present; unsigned value; };
constexpr const char* dcs_create_uint_keys[]{"Width", "Height", "OutWidth", "OutHeight", "CreationNodeMask",
    "VisibilityNodeMask", "DLSS.Hint.Render.Preset.DLAA", "DLSS.Hint.Render.Preset.Quality",
    "DLSS.Hint.Render.Preset.Balanced", "DLSS.Hint.Render.Preset.Performance",
    "DLSS.Hint.Render.Preset.UltraPerformance", "DLSS.Hint.Render.Preset.UltraQuality"};
constexpr const char* dcs_create_int_keys[]{"PerfQualityValue", "RTXValue", "DLSS.Feature.Create.Flags",
    "DLSS.Enable.Output.Subrects"};
constexpr std::size_t dcs_create_key_count = std::size(dcs_create_uint_keys) + std::size(dcs_create_int_keys);
using DcsCreateValues = std::array<DcsCreateValue, dcs_create_key_count>;

[[nodiscard]] DcsCreateValues dcs_capture_create_values(const NgxParameters* parameters) noexcept {
    DcsCreateValues values{};
    std::size_t index{};
    for (const auto* name : dcs_create_uint_keys) {
        auto& v = values[index++]; v.name = name;
        unsigned u{}; int i{};
        if (parameters && ngx_succeeded(parameters->Get(name, &u))) { v.present = true; v.value = u; }
        else if (parameters && ngx_succeeded(parameters->Get(name, &i))) { v.present = v.is_int = true; v.value = static_cast<unsigned>(i); }
    }
    for (const auto* name : dcs_create_int_keys) {
        auto& v = values[index++]; v.name = name; v.is_int = true;
        unsigned u{}; int i{};
        if (parameters && ngx_succeeded(parameters->Get(name, &i))) { v.present = true; v.value = static_cast<unsigned>(i); }
        else if (parameters && ngx_succeeded(parameters->Get(name, &u))) { v.present = true; v.is_int = false; v.value = u; }
    }
    return values;
}

void dcs_apply_create_value(NgxParameters* parameters, const DcsCreateValue& v) noexcept {
    if (v.is_int) parameters->Set(v.name, static_cast<int>(v.value));
    else parameters->Set(v.name, v.value);
}

struct DcsDeferredFeature {
    DcsDeferredHandle* fake{};
    NgxHandle* real{};
    std::uint32_t feature{};
    std::size_t slot{};
    DcsCreateValues create{};
    std::uint64_t created_ms{}, materializations{};
};
std::mutex dcs_deferred_mutex;
std::vector<DcsDeferredFeature> dcs_deferred_features;
std::atomic<unsigned> dcs_deferred_count{};
std::atomic<unsigned> dcs_deferred_next_id{0x7FD00000U};

[[nodiscard]] DcsDeferredFeature* dcs_find_deferred_locked(const NgxHandle* handle) noexcept {
    if (handle == nullptr) return nullptr;
    for (auto& entry : dcs_deferred_features)
        if (reinterpret_cast<const NgxHandle*>(entry.fake) == handle) return &entry;
    return nullptr;
}

bool dcs_is_deferred_d3d11(const NgxHandle* handle) noexcept {
    if (handle == nullptr || dcs_deferred_count.load(std::memory_order_acquire) == 0U) return false;
    std::lock_guard lock(dcs_deferred_mutex);
    return dcs_find_deferred_locked(handle) != nullptr;
}

// The DX11 SR snippet hooked as a public runtime: the transport needs its module
// and private DX11 features use its create/evaluate/release trampolines.
[[nodiscard]] bool dcs_public_d3d11_slot(std::size_t& slot) noexcept {
    for (std::size_t index{}; index < d3d11_runtimes.size(); ++index) {
        const auto& runtime = d3d11_runtimes[index];
        if (runtime.module.load(std::memory_order_acquire) && runtime.create.load(std::memory_order_acquire) &&
            runtime.evaluate.load(std::memory_order_acquire) && runtime.release.load(std::memory_order_acquire)) {
            slot = index;
            return true;
        }
    }
    return false;
}

bool dcs_try_defer_d3d11_create(ID3D11DeviceContext* const context, const std::uint32_t feature,
    NgxParameters* const parameters, NgxHandle** const handle, NgxResult& result) noexcept {
    if (feature != 1U || context == nullptr || parameters == nullptr || handle == nullptr || !dcs_quad_focus_env())
        return false;
    // Without the core evaluate detours an opaque handle could reach the real
    // core. NVIDIA's core exports no EvaluateFeature_C (the SDK's static library
    // routes it to EvaluateFeature); a core that does export it must be detoured too.
    const auto core = find_core_runtime();
    const auto get_proc = real_get_proc_address.load(std::memory_order_acquire);
    const bool core_exports_c = core && get_proc && get_proc(core, "NVSDK_NGX_D3D11_EvaluateFeature_C") != nullptr;
    if (!core || !real_core_evaluate_d3d11.load(std::memory_order_acquire) ||
        (core_exports_c && !real_core_evaluate_d3d11_c.load(std::memory_order_acquire)) ||
        !real_core_create_d3d11.load(std::memory_order_acquire) ||
        !real_core_release_d3d11.load(std::memory_order_acquire)) return false;
    const auto settings = current_settings();
    // Only configurations whose transport replaces DCS's SR on accepted views.
    // NR-only after upscaling evaluates DCS's own feature on every frame.
    const bool native_sr = !settings.enabled && settings.nr_enabled &&
        (!settings.nr_foveated || !settings.nr_use_sr_foveation) && !settings.peripheral_dlaa_enabled &&
        settings.nr_processing_order != NrProcessingOrder::before_upscaling;
    if (!settings.d3d11_use_d3d12_transport || !(settings.enabled || settings.nr_enabled) || native_sr) return false;
    std::size_t slot{};
    if (!dcs_public_d3d11_slot(slot)) return false;
    const auto out_width = get_ui(parameters, "OutWidth");
    const auto out_height = get_ui(parameters, "OutHeight");
    // Peripheral views always evaluate natively; only focus (or not yet
    // classifiable) extents are deferred.
    const int focus = dcs_quad_focus_create_extent(out_width, out_height);
    if (focus == 0) {
        trace_event("DCS deferred DX11 create skipped: output %ux%u is not a Quad Views focus extent", out_width, out_height);
        return false;
    }
    auto* fake = new (std::nothrow) DcsDeferredHandle{dcs_deferred_next_id.fetch_add(1U), dcs_deferred_magic};
    if (fake == nullptr) return false;
    DcsDeferredFeature entry{};
    entry.fake = fake;
    entry.feature = feature;
    entry.slot = slot;
    entry.create = dcs_capture_create_values(parameters);
    entry.created_ms = GetTickCount64();
    {
        std::lock_guard lock(dcs_deferred_mutex);
        dcs_deferred_features.push_back(entry);
        dcs_deferred_count.store(static_cast<unsigned>(dcs_deferred_features.size()), std::memory_order_release);
    }
    auto* const opaque = reinterpret_cast<NgxHandle*>(fake);
    const auto& runtime = d3d11_runtimes[slot];
    register_d3d11_game_feature(opaque, feature, runtime.create.load(std::memory_order_acquire),
        runtime.release.load(std::memory_order_acquire), NgxOutputExtent{out_width, out_height});
    register_stereo_view(static_cast<DlssViewId>(reinterpret_cast<std::uintptr_t>(opaque)));
    *handle = opaque;
    result = 1U;
    const auto live = dcs_ledger_created(opaque, 1U, feature, parameters, true);
    trace_event("DCS deferred DX11 DLSS create handle=%p input=%ux%u output=%ux%u flags=0x%08X quality=%u focus=%s "
        "slot=%zu: no VRAM until DCS's own feature is evaluated; live_game_features=%u",
        opaque, get_ui(parameters, "Width"), get_ui(parameters, "Height"), out_width, out_height,
        get_ngx_integer_bits(parameters, "DLSS.Feature.Create.Flags"), get_ngx_integer_bits(parameters, "PerfQualityValue"),
        focus > 0 ? "layout" : "unclassified", slot, live);
    if (live > 1U) dcs_ledger_trace("after-create");
    return true;
}

// Creates the real core feature with the stored creation values. Called on the
// render thread from the evaluation that needs DCS's own feature.
[[nodiscard]] NgxHandle* dcs_materialize_deferred(ID3D11DeviceContext* const context, const NgxHandle* const handle,
    const NgxParameters* const parameters, const char* const reason, NgxResult& result) noexcept {
    DcsCreateValues create{};
    std::uint32_t feature{};
    {
        std::lock_guard lock(dcs_deferred_mutex);
        auto* entry = dcs_find_deferred_locked(handle);
        if (entry == nullptr) { result = 0xBAD00005U; return nullptr; }
        if (entry->real) return entry->real;
        create = entry->create;
        feature = entry->feature;
    }
    const auto create_feature = real_core_create_d3d11.load(std::memory_order_acquire);
    if (create_feature == nullptr || parameters == nullptr || context == nullptr) { result = 0xBAD00007U; return nullptr; }
    auto* const bag = const_cast<NgxParameters*>(parameters);
    // Replay only values that differ; restore exactly those afterwards.
    const auto current = dcs_capture_create_values(bag);
    std::array<bool, dcs_create_key_count> changed{};
    for (std::size_t i{}; i < create.size(); ++i) {
        if (!create[i].present) continue;
        if (current[i].present && current[i].value == create[i].value) continue;
        dcs_apply_create_value(bag, create[i]);
        changed[i] = true;
    }
    const auto before = dcs_vram_sample(context);
    const auto create_started = GetTickCount64();
    NgxHandle* real{};
    dcs_materializing_feature = true;
    result = create_feature(context, feature, bag, &real);
    dcs_materializing_feature = false;
    const auto elapsed = GetTickCount64() - create_started;
    for (std::size_t i{}; i < current.size(); ++i)
        if (changed[i] && current[i].present) dcs_apply_create_value(bag, current[i]);
    const auto after = dcs_vram_sample(context);
    std::uint64_t count{};
    bool keep{};
    if (ngx_succeeded(result) && real) {
        std::lock_guard lock(dcs_deferred_mutex);
        if (auto* entry = dcs_find_deferred_locked(handle); entry && !entry->real) {
            entry->real = real;
            count = ++entry->materializations;
            keep = true;
        }
    }
    if (ngx_succeeded(result) && real && !keep) {
        static_cast<void>(real_core_release_d3d11.load(std::memory_order_acquire)(real));
        real = nullptr;
        result = 0xBAD00005U;
    }
    trace_event("DCS deferred DX11 DLSS materialized handle=%p real=%p reason=%s result=0x%08X count=%llu "
        "input=%ux%u output=%ux%u elapsed_ms=%llu vram_before_mb=%.1f vram_after_mb=%.1f delta_mb=%+.1f",
        handle, real, reason, unsigned(result), static_cast<unsigned long long>(count),
        create[0].value, create[1].value, create[2].value, create[3].value, static_cast<unsigned long long>(elapsed),
        dcs_mb(before.usage), dcs_mb(after.usage), dcs_delta_mb(before, after));
    return keep ? real : nullptr;
}

// The pipeline's "original" evaluate for an opaque handle: DCS's own feature is
// needed now, so create it first. Other handles (private DX11 features created
// through the snippet) keep the snippet's own evaluate.
template<class Callback, class Evaluate>
NgxResult dcs_evaluate_real(ID3D11DeviceContext* context, const NgxHandle* handle,
    const NgxParameters* parameters, Callback callback, Evaluate evaluate_real) {
    NgxResult result{};
    bool created_now{};
    {
        std::lock_guard lock(dcs_deferred_mutex);
        const auto* entry = dcs_find_deferred_locked(handle);
        created_now = entry && !entry->real;
    }
    const auto before = created_now ? dcs_vram_sample(context) : DcsVram{};
    auto* real = dcs_materialize_deferred(context, handle, parameters, "game-feature-evaluation", result);
    if (real == nullptr) return ngx_succeeded(result) ? 0xBAD00005U : result;
    result = evaluate_real(real, callback);
    // NGX allocates a DLSS feature's working set on its first evaluation.
    if (created_now) {
        const auto after = dcs_vram_sample(context);
        trace_event("DCS deferred DX11 DLSS first evaluation handle=%p real=%p result=0x%08X vram_before_create_mb=%.1f "
            "vram_after_evaluation_mb=%.1f delta_mb=%+.1f", handle, real, unsigned(result), dcs_mb(before.usage),
            dcs_mb(after.usage), dcs_delta_mb(before, after));
    }
    return result;
}

NgxResult dcs_deferred_evaluate(ID3D11DeviceContext* context, const NgxHandle* handle,
    const NgxParameters* parameters, NgxProgressCallback callback) {
    if (dcs_is_deferred_d3d11(handle)) {
        return dcs_evaluate_real(context, handle, parameters, callback, [&](NgxHandle* real, NgxProgressCallback cb) {
            const auto evaluate = real_core_evaluate_d3d11.load(std::memory_order_acquire);
            return evaluate ? evaluate(context, real, parameters, cb) : 0xBAD00007U;
        });
    }
    std::size_t slot{};
    const auto evaluate = dcs_public_d3d11_slot(slot) ? d3d11_runtimes[slot].evaluate.load(std::memory_order_acquire) : nullptr;
    return evaluate ? evaluate(context, handle, parameters, callback) : 0xBAD00007U;
}

NgxResult dcs_deferred_evaluate_c(ID3D11DeviceContext* context, const NgxHandle* handle,
    const NgxParameters* parameters, NgxProgressCallbackC callback) {
    if (dcs_is_deferred_d3d11(handle)) {
        return dcs_evaluate_real(context, handle, parameters, callback, [&](NgxHandle* real, NgxProgressCallbackC cb) {
            // NVIDIA's core has no EvaluateFeature_C; the SDK routes C callers to EvaluateFeature.
            if (const auto evaluate_c = real_core_evaluate_d3d11_c.load(std::memory_order_acquire))
                return evaluate_c(context, real, parameters, cb);
            const auto evaluate = real_core_evaluate_d3d11.load(std::memory_order_acquire);
            return evaluate ? evaluate(context, real, parameters, nullptr) : 0xBAD00007U;
        });
    }
    std::size_t slot{};
    const auto evaluate = dcs_public_d3d11_slot(slot) ? d3d11_runtimes[slot].evaluate_c.load(std::memory_order_acquire) : nullptr;
    return evaluate ? evaluate(context, handle, parameters, callback) : 0xBAD00007U;
}

// One runtime per public slot: the snippet's module/create/release plus the
// materializing evaluates.
std::array<D3D11RuntimeCallbacks, d3d11_runtime_capacity> dcs_deferred_runtimes{};
[[nodiscard]] const D3D11RuntimeCallbacks& dcs_deferred_runtime(std::size_t slot) noexcept {
    auto& runtime = dcs_deferred_runtimes[slot];
    const auto& source = d3d11_runtimes[slot];
    runtime.module.store(source.module.load(std::memory_order_acquire), std::memory_order_release);
    runtime.init.store(source.init.load(std::memory_order_acquire), std::memory_order_release);
    runtime.create.store(source.create.load(std::memory_order_acquire), std::memory_order_release);
    runtime.release.store(source.release.load(std::memory_order_acquire), std::memory_order_release);
    runtime.evaluate.store(&dcs_deferred_evaluate, std::memory_order_release);
    runtime.evaluate_c.store(&dcs_deferred_evaluate_c, std::memory_order_release);
    return runtime;
}

template<class Invoke>
NgxResult dcs_evaluate_deferred(ID3D11DeviceContext* const context, const NgxHandle* const handle,
    const NgxParameters* const parameters, Invoke invoke) {
    std::size_t slot{};
    {
        std::lock_guard lock(dcs_deferred_mutex);
        auto* entry = dcs_find_deferred_locked(handle);
        if (entry == nullptr) return 0xBAD00005U;
        slot = entry->slot;
    }
    diagnostic_note_dlss_source(DiagnosticApi::d3d11, slot != 0U);
    const auto& runtime = dcs_deferred_runtime(slot);
    return evaluate_with_eye_calibration(context, handle, parameters, [&] { return invoke(runtime); });
}

NgxResult hook_core_evaluate_d3d11(ID3D11DeviceContext* const context, const NgxHandle* const handle,
    const NgxParameters* const parameters, const NgxProgressCallback callback) {
    if (!dcs_is_deferred_d3d11(handle)) {
        const auto original = real_core_evaluate_d3d11.load(std::memory_order_acquire);
        return original ? original(context, handle, parameters, callback) : 0xBAD00007U;
    }
    if (calibration_evaluation_depth) // Nested inside another evaluation: just DCS's feature.
        return dcs_deferred_evaluate(context, handle, parameters, callback);
    return dcs_evaluate_deferred(context, handle, parameters, [&](const D3D11RuntimeCallbacks& runtime) {
        return evaluate_d3d11_impl(runtime, context, handle, parameters, callback);
    });
}

NgxResult hook_core_evaluate_d3d11_c(ID3D11DeviceContext* const context, const NgxHandle* const handle,
    const NgxParameters* const parameters, const NgxProgressCallbackC callback) {
    if (!dcs_is_deferred_d3d11(handle)) {
        const auto original = real_core_evaluate_d3d11_c.load(std::memory_order_acquire);
        return original ? original(context, handle, parameters, callback) : 0xBAD00007U;
    }
    if (calibration_evaluation_depth)
        return dcs_deferred_evaluate_c(context, handle, parameters, callback);
    return dcs_evaluate_deferred(context, handle, parameters, [&](const D3D11RuntimeCallbacks& runtime) {
        return evaluate_d3d11_c_impl(runtime, context, handle, parameters, callback);
    });
}

// ReleaseFeature of an opaque handle: release the real feature only if it was
// ever created, then the per-view state, then the opaque handle itself.
bool dcs_release_deferred_d3d11(NgxHandle* const handle, NgxResult& result) noexcept {
    if (!dcs_is_deferred_d3d11(handle)) return false;
    DcsDeferredFeature entry{};
    {
        std::lock_guard lock(dcs_deferred_mutex);
        auto* found = dcs_find_deferred_locked(handle);
        if (found == nullptr) return false;
        entry = *found;
        dcs_deferred_features.erase(dcs_deferred_features.begin() + (found - dcs_deferred_features.data()));
        dcs_deferred_count.store(static_cast<unsigned>(dcs_deferred_features.size()), std::memory_order_release);
    }
    result = dcs_traced_feature_release(1U, handle, [&]() -> NgxResult {
        if (entry.real) {
            const auto release_feature = real_core_release_d3d11.load(std::memory_order_acquire);
            if (release_feature) return release_feature(entry.real);
        }
        return 1U;
    });
    release_d3d11_transport_view(handle);
    release_d3d11_peripheral_dlaa_view(handle);
    unregister_d3d11_game_feature(handle);
    const auto view_id = static_cast<DlssViewId>(reinterpret_cast<std::uintptr_t>(handle));
    unregister_stereo_view(view_id);
    forget_gaze_view(view_id);
    trace_event("DCS deferred DX11 DLSS release handle=%p real=%p materializations=%llu result=0x%08X",
        handle, entry.real, static_cast<unsigned long long>(entry.materializations), unsigned(result));
    delete entry.fake;
    return true;
}

// The core destroys every feature of the device at shutdown; never release
// those real handles again.
void dcs_forget_deferred_reals(const char* reason) noexcept {
    std::lock_guard lock(dcs_deferred_mutex);
    unsigned forgotten{};
    for (auto& entry : dcs_deferred_features) if (entry.real) { entry.real = nullptr; ++forgotten; }
    if (!dcs_deferred_features.empty())
        trace_event("DCS deferred DX11 DLSS %s: forgot %u core-owned features, %zu opaque handles remain",
            reason, forgotten, dcs_deferred_features.size());
}

NgxResult hook_core_shutdown_d3d11() {
    const auto original = real_core_shutdown_d3d11.load(std::memory_order_acquire);
    const auto result = original ? original() : 0xBAD00007U;
    dcs_forget_deferred_reals("shutdown");
    return result;
}

NgxResult hook_core_shutdown_d3d11_1(ID3D11Device* const device) {
    const auto original = real_core_shutdown_d3d11_1.load(std::memory_order_acquire);
    const auto result = original ? original(device) : 0xBAD00007U;
    dcs_forget_deferred_reals("shutdown1");
    return result;
}
