@echo off
setlocal

set "BUNDLE_ROOT=C:\DroidVM\TurnipRuns\run-33322445949\turnip-wddm-arm64-icd-bundle"
set "PROBE_PATH=%BUNDLE_ROOT%\tu_wddm_vulkan_probe_arm64.exe"
set "OUTPUT_PREFIX=C:\DroidVM\TurnipRuns\evidence\context-diagnostic-lifecycle-once-20260831"

if not exist "%PROBE_PATH%" (
    echo Missing lifecycle probe: %PROBE_PATH% 1>&2
    exit /b 2
)

for %%F in (
    "%OUTPUT_PREFIX%.stdout.txt"
    "%OUTPUT_PREFIX%.stderr.txt"
    "%OUTPUT_PREFIX%.exit.txt"
) do if exist "%%~F" (
    echo Refusing to overwrite existing evidence: %%~F 1>&2
    exit /b 3
)

pushd "%BUNDLE_ROOT%" || exit /b 4
"%PROBE_PATH%" 1> "%OUTPUT_PREFIX%.stdout.txt" 2> "%OUTPUT_PREFIX%.stderr.txt"
set "PROBE_EXIT=%ERRORLEVEL%"
popd

> "%OUTPUT_PREFIX%.exit.txt" echo %PROBE_EXIT%
type "%OUTPUT_PREFIX%.stdout.txt"
type "%OUTPUT_PREFIX%.stderr.txt" 1>&2
exit /b %PROBE_EXIT%
