# =============================================================================
# bm.ps1 — BetterMagnifier derle / calistir / log oku
# =============================================================================
# Kullanim:
#   .\bm.ps1              # Debug derle
#   .\bm.ps1 run          # Debug derle + calistir
#   .\bm.ps1 release      # Release derle
#   .\bm.ps1 check        # Debug derle + tanilama modlarini calistir
#   .\bm.ps1 log          # en son log dosyasini goster
#   .\bm.ps1 errors       # en son log'daki WARN/ERROR satirlari
#   .\bm.ps1 kill         # calisan ornekleri kapat
#   .\bm.ps1 clean        # bin + obj sil
#
# Neden script: msbuild yolu uzun ve Developer PowerShell acmak gerekmiyor.
#
# 'check' neden ayri: manifest RequireAdministrator istedigi icin exe'yi normal
# bir kabuktan calistirmak mumkun degil ("requires elevation"). Bu komut
# -Verb RunAs ile bir kez UAC sorar, sonucu log'dan okur.
# =============================================================================

param(
    [ValidateSet('build', 'run', 'release', 'check', 'log', 'errors', 'kill', 'clean')]
    [string]$Action = 'build'
)

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$MSBuild = "C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe"

if (-not (Test-Path $MSBuild)) {
    # VS baska yere kuruluysa vswhere ile bul
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -requires Microsoft.Component.MSBuild -property installationPath
        if ($vsPath) { $MSBuild = Join-Path $vsPath "MSBuild\Current\Bin\MSBuild.exe" }
    }
}

function Invoke-Build([string]$Config) {
    Write-Host "==> $Config x64 derleniyor..." -ForegroundColor Cyan
    # /restore: the control panel pulls the Windows App SDK in via
    # PackageReference, and without a restore the WinRT projection is not there.
    & $MSBuild .\BetterMagnifier.sln /restore /p:Configuration=$Config /p:Platform=x64 /v:minimal /nologo
    if ($LASTEXITCODE -ne 0) {
        Write-Host "==> DERLEME BASARISIZ (exit $LASTEXITCODE)" -ForegroundColor Red
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
        Write-Host "==> Calistiriliyor. Kapatmak icin: tepsi ikonu > Exit" -ForegroundColor Cyan
        Write-Host "    Ctrl+Alt+Z = zoom ac/kapa   Ctrl+Alt+X = freeze   tekerlek = zoom" -ForegroundColor DarkGray
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

        if ($ok) { Write-Host "==> Tanilama temiz" -ForegroundColor Green }
        else     { Write-Host "==> Tanilamada hata var" -ForegroundColor Red; exit 1 }
    }

    'log' {
        $log = Get-LatestLog
        if ($log) { Write-Host "==> $($log.FullName)" -ForegroundColor Cyan; Get-Content $log.FullName }
        else { Write-Host "log yok - once calistir" -ForegroundColor Yellow }
    }

    'errors' {
        $log = Get-LatestLog
        if (-not $log) { Write-Host "log yok - once calistir" -ForegroundColor Yellow; break }
        Write-Host "==> $($log.Name)" -ForegroundColor Cyan
        $hits = Select-String -Path $log.FullName -Pattern '\[WARN |\[ERROR'
        if ($hits) { $hits | ForEach-Object { $_.Line } }
        else { Write-Host "WARN/ERROR yok - temiz" -ForegroundColor Green }
    }

    'kill' {
        $p = Get-Process BetterMagnifier -ErrorAction SilentlyContinue
        if ($p) { $p | Stop-Process -Force; Write-Host "$($p.Count) ornek kapatildi" -ForegroundColor Green }
        else { Write-Host "calisan ornek yok" }
    }

    'clean' {
        foreach ($d in @('.\bin', '.\obj')) {
            if (Test-Path $d) { Remove-Item $d -Recurse -Force; Write-Host "silindi: $d" -ForegroundColor Green }
        }
    }
}
