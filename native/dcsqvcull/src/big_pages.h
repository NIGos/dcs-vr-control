// Bigger pages for NGModel's per-instance data (StructBufferManager).
//
// When no page of an element size has room, the scan (NGModel 0xc1b0)
// creates one of max(count, 0xfe00 / elemSize) elements, about 63.5 KB
// [V 0xc206]. A busy airfield fills about 150 such pages per frame (7.0 MB of
// 96-byte and 2.9 MB of 112-byte elements, measured 2026-10-08). Each page is
// its own GPU structured buffer, uploaded with one Map(WRITE_DISCARD) of its
// used bytes at EndParse (slot 8, 0xbdb0) [V], and bound per draw as the
// sbPositions SRV selected by [item+0xd0] (the page index), with the element
// index in [item+0xd4].
//
// With big pages on, the page-size constant becomes g_bigBytes (4 MB by default; 12 MB measured -1.3% fps, 4 MB neutral), and at
// each per-frame reset the small pages are retired by setting their byte
// capacity to 0: the scan then never picks them (room = capBytes - used) and
// the upload skips them (used stays 0). New allocations land in one big page
// per element size. Every object keeps exactly the same bytes; only the page
// that holds them (and so the SRV bound with them) changes, which already
// varies from frame to frame. Fewer Map/Unmap calls, fewer SRV changes
// between draws, and same-model casters share one buffer (needed for shadow
// instancing, R12 §7).
//
// Turning it off restores the constant and the retired pages' capacities;
// big pages created meanwhile stay (DCS handles any page size).
// Included once from main.cpp inside its anonymous namespace, after
// alloc_slab.h and pose_sweep.h.
#pragma once

namespace bigpages {

constexpr uint32_t kInsnRva = 0xc206;  // mov eax, 0xfe00
constexpr uint32_t kStockBytes = 0xfe00;
uint32_t g_bigBytes = 4u << 20;  // [Model] BigPageBytes; fixed once installed
const uint8_t kStockInsn[5] = {0xB8, 0x00, 0xFE, 0x00, 0x00};

std::atomic<int> g_state{0};  // 0 = not tried, -1 = unavailable, 1 = ready
std::atomic<bool> g_wanted{false};
bool g_applied = false;  // render-side state, changed only in OnReset
uint8_t* g_insn = nullptr;
struct Retired {
  uint32_t idx;
  uint32_t capBytes;
};
std::vector<Retired> g_retired;  // reset thread only
std::atomic<uint64_t> g_retiredCount{0};

bool WriteConst(uint32_t bytes) {
  uint8_t insn[5] = {0xB8};
  memcpy(insn + 1, &bytes, 4);
  return posesweep::PatchAllSuspended(g_insn, insn, 5);
}

bool Install() {
  if (g_state.load() != 0) return g_state.load() > 0;
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"NGModel.dll"));
  if (!base || allocslab::g_state.load() < 1) return false;  // not yet: retried later
  g_state = -1;
  g_insn = base + kInsnRva;
  uint8_t cur[5];
  if (!allocslab::ReadBytes(g_insn, cur, 5)) return false;
  uint32_t imm;
  memcpy(&imm, cur + 1, 4);
  if (cur[0] != 0xB8 || (imm != kStockBytes && imm < (1u << 20))) {
    Log("big model pages: NGModel.dll does not match this build; skipped");
    return false;
  }
  allocslab::g_onReset = [](void* mgr) {
    const bool want = g_wanted.load(std::memory_order_relaxed);
    if (want != g_applied) {
      if (!WriteConst(want ? g_bigBytes : kStockBytes)) return;  // try again at the next reset
      g_applied = want;
    }
    auto* b = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(mgr) + 8);
    auto* e = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(mgr) + 0x10);
    if (g_applied) {
      // Retire every small page that still has capacity.
      for (uint8_t* p = b; p && p < e; p += allocslab::kPageStride) {
        auto* pg = reinterpret_cast<allocslab::Page*>(p);
        if (pg->capBytes && pg->capBytes < g_bigBytes / 2) {
          g_retired.push_back({pg->idx, pg->capBytes});
          pg->capBytes = 0;
        }
      }
    } else if (!g_retired.empty()) {
      for (const Retired& r : g_retired) {
        for (uint8_t* p = b; p && p < e; p += allocslab::kPageStride) {
          auto* pg = reinterpret_cast<allocslab::Page*>(p);
          if (pg->idx == r.idx && pg->capBytes == 0) pg->capBytes = r.capBytes;
        }
      }
      g_retired.clear();
    }
    g_retiredCount.store(g_retired.size(), std::memory_order_relaxed);
  };
  g_applied = imm != kStockBytes;
  if (g_applied) g_bigBytes = imm;  // left patched by a previous payload
  g_state = 1;
  Log("big model pages: ready (page size %u bytes when on)", g_bigBytes);
  return true;
}

// Takes effect at the next per-frame reset of the model data pages.
void SetOn(bool on) {
  if (g_state.load() <= 0) return;
  g_wanted = on;
}

}  // namespace bigpages
