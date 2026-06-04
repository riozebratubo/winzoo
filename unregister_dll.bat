rem  1. Delete the registry key:
reg delete "HKCU\Software\Classes\CLSID\{56FDF344-FD6D-11d0-958A-006097C9A090}" /f

rem  2. Check what process is locking the file:
rem tasklist /m winzoo_com.dll

rem  If Explorer shows up there, restart it:
rem taskkill /f /im explorer.exe && start explorer.exe