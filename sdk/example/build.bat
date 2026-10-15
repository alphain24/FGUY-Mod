@echo off
rem Builds example.dll (32-bit, like the game) with Visual Studio 2022.
setlocal
set VS=C:\Program Files\Microsoft Visual Studio\2022\Community
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /O2 /MT /W4 /EHsc /LD example_plugin.cpp /Fobuild\ /Fe:build\example.dll /link user32.lib || exit /b 1
echo Built build\example.dll
