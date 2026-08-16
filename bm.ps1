# =============================================================================
# bm.ps1 — build / run / read logs for BetterMagnifier
# =============================================================================
# Usage:
#   .\bm.ps1              # build Debug
#   .\bm.ps1 run          # build Debug, then launch
#   .\bm.ps1 release      # build Release
#   .\bm.ps1 check        # build Debug + run the diagnostic modes
#   .\bm.ps1 check-update # ask the build what the latest release is
#   .\bm.ps1 installer    # build Release + produce the NSIS setup
#   .\bm.ps1 log          # show the newest log file
#   .\bm.ps1 errors       # WARN/ERROR lines from the newest log
#   .\bm.ps1 kill         # kill running instances
#   .\bm.ps1 clean        # delete bin + obj
#
# Why a script: the MSBuild path is long, and this avoids having to open a
# Developer PowerShell.
#
# Why 'check' is separate: the manifest requires administrator rights, so the
# exe cannot be launched from an ordinary shell ("requires elevation"). This
# command asks for UAC once, via -Verb RunAs, and reads the outcome back out
# of the log.
# =============================================================================

param(
    [ValidateSet('build', 'run', 'release', 'check', 'check-update', 'installer', 'log', 'errors', 'kill', 'clean')]
    [string]$Action = 'build'
)

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$MSBuild = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"

if (-not (Test-Path $MSBuild)) {
    # Visual Studio installed somewhere else: find it via vswhere.
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -requires Microsoft.Component.MSBuild -property installationPath
        if ($vsPath) { $MSBuild = Join-Path $vsPath "MSBuild\Current\Bin\MSBuild.exe" }
    }
}

function Invoke-Build([string]$Config) {
    Write-Host "==> Building $Config x64..." -ForegroundColor Cyan
    # /restore: the control panel pulls the Windows App SDK in via
    # PackageReference, and without a restore the WinRT projection is not there.
    & $MSBuild .\BetterMagnifier.sln /restore /p:Configuration=$Config /p:Platform=x64 /v:minimal /nologo
    if ($LASTEXITCODE -ne 0) {
        Write-Host "==> BUILD FAILED (exit $LASTEXITCODE)" -ForegroundColor Red
        exit $LASTEXITCODE
    }
    Write-Host "==> OK: bin\$Config-x64\BetterMagnifier.exe" -ForegroundColor Green
}

function Get-LatestLog {
    $dirs = @(".\bin\Debug-x64\logs", ".\bin\Release-x64\logs") | Where-Object { Test-Path $_ }
    if (-not $dirs) { return $null }
    Get-ChildItem $dirs -Filter *.log | Sort-Object LastWriteTime | Select-Object -Last 1
}

switch ($Action) {

    'build'   { Invoke-Build 'Debug' }

    'release' { Invoke-Build 'Release' }

    'run' {
        Invoke-Build 'Debug'
        Get-Process BetterMagnifier -ErrorAction SilentlyContinue | Stop-Process -Force
        Write-Host "==> Launching. To quit: tray icon > Exit" -ForegroundColor Cyan
        Write-Host "    Ctrl+Alt+Z = toggle zoom   Ctrl+Alt+X = freeze   wheel = zoom" -ForegroundColor DarkGray
        Start-Process ".\bin\Debug-x64\BetterMagnifier.exe"
    }

    'check' {
        Invoke-Build 'Debug'

        $exe = ".\bin\Debug-x64\BetterMagnifier.exe"
        $ok  = $true

        # --self-check and --dump-osd are both exempt from the single-instance
        # mutex, so this works while the app is running. They are NOT exempt
        # from elevation: that is a property of the manifest, not the mode.
        foreach ($mode in '--self-check', '--dump-osd', '--dump-cursors') {
            Write-Host "==> $mode" -ForegroundColor Cyan
            $p = Start-Process $exe -ArgumentList $mode -Verb RunAs -Wait -PassThru
            if ($p.ExitCode -ne 0) {
                Write-Host "    FAILED (exit $($p.ExitCode))" -ForegroundColor Red
                $ok = $false
            }
        }

        $log = Get-LatestLog
        if ($log) {
            Select-String -Path $log.FullName -Pattern 'passed|failure\(s\)|FAILED|\[ERROR' |
                ForEach-Object { "    " + $_.Line.Trim() }
        }

        if ($ok) { Write-Host "==> Diagnostics clean" -ForegroundColor Green }
        else     { Write-Host "==> Diagnostics reported a failure" -ForegroundColor Red; exit 1 }
    }

    'check-update' {
        Invoke-Build 'Debug'

        # Elevation is a property of the binary, so this needs RunAs like the
        # other diagnostic modes. Unlike them, --check-update is compiled into
        # Release too: the binary you actually want to ask about its update
        # state is the one that ships.
        $p = Start-Process ".\bin\Debug-x64\BetterMagnifier.exe" `
                 -ArgumentList '--check-update' -Verb RunAs -Wait -PassThru

        switch ($p.ExitCode) {
            0       { Write-Host "==> Up to date" -ForegroundColor Green }
            2       { Write-Host "==> An update is available" -ForegroundColor Yellow }
            default { Write-Host "==> Check failed (exit $($p.ExitCode))" -ForegroundColor Red }
        }

        $log = Get-LatestLog
        if ($log) {
            Select-String -Path $log.FullName -Pattern 'Update check|Update:' |
                Select-Object -Last 8 |
                ForEach-Object { "    " + $_.Line.Trim() }
        }
    }

    'installer' {
        Invoke-Build 'Release'

        $makensis = "${env:ProgramFiles(x86)}\NSIS\makensis.exe"
        if (-not (Test-Path $makensis)) {
            $cmd = Get-Command makensis -ErrorAction SilentlyContinue
            if ($cmd) { $makensis = $cmd.Source }
        }

        if (-not (Test-Path $makensis)) {
            Write-Host "==> NSIS not found. Install it with:" -ForegroundColor Red
            Write-Host "    winget install NSIS.NSIS" -ForegroundColor Yellow
            exit 1
        }

        # The version comes from src\Version.h, the same place the binary's
        # version resource comes from. There is deliberately no second place to
        # type it: release-please writes that file and nothing else.
        $header = Get-Content src\Version.h -Raw
        $major  = [regex]::Match($header, '#define\s+BM_VERSION_MAJOR\s+(\d+)').Groups[1].Value
        $minor  = [regex]::Match($header, '#define\s+BM_VERSION_MINOR\s+(\d+)').Groups[1].Value
        $patch  = [regex]::Match($header, '#define\s+BM_VERSION_PATCH\s+(\d+)').Groups[1].Value
        $version = "$major.$minor.$patch"

        & .\tools\check-version.ps1 -Configuration Release
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

        New-Item -ItemType Directory -Force installer\out | Out-Null

        Write-Host "==> Building the installer for $version..." -ForegroundColor Cyan
        & $makensis "/DVERSION=$version" installer\BetterMagnifier.nsi
        if ($LASTEXITCODE -ne 0) {
            Write-Host "==> makensis FAILED (exit $LASTEXITCODE)" -ForegroundColor Red
            exit $LASTEXITCODE
        }

        Write-Host "==> OK: installer\out\BetterMagnifier-$version-x64-setup.exe" -ForegroundColor Green
        Write-Host "    No Windows App Runtime bundled - CI passes /DRUNTIME_EXE for that." -ForegroundColor DarkGray
    }

    'log' {
        $log = Get-LatestLog
        if ($log) { Write-Host "==> $($log.FullName)" -ForegroundColor Cyan; Get-Content $log.FullName }
        else { Write-Host "No log yet - build and run first" -ForegroundColor Yellow }
    }

    'errors' {
        $log = Get-LatestLog
        if (-not $log) { Write-Host "No log yet - build and run first" -ForegroundColor Yellow; break }
        Write-Host "==> $($log.Name)" -ForegroundColor Cyan
        $hits = Select-String -Path $log.FullName -Pattern '\[WARN |\[ERROR'
        if ($hits) { $hits | ForEach-Object { $_.Line } }
        else { Write-Host "No WARN/ERROR - clean" -ForegroundColor Green }
    }

    'kill' {
        $p = Get-Process BetterMagnifier -ErrorAction SilentlyContinue
        if ($p) { $p | Stop-Process -Force; Write-Host "Stopped $($p.Count) instance(s)" -ForegroundColor Green }
        else { Write-Host "No running instance" }
    }

    'clean' {
        foreach ($d in @('.\bin', '.\obj')) {
            if (Test-Path $d) { Remove-Item $d -Recurse -Force; Write-Host "Deleted: $d" -ForegroundColor Green }
        }
    }
}
