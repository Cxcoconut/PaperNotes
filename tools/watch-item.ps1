param(
  [int]$Seconds = 22,
  [int]$IntervalMs = 600
)

$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes

$auto = [System.Windows.Automation.AutomationElement]
$root = $auto::RootElement

function Find-ByName([string]$name) {
  try {
    $cond = New-Object System.Windows.Automation.PropertyCondition($auto::NameProperty, $name)
    return $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond)
  } catch {
    return $null
  }
}

$menuSeenAt = $null
$itemSeenAt = $null
$menuGone = $false
$deadline = (Get-Date).AddSeconds($Seconds)
$start = Get-Date
while ((Get-Date) -lt $deadline) {
  $elapsed = [int]((Get-Date) - $start).TotalMilliseconds
  $more = Find-ByName '显示更多选项'
  $item = Find-ByName '新建便签'

  if ($more -and -not $menuSeenAt) {
    $menuSeenAt = $elapsed
    Write-Output "t=${elapsed}ms  MENU_OPEN (found '显示更多选项')"
  }
  if ($item -and -not $itemSeenAt) {
    $itemSeenAt = $elapsed
    try {
      Write-Output "t=${elapsed}ms  >>> '新建便签' PRESENT  (enabled=$($item.Current.IsEnabled) offscreen=$($item.Current.IsOffscreen) class=$($item.Current.ClassName))"
    } catch {
      Write-Output "t=${elapsed}ms  >>> '新建便签' PRESENT"
    }
  }
  if (-not $more -and $menuSeenAt -and -not $itemSeenAt -and -not $menuGone) {
    $menuGone = $true
    Write-Output "t=${elapsed}ms  menu closed without '新建便签'"
    break
  }
  Start-Sleep -Milliseconds $IntervalMs
}

Write-Output "---- SUMMARY ----"
if ($menuSeenAt) { Write-Output "menu_open_at_ms   = $menuSeenAt" } else { Write-Output "menu_open_at_ms   = NEVER (no menu detected)" }
if ($itemSeenAt) { Write-Output "item_present_at_ms= $itemSeenAt" } else { Write-Output "item_present_at_ms= NEVER" }