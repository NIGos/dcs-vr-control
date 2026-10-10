"""Virtual pancake lens: sequential 3D ray tracer, design fit and pupil-swim evaluation.

Coordinates: millimetres, z points from the eye towards the display. The design
pupil P0 (eye pupil, straight gaze) is at the origin. Rays are traced in reverse,
from the eye to the panel, which is the usual way to design eyepieces.

Lens: 3 elements, 6 surfaces s1..s6 (s1 faces the eye). A pancake fold is made of
a reflective polarizer (RP) on surface a and a half mirror (HM) on surface b, a < b.
Reverse path: eye -> s1..s_b (transmit at a, reflect at b) -> back to a (reflect)
-> forward to b (transmit) -> s_b+1..s6 -> panel.
"""
import numpy as np

# ---------------------------------------------------------------- Pimax data
K_PIMAX = np.array([1.00016, 1.01739, 1.03625, 1.05658, 1.07842, 1.102, 1.12748, 1.15512,
                    1.18504, 1.21739, 1.25225, 1.28983, 1.3304, 1.37529, 1.42702, 1.49145,
                    1.57788, 1.69782])
MAXR = 0.7513983845710754
MPT_MM = 16.8                              # metres per tan angle at centre, in mm
CHROMA = dict(R=(-0.004996, -0.002793), B=(0.005907, 0.003212))
WL = dict(R=0.620, G=0.530, B=0.460)       # micrometres, micro-OLED primaries (assumed)


def _catmull(t):
    """Catmull-Rom through K_PIMAX, t in [0, 1] (Oculus-style, end knots extrapolated)."""
    K = K_PIMAX
    n = len(K) - 1
    x = np.clip(t, 0, 1) * n
    i = np.minimum(x.astype(int), n - 1)
    f = x - i
    p0 = np.where(i == 0, 2 * K[0] - K[1], K[np.maximum(i - 1, 0)])
    p1 = K[i]
    p2 = K[i + 1]
    p3 = np.where(i + 2 > n, 2 * K[n] - K[n - 1], K[np.minimum(i + 2, n)])
    return 0.5 * (2 * p1 + (-p0 + p2) * f + (2 * p0 - 5 * p1 + 4 * p2 - p3) * f ** 2
                  + (-p0 + 3 * p1 - 3 * p2 + p3) * f ** 3)


def pimax_tan_from_panel(r_mm):
    rt = np.asarray(r_mm, float) / MPT_MM
    return rt * _catmull((rt / MAXR) ** 2)


_r_grid = np.linspace(0, MAXR * MPT_MM, 4000)
_tan_grid = pimax_tan_from_panel(_r_grid)


def pimax_panel_from_angle(theta_deg):
    return np.interp(np.tan(np.radians(theta_deg)), _tan_grid, _r_grid)


# ---------------------------------------------------------------- materials
MATERIALS = {  # name: (nd, Vd)
    'PMMA': (1.4918, 57.4), 'COP-E48R': (1.5310, 56.0), 'APL5014': (1.5445, 56.0),
    'PC': (1.5855, 29.9), 'OKP4HT': (1.6320, 23.3), 'EP5000': (1.6355, 23.9),
    'N-BK7': (1.5168, 64.2), 'H-ZF52': (1.8467, 23.8),
}


def index(mat, wl_um):
    nd, vd = MATERIALS[mat]
    lf, ld, lc = 0.4861, 0.5876, 0.6563
    B = (nd - 1) / vd / (1 / lf ** 2 - 1 / lc ** 2)
    A = nd - B / ld ** 2
    return A + B / wl_um ** 2


# ---------------------------------------------------------------- surfaces
def sag(r2, c, a4, a6, a8):
    arg = 1 - c * c * r2
    ok = arg > 1e-9
    s = c * r2 / (1 + np.sqrt(np.where(ok, arg, 1.0))) + a4 * r2 ** 2 + a6 * r2 ** 3 + a8 * r2 ** 4
    return s, ok


def dsag_dr_over_r(r2, c, a4, a6, a8):
    arg = np.maximum(1 - c * c * r2, 1e-9)
    return c / np.sqrt(arg) + 4 * a4 * r2 + 6 * a6 * r2 ** 2 + 8 * a8 * r2 ** 3


def intersect(p, d, zv, coef):
    c, a4, a6, a8 = coef
    t = (zv - p[:, 2]) / d[:, 2]
    ok = np.isfinite(t)
    for _ in range(8):
        q = p + t[:, None] * d
        r2 = q[:, 0] ** 2 + q[:, 1] ** 2
        s, okk = sag(r2, c, a4, a6, a8)
        g = dsag_dr_over_r(r2, c, a4, a6, a8)
        f = q[:, 2] - zv - s
        df = d[:, 2] - g * (q[:, 0] * d[:, 0] + q[:, 1] * d[:, 1])
        step = f / np.where(np.abs(df) > 1e-9, df, 1e-9)
        t = t - step
    q = p + t[:, None] * d
    r2 = q[:, 0] ** 2 + q[:, 1] ** 2
    s, okk = sag(r2, c, a4, a6, a8)
    ok &= okk & (np.abs(q[:, 2] - zv - s) < 1e-6) & (r2 < 30.0 ** 2)
    g = dsag_dr_over_r(r2, c, a4, a6, a8)
    n = np.stack([-g * q[:, 0], -g * q[:, 1], np.ones_like(g)], 1)
    n /= np.linalg.norm(n, axis=1)[:, None]
    return q, n, ok, r2


def refract(d, n, n1, n2):
    cos = -(d * n).sum(1)
    n = np.where(cos[:, None] < 0, -n, n)
    cos = np.abs(cos)
    eta = n1 / n2
    k = 1 - eta ** 2 * (1 - cos ** 2)
    ok = k > 0
    dn = eta * d + (eta * cos - np.sqrt(np.maximum(k, 0)))[:, None] * n
    return dn / np.linalg.norm(dn, axis=1)[:, None], ok


def reflect(d, n):
    return d - 2 * (d * n).sum(1)[:, None] * n


class Lens:
    """params: dict with c[6], a4[6], a6[6], a8[6], t (3 element thicknesses),
    g (2 air gaps), bfl, er (eye relief), mats[3], fold=(a, b)."""

    def __init__(self, P):
        self.P = P
        z = [P['er']]
        seq = [P['t'][0], P['g'][0], P['t'][1], P['g'][1], P['t'][2]]
        for s in seq:
            z.append(z[-1] + s)
        self.zv = np.array(z)
        self.zpanel = self.zv[-1] + P['bfl']
        self.coef = [(P['c'][i], P['a4'][i], P['a6'][i], P['a8'][i]) for i in range(6)]

    def media(self, wl):
        m = self.P['mats']
        return [1.0, index(m[0], wl), 1.0, index(m[1], wl), 1.0, index(m[2], wl), 1.0]

    def trace(self, p, d, wl, record=False):
        """Trace rays (N,3) from the eye side to the panel. Returns panel hits (N,2), ok mask,
        and per-surface max ray height (for clear apertures)."""
        reg = self.media(wl)
        a, b = self.P['fold']
        ok = np.ones(len(p), bool)
        hmax = np.zeros(6)
        p = p.copy(); d = d.copy()

        def step(p, d, ok, i, mode, n1, n2):
            q, nrm, okk, r2 = intersect(p, d, self.zv[i], self.coef[i])
            ok = ok & okk
            if ok.any():
                hmax[i] = max(hmax[i], np.sqrt(r2[ok].max()))
            if mode == 'R':
                dn = reflect(d, nrm); okr = np.ones(len(p), bool)
            else:
                dn, okr = refract(d, nrm, n1, n2)
            return q, dn, ok & okr

        # forward to b
        for i in range(0, b):  # surfaces index 0..b-1 are s1..s_b
            if i < b - 1:
                p, d, ok = step(p, d, ok, i, 'T', reg[i], reg[i + 1])
            else:
                p, d, ok = step(p, d, ok, i, 'R', 0, 0)
        # back to a
        for i in range(b - 2, a - 2, -1):
            if i > a - 1:
                p, d, ok = step(p, d, ok, i, 'T', reg[i + 1], reg[i])
            else:
                p, d, ok = step(p, d, ok, i, 'R', 0, 0)
        # forward to b and beyond
        for i in range(a, 6):
            p, d, ok = step(p, d, ok, i, 'T', reg[i], reg[i + 1])
        t = (self.zpanel - p[:, 2]) / d[:, 2]
        ok &= (t > 0) & (d[:, 2] > 0)
        q = p + t[:, None] * d
        return q[:, :2], ok, hmax


# ---------------------------------------------------------------- ray bundles
def pupil_disc(radius, rings=2, per_ring=6):
    pts = [(0.0, 0.0)]
    for k in range(1, rings + 1):
        rr = radius * k / rings
        n = per_ring * k
        for j in range(n):
            a = 2 * np.pi * j / n
            pts.append((rr * np.cos(a), rr * np.sin(a)))
    return np.array(pts)


def dir_from_angles(tx_deg, ty_deg):
    """Direction with tan components tan(tx), tan(ty) (viewing angles), unit length."""
    v = np.stack([np.tan(np.radians(tx_deg)), np.tan(np.radians(ty_deg)), np.ones_like(np.asarray(tx_deg, float))], -1)
    return v / np.linalg.norm(v, axis=-1, keepdims=True)


def bundle(lens, pupil_center, dvec, vid, wl, pupil_r=1.75, rings=2):
    """Rays from a pupil disc (perpendicular to dvec) aimed at the virtual point at distance vid."""
    disc = pupil_disc(pupil_r, rings)
    # basis perpendicular to dvec
    up = np.array([0, 1.0, 0])
    e1 = np.cross(up, dvec); e1 /= np.linalg.norm(e1)
    e2 = np.cross(dvec, e1)
    P = pupil_center + disc[:, :1] * e1 + disc[:, 1:] * e2
    V = pupil_center + vid * dvec
    D = V - P
    D /= np.linalg.norm(D, axis=1)[:, None]
    hits, ok, _ = lens.trace(P, D, wl)
    return hits, ok
