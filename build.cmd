@echo off
rem ============================================================
rem  BlockyEngine build script
rem    build            -> configure (if needed) + build Debug
rem    build release    -> build RelWithDebInfo
rem    build clean      -> wipe build dir
rem    build run <name> -> build, then run scene <name>
rem ============================================================
setlocal EnableDelayedExpansion

set "VSROOT=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
set "CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
set "VCVARS=%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"

if not exist "%CMAKE%" ( echo [build] cmake not found: "%CMAKE%" & exit /b 1 )
if not exist "%VCVARS%" ( echo [build] vcvars64 not found: "%VCVARS%" & exit /b 1 )

set "CONFIG=Debug"
set "ACTION=build"
set "SCENE="

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="release" ( set "CONFIG=RelWithDebInfo" & shift & goto parse )
if /i "%~1"=="debug"   ( set "CONFIG=Debug"          & shift & goto parse )
if /i "%~1"=="clean"   ( set "ACTION=clean"          & shift & goto parse )
if /i "%~1"=="run"     ( set "ACTION=run" & set "SCENE=%~2" & shift & shift & goto parse )
echo [build] unknown argument: %~1
exit /b 1
:parsed

set "BUILDDIR=%~dp0build\%CONFIG%"

if "%ACTION%"=="clean" (
    echo [build] removing %~dp0build
    if exist "%~dp0build" rmdir /s /q "%~dp0build"
    exit /b 0
)

if not defined VCINSTALLDIR (
    echo [build] entering MSVC x64 environment...
    call "%VCVARS%" >nul || ( echo [build] vcvars64 failed & exit /b 1 )
)

if not exist "%BUILDDIR%\build.ninja" (
    echo [build] configuring %CONFIG% ...
    "%CMAKE%" -S "%~dp0." -B "%BUILDDIR%" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DCMAKE_BUILD_TYPE=%CONFIG% -DCMAKE_CXX_COMPILER=cl || exit /b 1
)

"%CMAKE%" --build "%BUILDDIR%" || exit /b 1

if "%ACTION%"=="run" (
    if "%SCENE%"=="" ( echo [build] usage: build run ^<scene-name^> & exit /b 1 )
    set "EXE=%BUILDDIR%\scenes\scene_%SCENE%.exe"
    if not exist "!EXE!" ( echo [build] no such scene executable: !EXE! & exit /b 1 )
    echo [build] running scene "%SCENE%" ...
    pushd "%~dp0"
    "!EXE!" || ( popd & exit /b 1 )
    popd
)

echo [build] ok (%CONFIG%)
exit /b 0
