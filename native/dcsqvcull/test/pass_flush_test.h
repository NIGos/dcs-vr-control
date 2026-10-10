// Offline test for pass_flush.h: pass kinds from RTTI names, the Flush
// decisions per mask bit on synthetic frames (top-level and nested cascades,
// G-buffer runs, first pass, mask changes), the paired A/B statistics, then a
// real D3D11 device (hardware, WARP fallback) driven through pass_timing.h's
// execute hook with fake pass objects whose RTTI names match DCS's: the render
// thread latches at the first cascade, Flush really runs and is counted per
// reason, other threads never flush, and mask 0 removes the callback.
#pragma once

namespace pftest {

// Fake pass objects (structs: RTTI ".?AU<name>@...", as DCS's PassData templates).
struct CascadeShadowPassData {
  virtual ~CascadeShadowPassData() {}
  int pad[30] = {};
};
struct GBufferPassData {
  virtual ~GBufferPassData() {}
};
struct LightingModule {
  virtual ~LightingModule() {}
};

using pflush::kCascade;
using pflush::kGBuffer;
using pflush::kOther;

struct Ev {
  bool begin;
  uint8_t kind;
  int depth;
  bool first;
};

// Runs a sequence; returns the reason bits per event (0 = no Flush).
std::vector<uint32_t> Play(pflush::Planner& p, const std::vector<Ev>& evs) {
  std::vector<uint32_t> r;
  for (const Ev& e : evs) r.push_back(e.begin ? p.Begin(e.kind, e.depth, e.first) : p.End(e.kind, e.depth));
  return r;
}
int Count(const std::vector<uint32_t>& r) {
  int n = 0;
  for (uint32_t x : r) n += x != 0;
  return n;
}

// One frame of top-level passes: Generator, 4 cascades, G-buffer #0 #1, Lighting, G-buffer #2 #3, Lighting.
std::vector<Ev> Frame() {
  std::vector<Ev> f;
  auto pass = [&](uint8_t k, bool first = false) {
    f.push_back({true, k, 0, first});
    f.push_back({false, k, 0, false});
  };
  pass(kOther, true);
  for (int c = 0; c < 4; ++c) pass(kCascade);
  pass(kGBuffer);
  pass(kGBuffer);
  pass(kOther);
  pass(kGBuffer);
  pass(kGBuffer);
  pass(kOther);
  return f;
}

void Logic() {
  Check(pflush::KindFromName("CascadeShadowPassData") == kCascade &&
            pflush::KindFromName("CascadeShadowPassData (shadow)") == kCascade &&
            pflush::KindFromName("GBufferPassData") == kGBuffer && pflush::KindFromName("PassData") == kOther &&
            pflush::KindFromName("GBufferPassDataX") == kOther && pflush::KindFromName("SimplePassData") == kOther,
        "pass flush: pass kinds from RTTI names");
  std::vector<Ev> two = Frame();
  {
    std::vector<Ev> f2 = Frame();
    two.insert(two.end(), f2.begin(), f2.end());
  }
  {
    pflush::Planner p;
    p.mask = 0;
    Check(Count(Play(p, two)) == 0, "pass flush: mask 0 never flushes");
  }
  {
    pflush::Planner p;
    p.mask = pflush::kCascadeGroup;
    std::vector<uint32_t> r = Play(p, Frame());
    // Flush at the start of G-buffer #0 (event 10), once.
    Check(Count(r) == 1 && r[10] == pflush::kCascadeGroup, "pass flush: cascade group flushes once, after the last cascade");
  }
  {
    pflush::Planner p;
    p.mask = pflush::kEachCascade | pflush::kCascadeGroup;
    std::vector<uint32_t> r = Play(p, Frame());
    Check(Count(r) == 4 && r[3] == pflush::kEachCascade && r[9] == pflush::kEachCascade && r[10] == 0,
          "pass flush: each cascade (the group flush is then not scheduled)");
  }
  {
    pflush::Planner p;
    p.mask = pflush::kGBufferGroup;
    std::vector<uint32_t> r = Play(p, two);
    Check(Count(r) == 4 && r[14] == pflush::kGBufferGroup && r[20] == pflush::kGBufferGroup && r[12] == 0,
          "pass flush: g-buffer group flushes after each run of executions");
  }
  {
    pflush::Planner p;
    p.mask = pflush::kEachGBuffer;
    Check(Count(Play(p, two)) == 8, "pass flush: each g-buffer execution");
  }
  {
    pflush::Planner p;
    p.mask = pflush::kEachTop;
    Check(Count(Play(p, Frame())) == 11, "pass flush: each top-level pass");
  }
  {
    pflush::Planner p;
    p.mask = pflush::kFirstPass;
    std::vector<uint32_t> r = Play(p, two);
    Check(Count(r) == 2 && r[0] == pflush::kFirstPass && r[22] == pflush::kFirstPass,
          "pass flush: before the first pass of each frame");
  }
  {
    // Cascades nested in a shadow module: the group flush comes at the module's end.
    pflush::Planner p;
    p.mask = pflush::kCascadeGroup | pflush::kEachTop;
    std::vector<Ev> f = {{true, kOther, 0, true}};
    for (int c = 0; c < 4; ++c) {
      f.push_back({true, kCascade, 1, false});
      f.push_back({false, kCascade, 1, false});
    }
    f.push_back({false, kOther, 0, false});
    std::vector<uint32_t> r = Play(p, f);
    Check(Count(r) == 1 && r.back() == (pflush::kCascadeGroup | pflush::kEachTop),
          "pass flush: nested cascades, one Flush at the parent's end for both reasons");
  }
  {
    // A pending group flush is dropped when the mask loses its bit.
    pflush::Planner p;
    p.mask = pflush::kCascadeGroup;
    p.End(kCascade, 0);
    p.mask = 0;
    Check(p.Begin(kOther, 0, false) == 0 && p.pending == kOther, "pass flush: pending group dropped by a mask change");
  }
  {
    // Bench pairs: OFF 10, ON 9, OFF 11, ON 8, OFF 10 (one invalid OFF block).
    const std::vector<double> v = {10, 9, 11, 8, 10, 100};
    const std::vector<bool> on = {false, true, false, true, false, true};
    const std::vector<bool> ok = {true, true, true, true, true, false};
    const pflush::Paired p = pflush::PairedChange(v, on, ok);
    const double d1 = 9 - 10.5, d2 = 8 - 10.5;
    Check(p.pairs == 2 && std::fabs(p.delta - (d1 + d2) / 2) < 1e-9 && std::fabs(p.on - 8.5) < 1e-9 &&
              std::fabs(p.off - 31.0 / 3) < 1e-9 && p.ci > 0,
          "pass flush: paired change against the OFF neighbours");
  }
  Check(pflush::MaskText(0x82) == "cascade group, before first pass" && pflush::MaskText(0) == "none",
        "pass flush: mask text");
}

std::vector<int> g_execDepth;
void __fastcall FakeExec(void*, uint64_t) { g_execDepth.push_back(ptiming::t_depth); }

void ResetState() {
  pflush::g_rt = 0;
  pflush::g_lastFrame = ~0ull;
  pflush::g_plan = pflush::Planner{};
  pflush::g_flushes = 0;
  pflush::g_flushTicks = 0;
  for (auto& r : pflush::g_byReason) r = 0;
}

void Device() {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev,
                               nullptr, &ctx)) &&
      FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr,
                               &ctx))) {
    Check(false, "pass flush: create a D3D11 device");
    return;
  }
  CascadeShadowPassData casc;
  GBufferPassData gb;
  LightingModule light;
  Check(pflush::KindOf(&casc) == kCascade && pflush::KindOf(&gb) == kGBuffer && pflush::KindOf(&light) == kOther &&
            pflush::KindOf(&casc) == kCascade && pflush::KindOf(nullptr) == kOther,
        "pass flush: kinds from the pass objects' RTTI, cached by vtable");

  const ptiming::ExecFn origSave = ptiming::g_orig;
  ptiming::g_orig = &FakeExec;
  std::atomic<uint64_t> frames{0};
  ResetState();
  Check(pflush::Prepare(dev, &frames) && pflush::Ready(), "pass flush: prepare (immediate context)");
  const uint32_t mask = pflush::kEachCascade | pflush::kGBufferGroup | pflush::kEachTop | pflush::kRecExecute |
                        pflush::kFirstPass;
  pflush::SetMask(mask);
  Check(ptiming::g_flushBoundary.load() == &pflush::OnBoundary, "pass flush: callback installed with a mask");
  g_execDepth.clear();

  // Before any cascade: nothing latched, nothing flushed.
  ++frames;
  ptiming::Hook(&light, 0);
  pflush::AfterExecute();
  Check(pflush::g_rt.load() == 0 && pflush::g_flushes.load() == 0, "pass flush: no Flush before the render thread latches");

  // Three frames: Lighting, 4 cascades (each with a recorder Execute inside: called after the hook here),
  // 2 G-buffer executions, Lighting.
  ID3D11Texture2D* tex = nullptr;
  D3D11_TEXTURE2D_DESC td{};
  td.Width = td.Height = 16;
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_RENDER_TARGET;
  dev->CreateTexture2D(&td, nullptr, &tex);
  ID3D11RenderTargetView* rtv = nullptr;
  if (tex) dev->CreateRenderTargetView(tex, nullptr, &rtv);
  const float red[4] = {1, 0, 0, 1};
  for (int f = 0; f < 3; ++f) {
    ++frames;
    ptiming::Hook(&light, 0);
    for (int c = 0; c < 4; ++c) {
      if (rtv) ctx->ClearRenderTargetView(rtv, red);
      ptiming::Hook(&casc, 0);
      pflush::AfterExecute();
    }
    ptiming::Hook(&gb, 0);
    ptiming::Hook(&gb, 0);
    ptiming::Hook(&light, 0);
  }
  auto R = [](uint32_t bit) {
    unsigned long b;
    _BitScanForward(&b, bit);
    return pflush::g_byReason[b].load();
  };
  printf("     flushes %llu: cascade %llu, g-buffer group %llu, top %llu, execute %llu, first %llu\n",
         static_cast<unsigned long long>(pflush::g_flushes.load()), static_cast<unsigned long long>(R(pflush::kEachCascade)),
         static_cast<unsigned long long>(R(pflush::kGBufferGroup)), static_cast<unsigned long long>(R(pflush::kEachTop)),
         static_cast<unsigned long long>(R(pflush::kRecExecute)), static_cast<unsigned long long>(R(pflush::kFirstPass)));
  // Latched at frame 0's first cascade begin, which is then that frame's first pass seen. Per frame: each
  // cascade 4, execute 4, g-buffer group 1 (at the second Lighting begin), top-level ends 8 (7 in frame 0, whose
  // first Lighting ran before the latch; cascade ends share their Flush with the cascade reason).
  Check(pflush::g_rt.load() == GetCurrentThreadId(), "pass flush: render thread latched at the first cascade");
  Check(R(pflush::kEachCascade) == 12 && R(pflush::kRecExecute) == 12 && R(pflush::kGBufferGroup) == 3 &&
            R(pflush::kFirstPass) == 3 && R(pflush::kEachTop) == 7 + 8 + 8,
        "pass flush: Flush reasons counted per boundary");
  // Flushes: frame 0: first (c0 begin) + 4 cascade ends + 4 executes + 2 g-buffer ends + group (Lighting begin)
  // + Lighting end = 13; frames 1-2: first + 8 top-level ends + 4 executes + group = 14.
  Check(pflush::g_flushes.load() == 13 + 14 + 14, "pass flush: one Flush per boundary, however many reasons");
  Check(g_execDepth.size() == 1 + 3 * 8 && g_execDepth[0] == 1, "pass flush: the execute hook forwards every pass");

  // Another thread never flushes.
  const uint64_t before = pflush::g_flushes.load();
  std::thread other([&] {
    ptiming::Hook(&casc, 0);
    pflush::AfterExecute();
  });
  other.join();
  Check(pflush::g_flushes.load() == before, "pass flush: passes on other threads do not flush");

  // Mask 0: the callback goes; a session keeps it with no Flush.
  pflush::SetMask(0);
  Check(ptiming::g_flushBoundary.load() == nullptr, "pass flush: mask 0 removes the callback");
  pflush::SetSession(true);
  ++frames;
  ptiming::Hook(&casc, 0);
  pflush::AfterExecute();
  Check(ptiming::g_flushBoundary.load() == &pflush::OnBoundary && pflush::g_flushes.load() == before,
        "pass flush: bench session with mask 0: callback kept, no Flush");
  // Frame-start probe: xrEndFrame end, xrBeginFrame wrapper times, then the next first pass (and its CPU time).
  pflush::ProbeReset();
  pflush::g_probe = true;
  pflush::NoteXrEnd();
  pflush::g_tB0 = pflush::Qpc();
  pflush::g_tB1 = pflush::g_tB0 + 1;
  ++frames;
  ptiming::Hook(&light, 0);
  ptiming::Hook(&casc, 0);
  pflush::g_probe = false;
  const pflush::Probe& pr = pflush::g_pr;
  Check(pr.frames == 1 && pr.withXr == 1 && pr.withBegin == 1 && pr.counterFirst == 1 && pr.firstPassN == 1 &&
            pr.firstVt == &light && pr.xrToPass.size() == 1 && pr.xrToPassUs >= 0,
        "pass flush: frame-start probe, one frame from xrEndFrame to the first pass");
  pflush::SetSession(false);
  Check(ptiming::g_flushBoundary.load() == nullptr, "pass flush: session end removes the callback");

  if (rtv) rtv->Release();
  if (tex) tex->Release();
  pflush::Shutdown();
  Check(!pflush::Ready(), "pass flush: shutdown releases the context");
  ResetState();
  ptiming::g_orig = origSave;
  ctx->Release();
  dev->Release();
}

void Run() {
  Logic();
  Device();
}

}  // namespace pftest
