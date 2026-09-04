@echo off
setlocal

if "%~2"=="" (
    echo Usage: %~nx0 BUNDLE_ROOT OUTPUT_PREFIX 1>&2
    exit /b 2
)

set "DIRECT_RUNNER=%~dp0run-turnip-wddm-win32-direct.cmd"
if not exist "%DIRECT_RUNNER%" (
    echo Missing direct runner: %DIRECT_RUNNER% 1>&2
    exit /b 3
)

for /L %%N in (1,1,50) do (
    call "%DIRECT_RUNNER%" "%~1" "%~2-%%N"
    if errorlevel 1 exit /b 1
)

echo Win32 interactive loop passed: iterations=50> "%~2-loop.stdout.txt"
exit /b 0
