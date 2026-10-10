// Offline test for vram_count.h: format sizes and texture bytes, the
// orphan/pinned classification, the per-second summary, then a real D3D11
// device (hardware, WARP fallback): the device create hooks count and forward
// and are removed again, a resource's bytes from its description, and the
// refcount rules ResolveViews/Pinned rely on (a view whose texture the
// "owner" released is kept alive by our reference alone; one whose owner
// still holds the texture is not).
#pragma once

namespace vctest {

void Pure() {
  uint32_t bits = 0, block = 0;
  vramc::FormatInfo(DXGI_FORMAT_R8G8B8A8_UNORM, &bits, &block);
  Check(bits == 32 && block == 1, "vram count: RGBA8 is 32 bits per texel");
  vramc::FormatInfo(DXGI_FORMAT_BC1_UNORM_SRGB, &bits, &block);
  Check(bits == 64 && block == 4, "vram count: BC1 is 64 bits per 4x4 block");
  vramc::FormatInfo(DXGI_FORMAT_BC7_UNORM, &bits, &block);
  Check(bits == 128 && block == 4, "vram count: BC7 is 128 bits per 4x4 block");
  vramc::FormatInfo(DXGI_FORMAT_D32_FLOAT_S8X24_UINT, &bits, &block);
  Check(bits == 64, "vram count: D32S8X24 is 64 bits");
  vramc::FormatInfo(DXGI_FORMAT_YUY2, &bits, &block);
  Check(bits == 16, "vram count: YUY2 is 16 bits");
  // 4096^2 BC7, full chain: 16 MB * 4/3 (+ the 4x4 floor of the small mips).
  const uint64_t bc7 = vramc::TextureBytes(DXGI_FORMAT_BC7_UNORM, 4096, 4096, 1, 0, 1, 1);
  uint64_t want = 0;
  for (uint32_t m = 0; m < 13; ++m) {
    const uint64_t w = std::max(1u, 4096u >> m);
    want += ((w + 3) / 4) * ((w + 3) / 4) * 16;
  }
  Check(bc7 == want && vramc::FullMips(4096, 4096, 1) == 13, "vram count: BC7 4096^2 full mip chain bytes");
  Check(vramc::TextureBytes(DXGI_FORMAT_R16G16B16A16_FLOAT, 100, 50, 1, 1, 6, 4) == 100ull * 50 * 8 * 6 * 4,
        "vram count: array and sample count multiply");
  Check(vramc::TextureBytes(DXGI_FORMAT_R8_UNORM, 8, 8, 8, 0, 1, 1) == 512 + 64 + 8 + 1,
        "vram count: 3D texture mips halve the depth too");
  Check(vramc::TextureBytes(DXGI_FORMAT_UNKNOWN, 8, 8, 1, 1, 1, 1) == 0, "vram count: unknown format counts 0");

  // Orphans and pinning: v1/v2 on r1 (both only ours, nobody holds r1 itself): pinned;
  // v3 on r2 (only ours, but someone holds r2 directly): not pinned;
  // v4 on r3 (someone else holds the view): not an orphan, not pinned; v5 stale on r4 (ours only).
  int a, b, c, d, e, r1, r2, r3, r4;
  std::vector<vramc::ViewInfo> v = {
      {&a, &r1, 1, 1, 0, 1000, false}, {&b, &r1, 1, 1, 0, 1000, false}, {&c, &r2, 1, 1, 1, 300, false},
      {&d, &r3, 1, 3, 0, 70, false},  {&e, &r4, 2, 2, 0, 5, true}};
  const vramc::PinResult p = vramc::Pinned(v);
  Check(p.uniqueViews == 5 && p.orphanViews == 4 && p.staleViews == 1, "vram count: orphan and stale views");
  Check(p.uniqueRes == 4 && p.resBytes == 1375 && p.staleRes == 1 && p.staleBytes == 5,
        "vram count: resources and stale bytes");
  Check(p.pinnedRes == 2 && p.pinnedBytes == 1005, "vram count: resources kept alive by us alone");

  // Summary: budget 10 GB; usage 8, 9.5, 10.2 GB; one hitch with 100 MB created and one near the budget.
  std::vector<vramc::Sample> s(5);
  const uint64_t G = 1ull << 30;
  const double use[5] = {8.0, 8.0, 9.5, 10.2, 8.0};
  const double worst[5] = {12, 12, 40, 13, 50};
  for (int i = 0; i < 5; ++i) {
    s[i].local.budget = 10 * G;
    s[i].local.usage = static_cast<uint64_t>(use[i] * G);
    s[i].worstMs = worst[i];
  }
  s[4].texBytes = 100ull << 20;
  s[2].local.usage = static_cast<uint64_t>(9.6 * G);
  const vramc::Summary sm = vramc::Summarize(s);
  Check(sm.n == 5 && sm.secAbove90 == 2 && sm.secAbove100 == 1 && std::fabs(sm.maxUsePct - 102.0) < 0.01,
        "vram count: seconds at and above the budget");
  Check(sm.medianWorstMs == 13 && sm.hitchSecs == 2 && sm.hitchWithCreates == 1 && sm.hitchNearBudget == 1,
        "vram count: hitches against creates and budget");
  Check(std::fabs(sm.growthMb) < 1e-6, "vram count: change over the phase");
}

void Device() {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev,
                               nullptr, &ctx)) &&
      FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr,
                               &ctx))) {
    Check(false, "vram count: create a D3D11 device");
    return;
  }
  // Budget and usage through the adapter.
  Check(vramc::OpenAdapter(dev), "vram count: IDXGIAdapter3 from the device");
  vramc::Sample x;
  vramc::ReadMem(x);
  printf("     local usage %.1f MB of a %.1f MB budget, non-local %.1f MB\n", x.local.usage / 1048576.0,
         x.local.budget / 1048576.0, x.nonLocal.usage / 1048576.0);
  Check(x.local.budget > 0 || x.nonLocal.budget > 0, "vram count: QueryVideoMemoryInfo gives a budget");

  // Create hooks: counted on this device, forwarded, removed.
  void** vt = *reinterpret_cast<void***>(dev);
  void* before = vt[vramc::kSlotTex2D];
  void* beforeBuf = vt[vramc::kSlotBuffer];
  Check(vramc::HookCreates(dev) && vt[vramc::kSlotTex2D] != before, "vram count: device create hooks in");
  D3D11_TEXTURE2D_DESC td{};
  td.Width = td.Height = 256;
  td.MipLevels = 0;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  ID3D11Texture2D* tex = nullptr;
  ID3D11Texture2D* tex2 = nullptr;
  HRESULT hr = dev->CreateTexture2D(&td, nullptr, &tex);
  td.MipLevels = 1;
  std::vector<uint32_t> pixels(256 * 256, 0x12345678u);
  D3D11_SUBRESOURCE_DATA init{pixels.data(), 256 * 4, 0};
  ID3D11Texture2D* dupA = nullptr;
  ID3D11Texture2D* dupB = nullptr;
  dev->CreateTexture2D(&td, &init, &dupA);
  dev->CreateTexture2D(&td, &init, &dupB);
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = 4096;
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  ID3D11Buffer* buf = nullptr;
  dev->CreateBuffer(&bd, nullptr, &buf);
  const uint64_t full = vramc::TextureBytes(DXGI_FORMAT_R8G8B8A8_UNORM, 256, 256, 1, 0, 1, 1);
  const uint64_t one = 256ull * 256 * 4;
  Check(SUCCEEDED(hr) && tex && dupA && dupB && buf && vramc::g_texCreates.load() == 3 &&
            vramc::g_texBytes.load() == full + 2 * one && vramc::g_bufCreates.load() == 1 &&
            vramc::g_bufBytes.load() == 4096,
        "vram count: creates counted with their bytes and forwarded");
  Check(vramc::g_dupCreates == 1 && vramc::g_dupBytes == one, "vram count: same description and data counted once");
  Check(vramc::ResourceBytes(tex) == full && vramc::ResourceBytes(buf) == 4096,
        "vram count: resource bytes from the description");
  vramc::UnhookCreates();
  Check(vt[vramc::kSlotTex2D] == before && vt[vramc::kSlotBuffer] == beforeBuf && !vramc::g_counting.load(),
        "vram count: device table restored");
  dev->CreateTexture2D(&td, nullptr, &tex2);
  Check(tex2 && vramc::g_texCreates.load() == 3, "vram count: nothing counted after the phase");

  // Refcount rules on real views: "DCS" holds tex2 and its view; we add a table reference to each view.
  ID3D11ShaderResourceView* held = nullptr;  // owner still holds tex2
  ID3D11ShaderResourceView* gone = nullptr;  // owner releases dupA and its view
  ID3D11ShaderResourceView* kept = nullptr;  // owner releases the view but keeps dupB (a mip-set view swap)
  dev->CreateShaderResourceView(tex2, nullptr, &held);
  dev->CreateShaderResourceView(dupA, nullptr, &gone);
  dev->CreateShaderResourceView(dupB, nullptr, &kept);
  if (!held || !gone || !kept) {
    Check(false, "vram count: test views");
  } else {
    held->AddRef();  // the table's reference
    gone->AddRef();
    kept->AddRef();
    gone->Release();  // the owner's view reference
    kept->Release();
    dupA->Release();  // the owner's texture reference
    dupA = nullptr;
    std::unordered_map<const void*, vramc::ViewInfo> views;
    views[held] = {held, nullptr, 1, 0, 0, 0, false};
    views[gone] = {gone, nullptr, 1, 0, 0, 0, true};
    views[kept] = {kept, nullptr, 1, 0, 0, 0, false};
    std::vector<vramc::ViewInfo> out;
    vramc::ResolveViews(views, out);
    for (const vramc::ViewInfo& vi : out)
      printf("     %s view: refs %u (ours %u), resource refs %u, %llu bytes\n", vi.view == held ? "held" : vi.view == gone ? "gone" : "kept",
             vi.refs, vi.ours, vi.resRefs, static_cast<unsigned long long>(vi.bytes));
    const vramc::PinResult p = vramc::Pinned(out);
    Check(p.uniqueViews == 3 && p.orphanViews == 2 && p.uniqueRes == 3,
          "vram count: released views are orphans, the held one is not");
    Check(p.pinnedRes == 1 && p.pinnedBytes == one && p.staleBytes == one,
          "vram count: only the released texture counts as kept alive by us");
    held->Release();
    held->Release();
    gone->Release();  // the table's reference: the texture goes now
    kept->Release();
  }
  if (dupB) dupB->Release();
  if (tex2) tex2->Release();
  if (tex) tex->Release();
  if (buf) buf->Release();
  vramc::g_texCreates = vramc::g_texBytes = vramc::g_bufCreates = vramc::g_bufBytes = 0;
  vramc::g_dupCreates = vramc::g_dupBytes = 0;
  vramc::g_byDesc.clear();
  vramc::g_seen.clear();
  if (vramc::g_adapter) {
    vramc::g_adapter->Release();
    vramc::g_adapter = nullptr;
  }
  ctx->Release();
  dev->Release();
}

void Run() {
  Pure();
  Device();
}

}  // namespace vctest
