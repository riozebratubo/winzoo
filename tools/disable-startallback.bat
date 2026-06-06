@echo off
REM ---------------------------------------------------------------------------
REM disable-startallback.bat
REM
REM Turns OFF StartAllBack's taskbar / Start-menu takeover so winzoo can own the
REM taskbar without the two fighting (the symptom is a constantly blinking/
REM redrawing bar and windows that tremble and resize).
REM
REM How: StartAllBack injects into explorer.exe through its loader COM object
REM   CLSID {117E3954-5034-453A-A18B-7B79493646E6} -> StartAllBackLoaderX64.dll
REM We add a PER-USER (HKCU) override for that CLSID pointing at a non-existent
REM DLL. COM resolves HKCU\Software\Classes before HKLM, so the loader fails to
REM load and StartAllBack never hooks the taskbar. No admin / UAC required.
REM Reverse it any time with enable-startallback.bat.
REM ---------------------------------------------------------------------------
setlocal
set "CLSID={117E3954-5034-453A-A18B-7B79493646E6}"
set "KEY=HKCU\Software\Classes\CLSID\%CLSID%\InProcServer32"

reg add "%KEY%" /ve /t REG_SZ /d "C:\nonexistent\StartAllBack.disabled.dll" /f >nul
reg add "%KEY%" /v ThreadingModel /t REG_SZ /d Apartment /f >nul

echo StartAllBack loader disabled (per-user override added).
echo Restarting Explorer so the change takes effect...
taskkill /f /im explorer.exe >nul 2>&1

echo.
echo Done. The default Windows 11 taskbar should appear.
echo You can now launch winzoo for testing.
echo Run enable-startallback.bat to restore StartAllBack.
echo.
pause
endlocal
