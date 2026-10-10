// VRAM census ([Suite] VramCount, default 0; VramCountSec, default 20;
// VramCountCreates, default 1). Measure only: nothing DCS or DcsQvCull does
// is changed, and with the key at 0 none of this runs. Design notes and
// numbers: docs/research/R20_gpu_vram.md.
//
// Per second for VramCountSec seconds (suite thread, polling every ~2 ms):
//  - DXGI QueryVideoMemoryInfo of DCS's adapter (IDXGIAdapter3 from DCS's
//    device), segment groups LOCAL (VRAM) and NON_LOCAL (system memory the GPU
//    maps): Budget, CurrentUsage, AvailableForReservation, CurrentReservation.
//    These are the process's own numbers (DCS plus everything in it: QVFR,
//    the OpenXR runtime's in-process part, DcsQvCull). Usage near or above the
//    budget means the video memory manager demotes or evicts allocations
//    (paging, a known source of hitches).
//  - NVML's whole-GPU used/total when nvml.dll is loaded (gpu.h), for the
//    other processes (compositor, runtime service) in comparison.
//  - Frames and the longest gap between two frame-counter steps (a lower
//    bound of the worst frame time at ~2 ms resolution).
//  - VramCountCreates=1: ID3D11Device CreateBuffer / CreateTexture1D/2D/3D on
//    DCS's device are counted (count and bytes by description; destruction is
//    not visible), with a "same description and same first 4 KB of initial
//    data as an earlier create in the phase" counter (possible duplicate
//    uploads). The device table is patched during the phase only and put
//    back at its end and at payload stop (Shutdown).
//
// At the start and at the end (suite thread):
//  - DcsQvCull's own D3D11 memory by category: the recorders' constant-buffer
//    rings (bytes created, CbRing::createdBytes), t127 offset buffers, late-
//    copy CBs, batching's offset buffers. DYNAMIC buffers are renamed by
//    every Map(DISCARD) (the driver keeps one copy per frame in flight), so
//    their footprint is a multiple of ByteWidth; command-list memory is the
//    driver's and is not visible.
//  - NGModel's model-data pages (alloc_slab's manager, under its lock): page
//    count and GPU bytes by element size, big pages (big_pages.h) separately.
//  - The recorders' texture tables (shrec::g_tex, gbrec::g_tex), both read
//    under their locks held shared (maintenance on the render thread only
//    tries the lock exclusively and defers): live entries, entries whose
//    texture no longer gives their views (TexCheck fails: a mip-set swap or a
//    freed texture since the entry was built; they keep the old views until
//    rebuilt or evicted), views only DcsQvCull still references (refcount
//    after AddRef/Release <= our references; command lists or job snapshots
//    holding the view make this a lower bound), and resources whose every
//    view we hold is such an orphan and that nobody references directly
//    (d3d11 views keep their resource alive without a public reference: the
//    resource's own refcount is 0 then, checked offline): the bytes DcsQvCull
//    alone keeps alive (estimated from the resource description).
//
// Included once from main.cpp inside its anonymous namespace, after
// shadow_rec.h, gb_rec.h, shadow_batch.h, gb_batch.h, alloc_slab.h, gpu.h
// and gpu_pass_timing.h; needs <dxgi1_4.h> (included by main.cpp).
#pragma once

namespace vramc {

// ---------------------------------------------------------------------------
// Pure helpers (offline tested)
// ---------------------------------------------------------------------------

// Bits per texel (block = 1) or per 4x4 block (block = 4) of a DXGI format;
// bits 0 for formats not listed.
inline void FormatInfo(DXGI_FORMAT f, uint32_t* bits, uint32_t* block) {
  const int v = static_cast<int>(f);
  *block = 1;
  if (v >= 1 && v <= 4) *bits = 128;
  else if (v >= 5 && v <= 8) *bits = 96;
  else if (v >= 9 && v <= 22) *bits = 64;
  else if (v >= 23 && v <= 47) *bits = 32;
  else if (v >= 48 && v <= 59) *bits = 16;
  else if (v >= 60 && v <= 65) *bits = 8;
  else if (v == 66) *bits = 1;
  else if (v == 67) *bits = 32;
  else if (v == 68 || v == 69) *bits = 16;
  else if ((v >= 70 && v <= 72) || (v >= 79 && v <= 81)) *bits = 64, *block = 4;   // BC1, BC4
  else if ((v >= 73 && v <= 78) || (v >= 82 && v <= 84)) *bits = 128, *block = 4;  // BC2, BC3, BC5
  else if (v == 85 || v == 86) *bits = 16;
  else if (v >= 87 && v <= 93) *bits = 32;
  else if (v >= 94 && v <= 99) *bits = 128, *block = 4;  // BC6H, BC7
  else if (v == 100 || v == 101 || v == 108 || v == 109) *bits = 32;  // AYUV, Y410, Y210, Y216
  else if (v == 102) *bits = 64;                                      // Y416
  else if (v == 103 || v == 106 || v == 110) *bits = 12;              // NV12, 420_OPAQUE, NV11
  else if (v == 104 || v == 105) *bits = 24;                          // P010, P016
  else if (v == 107) *bits = 16;                                      // YUY2
  else if (v >= 111 && v <= 113) *bits = 8;                           // AI44, IA44, P8
  else if (v == 114 || v == 115) *bits = 16;                          // A8P8, B4G4R4A4
  else *bits = 0;
}

inline uint32_t FullMips(uint32_t w, uint32_t h, uint32_t d) {
  uint32_t m = std::max(w, std::max(h, d)), n = 1;
  while (m > 1) m >>= 1, ++n;
  return n;
}

// Bytes of a texture's subresources (mips 0 = the full chain).
inline uint64_t TextureBytes(DXGI_FORMAT fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t mips, uint32_t array,
                             uint32_t samples) {
  uint32_t bits = 0, block = 1;
  FormatInfo(fmt, &bits, &block);
  if (!bits) return 0;
  if (!mips) mips = FullMips(w, h, d);
  uint64_t total = 0;
  for (uint32_t m = 0; m < mips; ++m) {
    const uint64_t mw = std::max<uint32_t>(1, w >> m), mh = std::max<uint32_t>(1, h >> m),
                   md = std::max<uint32_t>(1, d >> m);
    if (block > 1)
      total += ((mw + 3) / 4) * ((mh + 3) / 4) * md * bits / 8;
    else
      total += (mw * mh * md * bits + 7) / 8;
  }
  return total * std::max<uint32_t>(1, array) * std::max<uint32_t>(1, samples);
}

// One view the tables reference: our references, the refcount others hold
// (after our temporary AddRef/Release), its resource and that resource's
// refcount and bytes; stale = some entry holding it no longer matches its texture.
struct ViewInfo {
  const void* view = nullptr;
  const void* res = nullptr;
  uint32_t ours = 0;
  uint32_t refs = 0;     // the view's refcount (all holders)
  uint32_t resRefs = 0;  // the resource's public refcount (direct holders; views do not count)
  uint64_t bytes = 0;    // the resource's
  bool stale = false;
};
struct PinResult {
  uint64_t uniqueViews = 0, orphanViews = 0, staleViews = 0;
  uint64_t uniqueRes = 0, resBytes = 0, staleRes = 0, staleBytes = 0, pinnedRes = 0, pinnedBytes = 0;
};
// A view is an orphan when nothing but our references holds it (refs <=
// ours). A resource is pinned by us alone when every view of it we hold is
// an orphan and nobody holds the resource itself: a d3d11 view keeps its
// resource alive through a private reference, so the resource's public
// refcount counts only direct holders (offline: a texture whose owner let go
// of it and of its view reads 0 while our view still exists).
inline PinResult Pinned(const std::vector<ViewInfo>& views) {
  PinResult r;
  struct Agg {
    uint64_t bytes = 0;
    uint32_t views = 0, orphans = 0, resRefs = 0;
    bool stale = false;
  };
  std::unordered_map<const void*, Agg> res;
  for (const ViewInfo& v : views) {
    ++r.uniqueViews;
    const bool orphan = v.refs <= v.ours;
    r.orphanViews += orphan;
    r.staleViews += v.stale;
    if (!v.res) continue;
    Agg& a = res[v.res];
    a.bytes = v.bytes;
    a.resRefs = v.resRefs;
    ++a.views;
    a.orphans += orphan;
    a.stale |= v.stale;
  }
  for (auto& kv : res) {
    const Agg& a = kv.second;
    ++r.uniqueRes;
    r.resBytes += a.bytes;
    if (a.stale) {
      ++r.staleRes;
      r.staleBytes += a.bytes;
    }
    if (a.orphans == a.views && a.resRefs == 0) {
      ++r.pinnedRes;
      r.pinnedBytes += a.bytes;
    }
  }
  return r;
}

struct Mem {
  uint64_t budget = 0, usage = 0, avail = 0, reserv = 0;
};
struct Sample {
  Mem local, nonLocal;
  uint64_t gpuUsed = 0, gpuTotal = 0;  // NVML, whole GPU (0: unavailable)
  uint64_t frames = 0;
  double seconds = 0;
  double worstMs = 0;                  // longest frame-counter gap
  uint64_t texCreates = 0, texBytes = 0, bufCreates = 0, bufBytes = 0;
};
struct Summary {
  int n = 0;
  double meanLocalGb = 0, maxLocalGb = 0, minBudgetGb = 0, maxUsePct = 0, growthMb = 0;
  double meanNonLocalGb = 0, maxNonLocalGb = 0;
  int secAbove90 = 0, secAbove100 = 0;
  double medianWorstMs = 0, maxWorstMs = 0;
  int hitchSecs = 0;                   // longest frame > 2x the median longest frame
  int hitchWithCreates = 0;            // ... in a second with > 64 MB created
  int hitchNearBudget = 0;             // ... in a second at >= 95 % of the budget
};
inline Summary Summarize(const std::vector<Sample>& s) {
  Summary r;
  r.n = static_cast<int>(s.size());
  if (s.empty()) return r;
  const double gb = 1.0 / (1024.0 * 1024.0 * 1024.0);
  r.minBudgetGb = 1e30;
  std::vector<double> worst;
  for (const Sample& x : s) {
    const double use = x.local.usage * gb, bud = x.local.budget * gb;
    r.meanLocalGb += use;
    r.maxLocalGb = std::max(r.maxLocalGb, use);
    r.minBudgetGb = std::min(r.minBudgetGb, bud);
    const double pct = bud > 0 ? 100.0 * use / bud : 0.0;
    r.maxUsePct = std::max(r.maxUsePct, pct);
    r.secAbove90 += pct >= 90.0;
    r.secAbove100 += pct >= 100.0;
    r.meanNonLocalGb += x.nonLocal.usage * gb;
    r.maxNonLocalGb = std::max(r.maxNonLocalGb, x.nonLocal.usage * gb);
    worst.push_back(x.worstMs);
  }
  r.meanLocalGb /= s.size();
  r.meanNonLocalGb /= s.size();
  r.growthMb = (static_cast<double>(s.back().local.usage) - static_cast<double>(s.front().local.usage)) / 1048576.0;
  std::vector<double> w = worst;
  std::sort(w.begin(), w.end());
  r.medianWorstMs = w[w.size() / 2];
  r.maxWorstMs = w.back();
  for (const Sample& x : s) {
    if (r.medianWorstMs <= 0 || x.worstMs <= 2.0 * r.medianWorstMs) continue;
    ++r.hitchSecs;
    r.hitchWithCreates += (x.texBytes + x.bufBytes) > (64ull << 20);
    r.hitchNearBudget += x.local.budget && x.local.usage * 100 >= x.local.budget * 95;
  }
  return r;
}

// ---------------------------------------------------------------------------
// Live part
// ---------------------------------------------------------------------------

uint64_t ResourceBytes(ID3D11Resource* r) {
  if (!r) return 0;
  D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
  r->GetType(&dim);
  switch (dim) {
    case D3D11_RESOURCE_DIMENSION_BUFFER: {
      D3D11_BUFFER_DESC d;
      static_cast<ID3D11Buffer*>(r)->GetDesc(&d);
      return d.ByteWidth;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
      D3D11_TEXTURE1D_DESC d;
      static_cast<ID3D11Texture1D*>(r)->GetDesc(&d);
      return TextureBytes(d.Format, d.Width, 1, 1, d.MipLevels, d.ArraySize, 1);
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
      D3D11_TEXTURE2D_DESC d;
      static_cast<ID3D11Texture2D*>(r)->GetDesc(&d);
      return TextureBytes(d.Format, d.Width, d.Height, 1, d.MipLevels, d.ArraySize, d.SampleDesc.Count);
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
      D3D11_TEXTURE3D_DESC d;
      static_cast<ID3D11Texture3D*>(r)->GetDesc(&d);
      return TextureBytes(d.Format, d.Width, d.Height, d.Depth, d.MipLevels, 1, 1);
    }
    default:
      return 0;
  }
}

uint64_t BufBytes(ID3D11Buffer* b) {
  if (!b) return 0;
  D3D11_BUFFER_DESC d;
  b->GetDesc(&d);
  return d.ByteWidth;
}

ULONG RefCount(IUnknown* p) {
  p->AddRef();
  return p->Release();
}

// ---- DXGI and NVML ----
IDXGIAdapter3* g_adapter = nullptr;
struct NvmlMemory {
  unsigned long long total, free, used;
};
using NvmlGetMemoryInfo = int (*)(void*, NvmlMemory*);
NvmlGetMemoryInfo g_nvmlMem = nullptr;

bool OpenAdapter(ID3D11Device* dev) {
  if (g_adapter) return true;
  IDXGIDevice* dx = nullptr;
  if (FAILED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dx))) || !dx) return false;
  IDXGIAdapter* a = nullptr;
  const HRESULT hr = dx->GetAdapter(&a);
  dx->Release();
  if (FAILED(hr) || !a) return false;
  a->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&g_adapter));
  a->Release();
  return g_adapter != nullptr;
}

void ReadMem(Sample& s) {
  DXGI_QUERY_VIDEO_MEMORY_INFO i{};
  if (g_adapter && SUCCEEDED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &i)))
    s.local = {i.Budget, i.CurrentUsage, i.AvailableForReservation, i.CurrentReservation};
  if (g_adapter && SUCCEEDED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &i)))
    s.nonLocal = {i.Budget, i.CurrentUsage, i.AvailableForReservation, i.CurrentReservation};
  if (!g_nvmlMem && gpu::Init())
    if (HMODULE m = GetModuleHandleW(L"nvml.dll"))
      g_nvmlMem = reinterpret_cast<NvmlGetMemoryInfo>(GetProcAddress(m, "nvmlDeviceGetMemoryInfo"));
  NvmlMemory nm{};
  if (g_nvmlMem && gpu::g_device && g_nvmlMem(gpu::g_device, &nm) == 0) {
    s.gpuUsed = nm.used;
    s.gpuTotal = nm.total;
  }
}

// ---- Device creates (phase only) ----
constexpr int kSlotBuffer = 3, kSlotTex1D = 4, kSlotTex2D = 5, kSlotTex3D = 6;  // ID3D11Device vtable
ID3D11Device* g_dev = nullptr;
void** g_devVt = nullptr;
void* volatile g_origBuf = nullptr;
void* volatile g_origTex1 = nullptr;
void* volatile g_origTex2 = nullptr;
void* volatile g_origTex3 = nullptr;
std::atomic<bool> g_counting{false};
std::atomic<uint64_t> g_texCreates{0}, g_texBytes{0}, g_bufCreates{0}, g_bufBytes{0}, g_failed{0};
std::mutex g_censusMutex;
std::unordered_map<uint64_t, uint32_t> g_seen;        // description + data hash -> creates
std::map<std::string, std::pair<uint64_t, uint64_t>> g_byDesc;  // "kind format usage bind" -> (count, bytes)
uint64_t g_dupCreates = 0, g_dupBytes = 0;

inline uint64_t Fnv(uint64_t h, const void* p, size_t n) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 0x100000001b3ull;
  return h;
}

void Note(const char* kind, int fmt, UINT usage, UINT bind, uint64_t bytes, const void* desc, size_t descLen,
          const D3D11_SUBRESOURCE_DATA* init, size_t initBytes) {
  uint64_t h = Fnv(0xcbf29ce484222325ull, desc, descLen);
  const bool hashed = init && init->pSysMem;
  if (hashed) h = Fnv(h, init->pSysMem, std::min<size_t>(initBytes, 4096));
  char key[96];
  snprintf(key, sizeof(key), "%s fmt %d usage %u bind 0x%x", kind, fmt, usage, bind);
  std::lock_guard<std::mutex> lock(g_censusMutex);
  auto& d = g_byDesc[key];
  ++d.first;
  d.second += bytes;
  if (hashed && g_seen[h]++) {
    ++g_dupCreates;
    g_dupBytes += bytes;
  }
}

using CreateBufferFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_BUFFER_DESC*,
                                                   const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
using CreateTex1Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE1D_DESC*,
                                                 const D3D11_SUBRESOURCE_DATA*, ID3D11Texture1D**);
using CreateTex2Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE2D_DESC*,
                                                 const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**);
using CreateTex3Fn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_TEXTURE3D_DESC*,
                                                 const D3D11_SUBRESOURCE_DATA*, ID3D11Texture3D**);

HRESULT STDMETHODCALLTYPE H_CreateBuffer(ID3D11Device* d, const D3D11_BUFFER_DESC* desc,
                                         const D3D11_SUBRESOURCE_DATA* init, ID3D11Buffer** out) {
  const HRESULT hr = reinterpret_cast<CreateBufferFn>(g_origBuf)(d, desc, init, out);
  if (d == g_dev && desc && out && g_counting.load(std::memory_order_relaxed)) {
    if (FAILED(hr)) {
      g_failed.fetch_add(1, std::memory_order_relaxed);
    } else {
      g_bufCreates.fetch_add(1, std::memory_order_relaxed);
      g_bufBytes.fetch_add(desc->ByteWidth, std::memory_order_relaxed);
      Note("buffer", 0, desc->Usage, desc->BindFlags, desc->ByteWidth, desc, sizeof(*desc), init, desc->ByteWidth);
    }
  }
  return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateTex1(ID3D11Device* d, const D3D11_TEXTURE1D_DESC* desc,
                                       const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture1D** out) {
  const HRESULT hr = reinterpret_cast<CreateTex1Fn>(g_origTex1)(d, desc, init, out);
  if (d == g_dev && desc && out && g_counting.load(std::memory_order_relaxed)) {
    if (FAILED(hr)) {
      g_failed.fetch_add(1, std::memory_order_relaxed);
    } else {
      const uint64_t b = TextureBytes(desc->Format, desc->Width, 1, 1, desc->MipLevels, desc->ArraySize, 1);
      g_texCreates.fetch_add(1, std::memory_order_relaxed);
      g_texBytes.fetch_add(b, std::memory_order_relaxed);
      Note("tex1d", desc->Format, desc->Usage, desc->BindFlags, b, desc, sizeof(*desc), init,
           TextureBytes(desc->Format, desc->Width, 1, 1, 1, 1, 1));
    }
  }
  return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateTex2(ID3D11Device* d, const D3D11_TEXTURE2D_DESC* desc,
                                       const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture2D** out) {
  const HRESULT hr = reinterpret_cast<CreateTex2Fn>(g_origTex2)(d, desc, init, out);
  if (d == g_dev && desc && out && g_counting.load(std::memory_order_relaxed)) {
    if (FAILED(hr)) {
      g_failed.fetch_add(1, std::memory_order_relaxed);
    } else {
      const uint64_t b = TextureBytes(desc->Format, desc->Width, desc->Height, 1, desc->MipLevels, desc->ArraySize,
                                      desc->SampleDesc.Count);
      g_texCreates.fetch_add(1, std::memory_order_relaxed);
      g_texBytes.fetch_add(b, std::memory_order_relaxed);
      Note("tex2d", desc->Format, desc->Usage, desc->BindFlags, b, desc, sizeof(*desc), init,
           TextureBytes(desc->Format, desc->Width, desc->Height, 1, 1, 1, 1));
    }
  }
  return hr;
}
HRESULT STDMETHODCALLTYPE H_CreateTex3(ID3D11Device* d, const D3D11_TEXTURE3D_DESC* desc,
                                       const D3D11_SUBRESOURCE_DATA* init, ID3D11Texture3D** out) {
  const HRESULT hr = reinterpret_cast<CreateTex3Fn>(g_origTex3)(d, desc, init, out);
  if (d == g_dev && desc && out && g_counting.load(std::memory_order_relaxed)) {
    if (FAILED(hr)) {
      g_failed.fetch_add(1, std::memory_order_relaxed);
    } else {
      const uint64_t b = TextureBytes(desc->Format, desc->Width, desc->Height, desc->Depth, desc->MipLevels, 1, 1);
      g_texCreates.fetch_add(1, std::memory_order_relaxed);
      g_texBytes.fetch_add(b, std::memory_order_relaxed);
      Note("tex3d", desc->Format, desc->Usage, desc->BindFlags, b, desc, sizeof(*desc), init,
           TextureBytes(desc->Format, desc->Width, desc->Height, 1, 1, 1, 1));
    }
  }
  return hr;
}

struct DevHook {
  int slot;
  void* volatile* orig;
  void* hook;
};
const DevHook kDevHooks[] = {{kSlotBuffer, &g_origBuf, reinterpret_cast<void*>(&H_CreateBuffer)},
                             {kSlotTex1D, &g_origTex1, reinterpret_cast<void*>(&H_CreateTex1)},
                             {kSlotTex2D, &g_origTex2, reinterpret_cast<void*>(&H_CreateTex2)},
                             {kSlotTex3D, &g_origTex3, reinterpret_cast<void*>(&H_CreateTex3)}};

bool HookCreates(ID3D11Device* dev) {
  void** vt = *reinterpret_cast<void***>(dev);
  for (const DevHook& h : kDevHooks)
    if (vt[h.slot] == h.hook) return false;  // already ours (a phase still running)
  g_dev = dev;
  g_devVt = vt;
  for (const DevHook& h : kDevHooks) {
    *h.orig = vt[h.slot];
    d3ds::WriteTablePointer(&vt[h.slot], h.hook);
  }
  g_counting = true;
  return true;
}

// Puts d3d11's (or the previous holder's) entries back where ours still are.
// The forwarders stay valid (payload images are never unloaded).
void UnhookCreates() {
  g_counting = false;
  void** vt = g_devVt;
  if (!vt) return;
  for (const DevHook& h : kDevHooks)
    if (vt[h.slot] == h.hook) d3ds::WriteTablePointer(&vt[h.slot], *h.orig);
  g_devVt = nullptr;
}

void Shutdown() { UnhookCreates(); }

// ---- DcsQvCull's own buffers ----
struct OwnRow {
  const char* what;
  uint64_t bytes;
  uint32_t objects;
  const char* note;
};

void RingBytes(defrec::Pool& p, uint64_t* bytes, uint32_t* rings) {
  if (!p.Started()) return;
  for (int i = 0; i < p.Workers(); ++i) {
    *bytes += p.At(i).ring.createdBytes;
    ++*rings;
  }
}

std::vector<OwnRow> OwnCensus() {
  std::vector<OwnRow> rows;
  uint64_t b = 0;
  uint32_t n = 0;
  RingBytes(shrec::g_pool, &b, &n);
  rows.push_back({"shadow recorder: constant-buffer rings (DYNAMIC, renamed per Map DISCARD)", b, n, "per worker"});
  b = 0, n = 0;
  for (ID3D11Buffer* o : shrec::g_offBuf)
    if (o) b += BufBytes(o), ++n;
  rows.push_back({"shadow recorder: t127 offset buffers (DYNAMIC)", b, n, "one per worker"});
  b = 0, n = 0;
  for (const shrec::Casc& c : shrec::g_casc)
    if (c.b7) b += BufBytes(c.b7), ++n;
  rows.push_back({"shadow recorder: late-copied b7 (DEFAULT)", b, n, "one per cascade"});
  b = 0, n = 0;
  RingBytes(gbrec::g_pool, &b, &n);
  rows.push_back({"g-buffer recorder: constant-buffer rings (DYNAMIC, renamed per Map DISCARD)", b, n, "per worker"});
  b = 0, n = 0;
  for (const gbrec::Slot& s : gbrec::g_slot)
    for (int st = 0; st < 2; ++st)
      for (int k = 0; k < 3; ++k)
        if (s.ours[st][k]) b += BufBytes(s.ours[st][k]), ++n;
  rows.push_back({"g-buffer recorder: late-copied context CBs (DEFAULT)", b, n, "per execution slot"});
  b = 0, n = 0;
  if (shadowbatch::g_buf) b += BufBytes(shadowbatch::g_buf), ++n;
  if (gbbatch::g_buf) b += BufBytes(gbbatch::g_buf), ++n;
  rows.push_back({"batching: t127 offset buffers (DYNAMIC)", b, n, "shadow and g-buffer batching"});
  return rows;
}

// ---- NGModel's model-data pages ----
struct PageCensus {
  bool ok = false;
  uint64_t pages = 0, withBuf = 0, bigPages = 0, bigBytes = 0, smallBytes = 0, capBytes = 0, usedBytes = 0;
  std::map<uint32_t, std::pair<uint64_t, uint64_t>> bySize;  // element size -> (pages, GPU bytes)
};

uint64_t PageGpuBytesRaw(const allocslab::Page* p) {
  if (!p->gpuBuf) return 0;
  auto* srv = *reinterpret_cast<ID3D11ShaderResourceView* const*>(static_cast<const uint8_t*>(p->gpuBuf) + 0x30);
  if (!srv) return 0;
  ID3D11Resource* r = nullptr;
  srv->GetResource(&r);
  const uint64_t b = ResourceBytes(r);
  if (r) r->Release();
  return b;
}

uint64_t PageGpuBytesGuarded(const allocslab::Page* p) {
  __try {
    return PageGpuBytesRaw(p);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

PageCensus Pages() {
  PageCensus c;
  void* mgr = allocslab::g_mgr.load();
  if (!mgr || !allocslab::g_lock || !allocslab::g_unlock) return c;
  uint8_t* m = static_cast<uint8_t*>(mgr);
  allocslab::g_lock(m + allocslab::kMgrMutex);
  uint8_t* b = *reinterpret_cast<uint8_t**>(m + 8);
  uint8_t* e = *reinterpret_cast<uint8_t**>(m + 0x10);
  const size_t n = (b && e > b) ? static_cast<size_t>(e - b) / allocslab::kPageStride : 0;
  for (size_t i = 0; i < n && i < 100000; ++i) {
    const auto* p = reinterpret_cast<const allocslab::Page*>(b + i * allocslab::kPageStride);
    ++c.pages;
    c.capBytes += p->capBytes;
    c.usedBytes += p->prevUsed;
    const uint64_t gb = PageGpuBytesGuarded(p);
    if (!gb) continue;
    ++c.withBuf;
    auto& s = c.bySize[p->elemSize];
    ++s.first;
    s.second += gb;
    if (gb >= (1u << 20)) {
      ++c.bigPages;
      c.bigBytes += gb;
    } else {
      c.smallBytes += gb;
    }
  }
  allocslab::g_unlock(m + allocslab::kMgrMutex);
  c.ok = true;
  return c;
}

// ---- The recorders' texture tables ----
struct TableCount {
  uint64_t entries = 0, withViews = 0, usable = 0, stale = 0, viewRefs = 0, old = 0;
};

bool TexCheckGuarded(const gbbatch::TexEnv& env, const shrec::TexEntry& e) {
  __try {
    return shrec::TexCheckRaw(env, e);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Walks one table (its lock held shared by the caller); views go into `views`
// keyed by pointer (ours counts every reference both tables hold).
void WalkTable(const shrec::TexTable& t, const gbbatch::TexEnv& env, TableCount& c,
               std::unordered_map<const void*, ViewInfo>& views, uint32_t oldAge) {
  if (!t.e) return;
  uint32_t newest = 0;
  for (uint32_t i = 0; i < t.size; ++i)
    if (t.e[i].slot.load(std::memory_order_relaxed) == shrec::kTeLive)
      newest = std::max(newest, t.e[i].lastUse.load(std::memory_order_relaxed));
  for (uint32_t i = 0; i < t.size; ++i) {
    const shrec::TexEntry& e = t.e[i];
    if (e.slot.load(std::memory_order_relaxed) != shrec::kTeLive) continue;
    ++c.entries;
    c.usable += e.usable;
    if (newest - e.lastUse.load(std::memory_order_relaxed) > oldAge) ++c.old;
    bool any = false;
    for (ID3D11ShaderResourceView* v : e.views)
      if (v) any = true;
    if (!any) continue;
    ++c.withViews;
    const bool stale = !e.usable || !TexCheckGuarded(env, e);
    c.stale += stale;
    for (ID3D11ShaderResourceView* v : e.views) {
      if (!v) continue;
      ++c.viewRefs;
      ViewInfo& vi = views[v];
      vi.view = v;
      ++vi.ours;
      vi.stale |= stale;
    }
  }
}

// Refcounts and resources of the collected views (their tables' locks still held: they cannot be released).
void ResolveViews(std::unordered_map<const void*, ViewInfo>& views, std::vector<ViewInfo>& out) {
  out.reserve(views.size());
  for (auto& kv : views) {
    ViewInfo vi = kv.second;
    auto* v = static_cast<ID3D11ShaderResourceView*>(const_cast<void*>(vi.view));
    vi.refs = RefCount(v);
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    if (r) {
      vi.res = r;
      vi.bytes = ResourceBytes(r);
      vi.resRefs = r->Release();  // the reference GetResource took
    }
    out.push_back(vi);
  }
}

void TableCensus(const char* when) {
  const uint32_t kOldAge = 120;  // render entries (about 2 s at 60 fps)
  std::unordered_map<const void*, ViewInfo> views;
  TableCount sc, gc;
  std::vector<ViewInfo> resolved;
  const bool haveS = shrec::g_tex.e != nullptr, haveG = gbrec::g_tex.e != nullptr;
  if (!haveS && !haveG) {
    Log("  texture tables (%s): neither recorder has a table (recorders off or never installed)", when);
    return;
  }
  LARGE_INTEGER a, b;
  QueryPerformanceCounter(&a);
  AcquireSRWLockShared(&shrec::g_texLock);  // always this order (nothing else takes both)
  AcquireSRWLockShared(&gbrec::g_texLock);
  WalkTable(shrec::g_tex, shrec::g_env.tex, sc, views, kOldAge);
  WalkTable(gbrec::g_tex, gbrec::g_env.tex, gc, views, kOldAge);
  ResolveViews(views, resolved);
  ReleaseSRWLockShared(&gbrec::g_texLock);
  ReleaseSRWLockShared(&shrec::g_texLock);
  QueryPerformanceCounter(&b);
  const PinResult p = Pinned(resolved);
  const double mb = 1.0 / 1048576.0;
  auto row = [&](const char* nm, const TableCount& c, uint32_t size) {
    Log("    %-22s live %6llu of %6u slots, with views %6llu (usable %llu), view refs %7llu, stale (texture changed "
        "or freed since built) %6llu, unused > %u render entries %6llu",
        nm, static_cast<unsigned long long>(c.entries), size, static_cast<unsigned long long>(c.withViews),
        static_cast<unsigned long long>(c.usable), static_cast<unsigned long long>(c.viewRefs),
        static_cast<unsigned long long>(c.stale), kOldAge, static_cast<unsigned long long>(c.old));
  };
  Log("  texture tables (%s), read in %.2f ms with both locks shared:", when,
      (b.QuadPart - a.QuadPart) * g_qpcToUs / 1000.0);
  row("shadow recorder", sc, shrec::g_tex.size);
  row("g-buffer recorder", gc, gbrec::g_tex.size);
  Log("    both: %llu unique views (%llu held only by DcsQvCull, %llu in stale entries) on %llu resources, %.1f MB",
      static_cast<unsigned long long>(p.uniqueViews), static_cast<unsigned long long>(p.orphanViews),
      static_cast<unsigned long long>(p.staleViews), static_cast<unsigned long long>(p.uniqueRes), p.resBytes * mb);
  Log("    resources behind stale entries %llu, %.1f MB; resources kept alive by DcsQvCull alone (every view an "
      "orphan, nobody holds the resource) %llu, %.1f MB (lower bound: command lists and job snapshots also hold views)",
      static_cast<unsigned long long>(p.staleRes), p.staleBytes * mb, static_cast<unsigned long long>(p.pinnedRes),
      p.pinnedBytes * mb);
}

void LogOwn(const char* when) {
  const double mb = 1.0 / 1048576.0;
  uint64_t total = 0;
  Log("  DcsQvCull's own D3D11 buffers (%s; ByteWidth, before driver renaming; command-list memory not visible):",
      when);
  for (const OwnRow& r : OwnCensus()) {
    total += r.bytes;
    if (r.objects) Log("    %-72s %8.2f MB in %u (%s)", r.what, r.bytes * mb, r.objects, r.note);
  }
  Log("    total %.2f MB", total * mb);
  const PageCensus pc = Pages();
  if (!pc.ok) {
    Log("  model-data pages: allocator not hooked (alloc_slab off); not counted");
    return;
  }
  std::string sizes;
  char buf[64];
  for (auto& kv : pc.bySize) {
    snprintf(buf, sizeof(buf), " %uB x%llu %.1f MB", kv.first, static_cast<unsigned long long>(kv.second.first),
             kv.second.second * mb);
    sizes += buf;
  }
  Log("  model-data pages (DCS's, NGModel StructBufferManager): %llu pages, %llu with a GPU buffer; big pages %llu, "
      "%.1f MB; small %.1f MB; capacity %.1f MB, last frame used %.2f MB; by element size:%s",
      static_cast<unsigned long long>(pc.pages), static_cast<unsigned long long>(pc.withBuf),
      static_cast<unsigned long long>(pc.bigPages), pc.bigBytes * mb, pc.smallBytes * mb, pc.capBytes * mb,
      pc.usedBytes * mb, sizes.c_str());
}

// ---- Suite phase ----
void Measure(int seconds, bool creates, std::atomic<uint64_t>& frameCounter, std::atomic<bool>& abort) {
  ID3D11Device* dev = shadowinst::g_device;
  if (dev) {
    dev->AddRef();
  } else if (sfilt::Ctx* c = sfilt::g_ctx.load()) {
    c->GetDevice(&dev);
  }
  if (!dev) {
    Log("  vram count: DCS's device not known yet (it comes from [Model] ShadowInstancing=1 or [D3D] SplitFilter=1)");
    return;
  }
  if (!OpenAdapter(dev)) Log("  vram count: no IDXGIAdapter3 on DCS's device; budget and usage not read");
  LogOwn("start");
  TableCensus("start");
  const bool hooked = creates && HookCreates(dev);
  if (creates && !hooked) Log("  vram count: device create hooks already present; creates not counted");
  seconds = std::max(1, std::min(600, seconds));
  std::vector<Sample> samples;
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  const double gb = 1.0 / (1024.0 * 1024.0 * 1024.0), mb = 1.0 / 1048576.0;
  for (int s = 0; s < seconds && !abort.load() && !g_stop.load(); ++s) {
    const uint64_t t0c = g_texCreates.load(), t0b = g_texBytes.load(), b0c = g_bufCreates.load(),
                   b0b = g_bufBytes.load();
    LARGE_INTEGER start, now, lastStep;
    QueryPerformanceCounter(&start);
    lastStep = start;
    const uint64_t f0 = frameCounter.load();
    uint64_t f = f0;
    double worst = 0;
    do {
      Sleep(1);
      QueryPerformanceCounter(&now);
      const uint64_t fc = frameCounter.load();
      if (fc != f) {
        worst = std::max(worst, (now.QuadPart - lastStep.QuadPart) * 1000.0 / freq.QuadPart);
        lastStep = now;
        f = fc;
      }
    } while ((now.QuadPart - start.QuadPart) < freq.QuadPart);
    Sample x;
    ReadMem(x);
    x.frames = frameCounter.load() - f0;
    x.seconds = static_cast<double>(now.QuadPart - start.QuadPart) / freq.QuadPart;
    x.worstMs = std::max(worst, (now.QuadPart - lastStep.QuadPart) * 1000.0 / freq.QuadPart);
    x.texCreates = g_texCreates.load() - t0c;
    x.texBytes = g_texBytes.load() - t0b;
    x.bufCreates = g_bufCreates.load() - b0c;
    x.bufBytes = g_bufBytes.load() - b0b;
    samples.push_back(x);
    char nv[64] = "";
    if (x.gpuTotal) snprintf(nv, sizeof(nv), " | GPU used %.2f of %.1f GB", x.gpuUsed * gb, x.gpuTotal * gb);
    char cr[96] = "";
    if (hooked)
      snprintf(cr, sizeof(cr), " | created %llu tex %.1f MB, %llu buf %.1f MB",
               static_cast<unsigned long long>(x.texCreates), x.texBytes * mb,
               static_cast<unsigned long long>(x.bufCreates), x.bufBytes * mb);
    Log("  %3d s: local %.2f / %.2f GB (%.1f%%), avail %.2f, reserved %.2f | non-local %.2f / %.2f GB%s | %.1f fps, "
        "longest frame %.1f ms%s",
        s + 1, x.local.usage * gb, x.local.budget * gb,
        x.local.budget ? 100.0 * x.local.usage / x.local.budget : 0.0, x.local.avail * gb, x.local.reserv * gb,
        x.nonLocal.usage * gb, x.nonLocal.budget * gb, nv, x.frames / std::max(1e-6, x.seconds), x.worstMs, cr);
  }
  if (hooked) UnhookCreates();
  const Summary sm = Summarize(samples);
  if (sm.n) {
    Log("  vram summary (%d s): local mean %.2f GB, max %.2f GB, smallest budget %.2f GB, max %.1f%% of the budget "
        "(%d s at >= 90%%, %d s at >= 100%%), change over the phase %+.0f MB; non-local mean %.2f GB, max %.2f GB",
        sm.n, sm.meanLocalGb, sm.maxLocalGb, sm.minBudgetGb, sm.maxUsePct, sm.secAbove90, sm.secAbove100, sm.growthMb,
        sm.meanNonLocalGb, sm.maxNonLocalGb);
    Log("  hitches: longest frame per second median %.1f ms, max %.1f ms; %d s with a frame > 2x the median (%d of "
        "them with > 64 MB created, %d at >= 95%% of the budget)",
        sm.medianWorstMs, sm.maxWorstMs, sm.hitchSecs, sm.hitchWithCreates, sm.hitchNearBudget);
  }
  if (hooked) {
    std::vector<std::pair<std::string, std::pair<uint64_t, uint64_t>>> rows;
    uint64_t dups = 0, dupBytes = 0;
    {
      std::lock_guard<std::mutex> lock(g_censusMutex);
      rows.assign(g_byDesc.begin(), g_byDesc.end());
      dups = g_dupCreates;
      dupBytes = g_dupBytes;
      g_byDesc.clear();
      g_seen.clear();
      g_dupCreates = g_dupBytes = 0;
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
    Log("  creates on DCS's device during the phase: %llu textures %.1f MB, %llu buffers %.1f MB, %llu failed; "
        "same description and first 4 KB of initial data as an earlier create: %llu, %.1f MB",
        static_cast<unsigned long long>(g_texCreates.load()), g_texBytes.load() * mb,
        static_cast<unsigned long long>(g_bufCreates.load()), g_bufBytes.load() * mb,
        static_cast<unsigned long long>(g_failed.load()), static_cast<unsigned long long>(dups), dupBytes * mb);
    for (size_t i = 0; i < rows.size() && i < 12; ++i)
      Log("    %-44s %7llu creates %9.1f MB", rows[i].first.c_str(),
          static_cast<unsigned long long>(rows[i].second.first), rows[i].second.second * mb);
    g_texCreates = g_texBytes = g_bufCreates = g_bufBytes = g_failed = 0;
  }
  TableCensus("end");
  LogOwn("end");
  dev->Release();
}

}  // namespace vramc
