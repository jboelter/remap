@echo off
setlocal enabledelayedexpansion

pushd "%~dp0"

set "BUILD_TYPE=%~1"
if "%BUILD_TYPE%"=="" set "BUILD_TYPE=Debug"

set "TARGET_ARCH=%~2"
if "%TARGET_ARCH%"=="" set "TARGET_ARCH=%PROCESSOR_ARCHITECTURE%"
if /i "%TARGET_ARCH%"=="AMD64" set "TARGET_ARCH=x64"
if /i "%TARGET_ARCH%"=="X64" set "TARGET_ARCH=x64"
if /i "%TARGET_ARCH%"=="ARM64" set "TARGET_ARCH=arm64"

if /i not "%TARGET_ARCH%"=="x64" if /i not "%TARGET_ARCH%"=="arm64" (
    echo Unsupported target architecture "%TARGET_ARCH%". Use x64 or arm64.
    exit /b 1
)

set "BUILD_TYPE_LOWER=%BUILD_TYPE%"
if /i "%BUILD_TYPE%"=="Debug" set "BUILD_TYPE_LOWER=debug"
if /i "%BUILD_TYPE%"=="Release" set "BUILD_TYPE_LOWER=release"
if /i "%BUILD_TYPE%"=="RelWithDebInfo" set "BUILD_TYPE_LOWER=relwithdebinfo"
if /i "%BUILD_TYPE%"=="MinSizeRel" set "BUILD_TYPE_LOWER=minsizerel"
set "VS_PLATFORM=%TARGET_ARCH%"
if /i "%TARGET_ARCH%"=="arm64" set "VS_PLATFORM=ARM64"

set "VERSION_TAG=%~3"
if "%VERSION_TAG%"=="" set "VERSION_TAG=%REMAP_VERSION_TAG%"

set "GIT_HASH=%~4"
if "%GIT_HASH%"=="" set "GIT_HASH=%REMAP_GIT_HASH%"

set "CMAKE_VERSION_ARG="
if not "%VERSION_TAG%"=="" set "CMAKE_VERSION_ARG=-DREMAP_VERSION_TAG=%VERSION_TAG%"
set "CMAKE_GIT_HASH_ARG="
if not "%GIT_HASH%"=="" set "CMAKE_GIT_HASH_ARG=-DREMAP_GIT_HASH=%GIT_HASH%"

where cmake >nul 2>nul
if errorlevel 1 (
    echo cmake was not found on PATH.
    exit /b 1
)

set "BUILD_DIR=build\windows\%TARGET_ARCH%"

cmake -S . -B "%BUILD_DIR%" --fresh -G "Visual Studio 17 2022" -A %VS_PLATFORM% %CMAKE_VERSION_ARG% %CMAKE_GIT_HASH_ARG%
if errorlevel 1 exit /b %errorlevel%

cmake --build "%BUILD_DIR%" --config %BUILD_TYPE%
if errorlevel 1 exit /b %errorlevel%

echo.
echo Built binaries:
echo   %BUILD_DIR%\%BUILD_TYPE_LOWER%\remap.exe
echo   %BUILD_DIR%\%BUILD_TYPE_LOWER%\tap-timer.exe

popd
exit /b 0
