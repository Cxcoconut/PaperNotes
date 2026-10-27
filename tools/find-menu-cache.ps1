param(
  [Parameter(Mandatory = $true)][string]$Roots,
  [Parameter(Mandatory = $true)][string]$Needles,
  [long]$MaxBytes = 20000000
)

$ErrorActionPreference = 'Continue'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

# Invoked through -File, so array parameters arrive as one comma-joined string.
$rootList = $Roots -split '[;,]' | Where-Object { $_ -and $_.Trim() }
$needleList = $Needles -split '[;,]' | Where-Object { $_ -and $_.Trim() }

# Build byte patterns for each needle: ASCII and UTF-16LE.
$patterns = @()
foreach ($needle in $needleList) {
  $ascii = [Text.Encoding]::ASCII.GetBytes($needle)
  $utf16 = [Text.Encoding]::Unicode.GetBytes($needle)
  $patterns += [pscustomobject]@{ Needle = $needle; Kind = 'ascii'; Bytes = $ascii }
  $patterns += [pscustomobject]@{ Needle = $needle; Kind = 'utf16'; Bytes = $utf16 }
}

function Find-Pattern([byte[]]$haystack, [byte[]]$needle) {
  if ($needle.Length -eq 0 -or $haystack.Length -lt $needle.Length) { return $false }
  $limit = $haystack.Length - $needle.Length
  for ($i = 0; $i -le $limit; $i++) {
    if ($haystack[$i] -ne $needle[0]) { continue }
    $ok = $true
    for ($j = 1; $j -lt $needle.Length; $j++) {
      if ($haystack[$i + $j] -ne $needle[$j]) { $ok = $false; break }
    }
    if ($ok) { return $true }
  }
  return $false
}

$files = @()
foreach ($root in $rootList) {
  if (-not (Test-Path $root)) { Write-Output "SKIP (absent): $root"; continue }
  $files += Get-ChildItem $root -Recurse -File -Force -ErrorAction SilentlyContinue |
    Where-Object { $_.Length -gt 0 -and $_.Length -le $MaxBytes }
}
Write-Output "candidate files: $($files.Count)"

$hitCount = 0
foreach ($file in $files) {
  try { $bytes = [IO.File]::ReadAllBytes($file.FullName) } catch { continue }
  $found = @()
  foreach ($p in $patterns) {
    if (Find-Pattern $bytes $p.Bytes) { $found += "$($p.Needle)[$($p.Kind)]" }
  }
  if ($found.Count -gt 0) {
    $hitCount++
    Write-Output ("HIT  {0}  ({1} bytes)  ->  {2}" -f $file.FullName, $file.Length, ($found -join ', '))
  }
}
Write-Output "files with hits: $hitCount"