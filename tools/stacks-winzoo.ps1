# Non-invasively attach cdb to the live (hung) winzoo and dump every thread's
# stack to tools\dumps\stacks_<pid>.txt, then make the file user-readable.
# Run ELEVATED (winzoo is elevated). -pv = non-invasive: read-only, does not
# resume/disturb the target, safe to run against a hung process.
$ErrorActionPreference = 'Stop'
$cdb = "${env:ProgramFiles(x86)}\Windows Kits\10\Debuggers\x64\cdb.exe"
$p = Get-Process winzoo -ErrorAction SilentlyContinue
if (-not $p) { Write-Host 'winzoo not running'; exit 1 }

$outDir = Join-Path $PSScriptRoot 'dumps'
New-Item -ItemType Directory -Force $outDir | Out-Null
$out = Join-Path $outDir "stacks_$($p.Id).txt"

$sympath = "srv*C:\symbols*https://msdl.microsoft.com/download/symbols;D:\dev\winzoo\build\Release"
# ~*k 40 = all threads, 40 frames each. !runaway = per-thread CPU. lmvm winzoo* = module load.
& $cdb -pv -p $p.Id -y $sympath -c '.lines; ~*k 40; !runaway 7; lm; q' *> $out

icacls $out /grant '*S-1-5-32-545:F' | Out-Null
Write-Host "Wrote $out"
