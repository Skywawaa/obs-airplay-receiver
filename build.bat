@echo off
REM ============================================
REM OBS AirPlay Receiver - Windows Build Script
REM ============================================
REM
REM Prerequisites:
REM   1. Visual Studio 2019/2022 with C/C++ workload
REM   2. CMake 3.16+
REM   3. OBS Studio (installed or built from source)
REM   4. FFmpeg development libraries
REM
REM Set these paths before building:
REM   OBS_DIR     - Path to OBS Studio install
REM   FFMPEG_DIR  - Path to FFmpeg dev libraries
REM
REM Example:
REM   set OBS_DIR=C:\Program Files\obs-studio
REM   set FFMPEG_DIR=C:\ffmpeg-shared
REM   build.bat
REM ============================================

if not defined OBS_DIR (
    echo.
    echo WARNING: OBS_DIR not set. Trying default location...
    if exist "C:\Program Files\obs-studio" (
        set "OBS_DIR=C:\Program Files\obs-studio"
    ) else (
        echo ERROR: Cannot find OBS Studio. Please set OBS_DIR.
        echo Example: set OBS_DIR=C:\Program Files\obs-studio
        exit /b 1
    )
)

echo Using OBS_DIR: %OBS_DIR%

if defined FFMPEG_DIR (
    echo Using FFMPEG_DIR: %FFMPEG_DIR%
    set FFMPEG_CMAKE=-DFFMPEG_DIR="%FFMPEG_DIR%"
) else (
    echo FFMPEG_DIR not set - will try pkg-config or system paths
    set FFMPEG_CMAKE=
)

REM Create build directory
if not exist build mkdir build
cd build

REM Configure with CMake
cmake .. -G "Visual Studio 17 2022" -A x64 ^
    -DOBS_DIR="%OBS_DIR%" ^
    %FFMPEG_CMAKE%

if errorlevel 1 (
    echo.
    echo CMake configuration failed!
    echo Make sure OBS_DIR and FFMPEG_DIR are set correctly.
    cd ..
    exit /b 1
)

REM Build
cmake --build . --config Release

if errorlevel 1 (
    echo.
    echo Build failed!
    cd ..
    exit /b 1
)

echo.
echo ============================================
echo Build successful!
echo.
echo To install, run:
echo   cmake --install . --config Release
echo.
echo Or manually copy:
echo   build\Release\obs-airplay-receiver.dll
echo   to: %%APPDATA%%\obs-studio\plugins\obs-airplay-receiver\bin\64bit\
echo ============================================

cd ..
