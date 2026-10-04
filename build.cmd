@echo off
rem Builds FasterNGIO for Windows (CMake preset) and Linux (tools/build_linux.sh under WSL).
rem
rem   build.cmd [all|windows|linux] [cpu] [test]
rem
rem   all (default)  both platforms; windows / linux builds one
rem   cpu            the CPU-only variant (vs2026-cpu, Linux without the GPU stack)
rem   test           run the tests after building
rem
rem Windows needs VCPKG_ROOT and Visual Studio 2026. Linux runs in the WSL distribution named by
rem FASTERNGIO_WSL_DISTRO (default Ubuntu); see tools/build_linux.sh for its toolchain settings.
setlocal

set "TARGET=all"
set "VARIANT=gpu"
set "TEST="
:parse
if "%~1"=="" goto parsed
set "ARG=%~1"
if /i "%ARG%"=="all" set "TARGET=all" & goto next
if /i "%ARG%"=="windows" set "TARGET=windows" & goto next
if /i "%ARG%"=="linux" set "TARGET=linux" & goto next
if /i "%ARG%"=="gpu" set "VARIANT=gpu" & goto next
if /i "%ARG%"=="cpu" set "VARIANT=cpu" & goto next
if /i "%ARG%"=="test" set "TEST=1" & goto next
echo usage: %~nx0 [all^|windows^|linux] [cpu] [test]
exit /b 2
:next
shift
goto parse
:parsed

if not defined FASTERNGIO_WSL_DISTRO set "FASTERNGIO_WSL_DISTRO=Ubuntu"
set "PRESET=vs2026"
if "%VARIANT%"=="cpu" set "PRESET=vs2026-cpu"
set "LINUX_ARGS=%VARIANT%"
if defined TEST set "LINUX_ARGS=%VARIANT% --test"

pushd "%~dp0"

if "%TARGET%"=="linux" goto linux

echo === Windows (%PRESET%) ===
if not defined VCPKG_ROOT (
	echo error: VCPKG_ROOT is not set
	goto fail
)
cmake --preset %PRESET% || goto fail
cmake --build --preset %PRESET% || goto fail
if defined TEST (ctest --preset %PRESET% || goto fail)
echo Windows build: %~dp0build\%PRESET%\bin\RelWithDebInfo

if "%TARGET%"=="windows" goto done

:linux
echo === Linux (%VARIANT%, WSL %FASTERNGIO_WSL_DISTRO%) ===
wsl -d %FASTERNGIO_WSL_DISTRO% --cd "%~dp0." -- bash tools/build_linux.sh %LINUX_ARGS% || goto fail

:done
popd
echo Build succeeded.
exit /b 0

:fail
popd
echo Build failed.
exit /b 1
