// DCS VR Control test fixture named CheekyOpenXRLayer.dll: the two exports of the
// adapted OpenXR layer that the NGX gate reads (gaze snapshot and the four-view
// Quad Views layout), published by the test instead of an OpenXR session.
#include "cheeky_gaze_abi.h"
#include "dcs_quad_focus_policy.hpp"
#include <cstdint>
#include <cstring>

namespace {
CheekyGazeSnapshotV1 fake_snapshot{};
cheeky::foveated_dlss::DcsQuadLayoutV1 fake_layout{};
bool fake_has_layout{};
}

#define EXPORT extern "C" __declspec(dllexport)
// Null snapshot: none published (ABI 0). Null layout: the layer has no layout yet.
EXPORT void CheekyFakeDcsLayer(const CheekyGazeSnapshotV1* snapshot, const cheeky::foveated_dlss::DcsQuadLayoutV1* layout) {
    fake_snapshot = snapshot ? *snapshot : CheekyGazeSnapshotV1{};
    fake_has_layout = layout != nullptr;
    fake_layout = layout ? *layout : cheeky::foveated_dlss::DcsQuadLayoutV1{};
}
EXPORT std::uint32_t __cdecl CheekyOpenXR_GetGazeSnapshot(std::uint32_t abi, void* output, std::uint32_t size) {
    if (abi != CHEEKY_GAZE_ABI_VERSION || output == nullptr || size < sizeof(fake_snapshot)) return 0U;
    std::memcpy(output, &fake_snapshot, sizeof(fake_snapshot));
    return 1U;
}
EXPORT std::uint32_t __cdecl CheekyOpenXR_GetDcsQuadLayout(void* output, std::uint32_t size) {
    if (!fake_has_layout || output == nullptr || size < sizeof(fake_layout)) return 0U;
    std::memcpy(output, &fake_layout, sizeof(fake_layout));
    return 1U;
}
