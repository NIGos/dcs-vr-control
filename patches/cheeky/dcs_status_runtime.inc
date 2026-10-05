// DCS VR Control: publishes the live DLSS 5 status block (dcs_status_block.hpp) from the
// runtime tick, which the standalone host calls at the game's Present. Included by uevr/runtime.cpp
// at global scope, after `using namespace cheeky::foveated_dlss;`; called with State::mutex held,
// the same context in which the JSON snapshot reads these diagnostics.
static_assert(static_cast<unsigned>(DlssNrState::active) == 7U &&
    static_cast<unsigned>(DlssNrState::input_preparation_failed) == 8U, "DcsVrStatusV1 nr_state mapping");
namespace {
void dcs_status_publish(State& s, std::uint32_t renderer) noexcept {
    try {
        static auto* publisher = new cheeky::dcsvr::DcsStatusPublisher;
        static auto* nr_rate = new cheeky::dcsvr::DcsRateWindow;
        static auto* nr_failure_rate = new cheeky::dcsvr::DcsRateWindow;
        static auto* accept_rate = new cheeky::dcsvr::DcsRateWindow;
        static auto* reject_rate = new cheeky::dcsvr::DcsRateWindow;
        static auto* present_rate = new cheeky::dcsvr::DcsRateWindow;
        static std::uint64_t presents{};
        static const std::uint32_t hotkey = [] {
            cheeky::standalone::DcsNrHotkeySpec spec{};
            char text[16]{};
            const auto length = GetEnvironmentVariableA("DCSVR_NR_HOTKEY", text, sizeof(text));
            if (length > 0 && length < sizeof(text)) cheeky::standalone::dcs_nr_parse_hotkey(text, spec);
            return spec.virtual_key | (spec.modifiers << 8U);
        }();
        ++presents;
        const auto now = GetTickCount64();
        if (!publisher->due(now) || !publisher->open(GetCurrentProcessId())) return;
        const auto settings = current_settings();
        const auto nr = dlss_nr_snapshot();
        const auto diagnostic = diagnostic_snapshot(renderer == 1 ? DiagnosticApi::d3d12 : DiagnosticApi::d3d11);
        const auto focus = cheeky::dcsvr::dcs_focus_activity().sample(now);
        wchar_t quad[2]{};
        cheeky::dcsvr::DcsVrStatusV1 status{};
        status.publish_tick_ms = now;
        status.publisher_pid = GetCurrentProcessId();
        if (settings.nr_enabled) status.flags |= cheeky::dcsvr::status_nr_enabled;
        if (GetEnvironmentVariableW(L"DCSVR_QUAD_FOCUS", quad, 2) == 1 && quad[0] == L'1')
            status.flags |= cheeky::dcsvr::status_quad_focus;
        if (hotkey & 0xFFU) status.flags |= cheeky::dcsvr::status_nr_hotkey;
        if (s.started && s.graphics_ready) status.flags |= cheeky::dcsvr::status_graphics_ready;
        if (settings.enabled) status.flags |= cheeky::dcsvr::status_sr_enabled;
        if (settings.nr_foveated) status.flags |= cheeky::dcsvr::status_nr_foveated;
        status.nr_state = static_cast<std::uint32_t>(nr.state);
        const auto round = [](float value) { return static_cast<std::uint32_t>(value + 0.5F); };
        status.nr_evaluations_per_second = round(nr_rate->push(now, nr.evaluation_calls));
        status.nr_failures_per_second = round(nr_failure_rate->push(now, nr.failed_calls));
        status.focus_views_mapped = focus.views;
        status.focus_accepts_per_second = round(accept_rate->push(now, focus.accepted));
        status.focus_rejects_per_second = round(reject_rate->push(now, focus.rejected));
        status.focus_last_reject = focus.last_reject;
        status.nr_hotkey = hotkey;
        // The pass that matches the current settings; any measured NR pass otherwise.
        const bool before = settings.nr_processing_order == NrProcessingOrder::before_upscaling;
        float gpu = before ? (settings.nr_foveated ? diagnostic.before_foveated_nr_gpu_ms : diagnostic.before_full_nr_gpu_ms)
                           : (settings.nr_foveated ? diagnostic.foveated_dlss_nr_gpu_ms : diagnostic.full_dlss_nr_gpu_ms);
        for (const float other : {diagnostic.foveated_dlss_nr_gpu_ms, diagnostic.full_dlss_nr_gpu_ms,
                 diagnostic.before_foveated_nr_gpu_ms, diagnostic.before_full_nr_gpu_ms})
            if (!(gpu > 0.0F) && other > 0.0F) gpu = other;
        status.nr_gpu_ms = gpu > 0.0F && gpu < 1000.0F ? gpu : 0.0F;
        status.present_fps = present_rate->push(now, presents);
        cheeky::dcsvr::dcs_status_set_reason(status, cheeky::dcsvr::dcs_status_reason_text(status));
        publisher->publish(status);
    } catch (...) {}
}
} // namespace
