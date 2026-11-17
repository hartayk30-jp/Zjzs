@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"

:: ---------- self-elevate ----------
net session >nul 2>&1
if %errorlevel% neq 0 (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

:: ---------- sanity: cpp present ----------
if not exist "rob_run_menu.cpp" (
    echo [!] rob_run_menu.cpp not found in this folder.
    echo     Put build.bat next to the .cpp and run again.
    pause
    exit /b 1
)

:: ---------- locate MSVC vcvars64 ----------
set "VCVARS="
for %%P in (
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    "C:\Program Files\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
    "C:\Program Files\Microsoft Visual Studio\2019\Professional\VC\Auxiliary\Build\vcvars64.bat"
    "C:\Program Files\Microsoft Visual Studio\2019\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
) do (
    if exist %%P if not defined VCVARS set "VCVARS=%%~P"
)

if not defined VCVARS (
    echo [!] Visual Studio C++ build tools not found.
    echo     Install VS Community with "Desktop development with C++".
    echo     https://visualstudio.microsoft.com/downloads/
    pause
    exit /b 1
)

call "!VCVARS!" >nul

:: ---------- compile ----------
echo [+] building rob_run_menu.exe...
cl /nologo /std:c++20 /O2 /EHsc rob_run_menu.cpp /link user32.lib gdi32.lib /SUBSYSTEM:WINDOWS
if errorlevel 1 (
    echo.
    echo [!] build failed. scroll up for the compiler error.
    pause
    exit /b 1
)

if not exist "rob_run_menu.exe" (
    echo [!] compile reported success but no exe. check the folder.
    pause
    exit /b 1
)

:: ---------- report ----------
echo.
echo [+] built: %~dp0rob_run_menu.exe
echo.
echo     launch it as admin, or run start.bat.
pause
exit /b
