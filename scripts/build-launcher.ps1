# Builds the DCS Control launcher (native/launcher): the DcsControl.exe at the top of the release folder, which starts
# files\DcsControl.exe. Output: artifacts/native/launcher/DcsControl.exe.
$ErrorActionPreference = 'Stop'
$workspaceRoot = Split-Path -Parent $PSScriptRoot
$wrapper = Join-Path $PSScriptRoot 'msvc.cmd'
$source = Join-Path $workspaceRoot 'native/launcher'
$build = Join-Path $workspaceRoot 'artifacts/native/launcher'
New-Item -ItemType Directory -Path $build -Force | Out-Null
& $wrapper rc /nologo /fo (Join-Path $build 'launcher.res') (Join-Path $source 'launcher.rc')
if ($LASTEXITCODE -ne 0) { throw 'Launcher resources failed.' }
& $wrapper cl /nologo /std:c++20 /EHsc /O2 /W4 /WX /MT /DUNICODE /D_UNICODE "/Fo:$build\" "/Fe:$(Join-Path $build 'DcsControl.exe')" (Join-Path $source 'launcher.cpp') (Join-Path $build 'launcher.res') user32.lib /link /SUBSYSTEM:WINDOWS
if ($LASTEXITCODE -ne 0) { throw 'Launcher build failed.' }
Join-Path $build 'DcsControl.exe'
