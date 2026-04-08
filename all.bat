@echo off
setlocal

pushd "%~dp0"

powershell -NoProfile -ExecutionPolicy Bypass -File ".github\scripts\simulate-build-flow.ps1" %*
set "EXIT_CODE=%ERRORLEVEL%"

popd
exit /b %EXIT_CODE%
