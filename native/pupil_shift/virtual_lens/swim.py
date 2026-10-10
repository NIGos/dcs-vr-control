"""Pupil swim of fitted virtual lenses.

For a gaze direction g the eye pupil sits at P(g) = C + rp * g, C being the eye rotation
centre (rp behind the design pupil P0). A world direction w (object at infinity) is seen
through the lens at the panel point x where the bundle from P(g), aimed at the virtual
image distance, lands. The software renders x with the static design mapping (pupil at P0),
so the object appears at d0(x). Error = d0(x) - w, in degrees. At straight gaze it is zero.
"""
import json, sys
import numpy as np
import vlens as V
import design as D

PUPIL_R = 1.5


def static_map(lens, vid):
    th = np.linspace(0, 45, 91)
    rho = np.array([centroid(lens, np.zeros(3), np.array([0, 0, 1.0]), V.dir_from_angles(np.array(t), np.array(0.0)), vid)[0]
                    for t in th])
    return th, rho


def centroid(lens, pc, gaze, w, vid):
    disc = V.pupil_disc(PUPIL_R, 2)
    up = np.array([0, 1.0, 0])
    e1 = np.cross(up, gaze); e1 /= np.linalg.norm(e1)
    e2 = np.cross(gaze, e1)
    P = pc + disc[:, :1] * e1 + disc[:, 1:] * e2
    Dd = pc + vid * w - P
    Dd /= np.linalg.norm(Dd, axis=1)[:, None]
    hits, ok, _ = lens.trace(P, Dd, V.WL['G'])
    if not ok.all():
        return np.array([np.nan, np.nan])
    return hits.mean(0)


def error(lens, vid, smap, gx, gy, wx, wy, rp):
    th, rho = smap
    g = V.dir_from_angles(np.array(float(gx)), np.array(float(gy)))
    pc = np.array([0, 0, -rp]) + rp * g
    w = V.dir_from_angles(np.array(float(wx)), np.array(float(wy)))
    x = centroid(lens, pc, g, w, vid)
    r = np.hypot(*x)
    if not np.isfinite(r) or r > rho[-1]:
        return np.array([np.nan, np.nan])
    t0 = np.tan(np.radians(np.interp(r, rho, th)))
    tx, ty = t0 * x / max(r, 1e-12)
    return np.degrees(np.arctan([tx, ty])) - np.array([wx, wy])


def evaluate(rec, rps=(9.5, 10.5, 12.0)):
    cfg = rec['cfg']
    lens = V.Lens(D.unpack(np.array(rec['x']), cfg))
    vid = cfg['vid']
    smap = static_map(lens, vid)
    out = dict(seed=rec['seed'], cfg=cfg)
    gazes = [(5, 0), (10, 0), (15, 0), (20, 0), (25, 0), (30, 0), (35, 0), (0, 15), (0, 25), (15, 15), (25, 20)]
    for rp in rps:
        fov = {f'{gx},{gy}': error(lens, vid, smap, gx, gy, gx, gy, rp).tolist() for gx, gy in gazes}
        # whole-field shift at gaze 25 deg horizontal: change of error versus straight gaze
        grid = [(wx, wy) for wx in range(-40, 41, 10) for wy in range(-30, 31, 10)]
        field = []
        for wx, wy in grid:
            e1 = error(lens, vid, smap, 25, 0, wx, wy, rp)
            field.append([wx, wy] + e1.tolist())
        out[f'rp{rp}'] = dict(foveal=fov, field_gaze25=field)
    return out


if __name__ == '__main__':
    from multiprocessing import Pool
    recs = [json.loads(l) for l in open(sys.argv[1])]
    sel = [r for r in recs if 'x' in r and r['cost'] < float(sys.argv[3])]
    print('designs', len(sel))
    with Pool(14) as p, open(sys.argv[2], 'w') as fo:
        for o in p.imap_unordered(evaluate, sel):
            fo.write(json.dumps(o) + '\n'); fo.flush()
            print(o['seed'], o['rp10.5']['foveal']['25,0'], flush=True)
