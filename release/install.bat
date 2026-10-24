@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"
title Camera Mods installer
set "SRC=%~dp0"
set "TARGET="
if exist "%SRC%azimuth.exe" set "TARGET=%SRC%"
if not defined TARGET if exist "%SRC%..\azimuth.exe" set "TARGET=%SRC%..\"
if not defined TARGET (
  echo Azimuth folder not found next to this installer.
  set /p "TARGET=Paste the folder that contains azimuth.exe: "
  set "TARGET=!TARGET:"=!"
)
if not exist "!TARGET!\azimuth.exe" if not exist "!TARGET!azimuth.exe" (
  echo ERROR: azimuth.exe not found in "!TARGET!". Nothing was changed.
  pause & exit /b 1
)
for %%I in ("!TARGET!\.") do set "TARGET=%%~fI"
if not exist "%SRC%winmm.dll" ( echo ERROR: winmm.dll missing next to install.bat. & pause & exit /b 1 )
tasklist /fi "imagename eq azimuth.exe" 2>nul | find /i "azimuth.exe" >nul && (
  echo Please close the game first. & pause & exit /b 1 )
echo Installing into: !TARGET!
if exist "!TARGET!\winmm.dll" if not exist "!TARGET!\winmm.dll.orig" (
  copy /y "!TARGET!\winmm.dll" "!TARGET!\winmm.dll.orig" >nul
  echo Backup saved: winmm.dll.orig
)
copy /y "%SRC%winmm.dll" "!TARGET!\winmm.dll" >nul || ( echo ERROR: copy failed. & pause & exit /b 1 )
if not exist "!TARGET!\FH2_ExtraOptions.ini" (
  copy "%SRC%FH2_ExtraOptions.ini" "!TARGET!\FH2_ExtraOptions.ini" >nul
  echo Created FH2_ExtraOptions.ini
) else (
  findstr /i /c:"[Autodrive]" "!TARGET!\FH2_ExtraOptions.ini" >nul || (
    echo.>>"!TARGET!\FH2_ExtraOptions.ini"
    echo [Autodrive]>>"!TARGET!\FH2_ExtraOptions.ini"
    echo Enabled=1>>"!TARGET!\FH2_ExtraOptions.ini"
    echo Added [Autodrive] Enabled=1 to your INI
  )
)
copy /y "%SRC%VERSION.txt" "!TARGET!\CamMods_version.txt" >nul 2>nul
certutil -hashfile "!TARGET!\azimuth.exe" SHA256 2>nul | findstr /r /v "hash CertUtil" > "!TARGET!\CamMods_azimuth.sha" 
echo.
echo Done. Start the game via Azimuth.
pause
