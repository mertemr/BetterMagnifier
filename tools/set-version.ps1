# =============================================================================
# set-version.ps1 - set the version in src\Version.h and version.txt
# =============================================================================
# Usage:
#   .\tools\set-version.ps1 0.2.0
#
# NOT the normal path. release-please owns the version: it derives the next one
# from the Conventional Commits since the last tag and rewrites both files in a
# Release PR. This script is for bootstrapping the scheme and for a rare manual
# override - if you find yourself reaching for it routinely, the commit messages
# are not saying what you mean.
#
# Does not tag and does not commit. Merging release-please's Release PR is what
# creates a tag, and a tag created any other way would be a second writer for
# something that only works with one.
# =============================================================================
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string]$Version
)

$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')

$major, $minor, $patch = $Version -split '\.'

$path    = 'src\Version.h'
$content = Get-Content $path -Raw

# Anchored on the macro names rather than on the marker comments: the comments
# are what release-please matches, and two tools rewriting the same line by
# different rules is exactly how they drift apart.
$content = [regex]::Replace($content, '(#define\s+BM_VERSION_MAJOR\s+)\d+', "`${1}$major")
$content = [regex]::Replace($content, '(#define\s+BM_VERSION_MINOR\s+)\d+', "`${1}$minor")
$content = [regex]::Replace($content, '(#define\s+BM_VERSION_PATCH\s+)\d+', "`${1}$patch")
$content = [regex]::Replace($content, '(#define\s+BM_VERSION_STRING\s+)"\d+\.\d+\.\d+"', "`${1}""$Version""")

Set-Content $path $content -NoNewline -Encoding UTF8
Set-Content 'version.txt' "$Version`n" -NoNewline -Encoding UTF8

# assemblyIdentity requires four components; only the leading three move.
$manifestPath = 'src\app.manifest'
$manifest     = Get-Content $manifestPath -Raw
$manifest     = [regex]::Replace($manifest, 'version="\d+\.\d+\.\d+\.(\d+)"', "version=""$Version.`${1}""")
Set-Content $manifestPath $manifest -NoNewline -Encoding UTF8

Write-Host "src\Version.h, version.txt and src\app.manifest -> $Version" -ForegroundColor Green
Write-Host "Normally release-please does this; committing it by hand is a manual override." -ForegroundColor DarkGray
