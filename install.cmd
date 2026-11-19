@echo off
rem Installs Beat the Beat: Rhythm Paradise (the Qilin recompilation) from your
rem own extracted disc: double-click this file. tools\install\install.ps1 has
rem the details and the options (they can be given here too).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\install\install.ps1" %*
echo.
pause
