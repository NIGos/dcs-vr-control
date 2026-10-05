#pragma once
// DCS VR Control: live DLSS 5 status for the in-headset diagnostic overlay.
//
// The Cheeky runtime publishes one small versioned block in named shared memory,
// Local\DcsVrControlStatus-<pid>, at most every 100 ms from the game's Present. The OFXR
// fork patch (patches/ofxr-djules75) reads it in the same process and draws it; DCS VR Control
// may read it from outside. Pure data: one writer, readers copy it under a sequence counter
// (odd while a write is in progress) and retry. No lock is ever shared across processes.
//
// The layout is an ABI shared with patches/ofxr-djules75 (include/xrfg/dcsvr_status.hpp).
// Both sides pin it with the same static_asserts; change both or bump status_version.
#include <windows.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

namespace cheeky::dcsvr {
inline constexpr std::uint32_t status_magic = 0x53525644U; // "DVRS"
inline constexpr std::uint32_t status_version = 1U;
inline constexpr std::uint64_t status_interval_ms = 100U;

enum DcsStatusFlag : std::uint32_t {
    status_nr_enabled = 1U,      // DLSS-NR setting (includes the session-only in-flight toggle)
    status_quad_focus = 2U,      // DCSVR_QUAD_FOCUS=1: NR runs on the Quad Views focus pair only
    status_nr_hotkey = 4U,       // DCSVR_NR_HOTKEY parsed to a valid key
    status_graphics_ready = 8U,  // Cheeky attached to the game's device
    status_sr_enabled = 16U,     // Foveated DLSS Super Resolution setting
    status_nr_foveated = 32U,    // NR limited to the foveated region
};

struct DcsVrStatusV1 {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t structure_size;
    std::uint32_t sequence;                  // Even: stable. Odd: write in progress.
    std::uint64_t publish_tick_ms;           // GetTickCount64() of the publication
    std::uint32_t publisher_pid;
    std::uint32_t flags;                     // DcsStatusFlag
    std::uint32_t nr_state;                  // cheeky::foveated_dlss::DlssNrState
    std::uint32_t nr_evaluations_per_second; // Successful DLSS-NR evaluations (all views)
    std::uint32_t nr_failures_per_second;
    std::uint32_t focus_views_mapped;        // Distinct focus views accepted by the quad gate in the last second
    std::uint32_t focus_accepts_per_second;
    std::uint32_t focus_rejects_per_second;  // Includes the peripheral views, which are always rejected
    std::uint32_t focus_last_reject;         // DcsQuadFocusReason of the last rejection
    std::uint32_t nr_hotkey;                 // virtual key | modifiers << 8 (Ctrl 1, Alt 2, Shift 4)
    float nr_gpu_ms;                         // Averaged GPU time of the DLSS-NR pass; 0 when unknown
    float present_fps;                       // Game Present rate seen by Cheeky
    char reason[56];                         // ASCII, set when NR is on but not evaluated
};
static_assert(sizeof(DcsVrStatusV1) == 128U);
static_assert(offsetof(DcsVrStatusV1, sequence) == 12U && offsetof(DcsVrStatusV1, publish_tick_ms) == 16U);
static_assert(offsetof(DcsVrStatusV1, flags) == 28U && offsetof(DcsVrStatusV1, focus_views_mapped) == 44U);
static_assert(offsetof(DcsVrStatusV1, nr_gpu_ms) == 64U && offsetof(DcsVrStatusV1, reason) == 72U);

inline std::wstring dcs_status_mapping_name(std::uint32_t pid) {
    return L"Local\\DcsVrControlStatus-" + std::to_wstring(pid);
}

// Seqlock writer: odd sequence, payload, even sequence. Only the publisher writes.
inline void dcs_status_store(DcsVrStatusV1& shared, const DcsVrStatusV1& value) noexcept {
    std::atomic_ref<std::uint32_t> sequence(shared.sequence);
    const auto current = sequence.load(std::memory_order_relaxed);
    sequence.store(current | 1U, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    auto* bytes = reinterpret_cast<unsigned char*>(&shared);
    const auto* source = reinterpret_cast<const unsigned char*>(&value);
    constexpr auto sequence_end = offsetof(DcsVrStatusV1, sequence) + sizeof(std::uint32_t);
    std::memcpy(bytes, source, offsetof(DcsVrStatusV1, sequence));
    std::memcpy(bytes + sequence_end, source + sequence_end, sizeof(DcsVrStatusV1) - sequence_end);
    sequence.store((current | 1U) + 1U, std::memory_order_release);
}

// Seqlock reader. False while a write never settles or the block is not a V1 status.
inline bool dcs_status_load(const DcsVrStatusV1& shared, DcsVrStatusV1& out) noexcept {
    std::atomic_ref<std::uint32_t> sequence(const_cast<std::uint32_t&>(shared.sequence));
    for (int attempt = 0; attempt < 8; ++attempt) {
        const auto before = sequence.load(std::memory_order_acquire);
        if (before & 1U) { YieldProcessor(); continue; }
        std::memcpy(&out, &shared, sizeof(out));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (sequence.load(std::memory_order_relaxed) != before) continue;
        out.reason[sizeof(out.reason) - 1] = 0;
        return out.magic == status_magic && out.version == status_version && out.structure_size == sizeof(out);
    }
    return false;
}

// Per-second rate of a cumulative counter, sampled at the publication cadence.
class DcsRateWindow {
public:
    // Returns the rate over the sample closest to one second old (at least 200 ms of history).
    float push(std::uint64_t now_ms, std::uint64_t total) noexcept {
        if (count_ && (total < samples_[(head_ + samples_.size() - 1U) % samples_.size()].total ||
                now_ms < samples_[(head_ + samples_.size() - 1U) % samples_.size()].ms)) count_ = 0; // Reset or clock jump.
        samples_[head_] = {now_ms, total};
        head_ = (head_ + 1U) % samples_.size();
        if (count_ < samples_.size()) ++count_;
        const Sample* base{};
        for (std::size_t i = 0; i < count_; ++i) {
            const auto& sample = samples_[(head_ + samples_.size() - 1U - i) % samples_.size()];
            if (now_ms - sample.ms > 1100U) break;
            base = &sample;
        }
        if (!base || now_ms - base->ms < 200U) return 0.0F;
        return static_cast<float>(static_cast<double>(total - base->total) * 1000.0 / static_cast<double>(now_ms - base->ms));
    }
private:
    struct Sample { std::uint64_t ms{}, total{}; };
    std::array<Sample, 24> samples_{};
    std::size_t head_{}, count_{};
};

// Fed by the quad focus NGX gate (any thread); sampled by the publisher.
class DcsFocusActivity {
public:
    void accepted(std::uint64_t view, std::uint64_t now_ms) noexcept {
        std::lock_guard lock(mutex_);
        ++accepted_;
        Slot* target = &slots_[0];
        for (auto& slot : slots_) {
            if (slot.view == view) { target = &slot; break; }
            if (slot.ms < target->ms) target = &slot;
        }
        *target = {view, now_ms};
    }
    void rejected(std::uint32_t reason) noexcept {
        std::lock_guard lock(mutex_);
        ++rejected_;
        last_reject_ = reason;
    }
    struct Sample { std::uint64_t accepted{}, rejected{}; std::uint32_t views{}, last_reject{}; };
    Sample sample(std::uint64_t now_ms) const noexcept {
        std::lock_guard lock(mutex_);
        Sample result{accepted_, rejected_, 0U, last_reject_};
        for (const auto& slot : slots_)
            if (slot.ms != 0U && now_ms >= slot.ms && now_ms - slot.ms <= 1000U) ++result.views;
        return result;
    }
private:
    struct Slot { std::uint64_t view{}, ms{}; };
    mutable std::mutex mutex_;
    std::array<Slot, 8> slots_{};
    std::uint64_t accepted_{}, rejected_{};
    std::uint32_t last_reject_{};
};
inline DcsFocusActivity& dcs_focus_activity() noexcept { static DcsFocusActivity activity; return activity; }

inline void dcs_status_set_reason(DcsVrStatusV1& status, const char* text) noexcept {
    std::memset(status.reason, 0, sizeof(status.reason));
    if (text) strncpy_s(status.reason, sizeof(status.reason), text, _TRUNCATE);
}

// Why NR is on but nothing was evaluated in the last second. nr_state mirrors DlssNrState
// (waiting, disabled, runtime_missing, runtime_failed, unsupported_resources, feature_failed,
// evaluation_failed, active, input_preparation_failed); focus_last_reject mirrors DcsQuadFocusReason
// (accepted, snapshot_unavailable, snapshot_not_ready, ambiguous_match, no_proof).
inline const char* dcs_status_reason_text(const DcsVrStatusV1& s) noexcept {
    if (!(s.flags & status_nr_enabled) || s.nr_evaluations_per_second != 0U) return nullptr;
    if (!(s.flags & status_graphics_ready)) return "Cheeky not attached to the game";
    if (s.flags & status_quad_focus) {
        if (s.focus_accepts_per_second == 0U && s.focus_rejects_per_second == 0U) return "no DLSS frames from DCS";
        if (s.focus_accepts_per_second == 0U) {
            switch (s.focus_last_reject) {
            case 1U: return "focus: gaze layer missing";
            case 2U: return "focus: views not ready";
            case 3U: return "focus: ambiguous match";
            default: return "focus views not identified";
            }
        }
    }
    switch (s.nr_state) {
    case 0U: return "waiting for a DLSS frame";
    case 1U: return "disabled by the runtime";
    case 2U: return "nvngx_dlssnr.dll not loaded";
    case 3U: return "NR runtime failed to start";
    case 4U: return "unsupported depth/motion input";
    case 5U: return "NR feature creation failed";
    case 6U: return "NR evaluation failed";
    case 8U: return "NR input preparation failed";
    default: return "no NR evaluations";
    }
}

// Owns the mapping. The runtime is process-resident, so the publisher lives until exit.
class DcsStatusPublisher {
public:
    DcsStatusPublisher() = default;
    DcsStatusPublisher(const DcsStatusPublisher&) = delete;
    DcsStatusPublisher& operator=(const DcsStatusPublisher&) = delete;
    ~DcsStatusPublisher() { close(); }
    bool open(std::uint32_t pid) noexcept {
        if (view_) return true;
        if (failed_) return false;
        const auto name = dcs_status_mapping_name(pid);
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, name.c_str());
        if (mapping_) view_ = static_cast<DcsVrStatusV1*>(MapViewOfFile(mapping_, FILE_MAP_WRITE, 0, 0, sizeof(DcsVrStatusV1)));
        if (!view_) { close(); failed_ = true; return false; }
        return true;
    }
    // At most every status_interval_ms. False when not due or the mapping is unavailable.
    bool due(std::uint64_t now_ms) noexcept {
        if (now_ms < next_ms_) return false;
        next_ms_ = now_ms + status_interval_ms;
        return true;
    }
    void publish(DcsVrStatusV1 value) noexcept {
        if (!view_) return;
        value.magic = status_magic; value.version = status_version; value.structure_size = sizeof(value);
        dcs_status_store(*view_, value);
    }
    const DcsVrStatusV1* view() const noexcept { return view_; }
    void close() noexcept {
        if (view_) UnmapViewOfFile(view_);
        if (mapping_) CloseHandle(mapping_);
        view_ = nullptr; mapping_ = nullptr;
    }
private:
    HANDLE mapping_{};
    DcsVrStatusV1* view_{};
    std::uint64_t next_ms_{};
    bool failed_{};
};
} // namespace cheeky::dcsvr
