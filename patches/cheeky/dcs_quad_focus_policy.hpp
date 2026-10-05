#pragma once
#include "cheeky_gaze_abi.h"
#include <array>
#include <cstdint>
#include <iterator>

namespace cheeky::foveated_dlss {
// Local adapter extension: two ABI slots describe VARJO focus-left/focus-right.
// Peripheral views remain owned by DCS. The ABI structure itself is unchanged.
inline constexpr std::uint32_t dcs_quad_focus_status = 1U << 12U;
inline constexpr std::uint32_t dcs_quad_focus_single_mip = 1U << 9U;
inline constexpr std::uint32_t dcs_quad_focus_required_flags = dcs_quad_focus_status |
    CHEEKY_GAZE_STATUS_LAYER_ACTIVE | CHEEKY_GAZE_STATUS_SESSION_FOCUSED | CHEEKY_GAZE_STATUS_MAPPING_READY;
inline constexpr std::uint32_t dcs_quad_focus_rejected_flags = CHEEKY_GAZE_STATUS_AMBIGUOUS_RESOURCE |
    CHEEKY_GAZE_STATUS_UNSUPPORTED_VIEW_CONFIG;

// Side channel exported by the adapted OpenXR layer (CheekyOpenXR_GetDcsQuadLayout).
// It describes all four VARJO views of the last submitted projection: 0/1 are the
// peripheral pair, 2/3 the focus pair. The gaze ABI itself stays unchanged.
struct DcsQuadLayoutV1 {
    std::uint32_t structure_size{}, valid{};
    std::uint64_t session_generation{}, publication_qpc{}, sequence{};
    std::array<std::uint32_t, 4> width{}, height{}, array_index{};
    std::array<std::uint64_t, 4> swapchain{};
};
using DcsQuadLayoutFn = std::uint32_t(*)(void* output, std::uint32_t output_size);
inline constexpr const char* dcs_quad_layout_export = "CheekyOpenXR_GetDcsQuadLayout";

inline bool dcs_quad_focus_subimage_valid(unsigned texture_width, unsigned texture_height,
    unsigned array_size, unsigned mip_count, unsigned array_index,
    int x, int y, int width, int height) noexcept {
    return mip_count == 1 && array_index < array_size && x >= 0 && y >= 0 &&
        width > 0 && height > 0 &&
        std::uint64_t(x) + unsigned(width) <= texture_width &&
        std::uint64_t(y) + unsigned(height) <= texture_height;
}

template<class Graph>
bool dcs_quad_focus_copy_reaches(Graph& graph, std::uint64_t source_resource,
    unsigned x, unsigned y, unsigned width, unsigned height,
    const CheekyGazeViewV1& target, std::uint64_t now) {
    // With one mip the OpenXR array index is the exact D3D subresource index.
    // NGX output remains a single-slice resource; a submitted copy proves its role.
    if (!(target.flags & dcs_quad_focus_single_mip)) return false;
    return graph.reaches({source_resource, 0, x, y, width, height},
        {target.resource_identity, target.array_index,
            static_cast<unsigned>(target.image_rect_x), static_cast<unsigned>(target.image_rect_y),
            width, height}, now);
}

// Rate limiter for per-handle rejection traces: the first four, every power of
// two, and a reason/detail change at most once per second.
struct DcsQuadFocusTrace {
    std::uint64_t rejects{}, last_log_ms{};
    unsigned last_reason{~0U}, last_detail{~0U};
    bool should_log(unsigned reason, unsigned detail, std::uint64_t now_ms) noexcept {
        const auto n = ++rejects;
        const bool changed = reason != last_reason || detail != last_detail;
        last_reason = reason; last_detail = detail;
        const bool log = n <= 4 || (n & (n - 1)) == 0 || (changed && now_ms - last_log_ms >= 1000);
        if (log) last_log_ms = now_ms;
        return log;
    }
    void accepted() noexcept { rejects = 0; last_reason = last_detail = ~0U; }
};

// Per-handle state of the transient-rejection hold (see dcs_quad_focus_apply_hold).
struct DcsQuadFocusHold {
    bool valid{}, holding{};
    std::uint64_t proven_ms{}, session_generation{};
    unsigned width{}, height{};
};
struct DcsQuadFocusTransition {
    bool active{};
    DcsQuadFocusTrace trace{};
    DcsQuadFocusHold hold{};
    bool observe(bool next_active) noexcept {
        const bool reset_original = active && !next_active;
        active = next_active;
        return reset_original;
    }
};

// Bitmask of the reasons a snapshot cannot be used; zero means ready.
enum DcsQuadSnapshotProblem : std::uint32_t {
    dcs_snapshot_abi = 1U << 0U, dcs_snapshot_view_count = 1U << 1U, dcs_snapshot_session = 1U << 2U,
    dcs_snapshot_display_time = 1U << 3U, dcs_snapshot_stale = 1U << 4U, dcs_snapshot_future = 1U << 5U,
    dcs_snapshot_missing_flags = 1U << 6U, dcs_snapshot_rejected_flags = 1U << 7U,
};
inline std::uint32_t dcs_quad_focus_snapshot_problems(const CheekyGazeSnapshotV1& s,
    std::uint64_t now, std::uint64_t frequency) noexcept {
    std::uint32_t problems{};
    if (s.abi_version != CHEEKY_GAZE_ABI_VERSION || s.structure_size < sizeof(s)) problems |= dcs_snapshot_abi;
    if (s.view_count != 2) problems |= dcs_snapshot_view_count;
    if (!s.session_generation) problems |= dcs_snapshot_session;
    if (!s.predicted_display_time) problems |= dcs_snapshot_display_time;
    if (!frequency || !s.publication_qpc || now < s.publication_qpc) problems |= dcs_snapshot_future;
    // 250 ms: with frame generation and DCS near 30 FPS a snapshot published at xrEndFrame is often 40-80 ms old
    // when DLSS evaluates; 50 ms made DLSS-NR skip focus frames intermittently. The session check still applies.
    else if (now - s.publication_qpc > frequency / 4) problems |= dcs_snapshot_stale;
    if ((s.status_flags & dcs_quad_focus_required_flags) != dcs_quad_focus_required_flags)
        problems |= dcs_snapshot_missing_flags;
    if (s.status_flags & dcs_quad_focus_rejected_flags) problems |= dcs_snapshot_rejected_flags;
    return problems;
}
inline bool dcs_quad_focus_snapshot_ready(const CheekyGazeSnapshotV1& s,
    std::uint64_t now, std::uint64_t frequency) noexcept {
    return dcs_quad_focus_snapshot_problems(s, now, frequency) == 0;
}

template<class CopyProof>
unsigned dcs_quad_focus_match_count(const CheekyGazeSnapshotV1& s, std::uint64_t resource,
    unsigned x, unsigned y, unsigned width, unsigned height, CopyProof proof) noexcept {
    if (!resource || !width || !height || s.view_count != 2) return 0;
    unsigned matches{};
    for (const auto& v : s.views) {
        if (!(v.flags & CHEEKY_GAZE_VIEW_RESOURCE_VALID) || !v.resource_identity ||
            v.image_rect_x < 0 || v.image_rect_y < 0 ||
            v.image_rect_width != width || v.image_rect_height != height) continue;
        const bool exact = v.array_index == 0 && v.resource_identity == resource &&
            static_cast<unsigned>(v.image_rect_x) == x && static_cast<unsigned>(v.image_rect_y) == y;
        if (exact || proof(v)) ++matches;
    }
    return matches;
}

// Recently evaluated NGX features and their created output extents. DCS
// evaluates one feature per VARJO view; the census lets the size proof require
// that the evaluated set is exactly two focus-sized and two peripheral-sized.
class DcsQuadHandleCensus {
public:
    static constexpr std::uint64_t window_ms = 250;
    struct Counts { unsigned focus{}, peripheral{}, other{}; };
    void record(std::uint64_t handle, unsigned width, unsigned height, std::uint64_t now_ms) noexcept {
        Entry* slot{};
        for (auto& e : entries_) {
            if (e.handle == handle) { slot = &e; break; }
            if (!slot && (!e.handle || now_ms - e.time_ms > window_ms)) slot = &e;
        }
        if (!slot) {
            // More live features than tracked: never under-count them.
            overflow_until_ = now_ms + window_ms;
            slot = &entries_[0];
            for (auto& e : entries_) if (e.time_ms < slot->time_ms) slot = &e;
        }
        *slot = {handle, now_ms, width, height};
    }
    Counts count(unsigned focus_width, unsigned focus_height, unsigned peripheral_width,
        unsigned peripheral_height, std::uint64_t now_ms) const noexcept {
        Counts result{};
        for (const auto& e : entries_) {
            if (!e.handle || now_ms - e.time_ms > window_ms) continue;
            if (e.width == focus_width && e.height == focus_height) ++result.focus;
            else if (e.width == peripheral_width && e.height == peripheral_height) ++result.peripheral;
            else ++result.other;
        }
        if (now_ms < overflow_until_) ++result.other;
        return result;
    }
    void clear() noexcept { entries_ = {}; overflow_until_ = 0; }
private:
    struct Entry { std::uint64_t handle{}, time_ms{}; unsigned width{}, height{}; };
    std::array<Entry, 16> entries_{};
    std::uint64_t overflow_until_{};
};

// Size-bijection proof, used only when neither exact identity nor a submitted
// copy proves the role (DCS post-processes/HUD-composites after DLSS, so its NGX
// output is never the swapchain image and D3D11 copies are not observed).
// Accept only when the submitted VARJO layout and the evaluated NGX features
// agree as a one-to-one size mapping: both focus views share one extent, both
// peripheral views share a different extent, the snapshot's focus pair matches
// the layout, this evaluation's created output has the focus extent, and the
// live features are exactly two focus-sized plus two peripheral-sized. It does
// not identify the eye, so it is refused whenever processing consumes eye
// identity (foveated SR or foveated NR).
enum class DcsQuadSizeProof : unsigned {
    accepted, eye_identity_required, layout_unavailable, layout_invalid, layout_stale, layout_session,
    focus_pair_mismatch, peripheral_pair_mismatch, peripheral_equals_focus, snapshot_layout_mismatch,
    output_not_focus_size, handle_census,
};
inline const char* dcs_quad_size_proof_name(DcsQuadSizeProof value) noexcept {
    constexpr const char* names[]{"accepted", "eye-identity-required", "layout-unavailable", "layout-invalid",
        "layout-stale", "layout-session-mismatch", "focus-pair-size-mismatch", "peripheral-pair-size-mismatch",
        "peripheral-equals-focus-size", "snapshot-layout-mismatch", "output-not-focus-size", "handle-census"};
    const auto index = static_cast<unsigned>(value);
    return index < std::size(names) ? names[index] : "unknown";
}
inline DcsQuadSizeProof dcs_quad_focus_size_proof(const CheekyGazeSnapshotV1& s, const DcsQuadLayoutV1* layout,
    std::uint64_t now, std::uint64_t frequency, unsigned width, unsigned height,
    const DcsQuadHandleCensus::Counts& census, bool eye_identity_required) noexcept {
    using P = DcsQuadSizeProof;
    if (eye_identity_required) return P::eye_identity_required;
    if (!layout) return P::layout_unavailable;
    const auto& l = *layout;
    if (l.structure_size < sizeof(l) || !l.valid) return P::layout_invalid;
    if (!frequency || !l.publication_qpc || now < l.publication_qpc || now - l.publication_qpc > frequency / 4)
        return P::layout_stale;
    if (l.session_generation != s.session_generation) return P::layout_session;
    if (!l.width[2] || !l.height[2] || l.width[2] != l.width[3] || l.height[2] != l.height[3])
        return P::focus_pair_mismatch;
    if (!l.width[0] || !l.height[0] || l.width[0] != l.width[1] || l.height[0] != l.height[1])
        return P::peripheral_pair_mismatch;
    if (l.width[0] == l.width[2] && l.height[0] == l.height[2]) return P::peripheral_equals_focus;
    for (unsigned eye = 0; eye < 2; ++eye) {
        const auto& v = s.views[eye];
        if (!(v.flags & CHEEKY_GAZE_VIEW_RESOURCE_VALID) || v.image_rect_width != l.width[2 + eye] ||
            v.image_rect_height != l.height[2 + eye] || v.array_index != l.array_index[2 + eye] ||
            v.swapchain_identity != l.swapchain[2 + eye]) return P::snapshot_layout_mismatch;
    }
    if (width != l.width[2] || height != l.height[2]) return P::output_not_focus_size;
    if (census.focus != 2 || census.peripheral != 2 || census.other != 0) return P::handle_census;
    return P::accepted;
}

enum class DcsQuadFocusReason : unsigned {
    accepted, snapshot_unavailable, snapshot_not_ready, ambiguous_match, no_proof,
};
inline const char* dcs_quad_focus_reason_name(DcsQuadFocusReason value) noexcept {
    constexpr const char* names[]{"accepted", "snapshot-unavailable", "snapshot-not-ready", "ambiguous-match", "no-proof"};
    const auto index = static_cast<unsigned>(value);
    return index < std::size(names) ? names[index] : "unknown";
}
struct DcsQuadFocusDecision {
    bool allowed{};
    DcsQuadFocusReason reason{DcsQuadFocusReason::snapshot_unavailable};
    const char* route{"none"};
    unsigned matches{}, exact_matches{}, copy_matches{};
    std::uint32_t snapshot_problems{}, missing_flags{}, rejected_flags{};
    DcsQuadSizeProof size{DcsQuadSizeProof::layout_unavailable};
    DcsQuadHandleCensus::Counts census{};
    unsigned detail() const noexcept {
        return reason == DcsQuadFocusReason::snapshot_not_ready ? snapshot_problems | (missing_flags << 8U)
            : reason == DcsQuadFocusReason::no_proof ? static_cast<unsigned>(size) : matches;
    }
};

// Pure decision used by the NGX gate. The census is updated with this evaluation
// before counting, so the current feature always participates in the census.
template<class CopyProof>
DcsQuadFocusDecision dcs_quad_focus_decide(std::uint64_t handle, bool snapshot_loaded,
    const CheekyGazeSnapshotV1& s, std::uint64_t now, std::uint64_t frequency, std::uint64_t resource,
    unsigned x, unsigned y, unsigned width, unsigned height, CopyProof proof, const DcsQuadLayoutV1* layout,
    DcsQuadHandleCensus& census, std::uint64_t now_ms, bool eye_identity_required) noexcept {
    DcsQuadFocusDecision d{};
    if (handle && width && height) census.record(handle, width, height, now_ms);
    if (!snapshot_loaded) return d;
    d.snapshot_problems = dcs_quad_focus_snapshot_problems(s, now, frequency);
    d.missing_flags = dcs_quad_focus_required_flags & ~s.status_flags;
    d.rejected_flags = s.status_flags & dcs_quad_focus_rejected_flags;
    if (layout && layout->valid)
        d.census = census.count(layout->width[2], layout->height[2], layout->width[0], layout->height[0], now_ms);
    if (d.snapshot_problems) { d.reason = DcsQuadFocusReason::snapshot_not_ready; return d; }
    // The proof is only consulted for views that are not exact matches.
    d.matches = dcs_quad_focus_match_count(s, resource, x, y, width, height, [&](const CheekyGazeViewV1& v) {
        const bool copied = proof(v);
        d.copy_matches += copied ? 1U : 0U;
        return copied;
    });
    d.exact_matches = d.matches - d.copy_matches;
    if (d.matches > 1) { d.reason = DcsQuadFocusReason::ambiguous_match; return d; }
    if (d.matches == 1) {
        d.allowed = true; d.reason = DcsQuadFocusReason::accepted;
        d.route = d.exact_matches ? "exact-resource" : "submitted-copy";
        return d;
    }
    d.size = dcs_quad_focus_size_proof(s, layout, now, frequency, width, height, d.census, eye_identity_required);
    if (d.size != DcsQuadSizeProof::accepted) { d.reason = DcsQuadFocusReason::no_proof; return d; }
    d.allowed = true; d.reason = DcsQuadFocusReason::accepted; d.route = "size-bijection";
    return d;
}

// Transient-rejection hold. A handle proven to be a focus view by the size
// bijection stays accepted through rejections that only say "the proof could
// not be re-established right now" (stale snapshot/layout, census momentarily
// incomplete), for at most dcs_quad_focus_hold_ms after its last real proof.
// Releasing on such rejections drained the GPU and rebuilt SR/NR features and
// transport textures on DCS's render thread (81-622 ms hitches, history resets),
// and the hitch itself made the next snapshot stale. A definite counter-proof
// (another session, output not the held focus extent, a layout whose focus or
// peripheral extent no longer fits, eye identity required) ends the hold at once.
inline constexpr std::uint64_t dcs_quad_focus_hold_ms = 2000;
inline constexpr std::uint32_t dcs_quad_focus_transient_snapshot_problems = dcs_snapshot_stale | dcs_snapshot_future;
inline bool dcs_quad_focus_transient(const DcsQuadFocusDecision& d) noexcept {
    if (d.reason == DcsQuadFocusReason::snapshot_not_ready)
        return d.snapshot_problems && !(d.snapshot_problems & ~dcs_quad_focus_transient_snapshot_problems);
    if (d.reason == DcsQuadFocusReason::no_proof)
        return d.size == DcsQuadSizeProof::layout_stale || d.size == DcsQuadSizeProof::handle_census;
    return false;
}
// Applies the hold to a fresh decision. Returns true when the decision was a
// rejection converted into a held acceptance (route "transient-hold"); the
// original reason/detail stay in the decision for diagnostics.
inline bool dcs_quad_focus_apply_hold(DcsQuadFocusDecision& d, DcsQuadFocusHold& hold,
    const CheekyGazeSnapshotV1& s, const DcsQuadLayoutV1* layout, unsigned width, unsigned height,
    std::uint64_t now_ms, bool eye_identity_required) noexcept {
    if (d.allowed) {
        if (d.route && d.route[0] == 's' && !eye_identity_required) // "size-bijection"
            hold = {true, false, now_ms, s.session_generation, width, height};
        else hold = {};
        return false;
    }
    const bool layout_consistent = !layout || !layout->valid ||
        (layout->session_generation == hold.session_generation && layout->width[2] == width &&
            layout->height[2] == height && !(layout->width[0] == width && layout->height[0] == height));
    if (hold.valid && !eye_identity_required && dcs_quad_focus_transient(d) && width == hold.width &&
        height == hold.height && s.session_generation == hold.session_generation && layout_consistent &&
        now_ms >= hold.proven_ms && now_ms - hold.proven_ms <= dcs_quad_focus_hold_ms) {
        hold.holding = true;
        d.allowed = true; d.route = "transient-hold";
        return true;
    }
    hold = {};
    return false;
}
} // namespace cheeky::foveated_dlss
