@echo off
rem Builds and runs the Direct3D 9 device test (32-bit, like the game).
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
cd /d "%~dp0"
cl /nologo /EHsc d3d9test.cpp /Fe:d3d9test.exe || exit /b 1
d3d9test.exe
