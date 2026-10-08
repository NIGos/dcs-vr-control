// Render-thread isolation: gives DCS's render thread a physical core of its
// own (both SMT siblings reserved, the thread on the first one) at high
// priority, and moves every other DCS thread off that core. Scheduling only:
// nothing about what is rendered changes. Previous affinities and priorities
// are restored exactly when switched off.
// Included once from main.cpp after the globals it uses (Log).
#pragma once

namespace threadtune {

std::atomic<DWORD>& g_renderTid = ptiming::g_topTid;  // set by the pass timing hook
std::mutex g_mutex;
struct Saved {
  DWORD tid;
  DWORD_PTR mask;
};
std::vector<Saved> g_saved;
int g_renderPrio = THREAD_PRIORITY_NORMAL;
bool g_active = false;

// Logical CPU pair reserved for the render thread (SMT siblings are adjacent
// on Windows). Core 2 avoids CPU 0, where many system interrupts land.
constexpr int kCore = 2;

bool Apply() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_active) return true;
  DWORD rt = g_renderTid.load();
  if (!rt) {
    Log("thread tuning: render thread not identified yet");
    return false;
  }
  DWORD_PTR procMask = 0, sysMask = 0;
  GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask);
  DWORD_PTR reserved = (DWORD_PTR(1) << (kCore * 2)) | (DWORD_PTR(1) << (kCore * 2 + 1));
  DWORD_PTR renderMask = DWORD_PTR(1) << (kCore * 2);
  if ((procMask & reserved) != reserved) {
    Log("thread tuning: CPU %d/%d not available to DCS", kCore * 2, kCore * 2 + 1);
    return false;
  }
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return false;
  THREADENTRY32 te{sizeof(te)};
  int moved = 0;
  for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
    if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
    HANDLE h = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
    if (!h) continue;
    DWORD_PTR want = te.th32ThreadID == rt ? renderMask : (procMask & ~reserved);
    DWORD_PTR prev = SetThreadAffinityMask(h, want);
    if (prev) {
      g_saved.push_back({te.th32ThreadID, prev});
      ++moved;
    }
    if (te.th32ThreadID == rt) {
      g_renderPrio = GetThreadPriority(h);
      SetThreadPriority(h, THREAD_PRIORITY_HIGHEST);
    }
    CloseHandle(h);
  }
  CloseHandle(snap);
  g_active = true;
  Log("thread tuning: render thread %lu isolated on CPU %d (sibling %d reserved), %d threads adjusted", rt,
      kCore * 2, kCore * 2 + 1, moved);
  return true;
}

void Restore() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_active) return;
  for (const Saved& s : g_saved) {
    HANDLE h = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, s.tid);
    if (!h) continue;  // thread exited
    SetThreadAffinityMask(h, s.mask);
    if (s.tid == g_renderTid.load()) SetThreadPriority(h, g_renderPrio);
    CloseHandle(h);
  }
  g_saved.clear();
  g_active = false;
  Log("thread tuning: affinities and priority restored");
}

}  // namespace threadtune
