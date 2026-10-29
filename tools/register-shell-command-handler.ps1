param(
  [string]$DllPath,
  [switch]$Unregister
)

$ErrorActionPreference = 'Stop'

# Default to this clone's build output so the script works from any checkout.
if (-not $DllPath) {
  $repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
  $DllPath = Join-Path $repo 'out\bin\PaperNotes.Shell.dll'
}
[Console]::OutputEncoding = [Text.Encoding]::UTF8

# ---------------------------------------------------------------------------
# EXPLORER COMMAND HANDLER PATH  (replaces the MSIX package)
#
# A plain shell verb can name a COM object instead of a command line:
#
#   DesktopBackground\shell\<verb>
#       (Default) / MUIVerb          = label
#       ExplorerCommandHandler       = {CLSID of an IExplorerCommand}
#   CLSID\{that CLSID}\InprocServer32 = path to the DLL
#
# When ExplorerCommandHandler is present the shell calls the object's
# IExplorerCommand directly, in process, and -- on Windows 11 -- renders it in
# the MODERN first-level desktop menu.  No MSIX, no package identity, no
# developer mode and no code signing are involved.
#
# Evidence that this is the mechanism the OS itself uses: on a stock Windows 11
# desktop these HKLM verbs are registered exactly this way and appear in the
# modern first-level menu, with InprocServer32 pointing at a plain DLL:
#
#   HKLM\...\DesktopBackground\shell\.SpotlightLearnMore
#       ExplorerCommandHandler = {5C6B744C-...}  -> shell32.dll
#   HKLM\...\DesktopBackground\shell\EditStickers
#       ExplorerCommandHandler = {3C0E1E28-...}  -> themecpl.dll
#
# Everything here is per-user (HKCU), so no administrator rights are needed and
# uninstall is a complete key removal.
# ---------------------------------------------------------------------------

# Built from code points: Windows PowerShell 5.1 decodes a BOM-less UTF-8
# script as ANSI, so literal Chinese here would be corrupted before it runs.
$label = -join [char[]](0x65B0, 0x5EFA, 0x4FBF, 0x7B7E)   # 新建便签

# {3B1A6C2E-6F41-4A1C-9D3A-7E5B2C8F0A11}
# The IExplorerCommand implementation.  This is the same CLSID the MSIX manifest
# declares in com:SurrogateServer; here it is registered classically instead, so
# the object is loaded in-process and the package is unnecessary.
$clsid = '{3B1A6C2E-6F41-4A1C-9D3A-7E5B2C8F0A11}'

$clsidKey = "HKCU:\Software\Classes\CLSID\$clsid"
$inprocKey = Join-Path $clsidKey 'InprocServer32'
$verbRoots = @(
  'HKCU:\Software\Classes\DesktopBackground\shell\PaperNotes'
  'HKCU:\Software\Classes\Directory\Background\shell\PaperNotes'
)

if ($Unregister) {
  foreach ($path in @($verbRoots + $clsidKey)) {
    if (Test-Path $path) { Remove-Item $path -Recurse -Force; "removed $path" }
    else { "absent  $path" }
  }
  Add-Type @'
using System; using System.Runtime.InteropServices;
public static class ShellNotify2 {
  [DllImport("shell32.dll")] public static extern void SHChangeNotify(int e, uint f, IntPtr a, IntPtr b);
}
'@
  [ShellNotify2]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)
  exit 0
}

if (-not (Test-Path $DllPath)) { throw "Shell DLL not found: $DllPath" }

New-Item -Path $inprocKey -Force | Out-Null
Set-ItemProperty -Path $inprocKey -Name '(Default)'      -Value $DllPath
Set-ItemProperty -Path $inprocKey -Name 'ThreadingModel' -Value 'Apartment'
Set-ItemProperty -Path $clsidKey -Name '(Default)'       -Value $label

foreach ($root in $verbRoots) {
  New-Item -Path $root -Force | Out-Null
  Set-ItemProperty -Path $root -Name '(Default)'             -Value $label
  Set-ItemProperty -Path $root -Name 'MUIVerb'               -Value $label
  Set-ItemProperty -Path $root -Name 'ExplorerCommandHandler' -Value $clsid
}

Add-Type @'
using System; using System.Runtime.InteropServices;
public static class ShellNotify3 {
  [DllImport("shell32.dll")] public static extern void SHChangeNotify(int e, uint f, IntPtr a, IntPtr b);
}
'@
[ShellNotify3]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)

"---- registered ----"
"InprocServer32        : $((Get-ItemProperty $inprocKey).'(default)')"
"ThreadingModel        : $((Get-ItemProperty $inprocKey).ThreadingModel)"
foreach ($root in $verbRoots) {
  $p = Get-ItemProperty $root
  "verb                  : $root"
  "  MUIVerb             = $($p.MUIVerb)"
  "  ExplorerCommandHandler = $($p.ExplorerCommandHandler)"
  # Read the label back as bytes: a mis-encoded write would look fine in the
  # console but render as mojibake in the menu.
  $bytes = (Get-ItemProperty $root).MUIVerb.ToCharArray() | ForEach-Object { '{0:X2}' -f [int]$_ }
  "  label bytes         = $($bytes -join ' ')"
}
"OK"
