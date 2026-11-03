@echo off
rem Builds the FGUY Controller Look plugin (32-bit, like the game) into dist\FGUYMod\mods\FGUYControllerLook\
setlocal
set VS=
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VS=%%i
if not defined VS set VS=C:\Program Files\Microsoft Visual Studio\2022\Community
if not exist "%VS%\VC\Auxiliary\Build\vcvarsall.bat" (echo Visual Studio 2022 with C++ isn't installed & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
cd /d "%~dp0"
set OUT=..\..\..\dist\FGUYMod\mods\FGUYControllerLook
if not exist ..\..\..\build\controllerlook mkdir ..\..\..\build\controllerlook
cl /nologo /O2 /MT /W4 /EHsc /std:c++17 /DUNICODE /D_UNICODE /LD controllerlook.cpp ^
   /Fo..\..\..\build\controllerlook\ /Fe:..\..\..\build\controllerlook\controllerlook.dll /link user32.lib || exit /b 1
copy /y ..\..\..\build\controllerlook\controllerlook.dll %OUT%\ >nul || exit /b 1
echo Built %OUT%\controllerlook.dll
