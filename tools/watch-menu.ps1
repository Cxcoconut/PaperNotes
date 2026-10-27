param(
  [int]$Seconds = 20,
  [int]$IntervalMs = 1000
)

$ErrorActionPreference = 'Continue'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes

$auto = [System.Windows.Automation.AutomationElement]
$root = $auto::RootElement

function Get-MenuItems {
  $items = @()
  $cond = New-Object System.Windows.Automation.PropertyCondition(
    $auto::ControlTypeProperty, [System.Windows.Automation.ControlType]::MenuItem)
  $found = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond)
  foreach ($el in $found) {
    try {
      if ($el.Current.IsOffscreen) { continue }
      $name = $el.Current.Name
      $items += $name
    } catch { }
  }
  return $items
}

$deadline = (Get-Date).AddSeconds($Seconds)
$lastSignature = ''
$samples = 0
while ((Get-Date) -lt $deadline) {
  $items = Get-MenuItems
  if ($items.Count -gt 0) {
    $signature = ($items -join '|')
    $stamp = Get-Date -Format 'HH:mm:ss'
    if ($signature -ne $lastSignature) {
      $samples++
      Write-Output "[$stamp] MENU ($($items.Count) items):"
      foreach ($item in $items) { Write-Output "    - $item" }
      $has = $items | Where-Object { $_ -like '*新建便签*' }
      if ($has) { Write-Output "    >>> 新建便签 PRESENT" } else { Write-Output "    >>> 新建便签 absent" }
      $lastSignature = $signature
    }
  } elseif ($lastSignature -ne '') {
    Write-Output "[$(Get-Date -Format 'HH:mm:ss')] menu closed"
    $lastSignature = ''
  }
  Start-Sleep -Milliseconds $IntervalMs
}
Write-Output "DONE samples=$samples"