// Offline tests for shadow_batch.h's depth compare (the 2026-10-08 live crash
// came from reading 4 bytes per texel of a narrower depth format).
#pragma once

namespace sbtest {

ID3D11Texture2D* MakeTex(ID3D11Device* dev, DXGI_FORMAT f, UINT w, UINT h, UINT slices, UINT bpt, uint8_t fill,
                         int poke) {
  std::vector<uint8_t> data(static_cast<size_t>(w) * h * bpt * slices, fill);
  if (poke >= 0) data[static_cast<size_t>(poke) * bpt] ^= 0x5a;
  std::vector<D3D11_SUBRESOURCE_DATA> init(slices);
  for (UINT s = 0; s < slices; ++s)
    init[s] = {data.data() + static_cast<size_t>(s) * w * h * bpt, w * bpt, w * h * bpt};
  D3D11_TEXTURE2D_DESC d = {};
  d.Width = w;
  d.Height = h;
  d.MipLevels = 1;
  d.ArraySize = slices;
  d.Format = f;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  ID3D11Texture2D* t = nullptr;
  dev->CreateTexture2D(&d, init.data(), &t);
  return t;
}

void Run() {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr,
                               &ctx))) {
    Check(false, "shadow batch: WARP device for the depth compare test");
    return;
  }
  shadowbatch::g_ctx = ctx;
  struct Case {
    DXGI_FORMAT f;
    UINT bpt;
  } cases[] = {{DXGI_FORMAT_R16_TYPELESS, 2}, {DXGI_FORMAT_R32_TYPELESS, 4}, {DXGI_FORMAT_R32G8X24_TYPELESS, 8}};
  bool ok = true;
  for (const Case& c : cases) {
    ID3D11Texture2D* a = MakeTex(dev, c.f, 37, 19, 4, c.bpt, 0x11, -1);
    ID3D11Texture2D* b = MakeTex(dev, c.f, 37, 19, 4, c.bpt, 0x11, -1);
    ID3D11Texture2D* e = MakeTex(dev, c.f, 37, 19, 4, c.bpt, 0x11, 37 * 19 * 2 + 5);  // one texel in slice 2
    if (!a || !b || !e) {
      ok = false;
      continue;
    }
    ID3D11Texture2D* sa = shadowbatch::CopyToStaging(a);
    ID3D11Texture2D* sb = shadowbatch::CopyToStaging(b);
    ID3D11Texture2D* se = shadowbatch::CopyToStaging(e);
    uint64_t texels = 0;
    const int64_t same = shadowbatch::CompareGuarded(sa, sb, &texels);
    uint64_t texels2 = 0;
    const int64_t diff = shadowbatch::CompareGuarded(sa, se, &texels2);
    ok = ok && same == 0 && diff == 1 && texels == 37ull * 19 * 4 && texels2 == texels;
    for (ID3D11Texture2D* t : {a, b, e, sa, sb, se})
      if (t) t->Release();
  }
  Check(ok, "shadow batch: depth compare is exact for 2-, 4- and 8-byte formats over every slice");
  shadowbatch::g_ctx = nullptr;
  ctx->Release();
  dev->Release();
}

}  // namespace sbtest
