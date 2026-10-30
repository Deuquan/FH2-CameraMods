@echo off
setlocal
cd /d "%~dp0"
rem Run build_plugin.bat first (creates out\CameraMods.dll). Builds dist\FH2-CameraMods-v1.1.0.zip + .sha256
set "NAME=FH2-CameraMods-v1.1.0"
if not exist "out\CameraMods.dll" ( echo Run build_plugin.bat first. & pause & exit /b 1 )
rmdir /s /q "dist" 2>nul & mkdir "dist\%NAME%"
xcopy /e /i /y release\plugin_pkg "dist\%NAME%" >nul
mkdir "dist\%NAME%\mods\Camera Mods\plugins" 2>nul
copy out\CameraMods.dll "dist\%NAME%\mods\Camera Mods\plugins\" >nul
copy release\README_plugin.md "dist\%NAME%\README.md" >nul
copy LICENSE "dist\%NAME%\" >nul
powershell -NoProfile -Command "Compress-Archive -Path 'dist\%NAME%' -DestinationPath 'dist\%NAME%.zip' -Force; $h=(Get-FileHash 'dist\%NAME%.zip' -Algorithm SHA256).Hash.ToLower(); Set-Content -Encoding ascii 'dist\%NAME%.zip.sha256' ($h+'  %NAME%.zip'); Write-Host $h"
echo Done: dist\%NAME%.zip
pause
