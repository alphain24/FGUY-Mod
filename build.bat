@echo off
rem Builds the mod loader (dinput8.dll, 32-bit) into build\
setlocal
rem (Visual Studio 2022 with C++ - Community or the Build Tools; vswhere finds whichever is installed)
set VS=
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VS=%%i
if not defined VS set VS=C:\Program Files\Microsoft Visual Studio\2022\Community
if not exist "%VS%\VC\Auxiliary\Build\vcvarsall.bat" (echo Visual Studio 2022 with C++ isn't installed & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /O2 /MT /W4 /EHsc /std:c++17 /DUNICODE /D_UNICODE /LD ^
   src\loader\*.cpp ^
   /Fobuild\ /Febuild\dinput8.dll ^
   /link /DEF:src\loader\dinput8.def user32.lib kernel32.lib advapi32.lib dxguid.lib || exit /b 1
echo Built build\dinput8.dll
