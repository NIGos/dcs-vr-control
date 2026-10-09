// FX pass Apply counter (R14 lead 4, measurement only; no behaviour change).
//
// Every FX pass Apply in dx11backend goes through SPassBlock vtable slot 13
// (vtable RVA 0xb96b0, slot address 0xb9718 -> 0x65850). 0x65850 stores the
// context in effect+0x158 and calls ApplyPassBlock 0x67f90(effect, pass,
// rsIndex = 4th argument); 0x67f90 has no other caller [V]. 0x67f90 runs the
// pass assignments (0x67e10), then:
//   blend block  pass+0x58 -> 0x68240, OMSetBlendState(pass+0x20 = [blk+0x18], pass+0x28 factor, [pass+0x38] mask)
//   depth block  pass+0x60 -> 0x68240, OMSetDepthStencilState(pass+0x40 = [blk+0x18], [pass+0x48] ref)
//   raster block pass+0x68 -> 0x68240, RSSetState([blk + rsIndex*8 + 0x18])
//   RT block     pass+0x78 (array of RTV**, count pass+0x70, DSV** pass+0xb8) -> OMSetRenderTargets
//   0x68470 ApplyShaderBlock(effect, block, stage) for VS +0xc0 (0), PS +0xc8 (4), GS +0xd0 (1),
//   HS +0xe8 (2), DS +0xe0 (3), CS +0xd8 (5) [V 0x67f90-0x68233].
// Shader block layout used by 0x68470 [V 0x685cd-0x688e0]:
//   +0x08 per-stage call table, +0x18 shader object (SetShader),
//   +0x20/+0x28 CB dependencies (0x20 each: u32 start, u32 count, SConstantBuffer** +8, ID3D11Buffer** +0x10),
//   +0x30/+0x38 sampler dependencies (same shape, ID3D11SamplerState** +0x10),
//   +0x50/+0x58 resource dependencies (same shape, ID3D11ShaderResourceView** +0x10; one
//   setShaderResources call per dependency), +0x60 UAV count, +0x70/+0x78 tbuffers (SConstantBuffer*).
// SConstantBuffer: +0x10 ID3D11Buffer, +0x20 backing bytes, +0x28 size, +0x5c flags
// (bit 0 dirty -> UpdateSubresource at Apply unless bit 7, user-managed) [V 0x68628-0x68665].
//
// While [Suite] FxApplyCount runs (5 s), the slot is wrapped and, on the render
// thread only, each Apply is counted: same pass as the previous Apply, whether
// the state objects and shaders it bound are identical to the previous Apply's,
// rdtsc inside Apply, and per stage how many CB / sampler / SRV slots and CB
// contents changed between consecutive same-pass Applies. A least-squares cost
// model over all Applies splits Apply time into the parts a fast path would
// keep (SRV binds, dirty CB uploads, changed CB/sampler sets) and the rest,
// which gives the estimate. The slot is restored afterwards.
// Included once from main.cpp inside its anonymous namespace, after shadow_tex.h
// (shadowtex::CodeIs) and pass_timing.h (ptiming::g_topTid).
#pragma once

namespace fxapply {

constexpr uint32_t kPassVtbl = 0xb96b0;  // .?AUSPassBlock@D3DX11Effects@@
constexpr int kSlot = 13;                // slot address 0xb9718
constexpr uint32_t kApply = 0x65850;

struct CodeRange {
  uint32_t begin, end;
  uint64_t hash;  // FNV-1a 64 of the bytes (shadowtex::CodeIs)
};
// Apply, ApplyPassBlock, ApplyRenderStateBlock and ApplyShaderBlock (three
// parts; CodeIs reads at most 512 bytes): every offset read below comes from these.
constexpr CodeRange kCode[] = {
    {0x65850, 0x65893, 0x82ae1bb9caeeadf9ull}, {0x67f90, 0x68234, 0xc787fe71c10919ffull},
    {0x68240, 0x68463, 0x08d547beb0aed40bull}, {0x68470, 0x68670, 0x31af43e49493ce58ull},
    {0x68670, 0x68870, 0xdcbb1f94e1856f67ull}, {0x68870, 0x68918, 0x9b5985eebe0ad652ull},
};

using Fn = uint64_t(__fastcall*)(uint8_t* pass, uint64_t flags, void* ctx, uint64_t rsIndex);
Fn g_orig = nullptr;
void** g_slot = nullptr;
std::atomic<bool> g_on{false};
DWORD g_thread = 0;  // render thread (set before g_on)
std::atomic<uint64_t> g_otherCalls{0};

constexpr int kStages = 6;
const char* const kStageNames[kStages] = {"VS", "GS", "HS", "DS", "PS", "CS"};
constexpr uint32_t kStageOff[kStages] = {0xc0, 0xd0, 0xe8, 0xe0, 0xc8, 0xd8};
constexpr int kApplyOrder[kStages] = {0, 4, 1, 2, 3, 5};  // order 0x67f90 applies them (CB upload attribution)
constexpr int kMaxCb = 16, kMaxSamp = 16, kMaxSrv = 64, kMaxRt = 8;

struct StageState {
  void* block;
  void* shader;
  uint32_t nCbDeps, nSampDeps, nSrvDeps, nCb, nSamp, nSrv, nTb, nUav;
  bool overflow;  // more slots than recorded: compared as changed
  void* cb[kMaxCb];
  void* samp[kMaxSamp];
  void* srv[kMaxSrv];
  uint8_t cbDep[kMaxCb], sampDep[kMaxSamp];  // dependency index of each recorded slot
};
struct PassState {
  uint8_t* pass;
  uint32_t rs;
  void* blend;
  float blendFactor[4];
  uint32_t sampleMask;
  void* depth;
  uint32_t stencilRef;
  void* raster;
  uint32_t nRt;
  void* rtv[kMaxRt];
  void* dsv;
  bool valid;
  StageState st[kStages];
};

struct StageCnt {
  uint64_t present = 0;          // same-pass Applies with this stage
  uint64_t blockChanged = 0;     // shader block or shader object differs from the previous Apply
  uint64_t anyChange = 0;        // Applies with any change in this stage
  uint64_t cbSlots = 0, cbChanged = 0, cbDepsChanged = 0;
  uint64_t sampSlots = 0, sampChanged = 0, sampDepsChanged = 0;
  uint64_t srvSlots = 0, srvChanged = 0, srvDepsChanged = 0, srvDeps = 0;
  uint64_t uploads = 0, uploadsChanged = 0;  // dirty FX CBs uploaded; of those, bytes differ from the last upload
};

// Least-squares cost model: cycles ~ b0 + b1*stages + b2*srvDeps + b3*uploads + b4*cbDeps + b5*sampDeps.
constexpr int kFeat = 6;
const char* const kFeatNames[kFeat] = {"base", "per stage", "per SRV dependency", "per CB upload",
                                       "per CB dependency", "per sampler dependency"};

struct Counters {
  uint64_t calls = 0, samePass = 0, sameState = 0, sameAll = 0, sameStateSpecial = 0;
  uint64_t cycAll = 0, cycSame = 0, cycOther = 0, cycSameState = 0, cycSameAll = 0;
  uint64_t faults = 0;
  // Per identical-state hit, the work a fast path would keep (summed features).
  double hitSrvDeps = 0, hitUploads = 0, hitCbDepsChanged = 0, hitSampDepsChanged = 0;
  double xtx[kFeat][kFeat] = {}, xty[kFeat] = {}, yy = 0, ySum = 0;
  StageCnt stage[kStages];
};

Counters g_c;  // render thread only while g_on; read after g_on = false
PassState g_buf[2];
PassState* g_prev = &g_buf[0];
PassState* g_cur = &g_buf[1];

// Bytes of each FX constant buffer at its last UpdateSubresource from Apply.
std::unordered_map<void*, std::vector<uint8_t>> g_cbBytes;

// Pre-Apply: per stage, dirty FX CBs that this Apply will upload, and how many
// of them differ from their last upload. A CB shared by stages is attributed to
// the first stage that uploads it (Apply order).
struct PreScan {
  uint32_t uploads[kStages], changed[kStages];
};

void NoteUpload(void* cbv, int stage, PreScan& ps, void** seen, int& nSeen) {
  auto* cb = static_cast<uint8_t*>(cbv);
  for (int i = 0; i < nSeen; ++i)
    if (seen[i] == cb) return;
  if (nSeen < 64) seen[nSeen++] = cb;
  const uint8_t flags = cb[0x5c];
  if (!(flags & 1) || (flags & 0x80)) return;
  ps.uploads[stage]++;
  const uint8_t* bytes = *reinterpret_cast<uint8_t**>(cb + 0x20);
  const uint32_t size = *reinterpret_cast<uint32_t*>(cb + 0x28);
  if (!bytes || size == 0 || size > (64u << 10)) {
    ps.changed[stage]++;
    return;
  }
  std::vector<uint8_t>& last = g_cbBytes[cb];
  if (last.size() != size || memcmp(last.data(), bytes, size) != 0) {
    ps.changed[stage]++;
    last.assign(bytes, bytes + size);
  }
}

void PreScanApply(uint8_t* pass, PreScan& ps) {
  memset(&ps, 0, sizeof(ps));
  void* seen[64];
  int nSeen = 0;
  for (int k = 0; k < kStages; ++k) {
    const int s = kApplyOrder[k];
    auto* blk = *reinterpret_cast<uint8_t**>(pass + kStageOff[s]);
    if (!blk) continue;
    const uint32_t nDeps = *reinterpret_cast<uint32_t*>(blk + 0x20);
    auto* deps = *reinterpret_cast<uint8_t**>(blk + 0x28);
    for (uint32_t d = 0; d < nDeps && d < 32; ++d) {
      const uint8_t* dep = deps + d * 0x20;
      const uint32_t n = *reinterpret_cast<const uint32_t*>(dep + 4);
      void** fx = *reinterpret_cast<void** const*>(dep + 8);
      for (uint32_t i = 0; i < n && i < kMaxCb; ++i)
        if (fx[i]) NoteUpload(fx[i], s, ps, seen, nSeen);
    }
    const uint32_t nTb = *reinterpret_cast<uint32_t*>(blk + 0x70);
    void** tb = *reinterpret_cast<void***>(blk + 0x78);
    for (uint32_t i = 0; i < nTb && i < 32; ++i)
      if (tb[i]) NoteUpload(tb[i], s, ps, seen, nSeen);
  }
}

// Reads a dependency array's bound pointers (+0x10) after Apply.
void ReadDeps(const uint8_t* blk, uint32_t countOff, uint32_t arrOff, uint32_t& nDeps, uint32_t& nSlots, void** out,
              uint8_t* depOf, int maxSlots, bool& overflow) {
  nDeps = *reinterpret_cast<const uint32_t*>(blk + countOff);
  const uint8_t* deps = *reinterpret_cast<uint8_t* const*>(blk + arrOff);
  nSlots = 0;
  for (uint32_t d = 0; d < nDeps && d < 64; ++d) {
    const uint8_t* dep = deps + d * 0x20;
    const uint32_t n = *reinterpret_cast<const uint32_t*>(dep + 4);
    void* const* bound = *reinterpret_cast<void* const* const*>(dep + 0x10);
    for (uint32_t i = 0; i < n; ++i) {
      if (static_cast<int>(nSlots) >= maxSlots) {
        overflow = true;
        return;
      }
      depOf[nSlots] = static_cast<uint8_t>(d < 255 ? d : 255);
      out[nSlots++] = bound[i];
    }
  }
}

void Capture(uint8_t* pass, uint32_t rs, PassState& p) {
  p.pass = pass;
  p.rs = rs;
  p.valid = true;
  p.blend = *reinterpret_cast<void**>(pass + 0x58) ? *reinterpret_cast<void**>(pass + 0x20) : nullptr;
  memcpy(p.blendFactor, pass + 0x28, sizeof(p.blendFactor));
  p.sampleMask = *reinterpret_cast<uint32_t*>(pass + 0x38);
  p.depth = *reinterpret_cast<void**>(pass + 0x60) ? *reinterpret_cast<void**>(pass + 0x40) : nullptr;
  p.stencilRef = *reinterpret_cast<uint32_t*>(pass + 0x48);
  auto* rsBlk = *reinterpret_cast<uint8_t**>(pass + 0x68);
  p.raster = rsBlk ? *reinterpret_cast<void**>(rsBlk + static_cast<uint64_t>(rs) * 8 + 0x18) : nullptr;
  p.nRt = 0;
  p.dsv = nullptr;
  if (*reinterpret_cast<void**>(pass + 0x78)) {
    const uint32_t n = *reinterpret_cast<uint32_t*>(pass + 0x70);
    p.nRt = n < kMaxRt ? n : kMaxRt;
    for (uint32_t i = 0; i < p.nRt; ++i) p.rtv[i] = **reinterpret_cast<void***>(pass + 0x78 + i * 8);
    void** dsv = *reinterpret_cast<void***>(pass + 0xb8);
    p.dsv = dsv ? *dsv : nullptr;
    if (p.nRt == 0) p.nRt = 0xffffffffu;  // RT block with count 0: still "sets RTs"
  }
  for (int s = 0; s < kStages; ++s) {
    StageState& st = p.st[s];
    auto* blk = *reinterpret_cast<uint8_t**>(pass + kStageOff[s]);
    st.block = blk;
    st.overflow = false;
    if (!blk) {
      st.shader = nullptr;
      st.nCbDeps = st.nSampDeps = st.nSrvDeps = st.nCb = st.nSamp = st.nSrv = st.nTb = st.nUav = 0;
      continue;
    }
    st.shader = *reinterpret_cast<void**>(blk + 0x18);
    ReadDeps(blk, 0x20, 0x28, st.nCbDeps, st.nCb, st.cb, st.cbDep, kMaxCb, st.overflow);
    ReadDeps(blk, 0x30, 0x38, st.nSampDeps, st.nSamp, st.samp, st.sampDep, kMaxSamp, st.overflow);
    uint8_t srvDep[kMaxSrv];
    ReadDeps(blk, 0x50, 0x58, st.nSrvDeps, st.nSrv, st.srv, srvDep, kMaxSrv, st.overflow);
    st.nUav = *reinterpret_cast<uint32_t*>(blk + 0x60);
    st.nTb = *reinterpret_cast<uint32_t*>(blk + 0x70);
  }
}

// Changed slots between two captures of the same block; also the number of
// dependencies with at least one changed slot (one Set call each).
uint32_t DiffSlots(void* const* a, void* const* b, const uint8_t* depOf, uint32_t n, uint32_t& depsChanged) {
  uint32_t changed = 0;
  int lastDep = -1;
  depsChanged = 0;
  for (uint32_t i = 0; i < n; ++i)
    if (a[i] != b[i]) {
      ++changed;
      if (depOf && depOf[i] != lastDep) {
        lastDep = depOf[i];
        ++depsChanged;
      }
    }
  return changed;
}

void Accumulate(const PassState& cur, const PassState& prev, const PreScan& ps, uint64_t cycles) {
  Counters& c = g_c;
  c.calls++;
  c.cycAll += cycles;
  // Cost-model features of this Apply.
  double x[kFeat] = {1, 0, 0, 0, 0, 0};
  for (int s = 0; s < kStages; ++s) {
    const StageState& st = cur.st[s];
    if (!st.block) continue;
    x[1] += 1;
    x[2] += st.nSrvDeps;
    x[3] += ps.uploads[s];
    x[4] += st.nCbDeps;
    x[5] += st.nSampDeps;
  }
  const double y = static_cast<double>(cycles);
  for (int i = 0; i < kFeat; ++i) {
    for (int j = 0; j < kFeat; ++j) c.xtx[i][j] += x[i] * x[j];
    c.xty[i] += x[i] * y;
  }
  c.yy += y * y;
  c.ySum += y;

  const bool samePass = prev.valid && prev.pass == cur.pass;
  if (!samePass) {
    c.cycOther += cycles;
    return;
  }
  c.samePass++;
  c.cycSame += cycles;

  bool sameState = cur.rs == prev.rs && cur.blend == prev.blend &&
                   memcmp(cur.blendFactor, prev.blendFactor, sizeof(cur.blendFactor)) == 0 &&
                   cur.sampleMask == prev.sampleMask && cur.depth == prev.depth && cur.stencilRef == prev.stencilRef &&
                   cur.raster == prev.raster && cur.nRt == prev.nRt && cur.dsv == prev.dsv;
  if (sameState && cur.nRt != 0xffffffffu)
    for (uint32_t i = 0; i < cur.nRt; ++i)
      if (cur.rtv[i] != prev.rtv[i]) sameState = false;
  bool special = cur.nRt != 0;  // RT block, UAVs or compute: not a plain draw pass
  bool bindingsSame = true;
  uint32_t cbDepsChangedAll = 0, sampDepsChangedAll = 0, srvDepsAll = 0, uploadsAll = 0;
  for (int s = 0; s < kStages; ++s) {
    const StageState& a = cur.st[s];
    const StageState& b = prev.st[s];
    if (!a.block && !b.block) continue;
    StageCnt& sc = c.stage[s];
    sc.present++;
    if (a.nUav || s == 5) special = true;
    if (a.block != b.block || a.shader != b.shader || a.overflow || b.overflow) {
      sc.blockChanged++;
      sc.anyChange++;
      sameState = false;
      bindingsSame = false;
      continue;
    }
    uint32_t cbDeps = 0, sampDeps = 0, srvDeps = 0;
    const uint32_t cbCh = DiffSlots(a.cb, b.cb, a.cbDep, a.nCb, cbDeps);
    const uint32_t sampCh = DiffSlots(a.samp, b.samp, a.sampDep, a.nSamp, sampDeps);
    const uint32_t srvCh = DiffSlots(a.srv, b.srv, nullptr, a.nSrv, srvDeps);
    sc.cbSlots += a.nCb;
    sc.cbChanged += cbCh;
    sc.cbDepsChanged += cbDeps;
    sc.sampSlots += a.nSamp;
    sc.sampChanged += sampCh;
    sc.sampDepsChanged += sampDeps;
    sc.srvSlots += a.nSrv;
    sc.srvChanged += srvCh;
    sc.srvDepsChanged += srvCh ? 1 : 0;
    sc.srvDeps += a.nSrvDeps;
    sc.uploads += ps.uploads[s];
    sc.uploadsChanged += ps.changed[s];
    if (cbCh || sampCh || srvCh || ps.changed[s]) sc.anyChange++;
    if (cbCh || sampCh || srvCh || ps.uploads[s]) bindingsSame = false;
    cbDepsChangedAll += cbDeps;
    sampDepsChangedAll += sampDeps;
    srvDepsAll += a.nSrvDeps;
    uploadsAll += ps.uploads[s];
  }
  if (!sameState) return;
  c.sameState++;
  c.cycSameState += cycles;
  if (special) c.sameStateSpecial++;
  c.hitSrvDeps += srvDepsAll;
  c.hitUploads += uploadsAll;
  c.hitCbDepsChanged += cbDepsChangedAll;
  c.hitSampDepsChanged += sampDepsChangedAll;
  if (bindingsSame) {
    c.sameAll++;
    c.cycSameAll += cycles;
  }
}

bool PreGuarded(uint8_t* pass, PreScan& ps) {
  __try {
    PreScanApply(pass, ps);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool CaptureGuarded(uint8_t* pass, uint32_t rs, PassState& p) {
  __try {
    Capture(pass, rs, p);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

uint64_t __fastcall Hook(uint8_t* pass, uint64_t flags, void* ctx, uint64_t rsIndex) {
  if (!g_on.load(std::memory_order_relaxed)) return g_orig(pass, flags, ctx, rsIndex);
  if (GetCurrentThreadId() != g_thread) {
    g_otherCalls.fetch_add(1, std::memory_order_relaxed);
    return g_orig(pass, flags, ctx, rsIndex);
  }
  PreScan ps;
  const bool preOk = PreGuarded(pass, ps);
  const uint64_t t0 = __rdtsc();
  const uint64_t r = g_orig(pass, flags, ctx, rsIndex);
  const uint64_t dt = __rdtsc() - t0;
  if (preOk && CaptureGuarded(pass, static_cast<uint32_t>(rsIndex), *g_cur)) {
    Accumulate(*g_cur, *g_prev, ps, dt);
    std::swap(g_cur, g_prev);
  } else {
    g_c.faults++;
    g_prev->valid = false;
  }
  return r;
}

const char* Check(uint8_t* dx) {
  auto** vtbl = reinterpret_cast<void**>(dx + kPassVtbl);
  if (!allocslab::RttiIs(dx, vtbl, ".?AUSPassBlock@D3DX11Effects@@") ||
      reinterpret_cast<uint8_t*>(SlotOriginal(&vtbl[kSlot])) != dx + kApply)
    return "fx apply counter: SPassBlock vtable does not match this dx11backend.dll build; skipped";
  // Measurement only (every read is SEH-guarded, nothing is changed): a code
  // hash mismatch is logged, not fatal. Loaded code can differ from the file
  // where the loader applied relocations inside a hashed range.
  for (const CodeRange& c : kCode)
    if (!shadowtex::CodeIs(dx, c.begin, c.end, c.hash))
      Log("fx apply counter: code hash differs at dx11backend+0x%x..0x%x (relocated or patched); counting anyway",
          static_cast<unsigned>(c.begin), static_cast<unsigned>(c.end));
  return nullptr;
}

// Solves (X'X + ridge) b = X'y by Gaussian elimination with partial pivoting.
bool Solve(const double (&xtx)[kFeat][kFeat], const double (&xty)[kFeat], double (&b)[kFeat]) {
  double m[kFeat][kFeat + 1];
  for (int i = 0; i < kFeat; ++i) {
    for (int j = 0; j < kFeat; ++j) m[i][j] = xtx[i][j];
    m[i][i] += 1e-6 * (xtx[i][i] > 0 ? xtx[i][i] : 1.0);
    m[i][kFeat] = xty[i];
  }
  for (int col = 0; col < kFeat; ++col) {
    int piv = col;
    for (int r = col + 1; r < kFeat; ++r)
      if (std::fabs(m[r][col]) > std::fabs(m[piv][col])) piv = r;
    if (std::fabs(m[piv][col]) < 1e-12) return false;
    if (piv != col)
      for (int j = 0; j <= kFeat; ++j) std::swap(m[col][j], m[piv][j]);
    for (int r = 0; r < kFeat; ++r) {
      if (r == col) continue;
      const double f = m[r][col] / m[col][col];
      for (int j = col; j <= kFeat; ++j) m[r][j] -= f * m[col][j];
    }
  }
  for (int i = 0; i < kFeat; ++i) b[i] = m[i][kFeat] / m[i][i];
  return true;
}

void Measure(int ms, std::atomic<uint64_t>& frames, double tscHz) {
  auto* dx = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"dx11backend.dll"));
  if (!dx || !tscHz) {
    Log("  fx apply counter: dx11backend.dll not loaded");
    return;
  }
  if (const char* why = Check(dx)) {
    Log("  %s", why);
    return;
  }
  auto** vtbl = reinterpret_cast<void**>(dx + kPassVtbl);
  g_slot = &vtbl[kSlot];
  g_orig = reinterpret_cast<Fn>(SlotOriginal(g_slot));
  g_c = Counters{};
  g_buf[0].valid = g_buf[1].valid = false;
  g_cbBytes.clear();
  g_otherCalls = 0;
  g_thread = ptiming::g_topTid.load();
  if (!g_thread) {
    Log("  fx apply counter: render thread not known yet (pass timing not running); skipped");
    return;
  }
  if (!HookSlot(g_slot, reinterpret_cast<void*>(&Hook), nullptr)) {
    Log("  fx apply counter: could not patch the SPassBlock vtable slot");
    return;
  }
  Sleep(200);  // let the wrapper settle before counting
  const uint64_t f0 = frames.load();
  g_on = true;
  Sleep(ms);
  g_on = false;
  const double f = static_cast<double>(frames.load() - f0);
  Sleep(100);
  UnhookSlot(g_slot, reinterpret_cast<void*>(g_orig));
  const Counters& c = g_c;
  if (f <= 0 || c.calls == 0) {
    Log("  fx apply counter: no Applies counted on the render thread (%.0f frames)", f);
    return;
  }
  const double nsPerCyc = 1e9 / tscHz;
  const double msPerFrame = 1000.0 / tscHz / f;
  auto pct = [](auto a, auto b) { return b > 0 ? 100.0 * static_cast<double>(a) / static_cast<double>(b) : 0.0; };
  auto meanNs = [&](uint64_t cyc, uint64_t n) { return n ? cyc * nsPerCyc / n : 0.0; };
  Log("  fx apply: render thread %lu, %.0f frames; Applies %.0f/frame (other threads %.0f/frame), %llu reads "
      "faulted",
      static_cast<unsigned long>(g_thread), f, c.calls / f, g_otherCalls.load() / f,
      static_cast<unsigned long long>(c.faults));
  Log("  fx apply: same pass as the previous Apply %.0f/frame (%.1f%%); of those, identical state objects and shaders "
      "%.0f/frame (%.1f%% of same-pass), also identical CB/sampler/SRV slots and no CB upload %.0f/frame (%.1f%%); "
      "identical-state hits with RT/UAV/compute binds %.0f/frame",
      c.samePass / f, pct(c.samePass, c.calls), c.sameState / f, pct(c.sameState, c.samePass), c.sameAll / f,
      pct(c.sameAll, c.samePass), c.sameStateSpecial / f);
  Log("  fx apply: time inside Apply %.3f ms/frame (mean %.0f ns); same-pass %.3f ms/frame (mean %.0f ns), other "
      "%.3f ms/frame (mean %.0f ns); identical-state hits %.3f ms/frame (mean %.0f ns), fully identical %.3f "
      "ms/frame (mean %.0f ns)",
      c.cycAll * msPerFrame, meanNs(c.cycAll, c.calls), c.cycSame * msPerFrame, meanNs(c.cycSame, c.samePass),
      c.cycOther * msPerFrame, meanNs(c.cycOther, c.calls - c.samePass), c.cycSameState * msPerFrame,
      meanNs(c.cycSameState, c.sameState), c.cycSameAll * msPerFrame, meanNs(c.cycSameAll, c.sameAll));
  for (int s = 0; s < kStages; ++s) {
    const StageCnt& sc = c.stage[s];
    if (!sc.present) continue;
    const double n = static_cast<double>(sc.present);
    Log("  fx apply stage %s: %.0f same-pass Applies/frame; block or shader changed %.1f%%, any change %.1f%%; "
        "per Apply: CB slots %.2f changed %.2f, samplers %.2f changed %.2f, SRV slots %.2f changed %.2f "
        "(SRV dependencies %.2f), CB uploads %.2f with new values %.2f",
        kStageNames[s], n / f, pct(sc.blockChanged, n), pct(sc.anyChange, n), sc.cbSlots / n, sc.cbChanged / n,
        sc.sampSlots / n, sc.sampChanged / n, sc.srvSlots / n, sc.srvChanged / n, sc.srvDeps / n, sc.uploads / n,
        sc.uploadsChanged / n);
  }
  // Cost model and estimate.
  double b[kFeat] = {};
  const bool solved = Solve(c.xtx, c.xty, b);
  double sse = c.yy;  // residual sum of squares = y'y - b'X'y (normal equations)
  for (int i = 0; i < kFeat; ++i) sse -= b[i] * c.xty[i];
  const double mean = c.ySum / c.calls;
  const double sst = c.yy - c.calls * mean * mean;
  if (solved)
    Log("  fx apply cost model (ns): %s %.1f, %s %.1f, %s %.1f, %s %.1f, %s %.1f, %s %.1f; R^2 %.2f",
        kFeatNames[0], b[0] * nsPerCyc, kFeatNames[1], b[1] * nsPerCyc, kFeatNames[2], b[2] * nsPerCyc,
        kFeatNames[3], b[3] * nsPerCyc, kFeatNames[4], b[4] * nsPerCyc, kFeatNames[5], b[5] * nsPerCyc,
        sst > 0 ? 1.0 - sse / sst : 0.0);
  else
    Log("  fx apply cost model: singular (features do not vary); kept work counted as 0");
  // A fast path on an identical-state hit keeps 0x67e10, the SRV binds, the dirty
  // CB uploads and the CB/sampler sets whose slots changed; it skips the state
  // blocks, OM/RS calls, SetShader and ApplyShaderBlock's bookkeeping.
  auto pos = [](double v) { return v > 0 ? v : 0.0; };
  const double keptCyc = solved ? pos(b[2]) * c.hitSrvDeps + pos(b[3]) * c.hitUploads +
                                      pos(b[4]) * c.hitCbDepsChanged + pos(b[5]) * c.hitSampDepsChanged
                                : 0.0;
  const double kRemainNs = 40.0;  // pass assignments + the hit test on a hit (R14 section 4.3)
  const double kCheckNs = 5.0;    // the test on every Apply
  const double hitMs = c.cycSameState * msPerFrame;
  const double keptMs = keptCyc * msPerFrame;
  const double remainMs = c.sameState * kRemainNs * 1e-6 / f;
  const double checkMs = c.calls * kCheckNs * 1e-6 / f;
  const double estimate = pos(hitMs - keptMs - remainMs) - checkMs;
  const double upper = pos(hitMs - remainMs) - checkMs;
  Log("  fx apply estimate: identical-state hits spend %.3f ms/frame in Apply; kept by a fast path %.3f ms/frame "
      "(SRV binds, CB uploads, changed CB/sampler sets per the cost model) + %.0f ns/hit remainder %.3f ms/frame; "
      "check %.0f ns on every Apply %.3f ms/frame",
      hitMs, keptMs, kRemainNs, remainMs, kCheckNs, checkMs);
  Log("  fx apply estimate: same-pass fast path saves about %.3f ms/frame (upper bound %.3f ms/frame); R14 gate "
      "0.4 ms: %s",
      estimate, upper, estimate >= 0.4 ? "PASS" : "below");
}

}  // namespace fxapply
