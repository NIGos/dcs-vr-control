// Timeline of the per-frame culling pipeline, to find where the render thread
// waits. Measured phases (wall time per frame and the thread they run on):
//   aggregated : GraphicsCore!collectRenderablesAggregated, called by
//                Visualizer on the render thread (IAT hook)
//   provider   : SceneBase::collectRenderables (ISceneObjectsProvider slot 1)
//   objects    : IView::collectSceneObjectsRenderables (timed by HookCollect)
//   sort       : IView::sortAndBatchRenderables (slot 4)
// Included once from main.cpp after the globals it uses (Log, g_qpcToUs).
#pragma once

namespace ctiming {

enum Phase { kAggregated, kProvider, kObjects, kSort, kPhaseCount };
const char* const kPhaseNames[kPhaseCount] = {"aggregated (render thread)", "provider collectRenderables",
                                              "collect objects", "sort+batch"};

struct Acc {
  std::atomic<uint64_t> us{0}, calls{0};
  std::atomic<DWORD> lastTid{0};
  std::atomic<uint64_t> tidChanges{0};
};
Acc g_acc[kPhaseCount];

inline int64_t Now() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}

inline void Add(Phase p, int64_t t0, int64_t t1) {
  Acc& a = g_acc[p];
  a.us.fetch_add(static_cast<uint64_t>((t1 - t0) * g_qpcToUs), std::memory_order_relaxed);
  a.calls.fetch_add(1, std::memory_order_relaxed);
  DWORD tid = GetCurrentThreadId();
  if (a.lastTid.exchange(tid) != tid) a.tidChanges.fetch_add(1, std::memory_order_relaxed);
}

// ---- hooks ----
using ProviderFn = bool(__fastcall*)(void*, uint32_t, void*, void*, void*, uint32_t);
using SortFn = void(__fastcall*)(void*, uint32_t, void*, void*, void*);
using AggregatedFn = void(__cdecl*)(uint32_t, void*, void*, bool, void*);
ProviderFn g_provider = nullptr;
SortFn g_sort = nullptr;
AggregatedFn g_aggregated = nullptr;
// Measurement observer (frame_start.h: culling start / end on the render
// thread): called with the QPC before (begin) and after the call; null = off.
std::atomic<void (*)(int64_t, bool)> g_aggObserver{nullptr};

bool __fastcall HookProvider(void* self, uint32_t n, void* infos, void* outs, void* tq, uint32_t f) {
  int64_t t0 = Now();
  bool r = g_provider(self, n, infos, outs, tq, f);
  Add(kProvider, t0, Now());
  return r;
}

void __fastcall HookSort(void* self, uint32_t n, void* infos, void* outs, void* tq) {
  int64_t t0 = Now();
  g_sort(self, n, infos, outs, tq);
  Add(kSort, t0, Now());
}

void __cdecl HookAggregated(uint32_t n, void* req, void* tq, bool b, void* stats) {
  int64_t t0 = Now();
  void (*ob)(int64_t, bool) = g_aggObserver.load(std::memory_order_relaxed);
  if (ob) ob(t0, true);
  g_aggregated(n, req, tq, b, stats);
  const int64_t t1 = Now();
  Add(kAggregated, t0, t1);
  if (ob) ob(t1, false);
}

bool PatchPtr(void** slot, void* hook, void** orig) { return HookSlot(slot, hook, orig); }

void Install(HMODULE scene) {
  // ISceneObjectsProvider vtable of DCSScene: slot 1 = collectRenderables.
  auto prov = reinterpret_cast<void**>(GetProcAddress(scene, "??_7DCSScene@@6BISceneObjectsProvider@render@@@"));
  void* provFn = reinterpret_cast<void*>(GetProcAddress(
      scene,
      "?collectRenderables@?$SceneBase@UObjectCollections@DCSSceneCollections@@ULightCollections@2@@Graphics@@"
      "UEBA_NIQEBUCollectionInfo@render@@QEAV?$vector@PEAUISceneRenderable@render@@V?$allocator@"
      "PEAUISceneRenderable@render@@@ed@@@ed@@PEAVTaskQueue@6@I@Z"));
  if (prov && provFn && SlotOriginal(&prov[1]) == provFn && PatchPtr(&prov[1], reinterpret_cast<void*>(&HookProvider), reinterpret_cast<void**>(&g_provider)))
    Log("timeline: hooked collectRenderables");
  // IView vtable slot 4 = sortAndBatchRenderables.
  auto view = reinterpret_cast<void**>(GetProcAddress(scene, "??_7DCSScene@@6BIView@SceneAggregator@Graphics@@@"));
  void* sortFn = reinterpret_cast<void*>(GetProcAddress(
      scene,
      "?sortAndBatchRenderables@?$SceneBase@UObjectCollections@DCSSceneCollections@@ULightCollections@2@@"
      "Graphics@@MEBAXIQEBUCollectionInfo@render@@QEAV?$vector@PEAUISceneRenderable@render@@V?$allocator@"
      "PEAUISceneRenderable@render@@@ed@@@ed@@PEAVTaskQueue@6@@Z"));
  if (view && sortFn && SlotOriginal(&view[4]) == sortFn && PatchPtr(&view[4], reinterpret_cast<void*>(&HookSort), reinterpret_cast<void**>(&g_sort)))
    Log("timeline: hooked sortAndBatchRenderables");
  // Visualizer's import of GraphicsCore!collectRenderablesAggregated.
  HMODULE vis = GetModuleHandleW(L"Visualizer.dll");
  void** slot = vis ? timercache::FindImport(
                          vis, "GraphicsCore.dll",
                          "?collectRenderablesAggregated@SceneAggregator@Graphics@@YAXIQEAUCollectionRequest@12@"
                          "PEAVTaskQueue@ed@@_NPEAUCollectRenderablesResult@SceneStatistics@@@Z")
                    : nullptr;
  if (slot && PatchPtr(slot, reinterpret_cast<void*>(&HookAggregated), reinterpret_cast<void**>(&g_aggregated)))
    Log("timeline: hooked collectRenderablesAggregated (render thread)");
}

struct Snap {
  uint64_t us[kPhaseCount], calls[kPhaseCount], tidChanges[kPhaseCount];
};
Snap Take() {
  Snap s{};
  for (int i = 0; i < kPhaseCount; ++i) {
    s.us[i] = g_acc[i].us.load();
    s.calls[i] = g_acc[i].calls.load();
    s.tidChanges[i] = g_acc[i].tidChanges.load();
  }
  return s;
}

void Report(const Snap& a, const Snap& b, uint64_t frames, double frameMs) {
  if (!frames) return;
  double f = static_cast<double>(frames);
  Log("  frame %.2f ms over %llu frames", frameMs, static_cast<unsigned long long>(frames));
  for (int i = 0; i < kPhaseCount; ++i) {
    uint64_t c = b.calls[i] - a.calls[i];
    if (!c) {
      Log("    %-30s not called", kPhaseNames[i]);
      continue;
    }
    double ms = (b.us[i] - a.us[i]) / 1000.0 / f;
    Log("    %-30s %6.2f ms/frame (%4.1f%% of frame)  calls/frame %.2f  thread changes %.0f%%", kPhaseNames[i], ms,
        frameMs > 0 ? 100.0 * ms / frameMs : 0.0, c / f, 100.0 * (b.tidChanges[i] - a.tidChanges[i]) / c);
  }
}

}  // namespace ctiming
