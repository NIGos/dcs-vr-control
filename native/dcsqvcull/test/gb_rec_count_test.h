// Offline tests for gb_rec_count.h (R18 S0): BLEND_MODE codes and the
// shader's define vector, model kinds, candidate sets, segment counting with
// the island rule, medians, the material-guard masks and the animated
// overrides, census name classes, gate evaluation, the item walk on fake
// NGModel/dx11backend memory, and on a real device (hardware, else WARP) the
// setup probe's change counting and ExecuteCommandList(TRUE) leaving the
// immediate context's targets and viewport as they were.
#pragma once

namespace grctest {
using namespace gbreccount;

void PureTests() {
  Check(BlendCode(nullptr) == kBmNone && BlendCode("BM_NONE") == kBmNone && BlendCode("0") == kBmNone &&
            BlendCode("BM_ALPHA_TEST") == kBmAlphaTest && BlendCode("2") == kBmAlphaTest &&
            BlendCode("BM_DECAL_DEFERRED") == kBmDecalDeferred && BlendCode("6") == kBmShadowedTransparent &&
            BlendCode("7") == kBmUnknown && BlendCode("BM_FOO") == kBmUnknown && BlendCode("") == kBmUnknown,
        "gbuffer rec S0: BLEND_MODE values (names and numbers of enums.hlsl)");
  const int k = KindCode(1, true, false, kBmAlphaTest);
  Check(k == (0 << 5 | 16 | kBmAlphaTest) && KindCode(8, false, true, kBmNone) == (2 << 5 | 8) &&
            KindCode(5, false, false, kBmUnknown) == (3 << 5 | 7) && k < kModelKinds &&
            KindName(k) == "SceneRenderable ModelMaterialMT pass 1 cockpit BM_ALPHA_TEST",
        "gbuffer rec S0: model kind codes and names");
  // Candidate sets.
  Check(InSet(kFNone, 0) && InSet(kFNone, 3) && !InSet(kFA2c, 0) && InSet(kFA2c, 1) && !InSet(kFDecal, 1) &&
            InSet(kFDecal, 2) && !InSet(kFNone | kFCockpit, 2) && InSet(kFNone | kFCockpit, 3) &&
            InSet(kFDecal | kFCockpit, 3) && !InSet(kFModel, 3) && !InSet(0, 0),
        "gbuffer rec S0: candidate sets {BM_NONE}, {+A2C}, {+decals}, {+cockpit}");
  Check(InSet(kFNone | kFClean, 4) && InSet(kFA2c | kFClean, 4) && !InSet(kFNone, 4) &&
            !InSet(kFDecal | kFClean, 4) && !InSet(kFNone | kFClean | kFCockpit, 4),
        "gbuffer rec S0: census-clean variant of set 1");
  // Segments: R = residual (0), N = BM_NONE, A = A2C. Cycles = 10 * (index + 1).
  //   N N N R A A R R N N N N  -> set 0 island 1: 3 runs; set 1 island 1: 3 runs (N N N | A A | N N N N).
  const uint8_t fl[] = {kFNone, kFNone, kFNone, 0, kFA2c, kFA2c, 0, 0, kFNone, kFNone, kFNone, kFNone};
  uint32_t cyc[12];
  for (int i = 0; i < 12; ++i) cyc[i] = 10 * (i + 1);
  SegResult r = Segments(fl, cyc, 12, 0, 1);
  Check(r.segments == 2 && r.recItems == 7 && r.recCyc == 10 + 20 + 30 + 90 + 100 + 110 + 120 &&
            r.resBetweenItems == 5 && r.resBetweenCyc == 40 + 50 + 60 + 70 + 80 && r.resOutsideCyc == 0,
        "gbuffer rec S0: segments of BM_NONE, island 1 (A2C residual between)");
  r = Segments(fl, cyc, 12, 1, 1);
  Check(r.segments == 3 && r.recItems == 9 && r.resBetweenItems == 3 && r.resBetweenCyc == 40 + 70 + 80,
        "gbuffer rec S0: segments with A2C, island 1");
  r = Segments(fl, cyc, 12, 1, 3);
  Check(r.segments == 2 && r.recItems == 7 && r.resBetweenItems == 5 && r.resBetweenCyc == 40 + 50 + 60 + 70 + 80,
        "gbuffer rec S0: island rule: a run shorter than the island stays residual");
  r = Segments(fl, cyc, 12, 1, 4);
  Check(r.segments == 1 && r.recItems == 4 && r.resBetweenItems == 0 && r.resBetweenCyc == 0 &&
            r.resOutsideCyc == 10 + 20 + 30 + 40 + 50 + 60 + 70 + 80,
        "gbuffer rec S0: residual outside the first..last segment");
  r = Segments(fl, nullptr, 12, 0, 100);
  Check(r.segments == 0 && r.recItems == 0 && r.resOutsideCyc == 0, "gbuffer rec S0: no run reaches the island");
  r = Segments(fl, cyc, 0, 0, 1);
  Check(r.segments == 0 && r.resOutsideCyc == 0, "gbuffer rec S0: empty vector");
  Check(Median({}) == 0 && Median({3, 1, 2}) == 2 && Median({4, 1, 3, 2}) == 2.5 && Quantile({1, 2, 3, 4, 5}, 0.9) == 5,
        "gbuffer rec S0: median and quantile");
  // Guard masks: overrides excluded, everything else compared.
  uint64_t over[2] = {};
  BaseOverrides(over);
  auto bit = [](const uint64_t m[2], uint32_t off) { return (m[(off / 4) >> 6] >> ((off / 4) & 63) & 1) != 0; };
  Check(bit(over, 0xfc) && bit(over, 0x20) && bit(over, 0x5c) && bit(over, 0x80) && bit(over, 0x88) &&
            !bit(over, 0x8c) && !bit(over, 0x1c) && !bit(over, 0x60) && !bit(over, 0xf8),
        "gbuffer rec S0: per-draw override dwords (+0xfc, +0x20..+0x5f, +0x80..+0x8b)");
  uint64_t m[2] = {};
  MarkCb(m, -8, 16);     // partly before the CB
  MarkCb(m, 0x12c, 16);  // partly past its end
  Check(bit(m, 0) && bit(m, 4) && !bit(m, 8) && bit(m, 0x12c) && m[1] >> (kCbDwords - 64) == 0,
        "gbuffer rec S0: dword marks clipped to def_uniforms");
  uint8_t a[kCbLen], b[kCbLen];
  for (uint32_t i = 0; i < kCbLen; ++i) a[i] = b[i] = static_cast<uint8_t>(i);
  uint64_t all[2] = {~0ull & ~over[0], ((1ull << (kCbDwords - 64)) - 1) & ~over[1]};
  b[0xfc] ^= 1;
  b[0x30] ^= 1;
  b[0x84] ^= 1;
  const bool overOnly = !DwordsDiffer(a, b, all);
  b[0x12f] ^= 1;
  Check(overOnly && DwordsDiffer(a, b, all), "gbuffer rec S0: override changes ignored, the last dword compared");
  // Census names.
  Check(NameCatOf("def_uniforms") == kNcMaterialCb && NameCatOf("cPerFrame") == kNcPerFrame &&
            NameCatOf("cPerView") == kNcPerView && NameCatOf("cAmbientMap") == kNcAmbient &&
            NameCatOf("sbPositions") == kNcPositions && NameCatOf("gBilinearClampSampler") == kNcSampler &&
            NameCatOf("DiffuseMap") == kNcOther && NameCatOf("cbTonemapParams") == kNcOther && NameCatOf(nullptr) == kNcOther,
        "gbuffer rec S0: census name classes");
  // Gates.
  GateIn in;
  in.modelMs = 6;
  in.segMeasured = true;
  in.medianSeg = 3;
  in.resBetweenMs = 0.4;
  in.censusMeasured = true;
  in.censusPct = 95;
  in.stableSamples = 1000;
  in.stablePct = 99.5;
  in.guardMeasured = true;
  in.guardChanges = 0;
  in.execMeasured = true;
  in.execUs = 9;
  GateOut o = Evaluate(in);
  bool allPass = true;
  for (int g : o.g) allPass &= g == 1;
  Check(allPass && o.verdict == 1 && !o.trigger, "gbuffer rec S0: every gate passes -> GO");
  GateIn x = in;
  x.execMeasured = false;
  o = Evaluate(x);
  Check(o.g[5] == -1 && o.verdict == 1, "gbuffer rec S0: Execute cost not measured leaves GO (S1 measures it)");
  x = in;
  x.guardMeasured = false;
  o = Evaluate(x);
  Check(o.g[4] == -1 && o.verdict == -1, "gbuffer rec S0: an unmeasured gate 1-5 -> incomplete");
  x = in;
  x.resBetweenMs = 1.2;
  o = Evaluate(x);
  Check(o.g[1] == 0 && o.verdict == 0 && !o.trigger, "gbuffer rec S0: residual between segments >= 1 ms fails gate 2");
  x = in;
  x.medianSeg = 9;
  o = Evaluate(x);
  Check(o.g[1] == 0 && o.trigger && o.verdict == 0, "gbuffer rec S0: segments > 8 is a no-go trigger");
  x = in;
  x.modelMs = 4;
  o = Evaluate(x);
  Check(o.g[0] == 0 && !o.trigger && o.verdict == 0, "gbuffer rec S0: model share 4 ms fails gate 1 (no trigger)");
  x.modelMs = 3;
  o = Evaluate(x);
  Check(o.trigger, "gbuffer rec S0: model share < 3.5 ms is a no-go trigger");
  x = in;
  x.stablePct = 98.9;
  x.execUs = 16;
  o = Evaluate(x);
  Check(o.g[3] == 0 && o.g[5] == 0 && o.verdict == 0, "gbuffer rec S0: stability and Execute cost gates");
}

// ---- Fake DCS memory for the item walk ----
alignas(16) void* g_fakeSrVt[4] = {nullptr, nullptr, nullptr, nullptr};
alignas(16) void* g_fakeMatVt[4] = {};
alignas(16) void* g_fakeShVt[4] = {};
alignas(16) void* g_fakeOtherVt[4] = {};

struct FakeShader {
  alignas(16) uint8_t sh[0x100] = {};
  alignas(16) uint8_t defs[0xa0] = {};
  void Init(const char* blend) {
    *reinterpret_cast<void**>(sh) = &g_fakeShVt[1];
    *reinterpret_cast<void**>(sh + 0x50) = this;  // effect (identity only)
    if (blend) {
      strcpy_s(reinterpret_cast<char*>(defs + 8), 0x48, "BLEND_MODE");
      strcpy_s(reinterpret_cast<char*>(defs + 0x58), 0x48, blend);
      *reinterpret_cast<uint8_t**>(sh + 0x98) = defs;
      *reinterpret_cast<uint8_t**>(sh + 0xa0) = defs + 0xa0;
    }
  }
};
struct FakeItem {
  alignas(16) uint8_t r[0x80] = {};
  alignas(16) uint8_t item[0x100] = {};
  alignas(16) uint8_t mat[0x300] = {};
  alignas(16) uint8_t props[0x280] = {};
  void Init(FakeShader& s, uint32_t pass, bool cockpit, bool transparent) {
    *reinterpret_cast<void**>(r) = &g_fakeSrVt[1];
    *reinterpret_cast<uint8_t**>(r + 0x10) = item;
    *reinterpret_cast<uint32_t*>(r + 0x60) = pass;
    r[0x64] = cockpit ? 1 : 0;
    *reinterpret_cast<uint8_t**>(item + 0x10) = mat;
    *reinterpret_cast<void**>(mat) = &g_fakeMatVt[1];
    *reinterpret_cast<uint8_t**>(mat + 0x28) = props;
    *reinterpret_cast<uint8_t**>(mat + 0x30) = s.sh;
    *reinterpret_cast<uint64_t*>(mat + 0x1d8) = 1;
    props[0x33] = transparent ? 1 : 0;
  }
};

void WalkTests() {
  FakeShader none, a2c, decal;
  none.Init(nullptr);
  a2c.Init("BM_ALPHA_TEST");
  decal.Init("5");
  Check(ReadBlendRaw(none.sh) == kBmNone && ReadBlendRaw(a2c.sh) == kBmAlphaTest && ReadBlendRaw(decal.sh) == kBmDecalDeferred,
        "gbuffer rec S0: BLEND_MODE read from the DX11Shader's define vector");
  FakeItem it[7];
  it[0].Init(none, 1, false, false);
  it[1].Init(none, 1, false, false);
  it[2].Init(a2c, 1, false, false);
  it[3].Init(decal, 1, false, false);
  it[4].Init(none, 1, true, false);   // cockpit
  it[5].Init(none, 1, false, true);   // transparent
  it[6].Init(none, 8, false, false);  // flat_shadow pass
  alignas(16) uint8_t other[0x40] = {};
  *reinterpret_cast<void**>(other) = &g_fakeOtherVt[1];
  void* vec[9] = {it[0].r, it[1].r, other, it[2].r, it[3].r, it[4].r, it[5].r, it[6].r, nullptr};
  // Save and set the module's state.
  void* const sSr = g_srVt;
  void* const sMat = g_modelMatVt;
  void* const sSh = g_shaderVt;
  MatEntry* const sMats = g_mats;
  const bool sCen = gbreccount::g_census, sTex = g_texOk;
  g_srVt = &g_fakeSrVt[1];
  g_modelMatVt = &g_fakeMatVt[1];
  g_shaderVt = &g_fakeShVt[1];
  g_mats = nullptr;
  gbreccount::g_census = false;
  g_texOk = false;
  memset(g_blend, 0, sizeof(g_blend));
  const int nsBefore = g_nsCount.load();
  SlotStat* st = new SlotStat();
  memset(st, 0, sizeof(*st));
  ++g_passGen;
  const bool ok = AnalyseRaw(vec, 9, *st);
  Check(ok && st->items == 9 && st->modelItems == 7 && st->matsNoEntry == 7 && st->cenDraws == 5 && st->untextured == 5,
        "gbuffer rec S0: walk counts items, model draws and pass-1 opaque draws");
  Check(g_flags[0] == (kFModel | kFNone) && g_flags[1] == (kFModel | kFNone) && g_flags[2] == 0 &&
            g_flags[3] == (kFModel | kFA2c) && g_flags[4] == (kFModel | kFDecal) &&
            g_flags[5] == (kFModel | kFNone | kFCockpit) && g_flags[6] == kFModel && g_flags[7] == kFModel &&
            g_flags[8] == 0,
        "gbuffer rec S0: walk flags (blend, cockpit; transparent and pass 8 not recordable)");
  Check(g_cls[0] == KindCode(1, false, false, kBmNone) && g_cls[3] == KindCode(1, false, false, kBmAlphaTest) &&
            g_cls[6] == KindCode(1, false, true, kBmNone) && g_cls[7] == KindCode(8, false, false, kBmNone) &&
            g_cls[2] >= kClsNs0 && g_cls[2] <= kClsNsOver && g_cls[8] == kClsSrNull,
        "gbuffer rec S0: walk classes (model kinds, other renderable by vtable, null)");
  const SegResult s0 = Segments(g_flags, nullptr, 9, 0, 1);
  const SegResult s3 = Segments(g_flags, nullptr, 9, 3, 1);
  Check(s0.segments == 1 && s0.recItems == 2 && s3.segments == 2 && s3.recItems == 5,
        "gbuffer rec S0: walk -> segments (foreign renderable splits)");
  delete st;
  // Restore (the class learnt from the fake vtable is forgotten).
  g_nsCount = nsBefore;
  memset(g_blend, 0, sizeof(g_blend));
  g_srVt = sSr;
  g_modelMatVt = sMat;
  g_shaderVt = sSh;
  g_mats = sMats;
  gbreccount::g_census = sCen;
  g_texOk = sTex;
}

void MaterialTests() {
  alignas(16) uint8_t mat[0x300] = {};
  for (uint32_t i = 0; i < kCbLen; ++i) mat[kCbOff + i] = static_cast<uint8_t>(i * 7);
  // Animated property list: one entry writing (unknown class: 16 bytes) at mat+0x150 (CB +0xc0).
  alignas(16) uint8_t prop[0x20] = {};
  alignas(16) uint8_t list[16] = {};
  *reinterpret_cast<uint8_t**>(list) = mat + 0x150;
  *reinterpret_cast<uint8_t**>(list + 8) = prop;
  *reinterpret_cast<uint8_t**>(mat + 8) = list;
  *reinterpret_cast<uint8_t**>(mat + 0x10) = list + 16;
  uint64_t over[2] = {};
  uint8_t* const sMd = g_md;
  g_md = nullptr;
  AnimOverrides(mat, over);
  auto bit = [](const uint64_t m[2], uint32_t off) { return (m[(off / 4) >> 6] >> ((off / 4) & 63) & 1) != 0; };
  Check(bit(over, 0xc0) && bit(over, 0xcc) && !bit(over, 0xd0) && !bit(over, 0xbc),
        "gbuffer rec S0: animated property destinations are overrides (widest write for an unknown class)");
  MatEntry* e = new MatEntry();
  memset(e, 0, sizeof(*e));
  const uint32_t sGen = g_renderGen;
  g_renderGen = 77;
  e->snapGen = 77;
  memcpy(e->cb, mat + kCbOff, kCbLen);
  SlotStat* st = new SlotStat();
  memset(st, 0, sizeof(*st));
  ++g_passGen;
  mat[0x18c] ^= 1;  // posStructOffset
  mat[0x150] ^= 1;  // animated
  mat[0x110] ^= 1;  // 1.0
  VisitMat(*e, mat, nullptr, 0, *st);
  const bool quiet = !(e->flags & (kMfGuardChg | kMfCbChg)) && (e->flags & kMfNoRefl) && e->passGen == g_passGen;
  mat[0x90 + 0x10] ^= 1;  // a resident dword
  ++g_passGen;
  VisitMat(*e, mat, nullptr, 0, *st);
  const bool caught = (e->flags & kMfGuardChg) && (e->flags & kMfCbChg);
  g_renderGen = 78;
  VisitMat(*e, mat, nullptr, 0, *st);
  Check(quiet && caught && (e->flags & kMfNoSnap) && st->mats == 3 && st->matsGuardChg == 1 && st->matsNoSnap == 1,
        "gbuffer rec S0: material bytes since render entry (overrides ignored, resident change caught, no copy)");
  g_renderGen = sGen;
  g_md = sMd;
  delete st;
  delete e;
}

void DeviceTests() {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev,
                               nullptr, &ctx)) &&
      FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr,
                               &ctx))) {
    Check(false, "gbuffer rec S0: create a D3D11 device");
    return;
  }
  D3D11_TEXTURE2D_DESC td{};
  td.Width = td.Height = 64;
  td.MipLevels = 1;
  td.ArraySize = 6;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  ID3D11Texture2D* arr = nullptr;
  dev->CreateTexture2D(&td, nullptr, &arr);
  ID3D11RenderTargetView* rtv[6] = {};
  for (UINT i = 0; i < 6; ++i) {
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = td.Format;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
    rd.Texture2DArray.FirstArraySlice = i;
    rd.Texture2DArray.ArraySize = 1;
    dev->CreateRenderTargetView(arr, &rd, &rtv[i]);
  }
  D3D11_TEXTURE2D_DESC dd = td;
  dd.ArraySize = 1;
  dd.Format = DXGI_FORMAT_D32_FLOAT;
  dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  ID3D11Texture2D* depth = nullptr;
  dev->CreateTexture2D(&dd, nullptr, &depth);
  ID3D11DepthStencilView* dsv = nullptr;
  dev->CreateDepthStencilView(depth, nullptr, &dsv);
  ctx->OMSetRenderTargets(6, rtv, dsv);
  D3D11_VIEWPORT vp{0, 0, 64, 64, 0, 1};
  ctx->RSSetViewports(1, &vp);
  ID3D11DeviceContext* dc = nullptr;
  const bool haveDc = SUCCEEDED(dev->CreateDeferredContext(0, &dc)) && dc;
  Check(haveDc, "gbuffer rec S0: deferred context");
  // Module state for the probe.
  ID3D11DeviceContext* const sCtx = g_ctx;
  ID3D11DeviceContext* const sDc = g_dc;
  const double sHz = gbreccount::g_tscHz;
  g_ctx = ctx;
  g_dc = haveDc ? dc : nullptr;
  if (!gbreccount::g_tscHz) {
    LARGE_INTEGER q0, q1;
    QueryPerformanceCounter(&q0);
    const uint64_t c0 = __rdtsc();
    Sleep(50);
    QueryPerformanceCounter(&q1);
    LARGE_INTEGER fq;
    QueryPerformanceFrequency(&fq);
    gbreccount::g_tscHz = (__rdtsc() - c0) / (static_cast<double>(q1.QuadPart - q0.QuadPart) / fq.QuadPart);
  }
  g_execN[0] = g_execN[1] = 0;
  g_execSeq = 0;
  g_execFail = 0;
  void** const sRenderer = shadowbatch::g_rendererObj;
  shadowbatch::g_rendererObj = nullptr;  // no DX11Renderer here: flags not read
  alignas(16) uint8_t fakePass[0x700] = {};
  fakePass[0x90 + 0x599] = 1;
  t_pass = fakePass;
  SlotStat* st = new SlotStat();
  memset(st, 0, sizeof(*st));
  for (int i = 0; i < 4 * kExecEvery; ++i) ProbeSetup(*st);
  // The immediate context still has the targets and viewport.
  ID3D11RenderTargetView* now[8] = {};
  ID3D11DepthStencilView* nowDsv = nullptr;
  ctx->OMGetRenderTargets(8, now, &nowDsv);
  bool same = nowDsv == dsv;
  for (int i = 0; i < 8; ++i) same &= now[i] == (i < 6 ? rtv[i] : nullptr);
  for (auto* p : now)
    if (p) p->Release();
  if (nowDsv) nowDsv->Release();
  D3D11_VIEWPORT vnow[16];
  UINT nv = 16;
  ctx->RSGetViewports(&nv, vnow);
  same &= nv == 1 && memcmp(&vnow[0], &vp, sizeof(vp)) == 0;
  Check(!haveDc || (g_execN[0] == 2 && g_execN[1] == 2 && g_execFail == 0),
        "gbuffer rec S0: Execute(TRUE) timed every 4th execution, both list variants");
  Check(same, "gbuffer rec S0: Execute(TRUE) leaves the immediate targets and viewport as they were");
  if (haveDc) printf("     ExecuteCommandList(TRUE): empty %.2f us, binding %.2f us (median, this device)\n", ExecMedian(0), ExecMedian(1));
  Check(st->samples == 16 && st->unstable == 0 && st->chRtv == 0 && st->last.nrt == 6 && st->last.passFlagsOk &&
            st->last.passFlags[1] == 1 && st->rendererMissing == 16,
        "gbuffer rec S0: setup probe, identical executions are stable");
  D3D11_VIEWPORT vp2 = vp;
  vp2.Width = 32;
  ctx->RSSetViewports(1, &vp2);
  ProbeSetup(*st);
  ctx->OMSetRenderTargets(5, rtv, dsv);
  ProbeSetup(*st);
  fakePass[0x90 + 0x598] = 1;
  ProbeSetup(*st);
  Check(st->samples == 19 && st->chVp == 1 && st->chRtv == 1 && st->chPassFlags == 1 && st->unstable == 3 &&
            st->last.nrt == 5 && st->rtv0SeenCount == 1,
        "gbuffer rec S0: setup probe counts viewport, RTV and pass-flag changes");
  delete st;
  t_pass = nullptr;
  shadowbatch::g_rendererObj = sRenderer;
  g_ctx = sCtx;
  g_dc = sDc;
  gbreccount::g_tscHz = sHz;
  g_execN[0] = g_execN[1] = 0;
  if (dc) dc->Release();
  ctx->ClearState();
  dsv->Release();
  depth->Release();
  for (auto* p : rtv) p->Release();
  arr->Release();
  ctx->Release();
  dev->Release();
}

void Run() {
  PureTests();
  WalkTests();
  MaterialTests();
  DeviceTests();
}

}  // namespace grctest
