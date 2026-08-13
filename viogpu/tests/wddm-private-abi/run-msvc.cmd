@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
set "SHARED_INCLUDE=%SCRIPT_DIR%\..\..\shared"
set "OUT_DIR=%TEMP%\wddm-private-abi-%RANDOM%-%RANDOM%"
mkdir "%OUT_DIR%" || exit /b 1

for %%E in (KMD UMD) do (
    cl /nologo /std:c++17 /W4 /WX /EHsc /DABI_ENDPOINT_%%E /I"%SCRIPT_DIR%" /I"%SHARED_INCLUDE%" "%SCRIPT_DIR%\abi_manifest.cpp" /Fe:"%OUT_DIR%\%%E.exe"
    if errorlevel 1 goto :fail
    "%OUT_DIR%\%%E.exe" >"%OUT_DIR%\%%E.txt"
    if errorlevel 1 goto :fail
    fc /b "%SCRIPT_DIR%\expected-pre-v1.txt" "%OUT_DIR%\%%E.txt" >nul
    if errorlevel 1 (
        fc "%SCRIPT_DIR%\expected-pre-v1.txt" "%OUT_DIR%\%%E.txt"
        goto :fail
    )
    echo PASS %%E-msvc
)

rmdir /s /q "%OUT_DIR%"
exit /b 0

:fail
echo FAIL wddm-private-abi: artifacts retained in %OUT_DIR%
exit /b 1
