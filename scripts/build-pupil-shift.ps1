# Builds the pupil shift OpenXR layer (native/pupil_shift) and runs its tests.
$ErrorActionPreference = 'Stop'
$workspaceRoot = Split-Path -Parent $PSScriptRoot
$wrapper = Join-Path $PSScriptRoot 'msvc.cmd'
$build = Join-Path $workspaceRoot 'artifacts/native/pupil-shift'
& $wrapper cmake -S (Join-Path $workspaceRoot 'native/pupil_shift') -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw 'Pupil shift configuration failed.' }
& $wrapper cmake --build $build --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Pupil shift build failed.' }
& $wrapper ctest --test-dir $build --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Pupil shift tests failed.' }
