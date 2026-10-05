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
& (Join-Path $PSScriptRoot 'msvc.cmd') msbuild "$quadRoot/openxr-api-layer/openxr-api-layer.vcxproj" /t:Build /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145 /p:SolutionName=XR_APILAYER_MBUCCHIA_quad_views_foveated "/p:SolutionDir=$solutionDir" /m:4 /verbosity:minimal
if ($LASTEXITCODE -ne 0) { throw 'Quad Views build failed.' }
