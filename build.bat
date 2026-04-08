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

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    set "VSINSTALL="
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requiresAny -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.Tools.ARM64 -property installationPath`) do set "VSINSTALL=%%I"
    if not defined VSINSTALL (
        echo Could not find a Visual Studio installation.
        exit /b 1
    )

    set "VSDEVCMD=!VSINSTALL!\Common7\Tools\VsDevCmd.bat"
    if not exist "!VSDEVCMD!" (
        echo Could not find VsDevCmd.bat at "!VSDEVCMD!".
        exit /b 1
    )

    set "HOST_ARCH=amd64"
    if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" set "HOST_ARCH=arm64"

    set "VS_TARGET_ARCH=%TARGET_ARCH%"
    if /i "%TARGET_ARCH%"=="x64" set "VS_TARGET_ARCH=amd64"

    call "!VSDEVCMD!" -arch=%VS_TARGET_ARCH% -host_arch=%HOST_ARCH%
    if errorlevel 1 exit /b %errorlevel%
)
if not exist "%VSWHERE%" if not defined VSCMD_VER (
    echo Could not find vswhere.exe.
    exit /b 1
)

where cmake >nul 2>nul
if errorlevel 1 (
    echo cmake was not found on PATH.
    exit /b 1
)

where ninja >nul 2>nul
if errorlevel 1 (
    echo ninja was not found on PATH.
    exit /b 1
)

where clang-cl >nul 2>nul
if errorlevel 1 (
    echo clang-cl was not found on PATH.
    exit /b 1
)

set "BUILD_DIR=build\windows\%TARGET_ARCH%"

cmake -S . -B "%BUILD_DIR%" -G Ninja -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=%BUILD_TYPE%
if errorlevel 1 exit /b %errorlevel%

cmake --build "%BUILD_DIR%"
if errorlevel 1 exit /b %errorlevel%

echo.
echo Built binaries:
echo   %BUILD_DIR%\%BUILD_TYPE_LOWER%\remap.exe
echo   %BUILD_DIR%\%BUILD_TYPE_LOWER%\tap-timer.exe

popd
exit /b 0
