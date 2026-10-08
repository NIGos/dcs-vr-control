// Parallel copy for large dynamic structured-buffer uploads.
//
// DX11StructuredBuffer::update (dx11backend vtable RVA 0xb5980, slot 2,
// 0x32290) with a dynamic buffer ([this+0x44] == 3) does
//   p = this->vt[3](this, 1);  memcpy(p + offset, data, size);  this->vt[4](this);
// [V 0x322a8-0x322de], i.e. Map(WRITE_DISCARD), one copy, Unmap. NGModel's
// per-frame model data upload (StructBufferManager slot 8, 0xbdb0) sends
// several MB this way on the render thread (memcpy 1.3% of it, 2026-10-08).
// Writing a freshly renamed WRITE_DISCARD allocation is limited by first
// touch, and splitting the copy over 2 more threads halves it (offline on
// this PC: 7 MB in 0.68 ms with one thread, 0.36 ms with two).
//
// With it on, an update of a dynamic buffer of at least kMinBytes makes the
// same Map call, copies the same bytes split into chunks by the calling
// thread and the helper threads, waits for all chunks, then makes the same
// Unmap call. The buffer receives exactly the same bytes; only the copying
// threads differ. Smaller or non-dynamic updates call the original.
// Included once from main.cpp inside its anonymous namespace.
#pragma once

namespace parupload {

constexpr uint32_t kVtableRva = 0xb5980;  // .?AVDX11StructuredBuffer@RenderAPI@@
constexpr int kSlot = 2;
constexpr size_t kMinBytes = 512u << 10;
constexpr int kHelpers = 2;

using UpdateFn = bool(__fastcall*)(void* self, uint32_t offset, const void* data, int32_t size);
using MapFn = uint8_t*(__fastcall*)(void* self, int mode);
using UnmapFn = void(__fastcall*)(void* self);
UpdateFn g_orig = nullptr;
void** g_slot = nullptr;
std::atomic<bool> g_on{false};
std::atomic<int> g_state{0};
std::atomic<bool> g_stop{false};
HANDLE g_threads[kHelpers] = {};
HANDLE g_wake[kHelpers] = {};

struct Job {
  std::atomic<uint8_t*> dst{nullptr};
  const uint8_t* src = nullptr;
  size_t len = 0;
  std::atomic<int> done{1};
};
Job g_jobs[kHelpers];
std::atomic<uint64_t> g_calls{0}, g_bytes{0}, g_cycles{0};

DWORD WINAPI Helper(void* arg) {
  const int i = static_cast<int>(reinterpret_cast<intptr_t>(arg));
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  while (!g_stop.load()) {
    WaitForSingleObject(g_wake[i], 100);
    Job& j = g_jobs[i];
    if (j.done.load(std::memory_order_acquire)) continue;
    memcpy(j.dst.load(std::memory_order_relaxed), j.src, j.len);
    j.done.store(1, std::memory_order_release);
  }
  return 0;
}

bool __fastcall Hook(void* self, uint32_t offset, const void* data, int32_t size) {
  auto* b = static_cast<uint8_t*>(self);
  if (!g_on.load(std::memory_order_relaxed) || size < static_cast<int32_t>(kMinBytes) ||
      *reinterpret_cast<int32_t*>(b + 0x44) != 3)
    return g_orig(self, offset, data, size);
  const uint64_t t0 = __rdtsc();
  void** vt = *static_cast<void***>(self);
  uint8_t* p = reinterpret_cast<MapFn>(vt[3])(self, 1);
  uint8_t* dst = p + offset;
  const auto* src = static_cast<const uint8_t*>(data);
  const size_t total = static_cast<size_t>(size);
  const size_t chunk = ((total / (kHelpers + 1)) + 63) & ~static_cast<size_t>(63);
  size_t pos = chunk;  // the caller copies [0, chunk)
  for (int i = 0; i < kHelpers; ++i) {
    Job& j = g_jobs[i];
    const size_t len = pos >= total ? 0 : (pos + chunk > total ? total - pos : chunk);
    if (len == 0) continue;
    j.src = src + pos;
    j.len = len;
    j.dst.store(dst + pos, std::memory_order_relaxed);
    j.done.store(0, std::memory_order_release);
    SetEvent(g_wake[i]);
    pos += len;
  }
  memcpy(dst, src, chunk < total ? chunk : total);
  for (int i = 0; i < kHelpers; ++i)
    while (!g_jobs[i].done.load(std::memory_order_acquire)) _mm_pause();
  reinterpret_cast<UnmapFn>(vt[4])(self);
  g_calls.fetch_add(1, std::memory_order_relaxed);
  g_bytes.fetch_add(total, std::memory_order_relaxed);
  g_cycles.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
  return true;
}

bool StartHelpers() {
  g_stop = false;
  for (int i = 0; i < kHelpers; ++i) {
    g_jobs[i].done.store(1);
    g_wake[i] = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_threads[i] = CreateThread(nullptr, 0, Helper, reinterpret_cast<void*>(static_cast<intptr_t>(i)), 0, nullptr);
    if (!g_wake[i] || !g_threads[i]) return false;
  }
  return true;
}

bool Install() {
  if (g_state.load() != 0) return g_state.load() > 0;
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(L"dx11backend.dll"));
  if (!base) return false;
  g_state = -1;
  auto** vtbl = reinterpret_cast<void**>(base + kVtableRva);
  if (!allocslab::RttiIs(base, vtbl, ".?AVDX11StructuredBuffer@RenderAPI@@") ||
      SlotOriginal(&vtbl[kSlot]) != base + 0x32290) {
    Log("parallel upload: DX11StructuredBuffer does not match this build; skipped");
    return false;
  }
  // The dynamic path's exact shape: mode check, Map(1), memcpy, Unmap [V 0x322a8].
  const uint8_t kShape[] = {0x83, 0x79, 0x44, 0x03};
  uint8_t got[4];
  if (!allocslab::ReadBytes(base + 0x322a8, got, 4) || memcmp(got, kShape, 4) != 0) {
    Log("parallel upload: DX11StructuredBuffer::update does not match this build; skipped");
    return false;
  }
  if (!StartHelpers()) return false;
  g_slot = &vtbl[kSlot];
  g_orig = reinterpret_cast<UpdateFn>(SlotOriginal(g_slot));
  if (!HookSlot(g_slot, reinterpret_cast<void*>(&Hook), nullptr)) return false;
  g_state = 1;
  Log("parallel upload: DX11StructuredBuffer::update hooked (dynamic updates >= %zu KB split over %d threads)",
      kMinBytes >> 10, kHelpers + 1);
  return true;
}

void StopHelpers() {
  g_stop = true;
  for (int i = 0; i < kHelpers; ++i) {
    if (g_wake[i]) SetEvent(g_wake[i]);
    if (g_threads[i]) {
      WaitForSingleObject(g_threads[i], 1000);
      CloseHandle(g_threads[i]);
    }
    if (g_wake[i]) CloseHandle(g_wake[i]);
    g_threads[i] = g_wake[i] = nullptr;
  }
}

void Shutdown() {
  g_on = false;
  if (g_slot && g_orig) UnhookSlot(g_slot, reinterpret_cast<void*>(g_orig));
  StopHelpers();
}

}  // namespace parupload
