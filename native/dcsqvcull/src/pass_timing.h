// Render-thread CPU time per rendering pass. Every pass of DCS's render graph
// runs through the virtual GraphicsCore!render::BaseRenderingPass::execute,
// referenced by GraphicsCore's pass vtables and imported by SceneRenderer.dll
// (whose pass vtables reach it through the import). Both are redirected; each
// pass object's exclusive time (nested passes subtracted) is attributed to the
// pass, named from its RTTI class.
// Included once from main.cpp after the globals it uses (Log, g_qpcToUs).
#pragma once

namespace ptiming {

const char kExecuteExport[] = "?execute@BaseRenderingPass@render@@UEAAX_K@Z";
using ExecFn = void(__fastcall*)(void* pass, uint64_t frame);
ExecFn g_orig = nullptr;
std::atomic<bool> g_recording{false};

struct Stat {
  uint64_t calls = 0;
  double exclUs = 0, inclUs = 0;
};
std::mutex g_mutex;
std::unordered_map<void*, Stat> g_stats;
std::atomic<uint64_t> g_frames{0};
double g_topLevelUs = 0;  // guarded by g_mutex

thread_local int t_depth = 0;
std::atomic<DWORD> g_topTid{0};  // thread running top-level passes (the render thread)
thread_local double t_childUs[64];

void __fastcall Hook(void* pass, uint64_t frame) {
  if (t_depth == 0) g_topTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
  if (!g_recording.load(std::memory_order_relaxed)) return g_orig(pass, frame);
  int d = t_depth++;
  if (d < 64) t_childUs[d] = 0;
  LARGE_INTEGER a, b;
  QueryPerformanceCounter(&a);
  g_orig(pass, frame);
  QueryPerformanceCounter(&b);
  t_depth--;
  double us = (b.QuadPart - a.QuadPart) * g_qpcToUs;
  double excl = d < 64 ? us - t_childUs[d] : us;
  if (d > 0 && d - 1 < 64) t_childUs[d - 1] += us;
  std::lock_guard<std::mutex> lock(g_mutex);
  Stat& s = g_stats[pass];
  s.calls++;
  s.inclUs += us;
  s.exclUs += excl;
  if (d == 0) g_topLevelUs += us;
}

void RttiRaw(void* obj, char* buf, size_t size) {
  __try {
    void** vt = *reinterpret_cast<void***>(obj);
    auto col = reinterpret_cast<uint8_t*>(vt[-1]);
    if (*reinterpret_cast<uint32_t*>(col) == 1) {
      auto self = *reinterpret_cast<uint32_t*>(col + 0x14);
      auto base = col - self;
      auto td = base + *reinterpret_cast<uint32_t*>(col + 0xC);
      strncpy_s(buf, size, reinterpret_cast<const char*>(td + 0x10), _TRUNCATE);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// Short readable name from the pass object's RTTI.
std::string RttiName(void* obj) {
  char buf[512] = {};
  RttiRaw(obj, buf, sizeof(buf));
  std::string s = buf;
  // Pick the most telling token: a "...PassData" struct, a "...Module", or the class.
  size_t p = s.find("PassData@");
  if (p != std::string::npos) {
    size_t b = s.rfind('U', p);
    size_t q = s.rfind("?$", p);
    size_t start = (b != std::string::npos && (q == std::string::npos || b > q)) ? b + 1 : 0;
    return s.substr(start, p + 8 - start) + (s.find("parseShadowMap") != std::string::npos ? " (shadow)" : "");
  }
  for (const char* key : {"Module@", "Pass@"}) {
    p = s.find(key);
    if (p != std::string::npos) {
      size_t b = s.rfind('@', p - 1);
      size_t start = b == std::string::npos ? 0 : b + 1;
      size_t a = s.rfind("V", p);
      if (a != std::string::npos && a + 1 > start) start = a + 1;
      return s.substr(start, p + strlen(key) - 1 - start);
    }
  }
  return s.size() > 60 ? s.substr(0, 60) : s;
}

// Patches every pointer to execute inside GraphicsCore's read-only data (its
// own pass vtables) and SceneRenderer's import of it.
void Install() {
  HMODULE gc = GetModuleHandleW(L"GraphicsCore.dll");
  if (!gc) return;
  void* exec = reinterpret_cast<void*>(GetProcAddress(gc, kExecuteExport));
  if (!exec) {
    Log("pass timing: execute export not found");
    return;
  }
  g_orig = reinterpret_cast<ExecFn>(exec);
  int n = 0;
  auto base = reinterpret_cast<uint8_t*>(gc);
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
  auto sec = IMAGE_FIRST_SECTION(nt);
  for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
    if (memcmp(sec->Name, ".rdata", 6) != 0) continue;
    auto p = reinterpret_cast<void**>(base + sec->VirtualAddress);
    size_t count = sec->Misc.VirtualSize / sizeof(void*);
    for (size_t k = 0; k < count; ++k)
      if (SlotOriginal(&p[k]) == exec && HookSlot(&p[k], reinterpret_cast<void*>(&Hook), nullptr)) ++n;
  }
  if (HMODULE sr = GetModuleHandleW(L"SceneRenderer.dll"))
    if (void** slot = timercache::FindImport(sr, "GraphicsCore.dll", kExecuteExport))
      if (HookSlot(slot, reinterpret_cast<void*>(&Hook), nullptr)) ++n;
  Log("pass timing: %d references to BaseRenderingPass::execute redirected", n);
}

void Measure(int ms, std::atomic<uint64_t>& frameCounter) {
  if (!g_orig) {
    Log("  pass timing not installed");
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stats.clear();
    g_topLevelUs = 0;
  }
  uint64_t f0 = frameCounter.load();
  g_recording = true;
  Sleep(ms);
  g_recording = false;
  Sleep(50);
  uint64_t frames = frameCounter.load() - f0;
  std::unordered_map<void*, Stat> snap;
  double top;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    snap.swap(g_stats);
    top = g_topLevelUs;
  }
  if (!frames) return;
  double f = static_cast<double>(frames);
  // Merge pass objects with the same name.
  std::map<std::string, Stat> byName;
  for (auto& kv : snap) {
    Stat& s = byName[RttiName(kv.first)];
    s.calls += kv.second.calls;
    s.exclUs += kv.second.exclUs;
    s.inclUs += kv.second.inclUs;
  }
  std::vector<std::pair<std::string, Stat>> v(byName.begin(), byName.end());
  std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.exclUs > b.second.exclUs; });
  Log("  render passes: %.2f ms/frame of pass execution (%zu pass objects, %zu kinds)", top / 1000.0 / f,
      snap.size(), v.size());
  for (size_t i = 0; i < v.size() && i < 30; ++i)
    Log("    %-48.48s %6.3f ms excl  %6.3f ms incl  calls/frame %.1f", v[i].first.c_str(), v[i].second.exclUs / 1000.0 / f,
        v[i].second.inclUs / 1000.0 / f, v[i].second.calls / f);
}

}  // namespace ptiming
