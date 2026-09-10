@echo off
setlocal
pushd "%~dp0"
if not exist build mkdir build
cl /nologo /EHsc /W4 /WX /std:c++17 display_timing_test.cpp /Febuild\display_timing_test.exe /Fobuild\display_timing_test.obj
if errorlevel 1 exit /b 1
build\display_timing_test.exe
set result=%errorlevel%
popd
exit /b %result%
