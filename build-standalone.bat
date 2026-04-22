@echo off
REM ============================================================
REM airplay-stream.exe - Standalone Build Script
REM ============================================================
REM
REM Builds airplay-stream.exe WITHOUT the OBS Studio plugin.
REM No OBS installation required.
REM
REM Prerequisites (all must be available before running):
REM   1. Visual Studio 2019/2022 with C/C++ workload
REM      Run this script from a "VS Developer Command Prompt".
REM   2. CMake 3.16+
REM   3. deps\uxplay-build\build\  — built UxPlay libs
REM      (airplay.lib, playfair.lib, llhttp.lib)
REM   4. deps\libplist-2.7.0\build\plist.lib  — built libplist
REM   5. deps\ffmpeg7-include\   — FFmpeg 7.x headers
REM      deps\obs-ffmpeg-libs\   — FFmpeg import libs
REM         (avformat.lib, avcodec.lib, avutil.lib, swresample.lib)
REM   6. OpenSSL — set OPENSSL_DIR or install via:
REM        choco install openssl
REM        scoop install openssl
REM   7. deps\w32-pthreads.lib + w32-pthreads headers
REM      (from the OBS SDK or installed separately)
REM   8. deps\dnssd.lib  — Apple Bonjour SDK import lib
REM
REM Optional overrides (set before calling this script):
REM   set OPENSSL_DIR=C:\path\to\openssl
REM   set FFMPEG_LIB_DIR=C:\path\to\ffmpeg-libs
REM   set FFMPEG_INCLUDE_DIR=C:\path\to\ffmpeg-include
REM   set W32_PTHREADS_DIR=C:\path\to\w32-pthreads-headers
REM ============================================================

setlocal

REM --- OpenSSL ---
if defined OPENSSL_DIR (
    echo Using OPENSSL_DIR: %OPENSSL_DIR%
    set OPENSSL_CMAKE=-DOPENSSL_DIR="%OPENSSL_DIR%"
) else (
    echo OPENSSL_DIR not set — will search default Scoop/Chocolatey paths
    set OPENSSL_CMAKE=
)

REM --- FFmpeg (optional overrides) ---
if defined FFMPEG_LIB_DIR (
    echo Using FFMPEG_LIB_DIR: %FFMPEG_LIB_DIR%
    set FFMPEG_LIB_CMAKE=-DFFMPEG_LIB_DIR="%FFMPEG_LIB_DIR%"
) else (
    set FFMPEG_LIB_CMAKE=
)

if defined FFMPEG_INCLUDE_DIR (
    echo Using FFMPEG_INCLUDE_DIR: %FFMPEG_INCLUDE_DIR%
    set FFMPEG_INC_CMAKE=-DFFMPEG_INCLUDE_DIR="%FFMPEG_INCLUDE_DIR%"
) else (
    set FFMPEG_INC_CMAKE=
)

REM --- w32-pthreads headers (optional override) ---
if defined W32_PTHREADS_DIR (
    echo Using W32_PTHREADS_DIR: %W32_PTHREADS_DIR%
    set W32_PT_CMAKE=-DW32_PTHREADS_DIR="%W32_PTHREADS_DIR%"
) else (
    set W32_PT_CMAKE=
)

REM --- Create build directory ---
if not exist build-standalone mkdir build-standalone
cd build-standalone

REM --- Configure with CMake ---
REM  BUILD_OBS_PLUGIN=OFF  — skip the OBS plugin entirely
REM  BUILD_STANDALONE=ON   — build airplay-stream.exe
cmake .. -G "NMake Makefiles" ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_C_COMPILER=cl ^
    -DBUILD_OBS_PLUGIN=OFF ^
    -DBUILD_STANDALONE=ON ^
    %OPENSSL_CMAKE% ^
    %FFMPEG_LIB_CMAKE% ^
    %FFMPEG_INC_CMAKE% ^
    %W32_PT_CMAKE%

if errorlevel 1 (
    echo.
    echo CMake configuration failed!
    echo Check that all dependencies listed above are present.
    cd ..
    exit /b 1
)

REM --- Build ---
nmake

if errorlevel 1 (
    echo.
    echo Build failed!
    cd ..
    exit /b 1
)

cd ..

echo.
echo ============================================================
echo Build successful!
echo.
echo Output: build-standalone\standalone\airplay-stream.exe
echo.
echo Copy the following DLLs next to airplay-stream.exe:
echo   avformat-*.dll avcodec-*.dll avutil-*.dll swresample-*.dll
echo   libcrypto-3-x64.dll  dnssd.dll  w32-pthreads.dll
echo.
echo Usage:
echo   airplay-stream.exe [--name "My Stream"] [--port 8888]
echo   airplay-stream.exe --hw-accel   (hardware audio codec)
echo   airplay-stream.exe --help
echo ============================================================
