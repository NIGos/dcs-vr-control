#include <windows.h>
#include "../../patches/cheeky/dcs_quad_focus_policy.hpp"
#include "../../patches/cheeky/dcs_nr_hotkey.hpp"
#include "../../external/cheeky/src/gaze_copy_graph.hpp"
#include "../../external/cheeky/openxr_layer/projection_selection.hpp"
#include <iostream>
#include <stdexcept>
#include <array>
using namespace cheeky::foveated_dlss;
void check(bool value, const char* description) { if (!value) throw std::runtime_error(description); }
int main() {
    try {
        CheekyGazeSnapshotV1 s{};
        s.abi_version = CHEEKY_GAZE_ABI_VERSION; s.structure_size = sizeof(s); s.view_count = 2;
        s.session_generation = 1; s.predicted_display_time = 1; s.publication_qpc = 1000;
        s.status_flags = dcs_quad_focus_status | CHEEKY_GAZE_STATUS_LAYER_ACTIVE |
            CHEEKY_GAZE_STATUS_SESSION_FOCUSED | CHEEKY_GAZE_STATUS_MAPPING_READY;
        for (unsigned i = 0; i < 2; ++i) {
            s.views[i].flags = CHEEKY_GAZE_VIEW_RESOURCE_VALID | dcs_quad_focus_single_mip;
            s.views[i].resource_identity = i + 3; s.views[i].image_rect_width = 64; s.views[i].image_rect_height = 64;
        }
        check(dcs_quad_focus_snapshot_ready(s, 1001, 1000), "Fresh focus snapshot rejected");
        check(dcs_quad_focus_snapshot_ready(s, 1200, 1000) && !dcs_quad_focus_snapshot_ready(s, 1300, 1000), "Snapshot freshness window is not 250 ms");
        check(!dcs_quad_focus_snapshot_ready(s, 999, 1000), "Future snapshot accepted");
        auto missing = s; missing.status_flags &= ~CHEEKY_GAZE_STATUS_SESSION_FOCUSED;
        check(!dcs_quad_focus_snapshot_ready(missing, 1001, 1000), "Unfocused session accepted");
        missing = s; missing.status_flags |= CHEEKY_GAZE_STATUS_AMBIGUOUS_RESOURCE;
        check(!dcs_quad_focus_snapshot_ready(missing, 1001, 1000), "Ambiguous scene accepted");
        const auto no_copy = [](const auto&) { return false; };
        check(dcs_quad_focus_match_count(s, 3, 0, 0, 64, 64, no_copy) == 1, "Left focus mapping failed");
        check(dcs_quad_focus_match_count(s, 4, 0, 0, 64, 64, no_copy) == 1, "Right focus mapping failed");
        check(dcs_quad_focus_match_count(s, 1, 0, 0, 64, 64, no_copy) == 0, "Same-sized peripheral guessed as focus");
        check(dcs_quad_focus_match_count(s, 3, 0, 0, 32, 64, no_copy) == 0, "Partial focus rectangle accepted");
        auto ambiguous = s; ambiguous.views[1].resource_identity = 3;
        check(dcs_quad_focus_match_count(ambiguous, 3, 0, 0, 64, 64, no_copy) == 2, "Duplicate focus resource not rejected");
        GazeCopyGraph graph; graph.record({{8, 0, 0, 0, 64, 64}, {3, 0, 0, 0, 64, 64}}, 100);
        auto copied = [&](const auto& target) { return graph.reaches({8, 0, 0, 0, 64, 64}, {target.resource_identity, 0, 0, 0, 64, 64}, 101); };
        check(dcs_quad_focus_match_count(s, 8, 0, 0, 64, 64, copied) == 1, "Submitted-copy provenance rejected");
        check(!graph.reaches({8, 0, 0, 0, 64, 64}, {3, 0, 0, 0, 64, 64}, 1000), "Expired copy accepted");
        auto array_pair = s; array_pair.views[0].array_index = 2; array_pair.views[1].array_index = 3;
        check(dcs_quad_focus_match_count(array_pair, 3, 0, 0, 64, 64, no_copy) == 0,
            "An array slice was accepted without copy provenance");
        graph.record({{9, 0, 0, 0, 64, 64}, {3, 2, 0, 0, 64, 64}}, 1100);
        auto array_copy = [&](const auto& target) {
            return dcs_quad_focus_copy_reaches(graph, 9, 0, 0, 64, 64, target, 1101);
        };
        check(dcs_quad_focus_match_count(array_pair, 9, 0, 0, 64, 64, array_copy) == 1,
            "Verified copy into focus slice 2 rejected");
        array_pair.views[0].flags &= ~dcs_quad_focus_single_mip;
        check(dcs_quad_focus_match_count(array_pair, 9, 0, 0, 64, 64, array_copy) == 0,
            "Unknown mip layout accepted");
        check(dcs_quad_focus_subimage_valid(128, 128, 4, 1, 3, 0, 0, 128, 128), "Valid focus subimage rejected");
        check(!dcs_quad_focus_subimage_valid(128, 128, 4, 1, 4, 0, 0, 128, 128), "Out-of-range array slice accepted");
        check(!dcs_quad_focus_subimage_valid(128, 128, 4, 2, 2, 0, 0, 128, 128), "Multiple mips inferred");
        check(!dcs_quad_focus_subimage_valid(128, 128, 4, 1, 2, -1, 0, 128, 128), "Negative rectangle accepted");
        check(!dcs_quad_focus_subimage_valid(128, 128, 4, 1, 2, 64, 0, 128, 128), "Out-of-bounds rectangle accepted");
        std::array<XrCompositionLayerProjectionView, 4> views{};
        XrCompositionLayerProjection p{XR_TYPE_COMPOSITION_LAYER_PROJECTION}; p.viewCount = 4; p.views = views.data();
        const auto* layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&p);
        XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO}; info.layerCount = 1; info.layers = &layer;
        auto dummy = [](XrSwapchain) { return false; };
        check(cheeky::openxr::select_projection(&info, dummy).unsupported, "Upstream stereo guard bypassed");
        auto selected = cheeky::openxr::select_projection(&info, dummy, true);
        check(selected.projection == &p && selected.first_view == 2, "VARJO focus order not preserved");
        check(p.viewCount == 4 && p.views == views.data(), "Projection mutated");
        DcsQuadFocusTransition transition;
        check(!transition.observe(false), "Cold passthrough reset unnecessarily");
        check(!transition.observe(true), "Entering focus reset original unnecessarily");
        check(!transition.observe(true), "Stable focus reset original unnecessarily");
        check(transition.observe(false), "Mapping loss did not reset original temporal history");
        check(!transition.observe(false), "Mapping loss reset original history repeatedly");
        // Size-bijection route (DCS post-processes after DLSS, so no identity/copy proof exists).
        DcsQuadLayoutV1 l{}; l.structure_size = sizeof(l); l.valid = 1; l.session_generation = 1; l.publication_qpc = 1000;
        l.width = {96, 96, 64, 64}; l.height = {80, 80, 64, 64}; l.swapchain = {1, 2, 3, 4};
        s.views[0].swapchain_identity = 3; s.views[1].swapchain_identity = 4;
        DcsQuadHandleCensus census;
        for (std::uint64_t h = 1; h <= 4; ++h) census.record(h, h <= 2 ? 64 : 96, h <= 2 ? 64 : 80, 10);
        auto d = dcs_quad_focus_decide(1, true, s, 1001, 1000, 99, 0, 0, 64, 64, no_copy, &l, census, 11, false);
        check(d.allowed && std::string(d.route) == "size-bijection", "Size bijection rejected");
        check(!dcs_quad_focus_decide(3, true, s, 1001, 1000, 98, 0, 0, 96, 80, no_copy, &l, census, 11, false).allowed,
            "Peripheral accepted by size bijection");
        check(!dcs_quad_focus_decide(1, true, s, 1001, 1000, 99, 0, 0, 64, 64, no_copy, &l, census, 11, true).allowed,
            "Size bijection used while eye identity matters");
        bool nr{};
        check(cheeky::standalone::dcs_nr_enabled_from_snapshot(R"({"settings":{"NrEnabled":true}})", nr) && nr, "NrEnabled not parsed");
        check(cheeky::standalone::dcs_nr_toggle_command(5, false) == "1\n5\nset_session\nNrEnabled=0\n", "Toggle command format");
        std::cout << "PASS quad focus policy checks: routing, provenance, freshness, ambiguity, view selection, history recovery, size bijection, NR hotkey\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
