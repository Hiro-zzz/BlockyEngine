@echo off
rem ============================================================
rem  BlockyEngine build script
rem    build            -> configure (if needed) + build Debug
rem    build release    -> build RelWithDebInfo
rem    build clean      -> wipe build dir
rem    build run <name> -> build, then run scene <name>
rem
rem  The compiler, CMake and Ninja all live inside Visual Studio and none of
rem  them is on PATH. This finds the installation rather than naming one; see
rem  :find_visual_studio at the bottom, and docs/install.md for what to install.
rem ============================================================
setlocal EnableDelayedExpansion

rem The parentheses in this name close any block that mentions it, so it is
rem copied once here, at the top level, where there is no block to close.
set "PF86=%ProgramFiles(x86)%"

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

rem Deleting a directory needs no compiler, so this runs before the toolchain
rem is looked for: `clean` has to work on a machine that cannot build.
if "%ACTION%"=="clean" (
    echo [build] removing %~dp0build
    if exist "%~dp0build" rmdir /s /q "%~dp0build"
    exit /b 0
)

call :find_visual_studio
if not defined VSROOT (
    echo [build] no Visual Studio with the C++ toolset was found.
    echo [build]
    echo [build] Looked for vswhere at:
    echo [build]   "%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
    echo [build] and then in the usual install directories.
    echo [build]
    echo [build] If it is somewhere else, say so and try again:
    echo [build]   set "BLOCKY_VSROOT=C:\Program Files\Microsoft Visual Studio\2022\Community"
    echo [build]
    echo [build] If it is not installed at all, docs/install.md lists the two
    echo [build] components needed -- the build tools are a free download.
    exit /b 1
)

set "CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
set "VCVARS=%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"

if not exist "%CMAKE%" (
    echo [build] found "%VSROOT%" but no cmake in it.
    echo [build] The "C++ CMake tools for Windows" component is missing; see docs/install.md
    exit /b 1
)
if not exist "%NINJA%" (
    echo [build] found "%VSROOT%" but no ninja in it.
    echo [build] The "C++ CMake tools for Windows" component is missing; see docs/install.md
    exit /b 1
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

rem ============================================================
rem  Sets VSROOT to an installation that has vcvars64.bat, or leaves it empty.
rem
rem  Three ways, in order of how much they are worth trusting:
rem
rem    1. BLOCKY_VSROOT, if somebody said where it is. An explicit answer beats
rem       a search, and it is the escape hatch for a layout nothing else knows.
rem    2. vswhere -- the tool Microsoft ships for exactly this question, and it
rem       arrives with every Visual Studio since 2017. `-latest` picks the
rem       newest, `-requires` skips installations without the C++ toolset,
rem       which is the whole point of asking.
rem    3. The usual directories, newest first. Only reached when vswhere is
rem       missing, which means an installation older than it or a copy moved
rem       by hand.
rem ============================================================
:find_visual_studio
set "VSROOT="

if defined BLOCKY_VSROOT (
    if exist "%BLOCKY_VSROOT%\VC\Auxiliary\Build\vcvars64.bat" set "VSROOT=%BLOCKY_VSROOT%"
    goto :eof
)

set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :vs_guess

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VSROOT=%%i"
if not defined VSROOT goto :vs_guess
if exist "!VSROOT!\VC\Auxiliary\Build\vcvars64.bat" goto :eof
set "VSROOT="

:vs_guess
for %%r in (
    "%PF86%\Microsoft Visual Studio\18\BuildTools"
    "%ProgramFiles%\Microsoft Visual Studio\18\BuildTools"
    "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Community"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Professional"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise"
    "%PF86%\Microsoft Visual Studio\2019\BuildTools"
    "%PF86%\Microsoft Visual Studio\2019\Community"
) do if not defined VSROOT if exist "%%~r\VC\Auxiliary\Build\vcvars64.bat" set "VSROOT=%%~r"
goto :eof
