@echo off
rem Builds MLTweaksNative.dll with the Visual Studio 2022 Build Tools (x64).
rem Static C runtime (/MT) on purpose: the game ships a 2015-era VC runtime, and a DLL
rem built with a current compiler must not depend on that one.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if not exist build mkdir build
rc /nologo /fo build\MLTweaksNative.res src\MLTweaksNative.rc || exit /b 1
cl /nologo /O2 /EHsc /MT /LD /W4 src\MLTweaksNative.cpp build\MLTweaksNative.res /Fo:build\ /Fe:MLTweaksNative.dll /link /NOLOGO /IMPLIB:build\MLTweaksNative.lib
