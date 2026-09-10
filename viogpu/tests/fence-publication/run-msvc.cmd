@echo off
setlocal
pushd "%~dp0"
if not exist build mkdir build
cl /nologo /EHsc /W4 /WX /std:c++17 fence_publication_test.cpp /Febuild\fence_publication_test.exe /Fobuild\fence_publication_test.obj
if errorlevel 1 exit /b 1
build\fence_publication_test.exe
set result=%errorlevel%
popd
exit /b %result%
