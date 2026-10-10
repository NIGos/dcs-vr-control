"""Refit converged lenses with the eyebox sharpness constraint."""
import json, sys
import numpy as np
from scipy.optimize import least_squares
import design as D

def refit(rec):
    cfg = dict(rec['cfg']); cfg['eyebox'] = True; cfg['eyebox_thr'] = float(sys.argv[3])
    x = np.array(rec['x'])
    r = least_squares(lambda xx: D.residuals(xx, cfg, 45.0), x, bounds=(D.LO, D.HI), max_nfev=1200, x_scale='jac')
    det = D.residuals(r.x, cfg, 45.0, detail=True)
    return dict(seed=rec['seed'], cfg=cfg, x=r.x.tolist(), cost=float((D.residuals(r.x, cfg, 45.0) ** 2).sum()), detail=det)

if __name__ == '__main__':
    from multiprocessing import Pool
    recs = [json.loads(l) for l in open(sys.argv[1])]
    sel = [r for r in recs if r.get('cost', 1e9) < 1]
    with Pool(14) as p, open(sys.argv[2], 'w') as fo:
        for o in p.imap_unordered(refit, sel):
            fo.write(json.dumps(o) + '\n'); fo.flush()
            print(o['seed'], round(o['cost'], 2), flush=True)
