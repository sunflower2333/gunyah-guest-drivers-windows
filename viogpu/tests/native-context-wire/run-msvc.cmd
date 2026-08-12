@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
set "WINDOWS_INCLUDE=%SCRIPT_DIR%\..\..\common"
set "OUT_DIR=%TEMP%\nctx-wire-%RANDOM%-%RANDOM%"
mkdir "%OUT_DIR%" || exit /b 1

cl /nologo /std:c++17 /W4 /WX /EHsc /DABI_ENDPOINT_WINDOWS /I"%SCRIPT_DIR%" /I"%WINDOWS_INCLUDE%" "%SCRIPT_DIR%\abi_manifest.cpp" /Fe:"%OUT_DIR%\abi_manifest.exe"
if errorlevel 1 goto :fail

"%OUT_DIR%\abi_manifest.exe" >"%OUT_DIR%\manifest.txt"
if errorlevel 1 goto :fail

fc /b "%SCRIPT_DIR%\expected-v2.txt" "%OUT_DIR%\manifest.txt" >nul
if errorlevel 1 goto :mismatch

echo PASS windows-msvc
rmdir /s /q "%OUT_DIR%"
exit /b 0

:mismatch
fc "%SCRIPT_DIR%\expected-v2.txt" "%OUT_DIR%\manifest.txt"

:fail
echo FAIL windows-msvc: artifacts retained in %OUT_DIR%
exit /b 1
