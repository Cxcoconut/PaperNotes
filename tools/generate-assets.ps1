param(
  [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\packaging\Assets')
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function New-PaperNotesLogo {
  param([int]$Size, [string]$Path)

  $bmp = New-Object System.Drawing.Bitmap($Size, $Size)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
  $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias
  $g.Clear([System.Drawing.Color]::Transparent)

  $pad = [Math]::Max(2, [int]($Size * 0.11))
  $shadowOffset = [Math]::Max(1, [int]($Size * 0.035))
  $body = New-Object System.Drawing.Rectangle(
    $pad, ($pad + $shadowOffset),
    ($Size - 2 * $pad), ($Size - 2 * $pad - $shadowOffset))
  $fold = [Math]::Max(4, [int]($Size * 0.22))

  $shadow = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(55, 50, 40, 20))
  $g.FillRectangle($shadow, $pad + $shadowOffset, $pad + 2 * $shadowOffset, $body.Width, $body.Height)
  $shadow.Dispose()

  $paper = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 255, 249, 221))
  $edge = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 204, 182, 128), [Math]::Max(1, $Size / 48))
  $points = @(
    (New-Object System.Drawing.PointF($body.Left, $body.Top)),
    (New-Object System.Drawing.PointF(($body.Right - $fold), $body.Top)),
    (New-Object System.Drawing.PointF($body.Right, ($body.Top + $fold))),
    (New-Object System.Drawing.PointF($body.Right, $body.Bottom)),
    (New-Object System.Drawing.PointF($body.Left, $body.Bottom))
  )
  $g.FillPolygon($paper, $points)
  $g.DrawPolygon($edge, $points)

  $foldBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 245, 224, 164))
  $foldPoints = @(
    (New-Object System.Drawing.PointF(($body.Right - $fold), $body.Top)),
    (New-Object System.Drawing.PointF(($body.Right - $fold), ($body.Top + $fold))),
    (New-Object System.Drawing.PointF($body.Right, ($body.Top + $fold)))
  )
  $g.FillPolygon($foldBrush, $foldPoints)
  $g.DrawLine($edge, $body.Right - $fold, $body.Top, $body.Right - $fold, $body.Top + $fold)
  $g.DrawLine($edge, $body.Right - $fold, $body.Top + $fold, $body.Right, $body.Top + $fold)

  $ink = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 132, 108, 62), [Math]::Max(1, $Size / 64))
  $left = $body.Left + $body.Width * 0.18
  $right = $body.Right - $body.Width * 0.18
  for ($i = 0; $i -lt 4; $i++) {
    $y = $body.Top + $body.Height * (0.33 + 0.14 * $i)
    $lineRight = $right - (($i % 2) * $body.Width * 0.14)
    $g.DrawLine($ink, $left, $y, $lineRight, $y)
  }

  $accent = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 230, 126, 63), [Math]::Max(2, $Size / 24))
  $g.DrawLine($accent, $body.Left + $body.Width * 0.18, $body.Top + $body.Height * 0.18,
    $body.Left + $body.Width * 0.42, $body.Top + $body.Height * 0.18)

  $ink.Dispose(); $accent.Dispose(); $edge.Dispose(); $paper.Dispose(); $foldBrush.Dispose()
  $g.Dispose()
  $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
  $bmp.Dispose()
}

New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
$assets = @(
  @{ Size = 44;  Name = 'Square44x44Logo.png' },
  @{ Size = 150; Name = 'Square150x150Logo.png' },
  @{ Size = 310; Name = 'Square310x310Logo.png' },
  @{ Size = 50;  Name = 'StoreLogo.png' }
)
foreach ($asset in $assets) {
  New-PaperNotesLogo -Size $asset.Size -Path (Join-Path $OutputDirectory $asset.Name)
}

Get-ChildItem $OutputDirectory | Select-Object Name, Length
