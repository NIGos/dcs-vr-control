param([string]$Workspace = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$sdkVersion = '10.0.401'
$sdkHash = '24b670ad3d923bfcf47df6c3b034152398b42f6dbc388e10d783aee1cfb5e5817d399fc0ae2a12cfa822a55e61d34830ccb15c50ef6efee437ab874bb7c79430'
$sdkRoot = Join-Path $Workspace '.tools/dotnet'
$sdkExe = Join-Path $sdkRoot 'dotnet.exe'
if (Test-Path -LiteralPath (Join-Path $sdkRoot "sdk/$sdkVersion")) { Write-Output $sdkExe; exit 0 }
$cacheRoot = Join-Path $Workspace '.cache'
New-Item -ItemType Directory -Path $cacheRoot,$sdkRoot -Force | Out-Null
$sdkArchive = Join-Path $cacheRoot "dotnet-sdk-$sdkVersion-win-x64.zip"
if (-not (Test-Path -LiteralPath $sdkArchive)) {
    Invoke-WebRequest -Uri "https://builds.dotnet.microsoft.com/dotnet/Sdk/$sdkVersion/dotnet-sdk-$sdkVersion-win-x64.zip" -OutFile $sdkArchive
}
if ((Get-FileHash -LiteralPath $sdkArchive -Algorithm SHA512).Hash.ToLowerInvariant() -ne $sdkHash) {
    throw 'The SDK archive does not match the official Microsoft SHA512 digest.'
}
Expand-Archive -LiteralPath $sdkArchive -DestinationPath $sdkRoot -Force
Write-Output $sdkExe
