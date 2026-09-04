@echo off
setlocal

if "%~2"=="" (
    echo Usage: %~nx0 PROBE_PATH OUTPUT_PREFIX [PROBE_OPTION] 1>&2
    exit /b 2
)

set "PROBE_PATH=%~1"
set "OUTPUT_PREFIX=%~2"
set "PROBE_OPTION=%~3"

if not exist "%PROBE_PATH%" (
    echo Missing probe executable: %PROBE_PATH% 1>&2
    exit /b 3
)

for %%I in ("%PROBE_PATH%") do pushd "%%~dpI" || exit /b 4
if defined PROBE_OPTION (
    "%PROBE_PATH%" --stage=allocation "%PROBE_OPTION%" 1> "%OUTPUT_PREFIX%.stdout.txt" 2> "%OUTPUT_PREFIX%.stderr.txt"
) else (
    "%PROBE_PATH%" --stage=allocation 1> "%OUTPUT_PREFIX%.stdout.txt" 2> "%OUTPUT_PREFIX%.stderr.txt"
)
set "PROBE_EXIT=%ERRORLEVEL%"
type "%OUTPUT_PREFIX%.stdout.txt"
type "%OUTPUT_PREFIX%.stderr.txt" 1>&2
popd

exit /b %PROBE_EXIT%
