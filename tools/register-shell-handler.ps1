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

# Classic in-process context menu handler.
#
# Two independent shell entry points are needed:
#   shell\<verb>            static verb  -> Windows 10 menu, Win11 "Show more options"
#   shellex\ContextMenuHandlers + CLSID   -> classic handler path, which desktop
#                                           replacements enumerate via
#                                           IShellExtInit/IContextMenu
#
# Everything is per-user (HKCU), so no administrator rights are required and
# uninstall is a complete key removal.

# Built from code points: Windows PowerShell 5.1 decodes a BOM-less UTF-8
# script as ANSI, so literal Chinese here would be corrupted before it runs.
$label = -join [char[]](0x65B0, 0x5EFA, 0x4FBF, 0x7B7E)          # 新建便签
$clsid = '{3B1A6C2E-6F41-4A1C-9D3A-7E5B2C8F0A12}'

$clsidKey = "HKCU:\Software\Classes\CLSID\$clsid"
$inprocKey = Join-Path $clsidKey 'InprocServer32'
$handlerRoots = @(
  'HKCU:\Software\Classes\DesktopBackground\shellex\ContextMenuHandlers\PaperNotes'
  'HKCU:\Software\Classes\Directory\Background\shellex\ContextMenuHandlers\PaperNotes'
)

if ($Unregister) {
  foreach ($path in @($handlerRoots + $clsidKey)) {
    if (Test-Path $path) { Remove-Item $path -Recurse -Force; "removed $path" }
    else { "absent  $path" }
  }
  exit 0
}

if (-not (Test-Path $DllPath)) { throw "Shell DLL not found: $DllPath" }

New-Item -Path $inprocKey -Force | Out-Null
Set-ItemProperty -Path $inprocKey -Name '(Default)'      -Value $DllPath
Set-ItemProperty -Path $inprocKey -Name 'ThreadingModel' -Value 'Apartment'
Set-ItemProperty -Path $clsidKey -Name '(Default)'       -Value $label

foreach ($root in $handlerRoots) {
  New-Item -Path $root -Force | Out-Null
  Set-ItemProperty -Path $root -Name '(Default)' -Value $clsid
}

# Notify the shell so a running Explorer drops its cached handler list.
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class ShellNotify {
  [DllImport("shell32.dll")] public static extern void SHChangeNotify(int e, uint f, IntPtr a, IntPtr b);
}
'@
[ShellNotify]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)

"---- registered ----"
"InprocServer32 : $((Get-ItemProperty $inprocKey).'(default)')"
"ThreadingModel : $((Get-ItemProperty $inprocKey).ThreadingModel)"
foreach ($root in $handlerRoots) {
  "handler        : $root = $((Get-ItemProperty $root).'(default)')"
}
"OK"