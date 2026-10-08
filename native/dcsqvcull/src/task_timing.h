// Per-task timing of Scene.dll's culling tasks. collectSceneObjectsRenderables
// runs one std::function task per (object collection, unique frustum group);
// their lambdas are forEachCollection<6>::collectSceneObjectsInFrustum2<N>.
// The std::function implementation vtables are not exported, so the slots are
// located by RVA for this Scene.dll build and verified through RTTI before
// patching. Each task's wall time is attributed to its collection and to the
// viewport tag / shading model of the group's representative CollectionInfo.
// Included once from main.cpp after the globals it uses (Log, g_qpcToUs).
#pragma once

namespace ttiming {

struct TaskSite {
  uint32_t vtableRva;  // _Func_impl_no_alloc vtable
  uint32_t doCallRva;  // expected slot 2 (_Do_call)
  int collection;
};
// Scene.dll 2.9.30 (RTTI: _Func_impl_no_alloc<lambda_1 of collectSceneObjectsInFrustum2<N>>).
const TaskSite kSites[] = {
    {0x2af68, 0x1b4a0, 0}, {0x2b050, 0x1a930, 1}, {0x2b118, 0x1aad0, 2}, {0x2b198, 0x1ac70, 3},
    {0x2b2e8, 0x1ae10, 4}, {0x2b418, 0x1afb0, 5}, {0x2b488, 0x1b150, 6},
};
constexpr int kSiteCount = sizeof(kSites) / sizeof(kSites[0]);

using DoCall = void(__fastcall*)(void*);
DoCall g_orig[kSiteCount];

struct Key {
  int collection;
  uint32_t tag;
  uint16_t shading;
  bool operator<(const Key& o) const {
    return std::tie(collection, tag, shading) < std::tie(o.collection, o.tag, o.shading);
  }
};
struct Stat {
  uint64_t calls = 0;
  double totalUs = 0, maxUs = 0;
};
std::mutex g_mutex;
std::map<Key, Stat> g_stats;
std::atomic<bool> g_recording{false};

// Reads the representative CollectionInfo of a task (self+0x10 -> group,
// group[0] -> info). Kept separate: __try cannot share a frame with objects
// that need unwinding.
void ReadGroup(void* self, uint32_t* tag, uint16_t* shading) {
  __try {
    auto group = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(self) + 0x10);
    auto info = group ? *reinterpret_cast<uint8_t**>(group) : nullptr;
    if (info) {
      *tag = *reinterpret_cast<uint32_t*>(info + layout::kCiViewportTag);
      *shading = *reinterpret_cast<uint16_t*>(info + layout::kCiShadingModel);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

void Record(int collection, void* self, double us) {
  Key k{collection, 0xffffffffu, 0xffff};
  ReadGroup(self, &k.tag, &k.shading);
  std::lock_guard<std::mutex> lock(g_mutex);
  Stat& s = g_stats[k];
  s.calls++;
  s.totalUs += us;
  if (us > s.maxUs) s.maxUs = us;
}

template <int I>
void __fastcall Hook(void* self) {
  if (!g_recording.load(std::memory_order_relaxed)) return g_orig[I](self);
  LARGE_INTEGER a, b;
  QueryPerformanceCounter(&a);
  g_orig[I](self);
  QueryPerformanceCounter(&b);
  Record(kSites[I].collection, self, (b.QuadPart - a.QuadPart) * g_qpcToUs);
}

void* const kHooks[kSiteCount] = {&Hook<0>, &Hook<1>, &Hook<2>, &Hook<3>, &Hook<4>, &Hook<5>, &Hook<6>};

// RTTI check: vtable[-1] is the complete object locator whose type descriptor
// name must mention collectSceneObjectsInFrustum2.
bool RttiMatches(uint8_t* base, void** vtbl) {
  __try {
    auto col = reinterpret_cast<uint8_t*>(vtbl[-1]);
    if (*reinterpret_cast<uint32_t*>(col) != 1) return false;
    auto td = base + *reinterpret_cast<uint32_t*>(col + 0xC);
    return strstr(reinterpret_cast<const char*>(td + 0x10), "collectSceneObjectsInFrustum2") != nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void Install(HMODULE scene) {
  auto base = reinterpret_cast<uint8_t*>(scene);
  int ok = 0;
  for (int i = 0; i < kSiteCount; ++i) {
    auto vtbl = reinterpret_cast<void**>(base + kSites[i].vtableRva);
    if (!RttiMatches(base, vtbl) || SlotOriginal(&vtbl[2]) != base + kSites[i].doCallRva) {
      Log("task timing: collection %d vtable does not match this Scene.dll build; skipped", kSites[i].collection);
      continue;
    }
    if (!HookSlot(&vtbl[2], kHooks[i], reinterpret_cast<void**>(&g_orig[i]))) continue;
    ++ok;
  }
  Log("task timing: %d/%d culling task types hooked", ok, kSiteCount);
}

// Records for `ms` milliseconds and logs the tasks sorted by time per frame.
void Measure(int ms, std::atomic<uint64_t>& frameCounter) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stats.clear();
  }
  uint64_t f0 = frameCounter.load();
  g_recording = true;
  Sleep(ms);
  g_recording = false;
  uint64_t frames = frameCounter.load() - f0;
  std::map<Key, Stat> snap;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    snap.swap(g_stats);
  }
  if (!frames) {
    Log("  no frames");
    return;
  }
  double f = static_cast<double>(frames);
  std::vector<std::pair<Key, Stat>> v(snap.begin(), snap.end());
  std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.totalUs > b.second.totalUs; });
  double sum = 0;
  double perCollection[8] = {};
  for (auto& kv : v) {
    sum += kv.second.totalUs;
    if (kv.first.collection >= 0 && kv.first.collection < 8) perCollection[kv.first.collection] += kv.second.totalUs;
  }
  Log("  task CPU %.2f ms/frame over %llu frames (sum of all culling tasks)", sum / 1000.0 / f,
      static_cast<unsigned long long>(frames));
  for (int c = 0; c < 7; ++c) Log("    collection %d: %.2f ms/frame", c, perCollection[c] / 1000.0 / f);
  Log("  longest tasks (collection, viewport tag, shading model):");
  for (size_t i = 0; i < v.size() && i < 20; ++i) {
    const Key& k = v[i].first;
    const Stat& s = v[i].second;
    Log("    col %d tag %5d shading %3d: %.3f ms/frame, %.2f calls/frame, max %.3f ms", k.collection,
        static_cast<int>(k.tag), k.shading, s.totalUs / 1000.0 / f, s.calls / f, s.maxUs / 1000.0);
  }
}

}  // namespace ttiming
