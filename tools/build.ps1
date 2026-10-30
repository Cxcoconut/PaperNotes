param(
  [ValidateSet('Build', 'Install', 'Uninstall', 'Clean')]
  [string]$Action = 'Build'
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$bin = Join-Path $root 'out\bin'
$package = Join-Path $root 'out\package'
$manifest = Join-Path $root 'packaging\AppxManifest.xml'
$packageName = 'PaperNotes.Phase0'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found.' }
$installation = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual C++ build tools not found.' }
$msbuild = Join-Path $installation 'MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path $msbuild)) { throw "MSBuild not found: $msbuild" }

function Invoke-MSBuild {
  param([string]$Project)
  & $msbuild $Project /nologo /m /p:Configuration=Release /p:Platform=x64
  if ($LASTEXITCODE -ne 0) {
    throw "MSBuild failed for $Project (exit $LASTEXITCODE)"
  }
}

function Stop-PaperNotesProcesses {
  $running = Get-Process -Name PaperNotes -ErrorAction SilentlyContinue
  if ($running) {
    # Say it out loud: the exe cannot be replaced while it runs, so a rebuild
    # stops the app -- including the copy started at logon by autostart.  Silence
    # here looks like "autostart stopped working" on the next glance at the tray.
    Write-Host "停止运行中的 PaperNotes (pid=$($running.Id -join ','))；下次登录或手动启动会恢复"
    $running | Stop-Process -Force
  }
}

function Remove-ExistingPackage {
  $pkg = Get-AppxPackage -Name $packageName -ErrorAction SilentlyContinue
  if ($pkg) {
    Remove-AppxPackage -Package $pkg.PackageFullName -ErrorAction Stop
  }
}

switch ($Action) {
  'Clean' {
    Remove-Item -Recurse -Force (Join-Path $root 'out') -ErrorAction SilentlyContinue
    break
  }
  'Build' {
    Stop-PaperNotesProcesses
    New-Item -ItemType Directory -Force $bin | Out-Null
    Invoke-MSBuild (Join-Path $root 'src\App\PaperNotesApp.vcxproj')
    Invoke-MSBuild (Join-Path $root 'src\Shell\PaperNotesShell.vcxproj')

    New-Item -ItemType Directory -Force $package | Out-Null
    Copy-Item (Join-Path $bin 'PaperNotes.exe') $package -Force
    Copy-Item (Join-Path $bin 'PaperNotes.Shell.dll') $package -Force
    Copy-Item (Join-Path $root 'packaging\Assets') $package -Recurse -Force
    Copy-Item $manifest $package -Force

    [xml]$xml = Get-Content -Raw -Encoding UTF8 $manifest
    if (-not $xml.Package.Identity.Name) { throw 'Manifest identity missing.' }
    if (-not (Test-Path (Join-Path $package 'PaperNotes.exe'))) { throw 'Packaged executable missing.' }
    if (-not (Test-Path (Join-Path $package 'PaperNotes.Shell.dll'))) { throw 'Packaged shell DLL missing.' }

    Get-Item (Join-Path $package 'PaperNotes.exe'), (Join-Path $package 'PaperNotes.Shell.dll') |
      Select-Object Name, Length, LastWriteTime
    break
  }
  'Install' {
    & $PSCommandPath -Action Build
    Stop-PaperNotesProcesses
    Remove-ExistingPackage
    Add-AppxPackage -Register (Join-Path $package 'AppxManifest.xml') -ForceApplicationShutdown
    $pkg = Get-AppxPackage -Name $packageName | Select-Object -First 1
    if (-not $pkg) { throw 'Package registration failed.' }
    "INSTALLED=$($pkg.PackageFullName)"
    "FAMILY=$($pkg.PackageFamilyName)"
    "AUMID=$($pkg.PackageFamilyName)!App"
    break
  }
  'Uninstall' {
    Stop-PaperNotesProcesses
    Remove-ExistingPackage
    "UNINSTALLED=$packageName"
    "DATA_PRESERVED=$(Join-Path $env:USERPROFILE 'PaperNotesData')"
    break
  }
}
