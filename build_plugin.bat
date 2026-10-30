@echo off
setlocal
cd /d "%~dp0"
set "ZIG=D:\FH2 MODS\zig-x86_64-windows-0.18.0-dev.349+d924f83bf\zig-x86_64-windows-0.18.0-dev.349+d924f83bf\zig.exe"
set "LOG=%~dp0build_plugin_log.txt"
echo Plugin build gestartet: %DATE% %TIME% > "%LOG%"
if not exist "%ZIG%" (
  echo FEHLER: zig.exe nicht gefunden unter %ZIG% >> "%LOG%"
  type "%LOG%"
  pause
  exit /b 1
)
if not exist out mkdir out
python build_plugin.py "%ZIG%" --output "%~dp0out\CameraMods.dll" >> "%LOG%" 2>&1
echo Exitcode: %ERRORLEVEL% >> "%LOG%"
type "%LOG%"
echo.
echo Fertig. Die Ausgabe steht auch in build_plugin_log.txt
pause
