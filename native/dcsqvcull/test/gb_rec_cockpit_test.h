// Offline tests for gb_rec.h's R24 parts (docs/research/R24_cockpit.md):
//  - execution identity: collection descriptors (shading model, viewport
//    tag, rank) read, found and matched on a fake descriptor array; base
//    frames and the binding of the scope's ordinals in F2 (equal to the plain
//    ordinals), in F1 (cockpit executions apart) and with alternate-frame MFD
//    views inserted before the main viewports (the plain ordinal shifts, the
//    bound identity does not); cockpit slots paired with their view;
//  - cockpit jobs from fake DCS memory: cockpit items residual with the
//    cockpit off (as before), recorded with normal_cockpit*'s key when on, a
//    material drawn in and out of the cockpit residual;
//  - on a real device: cockpit draws (stencil ref 40) recorded by the worker
//    pool with a render-target texture that DCS draws to after the recording
//    and before the pass, executed between DCS's residual draws: bit-exact to
//    the stock pass that samples the new contents; the render-target checks
//    at the exec entry (a target of the pass, drawn since the last mip
//    generation).
// Run with the G-buffer recorder tests (QV_GBREC_ONLY=1 too).
#pragma once

namespace gbcktest {
using namespace gbrec;
using grtest::World;

// ---------------------------------------------------------------------------
// Execution identity
// ---------------------------------------------------------------------------
struct Descs {
  std::vector<uint8_t> b;
  void Add(uint16_t sm, uint32_t tag) {
    b.resize(b.size() + kCollStride);
    uint8_t* d = b.data() + b.size() - kCollStride;
    *reinterpret_cast<uint16_t*>(d) = sm;
    *reinterpret_cast<uint32_t*>(d + kCollTag) = tag;
  }
  size_t n() const { return b.size() / kCollStride; }
};

CollKey Key(uint16_t sm, uint32_t tag, uint16_t rank = 0) {
  CollKey k;
  k.sm = sm;
  k.tag = tag;
  k.rank = rank;
  k.ok = true;
  return k;
}

// One frame's G-buffer executions in graph order. mfd: an MFD sensor view
// first (its own tag, no cockpit); cockpit: F1 (the cockpit pass first in
// each view [V SceneRenderer 0x3a93c before 0x3ac2c, 0x3accd]).
void Frame(FrameSeq& s, bool cockpit, bool mfd, uint32_t tagBase = 0x100, bool sameTags = false) {
  if (mfd) {
    SeqAdd(s, Key(kSmGbuffer, 0x900));
    SeqAdd(s, Key(kSmDecal, 0x900));
  }
  for (uint16_t v = 0; v < 4; ++v) {
    const uint32_t tag = sameTags ? tagBase : tagBase + v;
    const uint16_t rank = sameTags ? v : 0;
    if (cockpit) SeqAdd(s, Key(kSmCockpit, tag, rank));
    SeqAdd(s, Key(kSmGbuffer, tag, rank));
    SeqAdd(s, Key(kSmDecal, tag, rank));
  }
}

void IdentityTests() {
  // Descriptors: cascades, then per view the cockpit (F1), main and decal collections.
  Descs d;
  for (uint32_t v = 0; v < 4; ++v) d.Add(14, 0x50);  // SM_SHADOW_MAP collections, one tag
  for (uint32_t v = 0; v < 4; ++v) {
    d.Add(kSmCockpit, 0x100 + v);
    d.Add(kSmGbuffer, 0x100 + v);
    d.Add(kSmDecal, 0x100 + v);
  }
  CollKey k;
  bool ok = CollKeyAt(d.b.data(), d.n(), 4 + 3 * 2 + 1, &k) && k.ok && k.sm == kSmGbuffer && k.tag == 0x102 &&
            k.rank == 0;
  ok = ok && CollKeyAt(d.b.data(), d.n(), 3, &k) && k.sm == 14 && k.rank == 3;
  ok = ok && CollFindIn(d.b.data(), d.n(), Key(kSmGbuffer, 0x102)) == 4 + 3 * 2 + 1 &&
       CollFindIn(d.b.data(), d.n(), Key(14, 0x50, 2)) == 2 && CollFindIn(d.b.data(), d.n(), Key(kSmGbuffer, 0x104)) < 0 &&
       CollFindIn(d.b.data(), d.n(), Key(kSmGbuffer, 0x102, 1)) < 0;
  ok = ok && CollIsAt(d.b.data(), d.n(), 4 + 3 * 3, Key(kSmCockpit, 0x103)) &&
       !CollIsAt(d.b.data(), d.n(), 4 + 3 * 3, Key(kSmCockpit, 0x102)) &&
       !CollIsAt(d.b.data(), d.n(), 2, Key(14, 0x50, 1)) && CollIsAt(d.b.data(), d.n(), 2, Key(14, 0x50, 2)) &&
       !CollKeyAt(d.b.data(), d.n(), static_cast<uint32_t>(d.n()), &k) && !k.ok;
  // Every view with one tag: the rank tells them apart.
  Descs e;
  for (uint32_t v = 0; v < 4; ++v) {
    e.Add(kSmGbuffer, 7);
    e.Add(kSmDecal, 7);
  }
  for (uint16_t v = 0; v < 4 && ok; ++v)
    ok = CollFindIn(e.b.data(), e.n(), Key(kSmGbuffer, 7, v)) == 2 * v && CollKeyAt(e.b.data(), e.n(), 2 * v + 1, &k) &&
         k.sm == kSmDecal && k.rank == v;
  // A fake render graph: [rg+0x408] begin, [rg+0x410] end.
  alignas(16) uint8_t rg[0x420] = {};
  *reinterpret_cast<uint8_t**>(rg + kCollDescs) = d.b.data();
  *reinterpret_cast<uint8_t**>(rg + kCollDescs + 8) = d.b.data() + d.b.size();
  size_t n = 0;
  ok = ok && CollDescs(rg, &n) == d.b.data() && n == d.n() && CollFindGuarded(rg, Key(kSmDecal, 0x101)) == 4 + 3 + 2 &&
       CollIsGuarded(rg, 4 + 3 + 2, Key(kSmDecal, 0x101)) && CollHash(Key(0, 1)) != CollHash(Key(3, 1)) &&
       CollHash(Key(0, 1)) != CollHash(Key(0, 1, 1)) && CollHash(CollKey()) == 0;
  Check(ok, "gbuffer rec identity: collection descriptors give (shading model, viewport tag, rank), found again by key");

  // F2: every frame a base frame; the binding equals the plain ordinals.
  {
    Binder b;
    Frame(b.cur, false, false);
    const bool c1 = EndFrame(b);
    Frame(b.cur, false, false);
    const bool c2 = EndFrame(b);
    FrameSeq ref;
    Frame(ref, false, false);
    bool same = !c1 && c2 && b.base.n == 8 && b.base.nck == 0;
    for (uint32_t scope : {0x10004u, 0x10055u, 0x1000fu, 0xffffu}) {
      CollKey out[kSlots];
      BindSlots(scope, true, b.base, out);
      for (int s = 0; s < kSlots && same; ++s) {
        const int o = OrdinalOfSlot(scope, s);
        same = s < kScopeSlots ? (o < 0 ? !out[s].ok : SameKey(out[s], ref.id[o])) : !out[s].ok;
      }
    }
    Check(same, "gbuffer rec identity: in F2 a stable sequence binds each scope bit to the execution of that plain "
                "ordinal (no cockpit executions: no cockpit slots)");
  }
  // F1 with MFD sensor views on alternate frames, starting either way.
  for (int startMfd = 0; startMfd < 2; ++startMfd) {
    Binder b;
    int commitAt = -1;
    for (int f = 0; f < 6; ++f) {
      const bool mfd = ((f + startMfd) & 1) != 0;
      Frame(b.cur, true, mfd);
      if (EndFrame(b) && commitAt < 0) commitAt = f;
    }
    CollKey out[kSlots];
    BindSlots(0x10055, true, b.base, out);
    bool bound = commitAt >= 0 && commitAt <= 3 && b.base.n == 8 && b.base.nck == 4 &&
                 b.extraFrames == (startMfd ? 2u : 3u);
    for (int v = 0; v < 4 && bound; ++v)
      bound = SameKey(out[v], Key(kSmGbuffer, 0x100 + v)) && SameKey(out[v + kScopeSlots], Key(kSmCockpit, 0x100 + v));
    // The plain ordinal #2 on an MFD frame is another execution (view 0's main one), the bound key is not.
    FrameSeq mfdFrame;
    Frame(mfdFrame, true, true);
    CollKey o2[kSlots];
    BindSlots(0x10004, true, b.base, o2);
    bound = bound && SameKey(o2[0], Key(kSmGbuffer, 0x101)) && SameKey(o2[kScopeSlots], Key(kSmCockpit, 0x101)) &&
            SameKey(mfdFrame.id[2], Key(kSmGbuffer, 0x100)) && !o2[1].ok;
    BindSlots(0x10004, false, b.base, o2);
    bound = bound && SameKey(o2[0], Key(kSmGbuffer, 0x101)) && !o2[kScopeSlots].ok;
    // A decal execution in scope has no cockpit pair.
    BindSlots(0x10008, true, b.base, o2);
    bound = bound && SameKey(o2[0], Key(kSmDecal, 0x101)) && !o2[kScopeSlots].ok;
    printf("     identity: F1 + MFD frames (first %s): bound after frame %d\n", startMfd ? "MFD" : "plain", commitAt);
    Check(bound, "gbuffer rec identity: F1 with alternate-frame MFD views binds the base frame's ordinals (cockpit "
                 "executions apart, paired with their view by tag and rank); the plain ordinal shifts on MFD frames");
  }
  // One tag for every view: the rank pairs a view's cockpit and main executions.
  {
    Binder b;
    for (int f = 0; f < 2; ++f) {
      Frame(b.cur, true, false, 9, true);
      EndFrame(b);
    }
    CollKey out[kSlots];
    BindSlots(0x10055, true, b.base, out);
    bool r = b.commits == 1;
    for (uint16_t v = 0; v < 4 && r; ++v)
      r = SameKey(out[v], Key(kSmGbuffer, 9, v)) && SameKey(out[v + kScopeSlots], Key(kSmCockpit, 9, v));
    Check(r, "gbuffer rec identity: views sharing one viewport tag are told apart by rank");
  }
  // F2 -> F1: the sequence changes (cockpit executions appear); bound again once stable.
  {
    Binder b;
    for (int f = 0; f < 2; ++f) {
      Frame(b.cur, false, false);
      EndFrame(b);
    }
    Frame(b.cur, true, false);
    const bool c3 = EndFrame(b);
    Frame(b.cur, true, false);
    const bool c4 = EndFrame(b);
    CollKey out[kSlots];
    BindSlots(0x10004, true, b.base, out);
    Check(!c3 && c4 && b.base.nck == 4 && SameKey(out[kScopeSlots], Key(kSmCockpit, 0x101)),
          "gbuffer rec identity: entering the cockpit rebinds after two equal base frames");
  }
  // The executions of an MFD frame find their bound slots whatever their position.
  {
    const int poolWas = g_poolSlots;
    CollKey ids[kSlots];
    for (int s = 0; s < kSlots; ++s) ids[s] = g_slot[s].id;
    Binder b;
    for (int f = 0; f < 2; ++f) {
      Frame(b.cur, true, false);
      EndFrame(b);
    }
    CollKey out[kSlots];
    BindSlots(0x10004, true, b.base, out);
    g_poolSlots = kSlots;
    for (int s = 0; s < kSlots; ++s) g_slot[s].id = out[s];
    FrameSeq mfdFrame;
    Frame(mfdFrame, true, true);
    int hits[kSlots] = {}, none = 0;
    for (uint32_t q = 0; q < mfdFrame.n; ++q) {
      const int s = SlotOfKey(mfdFrame.id[q]);
      s >= 0 ? ++hits[s] : ++none;
    }
    for (uint32_t q = 0; q < mfdFrame.nck; ++q) {
      const int s = SlotOfKey(mfdFrame.ck[q]);
      s >= 0 ? ++hits[s] : ++none;
    }
    g_poolSlots = 4;
    const int capped = SlotOfKey(out[kScopeSlots]);  // no cockpit workers: no cockpit slot
    g_poolSlots = poolWas;
    for (int s = 0; s < kSlots; ++s) g_slot[s].id = ids[s];
    Check(hits[0] == 1 && hits[kScopeSlots] == 1 && none == 12 && capped < 0,
          "gbuffer rec identity: on an MFD frame each bound execution is found once by its key, the rest unslotted");
  }
}

// ---------------------------------------------------------------------------
// Cockpit jobs (fake DCS memory)
// ---------------------------------------------------------------------------
constexpr uint64_t kTechCk = 2;  // [mat+0x1e0] of materials 0-3 (shader 0)

// Shader 0's materials are cockpit materials and their items cockpit items.
void MakeCockpit(World& w) {
  for (int m = 0; m < 4; ++m) *reinterpret_cast<uint64_t*>(w.mat[m] + 0x1e0) = kTechCk;
  for (int m = 4; m < grtest::kMats; ++m) *reinterpret_cast<uint64_t*>(w.mat[m] + 0x1e0) = 0;
  for (int i = 0; i < grtest::kN; ++i) w.rend[i][0x64] = (w.kind[i] == 0 && w.MatOf(i) < 4) ? 1 : 0;
  memcpy(w.matInit, w.mat, sizeof(w.mat));
}

void PubCockpit(Tables& t, World& w) {
  GbKey* k = NewKey(t, w.shader[0], kTechCk, 0);
  k->tech = kTechCk;
  k->flags = 0;
  k->effect = *reinterpret_cast<void**>(w.shader[0] + 0x50);
  k->techBegin = *reinterpret_cast<void**>(w.shader[0] + 0xb0);
  k->st.cbSlot = 2;
  k->st.sbSlot = 3;
  k->st.ctxMask = 1u << 7;
  k->psCbMask = 1u << 2;
  k->psTexCount = 2;
  k->psTexH[0] = 0;
  k->psTexSlot[0] = 0;
  k->psTexH[1] = 1;
  k->psTexSlot[1] = 1;
  k->stencilRef = 40;
  k->state.store(1);
  PublishKey(t, *k, w.shader[0]);
  ReadsEntry* e = NewReads(t, w.shader[0], kTechCk);
  e->tech = kTechCk;
  e->effect = k->effect;
  e->techBegin = k->techBegin;
  e->state = 1;
  e->mask[0] = 0x3;
  e->count = 2;
  PublishReads(t, *e, w.shader[0]);
  for (int m = 0; m < grtest::kMeshes; ++m) {
    shrec::MeshLive live;
    shrec::ReadMesh(w.mesh[m], live);
    shrec::MeshEntry* me = shrec::NewMesh(t.mesh, w.mesh[m], w.shader[0], kTechCk);
    me->shader = w.shader[0];
    me->tech = kTechCk;
    me->effect = k->effect;
    me->techBegin = k->techBegin;
    me->fp = live.fp;
    me->vb = live.vb;
    me->stride = live.stride;
    me->ib = live.ib;
    me->ibFormat = live.ibFormat;
    me->indexCount = 3 * live.count;
    me->state.store(1);
    shrec::PublishMesh(t.mesh, *me, w.mesh[m]);
  }
}

void CockpitJobTests() {
  World* w = new World;
  w->Init();
  MakeCockpit(*w);
  Tables t;
  AllocTables(t);
  TexTable tt;
  AllocTex(tt, 256);
  srtest::FakeView views[grtest::kTex], pages[grtest::kPages];
  for (int k = 0; k < grtest::kTex; ++k) w->SetView(k, &views[k]);
  for (int p = 0; p < grtest::kPages; ++p) w->SetPageSrv(p, &pages[p]);
  grtest::PubAll(t, *w);
  PubCockpit(t, *w);
  grtest::FillTex(tt, *w);
  Job* j = NewJob();
  auto build = [&](bool cockpit) {
    grtest::InitJob(*j, *w, t, tt);
    j->cockpit = cockpit;
    return BuildJob(*j) == kBuildOk;
  };
  // Off: as before R24 (cockpit items stay DCS's, their shader unclean).
  bool off = build(false);
  uint32_t ck = 0;
  for (int i = 0; i < grtest::kN && off; ++i) {
    if (!w->rend[i][0x64] || i == grtest::kPass2Item) continue;
    ++ck;
    off = j->items[i].reason == kRCockpit;
  }
  const uint32_t recOff = j->recorded;
  Check(off && ck > 100 && j->reasons[kRCockpit] == ck,
        "gbuffer rec cockpit: off, every cockpit item is residual (cockpit technique), as before");
  // On: cockpit items are candidates with normal_cockpit*'s key, mesh and reads.
  bool on = build(true);
  uint32_t ckRec = 0;
  for (int i = 0; i < grtest::kN && on; ++i) {
    const Item& it = j->items[i];
    if (!w->rend[i][0x64]) continue;
    on = it.reason == kRecorded || it.reason == kRIsland || it.reason == kRSegments;
    if (it.reason == kRecorded) {
      ++ckRec;
      on = on && it.cand != ~0u && j->cands[it.cand].key && j->cands[it.cand].key->tech == kTechCk &&
           j->cands[it.cand].key->stencilRef == 40 && j->mats[it.mat].ck && j->mats[it.mat].tech == kTechCk;
    }
    if (!on) printf("     cockpit item %d: reason %d\n", i, it.reason);
  }
  printf("     cockpit jobs: %u recorded with the cockpit off, %u with it on (%u cockpit items)\n", recOff, j->recorded,
         ckRec);
  Check(on && ckRec > 60 && j->recorded > recOff && j->reasons[kRCockpit] == 0,
        "gbuffer rec cockpit: on, cockpit items are recorded with the normal_cockpit* key (technique [mat+0x1e0])");
  // A material drawn both in and out of the cockpit in one vector: DCS's.
  int flip = -1;
  for (int i = 0; i < grtest::kN && flip < 0; ++i)
    if (w->rend[i][0x64] && w->MatOf(i) == 1) flip = i;
  w->rend[flip][0x64] = 0;
  bool mix = build(true);
  for (int i = 0; i < grtest::kN && mix; ++i)
    if (w->kind[i] == 0 && w->MatOf(i) == 1 && i != grtest::kPass2Item)
      mix = j->items[i].reason == kRCockpitMix;
  w->rend[flip][0x64] = 1;
  Check(mix && j->reasons[kRCockpitMix] > 0,
        "gbuffer rec cockpit: a material with cockpit and other items in one vector stays DCS's");
  FreeJob(j);
  FreeTex(tt);
  FreeTables(t);
  delete w;
}

// ---------------------------------------------------------------------------
// Device: cockpit draws with a render-target texture written before use
// ---------------------------------------------------------------------------
void CrashRegressionTests(ID3D11Device* dev);

void CockpitDeviceTests(srtest::Compiler& comp) {
  World* w = new World;
  w->Init();
  MakeCockpit(*w);
  grtest::Dev d;
  bool warp = false;
  if (!grtest::CreateDev(d, comp, *w, &warp)) {
    Check(false, "gbuffer rec cockpit device: test objects");
    grtest::Destroy(d);
    delete w;
    return;
  }
  // Texture 0 (a read Diffuse) becomes a render target: an MFD display that DCS draws to each frame.
  grtest::Rel(d.texSrv[0]);
  grtest::Rel(d.texRes[0]);
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = td.Height = 8;
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  ID3D11RenderTargetView* mfdRtv = nullptr;
  d.dev->CreateTexture2D(&td, nullptr, &d.texRes[0]);
  if (d.texRes[0]) {
    d.dev->CreateShaderResourceView(d.texRes[0], nullptr, &d.texSrv[0]);
    d.dev->CreateRenderTargetView(d.texRes[0], nullptr, &mfdRtv);
  }
  w->SetView(0, d.texSrv[0]);
  if (!d.texSrv[0] || !mfdRtv) {
    Check(false, "gbuffer rec cockpit device: render-target texture");
    grtest::Rel(mfdRtv);
    grtest::Destroy(d);
    delete w;
    return;
  }
  auto writeMfd = [&](float r, float g) {
    const float c[4] = {r, g, 0.5f, 1.0f};
    d.imm->ClearRenderTargetView(mfdRtv, c);
  };
  auto stock = [&](const TexTable& tt) {
    grtest::ResetDcsState(*w);
    grtest::PassSetup(d);
    for (int i = 0; i < grtest::kN; ++i) grtest::DrawItem(d, *w, i, tt);
    return grtest::ReadTargets(d);
  };
  Tables t;
  AllocTables(t);
  TexTable tt;
  AllocTex(tt, 256);
  grtest::FillTex(tt, *w);
  for (int s = 0; s < grtest::kShaders; ++s) grtest::PubReads(t, *w, s);
  {
    ReadsEntry* e = NewReads(t, w->shader[0], kTechCk);
    e->tech = kTechCk;
    e->effect = *reinterpret_cast<void**>(w->shader[0] + 0x50);
    e->techBegin = *reinterpret_cast<void**>(w->shader[0] + 0xb0);
    e->state = 1;
    e->mask[0] = 0x3;
    e->count = 2;
    PublishReads(t, *e, w->shader[0]);
  }
  writeMfd(0.25f, 0.75f);
  const std::vector<uint8_t> refA = stock(tt);
  // Probes (DCS's draws read back): the cockpit key learns stencil ref 40.
  grtest::ResetDcsState(*w);
  grtest::PassSetup(d);
  std::set<std::pair<int, int>> probed;
  for (int i = 0; i < grtest::kN; ++i)
    if (grtest::IsCandShader(*w, i) && probed.insert({World::ShaderOfMat(w->MatOf(i)), World::MeshOf(i)}).second)
      grtest::ProbeOn(d, *w, t, i, tt);
  const GbKey* kc = FindKey(t, w->shader[0], kTechCk, 0, *reinterpret_cast<void**>(w->shader[0] + 0x50),
                            *reinterpret_cast<void**>(w->shader[0] + 0xb0));
  Check(kc && kc->state.load() > 0 && kc->stencilRef == 40,
        "gbuffer rec cockpit device: the probe of a cockpit draw publishes its key with stencil ref 40");
  defrec::Pool pool;
  defrec::PoolConfig pc;
  pc.workers = kThreads;
  pc.cb.mode = defrec::CbMode::kOffsets;
  pc.name = "gbuffer rec cockpit test";
  Job* j = NewJob();
  if (!pool.Start(d.dev, pc) || pool.At(0).ring.Mode() != defrec::CbMode::kOffsets || !j) {
    printf("SKIP gbuffer rec cockpit device: no worker with constant-buffer offsets on this device\n");
  } else {
    grtest::InitJob(*j, *w, t, tt);
    j->cockpit = true;
    j->splitMin = 16;
    ReleaseTargetRefs(*j);
    for (int k = 0; k < grtest::kTargets; ++k) {
      j->rtv[k] = d.rtv[k];
      j->rtv[k]->AddRef();
    }
    j->dsv = d.dsv;
    j->dsv->AddRef();
    j->nvp = 1;
    j->vp[0] = grtest::kVpFull;
    j->nsc = 0;
    j->ctxBuf[0][0] = d.ours[0][0];
    j->ctxBuf[0][1] = d.ours[0][1];
    j->ctxBuf[1][0] = d.ours[1][0];
    j->ctxBuf[1][1] = d.ours[1][1];
    for (int st = 0; st < 2; ++st) {
      j->pool[st][5] = d.pool;
      d.pool->AddRef();
    }
    std::vector<ID3D11CommandList*> cls;
    grtest::ResetDcsState(*w);
    const bool rec = grtest::RecordOnPool(pool, *j, cls, false);
    uint32_t ckRec = 0;
    for (int i = 0; i < grtest::kN; ++i) ckRec += w->rend[i][0x64] && j->items[i].reason == kRecorded;
    // DCS draws the MFD display after the lists were recorded, before the pass (its writer precedes the reader).
    writeMfd(0.9f, 0.1f);
    const std::vector<uint8_t> refB = stock(tt);
    bool ss = false;
    grtest::ResetDcsState(*w);
    const bool run = rec && grtest::RunRecorded(d, *w, *j, cls, tt, false, &ss);
    const std::vector<uint8_t> got = grtest::ReadTargets(d);
    size_t diff = refB.size() == got.size() ? 0 : SIZE_MAX, diffA = 0;
    for (size_t k = 0; diff != SIZE_MAX && k < refB.size(); ++k) diff += refB[k] != got[k];
    for (size_t k = 0; k < refA.size() && k < refB.size(); ++k) diffA += refA[k] != refB[k];
    printf("     cockpit: %u of %d items recorded (%u cockpit) in %u segment(s); render target redrawn after the "
           "recording: %zu bytes differ from stock (the redraw changes %zu)\n",
           j->recorded, grtest::kN, ckRec, j->segCount, diff, diffA);
    Check(rec && run && ss && ckRec > 60 && diff == 0 && diffA > 0,
          "gbuffer rec cockpit device: cockpit draws (stencil ref 40) sampling a render target that DCS draws to after "
          "the recording and before the pass equal the stock pass bit for bit (the lists sample it when executed)");
    for (auto*& l : cls) grtest::Rel(l);
    // The exec entry's render-target checks (R24 3.4).
    alignas(16) uint8_t fakeTex[0x200] = {}, inner[0x60] = {}, innerDesc[0x50] = {};
    *reinterpret_cast<uint8_t**>(fakeTex + 0x10) = inner;
    *reinterpret_cast<uint8_t**>(inner + 8) = innerDesc;
    innerDesc[0x44] = 0x10;  // mips
    TexEntry e;
    e.rt = 1;
    e.tex = fakeTex;
    e.nviews = 1;
    e.views[0] = d.texSrv[0];
    TexEntry plain;
    plain.tex = fakeTex;
    plain.nviews = 1;
    plain.views[0] = d.texSrv[1];
    j->segEnts[0] = &plain;
    j->segEnts[1] = &e;
    Seg sg = {};
    sg.entFirst = 0;
    sg.entEnd = 2;
    uint32_t seen = 0, drawn = 0;
    const int whyFree = RtCheckGuarded(*j, sg, &seen, &drawn);
    const bool c1 = whyFree == kSExecuted && seen == 1 && drawn == 1;
    inner[0x54] = 1;  // mips generated since the last draw to it
    seen = drawn = 0;
    const int whyGen = RtCheckGuarded(*j, sg, &seen, &drawn);
    const bool c2 = whyGen == kSExecuted && seen == 1 && drawn == 0;
    grtest::Rel(j->rtv[3]);
    j->rtv[3] = mfdRtv;  // the pass draws to that texture
    mfdRtv->AddRef();
    seen = drawn = 0;
    const int whyBound = RtCheckGuarded(*j, sg, &seen, &drawn);
    Check(c1 && c2 && whyBound == kSRtBound,
          "gbuffer rec cockpit device: exec-entry render-target checks (drawn since the last mip generation counted; "
          "a target of the pass makes the segment stock)");
    ReleaseTargetRefs(*j);
  }
  pool.Stop();
  CrashRegressionTests(d.dev);
  FreeJob(j);
  FreeTex(tt);
  FreeTables(t);
  grtest::Rel(mfdRtv);
  grtest::Destroy(d);
  delete w;
}

// ---------------------------------------------------------------------------
// The R24 F1 crash: cockpit slots asked the pool for 8 x 3 workers, the pool
// gave 16, and PumpRedo picked worker s + 16 (outside the pool's array) on the
// first frames after the bindings. Now: 8 x 2, every worker index checked,
// and a fault in the recorder's entry points is contained (recorder off).
// ---------------------------------------------------------------------------
void CrashRegressionTests(ID3D11Device* dev) {
  int sl = 0, th = 0;
  PoolShape(true, &sl, &th);
  const bool shape8 = sl == 8 && th == 2 && sl * th <= defrec::Pool::kMaxWorkers;
  PoolShape(false, &sl, &th);
  Check(shape8 && sl == 4 && th == 3, "gbuffer rec crash: the pool shape fits 16 workers (4 x 3; cockpit 8 x 2)");
  const int slotsWas = g_poolSlots, threadsWas = g_threads;
  const bool idWas = g_idMode, ckWas = g_cockpit.load(), onWas = g_on.load();
  const uint32_t scopeWas = g_scope.load();
  defrec::PoolConfig pc;
  pc.workers = 24;  // what the crashed build asked for
  pc.cb.mode = defrec::CbMode::kOffsets;
  pc.name = "gbuffer rec crash test";
  if (!g_pool.Start(dev, pc)) {
    printf("SKIP gbuffer rec crash: no worker pool on this device\n");
    return;
  }
  g_pool.Disable("test: nothing is submitted");
  // The crashed shape: worker 2 of slot s is 16 + s, outside a 16-worker pool.
  g_poolSlots = kSlots;
  g_threads = kThreads;
  const bool refused = g_pool.Workers() == 16 && !WkOk(0, 2) && !WkOk(7, 2) && WkOk(7, 1) && !WkOk(8, 0);
  // The fixed shape, the F1 bindings of the crash log (8 non-cockpit, 4 cockpit executions), redo planned at once.
  PoolShape(true, &g_poolSlots, &g_threads);
  g_idMode = true;
  g_cockpit = true;
  g_scope = 0x10055;
  g_bind = Binder();
  for (int f = 0; f < 3; ++f) {
    Frame(g_bind.cur, true, f == 1);
    if (EndFrame(g_bind)) Rebind();
  }
  bool bound = true;
  for (int s = 0; s < kScopeSlots; ++s)
    bound = bound && g_slot[s].id.ok && g_slot[s].id.sm == kSmGbuffer && g_slot[s + kScopeSlots].id.ok &&
            g_slot[s + kScopeSlots].id.sm == kSmCockpit && g_slot[s + kScopeSlots].job;
  bool pumped = true;
  for (int s = 0; s < kSlots; ++s) {
    Job* j = g_slot[s].job;
    if (!j) continue;
    j->segCount = 2;
    for (uint32_t k = 0; k < 2; ++k) j->redoState[k].store(kRedoWant);
    PumpRedo(s, *j);
    for (uint32_t k = 0; k < 2; ++k)
      pumped = pumped && j->redoState[k].load() == kRedoFail && j->redoArg[k].worker < g_threads &&
               Wk(s, j->redoArg[k].worker) < g_pool.Workers();
    j->segCount = 0;
    for (auto& st : j->redoState) st.store(kRedoNone);
  }
  // The bindings change (back to F2) with nothing running: the cockpit slots are unbound.
  for (int f = 0; f < 2; ++f) {
    Frame(g_bind.cur, false, false);
    if (EndFrame(g_bind)) Rebind();
  }
  bool unbound = true;
  for (int s = kScopeSlots; s < kSlots; ++s) unbound = unbound && !g_slot[s].id.ok;
  const bool clean = !g_disabled.load() && g_contained.load() == 0;
  Check(refused && bound && pumped && unbound && clean,
        "gbuffer rec crash: cockpit slots bound on the first F1 frames, redo pumped for all 8 slots on valid workers "
        "only, bindings changed again; no fault");
  // Containment: a fault in PumpRedo and in the sort observer latches the recorder off instead of crashing.
  void* bad = VirtualAlloc(nullptr, sizeof(Job), MEM_RESERVE, PAGE_NOACCESS);
  PumpRedo(0, *static_cast<Job*>(bad));
  const bool c1 = g_disabled.load() && g_contained.load() == 1;
  g_disabled = false;
  g_on = true;
  Job* pub = g_jobPub[0].load();
  g_jobPub[0].store(static_cast<Job*>(bad));
  void* vec[3] = {};
  OnSorted(vec);
  g_jobPub[0].store(pub);
  const bool c2 = g_disabled.load() && g_contained.load() == 2;
  Check(c1 && c2 && t_dcs == 0, "gbuffer rec crash: faults in PumpRedo and the sort observer are contained (recorder "
                                "latched off)");
  VirtualFree(bad, 0, MEM_RELEASE);
  // Restore the globals.
  g_pool.Stop();
  for (int s = 0; s < kSlots; ++s) {
    Slot& slt = g_slot[s];
    g_jobPub[s].store(nullptr);
    FreeJob(slt.job);
    if (slt.stockList) VirtualFree(slt.stockList, 0, MEM_RELEASE);
    slt.stockList = nullptr;
    slt.id = CollKey();
    slt.armed = false;
  }
  g_disabled = false;
  g_contained = 0;
  g_on = onWas;
  g_bind = Binder();
  g_poolSlots = slotsWas;
  g_threads = threadsWas;
  g_idMode = idWas;
  g_cockpit = ckWas;
  g_scope = scopeWas;
}

void Run() {
  IdentityTests();
  CockpitJobTests();
  srtest::Compiler c;
  if (c.Load() && c.compile) CockpitDeviceTests(c);
}

}  // namespace gbcktest
