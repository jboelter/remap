@echo off
setlocal

pushd "%~dp0"

set "BUILD_TYPE=%~1"
if "%BUILD_TYPE%"=="" set "BUILD_TYPE=Debug"

if not defined VSCMD_VER (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "%VSWHERE%" (
        echo Could not find vswhere.exe.
        exit /b 1
    )

    set "VSINSTALL="
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSINSTALL=%%I"
    if not defined VSINSTALL (
        echo Could not find a Visual Studio installation.
        exit /b 1
    )

    set "VSDEVCMD=%VSINSTALL%\Common7\Tools\VsDevCmd.bat"
    if not exist "%VSDEVCMD%" (
        echo Could not find VsDevCmd.bat at "%VSDEVCMD%".
        exit /b 1
    )

    set "BUILD_ARCH=amd64"
    if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" set "BUILD_ARCH=arm64"

    call "%VSDEVCMD%" -arch=%BUILD_ARCH% -host_arch=%BUILD_ARCH%
    if errorlevel 1 exit /b %errorlevel%
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

cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=%BUILD_TYPE%
if errorlevel 1 exit /b %errorlevel%

cmake --build build
if errorlevel 1 exit /b %errorlevel%

popd
exit /b 0
