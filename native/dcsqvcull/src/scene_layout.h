// Reverse-engineered layouts for DCS World 2.9.30 Scene.dll.
// See REPORT_scene_culling.md for how each offset was derived.
#pragma once
#include <cstdint>

namespace layout {

// render::CollectionInfo (array stride used by SceneBase::collectRenderables).
constexpr size_t kCollectionInfoStride = 0x6D8;
constexpr size_t kCiShadingModel = 0x000;  // uint16_t, 0 = main colour pass
constexpr size_t kCiClipVolume = 0x008;    // render::ClippingVolume*
constexpr size_t kCiViewProj = 0x198;      // osg::Matrixf, camera relative
constexpr size_t kCiViewportTag = 0x690;   // render::ViewportTag (first dword is the id)
constexpr size_t kCiAuxCallback = 0x6D0;   // non-null for aux colour buffers

// Plane set ("planes block"), used both as the head of ClippingVolume and as an
// exclusion volume entry.
constexpr size_t kPlaneStride = 0x28;       // double n[3], double d, int pmask, int nmask
constexpr size_t kPlaneMaskP = 0x20;        // bit i set: use max on axis i (most positive vertex)
constexpr size_t kPlaneMaskN = 0x24;        // bit i set: use max on axis i (most negative vertex)
constexpr int kMaxPlanes = 10;              // 0x190 / 0x28
constexpr size_t kPlaneCount = 0x1A8;       // int32
constexpr size_t kPlaneExtraCount = 0x1AC;  // int32, secondary array at +0x190 (stride 0x18)
constexpr size_t kPlaneBlockSize = 0x1B0;

// render::ClippingVolume.
constexpr size_t kVolDistanceMax = 0x1F0;   // double
constexpr size_t kVolExclPtr = 0x2C0;       // small_vector<PlaneBlock, 2>: data pointer
constexpr size_t kVolExclSize = 0x2C8;      // uint64 size
constexpr size_t kVolExclCap = 0x2D0;       // uint64 capacity
constexpr size_t kVolExclInline = 0x2D8;    // inline storage, 2 entries
constexpr int kVolExclInlineCap = 2;
constexpr size_t kVolSize = kVolExclInline + kVolExclInlineCap * kPlaneBlockSize;  // 0x638

// ed::vector<ISceneRenderable*>: begin, end, capacity.
constexpr size_t kVectorStride = 0x18;

// Graphics::SceneBase<...> (the DCSScene object, i.e. IView subobject - 8).
constexpr size_t kSceneCollectThreadsMax = 0x40;  // uint32, clamped to 16 by the setter

}  // namespace layout
