param([string]$Workspace = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$cacheRoot = Join-Path $Workspace '.cache/packages'
$externalRoot = Join-Path $Workspace 'external'
New-Item -ItemType Directory -Path $cacheRoot,$externalRoot -Force | Out-Null
$packages = @(
    @{Id='ofxr'; Repo='tig3rmast3r/OFXR-Bridge'; Tag='0.2.1'; Revision='dad56acafc6e1dde219940427738b926cf2ea555'; File='OFXR-Bridge-v0.2.1-V116.zip'; Hash='db224491cbc9553a51fe55d08623d25864c4ec0a3d49bd76e9d6c4d9b2410c67'},
    @{Id='cheeky'; Repo='ClarkCheekyKent/CheekyFoveatedDLSS'; Tag='v0.5.4'; Revision='d6c18b23d31f7339735114c621bc3f9066e1a32d'; File='CheekyFoveatedDLSS-0.5.4-Standalone-release.zip'; Hash='d119a8edaacf0924fa748f6b7a2d13486baa39313e965a3eed6e385234f5116c'},
    @{Id='sboys'; Repo='sboys3/CustomHeadsetOpenVR'; Tag='1.3.0'; Revision='21185da63a9e4307ae876b3702a43a43044d451c'; File='CustomHeadset-1.3.0-Pimax-UI.zip'; Hash='7e86492148f74b3e6c799bf758979c763c59cd170f59349e6a52cac3f38e99d3'}
)
foreach ($package in $packages) {
    $archive = Join-Path $cacheRoot $package.File
    if (-not (Test-Path -LiteralPath $archive)) {
        Invoke-WebRequest -Uri "https://github.com/$($package.Repo)/releases/download/$($package.Tag)/$($package.File)" -OutFile $archive
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $package.Hash) {
        throw "Package digest mismatch: $($package.Id)"
    }
    $expanded = Join-Path $cacheRoot $package.Id
    if (-not (Test-Path -LiteralPath $expanded)) { Expand-Archive -LiteralPath $archive -DestinationPath $expanded }
    Write-Output "Verified package $($package.Id) $($package.Tag)"
    if ($package.Id -in @('ofxr','cheeky','sboys')) {
        $source = Join-Path $externalRoot $package.Id
        if (-not (Test-Path -LiteralPath $source)) {
            & git clone --depth 1 --branch $package.Tag "https://github.com/$($package.Repo).git" $source
            if ($LASTEXITCODE -ne 0) { throw "Clone failed: $($package.Id)" }
        }
        $revision = & git -C $source rev-parse HEAD
        if ($LASTEXITCODE -ne 0) { throw 'Cannot identify upstream revision.' }
        if ($revision -ne $package.Revision) { throw "Unexpected source revision: $($package.Id)" }
        Write-Output $revision
    }
}
