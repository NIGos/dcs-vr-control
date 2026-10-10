param(
    [ValidateSet('Install','Uninstall','Verify')][string]$Action = 'Install',
    [string]$Destination = (Join-Path $env:LOCALAPPDATA 'Programs/DcsControl'),
    [switch]$NoShortcut,
    [switch]$InstallPrerequisites
)
$ErrorActionPreference = 'Stop'
# This script is files\scripts\installer.ps1; the release (with release-manifest.json) is the folder above files\.
$filesRoot = Split-Path -Parent $PSScriptRoot
$payloadRoot = Split-Path -Parent $filesRoot
$command = switch ($Action) { 'Install' {'app-install'} 'Uninstall' {'app-uninstall'} 'Verify' {'app-verify'} }
$installRoot = [IO.Path]::GetFullPath($Destination).TrimEnd('\','/')
function Get-WebViewVersion {
    foreach ($key in @('HKLM:\SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}','HKCU:\Software\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}')) {
        $value = Get-ItemPropertyValue -LiteralPath $key -Name pv -ErrorAction SilentlyContinue
        $parsedVersion = $null
        if ([Version]::TryParse($value,[ref]$parsedVersion) -and $parsedVersion -gt [Version]'0.0.0.0') { return $value }
    }
    return $null
}
function Install-MicrosoftPrerequisite([string]$Name,[string]$Url,[string]$Arguments) {
    $downloadRoot = Join-Path $installRoot '.prerequisite-downloads'
    New-Item -ItemType Directory -Path $downloadRoot -Force | Out-Null
    $installer = Join-Path $downloadRoot ($Name + '.exe')
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Write-Output "Downloading missing prerequisite from Microsoft: $Name"
    Invoke-WebRequest -Uri $Url -OutFile $installer -UseBasicParsing -TimeoutSec 120
    $signature = Get-AuthenticodeSignature -LiteralPath $installer
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation,') { throw "Prerequisite signature rejected: $Name. Nothing was executed." }
    $process = Start-Process -FilePath $installer -ArgumentList $Arguments -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -notin @(0,3010,1641)) { throw "Microsoft prerequisite installer failed: $Name ($($process.ExitCode))." }
    Remove-Item -LiteralPath $installer
    if ($process.ExitCode -in @(3010,1641)) { Write-Output 'Windows restart requested. Restart before the headset test.' }
}
$webViewVersion = Get-WebViewVersion
$cppLibraries = @('vcruntime140.dll','vcruntime140_1.dll','msvcp140.dll')
$missingCpp = @($cppLibraries | Where-Object { -not (Test-Path -LiteralPath (Join-Path $env:SystemRoot "System32/$_")) })
$vcFile = Join-Path $env:SystemRoot 'System32/vcruntime140.dll'
$vcVersion = if (Test-Path -LiteralPath $vcFile) { [Version](Get-Item -LiteralPath $vcFile).VersionInfo.FileVersion } else { [Version]'0.0' }
$needsCpp = $missingCpp.Count -gt 0 -or $vcVersion -lt [Version]'14.50'
if ($Action -eq 'Install') {
    & (Join-Path $filesRoot 'DcsVr.Cli.exe') app-verify --source $payloadRoot
    if ($LASTEXITCODE -ne 0) { throw 'Package integrity failed. Prerequisite installation was not attempted.' }
}
if ($Action -eq 'Install' -and $InstallPrerequisites) {
    if (-not $webViewVersion) { Install-MicrosoftPrerequisite 'WebView2' 'https://go.microsoft.com/fwlink/p/?LinkId=2124703' '/silent /install' }
    if ($needsCpp) { Install-MicrosoftPrerequisite 'VisualCpp-x64' 'https://aka.ms/vc14/vc_redist.x64.exe' '/install /passive /norestart' }
}
Write-Output $(if (Get-WebViewVersion) { 'WebView2 Runtime detected.' } else { 'WebView2 Runtime missing. Install the Evergreen Runtime from https://developer.microsoft.com/microsoft-edge/webview2/ before opening the GUI.' })
if ($needsCpp -and -not $InstallPrerequisites) { Write-Output 'Visual C++ x64 runtime 14.50 or newer is required. Run Install DCS Control.cmd to obtain official prerequisites.' }
& (Join-Path $filesRoot 'DcsVr.Cli.exe') $command --source $payloadRoot --destination $installRoot
if ($LASTEXITCODE -ne 0) { throw 'Application deployment failed. Review the error above; recovery backups are retained.' }
if ($NoShortcut -or $Action -eq 'Verify') { exit 0 }
$shortcutPath = Join-Path ([Environment]::GetFolderPath('Programs')) 'DCS Control.lnk'
$shell = New-Object -ComObject WScript.Shell
if ($Action -eq 'Install') {
    $shortcut = $shell.CreateShortcut($shortcutPath)
    $shortcut.TargetPath = Join-Path $installRoot 'DcsControl.exe'
    $shortcut.IconLocation = (Join-Path $installRoot 'DcsControl.exe') + ',0'
    $shortcut.WorkingDirectory = $installRoot
    $shortcut.Save()
    Write-Output "Installed DCS Control in $installRoot. No game or VR driver settings changed."
    # DCS Control was called DCS VR Control before 0.5.0: its default install and Start menu entry are replaced by this
    # one. Only that app's own files go (its uninstaller checks each one); your DCS backups and profiles are kept.
    $earlierRoot = Join-Path $env:LOCALAPPDATA 'Programs/DcsVrControl'
    if ($installRoot -ne [IO.Path]::GetFullPath($earlierRoot).TrimEnd('\','/') -and (Test-Path -LiteralPath (Join-Path $earlierRoot 'installation.json'))) {
        & (Join-Path $filesRoot 'DcsVr.Cli.exe') app-uninstall --destination $earlierRoot
        if ($LASTEXITCODE -eq 0) { Write-Output "Removed the earlier DCS VR Control installation from $earlierRoot." } else { Write-Output "The earlier DCS VR Control installation in $earlierRoot was left in place; remove it from there if you no longer need it." }
    }
    $earlierShortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'DCS VR Control.lnk'
    if (Test-Path -LiteralPath $earlierShortcut) { Remove-Item -LiteralPath $earlierShortcut }
} elseif (Test-Path -LiteralPath $shortcutPath) {
    if ($shell.CreateShortcut($shortcutPath).TargetPath -eq (Join-Path $installRoot 'DcsControl.exe')) { Remove-Item -LiteralPath $shortcutPath }
    Write-Output 'Owned application files removed. Game-mod backup state is retained.'
}
