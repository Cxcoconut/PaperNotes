# 纸笺便签 · 安装程序
#
# 普通安装方式：提权后安装文件、自签名证书、MSIX 包、经典菜单处理器与自启。
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\install.ps1 -Action Install
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\install.ps1 -Action Uninstall
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\install.ps1 -Action Install -DryRun
#
# 为什么必须提权（见 README §4.9）：
#   Win11 现代第一层菜单只能由 MSIX 提供，而 AppX 部署服务只信任**机器级**证书存储。
#   自签名证书必须由管理员写入 LocalMachine\Root，否则 Add-AppxPackage 报
#   0x800B0109「根证书必须是受信任的证书」。用户已确认可接受管理员权限。
#
# 为什么 DLL 用带版本号的文件名：
#   Shell 扩展是被 Explorer / 桌面接管程序**进程内**加载的，文件被锁住，
#   同名覆盖会失败（LNK1104）。版本号化让升级变成"换一个新文件"。

param(
  [ValidateSet('Install', 'Uninstall')]
  [string]$Action = 'Install',

  # 留空表示"询问用户"：安装时在提权窗口里选目录（默认 C:\Program Files\PaperNotes），
  # 卸载时留空则从 HKLM 状态里读回上次的安装位置。脚本化安装可直接传入固定路径。
  [string]$InstallDir = '',

  # 打包用的暂存目录，装完即删。MSIX 里 DLL 必须叫 PaperNotes.Shell.dll（清单写死）。
  [string]$StagingDir = (Join-Path $env:TEMP 'PaperNotes-Package'),

  # 空表示从 AppxManifest.xml 读取。默认值与清单分开写会漂移：
  # 曾经清单升到 0.1.0.1 而这里还停在 0.1.0.0，DLL 文件名和包版本就对不上了。
  [string]$Version = '',

  # 证书口令：仅用于本机 PFX 导出，不外传。生产分发应改用公开 CA 证书。
  [string]$PfxPassword = 'PaperNotesDev',

  [switch]$SkipAutostart,   # 安装时不配自启（默认开启，符合需求）
  [switch]$DryRun           # 只打印计划，不改系统
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$bin = Join-Path $repo 'out\bin'
$stateKey = 'HKLM:\Software\PaperNotes'
$publisher = 'CN=PaperNotesDev'
$clsidClassic = '{3B1A6C2E-6F41-4A1C-9D3A-7E5B2C8F0A12}'
$runKey = 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Run'
$runValue = 'PaperNotes'

# Take the version from the manifest so the DLL file name, the package version
# and the uninstall state can never disagree.
if (-not $Version) {
  $manifestPath = Join-Path $repo 'packaging\AppxManifest.xml'
  $manifestText = Get-Content $manifestPath -Raw -Encoding UTF8
  if ($manifestText -match 'Identity[\s\S]*?Version="([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)"') {
    $Version = $Matches[1]
  } else {
    throw "无法从 $manifestPath 读取 Identity/Version"
  }
}
$dllName = "PaperNotes.Shell.$Version.dll"

function Say($msg) { Write-Host $msg }
function Step($msg) { Write-Host ""; Write-Host "== $msg" }

function Assert-Admin {
  $id = [Security.Principal.WindowsIdentity]::GetCurrent()
  $principal = New-Object Security.Principal.WindowsPrincipal($id)
  if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Say "elevated: $($id.Name)"
    return $true
  }
  return $false
}

# 未提权时用 UAC 重新启动自己，参数原样透传。
function Restart-Elevated {
  $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $PSCommandPath,
               '-Action', $Action,
               '-StagingDir', "`"$StagingDir`"", '-Version', $Version,
               '-PfxPassword', $PfxPassword)
  # 空目录意味着"让用户选"，透传一个空串会把询问逻辑弄乱，所以干脆不带这个参数。
  if ($InstallDir) { $argList += @('-InstallDir', "`"$InstallDir`"") }
  if ($SkipAutostart) { $argList += '-SkipAutostart' }
  if ($DryRun) { $argList += '-DryRun' }
  Say "正在请求管理员权限…"
  Start-Process -FilePath 'powershell.exe' -ArgumentList $argList -Verb RunAs | Out-Null
  Say "已在新窗口中继续，请按提示操作。"
}

function Find-SdkTool([string]$name) {
  $root = 'C:\Program Files (x86)\Windows Kits\10\bin'
  # @() is essential: with a single SDK version the pipeline yields one string,
  # and $path[0] would then index into CHARACTERS and return "C".  That bug threw
  # "无法将 C 项识别为 cmdlet" from & $makeappx, and only surfaced when elevated.
  $candidates = @(Get-ChildItem $root -Directory -ErrorAction SilentlyContinue |
    Sort-Object Name -Descending |
    ForEach-Object { Join-Path $_.FullName "x64\$name" } |
    Where-Object { Test-Path $_ })
  if ($candidates.Count -eq 0) { throw "找不到 $name（Windows SDK 10 未安装？）" }
  return $candidates[0]
}

# ---------------------------------------------------------------- 安装

# ------------------------------------------------------- 安装位置选择

# 展开环境变量、去掉引号与尾部分隔符，返回绝对路径。
# 相对路径直接拒绝：提权窗口的工作目录未必是用户以为的那个。
function Resolve-InstallDir([string]$path) {
  if ([string]::IsNullOrWhiteSpace($path)) { throw '安装位置不能为空。' }
  $expanded = [Environment]::ExpandEnvironmentVariables($path.Trim().Trim('"').Trim("'"))
  if (-not [System.IO.Path]::IsPathRooted($expanded)) {
    throw "请填写完整路径（例如 D:\Apps\PaperNotes），不能是相对路径：$expanded"
  }
  return [System.IO.Path]::GetFullPath($expanded).TrimEnd('\')
}

function Assert-InstallDir([string]$path, [switch]$NoCreate) {
  if ($path -match '^[A-Za-z]:$') { throw "不能直接装到盘符根目录：$path" }

  $windows = $env:SystemRoot
  if ($path -eq $windows -or
      $path.StartsWith($windows + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "不能装到 Windows 目录里：$path"
  }

  if (Test-Path -LiteralPath $path -PathType Leaf) {
    throw "这个路径是一个文件，不是目录：$path"
  }

  # 已存在但不像我们的安装目录时停下，别把文件混进用户已有的文件夹。
  if (Test-Path -LiteralPath $path) {
    $looksLikeOurs = Test-Path -LiteralPath (Join-Path $path 'PaperNotes.exe')
    if (-not $looksLikeOurs) {
      $existing = @(Get-ChildItem -LiteralPath $path -Force -ErrorAction SilentlyContinue)
      if ($existing.Count -gt 0) {
        throw ("该目录已有其他内容，且不是 PaperNotes 的安装目录：$path" +
               "`n请换一个空目录或新建目录。")
      }
    }
  }

  # DryRun 只打印计划，不该在磁盘上留下任何东西。
  if ($NoCreate) { return }

  New-Item -ItemType Directory -Force -Path $path | Out-Null

  # 真写一个文件才算数：只读盘、组策略、受控文件夹访问都会在这里暴露。
  $probe = Join-Path $path '.papernotes-write-test'
  try {
    Set-Content -LiteralPath $probe -Value 'ok' -ErrorAction Stop
    Remove-Item -LiteralPath $probe -Force -ErrorAction Stop
  } catch {
    throw "这个目录无法写入：$path`n$($_.Exception.Message)"
  }
}

# 系统的文件夹选择框。PowerShell 5.1 控制台默认 STA，可以直接弹；
# 弹不出来（无桌面会话）就退回手工输入，不让安装卡住。
function Browse-ForFolder([string]$initial) {
  try {
    Add-Type -AssemblyName System.Windows.Forms -ErrorAction Stop
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = '选择 PaperNotes 的安装位置'
    $dialog.ShowNewFolderButton = $true
    if (Test-Path -LiteralPath $initial) { $dialog.SelectedPath = $initial }
    if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
      return $dialog.SelectedPath
    }
    return $null
  } catch {
    Say "  （无法打开文件夹选择框：$($_.Exception.Message)）"
    return $null
  }
}

function Select-InstallDir([string]$default) {
  Say ''
  Say '请选择安装位置'
  Say "  直接回车 = 使用默认：$default"
  Say '  输入完整路径 = 装到该目录'
  Say '  输入 b = 打开文件夹选择框'
  while ($true) {
    $answer = Read-Host '安装位置'
    if ([string]::IsNullOrWhiteSpace($answer)) { return $default }
    if ($answer.Trim() -in @('b', 'B')) {
      $picked = Browse-ForFolder $default
      if ($picked) { return $picked }
      Say '  已取消选择，请重新输入。'
      continue
    }
    return $answer
  }
}

function Install-Files {
  Step "安装文件 -> $InstallDir"
  if ($DryRun) { Say "  [dry] 先停止运行中的 PaperNotes，再复制 PaperNotes.exe 与 $dllName"; return }

  # Must stop first: reinstalling over a running instance fails with
  # "file is in use" on PaperNotes.exe.  The first upgrade only worked because
  # the process happened to be stopped already.
  $running = Get-Process PaperNotes -ErrorAction SilentlyContinue
  if ($running) {
    Say "  停止运行中的 PaperNotes (pid=$($running.Id -join ','))"
    $running | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 900
    $still = Get-Process PaperNotes -ErrorAction SilentlyContinue
    if ($still) {
      throw "无法停止 PaperNotes（可能以管理员身份运行）：请手动结束进程后重试"
    }
  }

  New-Item -ItemType Directory -Force $InstallDir | Out-Null

  # Copy only what actually differs.  Blindly overwriting the shell DLL fails
  # whenever Explorer still has it loaded -- even when the new file is byte for
  # byte the one already there, which is exactly how an install died here after
  # a rebuild that only touched the exe.
  function Copy-IfChanged([string]$source, [string]$destination, [string]$label) {
    if (Test-Path $destination) {
      $same = (Get-FileHash $source -Algorithm SHA256).Hash -eq
              (Get-FileHash $destination -Algorithm SHA256).Hash
      if ($same) {
        Say "  $label 未变化，跳过"
        return
      }
    }
    try {
      Copy-Item $source $destination -Force -ErrorAction Stop
      Say "  $label"
    } catch {
      throw ("复制 $label 失败：$($_.Exception.Message)`n" +
             "该文件仍被占用。若内容确实变了，请提升 packaging\AppxManifest.xml 的版本号" +
             "（DLL 文件名带版本号，新版本会写成新文件，不会碰到被加载的旧文件）。")
    }
  }

  Copy-IfChanged (Join-Path $bin 'PaperNotes.exe') (Join-Path $InstallDir 'PaperNotes.exe') 'PaperNotes.exe'
  # 带版本号，见文件头注释
  Copy-IfChanged (Join-Path $bin 'PaperNotes.Shell.dll') (Join-Path $InstallDir $dllName) $dllName

  # Versioned DLL names mean an upgrade leaves the previous file behind.  Drop
  # the superseded copies, but never fail the install over it: Explorer may still
  # have one loaded, and the registration already points at the new file.
  Get-ChildItem $InstallDir -Filter 'PaperNotes.Shell.*.dll' -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -ne $dllName } |
    ForEach-Object {
      # Keep the file in its own variable: inside catch, $_ is the error record,
      # so $_.Name would print empty.
      $stale = $_
      try {
        Remove-Item $stale.FullName -Force -ErrorAction Stop
        Say "  清理旧版本 $($stale.Name)"
      } catch {
        Say "  旧版本 $($stale.Name) 正被占用，保留（不影响使用）"
      }
    }
}

function Ensure-SigningCert {
  Step "自签名证书（$publisher）"
  if ($DryRun) { Say "  [dry] 若不存在则在 LocalMachine\My 创建，并导入 LocalMachine\Root + TrustedPeople"; return $null }

  $existing = Get-ChildItem 'Cert:\LocalMachine\My' -ErrorAction SilentlyContinue |
    Where-Object { $_.Subject -eq $publisher -and $_.HasPrivateKey } |
    Sort-Object NotAfter -Descending | Select-Object -First 1

  if ($existing) {
    Say "  复用已有证书 $($existing.Thumbprint)（到期 $($existing.NotAfter.ToString('yyyy-MM-dd'))）"
    $cert = $existing
  } else {
    $cert = New-SelfSignedCertificate `
      -Type CodeSigningCert `
      -Subject $publisher `
      -KeyUsage DigitalSignature `
      -FriendlyName 'PaperNotes Code Signing' `
      -CertStoreLocation 'Cert:\LocalMachine\My' `
      -NotAfter (Get-Date).AddYears(5) `
      -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
    Say "  已创建 $($cert.Thumbprint)"
  }

  # AppX 部署只认机器级存储，两个都装：Root 供链校验，TrustedPeople 供侧载。
  $cer = Join-Path $env:TEMP "PaperNotes-$($cert.Thumbprint).cer"
  Export-Certificate -Cert $cert -FilePath $cer -Type CERT | Out-Null
  & certutil.exe -addstore -f Root $cer 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "写入 LocalMachine\Root 失败（需要管理员）" }
  & certutil.exe -addstore -f TrustedPeople $cer 2>&1 | Out-Null
  Remove-Item $cer -Force -ErrorAction SilentlyContinue
  Say "  已导入 LocalMachine\Root + TrustedPeople"

  return $cert
}

function Build-And-SignPackage() {
  param([System.Security.Cryptography.X509Certificates.X509Certificate2]$Cert)

  Step "打包并签名 MSIX"
  if ($DryRun) { Say "  [dry] makeappx pack -> signtool sign -> out\PaperNotes.signed.msix"; return $null }

  if (Test-Path $StagingDir) { Remove-Item $StagingDir -Recurse -Force }
  New-Item -ItemType Directory -Force $StagingDir | Out-Null
  Copy-Item (Join-Path $bin 'PaperNotes.exe') $StagingDir -Force
  # 包内路径由清单写死，必须是无版本号的名字
  Copy-Item (Join-Path $bin 'PaperNotes.Shell.dll') (Join-Path $StagingDir 'PaperNotes.Shell.dll') -Force
  Copy-Item (Join-Path $repo 'packaging\Assets') $StagingDir -Recurse -Force
  Copy-Item (Join-Path $repo 'packaging\AppxManifest.xml') $StagingDir -Force

  $makeappx = Find-SdkTool 'makeappx.exe'
  $signtool = Find-SdkTool 'signtool.exe'
  $msix = Join-Path $repo 'out\PaperNotes.signed.msix'
  if (Test-Path $msix) { Remove-Item $msix -Force }

  & $makeappx pack /d $StagingDir /p $msix /o 2>&1 | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "makeappx pack 失败" }

  $pfx = Join-Path $env:TEMP 'PaperNotes-sign.pfx'
  $secure = ConvertTo-SecureString -String $PfxPassword -Force -AsPlainText
  Export-PfxCertificate -Cert $Cert -FilePath $pfx -Password $secure | Out-Null
  & $signtool sign /fd SHA256 /f $pfx /p $PfxPassword /td SHA256 $msix 2>&1 | Out-Null
  Remove-Item $pfx -Force -ErrorAction SilentlyContinue
  if ($LASTEXITCODE -ne 0) { throw "signtool sign 失败" }
  Say "  已签名：$msix"

  return $msix
}

# The package carries its own copy of the exe under WindowsApps, and that is what
# the Win11 first-level menu launches -- not the file in $InstallDir.  If the
# package ends up holding an older exe than the one just built, the menu starts a
# stale build while everything else looks installed.  Compare the two so that
# mismatch fails here instead of showing up later as "the icon reverted".
function Assert-PackageParity {
  if ($DryRun) { Say "  [dry] 校验包内 exe 与构建产物一致"; return }

  $pkg = Get-AppxPackage -Name 'PaperNotes.Phase0' -ErrorAction SilentlyContinue |
    Select-Object -First 1
  if (-not $pkg) { throw '安装后找不到 PaperNotes.Phase0 包' }

  $packaged = Join-Path $pkg.InstallLocation 'PaperNotes.exe'
  if (-not (Test-Path $packaged)) { throw "包内缺少 PaperNotes.exe：$packaged" }

  $built = (Get-FileHash (Join-Path $bin 'PaperNotes.exe') -Algorithm SHA256).Hash
  $inPackage = (Get-FileHash $packaged -Algorithm SHA256).Hash
  if ($built -ne $inPackage) {
    Say "  !! 包内 exe 与刚构建的不一致"
    Say "     构建: $built"
    Say "     包内: $inPackage"
    throw ("MSIX 包里的 exe 不是这次构建的产物，安装结果不可信。" +
           "请先删除 $StagingDir 与 out\\PaperNotes.signed.msix 后重试。")
  }
  Say '  包内 exe 与构建产物一致'
}

function Install-Package {
  param([string]$MsixPath)
  Step "安装 MSIX（现代第一层菜单）"
  if ($DryRun) { Say "  [dry] Add-AppxPackage"; return $null }

  $old = Get-AppxPackage -Name 'PaperNotes.Phase0' -ErrorAction SilentlyContinue
  if ($old) {
    Say "  移除旧包 $($old.PackageFullName)"
    Remove-AppxPackage -Package $old.PackageFullName -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800
  }

  Add-AppxPackage -Path $MsixPath -ErrorAction Stop
  Start-Sleep -Milliseconds 500
  $pkg = Get-AppxPackage -Name 'PaperNotes.Phase0' | Select-Object -First 1
  if (-not $pkg) { throw "包注册后不可见" }
  if ($pkg.Status -ne 'Ok') { throw "包状态异常：$($pkg.Status)" }
  Say "  $($pkg.PackageFullName)"
  Say "  SignatureKind=$($pkg.SignatureKind) Status=$($pkg.Status)"
  return $pkg
}

# 开发阶段所有注册都写在 HKCU（免管理员）。正式安装改走 HKLM，
# 若不清掉旧的 HKCU 项，同一份注册会存在两处，表现为
# 「显示更多选项」里出现两条「新建便签」。所以安装/卸载都先清一遍。
function Clear-UserLeftovers {
  Step "清理旧的每用户注册（避免与 HKLM 重复）"
  foreach ($k in @(
    'HKCU:\Software\Classes\CLSID\{3B1A6C2E-6F41-4A1C-9D3A-7E5B2C8F0A11}',
    'HKCU:\Software\Classes\CLSID\{3B1A6C2E-6F41-4A1C-9D3A-7E5B2C8F0A12}',
    'HKCU:\Software\Classes\DesktopBackground\shellex\ContextMenuHandlers\PaperNotes',
    'HKCU:\Software\Classes\Directory\Background\shellex\ContextMenuHandlers\PaperNotes',
    'HKCU:\Software\Classes\DesktopBackground\shell\PaperNotes',
    'HKCU:\Software\Classes\Directory\Background\shell\PaperNotes')) {
    if ($DryRun) { Say "  [dry] 删除 $k" }
    elseif (Test-Path $k) {
      Remove-Item $k -Recurse -Force -ErrorAction SilentlyContinue
      Say "  删除 $k"
    }
  }
  $hkcuRun = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
  if (Test-Path $hkcuRun) {
    if (Get-ItemProperty $hkcuRun -Name $runValue -ErrorAction SilentlyContinue) {
      if ($DryRun) { Say "  [dry] 删除 HKCU Run\$runValue" }
      else {
        Remove-ItemProperty -Path $hkcuRun -Name $runValue -Force -ErrorAction SilentlyContinue
        Say "  删除 HKCU Run\$runValue"
      }
    }
  }
}

function Register-ClassicHandler {
  Step "注册经典菜单处理器（「显示更多选项」/ 第三方桌面接管）"
  $dll = Join-Path $InstallDir $dllName
  if ($DryRun) { Say "  [dry] HKLM CLSID $clsidClassic -> $dll"; return }

  $clsidKey = "HKLM:\Software\Classes\CLSID\$clsidClassic"
  $inproc = Join-Path $clsidKey 'InprocServer32'
  New-Item -Path $inproc -Force | Out-Null
  Set-ItemProperty -Path $inproc -Name '(Default)' -Value $dll
  Set-ItemProperty -Path $inproc -Name 'ThreadingModel' -Value 'Apartment'

  $label = -join [char[]](0x65B0, 0x5EFA, 0x4FBF, 0x7B7E)   # 新建便签
  Set-ItemProperty -Path $clsidKey -Name '(Default)' -Value $label

  foreach ($root in @(
    'HKLM:\Software\Classes\DesktopBackground\shellex\ContextMenuHandlers\PaperNotes',
    'HKLM:\Software\Classes\Directory\Background\shellex\ContextMenuHandlers\PaperNotes')) {
    New-Item -Path $root -Force | Out-Null
    Set-ItemProperty -Path $root -Name '(Default)' -Value $clsidClassic
  }
  Say "  InprocServer32 -> $dll"
  Say "  DesktopBackground + Directory\Background shellex 已登记"
}

function Register-Autostart {
  if ($SkipAutostart) { Step "跳过自启（-SkipAutostart）"; return }

  Step "配置开机自启"
  $exe = Join-Path $InstallDir 'PaperNotes.exe'
  $command = '"{0}" --autostart' -f $exe
  if ($DryRun) { Say "  [dry] HKLM Run $runValue = $command"; return }

  if (-not (Test-Path $runKey)) { New-Item -Path $runKey -Force | Out-Null }
  Set-ItemProperty -Path $runKey -Name $runValue -Value $command
  $back = (Get-ItemProperty -Path $runKey -Name $runValue).$runValue
  if ($back -ne $command) { throw "自启回读不一致：$back" }
  Say "  $runValue = $back"
}

function Save-State {
  param($Cert, $Package)

  if ($DryRun) { return }
  New-Item -Path $stateKey -Force | Out-Null
  Set-ItemProperty -Path $stateKey -Name 'InstallDir' -Value $InstallDir
  Set-ItemProperty -Path $stateKey -Name 'Version' -Value $Version
  Set-ItemProperty -Path $stateKey -Name 'ShellDll' -Value (Join-Path $InstallDir $dllName)
  if ($Cert) { Set-ItemProperty -Path $stateKey -Name 'CertThumbprint' -Value $Cert.Thumbprint }
  if ($Package) { Set-ItemProperty -Path $stateKey -Name 'PackageFullName' -Value $Package.PackageFullName }
}

function Start-App {
  if ($DryRun) { return }
  Step "启动"
  $exe = Join-Path $InstallDir 'PaperNotes.exe'
  Get-Process PaperNotes -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
  Start-Sleep -Milliseconds 500

  # 安装程序是提权的，直接启动会让便签程序也带管理员权限。这有两个坏处：
  #   1) 一个常驻 GUI + shell 扩展以高权限跑，没必要且放大攻击面；
  #   2) 提权进程写的 %LOCALAPPDATA% 与普通用户不是同一个位置，
  #      先提权跑一次、再普通启动，会看到两套便签数据。
  # explorer.exe 由登录用户运行，让它代启即可回到普通权限。
  try {
    Start-Process -FilePath 'explorer.exe' -ArgumentList "`"$exe`"" | Out-Null
    Start-Sleep -Milliseconds 1500
    $p = Get-Process PaperNotes -ErrorAction SilentlyContinue
    if ($p) {
      Say "  已启动（普通权限，便签以隐藏方式恢复）"
      return
    }
    Say "  explorer 代启未生效，回退为直接启动（将是管理员权限）"
  } catch {
    Say "  explorer 代启失败：$($_.Exception.Message)"
  }

  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $exe
  $psi.UseShellExecute = $false
  [System.Diagnostics.Process]::Start($psi) | Out-Null
  Say "  已启动"
}

# ---------------------------------------------------------------- 卸载

function Uninstall-All {
  Step "停止程序"
  if (-not $DryRun) { Get-Process PaperNotes -ErrorAction SilentlyContinue | Stop-Process -Force }

  Step "移除 MSIX 包"
  if ($DryRun) { Say "  [dry] Remove-AppxPackage" }
  else {
    $pkg = Get-AppxPackage -Name 'PaperNotes.Phase0' -ErrorAction SilentlyContinue
    if ($pkg) { Remove-AppxPackage -Package $pkg.PackageFullName -ErrorAction SilentlyContinue; Say "  已移除 $($pkg.PackageFullName)" }
    else { Say "  未安装" }
  }

  Step "清理菜单注册"
  foreach ($k in @(
    "HKLM:\Software\Classes\CLSID\$clsidClassic",
    'HKLM:\Software\Classes\DesktopBackground\shellex\ContextMenuHandlers\PaperNotes',
    'HKLM:\Software\Classes\Directory\Background\shellex\ContextMenuHandlers\PaperNotes')) {
    if ($DryRun) { Say "  [dry] 删除 $k" }
    elseif (Test-Path $k) { Remove-Item $k -Recurse -Force; Say "  删除 $k" }
  }
  Clear-UserLeftovers

  Step "清理自启"
  if ($DryRun) { Say "  [dry] 删除 HKLM Run\$runValue" }
  elseif (Test-Path $runKey) {
    if ((Get-ItemProperty $runKey -Name $runValue -ErrorAction SilentlyContinue)) {
      Remove-ItemProperty -Path $runKey -Name $runValue -Force
      Say "  已删除 $runValue"
    } else { Say "  未配置" }
  }

  # 只删自己装的证书，且仅在确实装过时删；别动别人的根证书。
  Step "移除自签名证书"
  $thumb = if (Test-Path $stateKey) { (Get-ItemProperty $stateKey -Name 'CertThumbprint' -ErrorAction SilentlyContinue).CertThumbprint } else { $null }
  if ($DryRun) { Say "  [dry] 若 thumbprint=$thumb 则从 LocalMachine\Root / TrustedPeople / My 删除" }
  elseif ($thumb) {
    foreach ($store in @('Root', 'TrustedPeople', 'My')) {
      & certutil.exe -delstore $store $thumb 2>&1 | Out-Null
    }
    Say "  已尝试删除 $thumb"
  } else { Say "  未记录证书，跳过" }

  Step "删除安装目录"
  if ($DryRun) { Say "  [dry] 删除 $InstallDir（保留便签数据）" }
  elseif (Test-Path $InstallDir) { Remove-Item $InstallDir -Recurse -Force -ErrorAction SilentlyContinue; Say "  已删除 $InstallDir" }

  if (-not $DryRun -and (Test-Path $stateKey)) { Remove-Item $stateKey -Recurse -Force -ErrorAction SilentlyContinue }

  Step "完成"
  Say "便签数据**未删除**，仍在 %LOCALAPPDATA%\PaperNotes（或回退目录）。"
  Say "需要彻底清除时手动删除该目录。"
}

# ---------------------------------------------------------------- 主流程

Say "纸笺便签 · $Action$(if ($DryRun) { ' (DryRun)' } else { '' })"

if (-not (Assert-Admin)) {
  # 卸载不强制提权会留下 HKLM 垃圾，所以两条路都要提权。
  if ($DryRun) { Say "未提权，仅打印计划（DryRun 不申请提权）" }
  else { Restart-Elevated; return }
}

# 提权后的窗口一关，屏幕上的输出就没了。所以落一份日志：
# 出问题时这是唯一线索（本机第一次跑就踩到，无日志只能盲猜）。
$logPath = $null
if (-not $DryRun) {
  $logDir = Join-Path $env:ProgramData 'PaperNotes'
  New-Item -ItemType Directory -Force $logDir -ErrorAction SilentlyContinue | Out-Null
  $logPath = Join-Path $logDir 'install.log'
  try { Start-Transcript -Path $logPath -Append -Force | Out-Null } catch { $logPath = $null }
}

try {
  if ($Action -eq 'Uninstall') {
    # 卸载必须作用于真正装好的那个目录，所以优先读回上次写进 HKLM 的位置。
    if (-not $InstallDir) {
      $InstallDir = (Get-ItemProperty -Path $stateKey -Name 'InstallDir' -ErrorAction SilentlyContinue).InstallDir
    }
    if (-not $InstallDir) { $InstallDir = Join-Path $env:ProgramFiles 'PaperNotes' }
    $InstallDir = Resolve-InstallDir $InstallDir
    Say "  卸载目标 $InstallDir"
    Uninstall-All
    return
  }

  # 问用户装哪儿。DryRun 不交互，直接按默认值打印计划。
  if (-not $InstallDir) {
    if ($DryRun) { $InstallDir = Join-Path $env:ProgramFiles 'PaperNotes' }
    else { $InstallDir = Select-InstallDir -default (Join-Path $env:ProgramFiles 'PaperNotes') }
  }
  $InstallDir = Resolve-InstallDir $InstallDir
  Assert-InstallDir $InstallDir -NoCreate:$DryRun

  Step "预检"
  if (-not (Test-Path (Join-Path $bin 'PaperNotes.exe'))) { throw "缺少 $bin\PaperNotes.exe，请先 tools\build.ps1 -Action Build" }
  if (-not (Test-Path (Join-Path $bin 'PaperNotes.Shell.dll'))) { throw "缺少 $bin\PaperNotes.Shell.dll" }
  Say "  构建产物就绪"
  Say "  安装目录 $InstallDir"

  Install-Files
  $cert = Ensure-SigningCert
  $msix = Build-And-SignPackage -Cert $cert
  $pkg = Install-Package -MsixPath $msix
  Assert-PackageParity
  Clear-UserLeftovers
  Register-ClassicHandler
  Register-Autostart
  Save-State -Cert $cert -Package $pkg
  if (-not $DryRun -and (Test-Path $StagingDir)) { Remove-Item $StagingDir -Recurse -Force -ErrorAction SilentlyContinue }
  Start-App

  Step "安装完成"
  Say "  安装目录：$InstallDir"
  Say "  桌面右键第一层：新建便签"
  Say "  找回便签：Ctrl+Alt+L 或列表窗口"
  Say "  卸载：tools\install.ps1 -Action Uninstall（保留便签数据）"
}
catch {
  Write-Host ""
  Write-Host "!! 安装失败：$($_.Exception.Message)" -ForegroundColor Red
  Write-Host "   位置：$($_.InvocationInfo.PositionMessage)" -ForegroundColor Red
  if ($logPath) { Write-Host "   完整日志：$logPath" -ForegroundColor Yellow }
  exit 1
}
finally {
  try { Stop-Transcript | Out-Null } catch { }
}
