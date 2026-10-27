$ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class T {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern void keybd_event(byte b, byte s, uint f, UIntPtr e);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  public struct RECT { public int L, T, R, B; }
  public static string ClassOf(IntPtr h) { var sb = new StringBuilder(256); GetClassName(h, sb, 256); return sb.ToString(); }
}
'@

$label  = -join [char[]](0x5EFA, 0x8BAE, 0x6D4B, 0x8BD5)   # 建议测试

# A verb shaped EXACTLY like the WorkBuddy entry that DOES appear.
$key = 'HKCU:\Software\Classes\Directory\Background\shell\PaperNotesProbe'
New-Item -Path $key -Force | Out-Null
Set-ItemProperty -Path $key -Name '(default)' -Value $label
Set-ItemProperty -Path $key -Name 'Icon'      -Value 'C:\cx_ProjectCode\PaperNotes\out\bin\PaperNotes.exe'
Remove-ItemProperty -Path $key -Name 'MUIVerb' -ErrorAction SilentlyContinue
$cmd = Join-Path $key 'command'
New-Item -Path $cmd -Force | Out-Null
Set-ItemProperty -Path $cmd -Name '(default)' -Value '"C:\cx_ProjectCode\PaperNotes\out\bin\PaperNotes.exe" "%V"'
"registered probe verb: $label"
"  shape: default only, Icon without ,0, command uses %V"

# Locate Tencent's desktop overlay window.
$script:pm = [IntPtr]::Zero
[void][T]::EnumWindows({ param($h,$l)
  if ([T]::ClassOf($h) -eq 'Progman') { $script:pm = $h; return $false }
  return $true }, [IntPtr]::Zero)
$script:tx = [IntPtr]::Zero
if ($script:pm -ne [IntPtr]::Zero) {
  [void][T]::EnumChildWindows($script:pm, { param($h,$l)
    $c = [T]::ClassOf($h)
    if ($c -eq 'TXMiniSkin' -or $c -eq 'TXMiniSkinWnd') { $script:tx = $h; return $false }
    return $true }, [IntPtr]::Zero)
}
$tx = $script:tx
"TXMiniSkin hwnd = $tx"
if ($tx -eq [IntPtr]::Zero) { "Tencent overlay not found; aborting"; exit 1 }

# Ask the overlay to raise its own context menu. WM_CONTEXTMENU bypasses
# hit-testing, so a full-screen window above the desktop does not matter.
$r = New-Object T+RECT
[void][T]::GetWindowRect($tx, [ref]$r)
$x = 400; $y = 500
$lparam = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
[T]::PostMessage($tx, 0x007B, $tx, $lparam) | Out-Null
Start-Sleep -Milliseconds 1500

# Read the menu through UI Automation.
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$auto = [System.Windows.Automation.AutomationElement]
$root = $auto::RootElement
$collected = @{}
for ($i = 0; $i -lt 14; $i++) {
  try {
    $cond = New-Object System.Windows.Automation.PropertyCondition($auto::ControlTypeProperty, [System.Windows.Automation.ControlType]::MenuItem)
    $found = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)
    foreach ($el in $found) {
      try {
        if ($el.Current.IsOffscreen) { continue }
        $n = $el.Current.Name
        if ($n) { $collected[$n] = $true }
      } catch { }
    }
  } catch { }
  Start-Sleep -Milliseconds 400
}

"---- Tencent menu items observed ----"
if ($collected.Keys.Count -eq 0) {
  "  (none captured)"
} else {
  $collected.Keys | Sort-Object | ForEach-Object { "  $_" }
}

$joined = ($collected.Keys -join '|')
"---- verdict ----"
"  WorkBuddy visible        : $($joined -match 'WorkBuddy')"
"  probe verb visible       : $($joined -match [regex]::Escape($label))"
"  PaperNotes 新建便签 visible: $($joined -match ([regex]::Escape((-join [char[]](0x65B0,0x5EFA,0x4FBF,0x7B7E)))))"

# Close the menu.
[T]::keybd_event(0x1B,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 60
[T]::keybd_event(0x1B,0,2,[UIntPtr]::Zero)