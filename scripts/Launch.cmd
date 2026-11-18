@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
setlocal DisableDelayedExpansion
set "game=%~1"
if not defined game if exist "%~dp0game-path.txt" set /p "game="<"%~dp0game-path.txt"
if not defined game goto missing_game
if not exist "%game%\sys\main.dol" goto missing_game

set "profile=%~dp0user"
if not exist "%profile%\Cache\Shaders" mkdir "%profile%\Cache\Shaders"
if not exist "%profile%\Config" mkdir "%profile%\Config"
if not exist "%profile%\config.ini" (
  > "%profile%\config.ini" echo [Video]
  >> "%profile%\config.ini" echo resolution=640x528
  >> "%profile%\config.ini" echo backend=Vulkan
  >> "%profile%\config.ini" echo fullscreen=false
)
if not exist "%profile%\Config\WiimoteNew.ini" (
  > "%profile%\Config\WiimoteNew.ini" echo [Wiimote1]
  >> "%profile%\Config\WiimoteNew.ini" echo Source = 1
  >> "%profile%\Config\WiimoteNew.ini" echo Device = DInput/0/Keyboard Mouse
  >> "%profile%\Config\WiimoteNew.ini" echo Buttons/A = X
  >> "%profile%\Config\WiimoteNew.ini" echo Buttons/B = Z
  >> "%profile%\Config\WiimoteNew.ini" echo Buttons/Plus = RETURN
  >> "%profile%\Config\WiimoteNew.ini" echo D-Pad/Up = UP
  >> "%profile%\Config\WiimoteNew.ini" echo D-Pad/Down = DOWN
  >> "%profile%\Config\WiimoteNew.ini" echo D-Pad/Left = LEFT
  >> "%profile%\Config\WiimoteNew.ini" echo D-Pad/Right = RIGHT
)

"%~dp0moderngekko-run.exe" --game "%game%" --module "%~dp0gSOMP01_recomp.dll" --user-dir "%profile%" --graphics Vulkan --no-mods
set "status=%errorlevel%"
if not "%status%"=="0" pause
exit /b %status%

:missing_game
echo Pass the path to the extracted SOMP01 partition containing sys and files.
pause
exit /b 2
