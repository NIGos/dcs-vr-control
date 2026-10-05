# Frame pacing and FPS limits

Open **Framegen** before a live test. The tool preserves existing FPS limits and image-quality settings by default. Refresh rate is a value you enter after checking Pimax or SteamVR; it is not detected headset telemetry.

OFXR 0.2.1 produces one intermediate frame between rendered frames when synthesis succeeds. Its OpenXR presenter handles display pacing. The manager uses DCS's real `graphics.maxFPS` option for a rendered-frame cap; it does not write a fictitious OFXR limiter key. OFXR is optical-flow frame generation, separate from NVIDIA DLSS Frame Generation.

| FPS limit mode | DCS change | Intended use |
| --- | --- | --- |
| Preserve existing DCS limit | No change to `graphics.maxFPS` | Keep an existing setup; the detected cap appears in the frame-rate summary. |
| Runtime pacing | Set cap to 300 FPS | Leave DCS cap headroom while investigating OpenXR pacing. This is not an unlimited mode. |
| Match refresh | Refresh / 2 with OFXR; full refresh with framegen off | Explicit comparison of a matched rendered cap. A 90 Hz selection writes 45 FPS with OFXR and 90 FPS without it. |
| Custom rendered-frame cap | User value, 30–300 FPS | Fine-tune real game FPS independently of display refresh. Fractional values are preserved. |

The supported range matches the installed DCS `MissionEditor/modules/Options/optionsDb.lua`: `graphics('maxFPS')`, slider 30–300, default 180. This version does not assume that zero means unlimited. The original value, including a missing field, is backed up and restored. Match refresh recalculates when framegen is enabled or disabled; the wizard shows that derived cap in Review.

**A cap is a ceiling, not a performance guarantee.** For a 90 Hz headset, 45 rendered FPS can supply up to 90 fresh frames/s with one intermediate frame per rendered frame. A 30 FPS cap can supply at most 60 before other bottlenecks. GPU/CPU load, failed synthesis, gaze/composition work and runtime presentation can reduce fresh output. A 300 FPS DCS cap does not force a 300 FPS render loop through OpenXR.

## Other limiters and synchronization

- **Disable DCS desktop VSync** explicitly writes `graphics.sync = false`. Leaving it off preserves the original value. This does not disable OpenXR synchronization or rotational timewarp.
- **NVIDIA / RTSS / other limiters** are a check you do yourself, listed by preflight: inspect NVIDIA global and DCS program **Max Frame Rate** and **Background Application Max Frame Rate**, plus the DCS RTSS profile. The tool does not read or modify these driver settings, so it no longer asks you to report them (earlier versions did; those saved answers are ignored). An external cap's position relative to OFXR must be established live.
- **Runtime motion smoothing** (Pimax Smart Smoothing, SteamVR motion smoothing) is likewise a preflight check you do yourself when frame generation is on: establish OFXR cadence with this second interpolation stage disabled first. The tool does not change the runtime.
- **Legacy autoexec.cfg** is detected beside the chosen options.lua. Simple `max_fps` assignments are reported without executing Lua. Unsupported commands remain unknown. The file is preserved; creating or changing it after Apply invalidates the saved launch check so pacing cannot silently change.
- Detected **RTSS processes** are reported as process facts. Their presence does not prove a cap is active, and their absence does not prove driver caps are off.

Use **Run preflight** for fresh detected settings and warnings. **Review files** lists the exact previous and next DCS scalar values, including caps and VSync. **Launch DCS** (or **Apply without launching**) writes those values with the launch configuration. Launch checks treat an edit to an owned cap as a change: the profile is restored and applied again, and a conflicting edit stops at Recovery. Recovery preserves conflicting user edits and reports them.

## First live comparison

1. Confirm the actual headset mode and enter its refresh rate. Keep all image-quality values fixed.
2. Check external/background caps and runtime interpolation. Begin with one cap strategy at a time.
3. Compare Preserve or Runtime pacing against Match refresh in the same cockpit scene. Match refresh is an optional test, not an automatic optimization. If either a runtime or another layer already reduces application cadence, do not add a second half-rate mechanism blindly.
4. Measure real application frame time, fresh synthetic frames, dropped frames, HUD clarity, head-motion latency and comfort. OFXR's output overlay counts accepted OpenXR submissions, including possible presenter repeats; a 90 FPS overlay alone does not prove 90 fresh images reached the panel.
5. Exit DCS, restore, then repeat for framegen, foveation and the combined neural pipeline. Export a fresh diagnostic report with the cap and refresh recorded.

## Offline evidence and limits

The release's manager and actual WebView2 tests cover both runtime routes, custom/derived caps, precise numeric values, preview invalidation, applied launch checks, restoration and the combined pipeline with the supplied signed DLSS-NR runtime. Native tests exercise OFXR's SteamVR continuous presenter while the application deliberately pauses below the half-refresh target. The exact bundled V116 DLL also passes this delayed-application fixture: presentation continues and frame order is retained. Repeated presentation during a pause is not fresh frame generation.

These fixtures do not establish DCS's actual limiter placement, the live Pimax/Sboys compositor, physical scanout or latency. No real game files or driver settings were changed during this verification.

Primary references: [OFXR source and documentation](https://github.com/tig3rmast3r/OFXR-Bridge), [OFXR overlay semantics](https://github.com/tig3rmast3r/OFXR-Bridge/blob/0.2.1/docs/FPS_OVERLAY.md), [NVIDIA Max Frame Rate and background-limit reference](https://www.nvidia.com/content/Control-Panel-Help/vLatest/en-us/mergedProjects/3D%20Settings/Manage_3D_Settings_%28reference%29.htm). DCS's cap and desktop VSync fields were verified directly in this PC's installed options database.
