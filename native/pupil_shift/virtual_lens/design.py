"""Fit random pancake lenses to the Pimax micro-OLED data (distortion, focal, chroma, sharpness)."""
import json, sys, time, os
import numpy as np
from scipy.optimize import least_squares
import vlens as V

SCALE = np.array([0.01] * 6 + [1e-5] * 6 + [1e-7] * 6 + [1e-9] * 6 + [1.0] * 6)
LO = np.array([-20] * 6 + [-10] * 18 + [0.8, 0.8, 0.8, 0.05, 0.05, 0.3])
HI = np.array([20] * 6 + [10] * 18 + [7.0, 7.0, 7.0, 8.0, 8.0, 15.0])
DIST_F = np.arange(0, 45.01, 2.5)
CA_F = np.arange(5, 40.01, 5)
SPOT_F = np.array([0, 10, 20, 30, 38, 44])
PLASTICS = ['PMMA', 'COP-E48R', 'APL5014', 'PC', 'OKP4HT', 'EP5000']
GLASSES = ['N-BK7', 'H-ZF52']
FOLDS = [(2, 3), (2, 4), (2, 5), (2, 6), (4, 5), (4, 6), (1, 4), (1, 6)]


def unpack(x, cfg):
    v = x * SCALE
    return dict(c=v[0:6], a4=v[6:12], a6=v[12:18], a8=v[18:24], t=v[24:27], g=v[27:29],
                bfl=v[29], er=cfg['er'], mats=cfg['mats'], fold=tuple(cfg['fold']))


def chief(lens, fields, wl):
    d = V.dir_from_angles(fields, np.zeros_like(fields))
    p = np.zeros_like(d)
    hits, ok, hmax = lens.trace(p, d, wl)
    return hits[:, 0], ok, hmax


def residuals(x, cfg, max_field=50.0, spot_w=1.0, detail=False):
    P = unpack(x, cfg)
    lens = V.Lens(P)
    res = []
    fields = DIST_F[DIST_F <= max_field]
    sfields = SPOT_F[SPOT_F <= max_field]
    # one batched trace for green: chief rays + spot bundles
    dch = V.dir_from_angles(fields, np.zeros_like(fields))
    disc = V.pupil_disc(1.75, 2)
    Pb, Db = [np.zeros_like(dch)], [dch]
    for f in sfields:
        d = V.dir_from_angles(np.array(float(f)), np.array(0.0))
        e1 = np.array([d[2], 0, -d[0]]); e2 = np.array([0, 1.0, 0])
        Pp = disc[:, :1] * e1 + disc[:, 1:] * e2
        Dd = cfg['vid'] * d - Pp
        Pb.append(Pp); Db.append(Dd / np.linalg.norm(Dd, axis=1)[:, None])
    hits, okall, hmax = lens.trace(np.vstack(Pb), np.vstack(Db), V.WL['G'])
    nF = len(fields)
    hG, okG = hits[:nF, 0], okall[:nF]
    target = V.pimax_panel_from_angle(fields)
    err = np.where(okG, (hG - target) / 0.01, 50.0)
    res.append(err)
    # chroma: red and blue chief rays in one trace each
    hR, okR, h2 = chief(lens, fields, V.WL['R'])
    hB, okB, h3 = chief(lens, fields, V.WL['B'])
    hmax = np.maximum(hmax, np.maximum(h2, h3))
    caf = CA_F[CA_F <= max_field]
    ca = []
    for name, h, ok in (('R', hR, okR), ('B', hB, okB)):
        c0, c1 = V.CHROMA[name]
        if ok.all() and okG.all() and np.all(np.diff(h) > 0):
            hg = np.interp(caf, fields, hG)
            th = np.interp(hg, h, fields)
            ratio = np.tan(np.radians(th)) / np.tan(np.radians(caf)) - 1
            rsq = (hg / V.MPT_MM) ** 2
            ca += list((ratio - (c0 + c1 * rsq)) / 0.001)
        else:
            ca += [30.0] * len(caf)
    res.append(np.array(ca) * 0.5)
    sp = []
    rms_list = []
    nb = len(disc)
    for k, f in enumerate(sfields):
        sl = slice(nF + k * nb, nF + (k + 1) * nb)
        hh, ok = hits[sl], okall[sl]
        if not ok.all():
            sp.append(30.0); rms_list.append(np.nan); continue
        rms = np.sqrt(((hh - hh.mean(0)) ** 2).sum(1).mean())
        rms_list.append(float(rms))
        thr = 0.012 + 0.025 * (f / 50) ** 2
        sp.append(max(0.0, rms - thr) / 0.005 * spot_w)
    res.append(np.array(sp))
    # eyebox: the eye must see sharply when it rotates to look off axis
    if cfg.get('eyebox'):
        rp = 10.5
        for gx, gy in ((15, 0), (25, 0), (0, 20), (18, 18), (-25, 0)):
            g = V.dir_from_angles(np.array(float(gx)), np.array(float(gy)))
            pc = np.array([0, 0, -rp]) + rp * g
            e1 = np.cross([0, 1.0, 0], g); e1 /= np.linalg.norm(e1); e2 = np.cross(g, e1)
            Pp = pc + disc[:, :1] * e1 + disc[:, 1:] * e2
            Dd = pc + cfg['vid'] * g - Pp
            Dd /= np.linalg.norm(Dd, axis=1)[:, None]
            hh, ok, hm = lens.trace(Pp, Dd, V.WL['G'])
            hmax = np.maximum(hmax, hm)
            if not ok.all():
                sp.append(30.0); continue
            rms = np.sqrt(((hh - hh.mean(0)) ** 2).sum(1).mean())
            rms_list.append(float(rms))
            sp.append(max(0.0, rms - cfg.get('eyebox_thr', 0.018)) / 0.005)
        res[-1] = np.array(sp)
    # physical constraints
    ph = []
    R = hmax + 0.5
    zv = lens.zv
    def zs(i, r):
        s, ok = V.sag(r * r, *lens.coef[i])
        return zv[i] + s if ok else np.nan
    for j in range(3):
        r = max(R[2 * j], R[2 * j + 1])
        et = zs(2 * j + 1, r) - zs(2 * j, r)
        ph.append(max(0.0, 0.6 - et) / 0.1 if np.isfinite(et) else 30.0)
    for i in (1, 3):
        r = max(R[i], R[i + 1])
        gap = zs(i + 1, r) - zs(i, r)
        ph.append(max(0.0, 0.1 - gap) / 0.05 if np.isfinite(gap) else 30.0)
    s6 = zs(5, R[5])
    ph.append(max(0.0, (s6 - lens.zpanel) + 0.2) / 0.05 if np.isfinite(s6) else 30.0)
    L = lens.zpanel - P['er']
    ph.append(max(0.0, L - 30.0))
    res.append(np.array(ph))
    out = np.concatenate(res)
    out = np.where(np.isfinite(out), out, 50.0)
    if detail:
        return dict(dist_err_mm=(hG - target).tolist(), ca=list(map(float, ca)), rms=rms_list,
                    phys=ph, L=float(L), hmax=hmax.tolist(), ok=bool(okG.all()))
    return out


def random_cfg(rng):
    mats = [rng.choice(PLASTICS) if rng.random() < 0.85 else rng.choice(GLASSES) for _ in range(3)]
    return dict(er=float(rng.uniform(12, 18)), vid=float(rng.uniform(1000, 2000)), mats=mats,
                fold=FOLDS[rng.integers(len(FOLDS))])


def random_x(rng, cfg):
    x = np.zeros(30)
    x[0:6] = rng.normal(0, 1.5, 6)
    b = cfg['fold'][1] - 1
    x[b] = -rng.uniform(2.0, 3.5)          # half mirror concave towards the eye
    x[24:27] = rng.uniform(1.5, 4.0, 3)
    x[27:29] = rng.uniform(0.3, 2.5, 2)
    x[29] = rng.uniform(1.0, 6.0)
    return x


def fit_one(seed):
    rng = np.random.default_rng(seed)
    cfg = random_cfg(rng)
    x0 = random_x(rng, cfg)
    sph = np.r_[0:6, 24:30]                # spheres + spacings
    allp = np.arange(30)
    stages = [(sph, 25.0, 0.2, 300), (allp, 25.0, 0.5, 300), (allp, 35.0, 1.0, 400),
              (allp, 42.0, 1.0, 500), (allp, 45.0, 1.0, 800)]
    x = x0.copy()
    try:
        for idx, mf, sw, nf in stages:
            def f(xs, idx=idx, mf=mf, sw=sw):
                xx = x.copy(); xx[idx] = xs
                return residuals(xx, cfg, mf, sw)
            r = least_squares(f, x[idx], bounds=(LO[idx], HI[idx]), max_nfev=nf, x_scale='jac')
            x[idx] = r.x
    except Exception as e:
        return dict(seed=seed, cfg=cfg, fail=str(e))
    det = residuals(x, cfg, 45.0, detail=True)
    cost = float((residuals(x, cfg, 45.0) ** 2).sum())
    return dict(seed=seed, cfg=cfg, x=x.tolist(), cost=cost, detail=det)


if __name__ == '__main__':
    from multiprocessing import Pool
    start, count, out = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
    t0 = time.time()
    with Pool(int(os.environ.get('NPROC', '14'))) as pool, open(out, 'a') as fo:
        for res in pool.imap_unordered(fit_one, range(start, start + count)):
            fo.write(json.dumps(res) + '\n'); fo.flush()
            print(res['seed'], round(res.get('cost', -1), 2), f'{time.time() - t0:.0f}s', flush=True)
