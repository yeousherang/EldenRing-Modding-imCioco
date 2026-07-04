@echo off
REM ============================================================
REM  Build CustomTalismanEffects (Release).
REM  Configures CMake automatically on first run, then builds.
REM  Copies DLL to release folder after successful build.
REM  Output: build\Release\CustomTalismanEffects.dll
REM         release\CustomTalismanEffects.dll
REM ============================================================
setlocal
set "PROJ=%~dp0"
if "%PROJ:~-1%"=="\" set "PROJ=%PROJ:~0,-1%"

where cmake >nul 2>&1
if errorlevel 1 (
    echo [build] ERROR: cmake not found in PATH.
    goto :fail
)

if not exist "%PROJ%\build\" (
    echo [build] First run - configuring CMake project...
    cmake -S "%PROJ%" -B "%PROJ%\build" -A x64
    if errorlevel 1 goto :fail
)

echo [build] Building Release...
cmake --build "%PROJ%\build" --config Release
if errorlevel 1 goto :fail

REM Create release folder if it doesn't exist
if not exist "%PROJ%\release\" (
    echo [build] Creating release folder...
    mkdir "%PROJ%\release\"
)

REM Copy DLL to release folder
echo [build] Copying DLL to release folder...
copy "%PROJ%\build\Release\CustomTalismanEffects.dll" "%PROJ%\release\CustomTalismanEffects.dll"
if errorlevel 1 (
    echo [build] ERROR: Failed to copy DLL to release folder.
    goto :fail
)

echo.
echo [build] OK -^> "%PROJ%\build\Release\CustomTalismanEffects.dll"
echo [build] OK -^> "%PROJ%\release\CustomTalismanEffects.dll"
goto :end

:fail
echo.
echo [build] BUILD FAILED.
endlocal
exit /b 1

:end
endlocal
pause