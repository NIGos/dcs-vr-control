$ErrorActionPreference = 'Stop'
$workspaceRoot = Split-Path -Parent $PSScriptRoot
$quadRoot = Join-Path $workspaceRoot 'external/quadviews'
if (-not (Test-Path -LiteralPath $quadRoot)) {
    & git clone --depth 1 --branch 1.1.3 https://github.com/mbucchia/Quad-Views-Foveated.git $quadRoot
    if ($LASTEXITCODE -ne 0) { throw 'Quad Views download failed.' }
}
if ((& git -C $quadRoot rev-parse HEAD) -ne '957ff0327185ea29acc41fc86c4cdc3caf428fb9') { throw 'Unexpected Quad Views revision.' }
& git -C $quadRoot submodule update --init --depth 1 external/OpenXR-SDK external/OpenXR-SDK-Source external/OpenXR-MixedReality external/FidelityFX-CAS external/fmt
if ($LASTEXITCODE -ne 0) { throw 'Quad Views dependencies failed.' }
$wilVersion = '1.0.220201.1'
$wilRoot = Join-Path $quadRoot "packages/Microsoft.Windows.ImplementationLibrary.$wilVersion"
if (-not (Test-Path -LiteralPath $wilRoot)) {
    $wilArchive = Join-Path $workspaceRoot '.cache/wil.nupkg'
    New-Item -ItemType Directory -Path (Split-Path -Parent $wilArchive) -Force | Out-Null
    $wilUrl = "https://api.nuget.org/v3-flatcontainer/microsoft.windows.implementationlibrary/$wilVersion/microsoft.windows.implementationlibrary.$wilVersion.nupkg"
    if (-not (Test-Path -LiteralPath $wilArchive)) { Invoke-WebRequest -Uri $wilUrl -OutFile $wilArchive }
    # Pinned like every other build input; a different download is never extracted.
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $wilArchive).Hash -ne '924F1553BF191578261312BEDFA9E9A441AD9FCA59AAEAE4F0038AA3533CCD34') {
        Remove-Item -LiteralPath $wilArchive -Force
        throw 'WIL package digest mismatch.'
    }
    # Extract beside the target and move only a complete extraction, so an interrupted run is never trusted.
    $wilPartial = $wilRoot + '.partial'
    if (Test-Path -LiteralPath $wilPartial) { Remove-Item -LiteralPath $wilPartial -Recurse -Force }
    [IO.Compression.ZipFile]::ExtractToDirectory($wilArchive, $wilPartial)
    Move-Item -LiteralPath $wilPartial -Destination $wilRoot
}
$solutionDir = $quadRoot + '\'
# Per-process settings preserve global configuration for other applications.
$layerSource = Join-Path $quadRoot 'openxr-api-layer/layer.cpp'
$layerText = [IO.File]::ReadAllText($layerSource)
if (-not $layerText.Contains('DCSVR_QUAD_SETTINGS')) {
    $original = 'LoadConfiguration(localAppData / "settings.cfg");'
    $replacement = @'
const DWORD profileLength = GetEnvironmentVariableW(L"DCSVR_QUAD_SETTINGS", nullptr, 0);
                    if (profileLength > 0 && profileLength < 32768) {
                        std::vector<wchar_t> profilePath(profileLength);
                        if (GetEnvironmentVariableW(L"DCSVR_QUAD_SETTINGS", profilePath.data(), profileLength)) {
                            LoadConfiguration(std::filesystem::path(profilePath.data()));
                        }
                    } else {
                        LoadConfiguration(localAppData / "settings.cfg");
                    }
'@
    if (-not $layerText.Contains($original)) { throw 'Quad Views configuration source changed.' }
    [IO.File]::WriteAllText($layerSource, $layerText.Replace($original, $replacement))
}
# stereo_output_multiplier: resolution of the composited stereo image, as a multiple of the runtime's recommendation.
# Upstream composites at focus_multiplier, so focus 1.25 makes a 1.25x stereo image that the runtime then scales down;
# with framegen every frame-generation copy of that oversized image costs VRAM (about 10.7 GB at Crystal Super
# resolution). Unset keeps upstream behaviour.
$layerText = [IO.File]::ReadAllText($layerSource)
if (-not $layerText.Contains('Composited stereo resolution')) {
    $edits = @(
        @('        float m_focusPixelDensity{1.f};', "        float m_focusPixelDensity{1.f};`n        float m_stereoOutputMultiplier{0.f};"),
        @('                    } else if (name == "focus_multiplier") {', "                    } else if (name == `"stereo_output_multiplier`") {`n                        m_stereoOutputMultiplier = std::max(0.1f, std::stof(value));`n                        parsed = true;`n                    } else if (name == `"focus_multiplier`") {"),
        @('                    m_focusPixelDensity * stereoViews[xr::StereoView::Left].recommendedImageRectWidth;', '                    (m_stereoOutputMultiplier > 0.f ? m_stereoOutputMultiplier : m_focusPixelDensity) * stereoViews[xr::StereoView::Left].recommendedImageRectWidth;'),
        @('            m_needComputeBaseFov = false;', "            Log(fmt::format(`"Composited stereo resolution: {}x{}\n`", m_fullFovResolution.width, m_fullFovResolution.height));`n            m_needComputeBaseFov = false;")
    )
    foreach ($edit in $edits) {
        if (([regex]::Matches($layerText, [regex]::Escape($edit[0]))).Count -ne 1) { throw "Quad Views source changed: $($edit[0])" }
        $layerText = $layerText.Replace($edit[0], $edit[1])
    }
    [IO.File]::WriteAllText($layerSource, $layerText)
}
# Direct source views: upstream copies the stereo and focus images into flat textures every frame (its own TODO)
# and samples the copies for composition and sharpening. When the view's rectangle is the whole of a single-slice,
# single-sample, shader-readable image, a view of that image in the same format reads the very texels the copy
# would hold, so the copies and the flat textures are skipped. Anything else (sub-rectangles, texture arrays, MSAA,
# a format the image cannot be viewed in) keeps the copy. Pixel-identical by construction.
$layerText = [IO.File]::ReadAllText($layerSource)
if (-not $layerText.Contains('DCSVR direct source view')) {
    # Multi-line anchors follow the checkout's line endings.
    $nl = if ($layerText.Contains("`r`n")) { "`r`n" } else { "`n" }
    $edits = @(
        @(
            (('            // Copy to a flat texture for sampling.', '            {') -join $nl),
            (('            // DCSVR direct source view: what composition and sharpening sample, per source.',
             '            ComPtr<ID3D11ShaderResourceView> stereoSourceSrv;',
             '            ComPtr<ID3D11ShaderResourceView> focusSourceSrv;',
             '            // Copy to a flat texture for sampling.', '            {') -join $nl)
        ),
        @(
            (('                                                    uint32_t startSlot) {',
             '                    D3D11_TEXTURE2D_DESC desc{};') -join $nl),
            (('                                                    uint32_t startSlot) -> ComPtr<ID3D11ShaderResourceView> {',
             '                    ComPtr<ID3D11ShaderResourceView> sourceSrv;',
             '                    {',
             '                        D3D11_TEXTURE2D_DESC sourceDesc{};',
             '                        image->GetDesc(&sourceDesc);',
             '                        if (sourceDesc.ArraySize == 1 && view.subImage.imageArrayIndex == 0 &&',
             '                            sourceDesc.SampleDesc.Count == 1 && (sourceDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) &&',
             '                            view.subImage.imageRect.offset.x == 0 && view.subImage.imageRect.offset.y == 0 &&',
             '                            view.subImage.imageRect.extent.width == (int32_t)sourceDesc.Width &&',
             '                            view.subImage.imageRect.extent.height == (int32_t)sourceDesc.Height) {',
             '                            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};',
             '                            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;',
             '                            srvDesc.Format = (DXGI_FORMAT)swapchain.createInfo.format;',
             '                            srvDesc.Texture2D.MipLevels = 1;',
             '                            if (SUCCEEDED(m_applicationDevice->CreateShaderResourceView(',
             '                                    image, &srvDesc, sourceSrv.ReleaseAndGetAddressOf()))) {',
             '                                swapchain.flatImage[startSlot + viewIndex].Reset();',
             '                                return sourceSrv;',
             '                            }',
             '                        }',
             '                    }',
             '                    D3D11_TEXTURE2D_DESC desc{};') -join $nl)
        ),
        @(
            (('                                                           &box);', '                };') -join $nl),
            (('                                                           &box);',
             '                    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};',
             '                    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;',
             '                    srvDesc.Format = (DXGI_FORMAT)swapchain.createInfo.format;',
             '                    srvDesc.Texture2D.MipLevels = 1;',
             '                    CHECK_HRCMD(m_applicationDevice->CreateShaderResourceView(',
             '                        swapchain.flatImage[startSlot + viewIndex].Get(), &srvDesc, sourceSrv.ReleaseAndGetAddressOf()));',
             '                    return sourceSrv;',
             '                };') -join $nl)
        ),
        @(
            (('                flattenSourceImage(sourceImage, stereoView, swapchainForStereoView, 0);',
             '                flattenSourceImage(sourceFocusImage, focusView, swapchainForFocusView, xr::StereoView::Count);') -join $nl),
            (('                stereoSourceSrv = flattenSourceImage(sourceImage, stereoView, swapchainForStereoView, 0);',
             '                focusSourceSrv =',
             '                    flattenSourceImage(sourceFocusImage, focusView, swapchainForFocusView, xr::StereoView::Count);') -join $nl)
        ),
        @(
            (('                ComPtr<ID3D11ShaderResourceView> srv;',
             '                {',
             '                    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};',
             '                    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;',
             '                    desc.Format = (DXGI_FORMAT)swapchainForFocusView.createInfo.format;',
             '                    desc.Texture2D.MipLevels = 1;',
             '                    CHECK_HRCMD(m_applicationDevice->CreateShaderResourceView(',
             '                        swapchainForFocusView.flatImage[xr::StereoView::Count + viewIndex].Get(),',
             '                        &desc,',
             '                        srv.ReleaseAndGetAddressOf()));',
             '                }') -join $nl),
            '                ComPtr<ID3D11ShaderResourceView> srv = focusSourceSrv;'
        ),
        @(
            (('                ComPtr<ID3D11ShaderResourceView> srvForStereoView;',
             '                {',
             '                    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};',
             '                    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;',
             '                    desc.Format = (DXGI_FORMAT)swapchainForStereoView.createInfo.format;',
             '                    desc.Texture2D.MipLevels = 1;',
             '                    CHECK_HRCMD(',
             '                        m_applicationDevice->CreateShaderResourceView(swapchainForStereoView.flatImage[viewIndex].Get(),',
             '                                                                      &desc,',
             '                                                                      srvForStereoView.ReleaseAndGetAddressOf()));',
             '                }') -join $nl),
            '                ComPtr<ID3D11ShaderResourceView> srvForStereoView = stereoSourceSrv;'
        ),
        @(
            (('                ComPtr<ID3D11ShaderResourceView> srvForFocusView;',
             '                {',
             '                    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};',
             '                    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;',
             '                    desc.Format = m_sharpenFocusView ? DXGI_FORMAT_R16G16B16A16_FLOAT',
             '                                                     : (DXGI_FORMAT)swapchainForFocusView.createInfo.format;',
             '                    desc.Texture2D.MipLevels = 1;',
             '                    CHECK_HRCMD(m_applicationDevice->CreateShaderResourceView(',
             '                        m_sharpenFocusView ? swapchainForFocusView.sharpenedImage[viewIndex].Get()',
             '                                           : swapchainForFocusView.flatImage[xr::StereoView::Count + viewIndex].Get(),',
             '                        &desc,',
             '                        srvForFocusView.ReleaseAndGetAddressOf()));',
             '                }') -join $nl),
            (('                ComPtr<ID3D11ShaderResourceView> srvForFocusView = focusSourceSrv;',
             '                if (m_sharpenFocusView) {',
             '                    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};',
             '                    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;',
             '                    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;',
             '                    desc.Texture2D.MipLevels = 1;',
             '                    CHECK_HRCMD(m_applicationDevice->CreateShaderResourceView(',
             '                        swapchainForFocusView.sharpenedImage[viewIndex].Get(),',
             '                        &desc,',
             '                        srvForFocusView.ReleaseAndGetAddressOf()));',
             '                }') -join $nl)
        )
    )
    foreach ($edit in $edits) {
        if (([regex]::Matches($layerText, [regex]::Escape($edit[0]))).Count -ne 1) { throw "Quad Views source changed: $(($edit[0] -split "`n")[0])" }
        $layerText = $layerText.Replace($edit[0], $edit[1])
    }
    [IO.File]::WriteAllText($layerSource, $layerText)
}
# focus_view_shape: upstream blends the focus view in over a rectangular band whose alpha never drops below 0.5
# (max(0.5, s.x * s.y)), so the focus area always ends in a 50 % step that stays visible against the much softer
# periphery. focus_view_shape=2 makes the focus area the ellipse inscribed in the focus view (a circle for a square
# focus view) and fades it out radially with smoothstep over the same width smoothen_focus_view_edges gives the
# rectangle (2 x its value in normalized view coordinates), down to exactly 0 at the edge. Other values (or none)
# keep upstream behaviour. Larger exponents round a square instead (4: a rounded square).
$layerText = [IO.File]::ReadAllText($layerSource)
if (-not $layerText.Contains('m_focusViewShape')) {
    $nl = if ($layerText.Contains("`r`n")) { "`r`n" } else { "`n" }
    $edits = @(
        @(
            (('        alignas(4) bool debugFocusView;', '    };') -join $nl),
            (('        alignas(4) bool debugFocusView;',
             '        alignas(4) float focusViewShape;',
             '        alignas(4) float padding[3];',
             '    };') -join $nl)
        ),
        @('        float m_smoothenFocusViewEdges{0.2f};', "        float m_smoothenFocusViewEdges{0.2f};$($nl)        float m_focusViewShape{0.f};"),
        @('                    } else if (name == "smoothen_focus_view_edges") {',
          (('                    } else if (name == "focus_view_shape") {',
            '                        m_focusViewShape = std::clamp(std::stof(value), 0.f, 16.f);',
            '                        parsed = true;',
            '                    } else if (name == "smoothen_focus_view_edges") {') -join $nl)),
        @('                drawing.smoothingArea = m_smoothenFocusViewEdges;',
          "                drawing.smoothingArea = m_smoothenFocusViewEdges;$($nl)                drawing.focusViewShape = m_focusViewShape;"),
        @('                            Log(fmt::format("Edge smoothing: {:.2f}\n", m_smoothenFocusViewEdges));',
          (('                            Log(fmt::format("Edge smoothing: {:.2f}\n", m_smoothenFocusViewEdges));',
            '                            if (m_focusViewShape >= 1.f) {',
            '                                Log(fmt::format("Focus view shape: rounded, exponent {:.1f}\n", m_focusViewShape));',
            '                            }') -join $nl))
    )
    foreach ($edit in $edits) {
        if (([regex]::Matches($layerText, [regex]::Escape($edit[0]))).Count -ne 1) { throw "Quad Views source changed: $(($edit[0] -split "`n")[0])" }
        $layerText = $layerText.Replace($edit[0], $edit[1])
    }
    [IO.File]::WriteAllText($layerSource, $layerText)
}
$pixelShaderSource = Join-Path $quadRoot 'openxr-api-layer/ProjectionPS.hlsl'
$pixelShaderText = [IO.File]::ReadAllText($pixelShaderSource)
if (-not $pixelShaderText.Contains('focusViewShape')) {
    $nl = if ($pixelShaderText.Contains("`r`n")) { "`r`n" } else { "`n" }
    $edits = @(
        @('    bool debugFocusView;', "    bool debugFocusView;$($nl)    float focusViewShape;"),
        @('    if (smoothingArea) {',
          (('    if (focusViewShape >= 1) {',
            '        // DCS VR Control: a rounded focus area fading out radially to exactly 0 at its edge.',
            '        float2 d = abs(layer1ProjectedCoordNdc);',
            '        float r = pow(pow(d.x, focusViewShape) + pow(d.y, focusViewShape), 1 / focusViewShape);',
            '        float width = clamp(2 * smoothingArea, 0.01, 0.95);',
            '        color1.a = isInside * (1 - smoothstep(1 - width, 1, r));',
            '    } else if (smoothingArea) {') -join $nl))
    )
    foreach ($edit in $edits) {
        if (([regex]::Matches($pixelShaderText, [regex]::Escape($edit[0]))).Count -ne 1) { throw "Quad Views shader changed: $(($edit[0] -split "`n")[0])" }
        $pixelShaderText = $pixelShaderText.Replace($edit[0], $edit[1])
    }
    [IO.File]::WriteAllText($pixelShaderSource, $pixelShaderText)
}
# Early out: about nine pixels in ten of the composited image lie outside the focus area (a 0.34 x 0.34 focus section,
# smaller still inside the round shape). There the focus view's alpha is exactly 0 - outside the rectangle by
# isInside, outside the circle because smoothstep(1 - width, 1, r) is exactly 1 for r >= 1 - so the composition is
# the stereo colour through the same alpha handling. Those pixels now skip the focus sample and the mask maths. The
# debug view of the focus layer keeps the full path. The focus sample is taken at level 0 (the view has one mip
# level; the sampler is the same), as gradients are not available in the branch.
$pixelShaderText = [IO.File]::ReadAllText($pixelShaderSource)
if (-not $pixelShaderText.Contains('DCSVR early out')) {
    $nl = if ($pixelShaderText.Contains("`r`n")) { "`r`n" } else { "`n" }
    $edits = @(
        ,@('    float4 color1 = sourceFocusTexture.Sample(sourceSampler, layer1TexCoord);',
          (('    // DCSVR early out: no focus contribution here, the stereo colour as the full path would leave it.',
            '    bool focusVisible = all(abs(layer1ProjectedCoordNdc) < 1);',
            '    if (focusVisible && focusViewShape >= 1) {',
            '        float2 e = abs(layer1ProjectedCoordNdc);',
            '        focusVisible = pow(pow(e.x, focusViewShape) + pow(e.y, focusViewShape), 1 / focusViewShape) < 1;',
            '    }',
            '    [branch] if (!focusVisible && !debugFocusView) {',
            '        if (ignoreAlpha) {',
            '            color0.a = 1;',
            '        }',
            '        if (!isUnpremultipliedAlpha) {',
            '            color0 = unpremultiplyAlpha(color0);',
            '        }',
            '        color0 = premultiplyAlpha(color0);',
            '        return float4(color0.rgb, color0.a);',
            '    }',
            '    float4 color1 = sourceFocusTexture.SampleLevel(sourceSampler, layer1TexCoord, 0);') -join $nl))
    )
    foreach ($edit in $edits) {
        if (([regex]::Matches($pixelShaderText, [regex]::Escape($edit[0]))).Count -ne 1) { throw "Quad Views shader changed: $(($edit[0] -split "`n")[0])" }
        $pixelShaderText = $pixelShaderText.Replace($edit[0], $edit[1])
    }
    [IO.File]::WriteAllText($pixelShaderSource, $pixelShaderText)
}
# Cached views: upstream creates the composition's views every frame on the game's render thread (the direct source
# views, the sharpening UAV and SRV, the composition RTV: five per eye). A view of the same resource with the same
# description is the same view, so each is created once and kept with the swapchain whose images it views (they live
# exactly as long as it does) or with the sharpening texture it views (dropped when that is recreated).
$layerText = [IO.File]::ReadAllText($layerSource)
if (-not $layerText.Contains('dcsvrSourceSrv')) {
    $nl = if ($layerText.Contains("`r`n")) { "`r`n" } else { "`n" }
    $edits = @(
        @('            ComPtr<ID3D11Texture2D> sharpenedImage[xr::StereoView::Count];',
          (('            ComPtr<ID3D11Texture2D> sharpenedImage[xr::StereoView::Count];',
            '            // DCSVR cached views of this swapchain''s images and of the sharpened textures.',
            '            std::map<ID3D11Texture2D*, ComPtr<ID3D11ShaderResourceView>> dcsvrSourceSrv;',
            '            std::map<ID3D11Texture2D*, ComPtr<ID3D11RenderTargetView>> dcsvrRtv;',
            '            ComPtr<ID3D11UnorderedAccessView> dcsvrSharpenedUav[xr::StereoView::Count];',
            '            ComPtr<ID3D11ShaderResourceView> dcsvrSharpenedSrv[xr::StereoView::Count];') -join $nl)),
        @((('                            if (SUCCEEDED(m_applicationDevice->CreateShaderResourceView(',
            '                                    image, &srvDesc, sourceSrv.ReleaseAndGetAddressOf()))) {',
            '                                swapchain.flatImage[startSlot + viewIndex].Reset();',
            '                                return sourceSrv;',
            '                            }') -join $nl),
          (('                            auto& cachedSrv = swapchain.dcsvrSourceSrv[image];',
            '                            if (cachedSrv || SUCCEEDED(m_applicationDevice->CreateShaderResourceView(',
            '                                    image, &srvDesc, cachedSrv.ReleaseAndGetAddressOf()))) {',
            '                                swapchain.flatImage[startSlot + viewIndex].Reset();',
            '                                return cachedSrv;',
            '                            }') -join $nl)),
        @((('                        CHECK_HRCMD(m_applicationDevice->CreateTexture2D(',
            '                            &desc, nullptr, swapchainForFocusView.sharpenedImage[viewIndex].ReleaseAndGetAddressOf()));') -join $nl),
          (('                        CHECK_HRCMD(m_applicationDevice->CreateTexture2D(',
            '                            &desc, nullptr, swapchainForFocusView.sharpenedImage[viewIndex].ReleaseAndGetAddressOf()));',
            '                        swapchainForFocusView.dcsvrSharpenedUav[viewIndex].Reset();',
            '                        swapchainForFocusView.dcsvrSharpenedSrv[viewIndex].Reset();') -join $nl)),
        @((('                ComPtr<ID3D11UnorderedAccessView> uav;',
            '                {',
            '                    D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};',
            '                    desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;',
            '                    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;',
            '                    CHECK_HRCMD(m_applicationDevice->CreateUnorderedAccessView(',
            '                        swapchainForFocusView.sharpenedImage[viewIndex].Get(), &desc, uav.ReleaseAndGetAddressOf()));',
            '                }') -join $nl),
          (('                ComPtr<ID3D11UnorderedAccessView>& uav = swapchainForFocusView.dcsvrSharpenedUav[viewIndex];',
            '                if (!uav) {',
            '                    D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};',
            '                    desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;',
            '                    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;',
            '                    CHECK_HRCMD(m_applicationDevice->CreateUnorderedAccessView(',
            '                        swapchainForFocusView.sharpenedImage[viewIndex].Get(), &desc, uav.ReleaseAndGetAddressOf()));',
            '                }') -join $nl)),
        @((('                if (m_sharpenFocusView) {',
            '                    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};',
            '                    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;',
            '                    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;',
            '                    desc.Texture2D.MipLevels = 1;',
            '                    CHECK_HRCMD(m_applicationDevice->CreateShaderResourceView(',
            '                        swapchainForFocusView.sharpenedImage[viewIndex].Get(),',
            '                        &desc,',
            '                        srvForFocusView.ReleaseAndGetAddressOf()));',
            '                }') -join $nl),
          (('                if (m_sharpenFocusView) {',
            '                    ComPtr<ID3D11ShaderResourceView>& sharpenedSrv = swapchainForFocusView.dcsvrSharpenedSrv[viewIndex];',
            '                    if (!sharpenedSrv) {',
            '                        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};',
            '                        desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;',
            '                        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;',
            '                        desc.Texture2D.MipLevels = 1;',
            '                        CHECK_HRCMD(m_applicationDevice->CreateShaderResourceView(',
            '                            swapchainForFocusView.sharpenedImage[viewIndex].Get(),',
            '                            &desc,',
            '                            sharpenedSrv.ReleaseAndGetAddressOf()));',
            '                    }',
            '                    srvForFocusView = sharpenedSrv;',
            '                }') -join $nl)),
        @((('                ComPtr<ID3D11RenderTargetView> rtv;',
            '                {',
            '                    D3D11_RENDER_TARGET_VIEW_DESC desc{};',
            '                    desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;',
            '                    desc.Format = (DXGI_FORMAT)swapchainForStereoView.createInfo.format;',
            '                    CHECK_HRCMD(m_applicationDevice->CreateRenderTargetView(',
            '                        destinationImage, &desc, rtv.ReleaseAndGetAddressOf()));',
            '                }') -join $nl),
          (('                ComPtr<ID3D11RenderTargetView>& rtv = swapchainForStereoView.dcsvrRtv[destinationImage];',
            '                if (!rtv) {',
            '                    D3D11_RENDER_TARGET_VIEW_DESC desc{};',
            '                    desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;',
            '                    desc.Format = (DXGI_FORMAT)swapchainForStereoView.createInfo.format;',
            '                    CHECK_HRCMD(m_applicationDevice->CreateRenderTargetView(',
            '                        destinationImage, &desc, rtv.ReleaseAndGetAddressOf()));',
            '                }') -join $nl))
    )
    foreach ($edit in $edits) {
        if (([regex]::Matches($layerText, [regex]::Escape($edit[0]))).Count -ne 1) { throw "Quad Views source changed: $(($edit[0] -split "`n")[0])" }
        $layerText = $layerText.Replace($edit[0], $edit[1])
    }
    [IO.File]::WriteAllText($layerSource, $layerText)
}
# Sharpening only what is shown: with the round focus area the composition never reads the focus view outside the
# circle (alpha exactly 0, early out above), so the sharpening pass skips its 16x16 tiles that lie wholly outside
# the circle widened by two texels (the composition's bilinear reach). About a fifth of the focus view. The sharpened
# texture is cleared once when it is created, so a skipped texel holds zero. Off for the rectangle and while the
# focus layer debug view (which shows the whole focus view) is on.
$sharpenSource = Join-Path $quadRoot 'openxr-api-layer/SharpeningCS.hlsl'
$sharpenText = [IO.File]::ReadAllText($sharpenSource)
if (-not $sharpenText.Contains('dcsvrSkip')) {
    $nl = if ($sharpenText.Contains("`r`n")) { "`r`n" } else { "`n" }
    $edits = @(
        @((('    uint4 const0;', '    uint4 const1;', '};') -join $nl),
          (('    uint4 const0;', '    uint4 const1;',
            '    // DCSVR: x the round shape exponent (0: off), y the margin in texels, zw the focus view size.',
            '    float4 dcsvrSkip;', '};') -join $nl)),
        @('    AU2 gxy = ARmp8x8(LocalThreadId.x) + AU2(WorkGroupId.x << 4u, WorkGroupId.y << 4u);',
          (('    AU2 gxy = ARmp8x8(LocalThreadId.x) + AU2(WorkGroupId.x << 4u, WorkGroupId.y << 4u);',
            '    if (dcsvrSkip.x >= 1) {',
            '        // The tile''s texel nearest the centre, in the composition''s normalized coordinates.',
            '        float2 size = dcsvrSkip.zw;',
            '        float2 tileMin = float2(WorkGroupId.xy) * 16.0;',
            '        float2 nearest = clamp(size * 0.5, tileMin, tileMin + 16.0);',
            '        float2 e = abs(nearest / size * 2.0 - 1.0);',
            '        float r = pow(pow(e.x, dcsvrSkip.x) + pow(e.y, dcsvrSkip.x), 1.0 / dcsvrSkip.x);',
            '        if (r >= 1.0 + 2.0 * dcsvrSkip.y / min(size.x, size.y)) {',
            '            return;',
            '        }',
            '    }') -join $nl))
    )
    foreach ($edit in $edits) {
        if (([regex]::Matches($sharpenText, [regex]::Escape($edit[0]))).Count -ne 1) { throw "Quad Views sharpening shader changed: $(($edit[0] -split "`n")[0])" }
        $sharpenText = $sharpenText.Replace($edit[0], $edit[1])
    }
    [IO.File]::WriteAllText($sharpenSource, $sharpenText)
}
$layerText = [IO.File]::ReadAllText($layerSource)
if (-not $layerText.Contains('DcsvrSkip')) {
    $nl = if ($layerText.Contains("`r`n")) { "`r`n" } else { "`n" }
    $edits = @(
        @((('        alignas(4) uint32_t Const1[4];', '    };') -join $nl),
          (('        alignas(4) uint32_t Const1[4];', '        alignas(4) float DcsvrSkip[4];', '    };') -join $nl)),
        @((('                    CHECK_HRCMD(m_applicationDevice->CreateUnorderedAccessView(',
            '                        swapchainForFocusView.sharpenedImage[viewIndex].Get(), &desc, uav.ReleaseAndGetAddressOf()));',
            '                }') -join $nl),
          (('                    CHECK_HRCMD(m_applicationDevice->CreateUnorderedAccessView(',
            '                        swapchainForFocusView.sharpenedImage[viewIndex].Get(), &desc, uav.ReleaseAndGetAddressOf()));',
            '                    // A new sharpened texture: zero where the sharpening may skip.',
            '                    const float zero[4] = {0.f, 0.f, 0.f, 0.f};',
            '                    m_renderContext->ClearUnorderedAccessViewFloat(uav.Get(), zero);',
            '                }') -join $nl)),
        @((('                         (AF1)focusView.subImage.imageRect.extent.width,',
            '                         (AF1)focusView.subImage.imageRect.extent.height);',
            '                {') -join $nl),
          (('                         (AF1)focusView.subImage.imageRect.extent.width,',
            '                         (AF1)focusView.subImage.imageRect.extent.height);',
            '                sharpening.DcsvrSkip[0] = m_debugFocusView ? 0.f : m_focusViewShape;',
            '                sharpening.DcsvrSkip[1] = 2.f;',
            '                sharpening.DcsvrSkip[2] = (float)focusView.subImage.imageRect.extent.width;',
            '                sharpening.DcsvrSkip[3] = (float)focusView.subImage.imageRect.extent.height;',
            '                {') -join $nl))
    )
    foreach ($edit in $edits) {
        if (([regex]::Matches($layerText, [regex]::Escape($edit[0]))).Count -ne 1) { throw "Quad Views source changed: $(($edit[0] -split "`n")[0])" }
        $layerText = $layerText.Replace($edit[0], $edit[1])
    }
    [IO.File]::WriteAllText($layerSource, $layerText)
}
# A gaze sample that is not finite (seen once on a Pimax Crystal Super as DCS's session became ready, the eye tracker
# still starting) gave the focus view a NaN projection; DecomposeProjectionMatrix then threw "Invalid projection
# matrix" and xrLocateViews failed, which DCS treats as fatal (black window). Such a frame now uses the fixed focus
# area, exactly as when the gaze is reported invalid.
$layerText = [IO.File]::ReadAllText($layerSource)
if (-not $layerText.Contains('DCSVR invalid gaze')) {
    $nl = if ($layerText.Contains("`r`n")) { "`r`n" } else { "`n" }
    $edits = @(
        @((('                                        if (!isGazeValid ||',
            '                                            !ProjectPoint(viewForGazeProjection, gazeUnitVector, projectedGaze)) {') -join $nl),
          (('                                        // DCSVR invalid gaze: a gaze that is not finite counts as no gaze.',
            '                                        if (!isGazeValid ||',
            '                                            !ProjectPoint(viewForGazeProjection, gazeUnitVector, projectedGaze) ||',
            '                                            !std::isfinite(projectedGaze.x) || !std::isfinite(projectedGaze.y)) {') -join $nl)),
        @((('                                            views[i].fov =',
            '                                                xr::math::ComputeBoundingFov(m_cachedEyeFov[stereoViewIndex], min, max);') -join $nl),
          (('                                            try {',
            '                                                views[i].fov =',
            '                                                    xr::math::ComputeBoundingFov(m_cachedEyeFov[stereoViewIndex], min, max);',
            '                                            } catch (const std::exception&) {',
            '                                                // DCSVR invalid gaze: a degenerate focus area falls back to the fixed one.',
            '                                                views[i].fov = m_cachedEyeFov[i];',
            '                                            }') -join $nl))
    )
    foreach ($edit in $edits) {
        if (([regex]::Matches($layerText, [regex]::Escape($edit[0]))).Count -ne 1) { throw "Quad Views source changed: $(($edit[0] -split "`n")[0])" }
        $layerText = $layerText.Replace($edit[0], $edit[1])
    }
    [IO.File]::WriteAllText($layerSource, $layerText)
}
& (Join-Path $PSScriptRoot 'msvc.cmd') msbuild "$quadRoot/openxr-api-layer/openxr-api-layer.vcxproj" /t:Build /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /p:SolutionName=XR_APILAYER_MBUCCHIA_quad_views_foveated "/p:SolutionDir=$solutionDir" /m:4 /verbosity:minimal
if ($LASTEXITCODE -ne 0) { throw 'Quad Views build failed.' }
# The composition with our performance edits against the same shaders without them, on this machine's GPU (WARP
# without one): every pixel identical for the round and rectangular focus area, with and without sharpening.
$compositionTest = Join-Path $workspaceRoot 'artifacts/native/qv-test/qv_composition_test.exe'
New-Item -ItemType Directory -Path (Split-Path -Parent $compositionTest) -Force | Out-Null
& (Join-Path $PSScriptRoot 'msvc.cmd') cl /nologo /std:c++20 /EHsc /O2 "/Fe:$compositionTest" "/Fo:$(Split-Path -Parent $compositionTest)\" (Join-Path $workspaceRoot 'tests/quadviews-composition/qv_composition_test.cpp') d3d11.lib d3dcompiler.lib
if ($LASTEXITCODE -ne 0) { throw 'Quad Views composition test build failed.' }
& $compositionTest (Join-Path $quadRoot 'openxr-api-layer') (Join-Path $quadRoot 'external/FidelityFX-CAS/ffx-cas')
if ($LASTEXITCODE -ne 0) { throw 'Quad Views composition test failed.' }
