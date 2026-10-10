// setShaderResources identical-tail trim (R15 F2). [Model] SrvTailTrim, default 0.
//
// dx11backend's DX11Renderer::setShaderResources (0x1ac20, srv_span_count.h)
// [V 0x1ac20-0x1ad0a, dx11backend.dll of 2026-09-19]:
//   old = this[0xf8 + 4*stage]; cache = this + 0x118 + 0x400*stage (128 slots)
//   first = length of the identical prefix of views[0, count) against cache
//   cache[0, count) = views; if old > count: cache[count, old) = 0 (memset)
//   this[0xf8 + 4*stage] = count
//   end = max(count, old); if end > first: setter(this[0x30], first, end - first, cache + first)
// The start argument (r9) is ignored. The setter is the FX call-table member
// pointer (vcall thunk `mov rax,[rcx]; jmp [rax+off]`, split_filter.h).
//
// Trim: the cache update is stock's (only slots that differ are written, so
// the memory afterwards is identical), but the call binds [first, lastDiff],
// or through old-1 when the count shrank (the nulled slots are bound exactly
// as stock binds them). The slots after lastDiff hold what the cache says is
// bound already. d3d11's binders have no per-slot equality skip (R15 F2), so
// leaving them out saves their bind tracking and the UMD's per-slot work.
// Exact when the device agrees with the cache in those slots, which is the
// assumption stock itself makes for the identical prefix. They could differ if
// d3d11 unbound a view on its own (its resource bound as an output) and DCS
// did not reset the cache: stock rebinds tail slots, the trim does not.
// [Suite] SrvTailTrimVerify measures exactly that.
//
// Both callers (ApplyShaderBlock 0x68546, 0x688b8, srvspan::kSites) are
// redirected: call rel32 -> near stub -> Trim. The function and its export
// stay untouched, so the cache that resetSlots / resetSlotsSRV /
// getShaderResourcesCount read is what stock writes, and switching off mid
// frame leaves nothing to repair. The setter is called as stock calls it,
// through the same thunk and the context's own table, so every table hook sees
// the same entry (split_filter.h hooks no SRV setter, and its code hashes
// exclude both call sites). Only the return address differs (our module), and
// no hook keys SRV calls on it. The recorders snapshot SRVs from the device
// (Get*), never from this cache, and replay on deferred contexts executed with
// ExecuteCommandList(TRUE), which restores the immediate state.
//
// [Suite] SrvTailTrimVerify (SrvTailTrimVerifySec, default 10 s): the sites go
// to TrimVerify. For every call whose range was trimmed it reads back the
// stock range [first, max(count, old)) with the stage's Get*ShaderResources,
// then performs the stock call over that range and reads back again: the two
// must be equal (the second readback is what stock leaves bound; the views
// are released). A mismatch latches the trim off for the session and logs a
// sample; the stock call already put the stock state in place. Slots where
// stock's own result differs from the cache (d3d11 refused or unbound a view)
// are counted for information.
// [Suite] BenchSrvTailTrim: bench mode 31, OFF = call sites restored (stock),
// ON = trim, no counting.
// Off (key, kill switch, bench OFF, payload stop) restores both call sites.
// Fail-safe: export, function hash or call-site mismatch turns it off with a
// log line; the sites are checked again before every patch.
// Included once from main.cpp inside its anonymous namespace, after
// srv_span_count.h (constants), pacer.h (AllocNear), pose_sweep.h
// (PatchAllSuspended), shadow_tex.h (CodeIs) and alloc_slab.h (ReadBytes).
#pragma once

namespace srvtrim {

using srvspan::kCountOff;
using srvspan::kMaxSlots;
using srvspan::kShadowOff;
using srvspan::kStageStride;
using srvspan::kStages;
constexpr uint32_t kCtxOff = 0x30;  // DX11Renderer: immediate context
using Fn = srvspan::Fn;
using Setter = void(__fastcall*)(void* ctx, UINT start, UINT n, void* const* views);

// ---------------------------------------------------------------------------
// The cache update (0x1ac20) and the trimmed range
// ---------------------------------------------------------------------------

struct Range {
  uint32_t first;     // stock's identical prefix
  uint32_t end;       // trimmed: one past the last slot that must be bound
  uint32_t stockEnd;  // stock: max(count, old)
};

// Updates the cache exactly as 0x1ac20 does; returns both ranges. Stock binds
// [first, stockEnd) when stockEnd > first, the trim [first, end) when end > first.
inline Range Update(void** cache, uint32_t* countSlot, uint32_t count, void* const* views) {
  const uint32_t old = *countSlot;
  uint32_t i = 0;
  while (i < count && cache[i] == views[i]) ++i;
  const uint32_t first = i;
  uint32_t end = first;
  for (; i < count; ++i) {
    void* const v = views[i];
    if (cache[i] != v) {
      cache[i] = v;
      end = i + 1;
    }
  }
  if (old > count) {
    memset(cache + count, 0, (old - count) * sizeof(void*));
    end = old;  // the nulled slots are bound as stock binds them
  }
  *countSlot = count;
  return {first, end, count > old ? count : old};
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

Fn g_fn = nullptr;                    // the original function (never patched)
uint8_t* g_base = nullptr;            // dx11backend (tests: a fake module)
uint32_t g_sites[2] = {};             // RVAs of the two call sites
uint32_t g_fnRva = 0;
uint8_t* g_stub = nullptr;            // near page: +0 -> Trim, +16 -> TrimVerify
uint8_t g_saved[2][5] = {};           // the original call bytes
std::atomic<int> g_state{0};          // 0 not installed, 1 checked, -1 failed
std::atomic<int> g_patched{0};        // 0 stock, 1 trim, 2 verify
std::atomic<bool> g_disabled{false};  // latched off for the session (verify mismatch)

// Verify counters (TrimVerify only; the production path counts nothing).
struct Counters {
  uint64_t calls, guarded, bound, trimmed, slotsStock, slotsBound, verified, mismatches, mismatchSlots;
  uint64_t stockVsCache, unverified;
};
Counters g_c;
SRWLOCK g_lock = SRWLOCK_INIT;
struct Sample {
  int stage, kind;
  uint32_t count, first, end, stockEnd, slot;
  void *trimmed, *stock, *cache;
};
constexpr int kMaxSamples = 8;
Sample g_samples[kMaxSamples];
int g_sampleCount = 0;

void ResetCounters() {
  AcquireSRWLockExclusive(&g_lock);
  memset(&g_c, 0, sizeof(g_c));
  g_sampleCount = 0;
  ReleaseSRWLockExclusive(&g_lock);
}

// ---------------------------------------------------------------------------
// Hot path
// ---------------------------------------------------------------------------

void __fastcall Trim(uint8_t* self, int stage, void* setter, uint32_t start, uint32_t count, void* const* views) {
  uint32_t* const countSlot = reinterpret_cast<uint32_t*>(self + kCountOff) + stage;
  if (static_cast<uint32_t>(stage) >= static_cast<uint32_t>(kStages) || count > kMaxSlots || *countSlot > kMaxSlots)
    return g_fn(self, stage, setter, start, count, views);  // outside the analysed shape: stock
  void** const cache = reinterpret_cast<void**>(self + kShadowOff + static_cast<size_t>(stage) * kStageStride);
  const Range r = Update(cache, countSlot, count, views);
  if (r.end > r.first)
    reinterpret_cast<Setter>(setter)(*reinterpret_cast<void**>(self + kCtxOff), r.first, r.end - r.first,
                                     cache + r.first);
}

// ---------------------------------------------------------------------------
// Verify
// ---------------------------------------------------------------------------

// Stage of a setter thunk: 0 VS, 1 PS, 2 GS, 3 HS, 4 DS, 5 CS; -1 unknown.
constexpr int kKinds = 6;
const char* const kKindNames[kKinds] = {"VS", "PS", "GS", "HS", "DS", "CS"};
const char* const kSetNames[kKinds] = {"VSSetShaderResources", "PSSetShaderResources", "GSSetShaderResources",
                                       "HSSetShaderResources", "DSSetShaderResources", "CSSetShaderResources"};
int ThunkKind(const void* setter) {
  uint8_t b[9] = {};
  if (!setter || !allocslab::ReadBytes(setter, b, sizeof(b)) || b[0] != 0x48 || b[1] != 0x8B || b[2] != 0x01 ||
      b[3] != 0xFF)
    return -1;
  int off;
  if (b[4] == 0x60) {
    off = b[5];
  } else if (b[4] == 0xA0) {
    int32_t d;
    memcpy(&d, b + 5, 4);
    off = d;
  } else {
    return -1;
  }
  for (int k = 0; k < kKinds; ++k)
    if (DcsQv_ContextSlot(kSetNames[k]) * 8 == off) return k;
  return -1;
}

int KindOf(void* setter) {
  struct Entry {
    void* setter;
    int kind;
  };
  static Entry cache[8];
  static int n = 0;
  static SRWLOCK lock = SRWLOCK_INIT;
  AcquireSRWLockExclusive(&lock);
  int kind = -2;
  for (int i = 0; i < n && kind == -2; ++i)
    if (cache[i].setter == setter) kind = cache[i].kind;
  if (kind == -2) {
    kind = ThunkKind(setter);
    if (n < 8) cache[n++] = {setter, kind};
  }
  ReleaseSRWLockExclusive(&lock);
  return kind;
}

void Get(int kind, ID3D11DeviceContext* c, UINT first, UINT n, ID3D11ShaderResourceView** out) {
  switch (kind) {
    case 0: c->VSGetShaderResources(first, n, out); break;
    case 1: c->PSGetShaderResources(first, n, out); break;
    case 2: c->GSGetShaderResources(first, n, out); break;
    case 3: c->HSGetShaderResources(first, n, out); break;
    case 4: c->DSGetShaderResources(first, n, out); break;
    default: c->CSGetShaderResources(first, n, out); break;
  }
  for (UINT i = 0; i < n; ++i)
    if (out[i]) out[i]->Release();  // only the pointer values are compared
}

void __fastcall TrimVerify(uint8_t* self, int stage, void* setter, uint32_t start, uint32_t count,
                           void* const* views) {
  uint32_t* const countSlot = reinterpret_cast<uint32_t*>(self + kCountOff) + stage;
  if (g_disabled.load(std::memory_order_relaxed) || static_cast<uint32_t>(stage) >= static_cast<uint32_t>(kStages) ||
      count > kMaxSlots || *countSlot > kMaxSlots) {
    if (!g_disabled.load(std::memory_order_relaxed)) {
      AcquireSRWLockExclusive(&g_lock);
      g_c.calls++;
      g_c.guarded++;
      ReleaseSRWLockExclusive(&g_lock);
    }
    return g_fn(self, stage, setter, start, count, views);
  }
  void** const cache = reinterpret_cast<void**>(self + kShadowOff + static_cast<size_t>(stage) * kStageStride);
  auto* const ctx = *reinterpret_cast<ID3D11DeviceContext**>(self + kCtxOff);
  const Range r = Update(cache, countSlot, count, views);
  const auto set = reinterpret_cast<Setter>(setter);
  if (r.end > r.first) set(ctx, r.first, r.end - r.first, cache + r.first);
  const bool trimmed = r.stockEnd > r.end;
  int kind = -1;
  uint32_t bad = 0, differ = 0, firstBad = 0;
  ID3D11ShaderResourceView* a[kMaxSlots];
  ID3D11ShaderResourceView* b[kMaxSlots];
  if (trimmed) {
    kind = KindOf(setter);
    const uint32_t n = r.stockEnd - r.first;
    if (kind >= 0) Get(kind, ctx, r.first, n, a);
    set(ctx, r.first, n, cache + r.first);  // stock's call: the stock state from here on
    if (kind >= 0) {
      Get(kind, ctx, r.first, n, b);
      for (uint32_t i = 0; i < n; ++i) {
        if (a[i] != b[i] && !bad++) firstBad = i;
        if (b[i] != cache[r.first + i]) ++differ;
      }
    }
  }
  AcquireSRWLockExclusive(&g_lock);
  Counters& c = g_c;
  c.calls++;
  if (r.stockEnd > r.first) c.slotsStock += r.stockEnd - r.first;
  if (r.end > r.first) {
    c.bound++;
    c.slotsBound += r.end - r.first;
  }
  if (trimmed) {
    c.trimmed++;
    if (kind < 0) {
      c.unverified++;
    } else {
      c.verified++;
      c.stockVsCache += differ;
      if (bad) {
        c.mismatches++;
        c.mismatchSlots += bad;
        if (g_sampleCount < kMaxSamples) {
          const uint32_t s = r.first + firstBad;
          g_samples[g_sampleCount++] = {stage, kind, count, r.first, r.end, r.stockEnd, s, a[firstBad], b[firstBad],
                                        cache[s]};
        }
      }
    }
  }
  ReleaseSRWLockExclusive(&g_lock);
  if (bad) g_disabled = true;  // the main loop restores the sites
}

void LogCounters(const char* what, double frames) {
  AcquireSRWLockExclusive(&g_lock);
  const Counters c = g_c;
  Sample s[kMaxSamples];
  const int ns = g_sampleCount;
  memcpy(s, g_samples, sizeof(s));
  ReleaseSRWLockExclusive(&g_lock);
  const double f = frames > 0 ? frames : 1;
  Log("  srv tail trim (%s): %.0f frames; %.0f calls/frame (%.0f outside the analysed shape, stock), %.0f bind "
      "calls/frame; stock binds %.0f slots/frame, trim %.0f (%.1f%% fewer); %.0f trimmed calls/frame, %.0f verified "
      "(%llu unverified: setter not a known thunk)",
      what, f, c.calls / f, c.guarded / f, c.bound / f, c.slotsStock / f, c.slotsBound / f,
      c.slotsStock ? 100.0 * (c.slotsStock - c.slotsBound) / c.slotsStock : 0.0, c.trimmed / f, c.verified / f,
      static_cast<unsigned long long>(c.unverified));
  Log("  srv tail trim (%s): mismatches %llu calls (%llu slots); slots where stock's own result differs from the "
      "cache %.1f/frame (d3d11 refused or unbound a view; stock and trim alike)",
      what, static_cast<unsigned long long>(c.mismatches), static_cast<unsigned long long>(c.mismatchSlots),
      c.stockVsCache / f);
  for (int i = 0; i < ns; ++i)
    Log("    mismatch: stage %d (%s) count %u, first %u, trimmed end %u, stock end %u: slot %u trim %p, stock %p, "
        "cache %p",
        s[i].stage, kKindNames[s[i].kind], s[i].count, s[i].first, s[i].end, s[i].stockEnd, s[i].slot, s[i].trimmed,
        s[i].stock, s[i].cache);
}

// ---------------------------------------------------------------------------
// Checks and patching
// ---------------------------------------------------------------------------

// The site calls the function (rel32) or our stub.
bool SiteCalls(uint8_t* site, const uint8_t* target) {
  uint8_t b[5];
  int32_t rel;
  if (!allocslab::ReadBytes(site, b, 5) || b[0] != 0xE8) return false;
  memcpy(&rel, b + 1, 4);
  return site + 5 + rel == target;
}

const char* CheckSites(uint8_t* base) {
  for (uint32_t s : g_sites)
    if (!SiteCalls(base + s, base + g_fnRva))
      return "srv tail trim: an ApplyShaderBlock call site does not call setShaderResources (patched by someone "
             "else?); off";
  return nullptr;
}

const char* CheckDcs(uint8_t* dx) {
  if (reinterpret_cast<uint8_t*>(GetProcAddress(reinterpret_cast<HMODULE>(dx), srvspan::kExport)) != dx + srvspan::kFn)
    return "srv tail trim: setShaderResources export is not at dx11backend+0x1ac20; off";
  if (!shadowtex::CodeIs(dx, srvspan::kFn, srvspan::kFnEnd, srvspan::kFnHash))
    return "srv tail trim: setShaderResources differs from the analysed build; off";
  return nullptr;
}

// mode 0 = original bytes, 1 = Trim stub, 2 = TrimVerify stub.
bool WriteSites(int mode) {
  for (int i = 0; i < 2; ++i) {
    uint8_t* site = g_base + g_sites[i];
    uint8_t b[5];
    if (mode == 0) {
      memcpy(b, g_saved[i], 5);
    } else {
      b[0] = 0xE8;
      const int32_t rel = static_cast<int32_t>(g_stub + (mode == 2 ? 16 : 0) - (site + 5));
      memcpy(b + 1, &rel, 4);
    }
    uint8_t cur[5];
    if (allocslab::ReadBytes(site, cur, 5) && memcmp(cur, b, 5) == 0) continue;
    if (!posesweep::PatchAllSuspended(site, b, 5)) return false;
  }
  return true;
}

// Production passes dx11backend; tests a fake module (dcs = false: no export
// or function hash check).
bool InstallAt(uint8_t* base, const uint32_t (&sites)[2], uint32_t fnRva, bool dcs) {
  const int st = g_state.load();
  if (st != 0) return st > 0;
  g_sites[0] = sites[0];
  g_sites[1] = sites[1];
  g_fnRva = fnRva;
  const char* why = dcs ? CheckDcs(base) : nullptr;
  if (!why) why = CheckSites(base);
  if (why) {
    Log("%s", why);
    g_state = -1;
    return false;
  }
  uint8_t* page = pacer::AllocNear(base);
  if (!page) {
    Log("srv tail trim: no memory within reach of dx11backend.dll; off");
    g_state = -1;
    return false;
  }
  memset(page, 0xCC, 4096);
  void* const fns[2] = {reinterpret_cast<void*>(&Trim), reinterpret_cast<void*>(&TrimVerify)};
  for (int k = 0; k < 2; ++k) {
    uint8_t* p = page + 16 * k;
    p[0] = 0x48;
    p[1] = 0xB8;  // mov rax, imm64
    memcpy(p + 2, &fns[k], 8);
    p[10] = 0xFF;
    p[11] = 0xE0;  // jmp rax
  }
  DWORD old;
  VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
  FlushInstructionCache(GetCurrentProcess(), page, 4096);
  for (int i = 0; i < 2; ++i) memcpy(g_saved[i], base + g_sites[i], 5);
  g_stub = page;
  g_base = base;
  g_fn = reinterpret_cast<Fn>(base + fnRva);
  g_state = 1;
  Log("srv tail trim: setShaderResources and its 2 call sites checked");
  return true;
}

bool Install() {
  if (g_state.load() != 0) return g_state.load() > 0;
  auto* dx = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"dx11backend.dll"));
  if (!dx) return false;  // retried
  return InstallAt(dx, srvspan::kSites, srvspan::kFn, true);
}

// verify = route the sites to TrimVerify. Each site must call the function
// or one of our stubs (anything else: off, logged once).
bool Attach(bool verify) {
  if (g_state.load() <= 0 || g_disabled.load()) return false;
  const int want = verify ? 2 : 1;
  if (g_patched.load() == want) return true;
  for (uint32_t s : g_sites) {
    uint8_t* site = g_base + s;
    if (!SiteCalls(site, g_base + g_fnRva) && !SiteCalls(site, g_stub) && !SiteCalls(site, g_stub + 16)) {
      Log("srv tail trim: a call site changed since the install (another patch?); off");
      g_state = -1;
      return false;
    }
  }
  if (!WriteSites(want)) {
    WriteSites(0);
    g_patched = 0;
    Log("srv tail trim: could not patch the call sites; off for now");
    return false;
  }
  g_patched = want;
  return true;
}

void Detach() {
  if (g_state.load() == 0 || !g_base) return;
  if (g_patched.load() == 0) return;
  if (!WriteSites(0))
    Log("srv tail trim: WARNING could not restore a call site");
  else
    g_patched = 0;
}

// Payload stop / hot reload: both call sites back to stock.
void Shutdown() { Detach(); }

}  // namespace srvtrim
