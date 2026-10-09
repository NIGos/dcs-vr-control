// Offline test for direct_upload.h: mapping at the first collect, the vt[2]
// bypass chained through par_upload's hook, restore by page index after the
// page vector reallocates, undo on a failed Map, restore at a reset when the
// upload never came, off = stock, and the sentinel verify (a)-(c). Fakes:
// StructBufferManager (page vector, mutex), DX11StructuredBuffer (Map with
// rename garbage, Unmap, stock update), ID3D11Buffer::GetDesc, and slot 8
// (creates missing buffers and calls update(0, data, used) as 0xbdb0 does).
#pragma once

namespace dutest {

namespace du = directupload;
using allocslab::Page;

struct FakeD3d : ID3D11Buffer {
  D3D11_BUFFER_DESC desc = {};
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  void STDMETHODCALLTYPE GetDevice(ID3D11Device** d) override { *d = nullptr; }
  HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_FAIL; }
  HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_FAIL; }
  HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_FAIL; }
  void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* t) override { *t = D3D11_RESOURCE_DIMENSION_BUFFER; }
  void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}
  UINT STDMETHODCALLTYPE GetEvictionPriority() override { return 0; }
  void STDMETHODCALLTYPE GetDesc(D3D11_BUFFER_DESC* d) override { *d = desc; }
};

// DX11StructuredBuffer layout as far as the code reads it.
struct FakeSb {
  void** vtbl;
  uint8_t pad0[0x18];
  ID3D11Buffer* d3d;  // +0x20
  uint8_t pad1[0x1c];
  int32_t usage;  // +0x44
  int id;
};
static_assert(offsetof(FakeSb, d3d) == 0x20 && offsetof(FakeSb, usage) == 0x44, "fake layout");

struct Gpu {
  std::vector<uint8_t> mem;  // the buffer's contents as the GPU would see them
  bool mapped = false, doubleMap = false;
  int maps = 0, unmaps = 0;
};
constexpr int kBufs = 32;
Gpu g_gpu[kBufs];
FakeSb g_sb[kBufs];
FakeD3d g_d3d[kBufs];
int g_nSb = 0, g_failMapId = -1, g_stockCopies = 0, g_lockDepth = 0, g_lockMax = 0;
void* g_vt[8] = {};

uint8_t* __fastcall FakeMap(void* self, int mode) {
  Gpu& g = g_gpu[static_cast<FakeSb*>(self)->id];
  if (g.mapped) g.doubleMap = true;
  if (static_cast<FakeSb*>(self)->id == g_failMapId || mode != 1) return nullptr;
  g.mapped = true;
  g.maps++;
  memset(g.mem.data(), 0xDD, g.mem.size());  // WRITE_DISCARD: a fresh allocation, old bytes gone
  return g.mem.data();
}
void __fastcall FakeUnmap(void* self) {
  Gpu& g = g_gpu[static_cast<FakeSb*>(self)->id];
  g.mapped = false;
  g.unmaps++;
}
// Stock dynamic update [V 0x322b9]: Map(1), memcpy(p, data + offset, size), Unmap.
bool __fastcall FakeStockUpdate(void* self, uint32_t offset, const void* data, int32_t size) {
  void** vt = *static_cast<void***>(self);
  uint8_t* p = reinterpret_cast<du::MapFn>(vt[3])(self, 1);
  if (!p) return false;
  memcpy(p, static_cast<const uint8_t*>(data) + offset, static_cast<size_t>(size));
  reinterpret_cast<du::UnmapFn>(vt[4])(self);
  ++g_stockCopies;
  return true;
}
void __fastcall FakeLock(void*) { g_lockMax = std::max(g_lockMax, ++g_lockDepth); }
void __fastcall FakeUnlock(void*) { --g_lockDepth; }

FakeSb* NewBuf(uint32_t bytes, int32_t usage = 3) {
  const int id = g_nSb++;
  FakeSb& s = g_sb[id];
  s = FakeSb{};
  s.vtbl = g_vt;
  s.d3d = &g_d3d[id];
  s.usage = usage;
  s.id = id;
  g_d3d[id].desc = {};
  g_d3d[id].desc.ByteWidth = bytes;
  g_d3d[id].desc.Usage = D3D11_USAGE_DYNAMIC;
  g_d3d[id].desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  g_gpu[id] = Gpu{};
  g_gpu[id].mem.assign(bytes, 0);
  return &s;
}

// The manager: page vector at +8/+0x10, mutex at +0x20.
alignas(16) uint8_t g_mgr[0x40];
std::vector<uint8_t*> g_storage;  // every page vector ever used (old ones poisoned, kept)
std::vector<std::vector<uint8_t>*> g_heaps;
Page* Pages() { return reinterpret_cast<Page*>(*reinterpret_cast<uint8_t**>(g_mgr + 8)); }
size_t NumPages() {
  return (*reinterpret_cast<uint8_t**>(g_mgr + 0x10) - *reinterpret_cast<uint8_t**>(g_mgr + 8)) /
         allocslab::kPageStride;
}
Page& P(uint32_t i) { return *reinterpret_cast<Page*>(*reinterpret_cast<uint8_t**>(g_mgr + 8) + i * 0x30); }
void SetVector(uint8_t* b, size_t n) {
  *reinterpret_cast<uint8_t**>(g_mgr + 8) = b;
  *reinterpret_cast<uint8_t**>(g_mgr + 0x10) = b + n * allocslab::kPageStride;
}
uint8_t* NewHeap(uint32_t cap) {
  auto* v = new std::vector<uint8_t>(cap, 0x11);
  g_heaps.push_back(v);
  return v->data();
}
struct Spec {
  uint32_t elem, cap, prevUsed, used;
  bool buf;
  int32_t usage;
};
// A fresh vector of pages, page i has idx i.
void Build(const std::vector<Spec>& specs) {
  auto* b = new uint8_t[specs.size() * allocslab::kPageStride + 16]();
  g_storage.push_back(b);
  SetVector(b, specs.size());
  for (uint32_t i = 0; i < specs.size(); ++i) {
    const Spec& s = specs[i];
    Page& p = P(i);
    p = Page{i, s.elem, s.elem ? s.cap / s.elem : 0, s.cap, s.prevUsed, s.used, NewHeap(s.cap ? s.cap : 64), nullptr};
    if (s.buf) p.gpuBuf = NewBuf(s.cap ? s.cap : 64, s.usage);
  }
}
// Grows the vector by one page (new heap, no GPU buffer) at a new address and
// poisons the old one, as std::vector reallocation would leave it.
void Grow(uint32_t elem, uint32_t cap) {
  const size_t n = NumPages();
  auto* b = new uint8_t[(n + 1) * allocslab::kPageStride + 16]();
  uint8_t* old = *reinterpret_cast<uint8_t**>(g_mgr + 8);
  memcpy(b, old, n * allocslab::kPageStride);
  memset(old, 0xCD, n * allocslab::kPageStride);
  g_storage.push_back(b);
  SetVector(b, n + 1);
  P(static_cast<uint32_t>(n)) = Page{static_cast<uint32_t>(n), elem, cap / elem, cap, 0, 0, NewHeap(cap), nullptr};
}

// Slot 8 as 0xbdb0 does it: create a missing buffer, then update(0, data, used).
void __fastcall FakeUpload(void* mgr) {
  uint8_t* b = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(mgr) + 8);
  uint8_t* e = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(mgr) + 0x10);
  for (uint8_t* p = b; p < e; p += allocslab::kPageStride) {
    auto* pg = reinterpret_cast<Page*>(p);
    if (!pg->used) continue;
    if (!pg->gpuBuf) pg->gpuBuf = NewBuf(pg->capBytes);
    void** vt = *static_cast<void***>(pg->gpuBuf);
    reinterpret_cast<parupload::UpdateFn>(vt[2])(pg->gpuBuf, 0, pg->data, static_cast<int32_t>(pg->used));
  }
}

// What the culling writers do: allocate, then write the block through `data`.
uint8_t Pat(uint32_t page, uint32_t off) { return static_cast<uint8_t>(page * 37 + off * 13 + 5); }
void Write(uint32_t page, uint32_t from, uint32_t to) {
  Page& p = P(page);
  for (uint32_t o = from; o < to; ++o) p.data[o] = Pat(page, o);
  if (to > p.used) p.used = to;
}
bool GpuHas(uint32_t page, uint32_t from, uint32_t to) {
  const Gpu& g = g_gpu[static_cast<FakeSb*>(P(page).gpuBuf)->id];
  for (uint32_t o = from; o < to; ++o)
    if (g.mem[o] != Pat(page, o)) return false;
  return true;
}
int Id(uint32_t page) { return static_cast<FakeSb*>(P(page).gpuBuf)->id; }
bool NoDoubleMapAndAllUnmapped() {
  for (int i = 0; i < g_nSb; ++i)
    if (g_gpu[i].doubleMap || g_gpu[i].mapped || g_gpu[i].maps != g_gpu[i].unmaps) return false;
  return true;
}

// The per-frame reset (slot 7): prevUsed = used, used = 0 [V 0xbd90].
void Frame0() {
  for (uint32_t i = 0; i < NumPages(); ++i) {
    Page& p = P(i);
    if (p.used) p.prevUsed = p.used;
    p.used = 0;
  }
}

void Run() {
  auto savedLock = allocslab::g_lock, savedUnlock = allocslab::g_unlock;
  allocslab::g_lock = &FakeLock;
  allocslab::g_unlock = &FakeUnlock;
  g_vt[2] = reinterpret_cast<void*>(&parupload::Hook);
  g_vt[3] = reinterpret_cast<void*>(&FakeMap);
  g_vt[4] = reinterpret_cast<void*>(&FakeUnmap);
  parupload::g_orig = &FakeStockUpdate;
  parupload::g_pre = &du::Bypass;
  parupload::g_on = false;
  du::g_sbVtbl = g_vt;
  du::g_origUpload = &FakeUpload;
  du::g_disabled = false;
  du::g_shutdown = false;
  du::ResetCounters();

  // ---- 1. main path, restore by index after the vector reallocates ----
  {
    g_nSb = 0;
    Build({
        {0x60, 4096, 100, 0, true, 3},   // 0 mapped
        {0x70, 8192, 0, 0, true, 3},     // 1 no use last frame: stock
        {0x60, 4096, 50, 0, false, 3},   // 2 no GPU buffer yet: stock
        {0x60, 0, 10, 0, true, 3},       // 3 retired (capacity 0)
        {0x60, 4096, 10, 192, true, 3},  // 4 in use at the collect: copied, then mapped
        {0x60, 4096, 10, 0, true, 3},    // 5 mapped, unused this frame: unmapped at the end
        {0x60, 4096, 10, 0, true, 2},    // 6 not dynamic: skipped
    });
    Write(4, 0, 192);  // before the collect (heap)
    uint8_t* heaps[7];
    for (uint32_t i = 0; i < 7; ++i) heaps[i] = P(i).data;
    du::g_on = true;
    du::OnReset(g_mgr);
    const uint64_t gen0 = allocslab::g_gen.load();
    du::OnCollect();
    const uint64_t gen1 = allocslab::g_gen.load();
    bool ok = du::g_open.load() == 3 && P(0).data == g_gpu[Id(0)].mem.data() &&
              P(4).data == g_gpu[Id(4)].mem.data() && P(5).data == g_gpu[Id(5)].mem.data() && P(1).data == heaps[1] &&
              P(2).data == heaps[2] && P(3).data == heaps[3] && P(6).data == heaps[6] && gen1 > gen0 &&
              g_lockDepth == 0 && g_lockMax == 1;
    ok = ok && GpuHas(4, 0, 192);  // copied into the mapping
    Check(ok, "direct upload: first collect maps pages with buffer, capacity and previous use; in-use bytes copied");
    du::OnCollect();  // a second collect of the same parse maps nothing more
    ok = g_gpu[Id(0)].maps == 1 && !g_gpu[Id(0)].doubleMap;
    // Culling: writes into the mappings and the heaps, a new page appears and
    // the vector moves.
    Write(0, 0, 960);
    Write(4, 192, 384);
    Write(1, 0, 500);
    Grow(0x60, 4096);
    Write(7, 0, 96);
    Write(2, 0, 64);
    du::HookUpload(g_mgr);
    const uint64_t gen2 = allocslab::g_gen.load();
    ok = ok && GpuHas(0, 0, 960) && GpuHas(4, 0, 384) && GpuHas(1, 0, 500) && GpuHas(7, 0, 96) && GpuHas(2, 0, 64);
    ok = ok && P(0).data == heaps[0] && P(4).data == heaps[4] && P(5).data == heaps[5] && P(1).data == heaps[1];
    ok = ok && du::g_open.load() == 0 && gen2 > gen1 && NoDoubleMapAndAllUnmapped() && g_lockDepth == 0;
    ok = ok && du::g_c.bypassed.load() == 2 && du::g_c.idleUnmaps.load() == 1 && du::g_c.restores.load() == 3 &&
         du::g_c.fallback.load() == 3 && du::g_c.fallbackNoBuf.load() == 2 && g_stockCopies == 3 &&
         du::g_c.skipped.load() == 1 && du::g_c.copiedAtOpen.load() == 1 && du::g_c.anomalies.load() == 0;
    Check(ok, "direct upload: mapped pages only unmap at the upload, others memcpy; heap pointers restored by index "
              "after the page vector moved");
  }

  // ---- 2. a failed Map undoes the frame ----
  {
    du::ResetCounters();
    g_stockCopies = 0;
    Frame0();
    uint8_t* heaps[8];
    for (uint32_t i = 0; i < 8; ++i) heaps[i] = P(i).data;
    g_failMapId = Id(4);  // after pages 0, 1 and 2 are mapped
    du::OnReset(g_mgr);
    du::OnCollect();
    g_failMapId = -1;
    bool ok = du::g_open.load() == 0 && du::g_c.mapFailures.load() == 1 && du::g_c.stockFrames.load() == 1;
    for (uint32_t i = 0; i < 8; ++i) ok = ok && P(i).data == heaps[i];
    for (int i = 0; i < g_nSb; ++i) g_gpu[i].doubleMap = false;  // the failing fake reports the refused Map
    ok = ok && NoDoubleMapAndAllUnmapped() && g_lockDepth == 0;
    Write(0, 0, 192);
    Write(4, 0, 96);
    du::HookUpload(g_mgr);
    ok = ok && GpuHas(0, 0, 192) && GpuHas(4, 0, 96) && g_stockCopies == 2 && du::g_c.bypassed.load() == 0 &&
         NoDoubleMapAndAllUnmapped() && !du::g_disabled.load();
    Check(ok, "direct upload: a failed Map unmaps and restores everything; that frame uploads with the stock copy");
  }

  // ---- 3. upload never came: restored at the next reset ----
  {
    du::ResetCounters();
    Frame0();
    uint8_t* heaps[8];
    for (uint32_t i = 0; i < 8; ++i) heaps[i] = P(i).data;
    du::OnReset(g_mgr);
    du::OnCollect();
    bool ok = du::g_open.load() > 0;
    du::OnReset(g_mgr);
    ok = ok && du::g_open.load() == 0 && du::g_c.resetRestores.load() == 1 && NoDoubleMapAndAllUnmapped();
    for (uint32_t i = 0; i < 8; ++i) ok = ok && P(i).data == heaps[i];
    du::HookUpload(g_mgr);  // close that parse
    Check(ok, "direct upload: a window left open is restored at the next reset");
  }

  // ---- 4. off: nothing changes ----
  {
    du::ResetCounters();
    g_stockCopies = 0;
    Frame0();
    du::g_on = false;
    int maps0 = 0;
    for (int i = 0; i < g_nSb; ++i) maps0 += g_gpu[i].maps;
    uint8_t* heaps[8];
    for (uint32_t i = 0; i < 8; ++i) heaps[i] = P(i).data;
    du::OnReset(g_mgr);
    du::OnCollect();
    bool ok = du::g_open.load() == 0;
    for (uint32_t i = 0; i < 8; ++i) ok = ok && P(i).data == heaps[i];
    Write(0, 0, 96);
    du::HookUpload(g_mgr);
    int maps1 = 0;
    for (int i = 0; i < g_nSb; ++i) maps1 += g_gpu[i].maps;
    ok = ok && GpuHas(0, 0, 96) && g_stockCopies == 1 && maps1 == maps0 + 1 && du::g_c.bypassed.load() == 0;
    Check(ok, "direct upload: off maps nothing and leaves every upload to the stock path");
  }

  // ---- 5. chained with par_upload ----
  {
    du::ResetCounters();
    g_stockCopies = 0;
    g_nSb = 0;
    const uint32_t big = 1u << 20;
    Build({
        {0x60, 4096, 100, 0, true, 3},  // mapped
        {0x60, big, 0, 0, true, 3},     // not mapped, large: split copy
    });
    du::g_on = true;
    bool ok = parupload::StartHelpers();
    parupload::g_on = true;
    parupload::g_calls = 0;
    du::OnReset(g_mgr);
    du::OnCollect();
    Write(0, 0, 960);
    Write(1, 0, (600u << 10) + 32);
    du::HookUpload(g_mgr);
    ok = ok && GpuHas(0, 0, 960) && GpuHas(1, 0, (600u << 10) + 32) && du::g_c.bypassed.load() == 1 &&
         parupload::g_calls.load() == 1 && g_stockCopies == 0 && NoDoubleMapAndAllUnmapped();
    parupload::g_on = false;
    parupload::StopHelpers();
    Check(ok, "direct upload: one vt[2] hook: mapped pages bypass, large unmapped pages still get the parallel copy");
  }

  // ---- 6. sentinel verify ----
  {
    g_nSb = 0;
    auto setup = [] {
      Build({
          {0x60, 4096, 100, 0, true, 3},  // 0 mapped, 96-byte class
          {0x70, 8192, 100, 0, true, 3},  // 1 mapped, 112-byte class (padding 0x64-0x6f)
          {0x60, 4096, 0, 0, true, 3},    // 2 stock
      });
      du::ResetCounters();
      du::g_disabled = false;
    };
    auto block = [](uint32_t page, uint32_t size, uint32_t count, uint32_t index) {
      du::OnBlock(size, count, page, index);
    };
    // Clean frame: whole 0x60 elements, 0x70 elements written to 0x64 only.
    setup();
    bool ok = du::StartVerify();
    du::OnReset(g_mgr);
    du::OnCollect();
    const uint32_t* m0 = reinterpret_cast<const uint32_t*>(g_gpu[Id(0)].mem.data());
    const uint32_t* h1 = reinterpret_cast<const uint32_t*>(P(1).data == g_gpu[Id(1)].mem.data()
                                                               ? du::g_e[1].heap
                                                               : P(1).data);
    ok = ok && m0[0] == du::kSentMapped && m0[1023] == du::kSentMapped && h1[0] == du::kSentHeap &&
         h1[2047] == du::kSentHeap && du::g_vPhase.load() == 1;
    block(0, 0x60, 2, 0);
    Write(0, 0, 0xc0);
    block(1, 0x70, 1, 0);
    Write(1, 0, 0x64);
    block(1, 0x70, 2, 1);
    Write(1, 0x70, 0x70 + 0x64);
    Write(1, 0xe0, 0xe0 + 0x64);
    P(1).used = 0x150;
    block(2, 0x60, 1, 0);
    Write(2, 0, 0x50);  // a stock page: not checked
    du::HookUpload(g_mgr);
    ok = ok && du::VerifyPassed() && du::g_v.frames.load() == 1 && du::g_v.blocks.load() == 4 && !du::g_disabled;
    ok = ok && GpuHas(0, 0, 0xc0) && GpuHas(1, 0, 0x64);
    du::StopVerify();
    Check(ok, "direct upload verify: clean frame passes (0x70 padding allowed, stock pages ignored)");

    // (a) a block with an unwritten dword.
    setup();
    du::StartVerify();
    du::OnReset(g_mgr);
    du::OnCollect();
    block(0, 0x60, 1, 0);
    Write(0, 0, 0x50);
    P(0).used = 0x60;
    du::HookUpload(g_mgr);
    bool a = du::g_v.a.load() == 1 && du::g_disabled.load() && du::g_v.nOff.load() >= 1 && du::g_v.off[0].check == 'a' &&
             du::g_v.off[0].off == 0x50;
    du::StopVerify();
    // (b) a write through the stale heap pointer.
    setup();
    du::StartVerify();
    du::OnReset(g_mgr);
    du::OnCollect();
    block(0, 0x60, 1, 0);
    Write(0, 0, 0x60);
    du::g_e[0].heap[0x200] = 1;
    du::HookUpload(g_mgr);
    bool b = du::g_v.b.load() == 1 && du::g_v.a.load() == 0 && du::g_disabled.load();
    du::StopVerify();
    // (c) an allocation after the upload.
    setup();
    du::StartVerify();
    du::OnReset(g_mgr);
    du::OnCollect();
    block(0, 0x60, 1, 0);
    Write(0, 0, 0x60);
    du::HookUpload(g_mgr);
    const bool clean = du::VerifyPassed();
    block(0, 0x60, 1, 1);
    du::OnReset(g_mgr);  // the next parse: counted as a mismatch at its upload
    du::OnCollect();
    du::HookUpload(g_mgr);
    bool c = clean && du::g_v.c.load() == 1 && du::g_disabled.load();
    du::StopVerify();
    // Latched off: the next parse maps nothing.
    du::OnReset(g_mgr);
    du::OnCollect();
    const bool latched = du::g_open.load() == 0;
    du::HookUpload(g_mgr);
    Check(a && b && c && latched && NoDoubleMapAndAllUnmapped(),
          "direct upload verify: (a) unwritten dword, (b) stale heap write, (c) allocation outside the window each "
          "latch it off");
  }

  // ---- untraced element class: the 0xb0 pages stay on the memcpy ----
  {
    du::g_disabled = false;
    du::g_on = true;
    du::ResetCounters();
    g_stockCopies = 0;
    g_nSb = 0;
    Build({
        {0xb0, 4224, 100, 0, true, 3},  // 0 registration class: stock
        {0x60, 4096, 100, 0, true, 3},  // 1 mapped
    });
    uint8_t* h0 = P(0).data;
    du::OnReset(g_mgr);
    du::OnCollect();
    bool ok = du::g_open.load() == 1 && P(0).data == h0 && du::g_skClass.load() == 1;
    Write(0, 0, 176);
    Write(1, 0, 96);
    du::HookUpload(g_mgr);
    ok = ok && GpuHas(0, 0, 176) && GpuHas(1, 0, 96) && P(0).data == h0 && du::g_open.load() == 0 &&
         du::g_c.bypassed.load() == 1 && NoDoubleMapAndAllUnmapped();
    Check(ok, "direct upload: pages of an untraced element class (0xb0) keep the stock copy");
  }

  du::g_on = false;
  du::g_disabled = false;
  du::g_sbVtbl = nullptr;
  du::g_origUpload = nullptr;
  du::g_mgr = nullptr;
  du::ResetCounters();
  parupload::g_pre = nullptr;
  parupload::g_orig = nullptr;
  allocslab::g_blockObs = nullptr;
  allocslab::g_lock = savedLock;
  allocslab::g_unlock = savedUnlock;
}

}  // namespace dutest
