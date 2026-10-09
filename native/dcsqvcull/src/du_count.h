// Gate counters for direct writes of model data into mapped GPU pages
// (R16 §7, G1-G3). Measurement only: nothing DCS sees changes, every hook
// forwards its call unchanged and is removed when the phase ends.
//
// Window: from the first collect call after the per-frame page reset
// (StructBufferManager slot 7, BeginParse) to the end of the per-frame upload
// (slot 8, 0xbdb0, EndParse). That is where direct upload would keep the
// model data pages mapped.
//
//  G1  Draws and dispatches inside the window. Every DCS draw goes through
//      DX11Renderer: draw, drawWithOffset, drawIndirect, drawMultIndirect,
//      compute and computeIndirect (exports, located in the vtable at run
//      time). They are wrapped in the real vtable via HookSlot for the phase
//      only. Shadow and G-buffer batching swap the renderer object to their
//      own copy of the vtable (shadowbatch::g_myVtbl) around a leader's draw;
//      that copy's draw entries are wrapped too (each wrapper forwards to the
//      entry it replaced, DrawN for slot 35) and put back afterwards, so the
//      batched draws are counted as well. For each draw inside the window,
//      every SRV bound to the stages the draw uses is checked against the
//      model data pages' ID3D11Buffers (the hazard: a mapped buffer bound to a
//      draw). With [D3D] Meter=1 the d3d11 draw count of the render thread
//      inside the window is reported as a cross-check of the renderer level.
//  G2  StructBufferManager::allocate calls (alloc_slab.h detour) outside the
//      window, and inside it from threads that are neither the render thread
//      nor culling workers (threads that allocate inside at least 10 % of the
//      windows).
//  G3  Pages with used > 0 when the window opens.
// Also: bytes, pages and time of the slot-8 upload per frame, and the used
// bytes of each page.
//
// Counting runs over whole parses: it starts and stops at a page reset.
// Included once from main.cpp inside its anonymous namespace, after
// alloc_slab.h, d3dstate.h, shadow_batch.h and par_upload.h.
#pragma once

namespace ducount {

constexpr int kUploadSlot = 8;
constexpr uint32_t kUploadRva = 0xbdb0;
constexpr size_t kSbD3dBuffer = 0x20;  // DX11StructuredBuffer: ID3D11Buffer* [V dx11backend 0x3222d]
constexpr size_t kSbRenderer = 0x10;   // DX11StructuredBuffer: DX11Renderer* [V dx11backend 0x32226]
constexpr size_t kRendererCtx = 0x30;  // DX11Renderer: immediate context [V dx11backend 0x32239]

enum Kind { kDraw, kDrawOffset, kDrawIndirect, kDrawMultIndirect, kCompute, kComputeIndirect, kKinds };
const char* const kKindNames[kKinds] = {"draw", "drawWithOffset", "drawIndirect", "drawMultIndirect", "compute",
                                        "computeIndirect"};
const char* const kExports[kKinds] = {
    "?draw@DX11Renderer@RenderAPI@@UEAAXHPEAUIShader@2@W4PRIMTYPE_ENUM@render@@HHHPEBD@Z",
    "?drawWithOffset@DX11Renderer@RenderAPI@@UEAAXHPEAUIShader@2@W4PRIMTYPE_ENUM@render@@HHH@Z",
    "?drawIndirect@DX11Renderer@RenderAPI@@UEAAXHPEAUIShader@2@W4PRIMTYPE_ENUM@render@@PEAUIBuffer@2@HPEBD@Z",
    "?drawMultIndirect@DX11Renderer@RenderAPI@@UEAAXHPEAUIShader@2@W4PRIMTYPE_ENUM@render@@PEAUIBuffer@2@P6AHHPEAX@"
    "Z3PEBD@Z",
    "?compute@DX11Renderer@RenderAPI@@UEAAXHPEAUIShader@2@HHH@Z",
    "?computeIndirect@DX11Renderer@RenderAPI@@UEAAXHPEAUIShader@2@PEAUIBuffer@2@I@Z",
};

// All six take only integer and pointer arguments (at most 7 after `this`)
// and return nothing, so one forwarding shape fits all: the wrapper passes
// on 7 argument slots whatever the callee uses.
using Fwd = void(__fastcall*)(void*, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
// [0, kKinds): real vtable; [kKinds, 2 kKinds): shadowbatch's vtable copy.
// Never cleared: a copy taken while our wrappers were in place keeps working.
Fwd g_fwd[2 * kKinds] = {};
void** g_slot[kKinds] = {};      // real vtable slots we hooked (null = not hooked)
void** g_copySlot[kKinds] = {};  // copy entries we replaced

using UploadFn = void(__fastcall*)(void* mgr);
UploadFn g_origUpload = nullptr;
void** g_uploadSlot = nullptr;

// Phase control. g_counting goes on and off at a page reset only.
std::atomic<bool> g_armed{false}, g_stopReq{false}, g_counting{false};
std::atomic<int> g_phase{0};  // 0 = outside, 1 = culling window, 2 = inside the slot-8 upload
std::atomic<bool> g_parse{false};  // a reset was seen and its upload not yet
std::atomic<uint32_t> g_winId{0};
std::atomic<DWORD> g_renderTid{0};

struct Counters {
  std::atomic<uint64_t> parses{0}, windows{0}, closed{0}, unclosed{0}, collectInWin{0}, collectOutside{0};
  std::atomic<uint64_t> draws[kKinds] = {}, winDraws[kKinds] = {}, winOther{0}, winHazard{0}, winHazardOther{0},
                                        winCtxUnknown{0};
  std::atomic<uint64_t> d3dWin{0};
  std::atomic<uint64_t> g3Windows{0}, g3Pages{0}, g3Bytes{0}, mapPages{0}, bufPages{0}, noMgr{0};
  std::atomic<uint64_t> uploadCalls{0}, uploadNoWin{0}, uploadBytes{0}, uploadPages{0}, uploadNoBuf{0},
      uploadCycles{0}, uploadMaxCycles{0};
};
Counters g_c;
uint64_t g_d3dOpen = 0;  // render thread only

// Model data page buffers at window open (render thread only).
constexpr int kMaxBufs = 4096;
void* g_bufs[kMaxBufs];
int g_nBufs = 0;
void* g_ctx = nullptr;  // the renderer's immediate context, from a page buffer
void* g_sbVtbl = nullptr;  // DX11StructuredBuffer vtable

// Per-element-size totals over the upload calls, and the last upload's pages.
struct ClassAcc {
  uint32_t elemSize = 0;
  uint64_t bytes = 0, pages = 0;
  uint32_t maxPage = 0, capMax = 0;
};
constexpr int kClasses = 8;
ClassAcc g_cls[kClasses];
struct PageSnap {
  uint32_t idx, elemSize, used, cap;
  bool buf;
};
constexpr int kMaxSnap = 512;
PageSnap g_snap[kMaxSnap];
int g_nSnap = 0, g_snapTotal = 0;

// Allocating threads.
struct Row {
  std::atomic<DWORD> tid{0};
  std::atomic<uint64_t> inWin{0}, pre{0}, upload{0}, post{0}, windows{0};
  uint32_t lastWin = 0;  // owner thread only
};
constexpr int kRows = 64;
Row g_rows[kRows + 1];  // the last one collects overflow threads
std::atomic<int> g_nRows{0};

Row* MyRow() {
  thread_local Row* r = nullptr;
  if (r) return r;
  const DWORD tid = GetCurrentThreadId();
  int i = g_nRows.fetch_add(1);
  if (i >= kRows) {
    g_nRows.store(kRows);
    r = &g_rows[kRows];
  } else {
    r = &g_rows[i];
    r->tid = tid;
  }
  return r;
}

bool ReadPtr(const void* at, void** out) {
  __try {
    *out = *reinterpret_cast<void* const*>(at);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// ---- G2 ----
void OnAlloc(uint32_t) {
  if (!g_counting.load(std::memory_order_relaxed)) return;
  Row* r = MyRow();
  const int ph = g_phase.load(std::memory_order_acquire);
  if (ph == 1) {
    const uint32_t w = g_winId.load(std::memory_order_relaxed);
    if (r != &g_rows[kRows] && r->lastWin != w) {
      r->lastWin = w;
      r->windows.fetch_add(1, std::memory_order_relaxed);
    }
    r->inWin.fetch_add(1, std::memory_order_relaxed);
  } else if (ph == 2) {
    r->upload.fetch_add(1, std::memory_order_relaxed);
  } else if (g_parse.load(std::memory_order_relaxed)) {
    r->pre.fetch_add(1, std::memory_order_relaxed);
  } else {
    r->post.fetch_add(1, std::memory_order_relaxed);
  }
}

// ---- window ----
void OnReset(void*) {
  if (g_counting.load()) {
    if (g_phase.load() != 0) g_c.unclosed++;
    if (g_stopReq.load()) {
      g_counting = false;
      g_armed = false;
    }
  } else if (g_armed.load() && !g_stopReq.load()) {
    g_counting = true;
  }
  g_phase = 0;
  g_parse = true;
  if (g_counting.load()) g_c.parses++;
}

// Page records under the manager's lock: G3, and the page buffers of this frame.
void ScanAtOpen(void* mgr) {
  uint8_t* m = static_cast<uint8_t*>(mgr);
  allocslab::g_lock(m + allocslab::kMgrMutex);
  uint8_t* b = *reinterpret_cast<uint8_t**>(m + 8);
  uint8_t* e = *reinterpret_cast<uint8_t**>(m + 0x10);
  uint64_t g3p = 0, g3b = 0, map = 0, bufs = 0;
  g_nBufs = 0;
  for (uint8_t* p = b; p && p < e; p += allocslab::kPageStride) {
    const auto* pg = reinterpret_cast<const allocslab::Page*>(p);
    if (pg->used) {
      ++g3p;
      g3b += pg->used;
    }
    if (!pg->gpuBuf) continue;
    ++bufs;
    if (pg->capBytes && pg->prevUsed) ++map;
    void* vt = nullptr;
    void* d3d = nullptr;
    if (!ReadPtr(pg->gpuBuf, &vt) || vt != g_sbVtbl ||
        !ReadPtr(static_cast<uint8_t*>(pg->gpuBuf) + kSbD3dBuffer, &d3d) || !d3d)
      continue;
    if (!g_ctx) {
      void* renderer = nullptr;
      if (ReadPtr(static_cast<uint8_t*>(pg->gpuBuf) + kSbRenderer, &renderer) && renderer)
        ReadPtr(static_cast<uint8_t*>(renderer) + kRendererCtx, &g_ctx);
    }
    if (g_nBufs < kMaxBufs) g_bufs[g_nBufs++] = d3d;
  }
  allocslab::g_unlock(m + allocslab::kMgrMutex);
  if (g3p) g_c.g3Windows++;
  g_c.g3Pages += g3p;
  g_c.g3Bytes += g3b;
  g_c.mapPages += map;
  g_c.bufPages += bufs;
}

void OnCollect() {
  if (!g_counting.load()) return;
  if (g_phase.load() != 0) {
    g_c.collectInWin++;
    return;
  }
  if (!g_parse.load()) {
    g_c.collectOutside++;
    return;
  }
  g_renderTid = GetCurrentThreadId();
  g_winId.fetch_add(1);
  g_c.windows++;
  if (void* mgr = allocslab::g_mgr.load())
    ScanAtOpen(mgr);
  else
    g_c.noMgr++;
  if (d3ds::g_vtblCount.load() > 0) g_d3dOpen = d3ds::MyCounters()->draws;
  g_phase.store(1, std::memory_order_release);
}

void Close() {
  if (d3ds::g_vtblCount.load() > 0) g_c.d3dWin += d3ds::MyCounters()->draws - g_d3dOpen;
  g_c.closed++;
}

// Slot 8: the per-frame upload of every page with used > 0 (render thread).
void __fastcall HookUpload(void* mgr) {
  if (!g_counting.load()) {
    g_origUpload(mgr);
    g_phase = 0;
    g_parse = false;
    return;
  }
  const bool inWin = g_phase.load() == 1;
  if (!inWin) g_c.uploadNoWin++;
  g_phase.store(2, std::memory_order_release);
  // The pages as the original sees them: it uploads [0, used) of each page
  // with used > 0 [V 0xbde0, 0xbeb5]. The workers are joined; no lock taken
  // here, as in the original.
  uint8_t* m = static_cast<uint8_t*>(mgr);
  uint8_t* b = *reinterpret_cast<uint8_t**>(m + 8);
  uint8_t* e = *reinterpret_cast<uint8_t**>(m + 0x10);
  uint64_t bytes = 0, pages = 0, noBuf = 0;
  int snap = 0, total = 0;
  for (uint8_t* p = b; p && p < e; p += allocslab::kPageStride) {
    const auto* pg = reinterpret_cast<const allocslab::Page*>(p);
    ++total;
    if (snap < kMaxSnap) g_snap[snap++] = {pg->idx, pg->elemSize, pg->used, pg->capBytes, pg->gpuBuf != nullptr};
    if (!pg->used) continue;
    ++pages;
    bytes += pg->used;
    if (!pg->gpuBuf) ++noBuf;
    int c = 0;
    while (c < kClasses && g_cls[c].elemSize && g_cls[c].elemSize != pg->elemSize) ++c;
    if (c == kClasses) continue;
    ClassAcc& a = g_cls[c];
    a.elemSize = pg->elemSize;
    a.bytes += pg->used;
    a.pages++;
    if (pg->used > a.maxPage) a.maxPage = pg->used;
    if (pg->capBytes > a.capMax) a.capMax = pg->capBytes;
  }
  g_nSnap = snap;
  g_snapTotal = total;
  const uint64_t t0 = __rdtsc();
  g_origUpload(mgr);
  const uint64_t dt = __rdtsc() - t0;
  g_c.uploadCalls++;
  g_c.uploadBytes += bytes;
  g_c.uploadPages += pages;
  g_c.uploadNoBuf += noBuf;
  g_c.uploadCycles += dt;
  if (dt > g_c.uploadMaxCycles.load()) g_c.uploadMaxCycles = dt;
  if (inWin) Close();
  g_phase = 0;
  g_parse = false;
}

// ---- G1 ----
bool IsPage(ID3D11Resource* r) {
  for (int i = 0; i < g_nBufs; ++i)
    if (g_bufs[i] == r) return true;
  return false;
}

// True if a model data page is bound as a shader resource of a stage the
// call uses (state after the call: DX11Renderer binds before it draws).
bool PageBound(ID3D11DeviceContext* ctx, bool compute) {
  constexpr UINT kN = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
  ID3D11ShaderResourceView* v[kN];
  bool hit = false;
  for (int s = compute ? 5 : 0; s < (compute ? 6 : 5); ++s) {
    memset(v, 0, sizeof(v));
    switch (s) {
      case 0: ctx->VSGetShaderResources(0, kN, v); break;
      case 1: ctx->HSGetShaderResources(0, kN, v); break;
      case 2: ctx->DSGetShaderResources(0, kN, v); break;
      case 3: ctx->GSGetShaderResources(0, kN, v); break;
      case 4: ctx->PSGetShaderResources(0, kN, v); break;
      default: ctx->CSGetShaderResources(0, kN, v); break;
    }
    for (UINT i = 0; i < kN; ++i) {
      if (!v[i]) continue;
      ID3D11Resource* r = nullptr;
      v[i]->GetResource(&r);
      if (r) {
        if (IsPage(r)) hit = true;
        r->Release();
      }
      v[i]->Release();
    }
  }
  return hit;
}

void OnDraw(int kind, void* self) {
  if (!g_counting.load(std::memory_order_relaxed)) return;
  g_c.draws[kind].fetch_add(1, std::memory_order_relaxed);
  if (g_phase.load(std::memory_order_acquire) == 0) return;
  const bool render = GetCurrentThreadId() == g_renderTid.load();
  if (render)
    g_c.winDraws[kind]++;
  else
    g_c.winOther++;
  void* ctx = nullptr;
  if (!render || !g_ctx || !ReadPtr(static_cast<uint8_t*>(self) + kRendererCtx, &ctx) || ctx != g_ctx) {
    g_c.winCtxUnknown++;
    return;
  }
  if (PageBound(static_cast<ID3D11DeviceContext*>(ctx), kind >= kCompute)) g_c.winHazard++;
}

template <int K>
void __fastcall Wrap(void* self, uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f,
                     uint64_t g) {
  g_fwd[K](self, a, b, c, d, e, f, g);
  OnDraw(K % kKinds, self);
}
void* const kWraps[2 * kKinds] = {&Wrap<0>, &Wrap<1>, &Wrap<2>, &Wrap<3>, &Wrap<4>,  &Wrap<5>,
                                  &Wrap<6>, &Wrap<7>, &Wrap<8>, &Wrap<9>, &Wrap<10>, &Wrap<11>};

// ---- install / remove (suite thread) ----
void ResetCounters() {
  g_c.~Counters();
  new (&g_c) Counters;
  for (ClassAcc& a : g_cls) a = ClassAcc{};
  g_nSnap = g_snapTotal = 0;
  for (Row& r : g_rows) {
    r.inWin = r.pre = r.upload = r.post = r.windows = 0;
  }
}

// Wraps the renderer's draw entries in the real vtable and in the batching
// copy. Returns the number of real slots wrapped.
int HookDraws(uint8_t* dx, char* note, size_t noteLen) {
  HMODULE mod = reinterpret_cast<HMODULE>(dx);
  auto** vtbl = reinterpret_cast<void**>(GetProcAddress(mod, "??_7DX11Renderer@RenderAPI@@6B@"));
  if (!vtbl) {
    snprintf(note, noteLen, "DX11Renderer vtable export not found");
    return 0;
  }
  int hooked = 0;
  std::string skipped;
  for (int k = 0; k < kKinds; ++k) {
    void* fn = reinterpret_cast<void*>(GetProcAddress(mod, kExports[k]));
    int slot = -1;
    for (int i = 0; fn && i < 128 && slot < 0; ++i)
      if (SlotOriginal(&vtbl[i]) == fn) slot = i;
    if (slot < 0) {
      skipped += std::string(" ") + kKindNames[k] + "(not found)";
      continue;
    }
    if (vtbl[slot] != fn) {  // another hook owns the slot (e.g. the d3d meter's one-shot capture)
      skipped += std::string(" ") + kKindNames[k] + "(slot " + std::to_string(slot) + " hooked elsewhere)";
      continue;
    }
    g_fwd[k] = reinterpret_cast<Fwd>(fn);
    if (!HookSlot(&vtbl[slot], kWraps[k], nullptr)) {
      skipped += std::string(" ") + kKindNames[k] + "(patch failed)";
      continue;
    }
    g_slot[k] = &vtbl[slot];
    ++hooked;
    // The batching copy (same layout): wrap whatever entry it holds.
    if (shadowbatch::g_rendererState == 1 && shadowbatch::g_myVtbl && slot < shadowbatch::kVtblCopy) {
      void** e = &shadowbatch::g_myVtbl[slot];
      g_fwd[kKinds + k] = reinterpret_cast<Fwd>(*e);
      InterlockedExchangePointer(e, kWraps[kKinds + k]);
      g_copySlot[k] = e;
    }
  }
  snprintf(note, noteLen, "%s", skipped.empty() ? "" : skipped.c_str());
  return hooked;
}

void UnhookDraws() {
  for (int k = 0; k < kKinds; ++k) {
    if (g_slot[k]) UnhookSlot(g_slot[k], reinterpret_cast<void*>(g_fwd[k]));
    g_slot[k] = nullptr;
    if (g_copySlot[k])
      InterlockedCompareExchangePointer(g_copySlot[k], reinterpret_cast<void*>(g_fwd[kKinds + k]),
                                        kWraps[kKinds + k]);
    g_copySlot[k] = nullptr;
  }
}

std::string ThreadName(DWORD tid) {
  using GetDesc = HRESULT(WINAPI*)(HANDLE, PWSTR*);
  static auto fn = reinterpret_cast<GetDesc>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
  std::string out;
  if (!fn) return out;
  HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
  if (!h) return out;
  PWSTR w = nullptr;
  if (SUCCEEDED(fn(h, &w)) && w) {
    char buf[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
    out = buf;
    LocalFree(w);
  }
  CloseHandle(h);
  return out;
}

void Report(double frames, double tscHz, int drawSlots, bool copyWrapped, const char* note);

void Measure(int ms, std::atomic<uint64_t>& frames, double tscHz, std::atomic<bool>& abort) {
  auto* ng = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"NGModel.dll"));
  auto* dx = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"dx11backend.dll"));
  if (!ng || !dx || !tscHz) {
    Log("  direct upload counters: NGModel.dll or dx11backend.dll not loaded");
    return;
  }
  // The allocate detour is written at a collect call; wait for it.
  for (int i = 0; i < 20 && allocslab::g_state.load() != 2; ++i) Sleep(50);
  if (allocslab::g_state.load() != 2 || !allocslab::g_lock) {
    Log("  direct upload counters: model allocator detour not active (state %d); skipped", allocslab::g_state.load());
    return;
  }
  auto** mvt = reinterpret_cast<void**>(ng + allocslab::kVtableRva);
  void** uslot = &mvt[kUploadSlot];
  if (SlotOriginal(uslot) != ng + kUploadRva || *uslot != ng + kUploadRva) {
    Log("  direct upload counters: StructBufferManager slot 8 is not NGModel+0x%x or is hooked; skipped", kUploadRva);
    return;
  }
  auto** sbvt = reinterpret_cast<void**>(dx + parupload::kVtableRva);
  if (!allocslab::RttiIs(dx, sbvt, ".?AVDX11StructuredBuffer@RenderAPI@@")) {
    Log("  direct upload counters: DX11StructuredBuffer vtable does not match this build; skipped");
    return;
  }
  g_sbVtbl = sbvt;
  g_ctx = nullptr;
  g_nBufs = 0;
  ResetCounters();

  char note[512] = {};
  const int drawSlots = HookDraws(dx, note, sizeof(note));
  bool copyWrapped = false;
  for (void** s : g_copySlot) copyWrapped |= s != nullptr;
  g_origUpload = reinterpret_cast<UploadFn>(ng + kUploadRva);
  g_uploadSlot = uslot;
  if (!HookSlot(uslot, reinterpret_cast<void*>(&HookUpload), nullptr)) {
    Log("  direct upload counters: could not patch StructBufferManager slot 8");
    UnhookDraws();
    return;
  }
  allocslab::g_allocObs = &OnAlloc;
  allocslab::g_collectObs = &OnCollect;
  allocslab::g_resetObs = &OnReset;

  // Count whole parses: on at a reset, off at a later reset.
  g_stopReq = false;
  g_armed = true;
  for (int i = 0; i < 100 && !g_counting.load() && !abort.load(); ++i) Sleep(10);
  const uint64_t f0 = frames.load();
  for (int t = 0; t < ms && !abort.load(); t += 50) Sleep(50);
  g_stopReq = true;
  for (int i = 0; i < 100 && g_counting.load(); ++i) Sleep(10);
  const uint64_t f1 = frames.load();
  const bool stillCounting = g_counting.exchange(false);
  g_armed = false;

  allocslab::g_allocObs = nullptr;
  allocslab::g_collectObs = nullptr;
  allocslab::g_resetObs = nullptr;
  UnhookSlot(uslot, reinterpret_cast<void*>(g_origUpload));
  g_uploadSlot = nullptr;
  UnhookDraws();
  Sleep(50);  // calls already inside a wrapper finish
  if (stillCounting) Log("  direct upload counters: no page reset seen at the stop; the last parse is partial");
  Report(static_cast<double>(f1 - f0), tscHz, drawSlots, copyWrapped, note);
}

void Report(double frames, double tscHz, int drawSlots, bool copyWrapped, const char* note) {
  const Counters& c = g_c;
  const double parses = static_cast<double>(c.parses.load());
  if (parses <= 0) {
    Log("  direct upload counters: no page reset counted (is a mission running?)");
    return;
  }
  auto u = [](const std::atomic<uint64_t>& a) { return static_cast<unsigned long long>(a.load()); };
  Log("  direct upload: %.0f parses, %.0f frames; culling windows %llu, closed by the slot-8 upload %llu, left open "
      "%llu; collect calls inside a window %llu, outside a parse %llu; render thread %lu",
      parses, frames, u(c.windows), u(c.closed), u(c.unclosed), u(c.collectInWin), u(c.collectOutside),
      static_cast<unsigned long>(g_renderTid.load()));
  uint64_t all = 0, win = 0;
  std::string perKind;
  for (int k = 0; k < kKinds; ++k) {
    all += c.draws[k].load();
    win += c.winDraws[k].load();
    char b[96];
    snprintf(b, sizeof(b), "%s%s %llu/%.0f", k ? ", " : "", kKindNames[k], u(c.winDraws[k]), c.draws[k].load() / parses);
    perKind += b;
  }
  Log("  hooks: %d DX11Renderer draw entries wrapped%s%s, batching vtable copy %s; renderer draws seen %.0f/parse",
      drawSlots, note[0] ? ", not wrapped:" : "", note, copyWrapped ? "wrapped too" : "not in use", all / parses);
  Log("  draws inside the window / all per parse: %s", perKind.c_str());
  const bool d3d = d3ds::g_vtblCount.load() > 0;
  char d3dPart[96];
  if (d3d)
    snprintf(d3dPart, sizeof(d3dPart), "%llu (d3d meter, render thread)", u(c.d3dWin));
  else
    snprintf(d3dPart, sizeof(d3dPart), "not measured ([D3D] Meter=1)");
  const uint64_t g1 = win + c.winOther.load();
  const bool g1Valid = drawSlots == kKinds && c.closed.load() > 0;
  Log("  G1: draws/dispatches inside the window %llu (render thread %llu, other threads %llu); with a model data page "
      "bound %llu, bindings not checked %llu; d3d11 draws inside the window %s -> %s",
      static_cast<unsigned long long>(g1), static_cast<unsigned long long>(win), u(c.winOther), u(c.winHazard),
      u(c.winCtxUnknown), d3dPart,
      !g1Valid ? "INCOMPLETE" : (g1 == 0 && (!d3d || c.d3dWin.load() == 0)) ? "PASS (0)" : "FAIL");

  // G2: per thread.
  const int n = std::min(g_nRows.load(), kRows);
  const uint64_t windows = c.windows.load();
  const DWORD rt = g_renderTid.load();
  uint64_t pre = 0, upl = 0, post = 0, rogue = 0, inWin = 0;
  for (int i = 0; i <= n; ++i) {
    const Row& r = i < n ? g_rows[i] : g_rows[kRows];
    pre += r.pre;
    upl += r.upload;
    post += r.post;
    inWin += r.inWin;
    const bool worker = i < n && (r.tid == rt || (windows && r.windows.load() * 10 >= windows));
    if (!worker) rogue += r.inWin;
  }
  const uint64_t g2 = pre + upl + post + rogue;
  Log("  G2: allocate calls %.0f/parse inside the window; outside it: before culling %llu, during the upload %llu, "
      "after the upload %llu; inside it from threads that are not render/culling threads %llu -> %s",
      inWin / parses, static_cast<unsigned long long>(pre), static_cast<unsigned long long>(upl),
      static_cast<unsigned long long>(post), static_cast<unsigned long long>(rogue),
      windows == 0 ? "INCOMPLETE" : g2 == 0 ? "PASS (0)" : "FAIL");
  for (int i = 0; i <= n; ++i) {
    const Row& r = i < n ? g_rows[i] : g_rows[kRows];
    const uint64_t tot = r.inWin + r.pre + r.upload + r.post;
    if (!tot) continue;
    const DWORD tid = i < n ? r.tid.load() : 0;
    const std::string name = tid ? ThreadName(tid) : std::string("(more threads)");
    Log("    thread %5lu %-24s %s: inside %llu (in %llu of %llu windows), before culling %llu, upload %llu, after %llu",
        static_cast<unsigned long>(tid), name.c_str(),
        tid == rt ? "render" : (windows && r.windows.load() * 10 >= windows ? "culling" : "OTHER"),
        static_cast<unsigned long long>(r.inWin.load()), static_cast<unsigned long long>(r.windows.load()),
        static_cast<unsigned long long>(windows), static_cast<unsigned long long>(r.pre.load()),
        static_cast<unsigned long long>(r.upload.load()), static_cast<unsigned long long>(r.post.load()));
  }
  const double w = windows ? static_cast<double>(windows) : 1.0;
  Log("  G3: windows opening with pages already used %llu of %llu (%.1f pages, %.1f KB per window); pages with a GPU "
      "buffer %.1f, of which direct upload would map (capacity and previous use) %.1f per window; no manager yet %llu",
      u(c.g3Windows), static_cast<unsigned long long>(windows), c.g3Pages / w, c.g3Bytes / w / 1024.0,
      c.bufPages / w, c.mapPages / w, u(c.noMgr));

  // Upload.
  const double calls = static_cast<double>(c.uploadCalls.load());
  if (calls > 0) {
    Log("  upload (slot 8): %.2f calls/parse (%llu outside a window); %.2f MB and %.1f pages per call (%.1f without a "
        "GPU buffer yet); time %.3f ms per call (max %.3f ms), %.2f GB/s",
        calls / parses, u(c.uploadNoWin), c.uploadBytes / calls / 1048576.0, c.uploadPages / calls,
        c.uploadNoBuf / calls, c.uploadCycles / calls / tscHz * 1000.0, c.uploadMaxCycles / tscHz * 1000.0,
        c.uploadCycles ? c.uploadBytes / (c.uploadCycles / tscHz) / 1e9 : 0.0);
    for (const ClassAcc& a : g_cls) {
      if (!a.elemSize) continue;
      Log("    element size %3u: %.2f MB in %.1f pages per call, mean %.0f KB per used page, largest page %u bytes of "
          "%u capacity",
          a.elemSize, a.bytes / calls / 1048576.0, a.pages / calls, a.pages ? a.bytes / 1024.0 / a.pages : 0.0,
          a.maxPage, a.capMax);
    }
    // Last upload, per page (largest first).
    std::vector<PageSnap> s(g_snap, g_snap + g_nSnap);
    std::sort(s.begin(), s.end(), [](const PageSnap& x, const PageSnap& y) { return x.used > y.used; });
    int used = 0;
    for (const PageSnap& p : s) used += p.used ? 1 : 0;
    Log("    last upload: %d pages exist, %d used; largest:", g_snapTotal, used);
    for (size_t i = 0; i < s.size() && i < 12 && s[i].used; ++i)
      Log("      page %4u: element %3u, used %8u of %8u bytes (%.1f%%)%s", s[i].idx, s[i].elemSize, s[i].used,
          s[i].cap, s[i].cap ? 100.0 * s[i].used / s[i].cap : 0.0, s[i].buf ? "" : ", no GPU buffer yet");
  } else {
    Log("  upload (slot 8): not called during the phase (the callback may not go through the vtable)");
  }
}

}  // namespace ducount
