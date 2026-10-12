@echo off
if exist "%~dp0mysql.env" (
  for /f "usebackq eol=# delims=" %%a in ("%~dp0mysql.env") do set "%%a"
)

setlocal EnableExtensions
cd /d "%~dp0"

echo ========================================
echo  9yin C++ Server - RUN
echo  Dir: %CD%
echo ========================================
echo.

set "EXE="
if exist "build\Release\protocol_probe.exe" set "EXE=build\Release\protocol_probe.exe"
if exist "build\protocol_probe.exe" set "EXE=build\protocol_probe.exe"
if exist "build\protocol_probe" set "EXE=build\protocol_probe"

if not defined EXE (
    echo [ERROR] protocol_probe.exe not found.
    echo Please run build.bat first.
    echo.
    pause
    exit /b 1
)

set "NINEYIN_SERVER_ROOT=%CD%"

echo EXE    : %EXE%
echo ROOT   : %NINEYIN_SERVER_ROOT%
echo.
echo Listen ports match client server_list.ini:
echo   Area  : 0.0.0.0:4000
echo   Game  : 0.0.0.0:19061
echo   GM    : http://127.0.0.1:19080/
echo.
echo You MUST see "SERVER READY" below.
echo IMPORTANT: run this bat as Administrator (port 80 for update mirror).
echo Right-click run.bat - Run as administrator
echo Do NOT change client updater config.
echo Press Ctrl+C to stop.
echo ----------------------------------------
echo.

start "" cmd /c "timeout /t 2 /nobreak >nul & start http://127.0.0.1:19080/"

REM Default in exe is already 4000+19061; pass explicitly for clarity
"%EXE%" --root "%NINEYIN_SERVER_ROOT%" --listen 0.0.0.0:4000,0.0.0.0:19061

echo.
echo Server exited.
pause
endlocal
