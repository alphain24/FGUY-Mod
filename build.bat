@echo off
rem Builds the mod loader (dinput8.dll, 32-bit) into build\
setlocal
set VS=C:\Program Files\Microsoft Visual Studio\2022\Community
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /O2 /MT /W4 /EHsc /std:c++17 /DUNICODE /D_UNICODE /LD ^
   src\loader\dllmain.cpp src\loader\common.cpp src\loader\config.cpp ^
   src\loader\display.cpp src\loader\netspy.cpp src\loader\vpad.cpp ^
   /Fobuild\ /Febuild\dinput8.dll ^
   /link /DEF:src\loader\dinput8.def user32.lib kernel32.lib advapi32.lib || exit /b 1
echo Built build\dinput8.dll
