@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"

:: ============================================================
:: analyze.bat - Run clang-tidy static analysis on winzoo
::
:: Usage:
::   analyze.bat            - run clang-tidy on all src/ files
::   analyze.bat [file...]  - run on specific files, e.g.:
::                            analyze.bat src\App.cpp src\Renderer.cpp
:: ============================================================

set VS_ROOT=C:\Program Files\Microsoft Visual Studio\18\Community
set VCVARS=%VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat
set CMAKE=%VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
set NINJA=%VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe
set CLANG_TIDY=%VS_ROOT%\VC\Tools\Llvm\x64\bin\clang-tidy.exe

if not exist "%VCVARS%" (
    echo ERROR: vcvars64.bat not found at: %VCVARS%
    exit /b 1
)
if not exist "%CMAKE%" (
    echo ERROR: cmake.exe not found at: %CMAKE%
    exit /b 1
)
if not exist "%NINJA%" (
    echo ERROR: ninja.exe not found at: %NINJA%
    exit /b 1
)
if not exist "%CLANG_TIDY%" (
    echo ERROR: clang-tidy.exe not found at: %CLANG_TIDY%
    exit /b 1
)

:: ------------------------------------------------------------
:: Activate MSVC environment so clang-cl can find system headers
:: ------------------------------------------------------------
call "%VCVARS%" >nul 2>&1

:: ------------------------------------------------------------
:: Configure an analysis build using Ninja + clang-cl.
:: clang-cl flags are understood by clang-tidy; it also produces
:: compile_commands.json which clang-tidy needs.
:: /MP is omitted — clang-cl does not support it.
:: ------------------------------------------------------------
set BUILD_DIR=%~dp0build-analyze

echo [analyze] Configuring build-analyze with Ninja + clang-cl...
"%CMAKE%" -B "%BUILD_DIR%" -S . ^
    -G Ninja ^
    -DCMAKE_C_COMPILER=clang-cl ^
    -DCMAKE_CXX_COMPILER=clang-cl ^
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_MAKE_PROGRAM="%NINJA%"
if errorlevel 1 (
    echo ERROR: CMake configure failed.
    exit /b 1
)

:: /MP and /Zc:preprocessor are MSVC-only flags that clang-cl does not support.
:: They are suppressed via --extra-arg=-Wno-unused-command-line-argument below.

echo.
echo [analyze] Building (generates compile_commands.json)...
"%CMAKE%" --build "%BUILD_DIR%"
if errorlevel 1 (
    echo.
    echo WARNING: Build had errors. clang-tidy will still run on successfully parsed files.
)

:: ------------------------------------------------------------
:: Collect source files to analyse
:: ------------------------------------------------------------
set FILES=
if "%~1"=="" (
    for %%f in (%~dp0src\*.cpp) do set FILES=!FILES! "%%f"
    goto :run_tidy
)
:argloop
if "%~1"=="" goto :run_tidy
set FILES=!FILES! "%~f1"
shift
goto :argloop

:run_tidy
echo.
echo [analyze] Running clang-tidy...
echo.
"%CLANG_TIDY%" %FILES% -p "%BUILD_DIR%" --extra-arg=-Wno-unused-command-line-argument

echo.
echo [analyze] Done.
endlocal
