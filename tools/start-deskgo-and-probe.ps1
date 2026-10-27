$ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$deskgo = 'C:\Program Files (x86)\Tencent\DeskGo\4.2.1662.127\DesktopMgr64.exe'

Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class W {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
  public struct RECT { public int L, T, R, B; }
  public struct POINT { public int X, Y; }
  public static string ClassOf(IntPtr h) { var sb = new StringBuilder(256); GetClassName(h, sb, 256); return sb.ToString(); }
}
'@

function Get-Progman {
  $script:pm = [IntPtr]::Zero
  [void][W]::EnumWindows({ param($h, $l)
    if ([W]::ClassOf($h) -eq 'Progman') { $script:pm = $h; return $false }
    return $true
  }, [IntPtr]::Zero)
  return $script:pm
}

function Get-TxWindow {
  $progman = Get-Progman
  if ($progman -eq [IntPtr]::Zero) { return [IntPtr]::Zero }
  $script:tx = [IntPtr]::Zero
  [void][W]::EnumChildWindows($progman, { param($h, $l)
    if ([W]::ClassOf($h) -eq 'TXMiniSkin' -or [W]::ClassOf($h) -eq 'TXMiniSkinWnd') { $script:tx = $h; return $false }
    return $true
  }, [IntPtr]::Zero)
  return $script:tx
}

"---- 启动前状态 ----"
"TXMiniSkin = $(Get-TxWindow)"
"DesktopMgr64 running = $([bool](Get-Process DesktopMgr64 -ErrorAction SilentlyContinue))"

if (-not (Get-Process DesktopMgr64 -ErrorAction SilentlyContinue)) {
  "starting DesktopMgr64..."
  Start-Process -FilePath $deskgo
  Start-Sleep -Seconds 8
}

"---- 启动后状态 ----"
$tx = Get-TxWindow
"TXMiniSkin = $tx"
$p = Get-Process DesktopMgr64 -ErrorAction SilentlyContinue
if ($p) { "DesktopMgr64 pid = $($p.Id)" }
if ($tx -ne [IntPtr]::Zero) {
  $r = New-Object W+RECT
  [void][W]::GetWindowRect($tx, [ref]$r)
  "TXMiniSkin rect = $($r.L),$($r.T),$($r.R),$($r.B)"
}

# Find a desktop point that Tencent's overlay actually owns (not covered by
# another top-level window such as the DSH app).
"---- 寻找可点击的桌面点 ----"
$found = $null
foreach ($y in 400, 600, 800, 1000, 300, 500, 700, 900) {
  foreach ($x in 60, 120, 180, 240, 300, 360, 420) {
    $pt = New-Object W+POINT
    $pt.X = $x; $pt.Y = $y
    $h = [W]::WindowFromPoint($pt)
    $cls = [W]::ClassOf($h)
    if ($cls -eq 'TXMiniSkin' -or $cls -eq 'TXMiniSkinWnd') {
      $found = "$x,$y"
      "FREE DESKTOP POINT = $x,$y  (class=$cls)"
      break
    }
  }
  if ($found) { break }
}
if (-not $found) {
  "no uncovered desktop point found; sampling what is on top instead"
  foreach ($y in 400, 600, 800) {
    foreach ($x in 60, 180, 300) {
      $pt = New-Object W+POINT; $pt.X = $x; $pt.Y = $y
      $h = [W]::WindowFromPoint($pt)
      "  ($x,$y) -> $([W]::ClassOf($h))"
    }
  }
}