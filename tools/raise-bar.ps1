# Confirmation probe: lift winzoo's taskbar (class WinzooTaskbar) above its own
# proxy Shell_TrayWnd in Z-order on the LIVE process. If the frozen taskbar
# becomes responsive after this, the "hang" is the proxy-occlusion bug.
# Run ELEVATED (winzoo is elevated; SetWindowPos on its windows needs same IL).
Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public class RB {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int m);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
}
'@
$TOPMOST=[IntPtr](-1); $NOMOVE=0x2; $NOSIZE=0x1; $NOACT=0x10
$wz = (Get-Process winzoo -ErrorAction SilentlyContinue).Id
if (-not $wz) { 'winzoo not running'; return }
$order = New-Object System.Collections.ArrayList
$cb = [RB+EnumProc]{ param($h,$l)
  [uint32]$p=0; [void][RB]::GetWindowThreadProcessId($h,[ref]$p)
  if ($wz -contains $p) {
    $c=New-Object System.Text.StringBuilder 128; [void][RB]::GetClassName($h,$c,128)
    [void]$order.Add(@{h=$h; cls=$c.ToString()})
  }
  return $true
}
[void][RB]::EnumWindows($cb,[IntPtr]::Zero)
'BEFORE (top to bottom):'; $order | ForEach-Object { "  0x{0:X}  {1}" -f $_.h.ToInt64(), $_.cls }
foreach ($w in $order) {
  if ($w.cls -eq 'WinzooTaskbar') {
    [void][RB]::SetWindowPos($w.h, $TOPMOST, 0,0,0,0, ($NOMOVE -bor $NOSIZE -bor $NOACT))
    "Raised 0x{0:X} to HWND_TOPMOST" -f $w.h.ToInt64()
  }
}
Start-Sleep -Milliseconds 200
$order2 = New-Object System.Collections.ArrayList
$cb2 = [RB+EnumProc]{ param($h,$l)
  [uint32]$p=0; [void][RB]::GetWindowThreadProcessId($h,[ref]$p)
  if ($wz -contains $p) {
    $c=New-Object System.Text.StringBuilder 128; [void][RB]::GetClassName($h,$c,128)
    [void]$order2.Add(("  0x{0:X}  {1}" -f $h.ToInt64(), $c.ToString()))
  }
  return $true
}
[void][RB]::EnumWindows($cb2,[IntPtr]::Zero)
'AFTER (top to bottom):'; $order2 | ForEach-Object { $_ }
