// Offline test for par_upload.h: the split copy writes exactly the source
// bytes through the object's own Map/Unmap, for sizes around the chunking.
#pragma once

namespace putest {

std::vector<uint8_t> g_mapped;
int g_maps = 0, g_unmaps = 0, g_origCalls = 0;
uint8_t* __fastcall FakeMap(void*, int mode) {
  ++g_maps;
  return mode == 1 ? g_mapped.data() : nullptr;
}
void __fastcall FakeUnmap(void*) { ++g_unmaps; }
bool __fastcall FakeOrig(void*, uint32_t, const void*, int32_t) {
  ++g_origCalls;
  return true;
}

void Run() {
  void* vt[8] = {};
  vt[3] = reinterpret_cast<void*>(&FakeMap);
  vt[4] = reinterpret_cast<void*>(&FakeUnmap);
  struct Obj {
    void** vtbl;
    uint8_t pad[0x3c];
    int32_t mode;
  } obj{vt, {}, 3};
  static_assert(offsetof(Obj, mode) == 0x44, "fake layout");
  parupload::g_orig = &FakeOrig;
  bool ok = parupload::StartHelpers();
  parupload::g_on = true;
  const size_t sizes[] = {parupload::kMinBytes, parupload::kMinBytes + 1, 3u << 20, (7u << 20) + 37, 100u};
  for (size_t sz : sizes) {
    for (uint32_t off : {0u, 96u}) {
      std::vector<uint8_t> src(sz);
      for (size_t i = 0; i < sz; ++i) src[i] = static_cast<uint8_t>(i * 7 + sz);
      g_mapped.assign(sz + off + 64, 0xEE);
      g_maps = g_unmaps = g_origCalls = 0;
      parupload::Hook(&obj, off, src.data(), static_cast<int32_t>(sz));
      if (sz < parupload::kMinBytes) {
        ok = ok && g_origCalls == 1 && g_maps == 0;
      } else {
        ok = ok && g_maps == 1 && g_unmaps == 1 && g_origCalls == 0 &&
             memcmp(g_mapped.data() + off, src.data(), sz) == 0 && g_mapped[off + sz] == 0xEE &&
             (off == 0 || g_mapped[off - 1] == 0xEE);
      }
    }
  }
  obj.mode = 2;  // not dynamic: original path
  g_origCalls = 0;
  std::vector<uint8_t> big(2u << 20, 1);
  parupload::Hook(&obj, 0, big.data(), static_cast<int32_t>(big.size()));
  ok = ok && g_origCalls == 1;
  parupload::g_on = false;
  parupload::StopHelpers();
  parupload::g_orig = nullptr;
  Check(ok, "parallel upload: split copy writes exactly the source bytes via Map/Unmap; small or non-dynamic go to the original");
}

}  // namespace putest
