// Test-only synthetic head motion. Hooks the Quad-Views-Foveated layer's
// xrLocateViews (the function DCS calls) and rotates every returned view pose
// about the vertical axis by a time-varying yaw, so DCS renders exactly as if
// the head were turning: same culling, LOD and streaming work. Used only
// while a motion profile records with [Suite] MotionSweep=1; off otherwise.
// The headset shows a swinging image while it runs.
//
// The detour follows the allocator detour's rules: code and target pointer on
// separate pages, and the 14-byte patch is written with every other thread of
// the process suspended and outside the patched bytes.
// Included once from main.cpp inside its anonymous namespace.
#pragma once

namespace posesweep {

const wchar_t* const kModule = L"XR_APILAYER_MBUCCHIA_quad_views_foveated.dll";
constexpr uint32_t kRva = 0x1aa70;  // DcsVrControl's bundled QVFR build of 2026-10-07
const uint8_t kPrologue[17] = {0x40, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41,
                               0x57, 0x48, 0x81, 0xEC, 0xE8, 0x00, 0x00, 0x00};
constexpr size_t kPage = 4096;

using Fn = int32_t(__fastcall*)(void* session, const void* info, void* state, uint32_t cap, uint32_t* count,
                                uint8_t* views);
Fn g_tramp = nullptr;
void** g_slot = nullptr;
std::atomic<bool> g_on{false};
std::atomic<int> g_state{0};
std::atomic<int64_t> g_t0{0};
std::atomic<double> g_yawDeg{0};

// Phases (seconds): still, slow, medium, fast, still. Yaw = A*sin(2*pi*t/T).
std::atomic<bool> g_taxi{false};  // [Suite] MotionTaxi: slow straight taxi instead of the yaw sweep

double YawAt(double t) {
  if (g_taxi.load()) return 0;
  const double A = 60.0;
  auto sweep = [&](double tt, double period) { return A * std::sin(2.0 * 3.14159265358979 * tt / period); };
  if (t < 5) return 0;
  if (t < 25) return sweep(t - 5, 8.0);   // peak about 47 deg/s
  if (t < 45) return sweep(t - 25, 4.0);  // peak about 94 deg/s
  if (t < 65) return sweep(t - 45, 2.0);  // peak about 188 deg/s
  return 0;
}

// Translation phase after the yaw phases: the head moves forward (-Z in the
// view's local space) at 100 m/s for 15 s, like a low pass, so DCS streams
// new terrain if it follows the head position that far.
double ForwardAt(double t) {
  if (g_taxi.load()) {
    // Taxi: 15 m/s forward from t = 5 s, through whatever is ahead.
    return t < 5 ? 0 : (t - 5) * 15.0;
  }
  if (t < 67) return 0;
  if (t < 82) return (t - 67) * 100.0;
  return 1500.0;
}

int32_t __fastcall Hook(void* session, const void* info, void* state, uint32_t cap, uint32_t* count, uint8_t* views) {
  int32_t r = g_tramp(session, info, state, cap, count, views);
  if (r < 0 || !g_on.load(std::memory_order_relaxed) || !views || !count) return r;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  double t = (now.QuadPart - g_t0.load()) * g_qpcToUs / 1e6;
  double yaw = YawAt(t) * 3.14159265358979 / 180.0;
  const double fwd = ForwardAt(t);
  g_yawDeg = YawAt(t);
  const float s = static_cast<float>(std::sin(yaw / 2)), c = static_cast<float>(std::cos(yaw / 2));
  const uint32_t n = std::min(*count, cap);
  for (uint32_t i = 0; i < n; ++i) {
    float* q = reinterpret_cast<float*>(views + i * 64 + 16);  // XrView.pose.orientation x,y,z,w
    float* p = q + 4;                                          // pose.position x,y,z
    // q' = qy * q with qy = (0, s, 0, c)
    float x = q[0], y = q[1], z = q[2], w = q[3];
    q[0] = c * x + s * z;
    q[1] = c * y + s * w;
    q[2] = c * z - s * x;
    q[3] = c * w - s * y;
    // p' = rotate p about Y
    float cy = static_cast<float>(std::cos(yaw)), sy = static_cast<float>(std::sin(yaw));
    float px = p[0], pz = p[2];
    p[0] = cy * px + sy * pz;
    p[2] = -sy * px + cy * pz - static_cast<float>(fwd);
  }
  return r;
}

// Suspends every other thread, checks that none is inside [addr, addr+len),
// writes, resumes. Retries a few times.
bool PatchAllSuspended(uint8_t* addr, const uint8_t* bytes, size_t len) {
  for (int attempt = 0; attempt < 20; ++attempt) {
    std::vector<HANDLE> held;
    held.reserve(4096);  // no heap allocation while threads are suspended (one may hold the heap lock)
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    THREADENTRY32 te{sizeof(te)};
    bool clear = true;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
      if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
      HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
      if (!h) continue;
      if (SuspendThread(h) == static_cast<DWORD>(-1)) {
        CloseHandle(h);
        continue;
      }
      held.push_back(h);
      CONTEXT ctx{};
      ctx.ContextFlags = CONTEXT_CONTROL;
      if (GetThreadContext(h, &ctx)) {
        uintptr_t rip = ctx.Rip, a = reinterpret_cast<uintptr_t>(addr);
        if (rip > a && rip < a + len) clear = false;
      }
    }
    CloseHandle(snap);
    bool done = false;
    if (clear) {
      DWORD old;
      if (VirtualProtect(addr, len, PAGE_EXECUTE_READWRITE, &old)) {
        memcpy(addr, bytes, len);
        VirtualProtect(addr, len, old, &old);
        FlushInstructionCache(GetCurrentProcess(), addr, len);
        done = true;
      }
    }
    for (HANDLE h : held) {
      ResumeThread(h);
      CloseHandle(h);
    }
    if (done) return true;
    Sleep(2);
  }
  return false;
}

bool Install() {
  if (g_state.load() != 0) return g_state.load() > 0;
  g_state = -1;
  auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(kModule));
  if (!base) {
    Log("pose sweep: Quad-Views-Foveated layer not loaded; skipped");
    return false;
  }
  uint8_t* target = base + kRva;
  uint8_t cur[17];
  if (!allocslab::ReadBytes(target, cur, 17)) return false;
  if (cur[0] == 0xFF && cur[1] == 0x25) {
    // Patched by an earlier payload: its jump leads to the stub page; the
    // slot is on the next page and the trampoline right after the stub.
    uint8_t* page = nullptr;
    memcpy(&page, cur + 6, 8);
    g_slot = reinterpret_cast<void**>(page + kPage);
    g_tramp = reinterpret_cast<Fn>(static_cast<void*>(page + 16));
  } else if (memcmp(cur, kPrologue, 17) == 0) {
    auto* page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 2 * kPage, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!page) return false;
    memset(page, 0xCC, kPage);
    // stub at 0: jmp [rip -> slot]
    int32_t rel = static_cast<int32_t>(kPage - 6);
    page[0] = 0xFF;
    page[1] = 0x25;
    memcpy(page + 2, &rel, 4);
    // trampoline at 16: original 17 bytes + jmp back
    uint8_t* tr = page + 16;
    memcpy(tr, kPrologue, 17);
    tr[17] = 0xFF;
    tr[18] = 0x25;
    memset(tr + 19, 0, 4);
    uint8_t* back = target + 17;
    memcpy(tr + 23, &back, 8);
    g_slot = reinterpret_cast<void**>(page + kPage);
    *g_slot = tr;  // pass-through until switched on
    DWORD old;
    VirtualProtect(page, kPage, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), page, kPage);
    g_tramp = reinterpret_cast<Fn>(static_cast<void*>(tr));
    uint8_t patch[14] = {0xFF, 0x25, 0, 0, 0, 0};
    memcpy(patch + 6, &page, 8);
    if (!PatchAllSuspended(target, patch, 14)) {
      Log("pose sweep: could not patch xrLocateViews safely; skipped");
      return false;
    }
  } else {
    Log("pose sweep: Quad-Views-Foveated build does not match; skipped");
    return false;
  }
  InterlockedExchangePointer(g_slot, reinterpret_cast<void*>(&Hook));
  g_state = 1;
  Log("pose sweep: hooked Quad-Views-Foveated xrLocateViews");
  return true;
}

void Start() {
  if (!Install()) return;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  g_t0 = now.QuadPart;
  g_on = true;
}

void Stop() { g_on = false; }

}  // namespace posesweep
