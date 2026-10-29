$ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class PN {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string cls, string title);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, string l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, StringBuilder l);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  public const uint WM_SETTEXT = 0x000C;
  public const uint WM_GETTEXT = 0x000D;
  public const uint WM_APP_NEWNOTE = 0x8002;
  public static string GetText(IntPtr h) { var sb = new StringBuilder(1024); SendMessage(h, WM_GETTEXT, (IntPtr)1024, sb); return sb.ToString(); }
}
'@

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$exe = Join-Path $repo 'out\bin\PaperNotes.exe'
$log = Join-Path $repo 'out\diagnostics\phase0.log'

function Start-App {
  $sh = New-Object -ComObject Shell.Application
  $sh.ShellExecute($exe, '', '', 'open', 1)
  Start-Sleep -Seconds 3
}

Get-Process PaperNotes -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 700
if (Test-Path $log) { Remove-Item $log -Force }

'==== 1. start ===='
Start-App
Get-Content $log | Select-String -Pattern 'restore:|writable' | Select-Object -Last 5

'==== 2. simulate shell handler posting new-note ===='
$hidden = [PN]::FindWindow('PaperNotes.HiddenWindow.v1', [NullString]::Value)
"hidden hwnd = $hidden"
[PN]::PostMessage($hidden, [PN]::WM_APP_NEWNOTE, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 900
$note = [PN]::FindWindow('PaperNotes.NoteWindow.v1', [NullString]::Value)
"note hwnd = $note visible=$([PN]::IsWindowVisible($note))"

'==== 3. type two lines ===='
$edit = [PN]::FindWindowEx($note, [IntPtr]::Zero, 'Edit', [NullString]::Value)
"edit hwnd = $edit"
$line1 = -join [char[]](0x7B2C, 0x4E00, 0x884C)
$line2 = -join [char[]](0x7B2C, 0x4E8C, 0x884C)
$text = $line1 + "`r`n" + $line2
[PN]::SendMessage($edit, [PN]::WM_SETTEXT, [IntPtr]::Zero, $text) | Out-Null
# Cross-process WM_SETTEXT does not raise EN_CHANGE on the parent, so deliver the
# exact notification the EDIT control sends while the user types.
[PN]::PostMessage($note, 0x0111, [IntPtr]0x03000000, $edit) | Out-Null
Start-Sleep -Seconds 2

'==== 4. disk check ===='
$storeRoots = @("$env:LOCALAPPDATA\PaperNotes", "$env:USERPROFILE\PaperNotesData", "$env:TEMP\PaperNotes", (Join-Path $repo 'out\bin\PaperNotesData'))
$foundStore = $null
foreach ($root in $storeRoots) {
  $notesDir = Join-Path $root 'notes'
  if (Test-Path $notesDir) {
    $files = Get-ChildItem $notesDir -Filter *.note -ErrorAction SilentlyContinue
    if ($files) {
      $foundStore = $root
      "STORE = $root"
      foreach ($f in $files) {
        "  file: $($f.Name)  $($f.Length) bytes"
        $raw = Get-Content $f.FullName -Raw -Encoding UTF8
        ($raw -split "`n" | Select-Object -First 8) | ForEach-Object { "    | $_" }
      }
    }
  }
}
if (-not $foundStore) { 'NO STORE FILE FOUND' }

'==== 5. kill app (simulate crash) ===='
Get-Process PaperNotes -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1

'==== 6. restart and check restore ===='
Start-App
Start-Sleep -Seconds 1
Get-Content $log | Select-String -Pattern 'restore:' | Select-Object -Last 2
$note2 = [PN]::FindWindow('PaperNotes.NoteWindow.v1', [NullString]::Value)
"restored hwnd = $note2 visible=$([PN]::IsWindowVisible($note2))  (expect False)"
$edit2 = [PN]::FindWindowEx($note2, [IntPtr]::Zero, 'Edit', [NullString]::Value)
"restored text = '$([PN]::GetText($edit2))'"


