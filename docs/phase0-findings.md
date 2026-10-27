# 阶段 0：技术可行性验证记录

本文件只记录**本次会话中实际执行并观察到**的结果。未验证的项目明确标注为"未验证"。

## 1. 验证环境

| 项目 | 值 | 来源 |
|---|---|---|
| 操作系统 | Windows 11，Build 26200（注册表 `DisplayVersion=25H2`） | `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion` |
| 版本字符串 | `Microsoft Windows 10.0.26200`（Win11 内部版本号仍为 10.0） | `RuntimeInformation.OSDescription` |
| 硬件标识 | `Get-CimInstance Win32_OperatingSystem` 被拒（权限），仅读到 `WindowsProductName=Windows 10 Home China` 缓存值 | 见"限制" |
| 屏幕 | 单显示器，物理 2559×1599，缩放 150%，DPI 96 虚拟化后 1707×1067 | 截图尺寸 + `GetDpiForSystem()` |
| 桌面接管程序 | 腾讯桌面整理 `TXMiniSkin` / `DesktopMgr64`，窗口 0,0,1707,1066 覆盖整个桌面 | `EnumChildWindows(Progman)` |
| 构建工具 | VS 2022 Build Tools 17.14，MSVC 14.44，Windows SDK 10.0.26100 | `vswhere` |
| 打包工具 | `makeappx.exe` / `signtool.exe`（SDK 10.0.26100） | 直接调用 |
| 包注册方式 | 开发模式松散注册（`Add-AppxPackage -Register`），未签名 | `Get-AppxPackage PaperNotes.Phase0` → `Status=Ok` |

**未验证：真实 Windows 10。** 本机无 Win10 实体机、虚拟机或远程机，无 VirtualBox/VMware/Hyper-V。

## 2. 采用的方案

- 原生 Win32 C++（无 Electron / .NET 运行时依赖），多显示器 PerMonitorV2。
- MSIX 打包 + `IExplorerCommand` COM 服务器（`com:SurrogateServer`，STA），
  通过 `desktop4:FileExplorerContextMenus` / `desktop5:ItemType Type="Directory\Background"` 注册现代菜单项。
- 同时注册**每用户注册表菜单项**（`HKCU\Software\Classes\DesktopBackground\shell\...`
  与 `Directory\Background\shell\...`），覆盖经典菜单与第三方桌面接管菜单。
- 坐标来源两路并行，应用启动时按优先级选择（见 §4）。

## 3. 验证结果

### 3.1 第一层现代菜单

| 项目 | 结果 | 证据 |
|---|---|---|
| 现代第一层菜单出现"新建便签" | **通过** | 截图 `out/diagnostics/modern2-7000.png`，菜单项为 `新建便签`，与 `PowerRename`、`Code` 同层 |
| 菜单项可被程序化调用 | **通过** | UIA 找到 `MenuItem 新建便签`，`InvokePattern` 可用并成功触发 |
| COM 直接激活 | **通过** | 跨进程 `CoCreateInstance` 返回 `GetTitle=0x0 TITLE=新建便签 GetState=0x0` |
| 注册表项系统侧可见 | **通过** | `IShellFolder::CreateViewObject(IID_IContextMenu)` + `QueryContextMenu` 枚举出 `[4] 新建便签` |
| 用户真实右键（桌面） | **依赖第三方** | 腾讯桌面整理运行时**看不到**；退出该程序后**出现** |

### 3.2 原始右键坐标

| 项目 | 结果 | 证据 |
|---|---|---|
| 全局低层鼠标钩子（`WH_MOUSE_LL`） | **本环境不可用** | 原生进程钩子安装成功（`install=1`）但 `hookEvents=0`；同分钟 PowerShell 探针收到 `[REAL]` 右键 4 次 |
| 菜单构建瞬间抓取光标 | **通过** | 共享内存 `build` 字段记录到真实点（例如 `x=979 y=459`），与点击菜单项时鼠标位置无关 |
| 多张便签 | **通过** | 用户确认；每次命令创建独立窗口，重叠时按 26px 阶梯错位 |

### 3.3 关键结论

1. **第一层菜单在系统原生路径下可行**，代码路径正确，无需退回"显示更多选项"。
2. **原始右键坐标可可靠获取**，且不依赖全局钩子：`GetTitle`/`GetState`/`QueryContextMenu`
   在菜单构建期间被调用，此时光标仍在右键点。
3. **桌面菜单可能被第三方接管**。腾讯桌面整理会缓存桌面菜单项，新注册项在它重启前不显示。

## 3.4 第三方桌面接管（腾讯桌面整理）的实测分析

这是本次最关键的环境结论。

**现象**：腾讯桌面整理（`DesktopMgr64.exe`，安装于
`C:\Program Files (x86)\Tencent\DeskGo\4.2.1662.127`）运行时，桌面右键菜单里
看不到"新建便签"；退出该程序后立刻出现。

**已排除的原因**：

| 假设 | 结论 | 证据 |
|---|---|---|
| 注册写错 / 编码错 | 排除 | 回读 `MUIVerb` 字节 `B0 65 FA 5E BF 4F 7E 7B` |
| 系统不认这条注册 | 排除 | `IShellFolder` + `QueryContextMenu` 枚举到 `[4] 新建便签` |
| 启动缓存（需重启它） | 排除 | 在我们注册之后（09:45）它重新启动过，仍未显示 |
| 只读 HKLM 不读 HKCU | 排除 | `HKCU\...\Directory\Background\shell` 下 WorkBuddy 显示、我们的不显示 |

**从 `Features64.dll` 静态提取到的实现证据**（UTF-16 字符串，已修正提取器）：

```
ContextMenuManager()::GetDesktopContextMenuHandlers
Directory\Background\shellex\ContextMenuHandlers
LoadIUnknownFromCLSID()::GetDesktopContextMenuHandlers LoadLibrary %s, %s Error = %d
LoadIUnknownFromCLSID()::CoCreateInstance %#x DllPath:%s  CLSID:%s
CThirdPartyMenuHandleHelper::LoadMenuItem
CThirdPartyMenuHandleHelper::ClearAllThirdPartyMenuHandler
ShortCutMenuBlacklist
DisableShortCutMenu
InProcServer32
Software\Classes\CLSID\%s
Wintrust.dll / WinVerifyTrust
D:\...\src\Features\MenuManage\ThirdPartyMenuHandleHelper.cpp
```

**结论**：它构建桌面菜单时走两条路 ——
1. `shellex\ContextMenuHandlers` → 读 `CLSID\{...}\InProcServer32` → `LoadLibrary` +
   `CoCreateInstance` 拿 `IContextMenu`；
2. `DesktopBackground\Shell\` 静态动词，但**经过 `CThirdPartyMenuHandleHelper`
   的准入判断**（黑名单 `ShortCutMenuBlacklist` + `WinVerifyTrust` 签名校验）。

我们的**静态动词**被第 2 条路径过滤（开发版未签名）；`WinVerifyTrust` 的存在使
"签名不足即隐藏"成为最合理解释。

**采用的对策（不改对方程序、不绕过其安全判断）**：改用第 1 条路径 ——
注册成标准 COM shellex 处理器。

实测验证（移除全部静态动词后单独枚举，确认处理器自身生效）：

```
QueryContextMenu hr=0x0000004A items=10
  [0] 护眼卫士
  [1] 右键菜单管理
  [2] 新建便签        <-- 仅由 COM 处理器产生
  [3] (separator)
  ...
```

## 3.5 两条注册通道的分工

| 通道 | 覆盖场景 | 状态 |
|---|---|---|
| MSIX `IExplorerCommand`（AppX COM 服务器） | Win11 桌面**现代第一层**菜单 | 已验证通过 |
| COM `shellex\ContextMenuHandlers`（`InprocServer32`） | Win10 经典菜单、Win11"显示更多选项"、**第三方桌面接管菜单** | 已在真实 Shell 枚举中验证生效 |
| ~~`shell\<verb>` 静态动词~~ | 与上一条重叠且可能重复 | **已移除**，避免菜单出现重复项 |

## 4. 坐标来源的优先级规则

两路来源都写入 `Local\PaperNotes.ClickState.v1` 共享内存：

- `hook`：应用的低层鼠标钩子记录物理右键按下点（真实安装环境下最权威）。
- `build`：Shell 扩展在菜单构建瞬间采样 `GetPhysicalCursorPos()`。

应用创建便签时：

1. 两路都新鲜且相距 ≤ 64px → 认为是同一次点击，采用 **hook**（真实按下点）。
2. 两路都新鲜但相距 > 64px → 采用 **build**（它绑定本次菜单，hook 可能来自无关右键）。
3. 只有一路新鲜 → 采用该路。
4. 都不新鲜 → 回退到光标所在显示器工作区内的确定性位置并阶梯错位。

每次记录的 tick 只被消费一次，避免连续新建时复用同一个点。

## 4.5 `ExplorerCommandHandler` 能否替代 MSIX（2026-10-27 实测，结论：**不能**）

原计划（旧交接文档 §9 第 3 项）设想：在 `DesktopBackground\shell\<verb>` 下设
`ExplorerCommandHandler` = CLSID，若能在 Win11 第一层菜单出现，就能去掉 MSIX，
免代码签名、免开发者模式。**实测否定了这个设想。**

### 为什么当初看起来很有希望

本机 `HKLM\Software\Classes\DesktopBackground\shell` 下确有系统自带动词用这条通道，
且 `InprocServer32` 指向普通 DLL（非打包 COM）：

```
.SpotlightLearnMore  ExplorerCommandHandler={5C6B744C-...}  -> C:\Windows\System32\shell32.dll
EditStickers         ExplorerCommandHandler={3C0E1E28-...}  -> C:\Windows\System32\themecpl.dll
```

它们也确实出现在现代第一层菜单（截图里「浏览背景」「下一个背景」）。

### 但换成"每用户 + 自建未签名 DLL"就不行

受控对照实验（每步都用真实桌面右键 + 截图验证，DeskGo 已退出）：

| 步骤 | MSIX 包 | ECH 动词 | shellex 处理器 | 桌面现代第一层是否出现 |
|---|---|---|---|---|
| A | 移除 | 无 | 有 | **不出现** |
| B | 移除 | 无 | 有 | **不出现**（资源管理器文件夹背景同样不出现）|
| C | 移除 | **有** | 有 | **不出现**（资源管理器文件夹背景同样不出现）|
| D | 恢复 | 无 | 有 | **出现**（恢复原状后确认）|

证据：`out/diagnostics/ech-test-{A,B,C}-*.png`。

### 排除性验证：注册本身是有效的

`probe-shell-menu.ps1` 在步骤 C 下枚举出**两条**「新建便签」：

```
[1] 新建便签   <- shellex 处理器（0A12）
[3] 新建便签   <- ExplorerCommandHandler 动词（0A11）
```

说明 `ExplorerCommandHandler` 注册被系统正确识别并进入了**经典**菜单，
只是**现代第一层菜单不渲染它**。

### 结论与教训

1. 系统自带的 `.SpotlightLearnMore` 能进现代菜单，原因不是 `ExplorerCommandHandler` 本身，
   而是它属于**系统组件**（shell32/themecpl，受信任）。**不能据此推断第三方未签名 DLL 同样可行。**
2. 本项目要进 Win11 现代第一层菜单，**MSIX（`IExplorerCommand` + `com:SurrogateServer`）仍是必需路径**。
3. 若同时保留 ECH 动词与 shellex 处理器，**「显示更多选项」里会出现两条重复项**——
   这是实际观察到的副作用，不要两个都注册。
4. 旧文档里「ECH 可替代 MSIX、交付成本大降」的乐观判断**已作废**，不要再按它排期。

> 方法论提醒：本实验一开始因为 MSIX 包被自动重新注册而得出过相反结论
> （误以为 shellex 处理器单独就能进现代菜单）。**做注册表对照实验前必须先确认
> `Get-AppxPackage` 状态**，否则 MSIX 会冒充 shellex 的功劳。

## 5. 已修复的缺陷（含根因）

| 缺陷 | 根因 | 修复 |
|---|---|---|
| 钩子运行一段时间后静默失效 | 钩子回调里做了文件 I/O（每个事件一次 CreateFile/WriteFile），超过 `LowLevelHooksTimeout` 被系统摘除 | 回调内只写共享内存，计数由定时器上报 |
| `SetWindowsHookEx` 成功但回调从不触发 | `hMod` 传了 EXE 模块句柄，USER32 转而查找 DLL 导出 | 改传 `nullptr` |
| 注册表菜单项显示乱码 | Windows PowerShell 5.1 读取**无 BOM 的 UTF-8 脚本**时按 ANSI 解码，中文字面量在读取阶段即损坏 | 用字符码 `[char[]](0x65B0,...)` 构造字符串，并回读校验字节 |
| 连续新建总在同一位置 | 钩子不可用时只用固定回退点 | 引入 build 采样 + 阶梯错位 |
| 桌面只能存在一张便签 | 原型只维护单个窗口句柄 | 改为 `NoteState` 列表，每张独立窗口与字体 |

## 6. 已知限制

1. **未在真实 Windows 10 验证**。Win10 无现代菜单，`IExplorerCommand` 不会出现在第一层；
   已实现 `IContextMenu` 回退分支与注册表菜单项，但**未在 Win10 上实测**。
2. **第三方桌面接管**（腾讯桌面整理、`DesktopMgr64`）会缓存桌面菜单：
   新注册的菜单项需该程序重启后才显示。安装程序需要检测并提示。
3. **本验证环境对原生子进程限制输入路径**：`SetCursorPos` 返回失败、
   全局钩子收不到输入、`Shell_NotifyIcon` 返回 `Access Denied(5)`。
   这三项在正常用户会话下应当可用，但**本次无法验证**，因此托盘图标与钩子路径属于"未验证"。
4. **未实现的阶段 1 功能**：自启动、便签内容持久化、托盘已保存列表、全局快捷键、安装包。
5. 便签当前**不保存内容**，关闭仅隐藏；重启应用后内容丢失。
6. 未做安装/升级/卸载的注册残留与 Explorer 稳定性验证。

## 7. 复现方式

```powershell
# 构建（含打包目录准备）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build.ps1 -Action Build

# 注册包（开发模式，需可写 Package 存储）
Add-AppxPackage -Register out\package\AppxManifest.xml -ForceApplicationShutdown

# 每用户注册表菜单项
powershell -NoProfile -ExecutionPolicy Bypass -File tools\register-shell-verb.ps1

# 观察共享坐标状态
powershell -NoProfile -ExecutionPolicy Bypass -File tools\read-clickstate.ps1

# 让系统自己构建桌面菜单并列出条目（不显示 UI）
powershell -NoProfile -ExecutionPolicy Bypass -File tools\probe-shell-menu.ps1
```