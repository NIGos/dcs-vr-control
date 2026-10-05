# Builds the CPU Boost prefetch fix (native/prefetch_fix) and runs its tests.
$ErrorActionPreference = 'Stop'
$workspaceRoot = Split-Path -Parent $PSScriptRoot
$wrapper = Join-Path $PSScriptRoot 'msvc.cmd'
$build = Join-Path $workspaceRoot 'artifacts/native/prefetch-fix'
& $wrapper cmake -S (Join-Path $workspaceRoot 'native/prefetch_fix') -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { throw 'Prefetch fix configuration failed.' }
& $wrapper cmake --build $build --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Prefetch fix build failed.' }
& $wrapper ctest --test-dir $build --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Prefetch fix tests failed.' }
