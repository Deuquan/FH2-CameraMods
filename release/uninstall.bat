@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"
set "TARGET="
if exist "%~dp0azimuth.exe" set "TARGET=%~dp0"
if not defined TARGET (
  set /p "TARGET=Paste the folder that contains azimuth.exe: "
  set "TARGET=!TARGET:"=!"
)
for %%I in ("!TARGET!\.") do set "TARGET=%%~fI"
if not exist "!TARGET!\azimuth.exe" ( echo ERROR: azimuth.exe not found. & pause & exit /b 1 )
tasklist /fi "imagename eq azimuth.exe" 2>nul | find /i "azimuth.exe" >nul && ( echo Close the game first. & pause & exit /b 1 )
if exist "!TARGET!\winmm.dll.orig" (
  copy /y "!TARGET!\winmm.dll.orig" "!TARGET!\winmm.dll" >nul && del "!TARGET!\winmm.dll.orig" && echo Original winmm.dll restored.
) else (
  del "!TARGET!\winmm.dll" && echo winmm.dll removed ^(no backup existed^).
)
del "!TARGET!\CamMods_version.txt" "!TARGET!\CamMods_azimuth.sha" 2>nul
echo Your FH2_ExtraOptions.ini was left untouched.
pause
