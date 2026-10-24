@echo off
setlocal
cd /d "%~dp0"
set "ZIG=D:\FH2 MODS\zig-x86_64-windows-0.18.0-dev.349+d924f83bf\zig-x86_64-windows-0.18.0-dev.349+d924f83bf\zig.exe"
set "LOG=%~dp0build_log.txt"
echo Build gestartet: %DATE% %TIME% > "%LOG%"
if not exist "%ZIG%" (
  echo FEHLER: zig.exe nicht gefunden unter %ZIG% >> "%LOG%"
  echo FEHLER: zig.exe nicht gefunden. Siehe build_log.txt
  pause
  exit /b 1
)
echo Zig-Version: >> "%LOG%"
"%ZIG%" version >> "%LOG%" 2>&1
where python >> "%LOG%" 2>&1
python --version >> "%LOG%" 2>&1
echo. >> "%LOG%"
echo === Build (Azimuth-Variante, unveraendert) === >> "%LOG%"
python build.py "%ZIG%" --azimuth --output "%~dp0out\winmm.dll" >> "%LOG%" 2>&1
echo Exitcode: %ERRORLEVEL% >> "%LOG%"
echo.
type "%LOG%"
echo.
echo Fertig. Die Ausgabe steht auch in build_log.txt
pause
