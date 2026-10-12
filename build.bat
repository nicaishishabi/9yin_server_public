@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"

echo ========================================
echo  9yin C++ Server - BUILD
echo  Dir: %CD%
echo ========================================
echo.

REM ---- 1. CMake must exist ----
where cmake >nul 2>&1
if errorlevel 1 (
    echo [ERROR] cmake not found in PATH.
    echo Install from https://cmake.org/download/
    echo and check "Add CMake to the system PATH".
    echo.
    pause
    exit /b 1
)
echo [OK] cmake found

REM ---- 2. ALWAYS remove stale build cache ----
REM     (cache from another machine/OS will break configure)
if exist build (
    echo [INFO] Removing old build\ folder to clear stale CMake cache ...
    rmdir /s /q build
    if exist build (
        echo [ERROR] Cannot delete build\ folder. Close any program using it and retry.
        pause
        exit /b 1
    )
)
mkdir build

REM ---- 3. Load VS / Build Tools env ----
set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "%VSWHERE%" (
    echo [INFO] Looking for VS with vswhere...
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do (
        set "VSINSTALL=%%i"
    )
    if defined VSINSTALL (
        if exist "!VSINSTALL!\VC\Auxiliary\Build\vcvars64.bat" (
            set "VCVARS=!VSINSTALL!\VC\Auxiliary\Build\vcvars64.bat"
        )
    )
)

if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"

REM Also try the path style from user's log (custom install dir)
if not defined VCVARS if exist "D:\VisualStudioTool\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=D:\VisualStudioTool\VC\Auxiliary\Build\vcvars64.bat"

if defined VCVARS (
    echo [OK] Loading VS env:
    echo      %VCVARS%
    call "%VCVARS%"
) else (
    echo [WARN] vcvars64.bat not found, hope cl/g++ is already in PATH
)

REM ---- 4. Detect compiler ----
set "USE_MSVC=0"
set "USE_MINGW=0"
where cl >nul 2>&1
if not errorlevel 1 (
    echo [OK] MSVC cl.exe detected
    set "USE_MSVC=1"
) else (
    where g++ >nul 2>&1
    if not errorlevel 1 (
        echo [OK] MinGW g++ detected
        set "USE_MINGW=1"
    )
)

if "%USE_MSVC%"=="0" if "%USE_MINGW%"=="0" (
    echo.
    echo [ERROR] No C++ compiler found ^(cl.exe / g++^).
    echo Open "x64 Native Tools Command Prompt" and run build.bat again.
    echo.
    pause
    exit /b 1
)

REM ---- 5. Configure with fallbacks ----
cd build
set "CONFIGURED=0"

if "%USE_MSVC%"=="1" (
    echo.
    echo [1/3] CMake configure -A x64 ...
    cmake .. -A x64 -DCMAKE_BUILD_TYPE=Release -DNINEYIN_BUILD_TESTS=OFF
    if not errorlevel 1 set "CONFIGURED=1"

    if "!CONFIGURED!"=="0" (
        echo [RETRY] Visual Studio 17 2022 ...
        cmake .. -G "Visual Studio 17 2022" -A x64 -DCMAKE_BUILD_TYPE=Release -DNINEYIN_BUILD_TESTS=OFF
        if not errorlevel 1 set "CONFIGURED=1"
    )
    if "!CONFIGURED!"=="0" (
        echo [RETRY] Visual Studio 16 2019 ...
        cmake .. -G "Visual Studio 16 2019" -A x64 -DCMAKE_BUILD_TYPE=Release -DNINEYIN_BUILD_TESTS=OFF
        if not errorlevel 1 set "CONFIGURED=1"
    )
    if "!CONFIGURED!"=="0" (
        echo [RETRY] NMake Makefiles ...
        cmake .. -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DNINEYIN_BUILD_TESTS=OFF
        if not errorlevel 1 set "CONFIGURED=1"
    )
)

if "%USE_MINGW%"=="1" if "!CONFIGURED!"=="0" (
    echo.
    echo [1/3] CMake configure - MinGW Makefiles ...
    cmake .. -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DNINEYIN_BUILD_TESTS=OFF
    if not errorlevel 1 set "CONFIGURED=1"
)

if "!CONFIGURED!"=="0" (
    echo.
    echo [ERROR] CMake configure failed with all generators.
    echo Scroll up to read the cmake error lines.
    cd ..
    pause
    exit /b 1
)

echo.
echo [2/3] Building ...
cmake --build . --config Release -j %NUMBER_OF_PROCESSORS%
if errorlevel 1 (
    echo.
    echo [ERROR] Build failed. Scroll up for compiler errors.
    cd ..
    pause
    exit /b 1
)

cd ..

set "EXE="
if exist "build\Release\protocol_probe.exe" set "EXE=build\Release\protocol_probe.exe"
if exist "build\protocol_probe.exe" set "EXE=build\protocol_probe.exe"
if exist "build\protocol_probe" set "EXE=build\protocol_probe"

if not defined EXE (
    echo [ERROR] Build finished but protocol_probe.exe not found.
    pause
    exit /b 1
)

echo.
echo [3/3] BUILD OK
echo   EXE: %CD%\%EXE%
echo.
echo Next: double-click run.bat to start the server.
echo.
pause
endlocal
