@echo off
rem Build and run all tutorial examples (Windows).
rem Run it from the "x64 Native Tools Command Prompt for VS 2022".
rem It runs three CMake commands; see the top of CMakeLists.txt.
setlocal
chcp 65001 > nul
cd /d "%~dp0"

cmake --preset default || exit /b 1
cmake --build --preset default || exit /b 1
ctest --preset default || exit /b 1

echo.
echo All examples passed
exit /b 0
