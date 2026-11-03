# End-to-end check for the history feature. ASCII-only so Windows PowerShell 5.1
# reads it correctly without a BOM.
#
# Notes on the environment this runs in: launched from a restricted token, the
# tray icon cannot be created, so the app opens the list window by itself at
# startup. The hotkey therefore *toggles* the list, which is why the helpers here
# keep pressing it until the window is actually visible instead of assuming one
# press shows it.
$ErrorActionPreference = 'Stop'

Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class UI {
  public delegate bool P(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(P cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, P cb, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr dlg, int id);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll", EntryPoint="SendMessageW")] public static extern IntPtr Send(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SendStr(IntPtr h, uint m, IntPtr w, StringBuilder l);
  [DllImport("user32.dll", EntryPoint="SendMessageW", CharSet=CharSet.Unicode)] public static extern IntPtr SendText(IntPtr h, uint m, IntPtr w, string l);
  [DllImport("user32.dll", EntryPoint="PostMessageW")] public static extern bool Post(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  public static IntPtr FindPid(string cls, uint pid){ IntPtr r=IntPtr.Zero;
    EnumWindows((h,l)=>{ var c=new StringBuilder(128); GetClassNameW(h,c,128);
      uint q; GetWindowThreadProcessId(h,out q);
      if(c.ToString()==cls && q==pid){ r=h; return false; } return true; }, IntPtr.Zero); return r; }
  public static IntPtr Child(IntPtr p,string cls){ IntPtr r=IntPtr.Zero;
    EnumChildWindows(p,(h,l)=>{ var c=new StringBuilder(64); GetClassNameW(h,c,64);
      if(c.ToString()==cls){ r=h; return false; } return true; }, IntPtr.Zero); return r; }
  public static void Fill(IntPtr n,string t){ IntPtr e=Child(n,"Edit");
    SendText(e,0x000C,IntPtr.Zero,t); Post(n,0x0111,(IntPtr)(0x0300<<16),e); }
  public static int Count(IntPtr lb){ return (int)Send(lb,0x018B,IntPtr.Zero,IntPtr.Zero); }
  public static string Item(IntPtr lb,int i){ int len=(int)Send(lb,0x018A,(IntPtr)i,IntPtr.Zero);
    var sb=new StringBuilder(len+2); SendStr(lb,0x0189,(IntPtr)i,sb); return sb.ToString(); }
  public static string Title(IntPtr h){ var sb=new StringBuilder(256); GetWindowTextW(h,sb,256); return sb.ToString(); }
  public static string ClickYes(uint pid){ var dlg=FindPid("#32770",pid);
    if(dlg==IntPtr.Zero) return "no-dialog";
    var yes=GetDlgItem(dlg,6); if(yes==IntPtr.Zero) return "no-yes";
    Send(yes,0x00F5,IntPtr.Zero,IntPtr.Zero); return "yes"; }
}
'@

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$root = Join-Path $repo 'out\bin'
$exe = Join-Path $root 'PaperNotes.exe'
$data = Join-Path $root 'PaperNotesData'
$notesDir = Join-Path $data 'notes'
$histDir = Join-Path $data 'history'

function Count-Files($dir) {
  if (-not (Test-Path $dir)) { return 0 }
  return @(Get-ChildItem $dir -Filter *.note -File -ErrorAction SilentlyContinue).Count
}

function Start-App {
  # Start-Process, not Process.Start: the latter hands the app our stdout handle
  # and the calling tool then waits for it forever.
  $p = Start-Process -FilePath $exe -PassThru
  Start-Sleep -Seconds 3
  return $p
}

function Show-List($proc) {
  $id = [uint32]$proc.Id
  $hidden = [UI]::FindPid('PaperNotes.HiddenWindow.v1', $id)
  if ($hidden -eq [IntPtr]::Zero) { throw 'hidden window missing' }

  # When the tray is unavailable the app shows the list at startup, so it can
  # already be visible and stale. Hide it first: the next toggle then runs the
  # show path, which is what refreshes the contents.
  $list = [UI]::FindPid('PaperNotes.ListWindow.v1', $id)
  if ($list -ne [IntPtr]::Zero -and [UI]::IsWindowVisible($list)) {
    [void][UI]::Post($hidden, 0x0312, [IntPtr]2, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 700
  }

  for ($i = 0; $i -lt 6; $i++) {
    [void][UI]::Post($hidden, 0x0312, [IntPtr]2, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 700
    $list = [UI]::FindPid('PaperNotes.ListWindow.v1', $id)
    if ($list -ne [IntPtr]::Zero -and [UI]::IsWindowVisible($list)) { return $list }
  }
  throw 'list window never became visible'
}

$failures = 0
function Check($name, $condition) {
  if ($condition) { Write-Host "PASS  $name" }
  else { Write-Host "FAIL  $name"; $script:failures++ }
}

# --- clean slate --------------------------------------------------------------
Get-Process PaperNotes -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1
Remove-Item $notesDir, $histDir -Recurse -Force -ErrorAction SilentlyContinue

$proc = Start-App
Write-Host "app pid=$($proc.Id)"

# --- create a note and let it save -------------------------------------------
$id = [uint32]$proc.Id
$hidden = [UI]::FindPid('PaperNotes.HiddenWindow.v1', $id)
[void][UI]::Post($hidden, 0x8002, [IntPtr]::Zero, [IntPtr]::Zero)
Start-Sleep -Seconds 2
$note = [UI]::FindPid('PaperNotes.NoteWindow.v1', $id)
[UI]::Fill($note, 'history test line one')
Start-Sleep -Seconds 2
Check 'note written to disk' ((Count-Files $notesDir) -eq 1)

# --- delete through the list window ------------------------------------------
$list = Show-List $proc
$lb = [UI]::Child($list, 'ListBox')
Check 'list shows one note' (([UI]::Count($lb)) -eq 1)
[void][UI]::Send($lb, 0x0186, [IntPtr]0, [IntPtr]::Zero)   # LB_SETCURSEL
[void][UI]::Post($list, 0x0111, [IntPtr]103, [IntPtr]::Zero)
Start-Sleep -Milliseconds 1200
Check 'confirm dialog appeared' (([UI]::ClickYes($id)) -eq 'yes')
Start-Sleep -Seconds 1
Check 'note file left the notes folder' ((Count-Files $notesDir) -eq 0)
Check 'history holds one entry' ((Count-Files $histDir) -eq 1)

# --- history survives a restart ----------------------------------------------
Get-Process PaperNotes -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 2
$proc = Start-App
$id = [uint32]$proc.Id
Write-Host "restarted pid=$id"
$list = Show-List $proc
[void][UI]::Post($list, 0x0111, [IntPtr]106, [IntPtr]::Zero)   # open history
Start-Sleep -Milliseconds 1200
$hist = [UI]::FindPid('PaperNotes.HistoryWindow.v1', $id)
Check 'history window opened' ($hist -ne [IntPtr]::Zero)
$hlb = [UI]::Child($hist, 'ListBox')
Check 'history still has the entry after restart' (([UI]::Count($hlb)) -eq 1)
if (([UI]::Count($hlb)) -gt 0) { Write-Host "      label: $([UI]::Item($hlb, 0))" }

# --- restore puts it back -----------------------------------------------------
[void][UI]::Send($hlb, 0x0186, [IntPtr]0, [IntPtr]::Zero)
[void][UI]::Post($hist, 0x0111, [IntPtr]301, [IntPtr]::Zero)   # restore
Start-Sleep -Seconds 2
Check 'note file is back' ((Count-Files $notesDir) -eq 1)
Check 'history emptied by restore' ((Count-Files $histDir) -eq 0)
Check 'note window recreated' (([UI]::FindPid('PaperNotes.NoteWindow.v1', $id)) -ne [IntPtr]::Zero)

# --- an empty note is destroyed outright -------------------------------------
$notesBefore = Count-Files $notesDir
$hidden = [UI]::FindPid('PaperNotes.HiddenWindow.v1', $id)
[void][UI]::Post($hidden, 0x8002, [IntPtr]::Zero, [IntPtr]::Zero)   # new, left empty
Start-Sleep -Seconds 2
$list = Show-List $proc
$lb = [UI]::Child($list, 'ListBox')
$last = [UI]::Count($lb) - 1
[void][UI]::Send($lb, 0x0186, [IntPtr]$last, [IntPtr]::Zero)
[void][UI]::Post($list, 0x0111, [IntPtr]103, [IntPtr]::Zero)
Start-Sleep -Milliseconds 1200
[void][UI]::ClickYes($id)
Start-Sleep -Seconds 1
Check 'empty note leaves no history entry' ((Count-Files $histDir) -eq 0)
Check 'empty note leaves no note file' ((Count-Files $notesDir) -eq $notesBefore)

# --- clearing the history destroys every entry -------------------------------
foreach ($text in 'first to be cleared', 'second to be cleared') {
  $hidden = [UI]::FindPid('PaperNotes.HiddenWindow.v1', $id)
  [void][UI]::Post($hidden, 0x8002, [IntPtr]::Zero, [IntPtr]::Zero)
  Start-Sleep -Seconds 2
  $note = [UI]::FindPid('PaperNotes.NoteWindow.v1', $id)
  [UI]::Fill($note, $text)
  Start-Sleep -Seconds 2
  $list = Show-List $proc
  $lb = [UI]::Child($list, 'ListBox')
  $last = [UI]::Count($lb) - 1
  [void][UI]::Send($lb, 0x0186, [IntPtr]$last, [IntPtr]::Zero)
  [void][UI]::Post($list, 0x0111, [IntPtr]103, [IntPtr]::Zero)
  Start-Sleep -Milliseconds 1200
  [void][UI]::ClickYes($id)
  Start-Sleep -Seconds 1
}
Check 'two entries accumulated' ((Count-Files $histDir) -eq 2)

$list = Show-List $proc
[void][UI]::Post($list, 0x0111, [IntPtr]106, [IntPtr]::Zero)
Start-Sleep -Milliseconds 1200
$hist = [UI]::FindPid('PaperNotes.HistoryWindow.v1', $id)
[void][UI]::Post($hist, 0x0111, [IntPtr]303, [IntPtr]::Zero)   # clear history
Start-Sleep -Milliseconds 1200
[void][UI]::ClickYes($id)
Start-Sleep -Seconds 1
Check 'clear history removes every entry' ((Count-Files $histDir) -eq 0)

Write-Host ""
if ($failures -eq 0) { Write-Host 'ALL CHECKS PASSED' } else { Write-Host "$failures CHECK(S) FAILED" }
Get-Process PaperNotes -ErrorAction SilentlyContinue | Stop-Process -Force
exit $failures
