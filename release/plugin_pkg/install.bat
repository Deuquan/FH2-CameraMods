@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"
title Camera Mods installer
set "SRC=%~dp0"
set "TARGET="
if exist "%SRC%azimuth.exe" set "TARGET=%SRC%"
if not defined TARGET if exist "%SRC%..\azimuth.exe" set "TARGET=%SRC%..\"
if not defined TARGET (
  echo Azimuth folder not found next to this installer. A window will open - please select it.
  for /f "usebackq delims=" %%F in (`powershell -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0pick_folder.ps1"`) do set "TARGET=%%F"
  if not defined TARGET (
    echo No folder selected.
    set /p "TARGET=Or paste the folder that contains azimuth.exe: "
    set "TARGET=!TARGET:"=!"
  )
)
if not exist "!TARGET!\azimuth.exe" if not exist "!TARGET!azimuth.exe" (
  echo ERROR: azimuth.exe not found in "!TARGET!". Nothing was changed.
  pause & exit /b 1
)
for %%I in ("!TARGET!\.") do set "TARGET=%%~fI"
if not exist "%SRC%mods\Camera Mods\plugins\CameraMods.dll" ( echo ERROR: CameraMods.dll missing next to install.bat. & pause & exit /b 1 )
tasklist /fi "imagename eq azimuth.exe" 2>nul | find /i "azimuth.exe" >nul && (
  echo Please close the game first. & pause & exit /b 1 )

rem Camera Mods is a plugin for the official Extra Options: winmm.dll must be Extra Options.
if not exist "!TARGET!\winmm.dll" (
  echo ERROR: Extra Options ^(winmm.dll^) is not installed in "!TARGET!".
  echo Install Extra Options 0.3.x first: https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions/releases
  pause & exit /b 1
)
if exist "!TARGET!\CamMods_version.txt" (
  findstr /b /c:"CamMods v1.0" "!TARGET!\CamMods_version.txt" >nul && (
    echo.
    echo An OLD Camera Mods version ^(v1.0.x^) is installed. It replaced winmm.dll with a modified
    echo Extra Options. Please first put the official Extra Options winmm.dll back
    echo ^(copy it from the Extra Options download into "!TARGET!"^), then run this installer again.
    echo The old backup of your original file is "winmm.dll.orig" ^(it is NOT Extra Options^).
    pause & exit /b 1
  )
)

echo Installing into: !TARGET!
xcopy /e /i /y "%SRC%mods" "!TARGET!\mods" >nul || ( echo ERROR: copy failed. & pause & exit /b 1 )
copy /y "%SRC%VERSION.txt" "!TARGET!\CamMods_version.txt" >nul 2>nul
echo.
echo Done. Camera Mods was installed as an Extra Options plugin ^(mods\Camera Mods^).
echo Start the game via Azimuth. FH2_ExtraOptions.log should show a line "CameraMods ...: hooks installed".
pause
