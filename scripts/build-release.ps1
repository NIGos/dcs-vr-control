param([switch]$NoRestore)
$ErrorActionPreference = 'Stop'
$workspaceRoot = Split-Path -Parent $PSScriptRoot
$env:DOTNET_CLI_TELEMETRY_OPTOUT='1'
$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1'
$env:DOTNET_GENERATE_ASPNET_CERTIFICATE='false'
$env:NUGET_PACKAGES=Join-Path $workspaceRoot '.cache/nuget'
$dotnet = Join-Path $workspaceRoot '.tools/dotnet/dotnet.exe'
$version = & (Join-Path $PSScriptRoot 'version.ps1')
$releaseRoot = Join-Path $workspaceRoot "artifacts/release/DcsVrControl-$version-win-x64"
# The user's real DCS settings are hashed before the pipeline; verify-web-release.py checks that nothing in
# building, testing or verifying the release changed them. A profile the user applied earlier is fine.
$actualDcs = @{}; foreach ($name in 'options.lua','autoexec.cfg') { $file = Join-Path $env:USERPROFILE "Saved Games/DCS/Config/$name"; if (Test-Path -LiteralPath $file) { $actualDcs[$file] = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash } }
$actualDcs | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $workspaceRoot 'artifacts/fps-actual-before.json') -Encoding utf8
if (Test-Path -LiteralPath $releaseRoot) {
    $resolvedRelease = (Resolve-Path -LiteralPath $releaseRoot).Path
    $allowedRelease = [IO.Path]::GetFullPath((Join-Path $workspaceRoot "artifacts/release/DcsVrControl-$version-win-x64"))
    if ($resolvedRelease -ne $allowedRelease -or -not $resolvedRelease.StartsWith([IO.Path]::GetFullPath($workspaceRoot) + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Refusing to clean an unexpected release destination.' }
    Remove-Item -LiteralPath $resolvedRelease -Recurse -Force
}
New-Item -ItemType Directory -Path $releaseRoot -Force | Out-Null
foreach ($project in @('src/DcsVr.App','src/DcsVr.Cli')) {
    $publishOptions = @('-c','Release','-r','win-x64','--self-contained','true','-o',$releaseRoot,'-p:DebugType=None','-p:DebugSymbols=false','--nologo')
    if ($NoRestore) { $publishOptions += '--no-restore' }
    & $dotnet publish (Join-Path $workspaceRoot $project) @publishOptions
    if ($LASTEXITCODE -ne 0) { throw 'Publish failed.' }
}
foreach ($dir in @('packages','components/quadviews','components/cheeky-focus/CheekyFoveatedDLSS','licenses','scripts','profiles','docs')) { New-Item -ItemType Directory -Path (Join-Path $releaseRoot $dir) -Force | Out-Null }
foreach ($filename in @('dxgi.dll','CheekyFoveatedDLSS/CheekyFoveatedDLSSHost.dll','CheekyFoveatedDLSS/CheekyFoveatedDLSSRuntime.dll')) {
    $target = Join-Path $releaseRoot "components/cheeky-focus/$filename"
    $source = if ($filename -eq 'dxgi.dll') { Join-Path $workspaceRoot ".cache/packages/cheeky/$filename" } else { Join-Path $workspaceRoot "artifacts/native/cheeky/bin/Release/$filename" }
    Copy-Item -LiteralPath $source -Destination $target -Force
    (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() | Set-Content -LiteralPath ($target + '.sha256') -Encoding ascii
}
# Cheeky stereo routes ship the three upstream binaries they use, taken from the pinned archive. The archive itself
# and its version.dll fallback loader (flagged by Microsoft Defender) are not distributed.
$cheekyArchive = Join-Path $workspaceRoot '.cache/packages/CheekyFoveatedDLSS-0.5.4-Standalone-release.zip'
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $cheekyArchive).Hash -ne 'D119A8EDAACF0924FA748F6B7A2D13486BAA39313E965A3EED6E385234F5116C') { throw 'Cheeky archive digest mismatch.' }
$cheekyZip = [IO.Compression.ZipFile]::OpenRead($cheekyArchive)
try {
    foreach ($entryName in @('dxgi.dll','CheekyFoveatedDLSS/CheekyFoveatedDLSSHost.dll','CheekyFoveatedDLSS/CheekyFoveatedDLSSRuntime.dll')) {
        $entry = $cheekyZip.Entries | Where-Object { $_.FullName.Replace('\','/') -eq $entryName } | Select-Object -First 1
        if (-not $entry) { throw "Cheeky archive is missing $entryName." }
        $target = Join-Path $releaseRoot "components/cheeky/$entryName"
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $target, $true)
        (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() | Set-Content -LiteralPath ($target + '.sha256') -Encoding ascii
    }
} finally { $cheekyZip.Dispose() }
foreach ($archive in @('OFXR-Bridge-v0.2.1-V116.zip')) {
    Copy-Item -LiteralPath (Join-Path $workspaceRoot ".cache/packages/$archive") -Destination (Join-Path $releaseRoot 'packages') -Force
}
# OFXR layer built from the djules75 fork 0.2.9.1 (scripts/build-ofxr-djules75.ps1): the DCS-compatible build, used for every framegen profile.
New-Item -ItemType Directory -Path (Join-Path $releaseRoot 'components/ofxr') -Force | Out-Null
$deferredOfxr = Join-Path $releaseRoot 'components/ofxr/XR_APILAYER_XRFrameBridge_diagnostic.dll'
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'artifacts/native/ofxr-djules75/XR_APILAYER_XRFrameBridge_diagnostic.dll') -Destination $deferredOfxr -Force
(Get-FileHash -LiteralPath $deferredOfxr -Algorithm SHA256).Hash.ToLowerInvariant() | Set-Content -LiteralPath ($deferredOfxr + '.sha256') -Encoding ascii
# CPU Boost prefetch fix (scripts/build-prefetch-fix.ps1), loaded by DCS as bin\dxgi2.dll when the profile enables it.
# Rebuilt and tested on every release so a stale DLL can never ship.
& (Join-Path $PSScriptRoot 'build-prefetch-fix.ps1')
New-Item -ItemType Directory -Path (Join-Path $releaseRoot 'components/boost') -Force | Out-Null
$prefetchFix = Join-Path $releaseRoot 'components/boost/prefetch_fix.dll'
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'artifacts/native/prefetch-fix/prefetch_fix.dll') -Destination $prefetchFix -Force
(Get-FileHash -LiteralPath $prefetchFix -Algorithm SHA256).Hash.ToLowerInvariant() | Set-Content -LiteralPath ($prefetchFix + '.sha256') -Encoding ascii
$cheekyDll = Join-Path $releaseRoot 'components/CheekyOpenXRLayer.dll'
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'artifacts/native/cheeky/bin/Release/CheekyOpenXRLayer.dll') -Destination $cheekyDll -Force
(Get-FileHash -LiteralPath $cheekyDll -Algorithm SHA256).Hash.ToLowerInvariant() | Set-Content -LiteralPath ($cheekyDll + '.sha256') -Encoding ascii
foreach ($filename in @('XR_APILAYER_MBUCCHIA_quad_views_foveated.dll','openxr-api-layer.json','settings.cfg')) {
    $target = Join-Path $releaseRoot "components/quadviews/$filename"
    Copy-Item -LiteralPath (Join-Path $workspaceRoot "external/quadviews/bin/x64/Release/$filename") -Destination $target -Force
    (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() | Set-Content -LiteralPath ($target + '.sha256') -Encoding ascii
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'installer.ps1') -Destination (Join-Path $releaseRoot 'scripts/installer.ps1') -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'README.md') -Destination (Join-Path $releaseRoot 'README.md') -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'PLAN_DCS_VR.md') -Destination $releaseRoot -Force
# The policy's version and corresponding-source name always match this build (GPL source offer).
$policy = Get-Content -LiteralPath (Join-Path $workspaceRoot 'distribution-policy.json') -Raw | ConvertFrom-Json
$policy.version = $version; $policy.correspondingSource = "DcsVrControl-$version-sources.zip"
$policy | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $releaseRoot 'distribution-policy.json') -Encoding utf8
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'docs/SETUP.md') -Destination (Join-Path $releaseRoot 'docs') -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'docs/FRAME_PACING.md') -Destination (Join-Path $releaseRoot 'docs') -Force
foreach ($doc in @('docs/INTERFACE.md','docs/VALIDATION.md','docs/FIRST_FLIGHT.md','docs/THIRD_PARTY.md','docs/RELEASE_NOTES.md','docs/APPROACHES.md','docs/QUAD_FOCUS_ADAPTER.md','docs/NEURAL_TUNING.md','docs/PERFORMANCE.md','docs/performance-results.json','docs/APP_ICON.md','docs/USER_GUIDE.md','docs/app-icon-preview.png')) { Copy-Item -LiteralPath (Join-Path $workspaceRoot $doc) -Destination (Join-Path $releaseRoot 'docs') -Force }
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'docs/screenshots') -Destination (Join-Path $releaseRoot 'docs') -Recurse -Force
$renderStart = [Diagnostics.ProcessStartInfo]::new((Join-Path $releaseRoot 'DcsVrControl.exe'))
$renderStart.UseShellExecute = $false; $renderStart.CreateNoWindow = $true
$renderRoot = Join-Path $workspaceRoot ('artifacts/web-ui/' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $renderRoot -Force | Out-Null
# The shipped interface previews are rendered from a neutral demo layout, so no path shows the build machine's user.
$demoRoot = Join-Path ([IO.Path]::GetPathRoot($workspaceRoot)) 'DcsVrDemo'
if (Test-Path -LiteralPath $demoRoot) { Remove-Item -LiteralPath $demoRoot -Recurse -Force }
New-Item -ItemType Directory -Path $demoRoot -Force | Out-Null
$renderStart.Environment['DCSVR_SMOKE_DEMO_ROOT'] = $demoRoot
$renderStart.ArgumentList.Add('--web-smoke'); $renderStart.ArgumentList.Add((Join-Path $renderRoot 'interface'))
$renderProcess = [Diagnostics.Process]::Start($renderStart)
if (-not $renderProcess.WaitForExit(120000)) { $renderProcess.Kill(); throw 'Release interface render timed out.' }
if ($renderProcess.ExitCode -ne 0) { throw 'Release interface render failed.' }
$renderProcess.Dispose()
Remove-Item -LiteralPath $demoRoot -Recurse -Force -ErrorAction SilentlyContinue
Get-ChildItem -LiteralPath $renderRoot -File | Where-Object { $_.Name -like 'interface-*.png' -or $_.Name -eq 'interface-checks.json' -or $_.Name -eq 'interface-renders.json' } | Copy-Item -Destination (Join-Path $releaseRoot 'docs') -Force
foreach ($index in 0..42) { if (-not (Test-Path -LiteralPath (Join-Path $releaseRoot "docs/interface-$index.png"))) { throw 'Release interface preview is incomplete.' } }
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'LICENSE') -Destination (Join-Path $releaseRoot 'licenses/Manager-MIT.txt') -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot '.cache/nuget/microsoft.web.webview2/1.0.4258.31/LICENSE.txt') -Destination (Join-Path $releaseRoot 'licenses/WebView2-SDK.txt') -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot '.cache/nuget/microsoft.web.webview2/1.0.4258.31/NOTICE.txt') -Destination (Join-Path $releaseRoot 'licenses/WebView2-NOTICE.txt') -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'external/cheeky/LICENSE') -Destination (Join-Path $releaseRoot 'licenses/Cheeky-GPL-3.0.txt') -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot '.cache/packages/ofxr/LICENSE') -Destination (Join-Path $releaseRoot 'licenses/OFXR-LGPL-3.0.txt') -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'external/ofxr-djules75/LICENSE') -Destination (Join-Path $releaseRoot 'licenses/OFXR-djules75-LGPL-3.0.txt') -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'external/ofxr-djules75/THIRD_PARTY.md') -Destination (Join-Path $releaseRoot 'licenses/OFXR-djules75-THIRD_PARTY.md') -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'external/quadviews/LICENSE') -Destination (Join-Path $releaseRoot 'licenses/QuadViews-MIT.txt') -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'external/quadviews/THIRD_PARTY') -Destination (Join-Path $releaseRoot 'licenses/QuadViews-THIRD-PARTY.txt') -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'external/quadviews/external/OpenXR-MixedReality/NOTICE') -Destination (Join-Path $releaseRoot 'licenses/OpenXR-MixedReality-NOTICE.txt') -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'external/quadviews/packages/Microsoft.Windows.ImplementationLibrary.1.0.220201.1/LICENSE') -Destination (Join-Path $releaseRoot 'licenses/WIL-MIT.txt') -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'external/quadviews/packages/Microsoft.Windows.ImplementationLibrary.1.0.220201.1/ThirdPartyNotices.txt') -Destination (Join-Path $releaseRoot 'licenses/WIL-ThirdPartyNotices.txt') -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot '.cache/packages/cheeky/licenses') -Destination (Join-Path $releaseRoot 'licenses/Cheeky-dependencies') -Recurse -Force
Copy-Item -LiteralPath (Join-Path $workspaceRoot 'external/sboys/LICENCE.md') -Destination (Join-Path $releaseRoot 'licenses/Sboys-GPL-2.0.md') -Force
& $dotnet (Join-Path $releaseRoot 'DcsVr.Cli.dll') presets | Set-Content -LiteralPath (Join-Path $releaseRoot 'profiles/presets.json') -Encoding utf8
@'
@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\installer.ps1" -Action Install -InstallPrerequisites
pause
'@ | Set-Content -LiteralPath (Join-Path $releaseRoot 'Install.cmd') -Encoding ascii
@'
@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\installer.ps1" -Action Uninstall
pause
'@ | Set-Content -LiteralPath (Join-Path $releaseRoot 'Uninstall.cmd') -Encoding ascii
$files = Get-ChildItem -LiteralPath $releaseRoot -Recurse -File | Where-Object { $_.Name -ne 'release-manifest.json' } | ForEach-Object {
    @{ path=[IO.Path]::GetRelativePath($releaseRoot,$_.FullName); sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(); bytes=$_.Length }
}
@{ schemaVersion=1; product='DcsVrControl'; version=$version; files=@($files) } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $releaseRoot 'release-manifest.json') -Encoding utf8
& (Join-Path $releaseRoot 'scripts/installer.ps1') -Action Verify -Destination (Join-Path $workspaceRoot 'artifacts/installer-verify') -NoShortcut
if ($LASTEXITCODE -ne 0) { throw 'Release integrity check failed.' }
$releaseZip = $releaseRoot + '.zip'
if (Test-Path -LiteralPath $releaseZip) { Remove-Item -LiteralPath $releaseZip }
[IO.Compression.ZipFile]::CreateFromDirectory($releaseRoot, $releaseZip, [IO.Compression.CompressionLevel]::Optimal, $true)
(Get-FileHash -LiteralPath $releaseZip -Algorithm SHA256).Hash.ToLowerInvariant() | Set-Content -LiteralPath ($releaseZip + '.sha256') -Encoding ascii
Write-Output $releaseZip
