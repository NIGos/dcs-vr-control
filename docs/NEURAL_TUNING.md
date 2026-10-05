# Neural tuning and RenoDX settings

The **DLSS 5** page exposes controls supported by Cheeky's DLSS-NR 310.8 evaluator. They are saved in the profile and written to its private Cheeky INI. Apply the reviewed profile and restart DCS to use them. The tool does not offer live in-game retuning.

The user's `renodx-dlss5.addon64` contains the corresponding `[RenoDX.DLSS5]` configuration names. The interface no longer imports that file: the same values are set directly under **Advanced image controls** on the DLSS 5 page (folded by default), using the mapping below. **Reset to defaults** restores the defaults below without altering intensity, working scale, processing order or the selected pipeline.

| RenoDX key | Tool control / renderer key | Range | Default |
| --- | --- | --- | --- |
| NRIntensity | DLSS 5 intensity / NrIntensity | 0–100% | 100% |
| NRLocalTone | Local tone / NrLocalToneStrength | 0–200% | 100% |
| NRLocalStructure | Local structure / NrLocalStructureStrength | 0–200% | 100% |
| NRSkinStructure | Skin structure / NrSkinStructureStrength | 0–200% | 100% |
| NRAutoMask | Automatic mask / NrAutomaticMask | Off/on | Off |
| NRUICorrection | UI correction / NrUiCorrection | Off/on | Off |
| NRColorStrength | Colour strength / NrColorStrength | 0–200% | 100% |
| NRTransferStrength | Transfer strength / NrHdrTransferStrength | 0–200% | 100% |
| NRPaperWhiteScale | Paper white / NrPaperWhiteScale | 0.01–8× | 1× |
| NRDepthMode | Depth / NrDepthConvention | Game/normal/reversed | Game |
| NRMVecScaleX | Motion X / NrMotionScaleXMultiplier | −4–4× | 1× |
| NRMVecScaleY | Motion Y / NrMotionScaleYMultiplier | −4–4× | 1× |

Colour and transfer strength tune Cheeky's colour reconstruction, rather than claiming byte-identical RenoDX implementation. Mask and UI settings request model behavior; acceptance/readback does not prove improved HUD quality. Skin structure requires the model's automatic mask path. Leave depth and motion scales at their defaults until captured guides justify changing them.

The pinned integration has no verified equivalent for global tone, diffuse-white nits, model preset/style selection, NR-native upscaling, multipass, screenshot keys or ReShade/Streamline hook toggles. These keys are reported as unmapped. The tool uses working-scale reconstruction to reduce neural cost; this is separate from enabling NR-native upscaling. The source comparison is with the user's actual add-on, whose identity strings identify the DLSS-NR 310.8 contract, and the pinned [Cheeky implementation](https://github.com/ClarkCheekyKent/CheekyFoveatedDLSS/tree/d6c18b23d31f7339735114c621bc3f9066e1a32d).

## Hardware evidence

`scripts/test-combined-gpu.ps1 -NeuralRuntime <nvngx_dlssnr.dll>` validates signed NVIDIA runtime/core binaries and builds an offscreen fixture. Build OFXR's full source libraries first. No game or headset runtime starts.

On the local RTX 5090, the NVIDIA-signed 310.8.0.0 runtime completed 12 neural evaluations across two histories. The test reads actual pixels, executes the exact Quad Views projection shaders with 256-pixel peripheral and 512-pixel focus textures, and passes the composed 512-pixel stereo image to source-built OFXR's NVIDIA synthesizer. It checks peripheral retention, focus presence, an unchanged real frame, two-eye optical-flow timing and synthetic output distinct from passthrough. Non-default tone/structure/skin/mask/UI parameters are applied through the real NGX API. The fixture exercises both NR processing-order settings.

This verifies the GPU stages together. It does not exercise DCS's DLSS inputs or DX11 transport, the focus adapter's real game mapping, the full Quad Views OpenXR layer, Pimax/Sboys presentation, eye tracking, moving cockpit geometry or performance at headset resolution. Runtime signature/version alone is not a functioning DCS-session certificate.
