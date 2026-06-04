@echo off
python "%~dp0check_settings_coverage.py"
if %ERRORLEVEL% neq 0 (
    echo.
    echo [FAIL] Coverage gaps detected - see above.
    pause
    exit /b 1
)
pause
