param(
  [Parameter(Mandatory = $true)][string]$Path,
  [string]$Patterns = '',
  [int]$MinLength = 5,
  [int]$MaxPerPattern = 40,
  [int]$MaxTotal = 20000
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

# Invoked through -File, an array parameter arrives as one comma-joined string,
# so split it back into individual patterns.
$patternList = @($Patterns -split '[;,]' | Where-Object { $_ -and $_.Trim() })

if (-not ('StringExtract' -as [type])) {
  Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;

public static class StringExtract {
    // ASCII runs, length >= min.
    public static string[] Ascii(byte[] data, int min) {
        var outList = new List<string>();
        int start = -1;
        for (int i = 0; i < data.Length; i++) {
            byte b = data[i];
            if (b >= 0x20 && b < 0x7F) {
                if (start < 0) start = i;
            } else {
                if (start >= 0 && i - start >= min) outList.Add(System.Text.Encoding.ASCII.GetString(data, start, i - start));
                start = -1;
            }
        }
        if (start >= 0 && data.Length - start >= min) outList.Add(System.Text.Encoding.ASCII.GetString(data, start, data.Length - start));
        return outList.ToArray();
    }

    // UTF-16LE runs of printable ASCII, scanned at BOTH byte alignments so a
    // string starting at an odd offset is still recovered.
    public static string[] Utf16(byte[] data, int min) {
        var outList = new List<string>();
        for (int align = 0; align < 2; align++) {
            int runStart = -1;
            int runLen = 0;
            int i = align;
            while (i + 1 < data.Length) {
                byte lo = data[i], hi = data[i + 1];
                if (hi == 0 && lo >= 0x20 && lo < 0x7F) {
                    if (runStart < 0) { runStart = i; runLen = 0; }
                    runLen++;
                } else {
                    if (runStart >= 0 && runLen >= min)
                        outList.Add(System.Text.Encoding.Unicode.GetString(data, runStart, runLen * 2));
                    runStart = -1;
                }
                i += 2;
            }
            if (runStart >= 0 && runLen >= min)
                outList.Add(System.Text.Encoding.Unicode.GetString(data, runStart, runLen * 2));
        }
        return outList.ToArray();
    }
}
'@
}

$bytes = [IO.File]::ReadAllBytes($Path)
Write-Output "FILE: $Path  bytes=$($bytes.Length)"

$ascii = [StringExtract]::Ascii($bytes, $MinLength)
$wide = [StringExtract]::Utf16($bytes, $MinLength)
$unique = @($ascii + $wide | Sort-Object -Unique)

Write-Output "strings: ascii=$($ascii.Count) utf16=$($wide.Count) unique=$($unique.Count)"

if ($patternList.Count -eq 0) {
  $unique | Select-Object -First $MaxTotal | ForEach-Object { $_ }
  exit 0
}

foreach ($pattern in $patternList) {
  Write-Output "==== /$pattern/ ===="
  $hits = @($unique | Where-Object { $_ -match $pattern } | Select-Object -First $MaxPerPattern)
  if ($hits.Count -gt 0) { $hits | ForEach-Object { "  $_" } } else { "  (none)" }
}