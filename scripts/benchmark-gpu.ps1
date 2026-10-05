param([Parameter(Mandatory=$true)][string]$NeuralRuntime, [ValidateRange(1,5)][int]$Repeats=2, [switch]$QualityCasesOnly)
$ErrorActionPreference='Stop'
$workspaceRoot=Split-Path -Parent $PSScriptRoot
# This checks signatures/ABI and the complete offscreen stage regression first.
& (Join-Path $PSScriptRoot 'test-combined-gpu.ps1') -NeuralRuntime $NeuralRuntime
$buildRoot=Join-Path $workspaceRoot 'artifacts/native/combined-gpu'
$validation=Get-Content -LiteralPath (Join-Path $buildRoot 'result.json') -Raw | ConvertFrom-Json
if (!$validation.passed) { throw 'Real runtime validation did not pass.' }
$reportRoot=Join-Path $workspaceRoot ('artifacts/performance/'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $reportRoot -Force | Out-Null
& nvidia-smi --query-gpu=name,driver_version,pstate,clocks.current.graphics,power.draw,temperature.gpu,utilization.gpu --format=csv | Set-Content -LiteralPath (Join-Path $reportRoot 'gpu-before.csv')
$cases=@(
    @(1024,2048,.50,50,'fast'),@(1024,2048,.65,50,'fast'),@(1024,2048,.75,50,'fast'),@(1024,2048,1.0,50,'fast'),
    @(1024,2048,.75,50,'medium'),@(1024,2048,.65,50,'medium'),@(1024,2048,.65,50,'slow'),@(1024,2048,.65,75,'fast'),@(1024,2048,.65,100,'fast'),
    @(1536,3072,.50,50,'fast'),@(1536,3072,.65,50,'fast'),@(1536,3072,.75,50,'fast'),@(1536,3072,.75,50,'medium'),
    @(2048,4096,.50,50,'fast'),@(2048,4096,.65,50,'fast'),@(2048,4096,.75,50,'fast'),@(2048,4096,.75,50,'medium')
)
$qualityCases=@(
    @(1024,2048,.75,50,'medium',1),@(1024,2048,.75,100,'slow',1),@(2048,4096,1.0,100,'slow',0),
    @(3072,6144,.75,50,'medium',0),@(3072,6144,.75,50,'fast',0),@(3072,6144,.65,50,'fast',0),@(3072,6144,.50,50,'fast',0)
)
$cases=if($QualityCasesOnly){$qualityCases}else{$cases+$qualityCases}
$results=[Collections.Generic.List[object]]::new()
for($repeat=0;$repeat -lt $Repeats;$repeat++) {
    # Alternate order so startup temperature and clocks do not favor one preset.
    $order=@(0..($cases.Count-1)); if($repeat % 2){[array]::Reverse($order)}
    foreach($index in $order) {
        $case=$cases[$index]; $name="case-$index-run-$repeat"; $json=Join-Path $reportRoot ($name+'.json')
        $start=[Diagnostics.ProcessStartInfo]::new((Join-Path $buildRoot 'neural_gpu.exe'))
        $start.UseShellExecute=$false; $start.CreateNoWindow=$true; $start.RedirectStandardOutput=$true; $start.RedirectStandardError=$true
        $flowArgument=[string]$case[3]+','+$case[4]+','+$(if($case.Count -gt 5){[string]$case[5]}else{'0'})
        foreach($argument in @($validation.ngxCore,$workspaceRoot,'--benchmark',$json,[string]$case[0],[string]$case[1],$case[2].ToString([Globalization.CultureInfo]::InvariantCulture),$flowArgument)) { $start.ArgumentList.Add($argument) }
        $process=[Diagnostics.Process]::Start($start); $out=$process.StandardOutput.ReadToEndAsync(); $err=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(60000)){$process.Kill();throw "Benchmark $name timed out."}
        $output=$out.GetAwaiter().GetResult(); $errors=$err.GetAwaiter().GetResult()
        $output | Set-Content -LiteralPath (Join-Path $reportRoot ($name+'.log')); $errors | Set-Content -LiteralPath (Join-Path $reportRoot ($name+'-errors.log'))
        if($process.ExitCode -ne 0){throw "Benchmark $name failed: $errors"}; $process.Dispose()
        $result=Get-Content -LiteralPath $json -Raw | ConvertFrom-Json
        if(!$result.passed -or $result.neuralFailures -ne 0 -or $result.steadyStateRecreations -ne 0){throw "Invalid benchmark $name"}
        $result | Add-Member run $repeat; $result | Add-Member caseIndex $index; $results.Add($result)
        Write-Output ("{0}: focus {1}, stereo {2}, NR {3:P0}, flow {4}% {5}, bidirectional {6}, GPU stages {7:N2} ms" -f $name,$case[0],$case[1],$case[2],$case[3],$case[4],$result.bidirectional,($result.gpuStageSum.medianUs/1000))
    }
}
& nvidia-smi --query-gpu=name,driver_version,pstate,clocks.current.graphics,power.draw,temperature.gpu,utilization.gpu --format=csv | Set-Content -LiteralPath (Join-Path $reportRoot 'gpu-after.csv')
$maxVariation=0.0
foreach($group in ($results | Group-Object caseIndex)) {
    $range=$group.Group.gpuStageSum.medianUs | Measure-Object -Minimum -Maximum
    if($range.Minimum -gt 0){$maxVariation=[math]::Max($maxVariation,100*($range.Maximum/$range.Minimum-1))}
}
@{passed=$true;timingComparable=($maxVariation -le 15);maxRunVariationPercent=$maxVariation;scope='Offscreen GPU timestamps. Includes NR, composition shaders and optical flow; excludes DCS rendering, SR, DX11 transport, sharpening, headset presentation and frame pacing. Timing comparison is flagged when repeated medians differ by more than 15%.';neuralSha256=$validation.neuralSha256;timestampUtc=[DateTimeOffset]::UtcNow;results=@($results.ToArray())} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $reportRoot 'results.json')
if($maxVariation -gt 15){Write-Warning 'Correctness passed, but repeated timings vary substantially. Do not use this run to select performance defaults.'}
Write-Output "PASS benchmark: $reportRoot"
