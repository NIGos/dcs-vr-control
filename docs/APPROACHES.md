# Combining DCS VR rendering: approaches and decision

Research and implementation review: 2 October 2026. This comparison covers the relevant integration families found in primary project documentation and the pinned source code. It cannot certify every unpublished mod or predict which configuration will be fastest in a headset.

## Selected approach

The 0.2 preview uses a local, opt-in Cheeky focus-pair adapter with the bundled Quad-Views-Foveated compositor. DCS retains its four views and native DLSS inputs. Only a positively mapped focus-left or focus-right output enters the adapted processing path. The software compositor then produces stereo images for OFXR, followed by the selected OpenXR runtime.

```text
DCS / DX11 native DLSS
  peripheral left + peripheral right: original processing
  focus left + focus right: adapted Cheeky, optional DLSS 5 via DX12 Transport
        ↓
Quad-Views-Foveated: four views → two composed views
        ↓
OFXR: stereo optical-flow frame generation
        ↓
Pimax OpenXR OR SteamVR OpenXR → Sboys
```

This is the strongest implementation candidate because it preserves game-provided temporal inputs and original feature ownership, uses the existing two-view neural transport, and puts interpolation after focus/periphery composition. These are engineering judgments. A performance ranking requires measurements.

On the original driver route, the full combination uses **software Quad Views on Pimax OpenXR**. Native Pimax Quad Views remains a separate baseline/experimental framegen route. Sboys uses SteamVR OpenXR and the same software compositor. This keeps one common composition path for both drivers.

The framegen backend here is OFXR optical-flow interpolation, with NVIDIA or FidelityFX selected in its settings. It is not an integration of NVIDIA's native DLSS Frame Generation/Multi Frame Generation into DCS. DLSS 5 refers to the optional neural rendering pass. [OFXR documentation](https://github.com/tig3rmast3r/OFXR-Bridge/blob/0.2.1/README.md), [NVIDIA neural rendering research](https://research.nvidia.com/labs/adlr/DLSS5/).

## Alternatives evaluated

| Approach | Benefit | Main obstacle | Decision |
| --- | --- | --- | --- |
| Native DLSS SR + native Pimax Quad Views | Existing original-runtime baseline; no neural transport | No DLSS 5 integration; interpolation of native quad resources remains unverified | Keep as baseline |
| Stereo Cheeky SR/NR + OFXR | Reuses upstream stereo behavior; available on both runtimes | Foveated reconstruction does not provide Quad Views rasterization savings | Keep as fallback and control measurement |
| **Focus-pair adaptation + software composition + OFXR** | Keeps four-view rendering while limiting extra processing to mapped focus resources | Focus/periphery appearance, real game mapping and neural runtime compatibility need headset verification | **Implemented in 0.2** |
| Four independent Cheeky NR views | Could process the peripheral images too and reduce neural appearance differences | Requires broader view-role, ABI, calibration and history work; greater processing cost | Next candidate if visible seams make focus-only NR unsuitable |
| NR after stereo composition | Neural treatment across the full view; naturally stereo output | Must obtain correctly composed depth, motion vectors and projection history; a color image alone is insufficient to reuse the game's SR call faithfully | Defer until focus-only quality is measured |
| OFXR before quad composition | Can operate near source view resources | Four-slice mapping limits and independent interpolation of moving focus boundaries; synchronization and continuity become harder | Avoid for the combined preset |
| Native Pimax quad composition followed by custom FG/NR | Could retain the vendor composition path | Composition occurs inside the runtime; an ordinary layer receiving four views does not expose its composed stereo resources | Requires a different runtime/compositor integration |
| VectorXR as compositor | Alternative software Quad Views controls and diagnostics | Adds a different compositor/settings contract that this adapter has not tested | Candidate replacement after reproducing the bundled path |
| OptiScaler/OptiScaler-DLSSNR family | Alternative upscaler hooks and temporal input bridges | OptiFG is documented for DX12; replacing the upscaler does not solve OpenXR four-view roles or VR presentation | Useful reference; avoid adding a second competing NGX hook |
| UniversalDLSS5 | Generic D3D11/D3D12 neural injector with game-guide and optical-flow paths | Presentation interception does not establish DCS OpenXR focus mapping, stereo history or quad composition | Possible alternative backend; no DCS four-view proof found in reviewed documentation |
| Luke Ross / UEVR neural bridges and AFW | Existing engine/mod-specific VR integrations | Their capture and presentation contracts belong to those engines/mods; DCS uses its own renderer | Reference only; not drop-in DCS components |
| Runtime reprojection / motion smoothing | Existing compositor interpolation alternative | Different timing/quality tradeoff; stacking independent interpolators can obscure results | Compare separately against OFXR |
| Native game-engine DLSS FG/NR integration | Best access to engine data, UI masks and scheduling | Requires an Eagle Dynamics integration or a substantial version-dependent renderer adaptation | Long-term route, outside this preview |

Primary sources: [Cheeky 0.5.4](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS/blob/v0.5.4/README.md), [Quad-Views-Foveated](https://github.com/mbucchia/Quad-Views-Foveated/tree/1.1.3), [VectorXR](https://github.com/DienerTech/vectorxr), [OptiScaler](https://github.com/OptiScaler/OptiScaler), [UniversalDLSS5](https://github.com/MotionflowOffical/UniversalDLSS5), [Luke Ross bridge](https://github.com/eregnier/dlss5-vr), [UEVR bridge](https://github.com/eregnier/uevr-dlss5), [Sboys 1.3.0](https://github.com/sboys3/CustomHeadsetOpenVR/releases/tag/1.3.0). The table's integration assessments are our analysis, not claims of upstream support.

## Limitations addressed in code

- Cheeky's two-slot gaze ABI now carries the two VARJO focus views only when the private adapter environment is enabled. The app still submits all four views.
- Exact resource/rectangle identity or observed copies establish focus routing. Packed resources and single-mip destination array slices are covered; dimensions or evaluation order alone never establish an eye.
- Missing, stale, unfocused, ambiguous or unsupported mapping returns to original DCS processing. Mapping loss releases adapted transport, clears neural history and requests one original-history reset on transition.
- Quad Views owns gaze and coverage in the combined preset. Cheeky processes the full focus region, avoiding a second moving foveation crop inside it.
- The Cheeky layer avoids a premature downstream extension query that crashed the full software-compositor layer chain. DCS-requested extensions remain forwarded.
- Runtime, layer order and provider settings are scoped to the launched process. Both runtime routes have isolated deployment and restoration tests.

See [adapter details](QUAD_FOCUS_ADAPTER.md) and [validation evidence](VALIDATION.md) for what was actually exercised.

## How to choose the best configuration

First compare baseline, Quad Views + OFXR, and stereo Cheeky + OFXR in the same mission. Then add the combined path with neural rendering off to isolate mapping and composition. Add DLSS 5 last, starting with fixed focus placement before dynamic gaze.

Select the winner using cockpit readability, both-eye consistency, focus boundary quality, median/p95 application frame times, visible synthetic presentation and acceptable motion latency. Count real application frames separately from runtime submissions. If neural cost outweighs its visible benefit, the best performance profile may leave it off. If focus-only NR creates visible lighting changes, evaluate four-view NR or post-composition NR rather than hiding the seam with larger settings.

The proprietary NVIDIA neural runtime, valid headset gaze and real rendering evidence cannot be manufactured by an installer or an offline test. Their absence is handled explicitly; it is not reported as a successful rendering session.
