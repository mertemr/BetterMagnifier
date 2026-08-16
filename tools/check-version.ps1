# =============================================================================
# check-version.ps1 - assert that every place a version appears agrees
# =============================================================================
# Four places carry the version, and they used to be written by four hands -
# which is the drift 9d6838d had to go and clean up:
#
#   src\Version.h     what the build compiles     (release-please writes it)
#   version.txt       the package version         (release-please writes it)
#   src\app.manifest  the assembly identity       (release-please writes it)
#   the built exe     what Windows and the updater report
#
# A mismatch is not cosmetic. The updater compares the release feed's tag_name
# against BM_VERSION_STRING, so if the binary's idea of its own version drifts
# from the released one, a user sees either a permanent "update available" or
# an update that never arrives - and neither failure says what it is.
#
# Usage:
#   .\tools\check-version.ps1                  # checks the Debug build
#   .\tools\check-version.ps1 -Configuration Release
# =============================================================================
param([string]$Configuration = 'Debug')

$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')

if (-not (Test-Path 'src\Version.h')) {
    Write-Host "FAIL: src\Version.h not found" -ForegroundColor Red
    exit 1
}

$header = Get-Content src\Version.h -Raw
$major  = [regex]::Match($header, '#define\s+BM_VERSION_MAJOR\s+(\d+)').Groups[1].Value
$minor  = [regex]::Match($header, '#define\s+BM_VERSION_MINOR\s+(\d+)').Groups[1].Value
$patch  = [regex]::Match($header, '#define\s+BM_VERSION_PATCH\s+(\d+)').Groups[1].Value

if (-not ($major -and $minor -and $patch)) {
    Write-Host "FAIL: could not read the version macros from src\Version.h" -ForegroundColor Red
    exit 1
}

$fromHeader = "$major.$minor.$patch"

# The string literal is written out separately from the triple - see the note in
# Version.h about rc.exe and the # operator - so the two can disagree, and this
# is the only thing that would notice.
$fromMacro = [regex]::Match($header, '#define\s+BM_VERSION_STRING\s+"([^"]+)"').Groups[1].Value

if ($fromMacro -ne $fromHeader) {
    Write-Host "FAIL: BM_VERSION_STRING is '$fromMacro' but the triple is '$fromHeader'" -ForegroundColor Red
    exit 1
}

if (-not (Test-Path 'version.txt')) {
    Write-Host "FAIL: version.txt not found" -ForegroundColor Red
    exit 1
}

$fromText = (Get-Content version.txt -Raw).Trim()

if ($fromHeader -ne $fromText) {
    Write-Host "FAIL: src\Version.h says '$fromHeader', version.txt says '$fromText'" -ForegroundColor Red
    Write-Host "      release-please writes both - if they differ, its config is wrong" -ForegroundColor Yellow
    exit 1
}

# assemblyIdentity requires four components, so this one is "<triple>.0".
# release-please rewrites the leading three and leaves the trailing .0 - if it
# ever stops doing that, this is what says so.
$manifest     = Get-Content src\app.manifest -Raw
$fromManifest = [regex]::Match($manifest, 'version="(\d+\.\d+\.\d+)\.\d+"').Groups[1].Value

if ($fromManifest -ne $fromHeader) {
    Write-Host "FAIL: src\app.manifest says '$fromManifest', src\Version.h says '$fromHeader'" -ForegroundColor Red
    exit 1
}

$exe = ".\bin\$Configuration-x64\BetterMagnifier.exe"

if (-not (Test-Path $exe)) {
    Write-Host "FAIL: $exe not found - build first" -ForegroundColor Red
    exit 1
}

$expected = "$fromHeader.0"
$actual   = (Get-Item $exe).VersionInfo.FileVersion.Trim()

if ($actual -ne $expected) {
    Write-Host "FAIL: exe reports '$actual', src\Version.h says '$expected'" -ForegroundColor Red
    Write-Host "      a stale build? run .\bm.ps1 $($Configuration.ToLower()) first" -ForegroundColor Yellow
    exit 1
}

Write-Host "OK: $fromHeader in src\Version.h, version.txt, app.manifest and the exe" -ForegroundColor Green
