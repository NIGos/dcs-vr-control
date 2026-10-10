# Virtual lens study (Pimax Crystal Super micro-OLED)

Question: how much pupil swim does the lens add on top of pure geometry, and can it be corrected without lab measurements?

## Data

`pimax_uoled_lens_params.json` was read, read-only, from the memory of the running Pimax compositor (`pi_server.exe`, lens type 5). It contains:

- distortion: Catmull-Rom curve with 18 knots, maxR 0.7514 in tangent units;
- 16.8 mm per tangent unit at the centre;
- chromatic aberration coefficients for red and blue.

The panel is assumed to be a Sony ECX344A: 6.3 µm pixels, 3840x3552. With that panel, the curve gives a field of view of ±48° horizontally and ±43° vertically. That matches SBoys3's independent capture of the compositor mesh (98.2 x 87.1 degrees).

## Method

- `design.py` fits random three-element pancake lenses to every known constraint:
  - the exact distortion curve, to under 1 µm;
  - the chromatic aberration;
  - sharpness, with RMS spot ≤ 12-37 µm at a 3.5 mm pupil;
  - physical edge thicknesses.

  The fold position, materials, eye relief (12-18 mm) and virtual image distance (1-2 m) are drawn at random.
- `refit.py` adds an eyebox constraint: the image must stay sharp when the eye turns to 25 degrees.
- `swim.py` traces each lens from the rotated pupil and reports the angular error. It splits that error into two parts:
  - the geometric part, pupil offset / virtual image distance;
  - the lens part, which is the remainder.

## Result

12 valid lenses, eye rotation radius 10.5 mm. Lens part of the swim at 25 degrees of gaze:

| Constraint | Mean | Standard deviation |
|---|---|---|
| No eyebox constraint | -0.22° | 0.37° (undetermined) |
| Eyebox RMS ≤ 25 µm | -0.04° | 0.12° |
| Eyebox RMS ≤ 18 µm | -0.01° | 0.075° |

If the lens is sharp when the eye turns, its own swim is close to zero within 25 degrees of gaze. What remains is geometric:

- the near-object perspective;
- pupil offset / virtual image distance.

`XR_APILAYER_DCSVR_pupil_shift` corrects that geometric part. Beyond 25 degrees of gaze the lenses disagree again, so no lens-specific correction is attempted there.

## Running it

Fit 56 lenses starting from seed 200:

```
python design.py 200 56 fits.jsonl
```

Refit the valid lenses with the 18 µm eyebox constraint:

```
python refit.py fits.jsonl refit18.jsonl 0.018
```

Trace the pupil swim of the lenses with cost below 2:

```
python swim.py refit18.jsonl swim18.jsonl 2
```
