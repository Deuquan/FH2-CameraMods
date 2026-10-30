@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"
set "TARGET="
if exist "%~dp0azimuth.exe" set "TARGET=%~dp0"
if not defined TARGET (
  echo Azimuth folder not found next to this uninstaller. A window will open - please select it.
  for /f "usebackq delims=" %%F in (`powershell -NoProfile -STA -ExecutionPolicy Bypass -File "%~dp0pick_folder.ps1"`) do set "TARGET=%%F"
  if not defined TARGET (
    echo No folder selected.
    set /p "TARGET=Or paste the folder that contains azimuth.exe: "
    set "TARGET=!TARGET:"=!"
  )
)
for %%I in ("!TARGET!\.") do set "TARGET=%%~fI"
if not exist "!TARGET!\azimuth.exe" ( echo ERROR: azimuth.exe not found. & pause & exit /b 1 )
tasklist /fi "imagename eq azimuth.exe" 2>nul | find /i "azimuth.exe" >nul && ( echo Close the game first. & pause & exit /b 1 )
if exist "!TARGET!\mods\Camera Mods" (
  rmdir /s /q "!TARGET!\mods\Camera Mods" && echo Camera Mods plugin removed.
) else (
  echo Camera Mods plugin was not installed.
)
del "!TARGET!\CamMods_version.txt" 2>nul
echo Extra Options and your FH2_ExtraOptions.ini were left untouched.
pause
