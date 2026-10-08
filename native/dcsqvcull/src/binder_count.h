// MetaShader binder repeat counter (measurement only). metaShader.dll's
// MetaShaderImpl2 (vtable RVA 0xd9cc8) slot 15 = render(item, context, pass)
// and slot 14 = setupParams(item, context) walk the shader's binder map and
// call every binder's slot 4 with (shader, binder data, item->subItems[i],
// context) [V 0x71fa0, 0x723a0]. When a call has the same shader, item
// sub-item array and context as the call just before it, every binder sees
// the same inputs again: the count is an upper bound on what a binder memo
// could skip (R8 C2).
// Included once from main.cpp inside its anonymous namespace.
#pragma once

namespace bindcount {

constexpr uint32_t kVtableRva = 0xd9cc8;
using RenderFn = uint64_t(__fastcall*)(void* self, void* item, void* ctx, uint64_t pass);
using SetupFn = uint64_t(__fastcall*)(void* self, void* item, void* ctx, uint64_t a4);
RenderFn g_origRender = nullptr;
SetupFn g_origSetup = nullptr;
std::atomic<bool> g_on{false};

struct Key {
  void* shader = nullptr;
  void* subItems = nullptr;
  void* firstSub = nullptr;
  void* ctx = nullptr;
};
struct Counts {
  uint64_t calls = 0, sameAll = 0, sameShader = 0;
};
Counts g_render, g_setup;
Key g_last;  // previous call of either slot (render thread only)
DWORD g_thread = 0;

Key MakeKey(void* self, void* item, void* ctx) {
  Key k{self, nullptr, nullptr, ctx};
  if (item) {
    k.subItems = *reinterpret_cast<void**>(static_cast<uint8_t*>(item) + 0x18);
    if (k.subItems) k.firstSub = *static_cast<void**>(k.subItems);
  }
  return k;
}

void Note(Counts& c, const Key& k) {
  if (GetCurrentThreadId() != g_thread) {
    if (g_thread) return;
    g_thread = GetCurrentThreadId();
  }
  c.calls++;
  if (k.shader == g_last.shader) {
    c.sameShader++;
    if (k.subItems == g_last.subItems && k.firstSub == g_last.firstSub && k.ctx == g_last.ctx) c.sameAll++;
  }
  g_last = k;
}

uint64_t __fastcall HookRender(void* self, void* item, void* ctx, uint64_t pass) {
  if (g_on.load(std::memory_order_relaxed)) Note(g_render, MakeKey(self, item, ctx));
  return g_origRender(self, item, ctx, pass);
}

uint64_t __fastcall HookSetup(void* self, void* item, void* ctx, uint64_t a4) {
  if (g_on.load(std::memory_order_relaxed)) Note(g_setup, MakeKey(self, item, ctx));
  return g_origSetup(self, item, ctx, a4);
}

bool Install() {
  if (g_origRender) return true;
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"metaShader.dll"));
  if (!base) return false;
  auto** vtbl = reinterpret_cast<void**>(base + kVtableRva);
  if (!allocslab::RttiIs(base, vtbl, ".?AVMetaShaderImpl2@render@@")) {
    Log("binder counters: vtable does not match this build; skipped");
    return false;
  }
  g_origRender = reinterpret_cast<RenderFn>(SlotOriginal(&vtbl[15]));
  g_origSetup = reinterpret_cast<SetupFn>(SlotOriginal(&vtbl[14]));
  return HookSlot(&vtbl[15], reinterpret_cast<void*>(&HookRender), nullptr) &&
         HookSlot(&vtbl[14], reinterpret_cast<void*>(&HookSetup), nullptr);
}

void Measure(int ms, std::atomic<uint64_t>& frames) {
  if (!Install()) return;
  g_thread = 0;
  g_render = {};
  g_setup = {};
  g_last = {};
  uint64_t f0 = frames.load();
  g_on = true;
  Sleep(ms);
  g_on = false;
  Sleep(50);
  double f = static_cast<double>(frames.load() - f0);
  if (f <= 0) return;
  auto line = [&](const char* name, const Counts& c) {
    Log("  metashader %s: %.0f calls/frame, same shader as previous call %.1f%%, same shader+item+context %.1f%%",
        name, c.calls / f, c.calls ? 100.0 * c.sameShader / c.calls : 0.0,
        c.calls ? 100.0 * c.sameAll / c.calls : 0.0);
  };
  line("render", g_render);
  line("setupParams", g_setup);
}

}  // namespace bindcount

// InstanceManager2 vtable (edterrainGraphics41 RVA 0x14f4f8) slot 19 =
// per-view instance visibility marking in the terrain parse (R8 C1, 0x9fbf0,
// four register arguments, no stack arguments [V]). Time per frame and the
// thread it runs on: a rewrite can only gain while that thread is on the
// critical path.
namespace imcount {

constexpr uint32_t kVtableRva = 0x14f4f8;
using Fn = uint64_t(__fastcall*)(void*, void*, void*, void*);
Fn g_orig = nullptr;
std::atomic<bool> g_on{false};
std::atomic<uint64_t> g_calls{0}, g_cycles{0}, g_renderThreadCalls{0};
DWORD g_renderThread = 0;

uint64_t __fastcall Hook(void* a, void* b, void* c, void* d) {
  if (!g_on.load(std::memory_order_relaxed)) return g_orig(a, b, c, d);
  uint64_t t0 = __rdtsc();
  uint64_t r = g_orig(a, b, c, d);
  g_cycles += __rdtsc() - t0;
  g_calls++;
  if (GetCurrentThreadId() == g_renderThread) g_renderThreadCalls++;
  return r;
}

bool Install() {
  if (g_orig) return true;
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"edterrainGraphics41.dll"));
  if (!base) return false;
  auto** vtbl = reinterpret_cast<void**>(base + kVtableRva);
  if (!allocslab::RttiIs(base, vtbl, ".?AVInstanceManager2@edtg41@@") ||
      reinterpret_cast<uint8_t*>(SlotOriginal(&vtbl[19])) != base + 0x9fbf0) {
    Log("instance-manager counter: vtable does not match this build; skipped");
    return false;
  }
  g_orig = reinterpret_cast<Fn>(SlotOriginal(&vtbl[19]));
  return HookSlot(&vtbl[19], reinterpret_cast<void*>(&Hook), nullptr);
}

void Measure(int ms, std::atomic<uint64_t>& frames, double tscHz, DWORD renderThread) {
  if (!Install()) return;
  g_renderThread = renderThread;
  g_calls = 0;
  g_cycles = 0;
  g_renderThreadCalls = 0;
  uint64_t f0 = frames.load();
  g_on = true;
  Sleep(ms);
  g_on = false;
  Sleep(50);
  double f = static_cast<double>(frames.load() - f0);
  if (f <= 0) return;
  Log("  instance visibility marking (slot 19): %.2f calls/frame (%.0f%% on the render thread), %.3f ms/frame",
      g_calls / f, g_calls ? 100.0 * g_renderThreadCalls / g_calls : 0.0, g_cycles / tscHz * 1000.0 / f);
}

}  // namespace imcount
