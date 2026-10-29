param(
  [string]$Executable,
  [switch]$Unregister
)

$ErrorActionPreference = 'Stop'

# Default to this clone's build output so the script works from any checkout.
if (-not $Executable) {
  $repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
  $Executable = Join-Path $repo 'out\bin\PaperNotes.exe'
}

# Per-user shell verbs.  They appear in the classic desktop context menu
# ("Show more options") and in third-party desktop replacements that enumerate
# the standard registry verbs, which is exactly what the modern MSIX
# registration cannot reach.
#
# HKCU only: no administrator rights, and removal is complete on uninstall.

# Windows PowerShell 5.1 decodes a BOM-less script as ANSI, so a literal
# "新 建 便 签" in this file would be corrupted before it ever runs.  Build the
# label from code points instead; that is encoding-proof.
$label = -join [char[]](0x65B0, 0x5EFA, 0x4FBF, 0x7B7E)

$targets = @(
  'HKCU:\Software\Classes\DesktopBackground\shell\PaperNotesNewNote',
  'HKCU:\Software\Classes\Directory\Background\shell\PaperNotesNewNote'
)

if ($Unregister) {
  foreach ($key in $targets) {
    if (Test-Path $key) {
      Remove-Item $key -Recurse -Force
      "removed $key"
    } else {
      "absent  $key"
    }
  }
  exit 0
}

if (-not (Test-Path $Executable)) { throw "Executable not found: $Executable" }

foreach ($key in $targets) {
  New-Item -Path $key -Force | Out-Null
  Set-ItemProperty -Path $key -Name '(Default)' -Value $label
  Set-ItemProperty -Path $key -Name 'MUIVerb'   -Value $label
  Set-ItemProperty -Path $key -Name 'Icon'      -Value "$Executable,0"

  $command = Join-Path $key 'command'
  New-Item -Path $command -Force | Out-Null
  Set-ItemProperty -Path $command -Name '(Default)' -Value "`"$Executable`" --new-note"
}

# Verify what was actually stored rather than trusting the write.
$ok = $true
foreach ($key in $targets) {
  $stored = (Get-ItemProperty $key).MUIVerb
  $bytes = ([Text.Encoding]::Unicode.GetBytes($stored) | ForEach-Object { $_.ToString('X2') }) -join ' '
  $match = ($stored -ceq $label)
  if (-not $match) { $ok = $false }
  "{0}`n  MUIVerb bytes : {1}`n  matches label: {2}" -f $key, $bytes, $match
  "  command        : " + (Get-ItemProperty (Join-Path $key 'command')).'(default)'
}
if (-not $ok) { throw 'Registry label round-trip failed.' }
'OK'