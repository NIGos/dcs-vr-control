// Offline checks: frustum decoding, exclusion building, Scene.dll export lookup.
#include "../src/main.cpp"

#include <cassert>
#include <thread>

namespace {

double ED_FakeTime() {
  LARGE_INTEGER c, f;
  QueryPerformanceCounter(&c);
  QueryPerformanceFrequency(&f);
  return double(c.QuadPart) / double(f.QuadPart);
}

int g_fail = 0;
void Check(bool ok, const char* what) {
  printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}

// Writes a symmetric-ish frustum into a ClippingVolume-like buffer the way
// DCS stores it: inward unit normals, d such that inside is n.p + d >= 0.
void MakeFrustum(uint8_t* vol, Vec3 eye, Vec3 fwd, Vec3 up, double l, double r, double b, double t,
                 double zn, double zf) {
  memset(vol, 0, kVolSize);
  Vec3 right{fwd.y * up.z - fwd.z * up.y, fwd.z * up.x - fwd.x * up.z, fwd.x * up.y - fwd.y * up.x};
  auto side = [&](Vec3 edgeDir, Vec3 inwardHint) {
    // plane through eye containing edgeDir and the axis perpendicular to inwardHint
    Vec3 axis{edgeDir.y * inwardHint.z - edgeDir.z * inwardHint.y,
              edgeDir.z * inwardHint.x - edgeDir.x * inwardHint.z,
              edgeDir.x * inwardHint.y - edgeDir.y * inwardHint.x};
    Vec3 n{edgeDir.y * axis.z - edgeDir.z * axis.y, edgeDir.z * axis.x - edgeDir.x * axis.z,
           edgeDir.x * axis.y - edgeDir.y * axis.x};
    if (Dot(n, inwardHint) < 0) n = n * -1.0;
    n = n * (1.0 / Len(n));
    return Plane{n, -Dot(n, eye)};
  };
  // edges: angles given as tangents
  Plane planes[6];
  planes[0] = {fwd, -Dot(fwd, eye + fwd * zn)};
  planes[1] = {fwd * -1.0, Dot(fwd, eye + fwd * zf)};
  planes[2] = side(fwd + right * l, right);         // left edge, inside toward +right
  planes[3] = side(fwd + right * r, right * -1.0);  // right edge
  planes[4] = side(fwd + up * b, up);
  planes[5] = side(fwd + up * t, up * -1.0);
  for (int i = 0; i < 6; ++i) WritePlane(vol, i, planes[i]);
  *reinterpret_cast<int32_t*>(vol + kPlaneCount) = 6;
}

bool InsideBlock(const uint8_t* block, Vec3 p, double radius) {
  int n = *reinterpret_cast<const int32_t*>(block + kPlaneCount);
  for (int i = 0; i < n; ++i) {
    const double* d = reinterpret_cast<const double*>(block + i * kPlaneStride);
    if (d[0] * p.x + d[1] * p.y + d[2] * p.z + d[3] <= radius) return false;
  }
  return true;
}

}  // namespace

int main() {
  g_log = stdout;
  g_mute = true;
  alignas(64) static uint8_t periph[0x700], focus[0x700];
  Vec3 eye{1000.0, 50.0, -2000.0}, fwd{0, 0, 1}, up{0, 1, 0};
  MakeFrustum(periph, eye, fwd, up, -1.2, 1.0, -1.1, 1.0, 0.05, 20000);  // ~50 deg half
  MakeFrustum(focus, eye, fwd, up, -0.30, 0.30, -0.28, 0.30, 0.05, 20000);  // ~17 deg half

  Frustum fp = DecodeFrustum(periph), ff = DecodeFrustum(focus);
  Check(fp.valid && ff.valid, "decode both frusta");
  Check(Len(fp.apex - eye) < 1e-6 && Len(ff.apex - eye) < 1e-6, "apex recovered");
  Check(Dot(ff.forward, fwd) > 0.9999, "forward recovered");
  printf("     periph mean half=%.1f deg, focus mean half=%.1f deg\n", fp.meanHalfAngle * 57.2958,
         ff.meanHalfAngle * 57.2958);

  uint8_t block[kPlaneBlockSize];
  // keep 0.35 of a focus spanning tan -0.30..0.30 -> excluded |tan x| < 0.105.
  BuildExclusionBlock(block, ff, 0.35);
  Vec3 centre = eye + fwd * 100.0;
  Check(InsideBlock(block, centre, 1.0), "centre object excluded");
  Check(InsideBlock(block, eye + Vec3{0.09, 0, 1} * 100.0, 0.1), "object in opaque core excluded");
  Check(!InsideBlock(block, eye + Vec3{0.12, 0, 1} * 100.0, 0.1), "object in blend band kept");
  Check(!InsideBlock(block, eye + Vec3{0.29, 0, 1} * 100.0, 0.1), "object at focus border kept");
  Check(!InsideBlock(block, eye + Vec3{0.5, 0, 1} * 100.0, 0.1), "object outside focus kept");
  // Asymmetric focus (gaze off-centre): the core must follow the focus centre.
  // MakeFrustum's "right" is fwd x up = -x, so this focus spans x in [-0.8, -0.2].
  alignas(64) static uint8_t offc[0x700];
  MakeFrustum(offc, eye, fwd, up, 0.20, 0.80, -0.30, 0.30, 0.05, 20000);
  Frustum fo = DecodeFrustum(offc);
  BuildExclusionBlock(block, fo, 0.35);
  Check(InsideBlock(block, eye + Vec3{-0.50, 0, 1} * 100.0, 0.1), "off-centre core excluded");
  Check(!InsideBlock(block, eye + Vec3{-0.25, 0, 1} * 100.0, 0.1), "off-centre band kept");
  Check(!InsideBlock(block, centre, 0.1), "head-centre kept when gaze is off-centre");

  // Full block (debug hole) equals the focus frustum.
  BuildExclusionBlock(block, ff, 1.0);
  Check(InsideBlock(block, eye + Vec3{0.28, 0, 1} * 100.0, 0.1) &&
            !InsideBlock(block, eye + Vec3{0.32, 0, 1} * 100.0, 0.1),
        "keep=1 matches focus frustum");

  // QVFR log on this machine (may be held open for writing by a running DCS).
  double sm = ReadQvfrSmoothing();
  printf("     qvfr smoothing read: %.2f\n", sm);
  Check(sm >= 0.0 && sm < 0.5, "QVFR smoothing parsed");
  sm = 0.30;
  g_qvSmoothing = sm;
  Check(std::fabs(EffectiveKeep() - 0.35) < 1e-9, "auto keep = 1 - 2*0.30 - 0.05");

  // Volume copy with exclusion.
  alignas(64) static uint8_t dst[0x700];
  Check(MakeVolumeWithExclusion(dst, periph, block), "volume copy");
  Check(*reinterpret_cast<uint64_t*>(dst + kVolExclSize) == 1 &&
            *reinterpret_cast<uint8_t**>(dst + kVolExclPtr) == dst + kVolExclInline &&
            memcmp(dst, periph, kVolExclPtr) == 0,
        "exclusion list layout");

  // Classification over a fake CollectionInfo array: L periph, R periph, L focus, R focus.
  alignas(64) static uint8_t vols[4][0x700];
  Vec3 eyeR = eye + Vec3{0.064, 0, 0};
  MakeFrustum(vols[0], eye, fwd, up, -1.2, 1.0, -1.1, 1.0, 0.05, 20000);
  MakeFrustum(vols[1], eyeR, fwd, up, -1.0, 1.2, -1.1, 1.0, 0.05, 20000);
  MakeFrustum(vols[2], eye, fwd, up, -0.3, 0.3, -0.28, 0.3, 0.05, 20000);
  MakeFrustum(vols[3], eyeR, fwd, up, -0.3, 0.3, -0.28, 0.3, 0.05, 20000);
  static uint8_t infos[5 * kCollectionInfoStride];
  memset(infos, 0, sizeof(infos));
  for (int i = 0; i < 4; ++i)
    *reinterpret_cast<uint8_t**>(infos + i * kCollectionInfoStride + kCiClipVolume) = vols[i];
  *reinterpret_cast<uint16_t*>(infos + 4 * kCollectionInfoStride) = 8;  // non-main pass
  *reinterpret_cast<uint8_t**>(infos + 4 * kCollectionInfoStride + kCiClipVolume) = vols[0];
  std::vector<ViewInfo> views;
  Patch patches[16];
  int pairs = 0, patched = 0;
  g_enabled = true;
  Prepare(5, infos, patches, 16, &pairs, &patched, views);
  Check(pairs == 2 && patched == 3, "two pairs found, 3 infos patched (incl. shared pass)");
  Check(*reinterpret_cast<uint8_t**>(infos + 4 * kCollectionInfoStride + kCiClipVolume) ==
            *reinterpret_cast<uint8_t**>(infos + kCiClipVolume),
        "pass sharing the peripheral volume keeps sharing the replacement");
  Check(views.size() == 4 && views[0].partner == 2 && views[1].partner == 3, "same-eye pairing");
  uint8_t* v0 = *reinterpret_cast<uint8_t**>(infos + kCiClipVolume);
  Check(v0 != vols[0] && *reinterpret_cast<uint64_t*>(v0 + kVolExclSize) == 1, "periph volume swapped");
  for (int i = 0; i < patched; ++i) *reinterpret_cast<uint8_t**>(patches[i].slot) = patches[i].original;
  Check(*reinterpret_cast<uint8_t**>(infos + kCiClipVolume) == vols[0], "restore");

  // Frame heap slabs: a fake TFrameMemoryHeap<1> with the original's bump
  // semantics (aligned size, xadd on +0x18, null past +0x10).
  {
    struct FakeHeap {
      uint8_t* base;
      uint64_t pad;
      uint8_t* end;
      std::atomic<uint8_t*> cursor;
    };
    static_assert(offsetof(FakeHeap, end) == 0x10 && offsetof(FakeHeap, cursor) == 0x18, "heap layout");
    static std::vector<uint8_t> mem(64 << 20);
    static FakeHeap heap;
    struct F {
      static void* Get(void* h, size_t size) {
        auto* fh = static_cast<FakeHeap*>(h);
        size_t a = (size + 15) & ~static_cast<size_t>(15);
        uint8_t* p = fh->cursor.fetch_add(a);
        return p + a > fh->end ? nullptr : p;
      }
      static void Reset(void* h) {
        auto* fh = static_cast<FakeHeap*>(h);
        fh->cursor = fh->base;
      }
    };
    heap.base = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(mem.data()) + 15) & ~uintptr_t(15));
    heap.end = heap.base + (48 << 20);
    heap.cursor = heap.base;
    frameheap::g_orig = &F::Get;
    frameheap::g_origReset = &F::Reset;
    bool aligned = true, inside = true, noOverlap = true;
    for (int on = 0; on < 2; ++on) {
      g_frameHeapOn = on == 1;
      for (int frame = 0; frame < 3; ++frame) {
        std::vector<std::vector<std::pair<uint8_t*, size_t>>> recs(8);
        std::vector<std::thread> th;
        for (int t = 0; t < 8; ++t)
          th.emplace_back([&, t] {
            for (int i = 0; i < 20000; ++i) {
              size_t sz = 16 + ((i * 37 + t * 11) % 400);
              auto* p = static_cast<uint8_t*>(frameheap::HookGet(&heap, sz));
              if (p) recs[t].push_back({p, sz});
            }
          });
        for (auto& x : th) x.join();
        std::vector<std::pair<uint8_t*, size_t>> all;
        for (auto& v : recs) all.insert(all.end(), v.begin(), v.end());
        std::sort(all.begin(), all.end());
        for (size_t i = 0; i < all.size(); ++i) {
          if (reinterpret_cast<uintptr_t>(all[i].first) & 15) aligned = false;
          if (all[i].first < heap.base || all[i].first + all[i].second > heap.end) inside = false;
          if (i && all[i - 1].first + all[i - 1].second > all[i].first) noOverlap = false;
        }
        frameheap::HookReset<0>(&heap);
      }
    }
    // Near the end of the heap the original is used directly.
    g_frameHeapOn = true;
    heap.cursor = heap.end - 1000;
    void* last = frameheap::HookGet(&heap, 64);
    Check(last == heap.end - 1000, "frame heap: near the end the original allocator is used");
    g_frameHeapOn = false;
    Check(aligned && inside, "frame heap slabs: 16-byte aligned, inside the heap");
    Check(noOverlap, "frame heap slabs: no overlapping allocations, with and without slabs");
    frameheap::g_orig = nullptr;
  }

  // Pose sweep: yaw rotation of returned views keeps unit quaternions and
  // rotates positions about Y; nothing changes when off.
  {
    struct Fake {
      static int32_t Locate(void*, const void*, void*, uint32_t cap, uint32_t* count, uint8_t* views) {
        *count = 2;
        for (uint32_t i = 0; i < cap && i < 2; ++i) {
          float* q = reinterpret_cast<float*>(views + i * 64 + 16);
          q[0] = 0; q[1] = 0; q[2] = 0; q[3] = 1;            // identity orientation
          q[4] = i ? 0.032f : -0.032f; q[5] = 1.2f; q[6] = 0;  // eye positions
        }
        return 0;
      }
    };
    posesweep::g_tramp = &Fake::Locate;
    alignas(16) uint8_t views[2 * 64] = {};
    uint32_t count = 0;
    posesweep::g_on = false;
    posesweep::Hook(nullptr, nullptr, nullptr, 2, &count, views);
    float* q0 = reinterpret_cast<float*>(views + 16);
    Check(q0[3] == 1 && q0[1] == 0 && q0[4] == -0.032f, "pose sweep off: views unchanged");
    // t = 7 s into the recording: slow sweep, yaw = 60*sin(2*pi*2/8) = 60 deg
    LARGE_INTEGER now, f;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&f);
    g_qpcToUs = 1e6 / static_cast<double>(f.QuadPart);
    posesweep::g_t0 = now.QuadPart - static_cast<int64_t>(7.0 * f.QuadPart);
    posesweep::g_on = true;
    posesweep::Hook(nullptr, nullptr, nullptr, 2, &count, views);
    posesweep::g_on = false;
    double yaw = posesweep::g_yawDeg.load() * 3.14159265358979 / 180.0;
    double len = std::sqrt(q0[0] * q0[0] + q0[1] * q0[1] + q0[2] * q0[2] + q0[3] * q0[3]);
    double expectY = std::sin(yaw / 2), expectW = std::cos(yaw / 2);
    double px = -0.032 * std::cos(yaw), pz = 0.032 * std::sin(yaw);
    printf("     pose sweep: yaw %.1f deg, q=(%.3f %.3f %.3f %.3f), left eye at (%.4f %.3f %.4f)\n",
           posesweep::g_yawDeg.load(), q0[0], q0[1], q0[2], q0[3], q0[4], q0[5], q0[6]);
    Check(std::fabs(posesweep::g_yawDeg.load()) > 30.0, "pose sweep applies a yaw in the slow phase");
    Check(std::fabs(len - 1.0) < 1e-5, "pose sweep keeps unit quaternions");
    Check(std::fabs(q0[1] - expectY) < 1e-5 && std::fabs(q0[3] - expectW) < 1e-5, "pose sweep orientation = yaw about Y");
    Check(std::fabs(q0[4] - px) < 1e-5 && std::fabs(q0[6] - pz) < 1e-5 && q0[5] == 1.2f, "pose sweep rotates positions about Y");
    posesweep::g_tramp = nullptr;
  }

  // Pacer MWAITX stub: called with rsi = flags, the way the patched loop does.
  if (pacer::HasMonitorx()) {
    auto* mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    // caller: push rsi; mov rsi, rcx; call stub (rel32); pop rsi; ret
    uint8_t* stub = mem + 64;
    uint8_t* c = mem;
    pacer::Emit(c, {0x56, 0x48, 0x89, 0xCE, 0xE8});
    int32_t rel = static_cast<int32_t>(stub - (c + 4));
    memcpy(c, &rel, 4);
    c += 4;
    pacer::Emit(c, {0x5E, 0xC3});
    const double tsc = 4.7e9;
    pacer::EmitMwaitStub(stub, static_cast<uint32_t>(tsc * 0.005));  // 5 ms: long enough to observe
    DWORD old;
    VirtualProtect(mem, 4096, PAGE_EXECUTE_READ, &old);
    auto call = reinterpret_cast<void (*)(volatile uint8_t*)>(mem);
    alignas(64) static volatile uint8_t flags[64];
    auto ms = [](LARGE_INTEGER a, LARGE_INTEGER b) {
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      return (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
    };
    LARGE_INTEGER t0, t1;
    flags[0] = 0;
    QueryPerformanceCounter(&t0);
    call(flags);
    QueryPerformanceCounter(&t1);
    double exitNow = ms(t0, t1);
    flags[0] = 1;
    flags[12] = 0;
    QueryPerformanceCounter(&t0);
    call(flags);
    QueryPerformanceCounter(&t1);
    double slept = ms(t0, t1);
    std::thread waker([] {
      Sleep(1);
      flags[0] = 0;
    });
    QueryPerformanceCounter(&t0);
    call(flags);
    QueryPerformanceCounter(&t1);
    waker.join();
    double woke = ms(t0, t1);
    printf("     pacer stub: exit flag %.3f ms, timeout %.3f ms, woken by write %.3f ms\n", exitNow, slept, woke);
    Check(exitNow < 0.5, "pacer stub returns at once when the exit flag is already set");
    // MWAITX may wake early (interrupts); it must sleep a little and never
    // longer than the timeout.
    Check(slept > 0.01 && slept < 50.0, "pacer stub sleeps, bounded by the timeout");
    Check(woke < 50.0, "pacer stub returns after a write to the flags");
    Check(flags[12] == 0, "pacer stub leaves the flags untouched");
  } else {
    printf("     pacer stub: CPU has no MONITORX, test skipped\n");
  }

  // Allocator slabs: a fake StructBufferManager with the original semantics
  // (page search 0xc1b0, locked bump 0xbf30, reset slot 7). Many threads
  // allocate; no two allocations may overlap within a generation, every
  // result must match its (page, index) out-parameters, and the reset must
  // invalidate every slab.
  {
    struct FakeMgr {
      void* vt;
      uint8_t* begin;
      uint8_t* end;
      uint8_t* cap;
      std::mutex m;  // stands in for the ed::mutex at +0x20
    };
    static_assert(offsetof(FakeMgr, m) == 0x20, "mutex offset");
    static FakeMgr mgr;
    static std::vector<uint8_t> pageMem(0x30 * 256);
    mgr.begin = mgr.end = pageMem.data();
    mgr.cap = pageMem.data() + pageMem.size();
    struct Fakes {
      static void Lock(void* mu) { static_cast<std::mutex*>(mu)->lock(); }
      static void Unlock(void* mu) { static_cast<std::mutex*>(mu)->unlock(); }
      static allocslab::Page* Scan(void* m, uint32_t size, uint32_t count) {
        auto* fm = static_cast<FakeMgr*>(m);
        for (uint8_t* p = fm->begin; p < fm->end; p += 0x30) {
          auto* pg = reinterpret_cast<allocslab::Page*>(p);
          if (pg->elemSize == size && pg->capBytes - pg->used >= size * count) return pg;
        }
        uint32_t n = std::max(count, 0xFE00u / size);
        auto* pg = reinterpret_cast<allocslab::Page*>(fm->end);
        *pg = {static_cast<uint32_t>((fm->end - fm->begin) / 0x30), size, n, n * size, 0, 0,
               static_cast<uint8_t*>(malloc(n * size)), nullptr};
        fm->end += 0x30;
        return pg;
      }
      static uint8_t* Alloc(void* m, uint32_t size, uint32_t count, uint32_t* page, uint32_t* index) {
        auto* fm = static_cast<FakeMgr*>(m);
        fm->m.lock();
        allocslab::Page* pg = Scan(m, size, count);
        uint32_t used = pg->used;
        pg->used += size * count;
        *page = pg->idx;
        *index = used / size;
        fm->m.unlock();
        return pg->data + used;
      }
      static void Reset(void* m) {
        auto* fm = static_cast<FakeMgr*>(m);
        for (uint8_t* p = fm->begin; p < fm->end; p += 0x30) {
          auto* pg = reinterpret_cast<allocslab::Page*>(p);
          pg->prevUsed = pg->used;
          pg->used = 0;
        }
      }
    };
    allocslab::g_lock = &Fakes::Lock;
    allocslab::g_unlock = &Fakes::Unlock;
    allocslab::g_scan = &Fakes::Scan;
    allocslab::g_tramp = &Fakes::Alloc;
    allocslab::g_origReset = &Fakes::Reset;
    g_allocSlabBytes = 4096;

    struct Rec {
      uint32_t page, off, len;
      uint8_t* ptr;
    };
    bool allMatch = true, noOverlap = true;
    uint64_t totalSlow = 0, totalCalls = 0;
    for (int mode = 0; mode < 2; ++mode) {
      g_allocSlabsOn = mode == 1;
      for (int frame = 0; frame < 3; ++frame) {
        std::vector<std::vector<Rec>> recs(8);
        std::vector<std::thread> th;
        for (int t = 0; t < 8; ++t)
          th.emplace_back([&, t] {
            uint32_t sizes[4] = {0x60, 0x70, 0xA0, 0xB0};
            for (int i = 0; i < 3000; ++i) {
              uint32_t size = sizes[(i * 7 + t) % 4], count = 1 + (i % 5), pg = 0, idx = 0;
              uint8_t* p = allocslab::HookAlloc(&mgr, size, count, &pg, &idx);
              recs[t].push_back({pg, idx * size, size * count, p});
            }
          });
        for (auto& x : th) x.join();
        std::map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> byPage;
        for (auto& v : recs)
          for (const Rec& r : v) {
            auto* pg = reinterpret_cast<allocslab::Page*>(mgr.begin + r.page * 0x30);
            if (r.ptr != pg->data + r.off || r.off + r.len > pg->used) allMatch = false;
            byPage[r.page].push_back({r.off, r.len});
          }
        for (auto& kv : byPage) {
          auto& v = kv.second;
          std::sort(v.begin(), v.end());
          for (size_t i = 1; i < v.size(); ++i)
            if (v[i - 1].first + v[i - 1].second > v[i].first) noOverlap = false;
        }
        allocslab::HookReset(&mgr);  // per-frame reset through the hooked slot
      }
    }
    allocslab::Totals tot = allocslab::Snapshot();
    totalSlow = tot.slow;
    totalCalls = tot.calls;
    printf("     allocator slabs: %llu calls, %llu slow-path reservations with slabs on\n",
           static_cast<unsigned long long>(totalCalls), static_cast<unsigned long long>(totalSlow));
    Check(allMatch, "every allocation matches its page and index, inside the used range");
    Check(noOverlap, "no two allocations overlap within a frame, with and without slabs");
    Check(totalSlow > 0 && totalSlow < totalCalls / 10, "slabs serve most requests without the lock");
    g_allocSlabsOn = false;
    allocslab::g_tramp = nullptr;
  }

  // Shadow caster tightening: no caster whose shadow can reach a view may be
  // removed, and some casters must be removed.
  {
    g_enabled = false;
    const Vec3 L = Normalize(Vec3{0.3, -0.7, 0.6});
    auto basis = [&](double rot, Vec3& X, Vec3& Y) {
      Vec3 x0 = Normalize(Cross(L, Vec3{0, 1, 0}));
      Vec3 y0 = Cross(x0, L);
      X = x0 * std::cos(rot) + y0 * std::sin(rot);
      Y = Cross(X, L);
    };
    auto makeBox = [&](uint8_t* vol, Vec3 X, Vec3 Y, Vec3 c, double hx, double hy, double back, double front) {
      memset(vol, 0, kVolSize);
      Vec3 ax[3] = {X, Y, L};
      double lo[3] = {Dot(X, c) - hx, Dot(Y, c) - hy, Dot(L, c) - back};
      double hi[3] = {Dot(X, c) + hx, Dot(Y, c) + hy, Dot(L, c) + front};
      for (int a = 0; a < 3; ++a) {
        WritePlane(vol, 2 * a, Plane{ax[a], -lo[a]});
        WritePlane(vol, 2 * a + 1, Plane{ax[a] * -1.0, hi[a]});
      }
      *reinterpret_cast<int32_t*>(vol + kPlaneCount) = 6;
    };
    alignas(64) static uint8_t casc[4][0x700], terr[0x700];
    static uint8_t sinfos[9 * kCollectionInfoStride];
    memset(sinfos, 0, sizeof(sinfos));
    Vec3 X, Y;
    basis(0.0, X, Y);
    const double ext[4] = {30, 120, 900, 5000};
    for (int c = 0; c < 4; ++c) {
      Vec3 mid = eye + fwd * (ext[c] * 0.5);
      makeBox(casc[c], X, Y, mid, ext[c], ext[c], ext[c] * 2, ext[c]);
    }
    Vec3 X2, Y2;
    basis(0.2, X2, Y2);
    makeBox(terr, X2, Y2, eye, 60000, 60000, 60000, 60000);
    for (int i = 0; i < 4; ++i)
      *reinterpret_cast<uint8_t**>(sinfos + i * kCollectionInfoStride + kCiClipVolume) = vols[i];
    for (int c = 0; c < 4; ++c) {
      uint8_t* ci = sinfos + (4 + c) * kCollectionInfoStride;
      *reinterpret_cast<uint16_t*>(ci + kCiShadingModel) = 14;
      *reinterpret_cast<uint8_t**>(ci + kCiClipVolume) = casc[c];
    }
    *reinterpret_cast<uint16_t*>(sinfos + 8 * kCollectionInfoStride + kCiShadingModel) = 15;
    *reinterpret_cast<uint8_t**>(sinfos + 8 * kCollectionInfoStride + kCiClipVolume) = terr;

    Patch sp[32];
    int sn = 0;
    g_shadowDebugInvert = false;
    for (int pass = 0; pass < 3; ++pass) {
    // pass 0: terrain map present (exact light); 1: tracking the cached light;
    // 2: no reference, both non-horizontal axes treated as possible lights.
    if (pass == 1) *reinterpret_cast<uint8_t**>(sinfos + 8 * kCollectionInfoStride + kCiClipVolume) = nullptr;
    if (pass == 2) g_lightValid = false;
    sn = 0;
    PrepareShadows(9, sinfos, sp, 32, &sn);
    Check(sn == 4, "all four cascades get a tightened volume");

    // Receivers as plane lists, for the reference ray test.
    std::vector<std::vector<Plane>> recv(4);
    for (int i = 0; i < 4; ++i) ReadPlanes(vols[i], recv[i]);
    // A receiver samples a cascade only inside its footprint (the box slabs
    // across the light) and nothing visible lies below the floor height.
    auto rayHits = [&](Vec3 p, const std::vector<Plane>& box) {
      for (const auto& recvPlanes : recv) {
        std::vector<Plane> pl = recvPlanes;
        for (const Plane& q : box)
          if (std::fabs(Dot(q.n, L)) < 0.999) pl.push_back(q);
        pl.push_back({Vec3{0, 1, 0}, -g_cfg.shadowFloorY});
        double t0 = 0, t1 = 1e6;
        for (const Plane& q : pl) {
          double a = Dot(q.n, L), b = Dot(q.n, p) + q.d;  // need b + a t >= 0
          if (std::fabs(a) < 1e-12) {
            if (b < 0) t1 = -1;
          } else if (a > 0) {
            t0 = std::max(t0, -b / a);
          } else {
            t1 = std::min(t1, -b / a);
          }
        }
        if (t0 <= t1) return true;
      }
      return false;
    };
    int violations = 0, removed = 0, total = 0, addedMax = 0;
    uint32_t seed = 12345;
    auto rnd = [&]() {
      seed = seed * 1664525u + 1013904223u;
      return (seed >> 8) / double(1 << 24);
    };
    for (int c = 0; c < 4; ++c) {
      uint8_t* tv = *reinterpret_cast<uint8_t**>(sinfos + (4 + c) * kCollectionInfoStride + kCiClipVolume);
      int np = *reinterpret_cast<int32_t*>(tv + kPlaneCount);
      addedMax = std::max(addedMax, np - 6);
      std::vector<Plane> all;
      ReadPlanes(tv, all);
      std::vector<Plane> box;
      ReadPlanes(casc[c], box);
      for (int k = 0; k < 20000; ++k) {
        Vec3 mid = eye + fwd * (ext[c] * 0.5);
        Vec3 p = mid + X * ((rnd() * 2 - 1) * ext[c]) + Y * ((rnd() * 2 - 1) * ext[c]) +
                 L * ((rnd() * 3 - 2) * ext[c]);
        bool inBox = true;
        for (const Plane& q : box)
          if (Dot(q.n, p) + q.d < 0) inBox = false;
        if (!inBox) continue;
        ++total;
        bool kept = true;
        for (const Plane& q : all)
          if (Dot(q.n, p) + q.d < 0) kept = false;
        if (!kept) ++removed;
        if (!kept && rayHits(p, box)) ++violations;
      }
    }
    for (int i = 0; i < sn; ++i) *reinterpret_cast<uint8_t**>(sp[i].slot) = sp[i].original;
    printf("     shadow tightening pass %d: %d of %d box points removed, max %d planes added\n", pass, removed, total,
           addedMax);
    Check(violations == 0, "no removed caster can shadow a view");
    Check(removed > total / 20, "tightening removes a meaningful share of the cascade boxes");
    Check(*reinterpret_cast<uint8_t**>(sinfos + 4 * kCollectionInfoStride + kCiClipVolume) == casc[0],
          "cascade restore");
    }
  }

  // Saccade detection: same views twice = no hold, moved focus = hold.
  {
    g_cfg.saccadeDeg = 1.0;
    g_cfg.saccadeHoldFrames = 6;
    Check(!UpdateGazeAndCheckHold(views), "first frame no hold");
    Check(!UpdateGazeAndCheckHold(views), "static gaze no hold");
    std::vector<ViewInfo> moved = views;
    MakeFrustum(vols[2], eye, fwd, up, -0.2, 0.4, -0.28, 0.3, 0.05, 20000);  // ~5.6 deg shift
    moved[2].fr = DecodeFrustum(vols[2]);
    Check(UpdateGazeAndCheckHold(moved), "gaze jump starts hold");
    for (int i = 0; i < 5; ++i) UpdateGazeAndCheckHold(moved);
    Check(!UpdateGazeAndCheckHold(moved), "hold ends after 6 frames");
  }

  // Real Scene.dll: exports and vtable slot.
  HMODULE scene = LoadLibraryExW(L"E:/SteamLibrary/steamapps/common/DCSWorld/bin/Scene.dll",
                                 nullptr, DONT_RESOLVE_DLL_REFERENCES);
  Check(scene != nullptr, "map Scene.dll");
  if (scene) {
    auto vt = reinterpret_cast<void**>(GetProcAddress(scene, kViewVtable));
    void* fn = reinterpret_cast<void*>(GetProcAddress(scene, kCollectExport));
    Check(vt && fn && vt[kCollectSlot] == fn, "vtable slot 3 == collectSceneObjectsRenderables");
  }
  // Benchmark: synthetic frames, ON is 10% cheaper.
  {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcToUs = 1e6 / static_cast<double>(f.QuadPart);
    g_dir = L"./";
    g_cfg.benchBlocks = 6;
    g_cfg.benchBlockSec = 2;
    g_cfg.benchSettleSec = 0.2;
    std::atomic<bool> stop{false};
    std::thread frames([&] {
      while (!stop) {
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        double ms = g_enabled.load() ? 9.0 : 10.0;
        do QueryPerformanceCounter(&b);
        while ((b.QuadPart - a.QuadPart) * g_qpcToUs < ms * 1000);
        BenchOnFrame(b.QuadPart, g_enabled.load() ? 900 : 1000, 500, 1000);
      }
    });
    g_enabled = true;
    RunBenchmark(0, true);
    stop = true;
    frames.join();
    Check(g_enabled.load(), "bench restores previous state");
  }
  // Profiler: a busy thread must show up with a multi-frame stack.
  {
    std::atomic<bool> stop{false};
    std::thread busy([&] {
      volatile double x = 1.0;
      while (!stop) x = std::sqrt(x + 1.0);
    });
    prof::Run(2, 4, 1, "selftest");
    stop = true;
    busy.join();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(L"./profile_*.txt", &fd);
    bool found = h != INVALID_HANDLE_VALUE;
    bool hasExe = false;
    if (found) {
      FILE* f = nullptr;
      _wfopen_s(&f, (std::wstring(L"./") + fd.cFileName).c_str(), L"r");
      char line[512];
      int shown = 0;
      while (f && fgets(line, sizeof(line), f)) {
        if (strstr(line, "test_main.exe")) hasExe = true;
        if (shown++ < 14) printf("     | %s", line);
      }
      if (f) fclose(f);
      FindClose(h);
      DeleteFileW((std::wstring(L"./") + fd.cFileName).c_str());
    }
    Check(found && hasExe, "profiler sampled the busy thread");
  }
  // Timer cache: the hook only reads the cached value, refreshed by the updater.
  {
    static std::atomic<int> realCalls{0};
    timercache::g_orig = [] { ++realCalls; return ED_FakeTime(); };
    timercache::g_periodUs = 1000;
    timercache::g_cached = timercache::g_orig();
    HANDLE t = CreateThread(nullptr, 0, timercache::Updater, nullptr, 0, nullptr);
    Sleep(20);
    timercache::g_on = true;
    int before = realCalls.load();
    double first = timercache::Hook();
    for (int i = 0; i < 100000; ++i) timercache::Hook();
    int during = realCalls.load() - before;
    Sleep(10);
    double later = timercache::Hook();
    Check(during < 20, "hook path never calls the real timer (only the ~1 kHz updater does)");
    Check(later > first, "cached value advances");
    Check(later - first < 0.1, "cached value tracks real time");
    int refresh = static_cast<int>(timercache::g_refreshes.load());
    Check(refresh > 10, "updater refreshes at ~1 kHz");
    timercache::g_on = false;
    int b2 = realCalls.load();
    timercache::Hook();
    Check(realCalls.load() >= b2 + 1, "disabled cache passes calls through");
    Check(timercache::FindImport(GetModuleHandleW(nullptr), "KERNEL32.dll", "QueryPerformanceCounter") != nullptr,
          "IAT lookup finds an import");

    // [Hotkeys] Toggle parser and the in-flight kill switch.
    {
      int vk = -1, mods = -1;
      Check(ParseHotkey(L"122:6", vk, mods) && vk == 122 && mods == 6, "hotkey 122:6 = Alt+Shift+F11");
      Check(ParseHotkey(L"0:0", vk, mods) && vk == 0 && mods == 0, "hotkey 0:0 = off");
      Check(!ParseHotkey(L"255:1", vk, mods) && vk == 0, "hotkey key 255 rejected");
      Check(!ParseHotkey(L"120:8", vk, mods), "hotkey modifiers 8 rejected");
      Check(!ParseHotkey(L"0:2", vk, mods), "hotkey key 0 with modifiers rejected");
      Check(!ParseHotkey(L"F11", vk, mods) && !ParseHotkey(L"122", vk, mods) && !ParseHotkey(L"122:", vk, mods) &&
                !ParseHotkey(L"122:6x", vk, mods) && !ParseHotkey(L"", vk, mods),
            "hotkey malformed values rejected");

      const Config saved = g_cfg;
      const wchar_t* iniPath = L"./DcsQvCull.ini";
      auto writeIni = [&](const char* extra) {
        FILE* f = nullptr;
        _wfopen_s(&f, iniPath, L"w");
        fprintf(f, "[Timing]\nShaderTimeCache=1\nCacheUs=1000\n[Scene]\nPartitionBoost=1\n[Hotkeys]\nToggle=122:6\n%s", extra);
        fclose(f);
      };
      writeIni("");
      LoadConfig(true);
      Check(g_cfg.toggleVk == 122 && g_cfg.toggleMods == 6 && !g_cfg.developerKeys, "ini hotkey read, developer keys off");
      Check(timercache::g_on.load() && g_partitionBoost.load(), "ini state: cache and boost on");
      ToggleEngine();
      Check(!timercache::g_on.load() && !g_partitionBoost.load() && g_engineOff.load(), "toggle turns both off");
      ToggleEngine();
      Check(timercache::g_on.load() && g_partitionBoost.load() && !g_engineOff.load(), "second toggle restores ini state");
      ToggleEngine();
      g_benchRunningFlag = true;
      Check(!ToggleEngine() && g_engineOff.load(), "toggle refused while a suite runs");
      g_benchRunningFlag = false;
      Sleep(20);
      writeIni("; profile applied\n");
      HANDLE h = CreateFileW(iniPath, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
      FILETIME bumped = g_iniTime;
      reinterpret_cast<ULARGE_INTEGER*>(&bumped)->QuadPart += 10000000;  // +1 s, never equal to the old time
      SetFileTime(h, nullptr, nullptr, &bumped);
      CloseHandle(h);
      LoadConfig(false);
      Check(!g_engineOff.load() && timercache::g_on.load() && g_partitionBoost.load(), "ini change resets the switch to ON");
      // Dev mode: [Dev] IniPath replaces the deployed ini, and is watched too.
      {
        FILE* f = nullptr;
        _wfopen_s(&f, L"./dev_test.ini", L"w");
        fprintf(f, "[Hotkeys]\nToggle=120:3\nDeveloperKeys=1\n");
        fclose(f);
        writeIni("[Dev]\nIniPath=.\\dev_test.ini\n");
        HANDLE h2 = CreateFileW(iniPath, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
        FILETIME bumped2 = g_iniTime;
        reinterpret_cast<ULARGE_INTEGER*>(&bumped2)->QuadPart += 20000000;
        SetFileTime(h2, nullptr, nullptr, &bumped2);
        CloseHandle(h2);
        LoadConfig(false);
        Check(g_cfg.toggleVk == 120 && g_cfg.toggleMods == 3 && g_cfg.developerKeys && !g_devDir.empty(),
              "dev mode: settings come from [Dev] IniPath");
        Sleep(20);
        _wfopen_s(&f, L"./dev_test.ini", L"w");
        fprintf(f, "[Hotkeys]\nToggle=121:1\n");
        fclose(f);
        HANDLE h3 = CreateFileW(L"./dev_test.ini", FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
        FILETIME bumped3 = g_devIniTime;
        reinterpret_cast<ULARGE_INTEGER*>(&bumped3)->QuadPart += 10000000;
        SetFileTime(h3, nullptr, nullptr, &bumped3);
        CloseHandle(h3);
        LoadConfig(false);
        Check(g_cfg.toggleVk == 121 && !g_cfg.developerKeys, "dev mode: a change to the dev ini is picked up");
        DeleteFileW(L"./dev_test.ini");
        g_devDir.clear();
      }
      DeleteFileW(iniPath);
      g_cfg = saved;
      g_partitionBoost = false;
    }

    TerminateThread(t, 0);
    CloseHandle(t);
    timercache::g_orig = nullptr;
  }

  // D3D11 meter on a real device: redundancy and hazard invalidation.
  {
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev,
                      nullptr, &ctx);
    Check(d3ds::LooksLikeContext(ctx, d3ds::ReferenceImpl()), "context recognised by implementation pointer");
    Check(d3ds::InstallFor(ctx), "d3d meter hooks a context vtable");
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 16;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* tex = nullptr;
    dev->CreateTexture2D(&td, nullptr, &tex);
    ID3D11ShaderResourceView* srv = nullptr;
    dev->CreateShaderResourceView(tex, nullptr, &srv);
    ID3D11RenderTargetView* rtv = nullptr;
    dev->CreateRenderTargetView(tex, nullptr, &rtv);

    d3ds::Totals a = d3ds::Snapshot();
    ctx->PSSetShaderResources(0, 1, &srv);  // new
    ctx->PSSetShaderResources(0, 1, &srv);  // redundant
    ctx->OMSetRenderTargets(1, &rtv, nullptr);  // runtime unbinds srv (hazard)
    ctx->PSSetShaderResources(0, 1, &srv);  // must NOT count as redundant
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);  // redundant
    ctx->ClearState();
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);  // after ClearState: not redundant
    ctx->Draw(3, 0);
    d3ds::Totals b = d3ds::Snapshot();
    Check(b.calls[d3ds::kPSSRV] - a.calls[d3ds::kPSSRV] == 3 &&
              b.redundant[d3ds::kPSSRV] - a.redundant[d3ds::kPSSRV] == 1,
          "SRV: 1 redundant of 3, hazard unbind respected");
    Check(b.calls[d3ds::kTopo] - a.calls[d3ds::kTopo] == 3 &&
              b.redundant[d3ds::kTopo] - a.redundant[d3ds::kTopo] == 1,
          "topology: ClearState respected");
    Check(b.draws - a.draws == 1, "draw counted");
    d3ds::Uninstall();  // stop the refresher before the context goes away
    srv->Release();
    rtv->Release();
    tex->Release();
    ctx->Release();
    dev->Release();
  }

  // Suite: configuration check + short profile, report file written.
  {
    g_cfg.suiteProfile = true;
    g_cfg.profileSeconds = 2;
    g_cfg.suiteBenchCull = false;
    g_cfg.suiteBenchThreads = false;
    SuiteThread(nullptr);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(L"./report_*.txt", &fd);
    bool hasConfig = false, hasProfile = false;
    if (h != INVALID_HANDLE_VALUE) {
      FILE* f = nullptr;
      _wfopen_s(&f, (std::wstring(L"./") + fd.cFileName).c_str(), L"r");
      char line[512];
      while (f && fgets(line, sizeof(line), f)) {
        if (strstr(line, "-- configuration --")) hasConfig = true;
        if (strstr(line, "profile: done")) hasProfile = true;
      }
      if (f) fclose(f);
      FindClose(h);
      DeleteFileW((std::wstring(L"./") + fd.cFileName).c_str());
    }
    WIN32_FIND_DATAW pd;
    HANDLE hp = FindFirstFileW(L"./profile_*.txt", &pd);
    if (hp != INVALID_HANDLE_VALUE) {
      DeleteFileW((std::wstring(L"./") + pd.cFileName).c_str());
      FindClose(hp);
    }
    Check(hasConfig && hasProfile, "suite writes report with configuration and profile");
  }
  printf("%d failure(s)\n", g_fail);
  return g_fail;
}
