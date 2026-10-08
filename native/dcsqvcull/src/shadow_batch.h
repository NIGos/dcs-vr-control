// Shadow caster instancing, stage 2 (R12): one instanced draw per group of
// identical casters in each cascade pass.
//
// Group key, per cascade pass execution (R12 2.2, measured in inst_count.h):
// ModelMaterialMT material, mesh, sbPositions page [item+0xd0], and the
// texture entries for casters that bind textures. Within a group only the
// per-object element offset [item+0xd4] differs; the material constant
// buffer bytes are otherwise identical (1.87M live comparisons, 0 differ).
//
// Flow (render thread):
//  1. Cascade execute wrapper (shadow_pass.h g_wrap), before the original:
//     plan the groups from the pass's caster vector; write every group's
//     [item+0xd4] values, in original order, into our structured buffer.
//  2. The original loop calls ShadowMapRenderable vt[1] per caster
//     (inst_count.h g_override):
//       solo   -> original call;
//       member -> skipped (the loop ignores the result [V GC 0xa5640]);
//       leader -> original call with: [item+0xd4] = the group's base index
//                 into our buffer, the effect pass's VS block pointing at our
//                 instanced variant of DCS's own shadow VS (shadow_inst.h),
//                 our buffer at VS t127, and DX11Renderer::draw's instance
//                 count argument = group size (a6, [V dx11backend 0x14bcb]:
//                 non-zero gives DrawIndexedInstanced(count, N, start, 0, 0)
//                 instead of DrawIndexed(count, start, 0)).
//     Our VS computes posStructOffset = qvInstOffsets[qvPsoBase + id]: the
//     same value each member would have used.
//  3. After the original: every touched material's mat+0x18c is set to the
//     value the stock loop would have left (last caster in original order).
// Depth-only output with GREATER and no blending does not depend on order.
//
// Verification ([Suite] ShadowInstVerify): each cascade pass runs twice in
// the same frame, stock then batched (each run clears depth itself), and the
// two depth targets are compared bit for bit; any mismatch turns batching off.
//
// The instance count reaches DX11Renderer::draw through a copy of the
// renderer's vtable, swapped into the renderer object only for the leader's
// call and restored right after.
// Included once from main.cpp inside its anonymous namespace, after
// inst_count.h, shadow_pass.h and shadow_inst.h.
#pragma once

namespace shadowbatch {

constexpr uint32_t kSmrVtbl = 0x59828;      // NGModel ShadowMapRenderable
constexpr uint32_t kModelMatVtbl = 0x592f0; // NGModel ModelMaterialMT
constexpr uint32_t kRendererVtbl = 0xb2a60; // dx11backend DX11Renderer
constexpr uint32_t kDrawRva = 0x14870;      // DX11Renderer::draw (slot 35)
constexpr int kDrawSlot = 35;
constexpr int kVtblCopy = 92;  // DX11Renderer vtable entries in this build (entry 92 is string data)
constexpr uint32_t kMaxOffsets = 1 << 16;
constexpr size_t kTable = 1 << 14;  // casters per pass (power of two)

std::atomic<bool> g_on{false};
std::atomic<bool> g_verify{false};
std::atomic<int> g_state{0};  // 0 = not tried, 1 = ready, -1 = unavailable
// Latched off for the session on any fault or verification mismatch.
std::atomic<bool> g_disabled{false};
std::atomic<bool> g_shutdown{false};
std::atomic<int> g_inWrap{0};
void Disable(const char* why) {
  if (!g_disabled.exchange(true)) Log("shadow batching: disabled for this session (%s)", why);
}

uint8_t* g_ng = nullptr;
void* g_smrVtbl = nullptr;
void* g_modelMatVtbl = nullptr;
void*** g_rendererApi = nullptr;  // renderer.dll RenderAPI::pRenderApi
void** g_rendererObj = nullptr;  // the DX11Renderer instance
void** g_rendererVtbl = nullptr;
void* g_myTable[kVtblCopy + 1];
void** g_myVtbl = nullptr;
using DrawFn = void(__fastcall*)(void* self, int a1, void* shader, int prim, int a4, int a5, int instances,
                                 const char* name);
DrawFn g_origDraw = nullptr;
thread_local uint32_t t_instances = 0;

ID3D11DeviceContext* g_ctx = nullptr;
ID3D11Buffer* g_buf = nullptr;
ID3D11ShaderResourceView* g_srv = nullptr;
DWORD g_renderThread = 0;

// Counters (render thread).
std::atomic<uint64_t> g_passes{0}, g_casters{0}, g_groups{0}, g_leaders{0}, g_skipped{0}, g_fallbacks{0}, g_tooLarge{0};
std::atomic<uint64_t> g_verifyPasses{0}, g_verifyMismatch{0}, g_verifyPixels{0}, g_verifyBadPixels{0}, g_verifyErrors{0};

// Optional draw-time check (gb_batch.h): runs inside the leader's call just
// before DX11Renderer::draw, after every bind of that draw; false = draw the
// leader alone (DCS's own instance count), its members then draw themselves.
using DrawCheckFn = bool (*)(void* arg);
thread_local DrawCheckFn t_check = nullptr;
thread_local void* t_checkArg = nullptr;

void __fastcall DrawN(void* self, int a1, void* shader, int prim, int a4, int a5, int instances, const char* name) {
  uint32_t n = t_instances;
  if (n && t_check && !t_check(t_checkArg)) n = 0;
  g_origDraw(self, a1, shader, prim, a4, a5, n ? static_cast<int>(n) : instances, name);
}

// ---- Plan (per cascade pass execution) ----
enum : uint8_t { kSolo = 0, kLeader = 1, kMember = 2 };
struct Slot {  // by renderable pointer
  void* r;
  uint32_t gen;
  uint8_t role;
  uint32_t base, count, group;
};
struct GroupSlot {  // by key
  uint32_t gen;
  void* mat;
  void* mesh;
  uint32_t page;
  uint64_t tex;
  int32_t first, last;  // caster indices, chained through g_next
  uint32_t count;
};
Slot g_slots[kTable];  // by caster index (this pass)
void** g_begin = nullptr;  // the planned caster vector
size_t g_n = 0, g_cursor = 0;  // next caster index the pass loop should reach
GroupSlot g_gslots[kTable];
int32_t g_next[kTable];
uint32_t g_gen = 0;
uint32_t g_offsets[kMaxOffsets];
uint8_t g_groupFailed[kTable];  // by group slot: the leader fell back, members draw themselves
struct MatLast {
  uint8_t* mat;
  uint32_t pso;
  uint32_t gen;
  int8_t eligible;  // -1 = not checked this pass; 0/1 = our VS exists and <= 32 textures
  int8_t textured;  // binds textures in the shadow path (props, not per item)
};
Slot* g_slotOf[kTable];      // caster index -> its slot (this pass)
uint32_t g_groupList[kTable];  // group slots created this pass, in first-seen order
uint32_t g_groupCount = 0;
MatLast g_mats[kTable];       // by material pointer
uint32_t g_matList[kTable];   // used slots, in first-seen order
uint32_t g_matCount = 0;
bool g_planActive = false;
bool g_batching = false;  // current run of the pass uses the plan
// Verification: called with true before the stock run and false before the
// batched run, so other caster-path optimizations (the shadow texture skip)
// are off in the reference and on in the compared run; nullptr = unused.
void (*g_verifyStock)(bool stock) = nullptr;
bool g_capture = false;   // verification run: capture the depth target at the first caster
ID3D11Resource* g_depth = nullptr;  // verification: depth target seen at the first caster

inline size_t PtrHash(const void* p) {
  uint64_t h = reinterpret_cast<uint64_t>(p) * 0x9E3779B97F4A7C15ull;
  return static_cast<size_t>(h >> 50) & (kTable - 1);
}

// The pass loop calls vt[1] for its casters in vector order [V GC 0xa5640];
// only ShadowMapRenderable entries reach our hook, so the cursor moves
// forward to `r` (skipping other classes). Not found = this caster was not
// planned (treated as solo; a member already skipped was drawn by its leader,
// and drawing a planned member again alone writes the same depth).
Slot* FindSlot(void* r) {
  for (size_t k = g_cursor; k < g_n; ++k) {
    if (g_begin[k] == r) {
      g_cursor = k + 1;
      return &g_slots[k];
    }
  }
  return nullptr;
}

// Technique handle the shadow submit will use, and our VS for it.
uint64_t TechOf(uint8_t* mat) {
  auto* props = *reinterpret_cast<uint8_t**>(mat + 0x28);
  return *reinterpret_cast<uint64_t*>(mat + (props[0x33] ? 0x218 : 0x210));
}

// Builds the plan. Returns false (plan inactive) on anything unexpected.
bool PlanRaw(void** vec) {
  auto** begin = static_cast<void**>(vec[0]);
  auto** end = static_cast<void**>(vec[1]);
  const size_t n = static_cast<size_t>(end - begin);
  if (!begin || n == 0) return false;
  if (n > kTable / 2) {
    g_tooLarge++;
    return false;
  }
  g_begin = reinterpret_cast<void**>(begin);
  g_n = n;
  g_cursor = 0;
  if (++g_gen == 0) {
    memset(g_slots, 0, sizeof(g_slots));
    memset(g_gslots, 0, sizeof(g_gslots));
    memset(g_mats, 0, sizeof(g_mats));
    g_gen = 1;
  }
  g_matCount = 0;
  g_groupCount = 0;
  uint32_t groups = 0;
  for (size_t i = 0; i < n; ++i) {
    void* r = begin[i];
    Slot* s = &g_slots[i];
    *s = {r, g_gen, kSolo, 0, 0, 0};
    g_slotOf[i] = s;
    g_next[i] = -1;
    if (*static_cast<void**>(r) != g_smrVtbl) continue;
    auto* item = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(r) + 0x10);
    if (!item) continue;
    auto* mat = *reinterpret_cast<uint8_t**>(item + 0x10);
    if (!mat || *reinterpret_cast<void**>(mat) != g_modelMatVtbl) continue;
    // Last element offset per material in original order (for step 3), and
    // the per-material facts (checked once per pass).
    MatLast* ml = nullptr;
    {
      size_t mi = PtrHash(mat);
      for (size_t k = 0;; ++k, mi = (mi + 1) & (kTable - 1)) {
        if (k == kTable) return false;
        MatLast& m = g_mats[mi];
        if (m.gen != g_gen) {
          m = {mat, *reinterpret_cast<uint32_t*>(item + 0xd4), g_gen, -1, 0};
          g_matList[g_matCount++] = static_cast<uint32_t>(mi);
          ml = &m;
          break;
        }
        if (m.mat == mat) {
          m.pso = *reinterpret_cast<uint32_t*>(item + 0xd4);
          ml = &m;
          break;
        }
      }
    }
    if (ml->eligible < 0) {
      void* shader = *reinterpret_cast<void**>(mat + 0x30);
      ml->eligible = shader && *reinterpret_cast<uint32_t*>(mat + 0x2d8) <= 32 &&
                             shadowinst::FindVs(shader, TechOf(mat), 0) != nullptr
                         ? 1
                         : 0;
      ml->textured = ml->eligible ? (instcount::IsTextured(mat) ? 1 : 0) : 0;
    }
    if (!ml->eligible) continue;
    void* mesh = *reinterpret_cast<void**>(item + 0xc0);
    const uint32_t page = *reinterpret_cast<uint32_t*>(item + 0xd0);
    const uint64_t tex = ml->textured ? instcount::TextureEntriesKey(mat, item) : 0;
    uint64_t h = reinterpret_cast<uint64_t>(mat) * 0x9E3779B97F4A7C15ull ^
                 reinterpret_cast<uint64_t>(mesh) * 0xC2B2AE3D27D4EB4Full ^ (page + tex) * 0x165667B19E3779F9ull;
    size_t gi = static_cast<size_t>(h >> 50) & (kTable - 1);
    for (size_t k = 0;; ++k, gi = (gi + 1) & (kTable - 1)) {
      if (k == kTable) return false;
      GroupSlot& g = g_gslots[gi];
      if (g.gen != g_gen) {
        g = {g_gen, mat, mesh, page, tex, static_cast<int32_t>(i), static_cast<int32_t>(i), 1};
        g_groupList[g_groupCount++] = static_cast<uint32_t>(gi);
        ++groups;
        break;
      }
      if (g.mat == mat && g.mesh == mesh && g.page == page && g.tex == tex) {
        g_next[g.last] = static_cast<int32_t>(i);
        g.last = static_cast<int32_t>(i);
        ++g.count;
        break;
      }
    }
  }
  // Assign roles and offsets.
  uint32_t cursor = 0, leaders = 0, skipped = 0;
  for (uint32_t li = 0; li < g_groupCount; ++li) {
    const uint32_t gi = g_groupList[li];
    GroupSlot& g = g_gslots[gi];
    if (g.count < 2) continue;
    if (cursor + g.count > kMaxOffsets) break;
    const uint32_t base = cursor;
    g_groupFailed[gi] = 0;
    for (int32_t i = g.first; i >= 0; i = g_next[i]) {
      auto* item = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(begin[i]) + 0x10);
      g_offsets[cursor++] = *reinterpret_cast<uint32_t*>(item + 0xd4);
      Slot* s = g_slotOf[i];
      s->role = i == g.first ? kLeader : kMember;
      s->base = base;
      s->count = g.count;
      s->group = static_cast<uint32_t>(gi);
    }
    ++leaders;
    skipped += g.count - 1;
  }
  if (cursor == 0) return false;
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(g_ctx->Map(g_buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
  memcpy(m.pData, g_offsets, cursor * sizeof(uint32_t));
  g_ctx->Unmap(g_buf, 0);
  g_casters += n;
  g_groups += groups;
  g_leaders += leaders;
  g_skipped += skipped;
  return true;
}

bool PlanGuarded(void** vec) {
  __try {
    return PlanRaw(vec);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void RestoreMats() {
  for (uint32_t i = 0; i < g_matCount; ++i) {
    const MatLast& m = g_mats[g_matList[i]];
    *reinterpret_cast<uint32_t*>(m.mat + 0x18c) = m.pso;
  }
}

// ---- Leader draw ----
// Draws the whole group with one instanced draw. Returns false when it fell
// back before touching anything (the caller then draws the leader alone and
// the members draw themselves). Every change to DCS state is made inside the
// __try and undone in the __finally. Plain C (no unwinding objects).
bool LeaderRaw(void* self, void* ctx, const Slot& s, uint64_t* ret) {
  auto* item = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(self) + 0x10);
  auto* mat = *reinterpret_cast<uint8_t**>(item + 0x10);
  auto* shader = *reinterpret_cast<uint8_t**>(mat + 0x30);
  const uint64_t tech = TechOf(mat);
  void* expectVs = nullptr;
  ID3D11VertexShader* ours = shadowinst::FindVsEx(shader, tech, 0, &expectVs);
  if (!ours || !expectVs || !g_ctx || !g_srv) return false;
  if (*g_rendererApi != static_cast<void*>(g_rendererObj) || *g_rendererObj != static_cast<void*>(g_rendererVtbl))
    return false;
  auto* techBegin = *reinterpret_cast<uint8_t**>(shader + 0xb0);
  auto* techObj = *reinterpret_cast<uint8_t**>(techBegin + (tech - 1) * 0x50 + 0x20);
  using PassFn = uint8_t*(__fastcall*)(void*, uint32_t);
  auto* fxpass = (*reinterpret_cast<PassFn**>(techObj))[7](techObj, 0);
  auto* vsBlock = *reinterpret_cast<uint8_t**>(fxpass + 0xc0);
  auto* dcsVs = *reinterpret_cast<ID3D11VertexShader**>(vsBlock + 0x18);
  if (static_cast<void*>(dcsVs) != expectVs) return false;  // not the VS our variant was built from
  const uint32_t pso = *reinterpret_cast<uint32_t*>(item + 0xd4);
  bool psoSet = false, vsSet = false, srvSet = false, vtblSet = false;
  __try {
    *reinterpret_cast<uint32_t*>(item + 0xd4) = s.base;
    psoSet = true;
    *reinterpret_cast<ID3D11VertexShader**>(vsBlock + 0x18) = ours;
    vsSet = true;
    g_ctx->VSSetShaderResources(127, 1, &g_srv);
    srvSet = true;
    t_instances = s.count;
    *g_rendererObj = g_myVtbl;
    vtblSet = true;
    *ret = instcount::g_orig(self, ctx);
  } __finally {
    if (vtblSet) *g_rendererObj = g_rendererVtbl;
    t_instances = 0;
    if (vsSet) {
      *reinterpret_cast<ID3D11VertexShader**>(vsBlock + 0x18) = dcsVs;
      g_ctx->VSSetShader(dcsVs, nullptr, 0);
    }
    if (srvSet) {
      ID3D11ShaderResourceView* none = nullptr;
      g_ctx->VSSetShaderResources(127, 1, &none);
    }
    if (psoSet) *reinterpret_cast<uint32_t*>(item + 0xd4) = pso;
  }
  return true;
}

int AvFilter(unsigned long code) {
  return code == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

void CaptureDepth() {
  if (!g_capture || g_depth) return;
  ID3D11DepthStencilView* dsv = nullptr;
  g_ctx->OMGetRenderTargets(0, nullptr, &dsv);
  if (dsv) {
    dsv->GetResource(&g_depth);
    dsv->Release();
  }
}

bool Override(void* self, void* ctx, uint64_t* ret) {
  if (GetCurrentThreadId() != g_renderThread || !g_planActive) return false;
  CaptureDepth();
  if (!g_batching) return false;
  Slot* s = FindSlot(self);
  if (!s || s->role == kSolo) return false;
  if (s->role == kMember) {
    if (g_groupFailed[s->group]) return false;  // the leader drew alone: draw this one too
    *ret = 0;
    return true;
  }
  bool done = false;
  __try {
    done = LeaderRaw(self, ctx, *s, ret);
  } __except (AvFilter(GetExceptionCode())) {
    done = false;
    Disable("access violation in the instanced leader draw");
  }
  if (done) return true;
  g_fallbacks++;
  g_groupFailed[s->group] = 1;
  return false;  // the original draws the leader alone; the members follow
}

// ---- Verification ----
ID3D11Texture2D* CopyToStaging(ID3D11Resource* res) {
  ID3D11Texture2D* tex = nullptr;
  if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) return nullptr;
  D3D11_TEXTURE2D_DESC d;
  tex->GetDesc(&d);
  tex->Release();
  if (d.SampleDesc.Count != 1) return nullptr;
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  d.MiscFlags = 0;
  ID3D11Device* dev = nullptr;
  g_ctx->GetDevice(&dev);
  ID3D11Texture2D* st = nullptr;
  if (dev) {
    dev->CreateTexture2D(&d, nullptr, &st);
    dev->Release();
  }
  if (st) g_ctx->CopyResource(st, res);
  return st;
}

// Bytes per texel of the depth formats a shadow map can use; 0 = unknown.
UINT TexelBytes(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_FLOAT:
      return 2;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
      return 4;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
      return 8;
    default:
      return 0;
  }
}

DXGI_FORMAT g_lastFormat = DXGI_FORMAT_UNKNOWN;

// Compares two staging copies texel by texel (mip 0 of every slice);
// returns mismatching texels, or -1 on error or an unknown format.
int64_t Compare(ID3D11Texture2D* a, ID3D11Texture2D* b, uint64_t* texels) {
  D3D11_TEXTURE2D_DESC d;
  a->GetDesc(&d);
  g_lastFormat = d.Format;
  const UINT bpt = TexelBytes(d.Format);
  if (bpt == 0) return -1;
  const size_t rowBytes = static_cast<size_t>(d.Width) * bpt;
  int64_t bad = 0;
  for (UINT slice = 0; slice < d.ArraySize; ++slice) {
    const UINT sub = D3D11CalcSubresource(0, slice, d.MipLevels);
    D3D11_MAPPED_SUBRESOURCE ma, mb;
    if (FAILED(g_ctx->Map(a, sub, D3D11_MAP_READ, 0, &ma))) return -1;
    if (FAILED(g_ctx->Map(b, sub, D3D11_MAP_READ, 0, &mb))) {
      g_ctx->Unmap(a, sub);
      return -1;
    }
    if (ma.RowPitch < rowBytes || mb.RowPitch < rowBytes) {
      g_ctx->Unmap(b, sub);
      g_ctx->Unmap(a, sub);
      return -1;
    }
    for (UINT y = 0; y < d.Height; ++y) {
      const auto* ra = static_cast<const uint8_t*>(ma.pData) + static_cast<size_t>(y) * ma.RowPitch;
      const auto* rb = static_cast<const uint8_t*>(mb.pData) + static_cast<size_t>(y) * mb.RowPitch;
      if (memcmp(ra, rb, rowBytes) == 0) continue;
      for (UINT x = 0; x < d.Width; ++x) bad += memcmp(ra + x * bpt, rb + x * bpt, bpt) != 0;
    }
    *texels += static_cast<uint64_t>(d.Width) * d.Height;
    g_ctx->Unmap(b, sub);
    g_ctx->Unmap(a, sub);
  }
  return bad;
}

int64_t CompareGuarded(ID3D11Texture2D* a, ID3D11Texture2D* b, uint64_t* texels) {
  __try {
    return Compare(a, b, texels);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}

void WrapInner(void* pass, void* ctx, shadowpass::ExecFn orig);

void Wrap(void* pass, void* ctx, shadowpass::ExecFn orig) {
  g_inWrap.fetch_add(1);
  if (g_shutdown.load() || g_disabled.load(std::memory_order_relaxed) || !g_on.load(std::memory_order_relaxed) ||
      g_state.load() != 1)
    orig(pass, ctx);
  else
    WrapInner(pass, ctx, orig);
  g_inWrap.fetch_sub(1);
}

void WrapInner(void* pass, void* ctx, shadowpass::ExecFn orig) {
  if (!g_renderThread) g_renderThread = GetCurrentThreadId();
  if (GetCurrentThreadId() != g_renderThread) return orig(pass, ctx);
  void** vec = shadowpass::CasterVector(pass, ctx);
  if (!vec || !PlanGuarded(vec)) return orig(pass, ctx);
  g_passes++;
  g_planActive = true;
  if (g_verify.load(std::memory_order_relaxed)) {
    // Stock run, copy; batched run, copy; compare.
    g_batching = false;
    g_capture = true;
    if (g_verifyStock) g_verifyStock(true);
    g_cursor = 0;
    orig(pass, ctx);
    g_capture = false;
    RestoreMats();
    ID3D11Texture2D* a = g_depth ? CopyToStaging(g_depth) : nullptr;
    if (g_depth) {
      g_depth->Release();
      g_depth = nullptr;
    }
    g_batching = true;
    g_capture = true;
    if (g_verifyStock) g_verifyStock(false);
    g_cursor = 0;
    orig(pass, ctx);
    g_capture = false;
    RestoreMats();
    ID3D11Texture2D* b = g_depth ? CopyToStaging(g_depth) : nullptr;
    if (g_depth) {
      g_depth->Release();
      g_depth = nullptr;
    }
    if (a && b) {
      uint64_t texels = 0;
      const int64_t bad = CompareGuarded(a, b, &texels);
      if (bad < 0) g_verifyErrors++;
      g_verifyPasses++;
      g_verifyPixels += texels;
      if (bad > 0) {
        g_verifyMismatch++;
        g_verifyBadPixels += static_cast<uint64_t>(bad);
        Disable("depth mismatch between the stock and the batched cascade");
      }
    }
    if (a) a->Release();
    if (b) b->Release();
  } else {
    g_batching = true;
    g_cursor = 0;
    orig(pass, ctx);
    RestoreMats();
  }
  g_batching = false;
  g_planActive = false;
}

// The renderer vtable copy with DrawN at slot 35, shared by shadow and
// G-buffer batching (gb_batch.h). 1 = ready, -1 = this build does not match.
int g_rendererState = 0;
bool InstallRenderer() {
  if (g_rendererState != 0) return g_rendererState > 0;
  auto* dx = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"dx11backend.dll"));
  HMODULE rmod = GetModuleHandleW(L"renderer.dll");
  if (!dx || !rmod) return false;  // retried later
  auto** api = reinterpret_cast<void***>(GetProcAddress(rmod, "?pRenderApi@RenderAPI@@3PEAUIRenderAPI@1@EA"));
  if (!api || !*api) return false;
  g_rendererState = -1;
  auto** obj = reinterpret_cast<void**>(*api);
  auto** vtbl = static_cast<void**>(*obj);
  if (reinterpret_cast<uint8_t*>(vtbl) != dx + kRendererVtbl || SlotOriginal(&vtbl[kDrawSlot]) != dx + kDrawRva) {
    Log("batching: DX11Renderer does not match this build; skipped");
    return false;
  }
  g_rendererApi = api;
  g_rendererObj = obj;
  g_rendererVtbl = vtbl;
  g_origDraw = reinterpret_cast<DrawFn>(SlotOriginal(&g_rendererVtbl[kDrawSlot]));
  g_myTable[0] = g_rendererVtbl[-1];  // RTTI locator
  memcpy(g_myTable + 1, g_rendererVtbl, kVtblCopy * sizeof(void*));
  g_myVtbl = g_myTable + 1;
  g_myVtbl[kDrawSlot] = reinterpret_cast<void*>(&DrawN);
  g_rendererState = 1;
  return true;
}

// Dynamic structured buffer of uint offsets (t127) and its view.
bool CreateOffsetBuffer(ID3D11Device* dev, uint32_t count, ID3D11Buffer** buf, ID3D11ShaderResourceView** srv) {
  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = count * sizeof(uint32_t);
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  bd.StructureByteStride = sizeof(uint32_t);
  if (FAILED(dev->CreateBuffer(&bd, nullptr, buf))) return false;
  D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
  sd.Format = DXGI_FORMAT_UNKNOWN;
  sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
  sd.Buffer.FirstElement = 0;
  sd.Buffer.NumElements = count;
  if (FAILED(dev->CreateShaderResourceView(*buf, &sd, srv))) {
    (*buf)->Release();
    *buf = nullptr;
    return false;
  }
  return true;
}

bool Install() {
  if (g_state.load() != 0) return g_state.load() > 0;
  if (shadowinst::g_state.load() != 1 || !shadowinst::g_device) return false;  // not yet
  if (!instcount::Install()) return false;
  shadowpass::Install();
  if (!shadowpass::g_orig) return false;
  if (!InstallRenderer()) {
    if (g_rendererState < 0) g_state = -1;
    return false;
  }
  g_state = -1;
  g_ng = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"NGModel.dll"));
  if (!g_ng) return false;
  g_smrVtbl = g_ng + kSmrVtbl;
  g_modelMatVtbl = g_ng + kModelMatVtbl;
  shadowinst::g_device->GetImmediateContext(&g_ctx);
  if (!CreateOffsetBuffer(shadowinst::g_device, kMaxOffsets, &g_buf, &g_srv)) return false;
  instcount::g_override = &Override;
  shadowpass::g_wrap = &Wrap;
  g_state = 1;
  Log("shadow batching: ready (cascade execute and caster hooks, DX11Renderer instanced draw)");
  return true;
}

void Shutdown() {
  g_shutdown = true;
  g_on = false;
  shadowpass::g_wrap = nullptr;
  instcount::g_override = nullptr;
  // Wait for a pass in flight (a verification pass can take a while).
  for (int i = 0; i < 400 && g_inWrap.load() != 0; ++i) Sleep(5);
  if (g_inWrap.load() != 0) {
    Log("shadow batching: a pass was still running at unload; its objects are left alive");
    return;
  }
  if (g_srv) g_srv->Release();
  if (g_buf) g_buf->Release();
  if (g_ctx) g_ctx->Release();
  g_srv = nullptr;
  g_buf = nullptr;
  g_ctx = nullptr;
}

void LogCounters(const char* label) {
  const uint64_t p = g_passes.load();
  Log("  shadow batching %s: %llu passes, casters %llu, groups %llu, instanced draws %llu, casters skipped %llu, "
      "fallbacks %llu, oversized passes %llu%s; verify: %llu passes compared, %llu with differences, %llu of %llu texels differ, "
      "%llu compare errors (format %d)",
      label, static_cast<unsigned long long>(p), static_cast<unsigned long long>(g_casters.load()),
      static_cast<unsigned long long>(g_groups.load()), static_cast<unsigned long long>(g_leaders.load()),
      static_cast<unsigned long long>(g_skipped.load()), static_cast<unsigned long long>(g_fallbacks.load()),
      static_cast<unsigned long long>(g_tooLarge.load()), g_disabled.load() ? ", DISABLED" : "",
      static_cast<unsigned long long>(g_verifyPasses.load()), static_cast<unsigned long long>(g_verifyMismatch.load()),
      static_cast<unsigned long long>(g_verifyBadPixels.load()), static_cast<unsigned long long>(g_verifyPixels.load()),
      static_cast<unsigned long long>(g_verifyErrors.load()), static_cast<int>(g_lastFormat));
}

void ResetCounters() {
  g_passes = g_casters = g_groups = g_leaders = g_skipped = 0;
  g_verifyPasses = g_verifyMismatch = g_verifyPixels = g_verifyBadPixels = g_verifyErrors = 0;
}

}  // namespace shadowbatch
