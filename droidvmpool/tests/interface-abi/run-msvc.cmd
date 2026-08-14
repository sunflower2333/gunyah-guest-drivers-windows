@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
set "OUT_DIR=%TEMP%\droidvmpool-interface-abi-%RANDOM%-%RANDOM%"
mkdir "%OUT_DIR%" || exit /b 1

cl /nologo /std:c++17 /W4 /WX /EHsc "%SCRIPT_DIR%\abi_manifest.cpp" /Fe:"%OUT_DIR%\abi.exe"
if errorlevel 1 goto :fail

"%OUT_DIR%\abi.exe" >"%OUT_DIR%\actual.txt"
if errorlevel 1 goto :fail

fc /b "%SCRIPT_DIR%\expected-v2.txt" "%OUT_DIR%\actual.txt" >nul
if errorlevel 1 goto :mismatch

echo PASS msvc
rmdir /s /q "%OUT_DIR%"
exit /b 0

:mismatch
fc "%SCRIPT_DIR%\expected-v2.txt" "%OUT_DIR%\actual.txt"

:fail
echo FAIL droidvmpool-interface-abi: artifacts retained in %OUT_DIR%
exit /b 1
