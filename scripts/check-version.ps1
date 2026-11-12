<#
.SYNOPSIS
    Verify that src/dd-win-prof/version.h matches a given vX.Y.Z tag.

.DESCRIPTION
    Parses DLL_VERSION_MAJOR / MINOR / PATCH from src/dd-win-prof/version.h
    and compares against the supplied tag (with the leading 'v' stripped).
    Exits non-zero on mismatch.

    Used by .github/workflows/release.yml to fail-fast when a tag is pushed
    without bumping version.h. Also runnable locally before pushing a tag.

.PARAMETER Tag
    The tag to check against, e.g. "v0.3.0". Required.

.EXAMPLE
    # Check the version.h matches what you're about to tag
    .\scripts\check-version.ps1 -Tag v0.3.0
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Tag
)

$ErrorActionPreference = 'Stop'

if ($Tag -notmatch '^v\d+\.\d+\.\d+$') {
    Write-Error "Tag '$Tag' does not match vX.Y.Z"
    exit 1
}
$expected = $Tag.TrimStart('v')

# version.h lives next to this script's repo, at src/dd-win-prof/version.h.
$repoRoot   = Split-Path -Parent $PSScriptRoot
$versionH   = Join-Path $repoRoot 'src/dd-win-prof/version.h'
if (-not (Test-Path $versionH)) {
    Write-Error "version.h not found at $versionH"
    exit 1
}

$content = Get-Content $versionH -Raw
$major = [regex]::Match($content, 'DLL_VERSION_MAJOR\s+(\d+)').Groups[1].Value
$minor = [regex]::Match($content, 'DLL_VERSION_MINOR\s+(\d+)').Groups[1].Value
$patch = [regex]::Match($content, 'DLL_VERSION_PATCH\s+(\d+)').Groups[1].Value
$actual = "$major.$minor.$patch"

Write-Host "Tag version:      $expected"
Write-Host "version.h value:  $actual"
if ($actual -ne $expected) {
    Write-Error "version.h ($actual) does not match tag ($expected). Bump src/dd-win-prof/version.h before tagging."
    exit 1
}
Write-Host "OK: version.h matches tag $Tag" -ForegroundColor Green
