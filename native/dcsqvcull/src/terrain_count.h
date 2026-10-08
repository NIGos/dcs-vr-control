// Terrain draw counters (measurement only). edterrainGraphics41's
// TerrainRenderable vtable (RVA 0x150ec0) slot 6 = render(shadingModel,
// context, flags). Counts calls and time per (renderable, shading model) per
// frame, to see whether any terrain layer is drawn more often than the views
// and passes require.
// Included once from main.cpp inside its anonymous namespace.
#pragma once

namespace terrcount {

constexpr uint32_t kVtableRva = 0x150ec0;
constexpr int kSlot = 6;
using Fn = void(__fastcall*)(void* self, int shading, void* ctx, uint32_t flags);
Fn g_orig = nullptr;
std::mutex g_mutex;
struct Stat {
  uint64_t calls = 0, cycles = 0;
  uint64_t lodPasses = 0;  // slot-5 calls the original makes (one per LOD-list entry, at least 1)
};

// Entries in the renderable's LOD list (small vector of int at +0x790, size
// word at +0x810: negative = inline with -size entries, -0x21 = empty, else
// heap begin/end), read the same way slot 6 does [V 0x12e41c].
uint64_t LodListSize(void* self) {
  auto* p = static_cast<uint8_t*>(self);
  int64_t v = *reinterpret_cast<int64_t*>(p + 0x810);
  if (v < 0) return v == -0x21 ? 0 : static_cast<uint64_t>(-v);
  auto* b = *reinterpret_cast<uint8_t**>(p + 0x790);
  auto* e = *reinterpret_cast<uint8_t**>(p + 0x798);
  return static_cast<uint64_t>(e - b) / 4;
}
std::map<std::pair<void*, int>, Stat> g_stats;
std::atomic<bool> g_on{false};

void __fastcall Hook(void* self, int shading, void* ctx, uint32_t flags) {
  if (!g_on.load(std::memory_order_relaxed)) return g_orig(self, shading, ctx, flags);
  uint64_t t0 = __rdtsc();
  g_orig(self, shading, ctx, flags);
  uint64_t dt = __rdtsc() - t0;
  uint64_t k = LodListSize(self);
  std::lock_guard<std::mutex> lock(g_mutex);
  Stat& s = g_stats[{self, shading}];
  s.calls++;
  s.cycles += dt;
  s.lodPasses += k ? k : 1;
}

bool Install() {
  if (g_orig) return true;
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"edterrainGraphics41.dll"));
  if (!base) return false;
  auto** vtbl = reinterpret_cast<void**>(base + kVtableRva);
  if (!allocslab::RttiIs(base, vtbl, ".?AVTerrainRenderable@edtg41@@")) {
    Log("terrain counters: vtable does not match this build; skipped");
    return false;
  }
  g_orig = reinterpret_cast<Fn>(SlotOriginal(&vtbl[kSlot]));
  return HookSlot(&vtbl[kSlot], reinterpret_cast<void*>(&Hook), nullptr);
}

void Measure(int ms, std::atomic<uint64_t>& frames, double tscHz) {
  if (!Install()) return;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stats.clear();
  }
  uint64_t f0 = frames.load();
  g_on = true;
  Sleep(ms);
  g_on = false;
  double f = static_cast<double>(frames.load() - f0);
  if (f <= 0) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  double total = 0;
  for (auto& kv : g_stats) total += kv.second.cycles;
  Log("  terrain render calls: %zu (renderable, shading) pairs, %.3f ms/frame total", g_stats.size(),
      total / tscHz * 1000.0 / f);
  for (auto& kv : g_stats)
    Log("    renderable %p shading %3d: %.2f calls/frame, %.3f ms/frame, %.2f LOD passes/call", kv.first.first,
        kv.first.second, kv.second.calls / f, kv.second.cycles / tscHz * 1000.0 / f,
        kv.second.calls ? static_cast<double>(kv.second.lodPasses) / kv.second.calls : 0.0);
}

}  // namespace terrcount
