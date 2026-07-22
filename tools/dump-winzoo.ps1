# Capture a full memory dump of a (hung) winzoo.exe for post-mortem debugging.
# Run from an ELEVATED PowerShell (winzoo runs elevated, the dumper must match):
#   powershell -ExecutionPolicy Bypass -File .\tools\dump-winzoo.ps1
# Writes tools\dumps\winzoo_<pid>_<timestamp>.dmp using the in-box comsvcs MiniDump.
# The dump shows every thread's stack — enough to see exactly where a hang sits
# (e.g. blocked in a cross-process SendMessage into Explorer, or in paging I/O
# if the D: drive reset with the binary mapped).
$ErrorActionPreference = 'Stop'

$procs = Get-Process winzoo -ErrorAction SilentlyContinue
if (-not $procs) {
    Write-Host 'winzoo.exe is not running.'
    exit 1
}

$dumpDir = Join-Path $PSScriptRoot 'dumps'
New-Item -ItemType Directory -Force $dumpDir | Out-Null

foreach ($p in $procs) {
    $stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
    $path  = Join-Path $dumpDir "winzoo_$($p.Id)_$stamp.dmp"
    Write-Host "Dumping PID $($p.Id) -> $path (takes a few seconds)..."
    # comsvcs.dll MiniDump is synchronous; rundll32 exits when the file is complete.
    Start-Process -Wait rundll32.exe -ArgumentList "C:\Windows\System32\comsvcs.dll, MiniDump $($p.Id) $path full"
    if (Test-Path $path) {
        Write-Host "Wrote $path ($([math]::Round((Get-Item $path).Length / 1MB)) MB)"
    } else {
        Write-Host "FAILED for PID $($p.Id) - is this PowerShell elevated?"
    }
}
