@echo off
REM Dev-loop reset for winzoo injection work.
REM Stops winzoo, removes winzoo's own COM registration (so a fresh Explorer stops
REM loading winzoo_com.dll and the build output unlocks), and restarts Explorer so it
REM releases the injected/COM-loaded DLL. Safe to run between build iterations.

REM Note: delays use ping, since timeout fails under redirected/non-interactive stdin.

echo [reset] stopping winzoo...
taskkill /f /im winzoo.exe >nul 2>&1

echo [reset] removing winzoo COM registration (CLSID_TaskbarList override)...
reg delete "HKCU\Software\Classes\CLSID\{56FDF344-FD6D-11d0-958A-006097C9A090}" /f >nul 2>&1

echo [reset] waiting for winzoo hooks to clear...
ping 127.0.0.1 -n 4 >nul

echo [reset] restarting Explorer to release winzoo_com.dll...
taskkill /f /im explorer.exe >nul 2>&1
ping 127.0.0.1 -n 3 >nul
start explorer.exe
ping 127.0.0.1 -n 3 >nul

echo [reset] done.
