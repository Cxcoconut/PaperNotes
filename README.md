# 纸笺便签 · PaperNotes

> Windows 桌面右键第一层菜单直接新建轻量便签。
> 本文档是项目的**唯一主文档**，现状、决策、坑、下一步都写在这里；
> 阶段 0 的完整实验记录另见 [`docs/phase0-findings.md`](docs/phase0-findings.md)。
>
> 最后更新：2026-10-27

---

## 1. 一句话现状

Windows 11 桌面右键第一层菜单的「新建便签」**已跑通并已可正式安装**
（现代菜单 + 腾讯桌面整理共存两种环境都验证过），
便签本体具备多张、自动保存、重启恢复（不铺开）、列表找回、可配置全局快捷键、开机自启。

安装程序 `tools\install.ps1` 已端到端验证：提权安装签名 MSIX + 经典菜单处理器 + 自启，
安装后桌面现代第一层菜单确认出现「新建便签」。

**尚未完成**：UI 打磨（含设置窗口外观待确认）、升级/卸载循环实测、阶段 2 验收。

---

## 2. 用户原始需求（权威，不可擅自改）

- 平台：Windows 10 / 11
- 触发：桌面空白处右键，**第一层菜单**直接出现「新建便签」，不能藏在「显示更多选项」里
- 便签：每次新建独立便签，可同时存在多张；纯文本、支持换行；新建时出现在**右键位置附近**
- 保存与关闭：本机自动保存；关闭便签默认保留，不弹保存确认；只有明确删除时才二次确认
- 启动与找回：开机自启动，但**启动时不自动铺开旧便签**；从托盘/简单列表重新打开
- 快捷键：新建便签、显示/隐藏便签两类都可自定义
- 外观与质量：轻量、纸质便签风格、响应快、不能卡死；正常安装方式，可接受管理员权限
- 暂不做：云同步、富文本、图片附件、搜索

### 硬性红线

第一层菜单或原始右键位置无法可靠实现时，**必须停下来问用户**，
不得悄悄改成「显示更多选项」、托盘或快捷键方案。

---

## 3. 进度

| 项目 | 状态 | 证据 |
|---|---|---|
| Win11 桌面右键第一层出现「新建便签」 | 完成 | `docs/evidence/modern2-7000.png` |
| 腾讯桌面整理运行时也能出现 | 完成 | 改走 COM 处理器路径（§4.2） |
| 原始右键位置（非点菜单位置） | 完成 | 菜单构建瞬间采样光标 |
| 多张便签 / 重叠错位 | 完成 | 用户确认 |
| 内容 + 位置自动保存 | 完成 | `tools/verify-persistence.ps1` 端到端跑通 |
| 重启恢复且**不铺开**（隐藏） | 完成 | 日志 `restore: N note(s) loaded hidden` |
| 关闭=隐藏、`×`=删除（二次确认） | 完成 | 标题栏 `−` 隐藏 / `×` 删除 |
| 便签列表窗口（托盘不可用时的入口） | 完成 | 列表 + 新建/打开/设置/删除/关闭 |
| 全局快捷键（默认 Ctrl+Alt+N / Ctrl+Alt+L） | 完成 | 注册成功日志 |
| 快捷键可配置 + 冲突提示 + 失败回滚 | 完成（界面外观待用户确认） | `settings.ini` 持久化 |
| DPI 缩放（150% 下不被字撑爆） | 完成 | 便签 510×405 物理像素 |
| 验证 `ExplorerCommandHandler` 能否替代 MSIX | **完成，结论：不能** | §4.2 / `docs/phase0-findings.md` §4.5 |
| 开机自启（`--autostart` 静默启动） | **完成** | §4.8，`tools/register-autostart.ps1` |
| MSIX 签名可行性（自签名 vs 购买证书） | **完成，结论见 §4.9** | 实测 `SignatureKind=Developer` |
| 安装程序（安装/注册/自启/卸载保留数据） | **完成** | §4.10，`tools/install.ps1` |
| Windows 10 实测 | **不做**（用户决定） | |
| 阶段 2 验收、性能指标、交付物 | **未做** | |

---

## 4. 架构与关键决策（含「为什么」）

### 4.1 进程与模块

```
PaperNotes.exe          常驻 Win32 进程：便签窗口、列表窗口、设置窗口、快捷键、存储
PaperNotes.Shell.dll    被 Explorer / 桌面接管程序进程内加载的 COM 上下文菜单处理器
```

不使用 Electron / .NET，纯 Win32 + MSVC，`/MT` 静态链接，单 exe 约 200KB。

### 4.2 菜单注册：两条通道，缺一不可

| 通道 | 注册位置 | 负责的场景 |
|---|---|---|
| AppX `IExplorerCommand` | MSIX 包内 `com:SurrogateServer` | **Win11 桌面现代第一层菜单（唯一途径）** |
| 经典 COM 处理器 `IContextMenu` | `HKCU\Software\Classes\CLSID\{...0A12}\InprocServer32` + `DesktopBackground\shellex\ContextMenuHandlers\PaperNotes` | Win10 经典菜单、Win11「显示更多选项」、**第三方桌面接管程序** |

两个 CLSID：`{3B1A6C2E-6F41-4A1C-9D3A-7E5B2C8F0A11}`（包内）、`{...0A12}`（经典）。
同一个 DLL，同一个对象（同时实现 `IExplorerCommand` + `IContextMenu` + `IShellExtInit`），
`DllGetClassObject` 接受两个 CLSID，避免两条注册互相干扰。

**为什么不用静态动词 `shell\<verb>`**：第三方桌面整理会按自己的准入规则过滤（见 §7.2），
而 `shellex\ContextMenuHandlers` 是它必须调用的标准接口。静态动词已移除，避免菜单重复。

**这两条通道的分工是实测出来的，不是推测**（2026-10-27 受控对照实验）：

| 步骤 | MSIX 包 | ECH 动词 | shellex 处理器 | Win11 现代第一层出现？ |
|---|---|---|---|---|
| A | 移除 | 无 | 有 | **不出现**（桌面） |
| B | 移除 | 无 | 有 | **不出现**（资源管理器文件夹背景） |
| C | 移除 | **有** | 有 | **不出现**（资源管理器文件夹背景） |
| D | 恢复 | 无 | 有 | **出现** |

结论：

- 现代第一层菜单 = **只有 MSIX 这一条路**
- shellex 处理器 = 负责经典菜单、「显示更多选项」、第三方桌面接管，**但它进不了现代第一层**
- 两者**不是等价冗余，是各自覆盖不同菜单面**，缺一不可

> 曾经的设想「用 `ExplorerCommandHandler` 取代 MSIX 就能免签名交付」**已被证伪**。
> 为什么当初看起来可行、以及完整实验设计，见 [`docs/phase0-findings.md`](docs/phase0-findings.md) §4.5。
> 证据：`docs/evidence/ech-test-{A,B,C}-*.png`。

### 4.3 原始右键位置（本项目最容易做错的地方）

绝不能把「点击菜单项时的鼠标位置」当成右键位置。

现方案：**Shell 扩展在菜单构建瞬间采样光标**。`GetTitle` / `GetState` / `QueryContextMenu`
这三个方法在右键之后、菜单可见之前被调用，此刻光标仍在右键点。

实现要点：

- 每次菜单构建会产生一串调用，用 **1200ms 间隔的突发检测**只保留第一次采样。
- 采样用 `GetPhysicalCursorPos`（shell 进程受 DPI 虚拟化影响，物理坐标才与便签窗口坐标系一致）。
- 结果写入共享内存 `Local\PaperNotes.ClickState.v1`，结构体 `ClickState`（56 字节，`static_assert` 锁定）。
- 应用侧 `ChooseAnchor()` 若发现低层钩子也记录了点，两者相距 ≤64px 视为同一次点击，优先钩子；否则用采样点。
- 每条记录只消费一次，避免连续新建复用同一个点。

低层鼠标钩子 `WH_MOUSE_LL` 保留但**非必需**——本机环境中钩子收不到真实事件（见 §8），
采样路径已独立验证可用。

### 4.4 点击到出现的延迟

处理器原本每次都 `ShellExecuteExW` 起新进程再转发消息，用户感知是几百毫秒。

现做法：**先 `FindWindow(PaperNotes.HiddenWindow.v1)`，找到就直接 `PostMessage(WM_APP+2)`**，
只有应用没运行时才启动进程。实测：

```
shell.log : invoke -> posted to running instance in 0ms
phase0.log: note #1 created ... clickToNoteMs=0
```

### 4.5 存储

每张便签一个文件，格式故意做成可读可手改：

```
x=102
y=102
w=340
h=270
<空行>
正文（UTF-8，可含换行）
```

目录按可写性**逐级回退**（`StoreDirectory()`）：
`%LOCALAPPDATA%\PaperNotes` → `%USERPROFILE%\PaperNotesData` → `%TEMP%\PaperNotes` → `<exe目录>\PaperNotesData`。

落盘去抖 600ms（`EN_CHANGE` → `SetTimer`），移动/缩放（`WM_EXITSIZEMOVE`）、隐藏、退出时强制保存。

> **本机现象**：因进程令牌限制，前两个位置写不进去，实际落在 `<exe目录>\PaperNotesData`
> （本机为 `out\bin\PaperNotesData`）。这不是设计意图，是环境所致；正常机器上会用 `%LOCALAPPDATA%`。
>
> 注意日志与便签**不在同一目录**：`LogLine()` 写 `DataDirectory()`（`%USERPROFILE%\PaperNotesData`），
> 便签文件写 `StoreDirectory()`。排查时别混。

### 4.6 DPI

所有布局常量是**设备无关像素**，必须经 `ScaleFor(dpi, value)` 缩放。

最初漏了这一步：屏幕 150%（dpi=144），窗口用 340×270 物理像素，字号却按 DPI 放大到 22px，
字把窗口撑爆，表现为「界面显示不全」。修复后新建便签 510×405 物理像素。

### 4.7 找回入口（托盘不可依赖）

Win11 默认把新托盘图标塞进折叠区；受限会话里 `Shell_NotifyIcon` 还可能直接 `Access Denied`。

所以：**列表窗口是主入口**，从列表「设置」按钮进快捷键设置；托盘失败时启动自动弹出列表。

---

### 4.8 开机自启（已实现）

自启走 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`，值为：

```
"C:\...\PaperNotes.exe" --autostart
```

- **必须整行加引号**：路径含空格时（如 `C:\Program Files\...`），不加引号 Windows 会去启动
  `C:\Program`。`tools\register-autostart.ps1` 写完会**回读并重新切分命令行**校验。
- `--autostart` 的语义：恢复便签**隐藏**，并且**连列表窗口都不弹**。
  原逻辑是「托盘图标失败就弹列表找底」，但在登录瞬间弹窗是最糟的体验，
  所以 autostart 分支显式跳过它（`Ctrl+Alt+L` 和右键菜单仍能找回）。
- 用 HKCU 而非 HKLM：不需要管理员，卸载只删一个值。
- 已实测两种启动的差异：

  | 启动方式 | 便签 | 列表窗口 | 日志特征 |
  |---|---|---|---|
  | `--autostart` | 隐藏恢复 | **不弹** | `autostart=1`，无 fallback 行 |
  | 普通启动 | 隐藏恢复 | 弹（托盘不可用时） | `autostart=0` + fallback 行 |

### 4.9 MSIX 签名的硬约束（2026-10-27 实测）

这是「怎么分发给普通用户」的核心，结论比想象中严格。

**实测过程**：自签名证书（`CN=PaperNotesDev`，与清单 `Publisher` 一致）→ `signtool sign` 成功
→ `signtool verify /pa` **通过**（把证书装进 `CurrentUser\Root` 后）→ `Add-AppxPackage` **失败**：

```
HRESULT: 0x800B0109
应用包中的签名的根证书必须是受信任的证书。
```

**根因**：AppX 部署服务（以 SYSTEM 运行）**只信任机器级存储**
（`LocalMachine\Root` / `LocalMachine\TrustedPeople`），不认当前用户存储。
所以 `signtool verify`（用用户存储）通过 ≠ 安装会通过，两者结论会打架。

**关键转机：证书进 `LocalMachine\Root` 之后就能装了。**

提权把证书导入机器级存储后，同一台机器、同一个包，`Add-AppxPackage` **成功**：

```
PaperNotes.Phase0_0.1.0.0_x64__gzfq254d1nysy
SignatureKind=Developer  Status=Ok
```

这同时证明了一件重要的事：**之前那次失败时开发者模式是开着的**，
所以开发者模式并不能绕过证书信任要求——**决定成败的是机器级证书信任，不是开发者模式**。

**三条分发路径及其真实代价**：

| 路径 | 目标机器要求 | 成本 | 适用 |
|---|---|---|---|
| 公开 CA 证书（OV/EV）签 MSIX | **无**（公共根已在机器存储里） | 买证书 + 资质审核 | 公开发布 |
| 自签名 + 安装程序**提权**写 `LocalMachine\Root` | 每台机器 UAC 一次 | 免费 | 自己/小范围 ← **当前采用** |
| 保持现状：开发者模式松散注册 | 每台机器开**开发者模式** | 免费 | 仅开发机 |

**重要**：用户已明确「可接受管理员权限」，所以第二条路在需求上是允许的，
`tools\install.ps1` 已按它实现并**端到端验证通过**（见 §4.10）。

> 误导性提醒：`signtool verify` 成功不能证明 MSIX 可安装。
> 判断标准只能是 `Add-AppxPackage` 本身，且必须在**目标机器**上验证。
>
> 另一个坑：签名包安装成功后 `SignatureKind` 是 `Developer`（自签名特征），
> 这不是错误状态，也不代表还需要开发者模式。

### 4.10 安装程序（`tools/install.ps1`）

已端到端跑通。流程：**提权 → 落安装目录 → 证书进机器级存储 → 打包签名 → 装 MSIX →
清 HKCU 残留 → 注册 HKLM 经典处理器 → 配自启 → 保存状态 → 降权启动**。

| 设计点 | 为什么 |
|---|---|
| **必须提权** | 写 `LocalMachine\Root` 与 `HKLM`；不这样做 MSIX 装不上（§4.9） |
| 未提权时自动 `RunAs` 重启自己 | 用户双击一次即可，参数原样透传 |
| 日志写 `C:\ProgramData\PaperNotes\install.log` | **提权窗口一关，屏幕输出就没了**，没有日志只能盲猜（见下方 bug） |
| DLL 用 `PaperNotes.Shell.0.1.0.0.dll` | 进程内扩展文件被锁，同名覆盖必失败（LNK1104）；升级靠换新文件名 |
| 包内仍叫 `PaperNotes.Shell.dll` | 清单 `Path` 写死，包内不能带版本号 |
| 安装前清 HKCU 同名注册 | 开发阶段注册在 HKCU，正式装写 HKLM，不清会重复出现两条菜单 |
| **启动时降权**（`explorer.exe <exe>` 代启） | 提权父子进程会继承管理员权限；一个常驻 GUI + shell 扩展以高权限跑没必要，而且提权进程的 `%LOCALAPPDATA%` 与普通用户不是同一处，先提权跑一次再普通启动会看到**两套便签数据** |
| 卸载保留便签数据 | 需求明确要求；数据在 `%LOCALAPPDATA%\PaperNotes` |
| 只删自己装的那张证书 | 按记录的 thumbprint 删，不动别人的根证书 |

**实测踩到并修掉的两个 bug**（都只在提权路径暴露，DryRun 抓不到）：

1. `Find-SdkTool` 用 `$candidates[0]` 取路径。只匹配到一条时 PowerShell 会把字符串当字符数组，
   返回 `'C'`，于是 `& $makeappx` 报「无法将 C 项识别为 cmdlet」。修法：结果外面套 `@()`。
2. 安装程序是提权的，直接 `Process.Start` 会让便签程序**继承管理员权限**，
   结果非管理员连 `Stop-Process` 都杀不掉它，且数据写进了提权用户的位置。修法：用 explorer 代启降权。

**升级注意事项**：本项目现在有**两份 exe**（`C:\Program Files\PaperNotes` 的经典版、
MSIX 包内的打包版）。两者靠单实例互斥量（`Local\PaperNotes.SingleInstance.v1`）保证只有一个在跑，
数据目录都收敛到 `%LOCALAPPDATA%\PaperNotes`，不会分叉。

---

## 5. 代码结构

```
src/
  Common/click_state.h        共享内存 ABI、隐藏窗口类名、WM_APP+2          (48 行)
  App/PaperNotesApp.cpp       全部应用逻辑（单文件）                        (2222 行)
  App/app.manifest            PerMonitorV2 + Win10/11 supportedOS
  Shell/PaperNotesShell.cpp   IExplorerCommand + IContextMenu + IShellExtInit + IClassFactory (474 行)
  Shell/PaperNotesShell.def   导出 DllGetClassObject / DllCanUnloadNow
packaging/AppxManifest.xml    MSIX 清单（desktop4/desktop5 + com:SurrogateServer）
tools/                        见 §6
docs/phase0-findings.md       阶段 0 完整证据与结论（含 §4.5 ECH 否定结论）
docs/evidence/                截图与日志证据（随仓库版本化）
```

应用内非显然的部分：

- `ChooseAnchor()`：坐标来源仲裁与一次性消费
- `SaveNote()` / `RestoreStoredNotes()`：持久化；恢复时**只创建不显示**
- `BubbleNoteToFront()`：用 `HWND_TOPMOST` → `HWND_NOTOPMOST` 抬升，因为后台进程抢不到前台
- `ApplyHotkeys()`：注销 → 注册 → 失败回滚，保证不会把可用快捷键弄丢
- `StoreDirectory()` / `DirectoryIsWritable()`：可写目录探测
- 窗口类：`PaperNotes.HiddenWindow.v1` / `.NoteWindow.v1` / `.ListWindow.v1` / `.SettingsWindow.v1`

---

## 6. 常用命令

```powershell
# ---- 交付：安装 / 卸载（会弹 UAC）----
powershell -NoProfile -ExecutionPolicy Bypass -File tools\install.ps1 -Action Install
powershell -NoProfile -ExecutionPolicy Bypass -File tools\install.ps1 -Action Uninstall
# 只看计划不改系统
powershell -NoProfile -ExecutionPolicy Bypass -File tools\install.ps1 -Action Install -DryRun
# 安装日志（提权窗口关闭后唯一的线索）
notepad C:\ProgramData\PaperNotes\install.log

# ---- 开发：构建 ----
# 构建两个项目（会自动先停掉正在运行的 PaperNotes，否则 exe 被占用）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build.ps1 -Action Build

# 只重建应用（DLL 被 Explorer/桌面整理占用时用这条）
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe" `
  src\App\PaperNotesApp.vcxproj /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo

# ---- 开发：菜单注册（免管理员的开发态）----
# 注册经典 COM 处理器（每用户，无需管理员）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\register-shell-handler.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools\register-shell-handler.ps1 -Unregister

# MSIX 松散注册（开发态；正式安装请用 install.ps1，它装的是签名包）
Add-AppxPackage -Register out\package\AppxManifest.xml -ForceApplicationShutdown

# 开机自启（开发态）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\register-autostart.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools\register-autostart.ps1 -Unregister

# 不弹菜单就让系统自己构建桌面菜单并列出条目，验证注册是否生效
powershell -NoProfile -ExecutionPolicy Bypass -File tools\probe-shell-menu.ps1

# 端到端验证持久化：新建→写文本→落盘→杀进程→重启→确认恢复且隐藏
powershell -NoProfile -ExecutionPolicy Bypass -File tools\verify-persistence.ps1

# 查看坐标共享内存状态
powershell -NoProfile -ExecutionPolicy Bypass -File tools\read-clickstate.ps1

# 暴露桌面以便手动/自动右键验证菜单（-Restore 还原窗口）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\ui-verify-menu.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools\ui-verify-menu.ps1 -Restore
```

构建环境：VS 2022 BuildTools 17.14、MSVC 14.44、Windows SDK 10.0.26100，
无 cmake/ninja/.NET SDK 依赖。`makeappx.exe` / `signtool.exe` 在 SDK 同目录下。

---

## 7. 关键坑与结论（最省时间的部分）

### 7.1 低层鼠标钩子

- **钩子回调里绝对不能做 I/O**：超过 `LowLevelHooksTimeout` 会被系统**静默摘除**，不报错。
  早期每事件写日志 → 到 2069 个事件后彻底失效。现回调内只写共享内存。
- `SetWindowsHookEx` 的 `hMod` 必须传 `nullptr`（进程内钩子）。传 EXE 模块句柄会「安装成功但永不回调」。
- 本机即使修好，钩子仍收不到真实事件（§8），但同机 PowerShell 探针能收到 → 是进程级限制，不是实现问题。

### 7.2 腾讯桌面整理（DeskGo 4.2.1662.127）

它接管了整个桌面（`TXMiniSkin` 覆盖整个屏幕），自己画菜单。静态提取 `Features64.dll` 得到：

```
ContextMenuManager()::GetDesktopContextMenuHandlers
Directory\Background\shellex\ContextMenuHandlers
LoadIUnknownFromCLSID()::CoCreateInstance / LoadLibrary
CThirdPartyMenuHandleHelper::LoadMenuItem
ShortCutMenuBlacklist
Wintrust.dll / WinVerifyTrust
```

结论：它枚举 **shellex COM 处理器**（所以我们改成处理器后立刻出现），
对静态动词则经自己的准入判断（黑名单 + 签名校验），未签名的开发版被过滤。

**明确不做**：绕过它的签名校验/黑名单。不稳定（它升级即失效），且属于绕过他人安全机制。

> 实测补充：DeskGo 运行时，桌面右键菜单是**它自己的菜单**，不是 Win11 现代菜单。
> 验证现代菜单必须先退出 DeskGo（`Stop-Process DesktopMgr64`），验证完再启动回来。

### 7.3 PowerShell 编码（踩了两次，损失很大）

- Windows PowerShell 5.1 读取**无 BOM 的 UTF-8 脚本**时按 ANSI 解码，
  脚本里的中文字面量在读取阶段就已损坏。**用字符码构造**：`-join [char[]](0x65B0,0x5EFA,...)`。
- **禁止** `Get-Content -Raw` + `Set-Content -Encoding UTF8` 改源码：会把中文字面量吞掉。
  要用 `[IO.File]::ReadAllText/WriteAllText` 配 `New-Object Text.UTF8Encoding($false)`。
- 用正则做批量替换时**必须断言「恰好匹配 1 处」**再写入；否则锚点撞车会误删大段代码（真实发生过，删掉 32k 字符）。
- `-File` 传数组参数会合并成一个逗号串，脚本内需 `-split '[;,]'`。
- `Get-Content` 直接读 UTF-8 便签文件会显示乱码（按 ANSI 解码）；那是**显示问题**，文件本身是好的。

### 7.4 其他

- `FindWindow(cls, $null)` 在 PowerShell 里会变成「标题为空字符串」，要用 `[NullString]::Value`。
- 注册表路径里带 `*` 时 PowerShell 会当通配符展开，扫全树导致卡死；要用 `-LiteralPath`。
- 跨进程 `WM_SETTEXT` **不会**给父窗口发 `EN_CHANGE`；测自动保存要手动投 `WM_COMMAND`，
  且通知码在高字：`wParam = 0x03000000`（`EN_CHANGE` = 0x0300）。
- 进程内加载的 DLL 无法原地替换（`LNK1104`）。构建前先把它**重命名挪开**，再重启宿主进程。
- MSIX 清单里 `uap10:RuntimeBehavior="win32App"` **不能**同时声明 `EntryPoint`。
- 注册表菜单标签写入后要**回读字节**校验（`新建便签` = `65B0 5EFA 4FBF 7B7E`）。
- 便签 `×` 走 `WM_CLOSE` → `SaveNote(force=true)` + `SW_HIDE`，**会顺带把空便签也落盘**；
  清理测试残留时要同时删窗口和 `.note` 文件。
- 此环境下 `Start-Process` 可能挂住 executor；用 `[System.Diagnostics.Process]::Start($psi)` 更稳。

---

## 8. 环境限制的真相（2026-10-27 更正，重要）

开发机：Windows 11 Build 26200、单显示器 2560×1600 @150%（dpi=144）、无管理员权限。

**先说结论：早先记录的「应用进程被令牌限制」是*启动上下文*造成的，不是机器限制。**

早期所有验证都是从 agent/受限上下文里拉起 `PaperNotes.exe`，于是出现：

| 操作 | 受限上下文启动 | **用 explorer 正常启动** |
|---|---|---|
| 写 `%LOCALAPPDATA%\PaperNotes` | `ERROR_ACCESS_DENIED(5)` | **成功**（`ok=1 gle=0`） |
| 写 `%USERPROFILE%\PaperNotesData` | `ERROR_ACCESS_DENIED(5)` | **成功** |
| `Shell_NotifyIcon(NIM_ADD)` | `ERROR_ACCESS_DENIED(5)` | **成功**（日志 `tray icon added`） |

安装程序用 `explorer.exe <exe>` 降权启动后，日志立刻出现：

```
writable dir=C:\Users\cx101\AppData\Local\PaperNotes ok=1 gle=0
tray icon added
restore: 1 note(s) loaded hidden from C:\Users\cx101\AppData\Local\PaperNotes
```

**所以**：

1. 存储落在 `<exe目录>\PaperNotesData` **不是设计意图的必然结果**，只是受限启动的副产物；
   正常启动会落在 `%LOCALAPPDATA%\PaperNotes`（设计意图）。
2. **托盘图标是好的**，之前"无法验证"是启动方式的问题，不是实现问题。
   列表窗口兜底仍然保留（Win11 会把托盘图标收进折叠区，仍需要主入口）。
3. 排查这类问题时，**先怀疑是谁把这个进程拉起来的**，再怀疑代码。

> **仍然成立的限制**：全局低层鼠标钩子在这个会话里收不到真实事件（`hookEvents` 长时间不增），
> 但采样路径已独立验证可用，且钩子本就是可选的（§4.3）。

> **`SetCursorPos` 是间歇性的**：多次返回 True 并成功移动光标，也会间歇失败
> （返回 False，`err=203`），此时 computer-use 的输入注入一并被拒
> （`COMPUTER_USE_INPUT_UNAVAILABLE`）。属于会话状态问题，等交互桌面恢复再试。

**无管理员权限**这一点仍然成立：`LocalMachine\*` 写入、`HKLM` 写入必须靠 UAC 提权，
所以安装程序按"提权"设计（§4.10）。

另：**无 Windows 10 环境**。**用户已决定不做 Win10 实测**，
Win10 经典菜单路径按"未验证"处理，不再列入阻塞项。

---

## 9. 下一步

1. ~~**开机自启**~~ —— **已完成**，见 §4.8。
2. ~~**安装程序**~~ —— **已完成并端到端验证**，见 §4.10。
3. ~~**决定 MSIX 分发方式**~~ —— **已决定：自签名 + 提权**（用户拍板），已随安装程序实现并验证。
   若日后要面向**公开发布**，再评估买 OV/EV 证书替换自签名（安装程序无需改动，
   只换签名证书；代价对比见 §4.9）。
4. **UI 打磨**：便签拖动/缩放手感、列表窗口双击行为、删除确认文案。
   另外待用户确认：设置窗口外观（§3 表中标注"界面外观待用户确认"）。
5. **升级路径实测**：装 → 装新版 → 卸 的完整循环。已知设计要点：
   DLL 带版本号所以可覆盖安装；卸载保留数据；`Save-State` 记录了证书 thumbprint 与包全名。
6. **阶段 2 验收**：Win11 菜单、连续新建/拖动/边缘/多显示器/不同缩放、
   自动保存（编辑后关闭、重启、强杀）、启动行为、快捷键冲突、重复启动、安装升级卸载、
   记录「新建到可输入」耗时、空闲内存、异常。（Win10 不做。）

### 已关闭的议题（不要再重开）

- ~~用 `ExplorerCommandHandler` 替代 MSIX~~ —— 实测**不可行**，见 §4.2。
- ~~Windows 10 实测~~ —— 用户决定不做。
- ~~开机自启~~ —— 已完成，见 §4.8。
- ~~MSIX 分发方式~~ —— 已选自签名 + 提权，见 §4.9。
- ~~安装程序~~ —— 已完成，见 §4.10。

---

## 10. 交接注意事项

- 仓库：`C:\cx_ProjectCode\PaperNotes`（git 已初始化并提交）。
  `out/` 在 `.gitignore`，产物不入库。**每次较大改动前先提交**——两次靠 git 救回源码。
- 构建前必须停掉 `PaperNotes.exe`（`build.ps1` 已内置），否则 `LNK1104`。
- 桌面整理（`DesktopMgr64.exe`）会在进程内加载 Shell DLL，导致 DLL 无法重建：
  先 `Stop-Process DesktopMgr64`，把旧 DLL 重命名，重建后再启动它。
- **做菜单注册对照实验前，必须先确认 `Get-AppxPackage PaperNotes.Phase0` 状态**：
  MSIX 包会"冒充" shellex 处理器的功劳，导致得出完全相反的结论（本会话真实踩过）。
- **当前机器已用 `tools\install.ps1` 正式安装过**，注册状态是**机器级**：

  ```
  HKLM\Software\Classes\CLSID\{3B1A6C2E-6F41-4A1C-9D3A-7E5B2C8F0A12}\InprocServer32
      = C:\Program Files\PaperNotes\PaperNotes.Shell.0.1.0.0.dll   (ThreadingModel=Apartment)
  HKLM\Software\Classes\DesktopBackground\shellex\ContextMenuHandlers\PaperNotes
  HKLM\Software\Classes\Directory\Background\shellex\ContextMenuHandlers\PaperNotes
  HKLM\Software\Microsoft\Windows\CurrentVersion\Run\PaperNotes = "<install>\PaperNotes.exe" --autostart
  HKLM\Software\PaperNotes                       (安装状态：InstallDir / ShellDll / CertThumbprint / PackageFullName)
  ```

  MSIX：`PaperNotes.Phase0_0.1.0.0_x64__gzfq254d1nysy`，`SignatureKind=Developer`、`Status=Ok`。
  **这个包是 Win11 现代第一层菜单的唯一来源，删掉菜单项就消失**（已实测）。
  证书：`CN=PaperNotesDev` 自签名，已导入 `LocalMachine\My` + `Root` + `TrustedPeople`。
  卸载走 `tools\install.ps1 -Action Uninstall`，它按记录的 thumbprint 只删这一张证书。

- 开发态的每用户注册（HKCU）已被安装程序清理干净。若要回到纯开发态，
  用 `register-shell-handler.ps1` + `register-autostart.ps1` 重新注册（免管理员）。

- 已弃用但仍保留的脚本（**不要当交付路径**）：
  - `tools\register-shell-verb.ps1`：静态动词，会与处理器重复，仅纯 Win10 兜底时考虑
  - `tools\register-shell-command-handler.ps1`：`ExplorerCommandHandler` 实验工具，
    `-Unregister` 可完全清理。注册后会与处理器重叠，在「显示更多选项」里出现**两条重复项**
- 实验用注册表备份在 `out\diagnostics\reg-backup-20261027\`（不入库）。
- `out\signing\` 里有自签名 PFX/cer（**含私钥，切勿入库或外发**）；`out/` 已 gitignore。

---

## 11. 证据文件

| 文件 | 内容 |
|---|---|
| `docs/evidence/modern2-7000.png` | Win11 桌面现代**第一层**菜单中出现「新建便签」 |
| `docs/evidence/note-ui2.png` | 便签窗口最终外观（`−` 隐藏 / `×` 删除） |
| `docs/evidence/settings-ui3.png` | 列表窗口含「设置」按钮 |
| `docs/evidence/phase0.log` | 运行日志（含 `clickToNoteMs=0`、`restore: N note(s) loaded hidden`） |
| `docs/evidence/ech-test-A-msix-removed-desktop-no-item.png` | 移除 MSIX 后桌面现代第一层**无**「新建便签」 |
| `docs/evidence/ech-test-B-msix-removed-explorer-no-item.png` | 同上，资源管理器文件夹背景 |
| `docs/evidence/ech-test-C-ech-verb-no-msix-explorer-no-item.png` | 有 ECH 动词、无 MSIX，仍然**无**「新建便签」 |
| `docs/evidence/installed-desktop-modern-menu.png` | **正式安装后**（签名 MSIX + HKLM 处理器）桌面现代第一层出现「新建便签」 |
| `docs/phase0-findings.md` | 阶段 0 完整验证记录、DeskGo 逆向证据、§4.5 ECH 否定结论 |

---

## 12. 文档约定

- **主文档就是本 README**。现状、决策、坑、下一步都写这里，不要再另起交接文档。
- 长实验记录写进 `docs/`，README 只留结论和指针。
- 截图/日志放 `docs/evidence/`，随仓库版本化（`out/` 不入库，别把证据只丢在那里）。
- 写「完成」必须有可复现的证据（脚本输出、截图、日志行），否则写「未验证」。
