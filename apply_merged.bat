@echo off
setlocal
cd /d "%~dp0"
if not exist merged_source.zip (
  echo merged_source.zip fehlt in diesem Ordner.
  pause
  exit /b 1
)
echo Entpacke neuen Quellcode ^(ExtraOptions 0.3.2 + CamMods^) in diesen Ordner ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Expand-Archive -LiteralPath 'merged_source.zip' -DestinationPath '.' -Force"
if errorlevel 1 (
  echo FEHLER beim Entpacken.
  pause
  exit /b 1
)
echo Fertig. Jetzt build.bat ausfuehren.
pause
