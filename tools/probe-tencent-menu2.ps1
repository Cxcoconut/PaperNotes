$ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class TT {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern void keybd_event(byte b, byte s, uint f, UIntPtr e);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  public static string ClassOf(IntPtr h) { var sb = new StringBuilder(256); GetClassName(h, sb, 256); return sb.ToString(); }
}
'@

$script:pm = [IntPtr]::Zero
[void][TT]::EnumWindows({ param($h,$l)
  if ([TT]::ClassOf($h) -eq 'Progman') { $script:pm = $h; return $false }
  return $true }, [IntPtr]::Zero)
$script:tx = [IntPtr]::Zero
if ($script:pm -ne [IntPtr]::Zero) {
  [void][TT]::EnumChildWindows($script:pm, { param($h,$l)
    $c = [TT]::ClassOf($h)
    if ($c -eq 'TXMiniSkin' -or $c -eq 'TXMiniSkinWnd') { $script:tx = $h; return $false }
    return $true }, [IntPtr]::Zero)
}
$tx = $script:tx
"TXMiniSkin = $tx"
if ($tx -eq [IntPtr]::Zero) { "overlay not found"; exit 1 }

$label = -join [char[]](0x65B0, 0x5EFA, 0x4FBF, 0x7B7E)
$x = 420; $y = 520
$lparam = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))

[TT]::SetForegroundWindow($tx) | Out-Null
Start-Sleep -Milliseconds 250

# Right-button down + up delivered directly to the overlay window. WM_CONTEXTMENU
# alone did not raise the menu, so drive the raw button messages instead.
[TT]::PostMessage($tx, 0x0204, [IntPtr]2, $lparam) | Out-Null   # WM_RBUTTONDOWN, MK_RBUTTON
Start-Sleep -Milliseconds 120
[TT]::PostMessage($tx, 0x0205, [IntPtr]0, $lparam) | Out-Null   # WM_RBUTTONUP
Start-Sleep -Milliseconds 1600

Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$auto = [System.Windows.Automation.AutomationElement]
$root = $auto::RootElement
$names = @{}
for ($i = 0; $i -lt 12; $i++) {
  try {
    $cond = New-Object System.Windows.Automation.PropertyCondition($auto::ControlTypeProperty, [System.Windows.Automation.ControlType]::MenuItem)
    $found = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)
    foreach ($el in $found) {
      try { if (-not $el.Current.IsOffscreen -and $el.Current.Name) { $names[$el.Current.Name] = $true } } catch { }
    }
  } catch { }
  Start-Sleep -Milliseconds 350
}

"---- observed menu items ----"
if ($names.Keys.Count -eq 0) { "  (none)" } else { $names.Keys | Sort-Object | ForEach-Object { "  $_" } }
$joined = ($names.Keys -join '|')
"---- verdict ----"
"  Tencent menu opened (saw 护眼卫士/右键菜单管理): $($joined -match '右键菜单管理|护眼卫士')"
"  our item visible                              : $($joined -match [regex]::Escape($label))"

[TT]::keybd_event(0x1B,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 60
[TT]::keybd_event(0x1B,0,2,[UIntPtr]::Zero)