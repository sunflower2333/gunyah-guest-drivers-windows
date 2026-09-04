@echo off
setlocal

if "%~2"=="" (
    echo Usage: %~nx0 BUNDLE_ROOT OUTPUT_PREFIX 1>&2
    exit /b 2
)

set "BUNDLE_ROOT=%~1"
set "OUTPUT_PREFIX=%~2"
set "PROBE_PATH=%BUNDLE_ROOT%\tu_wddm_vulkan_probe_arm64.exe"

if not exist "%PROBE_PATH%" (
    echo Missing probe executable: %PROBE_PATH% 1>&2
    exit /b 3
)

pushd "%BUNDLE_ROOT%" || exit /b 4
"%PROBE_PATH%" 1> "%OUTPUT_PREFIX%.stdout.txt" 2> "%OUTPUT_PREFIX%.stderr.txt"
set "PROBE_EXIT=%ERRORLEVEL%"
type "%OUTPUT_PREFIX%.stdout.txt"
type "%OUTPUT_PREFIX%.stderr.txt" 1>&2
popd

exit /b %PROBE_EXIT%
