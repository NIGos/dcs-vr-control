// Reproduces the 2026-10-08 hot-reload crash and checks the fix.
// Worker threads keep calling through a "jmp [rip+x]" stub while the main
// thread rewrites the stub's target pointer the way the loader does
// (VirtualProtect the pointer's page to PAGE_READWRITE, write, restore).
//   mode same:  pointer on the stub's own page (old layout)  -> expected to fault
//   mode split: pointer on the next page (new layout)         -> must not fault
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

static int __fastcall TargetA(int x) { return x + 1; }
static int __fastcall TargetB(int x) { return x + 2; }

static std::atomic<bool> g_stop{false};
static std::atomic<long> g_faults{0};
static std::atomic<long long> g_calls{0};

static int SafeCall(int (*fn)(int), int v) {
  __try {
    return fn(v);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_faults++;
    return -1;
  }
}

static bool Run(bool split) {
  g_stop = false;
  g_faults = 0;
  g_calls = 0;
  const size_t page = 4096;
  auto* mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, 2 * page, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
  void** slot = reinterpret_cast<void**>(split ? mem + page : mem + 16);
  int32_t rel = static_cast<int32_t>(reinterpret_cast<uint8_t*>(slot) - (mem + 6));
  mem[0] = 0xFF;
  mem[1] = 0x25;
  memcpy(mem + 2, &rel, 4);
  *slot = reinterpret_cast<void*>(&TargetA);
  DWORD old;
  // Old layout: one RWX page. New layout: code page RX, data page RW.
  VirtualProtect(mem, page, split ? PAGE_EXECUTE_READ : PAGE_EXECUTE_READWRITE, &old);
  FlushInstructionCache(GetCurrentProcess(), mem, page);
  auto stub = reinterpret_cast<int (*)(int)>(mem);

  std::vector<std::thread> th;
  for (int t = 0; t < 6; ++t)
    th.emplace_back([&] {
      while (!g_stop.load(std::memory_order_relaxed)) {
        SafeCall(stub, 1);
        g_calls++;
      }
    });
  for (int i = 0; i < 20000; ++i) {
    // Same steps as the loader's WriteSlot.
    VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
    InterlockedExchangePointer(slot, (i & 1) ? reinterpret_cast<void*>(&TargetB) : reinterpret_cast<void*>(&TargetA));
    VirtualProtect(slot, sizeof(void*), old, &old);
  }
  g_stop = true;
  for (auto& x : th) x.join();
  printf("%-6s layout: %lld calls, %ld faults\n", split ? "split" : "same", g_calls.load(), g_faults.load());
  return g_faults.load() == 0;
}

int main() {
  bool sameOk = Run(false);
  bool splitOk = Run(true);
  printf("%s old layout reproduces the crash (faults expected)\n", !sameOk ? "PASS" : "INFO (no fault this run)");
  printf("%s new layout never faults\n", splitOk ? "PASS" : "FAIL");
  return splitOk ? 0 : 1;
}
