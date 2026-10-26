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
if not exist "%SRC%winmm.dll" ( echo ERROR: winmm.dll missing next to install.bat. & pause & exit /b 1 )
tasklist /fi "imagename eq azimuth.exe" 2>nul | find /i "azimuth.exe" >nul && (
  echo Please close the game first. & pause & exit /b 1 )
echo Installing into: !TARGET!
if exist "!TARGET!\winmm.dll" if not exist "!TARGET!\winmm.dll.orig" (
  copy /y "!TARGET!\winmm.dll" "!TARGET!\winmm.dll.orig" >nul
  echo Backup saved: winmm.dll.orig
)
copy /y "%SRC%winmm.dll" "!TARGET!\winmm.dll" >nul || ( echo ERROR: copy failed. & pause & exit /b 1 )
if exist "!TARGET!\FH2_ExtraOptions.ini" (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%SRC%check_ini.ps1" "!TARGET!\FH2_ExtraOptions.ini"
  if errorlevel 1 (
    copy /y "!TARGET!\FH2_ExtraOptions.ini" "!TARGET!\FH2_ExtraOptions.ini.old" >nul
    copy /y "%SRC%FH2_ExtraOptions.ini" "!TARGET!\FH2_ExtraOptions.ini" >nul
    echo Your old FH2_ExtraOptions.ini contains options this mod does not accept.
    echo It was saved as FH2_ExtraOptions.ini.old and the default INI was installed.
  ) else (
    findstr /i /c:"[Autodrive]" "!TARGET!\FH2_ExtraOptions.ini" >nul || (
      >>"!TARGET!\FH2_ExtraOptions.ini" echo.
      >>"!TARGET!\FH2_ExtraOptions.ini" echo [Autodrive]
      >>"!TARGET!\FH2_ExtraOptions.ini" echo Enabled=1
      echo Added [Autodrive] Enabled=1 to your INI
    )
  )
) else (
  copy "%SRC%FH2_ExtraOptions.ini" "!TARGET!\FH2_ExtraOptions.ini" >nul
  echo Created FH2_ExtraOptions.ini
)
copy /y "%SRC%VERSION.txt" "!TARGET!\CamMods_version.txt" >nul 2>nul
certutil -hashfile "!TARGET!\azimuth.exe" SHA256 2>nul | findstr /r /v "hash CertUtil" > "!TARGET!\CamMods_azimuth.sha" 
echo.
echo Done. Start the game via Azimuth.
pause
