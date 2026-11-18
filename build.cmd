@echo off
rem Builds PowerMenu.exe for ARM64 and x64, then both installers.
rem   build.cmd            - exes + installers
rem   build.cmd noinstall  - exes only
setlocal
set ROOT=%~dp0
set VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat
set MAKENSIS=C:\Program Files (x86)\NSIS\makensis.exe
set PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer

call :build arm64 arm64 || exit /b 1
call :build x64 arm64_amd64 || exit /b 1

if /i "%1"=="noinstall" exit /b 0
if not exist "%ROOT%dist" mkdir "%ROOT%dist"
"%MAKENSIS%" /V2 /DARCH=arm64 "%ROOT%installer\PowerMenu.nsi" || exit /b 1
"%MAKENSIS%" /V2 /DARCH=x64 "%ROOT%installer\PowerMenu.nsi" || exit /b 1
echo.
echo Installers are in %ROOT%dist
exit /b 0

:build
setlocal
call "%VCVARS%" %2 >nul || exit /b 1
set OUT=%ROOT%build\%1
if not exist "%OUT%" mkdir "%OUT%"
rc /nologo /fo "%OUT%\PowerMenu.res" "%ROOT%src\PowerMenu.rc" || exit /b 1
cl /nologo /std:c++17 /O2 /MT /W4 /EHsc /DUNICODE /D_UNICODE /DNDEBUG /Fo"%OUT%\\" ^
   "%ROOT%src\main.cpp" "%OUT%\PowerMenu.res" ^
   /link /SUBSYSTEM:WINDOWS /MANIFEST:NO /OUT:"%OUT%\PowerMenu.exe" || exit /b 1
echo built %OUT%\PowerMenu.exe
exit /b 0
