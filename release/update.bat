@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"
rem === Set this to your GitHub repo, e.g. SomeUser/FH2-CameraMods ===
set "REPO=CHANGE_ME/CHANGE_ME"
if "%REPO%"=="CHANGE_ME/CHANGE_ME" ( echo update.bat is not configured yet ^(REPO is not set^). & pause & exit /b 1 )
set "TMP_DIR=%TEMP%\CamModsUpdate"
rmdir /s /q "%TMP_DIR%" 2>nul & mkdir "%TMP_DIR%"
echo Checking latest release of %REPO% ...
powershell -NoProfile -Command "$ErrorActionPreference='Stop'; $r=Invoke-RestMethod 'https://api.github.com/repos/%REPO%/releases/latest'; $z=$r.assets|?{$_.name -like '*.zip'}|select -First 1; $s=$r.assets|?{$_.name -like '*.sha256'}|select -First 1; if(-not $z -or -not $s){throw 'release has no zip/sha256'}; Write-Host ('Latest: '+$r.tag_name); Invoke-WebRequest $z.browser_download_url -OutFile '%TMP_DIR%\mod.zip'; $exp=((Invoke-WebRequest $s.browser_download_url).Content.ToString().Trim() -split '\s+')[0]; $act=(Get-FileHash '%TMP_DIR%\mod.zip' -Algorithm SHA256).Hash; if($exp -ne $act){throw 'SHA-256 mismatch - aborted'}; Expand-Archive '%TMP_DIR%\mod.zip' '%TMP_DIR%\x' -Force" || ( echo Update failed. Nothing was changed. & pause & exit /b 1 )
set "NEWDIR="
for /d %%D in ("%TMP_DIR%\x\*") do set "NEWDIR=%%D"
if not defined NEWDIR set "NEWDIR=%TMP_DIR%\x"
echo.
echo Downloaded and verified. Installing ...
call "!NEWDIR!\install.bat"
