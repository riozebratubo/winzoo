@echo off
setlocal

set CMAKE="C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

if not exist %CMAKE% (
    echo ERROR: CMake not found at %CMAKE%
    exit /b 1
)

%CMAKE% -B "%~dp0build" -S "%~dp0" -G "Visual Studio 18 2026" -A x64
if errorlevel 1 exit /b 1

%CMAKE% --build "%~dp0build" --config Release
if errorlevel 1 exit /b 1

echo.
echo Build complete: %~dp0build\Release\winzoo.exe

endlocal
