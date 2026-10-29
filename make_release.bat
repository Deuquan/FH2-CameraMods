@echo off
setlocal
cd /d "%~dp0"
rem Run build.bat first. Builds dist\FH2-CameraMods-v1.0.4.zip + .sha256
set "NAME=FH2-CameraMods-v1.0.4"
if not exist "out\winmm.dll" ( echo Run build.bat first. & pause & exit /b 1 )
rmdir /s /q "dist" 2>nul & mkdir "dist\%NAME%"
copy out\winmm.dll "dist\%NAME%\" >nul
copy release\install.bat "dist\%NAME%\" >nul
copy release\uninstall.bat "dist\%NAME%\" >nul
copy release\update.bat "dist\%NAME%\" >nul
copy release\VERSION.txt "dist\%NAME%\" >nul
copy release\check_ini.ps1 "dist\%NAME%\" >nul
copy release\pick_folder.ps1 "dist\%NAME%\" >nul
xcopy /e /i /y release\mods "dist\%NAME%\mods" >nul
copy release\README.md "dist\%NAME%\README.md" >nul
copy LICENSE "dist\%NAME%\" >nul
copy release\FH2_ExtraOptions.ini "dist\%NAME%\" >nul
powershell -NoProfile -Command "Compress-Archive -Path 'dist\%NAME%' -DestinationPath 'dist\%NAME%.zip' -Force; $h=(Get-FileHash 'dist\%NAME%.zip' -Algorithm SHA256).Hash.ToLower(); Set-Content -Encoding ascii 'dist\%NAME%.zip.sha256' ($h+'  %NAME%.zip'); Write-Host $h"
echo Done: dist\%NAME%.zip
pause
