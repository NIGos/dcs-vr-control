// Offline tests for fwd_rec_count.h (R21 S0): pass names to kinds, the FX
// pass index NGModel 0x15e20 picks, model kind codes, blended draws, rule
// flags and segments with the island rule, the route overrides, the census
// name split, gate evaluation and the verdict, and the item walk on fake
// NGModel/dx11backend memory (classes, FX pass indices, first failing rule,
// blended flag).
#pragma once

namespace frctest {
using namespace fwdreccount;

void PureTests() {
  Check(KindOfName("OpaquePass") == kOpaque && KindOfName("Main0OpaquePass") == kOpaque &&
            KindOfName("MirrorOpaquePass") == kMirrorOpaque && KindOfName("xTransparentPass") == kTransparent &&
            KindOfName("xSeparateTransparentPass") == kSeparateTransparent &&
            KindOfName("xMirrorTransparentPass") == kMirrorTransparent &&
            KindOfName("xTransparentCockpitPass") == kTransparentCockpit &&
            KindOfName("xTransparentCockpitDecalPass") == kTransparentCockpitDecal &&
            KindOfName("xTransparentDecalPass") == kTransparentDecal &&
            KindOfName("SimpleFlatShadowPass") == kFlatShadow && KindOfName("DemoSceneMapTDPass") == kDemoSceneMapTd &&
            KindOfName("DemoSceneMapPass") == kDemoSceneMap && KindOfName("LightMapPass") == kLightMap &&
            KindOfName("Pass") == kKindOther && KindOfName("") == kKindOther && KindOfName(nullptr) == kKindOther &&
            KindOfName("OpaquePassX") == kKindOther,
        "forward rec S0: pass names to kinds by the longest suffix");
  // FxPassIndex(pass, ck, b65, b68, opaque, selected) against the decoded 0x15e20.
  Check(FxPassIndex(1, false, false, false, true, false) == 0 && FxPassIndex(1, true, true, true, true, true) == 0,
        "forward rec S0: pass 1 opaque -> P0 (G-buffer), cockpit or not");
  Check(FxPassIndex(1, false, false, false, false, false) == 1 && FxPassIndex(1, false, true, false, false, false) == 2 &&
            FxPassIndex(2, false, false, false, true, false) == 1 && FxPassIndex(2, false, true, true, true, false) == 2 &&
            FxPassIndex(3, false, false, false, true, false) == 1,
        "forward rec S0: forward P1 (fwd_ps) / P2 (fwd_nsm_ps) from byte r+0x65");
  Check(FxPassIndex(2, true, false, false, true, false) == 1 && FxPassIndex(2, true, true, false, true, false) == 2 &&
            FxPassIndex(2, true, false, true, true, false) == 3 && FxPassIndex(2, true, true, true, true, false) == 4,
        "forward rec S0: cockpit P1-P4 from r+0x65 and r+0x68 bit 0");
  Check(FxPassIndex(2, false, true, false, true, true) == 0 && FxPassIndex(2, true, true, true, true, true) == 1 &&
            FxPassIndex(3, true, false, false, true, true) == 2,
        "forward rec S0: selected materials (props+0x12d): P0 without cockpit, else P1/P2 by pass 3");
  Check(fwdreccount::KindCode(1, false, false, gbreccount::kBmNone) == 0 && fwdreccount::KindCode(2, true, false, gbreccount::kBmAlphaTest) == (1 << 5 | 16 | 2) &&
            fwdreccount::KindCode(3, false, true, 1) == (2 << 5 | 8 | 1) && fwdreccount::KindCode(8, false, false, 0) == 3 << 5 &&
            fwdreccount::KindCode(5, false, false, gbreccount::kBmUnknown) == (4 << 5 | 7) &&
            fwdreccount::KindCode(9, true, true, 7) < kModelKinds &&
            ModelKindName(fwdreccount::KindCode(8, false, true, 1)) == "SceneRenderable ModelMaterialMT pass 8 (flat_shadow) transparent BM_TRANSPARENT",
        "forward rec S0: model kind codes and names");
  Check(!Blended(gbreccount::kBmNone, 1) && !Blended(gbreccount::kBmAlphaTest, 2) && Blended(gbreccount::kBmTransparent, 1) &&
            Blended(gbreccount::kBmAdditive, 1) && Blended(gbreccount::kBmDecal, 1) && Blended(gbreccount::kBmDecalDeferred, 1) &&
            Blended(gbreccount::kBmShadowedTransparent, 1) && Blended(gbreccount::kBmNone, 8) &&
            Blended(gbreccount::kBmUnknown, 1),
        "forward rec S0: blended draws (blend modes, flat_shadow, unknown)");
  Check(FlagsFor(kWRecordable, true, false) == (kFModel | kFStrict | kFGlobals | kFAny) &&
            FlagsFor(kWGlobals, true, true) == (kFModel | kFGlobals | kFAny | kFBlend) &&
            FlagsFor(kWInherit, true, false) == (kFModel | kFAny) && FlagsFor(kWNoShader, false, false) == kFModel,
        "forward rec S0: rule flags (strict, +globals, any)");
  // Segments: S = strict, G = globals only, A = any model, R = residual. Cycles 10 * (i + 1).
  const uint8_t S = FlagsFor(kWRecordable, true, false), G = FlagsFor(kWGlobals, true, true),
                A = FlagsFor(kWTechnique, true, true);
  const uint8_t fl[] = {S, S, G, 0, A, S, S, S, G, G};
  uint32_t cyc[10];
  for (int i = 0; i < 10; ++i) cyc[i] = 10 * (i + 1);
  SegResult r = Segments(fl, cyc, 10, kSetNeed[0], 1);
  Check(r.segments == 2 && r.recItems == 5 && r.recCyc == 10 + 20 + 60 + 70 + 80 && r.resBetweenItems == 3 &&
            r.resBetweenCyc == 30 + 40 + 50 && r.resOutsideCyc == 90 + 100 && r.blendItems == 0,
        "forward rec S0: strict segments, island 1");
  r = Segments(fl, cyc, 10, kSetNeed[1], 1);
  Check(r.segments == 2 && r.recItems == 8 && r.blendItems == 3 && r.resBetweenItems == 2 && r.resBetweenCyc == 40 + 50,
        "forward rec S0: +globals segments, island 1, blended draws counted");
  r = Segments(fl, cyc, 10, kSetNeed[1], 4);
  Check(r.segments == 1 && r.recItems == 5 && r.resOutsideCyc == 10 + 20 + 30 + 40 + 50,
        "forward rec S0: island rule keeps the short run residual");
  r = Segments(fl, cyc, 10, kSetNeed[2], 1);
  Check(r.segments == 2 && r.recItems == 9, "forward rec S0: any-model set");
  r = Segments(fl, nullptr, 0, kSetNeed[0], 1);
  Check(r.segments == 0 && r.resOutsideCyc == 0, "forward rec S0: empty vector");
  // Route overrides: base + forward +0x100, flat_shadow +0x10..+0x1f and +0xd0..+0xd7, normal_ir +0x60..+0x6f.
  uint64_t m[2] = {};
  RouteOverrides(m);
  auto bit = [](const uint64_t x[2], uint32_t off) { return (x[(off / 4) >> 6] >> ((off / 4) & 63) & 1) != 0; };
  Check(bit(m, 0xfc) && bit(m, 0x20) && bit(m, 0x88) && bit(m, 0x100) && bit(m, 0x10) && bit(m, 0x1c) &&
            bit(m, 0xd0) && bit(m, 0xd4) && bit(m, 0x60) && bit(m, 0x6c) && !bit(m, 0x0c) && !bit(m, 0x8c) &&
            !bit(m, 0xd8) && !bit(m, 0x104) && !bit(m, 0x70),
        "forward rec S0: every route's per-draw dwords are overrides, the rest guarded");
  Check(FlagsFor(kWUnmapped, true, false) == (kFModel | kFAny), "forward rec S0: unmapped names are not recordable");
  Check(strcmp(RegisterOf("cascadeShadowMap"), "t122") == 0 && strcmp(RegisterOf("LightsIdx"), "t95") == 0 &&
            strcmp(RegisterOf("cbWaterParams"), "b5") == 0 && !RegisterOf("DiffuseMap") && !RegisterOf("cPerView") &&
            !RegisterOf(nullptr),
        "forward rec S0: fixed-register globals (shader sources) vs material names");
  const std::vector<std::string> sp = SplitNames("a,bb,,c");
  Check(sp.size() == 3 && sp[0] == "a" && sp[1] == "bb" && sp[2] == "c" && SplitNames("").empty(),
        "forward rec S0: census name list split");
  // Gates.
  GateIn in;
  in.modelMs = 2.5;
  in.recMs = 1.8;
  in.segMeasured = true;
  in.medianSeg = 2;
  in.segPerFrame = 10;
  in.censusMeasured = true;
  in.strictPct = 95;
  in.stableSamples = 500;
  in.stablePct = 99.6;
  in.guardMeasured = true;
  in.guardChanges = 0;
  in.globCalls = 100;
  in.globStablePct = 100;
  GateOut o = Evaluate(in);
  bool all = true;
  for (int g : o.g) all &= g == 1;
  Check(all && o.verdict == 2, "forward rec S0: every gate passes -> GO");
  GateIn x = in;
  x.strictPct = 20;
  o = Evaluate(x);
  Check(o.g[3] == 0 && o.verdict == 1, "forward rec S0: census fails alone -> GO with late-bound globals");
  x.globStablePct = 90;
  o = Evaluate(x);
  Check(o.g[6] == 0 && o.verdict == 0, "forward rec S0: globals changing within a call -> NO-GO when they are needed");
  x.globCalls = 0;
  o = Evaluate(x);
  Check(o.g[6] == -1 && o.verdict == -1, "forward rec S0: globals needed but not checked -> incomplete");
  x = in;
  x.globStablePct = 50;
  o = Evaluate(x);
  Check(o.g[6] == 0 && o.verdict == 2, "forward rec S0: globals not needed (census clean): gate 7 does not decide");
  x = in;
  x.segPerFrame = 40;  // 0.64 ms > 20 % of 1.8 ms
  o = Evaluate(x);
  Check(o.g[2] == 0 && o.verdict == 0, "forward rec S0: Executes eating the saving fail gate 3");
  x = in;
  x.medianSeg = 5;
  o = Evaluate(x);
  Check(o.g[2] == 0 && o.verdict == 0, "forward rec S0: more than 4 segments per main call fail gate 3");
  x = in;
  x.modelMs = 1.2;
  x.recMs = 0.9;
  o = Evaluate(x);
  Check(o.g[0] == 0 && o.g[1] == 0 && o.verdict == 0, "forward rec S0: small model share -> NO-GO");
  x = in;
  x.guardMeasured = false;
  o = Evaluate(x);
  Check(o.g[5] == -1 && o.verdict == -1, "forward rec S0: an unmeasured gate -> incomplete");
  x = in;
  x.censusMeasured = false;
  o = Evaluate(x);
  Check(o.g[3] == -1 && o.verdict == -1, "forward rec S0: census not measured -> incomplete");
  x = in;
  x.stablePct = 98;
  x.guardChanges = 3;
  o = Evaluate(x);
  Check(o.g[4] == 0 && o.g[5] == 0 && o.verdict == 0, "forward rec S0: stability and material guard gates");
  Check(strstr(VerdictText(2), "GO") && strstr(VerdictText(1), "GLOBALS") && strcmp(VerdictText(0), "NO-GO") == 0,
        "forward rec S0: verdict texts");
}

// ---- Fake DCS memory for the item walk ----
alignas(16) void* g_fakeSrVt[4] = {};
alignas(16) void* g_fakeMatVt[4] = {};
alignas(16) void* g_fakeShVt[4] = {};
alignas(16) void* g_fakeOtherVt[4] = {};

struct FakeShader {
  alignas(16) uint8_t sh[0x100] = {};
  alignas(16) uint8_t defs[0xa0] = {};
  void Init(const char* blend) {
    *reinterpret_cast<void**>(sh) = &g_fakeShVt[1];
    *reinterpret_cast<void**>(sh + 0x50) = this;
    if (blend) {
      strcpy_s(reinterpret_cast<char*>(defs + 8), 0x48, "BLEND_MODE");
      strcpy_s(reinterpret_cast<char*>(defs + 0x58), 0x48, blend);
      *reinterpret_cast<uint8_t**>(sh + 0x98) = defs;
      *reinterpret_cast<uint8_t**>(sh + 0xa0) = defs + 0xa0;
    }
  }
};
// A mesh shadow_rec's ReadMesh accepts: indexed triangle list, one element.
struct FakeMesh {
  alignas(16) uint8_t mesh[0x220] = {};
  alignas(16) uint8_t ib[0x40] = {};
  alignas(16) uint8_t vb[0x160] = {};
  void Init() {
    *reinterpret_cast<void**>(mesh) = ib;
    *reinterpret_cast<void**>(mesh + 0x18) = vb;
    *reinterpret_cast<uintptr_t*>(ib + 0x28) = 0x1000;  // buffer pointers: identity only
    *reinterpret_cast<uint32_t*>(ib + 0x3c) = 2;
    *reinterpret_cast<uintptr_t*>(vb + 0x130) = 0x2000;
    *reinterpret_cast<uint32_t*>(vb + 0x150) = 32;
    *reinterpret_cast<uint32_t*>(mesh + 0x208) = 1;
    *reinterpret_cast<uint32_t*>(mesh + 0x20c) = shrec::kPrimTriList;
    *reinterpret_cast<uint32_t*>(mesh + 0x210) = 12;
  }
};
struct FakeItem {
  alignas(16) uint8_t r[0x80] = {};
  alignas(16) uint8_t item[0x100] = {};
  alignas(16) uint8_t mat[0x300] = {};
  alignas(16) uint8_t props[0x280] = {};
  void Init(FakeShader& s, FakeMesh* mesh, uint32_t pass, bool cockpit, bool transparent, bool b65) {
    *reinterpret_cast<void**>(r) = &g_fakeSrVt[1];
    *reinterpret_cast<uint8_t**>(r + 0x10) = item;
    *reinterpret_cast<uint32_t*>(r + 0x60) = pass;
    r[0x64] = cockpit ? 1 : 0;
    r[0x65] = b65 ? 1 : 0;
    *reinterpret_cast<uint8_t**>(item + 0x10) = mat;
    *reinterpret_cast<uint8_t**>(item + 0xc0) = mesh ? mesh->mesh : nullptr;
    *reinterpret_cast<void**>(mat) = &g_fakeMatVt[1];
    *reinterpret_cast<uint8_t**>(mat + 0x28) = props;
    *reinterpret_cast<uint8_t**>(mat + 0x30) = s.sh;
    *reinterpret_cast<uint64_t*>(mat + 0x1d8) = 1;
    props[0x33] = transparent ? 1 : 0;
  }
};

void WalkTests() {
  FakeShader none, transp;
  none.Init(nullptr);
  transp.Init("BM_TRANSPARENT");
  FakeMesh mesh;
  mesh.Init();
  FakeItem it[5];
  it[0].Init(transp, &mesh, 1, false, true, false);  // transparent forward draw: P1, blended
  it[1].Init(none, &mesh, 2, false, false, true);    // pass 2: P2
  it[2].Init(none, &mesh, 8, false, false, false);   // flat_shadow: other technique, blended
  it[3].Init(none, nullptr, 2, true, false, false);  // cockpit, no mesh
  it[4].Init(none, &mesh, 2, false, false, false);
  *reinterpret_cast<void**>(it[4].mat + 0x30) = nullptr;  // no DX11Shader
  alignas(16) uint8_t other[0x40] = {};
  *reinterpret_cast<void**>(other) = &g_fakeOtherVt[1];
  void* vec[7] = {it[0].r, other, it[1].r, it[2].r, it[3].r, it[4].r, nullptr};
  void* const sSr = g_srVt;
  void* const sMat = g_modelMatVt;
  void* const sSh = g_shaderVt;
  MatEntry* const sMats = g_mats;
  const bool sCen = fwdreccount::g_census, sTex = g_texOk;
  uint8_t* const sMd = g_md;
  g_srVt = &g_fakeSrVt[1];
  g_modelMatVt = &g_fakeMatVt[1];
  g_shaderVt = &g_fakeShVt[1];
  g_mats = nullptr;
  fwdreccount::g_census = false;
  g_texOk = false;
  g_md = nullptr;
  memset(g_blend, 0, sizeof(g_blend));
  const int nsBefore = g_nsCount.load();
  SlotStat* st = new SlotStat();
  memset(st, 0, sizeof(*st));
  ++g_passGen;
  const bool ok = AnalyseRaw(vec, 7, *st);
  Check(ok && st->items == 7 && st->modelItems == 5 && st->matsNoEntry == 5,
        "forward rec S0: walk counts items and model draws");
  Check(g_cls[0] == fwdreccount::KindCode(1, false, true, gbreccount::kBmTransparent) && g_cls[1] >= kClsNs0 &&
            g_cls[1] <= kClsNsOver && g_cls[3] == fwdreccount::KindCode(8, false, false, gbreccount::kBmNone) &&
            g_cls[4] == fwdreccount::KindCode(2, true, false, gbreccount::kBmNone) && g_cls[6] == kClsSrNull,
        "forward rec S0: walk classes (model kinds, other renderable, null)");
  Check(st->passIdx[1] == 3 && st->passIdx[2] == 1 && st->passIdx[kPassIdx - 1] == 1 && st->passIdx[0] == 0,
        "forward rec S0: walk FX pass indices (P1: transparent, cockpit, pass 2; P2: pass 2 with r+0x65; pass 8 other route)");
  Check(g_why[0] == kWCensusPending && g_why[1] == 0xff && g_why[2] == kWCensusPending && g_why[3] == kWTechnique &&
            g_why[4] == kWMesh && g_why[5] == kWNoShader && g_why[6] == 0xff,
        "forward rec S0: walk first failing rule (census off, flat_shadow technique, no mesh, no shader)");
  Check((g_flags[0] & kFBlend) && !(g_flags[2] & kFBlend) && (g_flags[3] & kFBlend) && (g_flags[3] & kFAny) &&
            !(g_flags[5] & kFAny) && g_flags[1] == 0 && !(g_flags[0] & kFGlobals),
        "forward rec S0: walk flags (blended, any model draw)");
  delete st;
  g_nsCount = nsBefore;
  memset(g_blend, 0, sizeof(g_blend));
  g_srVt = sSr;
  g_modelMatVt = sMat;
  g_shaderVt = sSh;
  g_mats = sMats;
  fwdreccount::g_census = sCen;
  g_texOk = sTex;
  g_md = sMd;
}

void Run() {
  PureTests();
  WalkTests();
}

}  // namespace frctest
