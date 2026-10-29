param(
  [string]$ExePath,
  [switch]$Unregister
)

$ErrorActionPreference = 'Stop'

# Default to this clone's build output so the script works from any checkout.
if (-not $ExePath) {
  $repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
  $ExePath = Join-Path $repo 'out\bin\PaperNotes.exe'
}
[Console]::OutputEncoding = [Text.Encoding]::UTF8

# Per-user autostart.
#
# HKCU (not HKLM) on purpose: it needs no administrator rights, it is what the
# installer can always write, and it matches the per-user COM registration the
# rest of the product uses.  Uninstall is a single value removal.
#
# --autostart makes the app restore notes hidden and skip the fallback list
# window, so signing in never opens anything on screen.
#
# NOTE: a Run value cannot be quoted carelessly.  The whole command including
# the exe path must be quoted, otherwise "C:\Program Files\..." breaks at the
# space and Windows tries to launch "C:\Program".

$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$valueName = 'PaperNotes'

if ($Unregister) {
  if (Test-Path $runKey) {
    $existing = (Get-ItemProperty -Path $runKey -Name $valueName -ErrorAction SilentlyContinue)
    if ($existing) {
      Remove-ItemProperty -Path $runKey -Name $valueName -Force
      "removed Run value '$valueName' (was: $($existing.$valueName))"
    } else {
      "absent  Run value '$valueName'"
    }
  } else {
    "absent  $runKey"
  }
  exit 0
}

if (-not (Test-Path $ExePath)) { throw "Executable not found: $ExePath" }

$command = '"{0}" --autostart' -f $ExePath

if (-not (Test-Path $runKey)) { New-Item -Path $runKey -Force | Out-Null }
Set-ItemProperty -Path $runKey -Name $valueName -Value $command
Set-ItemProperty -Path $runKey -Name $valueName -Value $command

# Read back: a silent encoding or quoting mistake here only shows up at the
# next sign-in, which is the worst possible time to discover it.
$back = (Get-ItemProperty -Path $runKey -Name $valueName).$valueName
"---- registered ----"
"Run value : $valueName = $back"
if ($back -ne $command) { throw "readback mismatch: '$back' != '$command'" }

# The quoted path must survive a command-line split, and the switch must parse.
$split = $back -split '\s+(?=-)'
if ($split.Count -lt 2) { throw "cannot split path from switch: $back" }
$quotedPath = $split[0].Trim('"')
if (-not (Test-Path $quotedPath)) { throw "split path does not exist: $quotedPath" }
"path      : $quotedPath  (exists)"
"switch    : $($split[1])"
"OK"
