# Returns the product version (for example 0.4.1-preview) from Directory.Build.props, the single source of truth.
$props = [xml](Get-Content -LiteralPath (Join-Path (Split-Path -Parent $PSScriptRoot) 'Directory.Build.props') -Raw)
$group = $props.Project.PropertyGroup
if (-not $group.VersionPrefix) { throw 'Directory.Build.props has no VersionPrefix.' }
if ($group.VersionSuffix) { "$($group.VersionPrefix)-$($group.VersionSuffix)" } else { "$($group.VersionPrefix)" }
