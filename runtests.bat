@echo off
setlocal
cd /d "%~dp0"

echo.
echo ============================================================
echo  winzoo instrumented tests
echo ============================================================
echo.

:: ── Check Python ────────────────────────────────────────────
python --version >nul 2>&1
if errorlevel 1 (
    echo ERROR: Python not found on PATH.
    echo Install Python 3.8+ and make sure it is on your PATH.
    exit /b 1
)

:: ── Check / install requirements ────────────────────────────
echo Checking test requirements...
python -c "import pytest, pywinauto, PIL" >nul 2>&1
if errorlevel 1 (
    echo   Requirements missing -- installing from tests\requirements.txt...
    python -m pip install -r tests\requirements.txt
    if errorlevel 1 (
        echo ERROR: Failed to install requirements.
        exit /b 1
    )
    echo   Requirements installed.
) else (
    echo   Requirements already satisfied.
)
echo.

:: ── Check the binary exists ──────────────────────────────────
if not exist "build\Release\winzoo.exe" (
    echo ERROR: build\Release\winzoo.exe not found.
    echo Run build.bat first, then re-run this script.
    exit /b 1
)

:: ── Run tests ────────────────────────────────────────────────
echo Running tests...
echo.
python -m pytest tests\ -v %*
set EXIT_CODE=%errorlevel%

echo.
if %EXIT_CODE% == 0 (
    echo ============================================================
    echo  All tests passed.
    echo ============================================================
) else (
    echo ============================================================
    echo  Tests FAILED  (exit code %EXIT_CODE%)
    echo ============================================================
)

exit /b %EXIT_CODE%
