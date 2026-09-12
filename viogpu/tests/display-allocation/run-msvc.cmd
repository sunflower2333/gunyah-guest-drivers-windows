@echo off
setlocal
pushd "%~dp0"
if not exist build mkdir build
powershell -NoProfile -ExecutionPolicy Bypass -File verify-fixtures.ps1
if errorlevel 1 exit /b 1
cl /nologo /EHsc /W4 /WX /std:c++17 display_allocation_test.cpp /Febuild\display_allocation_test.exe /Fobuild\display_allocation_test.obj
if errorlevel 1 exit /b 1
build\display_allocation_test.exe fixtures
set result=%errorlevel%
popd
exit /b %result%
