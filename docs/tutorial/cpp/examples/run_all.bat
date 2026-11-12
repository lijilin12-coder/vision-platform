@echo off
rem Build and run all tutorial examples on Windows. CI uses this script too.
rem Run it from the "x64 Native Tools Command Prompt for VS 2022":
rem     docs\tutorial\cpp\examples\run_all.bat
rem "build only" marks examples that contain a bug ON PURPOSE.
rem This file is kept ASCII-only: cmd.exe reads batch files in the system
rem code page, so non-ASCII text could break parsing on a Chinese Windows.
setlocal
chcp 65001 > nul
cd /d "%~dp0"
set F=/nologo /std:c++17 /W4 /EHsc /utf-8

call :run lesson00 test.cpp blob_count.cpp || exit /b 1
call :build lesson00 main.cpp blob_count.cpp || exit /b 1

call :run lesson01 test.cpp my_string.cpp || exit /b 1
call :run lesson01 complex_rule_of_zero.cpp || exit /b 1
call :run lesson01 c_style.cpp || exit /b 1
call :build lesson01 broken_copy.cpp || exit /b 1

echo == lesson02: CMake
cmake -S lesson02 -B lesson02\build -G Ninja > nul || exit /b 1
cmake --build lesson02\build > nul || exit /b 1
ctest --test-dir lesson02\build --output-on-failure > nul || exit /b 1
lesson02\build\app\overlap_app.exe || exit /b 1

call :run lesson03 copy_counter.cpp || exit /b 1
call :run lesson03 registry_bug.cpp || exit /b 1
call :build lesson03 dangling.cpp || exit /b 1

call :run lesson04 raii.cpp || exit /b 1
call :run lesson05 measurement.cpp || exit /b 1
set SAVED=%F%
set F=%F% /I..\..\..\..\..\core\common\include
call :run lesson06 errors.cpp || exit /b 1
set F=%SAVED%
call :run lesson07 variant.cpp || exit /b 1
call :run lesson08 interfaces.cpp || exit /b 1
call :run lesson09 algorithms.cpp || exit /b 1
call :run lesson10 templates.cpp || exit /b 1

call :run lesson12 debug_me.cpp || exit /b 1

call :build lesson13 overflow.cpp || exit /b 1
call :build lesson13 leak.cpp || exit /b 1
call :build lesson13 int_overflow.cpp || exit /b 1
call :run lesson13 leak_fixed.cpp || exit /b 1
call :run lesson13 growth.cpp || exit /b 1

set F=%F% /O2
call :run lesson14 benchmark.cpp || exit /b 1
set F=%SAVED%

call :build lesson15 race.cpp || exit /b 1
call :run lesson15 sync.cpp || exit /b 1
call :run lesson16 future_basics.cpp || exit /b 1
call :run lesson16 inference_worker.cpp || exit /b 1

echo.
echo All examples passed
exit /b 0

:run
echo == %1: %2 %3
pushd %1
cl %F% %2 %3 /Fe:prog.exe > build.log 2>&1
if errorlevel 1 ( type build.log & popd & echo BUILD FAILED: %1 %2 & exit /b 1 )
prog.exe
if errorlevel 1 ( popd & echo RUN FAILED: %1 %2 & exit /b 1 )
popd
exit /b 0

:build
echo == %1: %2 %3 [build only]
pushd %1
cl %F% %2 %3 /Fe:prog.exe > build.log 2>&1
if errorlevel 1 ( type build.log & popd & echo BUILD FAILED: %1 %2 & exit /b 1 )
popd
exit /b 0
