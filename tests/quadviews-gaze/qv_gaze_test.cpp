// Offline checks of native/quadviews/dcsvr_gaze.h: the saccade lead and the fixation deadzone of the eye-tracked
// focus area, on simulated gaze at DCS's rendered frame rates with frame generation (45 FPS in 2X, 30 FPS in 3X).
// Built and run by scripts/build-quadviews.ps1.
#include <cmath>
#include <cstdio>
#include <random>

#include "dcsvr_gaze.h"

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

constexpr float Rad = 0.0174532925f;
// A Crystal-like eye view: about 50 degrees each way.
const float HalfTan = std::tan(50.f * Rad);
const float NdcPerDeg = dcsvr::GazeFilter::NdcPerDegree(-HalfTan, HalfTan);

// Gaze unit vector for a horizontal angle (degrees, right positive) looking down -Z, and its NDC position.
void Unit(float deg, float& x, float& y, float& z) { x = std::sin(deg * Rad); y = 0.f; z = -std::cos(deg * Rad); }
float Ndc(float deg) { return std::tan(deg * Rad) / HalfTan; }

struct Frame {
    bool widened;
    dcsvr::GazeVec2 min, max, gaze;
};

// One located frame for both eyes with the gaze at `deg`; the focus box is +-section around the (filtered) gaze.
Frame Locate(dcsvr::GazeFilter& f, const dcsvr::GazeSettings& s, int64_t t, float deg, float section = 0.34f) {
    float x, y, z;
    Unit(deg, x, y, z);
    f.UpdateSpeed(t, x, y, z, s);
    Frame out{};
    for (int eye = 0; eye < 2; ++eye) {
        const dcsvr::GazeVec2 g = f.Eye(eye, t, {Ndc(deg), 0.f}, NdcPerDeg, NdcPerDeg, s);
        dcsvr::GazeVec2 lo{std::fmax(-1.f, g.x - section), std::fmax(-1.f, g.y - section)};
        dcsvr::GazeVec2 hi{std::fmin(1.f, g.x + section), std::fmin(1.f, g.y + section)};
        const bool widened = f.Extend(eye, t, lo, hi, s);
        if (eye == 0) out = {widened, lo, hi, g};
    }
    return out;
}
} // namespace

int main() {
    dcsvr::GazeSettings on;
    on.widening = true;
    on.deadzoneDeg = 0.5f;
    on.leadMs = 55.f;

    // A 25-degree saccade at about 450 deg/s, sampled at 45 FPS (2X): the gaze moves 0 -> 10 -> 20 -> 25 degrees.
    {
        dcsvr::GazeFilter f;
        const int64_t dt = 22'222'222;
        int64_t t = 1'000'000'000;
        Locate(f, on, t, 0.f);
        const Frame a = Locate(f, on, t += dt, 10.f);
        Check(f.InSaccade() && f.Stats().saccades == 1, "2X: a 450 deg/s move starts a saccade");
        Check(a.widened && a.max.x > Ndc(10.f) + 0.34f + 0.05f && std::fabs(a.min.x - (Ndc(10.f) - 0.34f)) < 1e-5f,
              "2X: the focus box reaches ahead to the right only");
        const Frame b = Locate(f, on, t += dt, 20.f);
        Check(b.widened && b.max.x >= Ndc(25.f), "2X: the box rendered mid-saccade already covers the 25-degree landing");
        Check(b.max.x - (Ndc(20.f) + 0.34f) <= on.maxExtend + 1e-5f, "2X: the reach is clamped to maxExtend");
        const Frame c = Locate(f, on, t += dt, 25.f);
        const Frame d = Locate(f, on, t += dt, 25.f);
        Check(!f.InSaccade() && d.widened && d.max.x - (Ndc(25.f) + 0.34f) < c.max.x - (Ndc(25.f) + 0.34f),
              "2X: after the eye rests the reach fades out over the 40 ms hold");
        const Frame e = Locate(f, on, t += dt, 25.f);
        Check(!e.widened && std::fabs(e.max.x - (Ndc(25.f) + 0.34f)) < 1e-5f, "2X: then the normal box again");
    }

    // The same saccade in 3X (30 FPS, longer lead): the reach still covers the landing.
    {
        dcsvr::GazeFilter f;
        dcsvr::GazeSettings s = on;
        s.leadMs = 85.f;
        const int64_t dt = 33'333'333;
        int64_t t = 1'000'000'000;
        Locate(f, s, t, 0.f);
        const Frame a = Locate(f, s, t += dt, 15.f);
        Check(a.widened && a.max.x >= Ndc(25.f), "3X: the first frame of the saccade covers the 25-degree landing");
    }

    // A leftward, upward-free saccade extends the left edge.
    {
        dcsvr::GazeFilter f;
        int64_t t = 1'000'000'000;
        Locate(f, on, t, 0.f);
        const Frame a = Locate(f, on, t += 22'222'222, -12.f);
        Check(a.widened && a.min.x < Ndc(-12.f) - 0.34f - 0.05f && std::fabs(a.max.x - (Ndc(-12.f) + 0.34f)) < 1e-5f,
              "a leftward saccade extends only the left edge");
    }

    // Fixation with tracker noise (0.3 degrees RMS): the focus stays put, and a real 3-degree move still follows at once.
    {
        dcsvr::GazeFilter f;
        std::mt19937 rng(7);
        std::normal_distribution<float> noise(0.f, 0.3f);
        int64_t t = 1'000'000'000;
        const float first = Locate(f, on, t, 5.f).gaze.x;
        int moved = 0, widened = 0;
        for (int i = 0; i < 200; ++i) {
            const Frame fr = Locate(f, on, t += 22'222'222, 5.f + std::fmax(-0.45f, std::fmin(0.45f, noise(rng))));
            if (fr.gaze.x != first) ++moved;
            if (fr.widened) ++widened;
        }
        Check(moved == 0, "fixation noise within the deadzone never moves the focus");
        Check(widened == 0 && f.Stats().saccades == 0, "fixation noise is never taken for a saccade");
        const Frame shift = Locate(f, on, t += 22'222'222, 8.f);
        Check(std::fabs(shift.gaze.x - Ndc(8.f)) < 1e-6f, "a 3-degree move is followed in the same frame (no lag)");
    }

    // Off: no widening, no deadzone.
    {
        dcsvr::GazeFilter f;
        dcsvr::GazeSettings off;
        int64_t t = 1'000'000'000;
        Locate(f, off, t, 0.f);
        const Frame a = Locate(f, off, t += 22'222'222, 10.f);
        const Frame b = Locate(f, off, t += 22'222'222, 10.2f);
        Check(!a.widened && std::fabs(b.gaze.x - Ndc(10.2f)) < 1e-6f, "with both options off the focus follows the gaze unchanged");
    }

    // The same display time located twice counts once; a gap (blink, pause) is never a saccade.
    {
        dcsvr::GazeFilter f;
        int64_t t = 1'000'000'000;
        Locate(f, on, t, 0.f);
        Locate(f, on, t, 0.f);
        Check(f.Stats().eyeFrames == 2, "a frame located twice is counted once per eye");
        Locate(f, on, t += 400'000'000, 20.f);
        Check(!f.InSaccade(), "a 400 ms gap (blink) is not a saccade");
        float x, y, z;
        Unit(0.f, x, y, z);
        f.UpdateSpeed(t + 22'222'222, std::nanf(""), y, z, on);
        Check(!f.InSaccade(), "a non-finite gaze changes nothing");
    }

    // DCS locating views for nearly the same display time twice: never a speed sample (no bogus saccade or peak), and a
    // frame held by the deadzone is counted once however often its views are located.
    {
        dcsvr::GazeFilter f;
        int64_t t = 1'000'000'000;
        Locate(f, on, t, 5.f);
        Locate(f, on, t + 1'000, 5.3f);  // 1 us later, 0.3 degrees of noise: would read as 300,000 deg/s
        Check(!f.InSaccade() && f.Stats().saccades == 0 && f.Stats().peakSpeed < 1000.f, "a display time 1 us later is not a speed sample");
        for (int i = 0; i < 20; ++i) {
            t += 22'222'222;
            Locate(f, on, t, 5.1f);
            Locate(f, on, t, 5.1f);
        }
        Check(f.Stats().held <= f.Stats().eyeFrames, "held frames never exceed eye-frames");
    }

    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
