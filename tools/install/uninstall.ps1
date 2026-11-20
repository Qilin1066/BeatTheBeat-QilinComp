# Removes Beat the Beat: Rhythm Paradise (the Qilin recompilation) as
# install.ps1 installed it: the shortcuts, the entry in Windows' "Installed
# apps" and the game's folder. Saves and settings stay unless -RemoveData is
# given. The installer copies this file beside the game; it removes the
# folder it is in.
[CmdletBinding()]
param(
    # Also removes the saves and settings (%LOCALAPPDATA%\qilin\beatthebeat).
    [switch]$RemoveData
)

$ErrorActionPreference = 'Stop'
$UninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\BeatTheBeat-QilinRecomp'
$InstallDir = $PSScriptRoot
$exe = Join-Path $InstallDir 'qilin_run.exe'

# Only a folder the installer made: it holds the game and the installer's own notes.
if (-not (Test-Path $exe) -or -not (Test-Path (Join-Path $InstallDir 'install-info.txt'))) {
    Write-Host "This folder ($InstallDir) isn't an installed Beat the Beat: nothing was removed." -ForegroundColor Red
    exit 1
}
if (Get-Process qilin_run -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }) {
    Write-Host 'The game is running: close it first.' -ForegroundColor Red
    exit 1
}

$list = Join-Path $InstallDir 'shortcuts.txt'
if (Test-Path $list) {
    foreach ($link in (Get-Content $list)) {
        # Only shortcuts, and only the ones the installer recorded.
        if ($link -and $link.EndsWith('.lnk') -and (Test-Path $link)) { Remove-Item $link -Force; Write-Host "Removed $link" }
    }
}
if (Test-Path $UninstallKey) { Remove-Item $UninstallKey -Recurse -Force }

if ($RemoveData) {
    $data = Join-Path $env:LOCALAPPDATA 'qilin\beatthebeat'
    if (Test-Path $data) { Remove-Item $data -Recurse -Force; Write-Host "Removed the saves and settings ($data)" }
} else {
    Write-Host 'Saves and settings are kept (%LOCALAPPDATA%\qilin\beatthebeat); -RemoveData removes them too.'
}

# The folder this script is in: remove what the installer put there, then
# the folder itself from outside it (a script can't delete its own folder
# while it runs in it).
foreach ($name in 'qilin_run.exe', 'Play.cmd', 'install-info.txt', 'shortcuts.txt', 'Uninstall.cmd') {
    Remove-Item (Join-Path $InstallDir $name) -Force -ErrorAction SilentlyContinue
}
Set-Location $env:TEMP
$self = Join-Path $InstallDir 'uninstall.ps1'
Start-Process powershell -WindowStyle Hidden -ArgumentList @('-NoProfile', '-Command',
    "Start-Sleep -Seconds 2; Remove-Item -LiteralPath '$($self -replace "'", "''")' -Force; if (-not (Get-ChildItem -LiteralPath '$($InstallDir -replace "'", "''")' -Force)) { Remove-Item -LiteralPath '$($InstallDir -replace "'", "''")' -Force }")
Write-Host 'Beat the Beat: Rhythm Paradise is removed.' -ForegroundColor Green
