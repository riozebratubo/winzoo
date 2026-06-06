@echo off
REM ---------------------------------------------------------------------------
REM enable-startallback.bat
REM
REM Re-enables StartAllBack by removing the per-user override that
REM disable-startallback.bat added, then restarting Explorer so StartAllBack's
REM loader injects again and its classic taskbar / Start menu come back.
REM No admin / UAC required.
REM
REM (Make sure winzoo is closed first, otherwise both will try to own the
REM  taskbar again.)
REM ---------------------------------------------------------------------------
setlocal
set "CLSID={117E3954-5034-453A-A18B-7B79493646E6}"

REM Remove the whole overridden CLSID key so COM falls back to the real HKLM
REM registration. Quiet if it wasn't there.
reg delete "HKCU\Software\Classes\CLSID\%CLSID%" /f >nul 2>&1

echo StartAllBack override removed.
echo Restarting Explorer so StartAllBack loads again...
taskkill /f /im explorer.exe >nul 2>&1

echo.
echo Done. StartAllBack should be active again.
echo.
pause
endlocal
