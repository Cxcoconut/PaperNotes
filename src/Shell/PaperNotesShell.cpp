#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <appmodel.h>

#include <string>
#include <new>
#include <thread>
#include <cstdarg>

#include "../Common/click_state.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace {

using papernotes::ClickState;
using papernotes::kClickStateMappingName;

// {3B1A6C2E-6F41-4A1C-9D3A-7E5B2C8F0A11}
// Registered by the MSIX package as an out-of-process COM server for the
// modern IExplorerCommand menu.
const CLSID kCommandClsid =
    {0x3b1a6c2e, 0x6f41, 0x4a1c, {0x9d, 0x3a, 0x7e, 0x5b, 0x2c, 0x8f, 0x0a, 0x11}};

// {3B1A6C2E-6F41-4A1C-9D3A-7E5B2C8F0A12}
// Registered classically (InprocServer32) as a shellex ContextMenuHandler.
// This is the Windows 10 path and also the channel that desktop-replacement
// shells enumerate through IShellExtInit/IContextMenu, so it reaches menus the
// static-verb registration cannot. It is a distinct class from the packaged one
// so the two registrations never contend.
const CLSID kHandlerClsid =
    {0x3b1a6c2e, 0x6f41, 0x4a1c, {0x9d, 0x3a, 0x7e, 0x5b, 0x2c, 0x8f, 0x0a, 0x12}};

constexpr wchar_t kCommandTitle[] = L"新建便签";

HMODULE g_module = nullptr;
volatile LONG g_objectCount = 0;
volatile LONG g_factoryCount = 0;
volatile LONG g_lockCount = 0;

void LogDebug(const wchar_t* text) {
  OutputDebugStringW(L"[PaperNotes.Shell] ");
  OutputDebugStringW(text);
  OutputDebugStringW(L"\n");
}

std::wstring ModuleDirectory() {
  wchar_t path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(g_module, path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return {};
  PathRemoveFileSpecW(path);
  return path;
}

std::wstring ExecutablePath() {
  const std::wstring dir = ModuleDirectory();
  if (dir.empty()) return {};
  std::wstring path = dir;
  path += L"\\PaperNotes.exe";
  return path;
}

std::wstring PackageFamilyName() {
  UINT32 length = 0;
  LONG result = GetCurrentPackageFamilyName(&length, nullptr);
  if (result != ERROR_INSUFFICIENT_BUFFER || length == 0) return {};
  std::wstring family(length, L'\0');
  result = GetCurrentPackageFamilyName(&length, family.data());
  if (result != ERROR_SUCCESS) return {};
  family.resize(wcslen(family.c_str()));
  return family;
}

void StampInvocation() {
  HANDLE mapping = CreateFileMappingW(
      INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(ClickState),
      kClickStateMappingName);
  if (!mapping) return;
  auto* state =
      static_cast<ClickState*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0,
                                             0, sizeof(ClickState)));
  if (state) {
    InterlockedExchange(&state->invokePid,
                        static_cast<LONG>(GetCurrentProcessId()));
    InterlockedExchange64(&state->invokeTick,
                          static_cast<LONG64>(GetTickCount64()));
    UnmapViewOfFile(state);
  }
  CloseHandle(mapping);
}

// Sample the cursor while File Explorer is building OUR menu.  This runs
// immediately after the right-click, before the menu is visible, so the point
// is the original right-click position and not the position of the click on
// the menu item.  It deliberately needs no global hook.
//
// Every menu build produces a burst of GetTitle/GetState/QueryContextMenu
// calls.  The FIRST sample of a burst is the accurate one, so a gap-based
// burst detector keeps only that.
void StampMenuBuild() {
  constexpr ULONGLONG kBurstGapMs = 1200;
  static ULONGLONG lastSampleTick = 0;

  const ULONGLONG now = GetTickCount64();
  if (lastSampleTick != 0 && now - lastSampleTick < kBurstGapMs) {
    lastSampleTick = now;
    return;
  }
  lastSampleTick = now;

  POINT point = {};
  // GetPhysicalCursorPos is immune to the DPI virtualisation that the shell
  // surrogate process is subject to, so the value matches the per-monitor-v2
  // coordinate space the note windows are placed in.
  if (!GetPhysicalCursorPos(&point)) {
    if (!GetCursorPos(&point)) return;
  }

  HANDLE mapping = CreateFileMappingW(
      INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(ClickState),
      kClickStateMappingName);
  if (!mapping) return;
  auto* state =
      static_cast<ClickState*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0,
                                             0, sizeof(ClickState)));
  if (state) {
    InterlockedExchange(&state->menuBuildX, point.x);
    InterlockedExchange(&state->menuBuildY, point.y);
    InterlockedExchange(&state->menuBuildPid,
                        static_cast<LONG>(GetCurrentProcessId()));
    InterlockedExchange64(&state->menuBuildTick,
                          static_cast<LONG64>(GetTickCount64()));
    UnmapViewOfFile(state);
  }
  CloseHandle(mapping);
}

bool LaunchViaActivationManager() {
  const std::wstring family = PackageFamilyName();
  if (family.empty()) return false;
  const std::wstring appUserModelId = family + L"!App";

  HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool uninitialize = SUCCEEDED(hr);
  if (hr == RPC_E_CHANGED_MODE) {
    hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) return false;
  } else if (FAILED(hr)) {
    return false;
  }

  IApplicationActivationManager* manager = nullptr;
  hr = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr,
                        CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&manager));
  if (FAILED(hr) || !manager) {
    if (uninitialize) CoUninitialize();
    return false;
  }

  DWORD processId = 0;
  hr = manager->ActivateApplication(appUserModelId.c_str(), L"--new-note",
                                    AO_NONE, &processId);
  manager->Release();
  if (uninitialize) CoUninitialize();
  return SUCCEEDED(hr);
}

bool LaunchViaExecutable() {
  const std::wstring exe = ExecutablePath();
  if (exe.empty()) return false;
  SHELLEXECUTEINFOW info = {};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
  info.lpVerb = L"open";
  info.lpFile = exe.c_str();
  info.lpParameters = L"--new-note";
  info.nShow = SW_SHOWNORMAL;
  return ShellExecuteExW(&info) != FALSE;
}

// Appends one line to the shared diagnostic log.  This only runs on menu
// invoke, which is a cold path, so file I/O here cannot stall the shell.
void LogTiming(const wchar_t* format, ...) {
  wchar_t message[1024] = {};
  va_list args;
  va_start(args, format);
  _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, args);
  va_end(args);

  SYSTEMTIME now = {};
  GetLocalTime(&now);
  wchar_t line[1200] = {};
  _snwprintf_s(line, _countof(line), _TRUNCATE,
               L"%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu %s\r\n",
               now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
               now.wSecond, now.wMilliseconds, GetCurrentProcessId(), message);

  wchar_t profile[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) == 0) return;
  std::wstring dir(profile);
  dir += L"\\PaperNotesData";
  CreateDirectoryW(dir.c_str(), nullptr);
  std::wstring path = dir + L"\\shell.log";

  HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  // UTF-8 so the file matches the app log and reads correctly in any editor.
  const int needed = WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0,
                                         nullptr, nullptr);
  if (needed > 1) {
    std::string utf8(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8.data(), needed, nullptr,
                        nullptr);
    DWORD written = 0;
    WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written,
              nullptr);
  }
  CloseHandle(file);
}

// Fast path: the app is usually already running, so a single PostMessage
// replaces an entire process creation round trip.
bool NotifyRunningInstance() {
  HWND hidden = FindWindowW(papernotes::kHiddenWindowClass, nullptr);
  if (!hidden) return false;
  return PostMessageW(hidden, papernotes::kNewNoteMessage, 0, 0) != FALSE;
}

void LaunchWorker() {
  const ULONGLONG started = GetTickCount64();
  if (NotifyRunningInstance()) {
    LogTiming(L"invoke -> posted to running instance in %llums",
              GetTickCount64() - started);
    return;
  }
  const ULONGLONG beforeLaunch = GetTickCount64();
  const bool activated = LaunchViaActivationManager();
  if (!activated) {
    LogDebug(L"activation manager failed; falling back to direct executable");
    LaunchViaExecutable();
  }
  LogTiming(L"invoke -> %s launch took %llums (total %llums)",
            activated ? L"activation-manager" : L"direct-executable",
            GetTickCount64() - beforeLaunch, GetTickCount64() - started);
}

void RequestNewNote() {
  StampInvocation();
  // Invoke must return quickly. Application activation and process startup
  // happen after the shell has closed the menu. The lock prevents this DLL
  // from being unloaded while the detached worker is still using its code.
  InterlockedIncrement(&g_lockCount);
  try {
    std::thread([] {
      LaunchWorker();
      InterlockedDecrement(&g_lockCount);
    }).detach();
  } catch (...) {
    InterlockedDecrement(&g_lockCount);
  }
}

class Command final : public IExplorerCommand, public IContextMenu,
                      public IShellExtInit {
 public:
  Command() { InterlockedIncrement(&g_objectCount); }
  ~Command() { InterlockedDecrement(&g_objectCount); }

  IFACEMETHODIMP QueryInterface(REFIID riid, void** result) override {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) ||
        IsEqualIID(riid, IID_IExplorerCommand)) {
      *result = static_cast<IExplorerCommand*>(this);
    } else if (IsEqualIID(riid, IID_IContextMenu)) {
      *result = static_cast<IContextMenu*>(this);
    } else if (IsEqualIID(riid, IID_IShellExtInit)) {
      *result = static_cast<IShellExtInit*>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  IFACEMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&refs_));
  }

  IFACEMETHODIMP_(ULONG) Release() override {
    const LONG count = InterlockedDecrement(&refs_);
    if (count == 0) delete this;
    return static_cast<ULONG>(count);
  }

  // IExplorerCommand
  IFACEMETHODIMP GetTitle(IShellItemArray*, PWSTR* title) override {
    StampMenuBuild();
    if (!title) return E_POINTER;
    return SHStrDupW(kCommandTitle, title);
  }

  IFACEMETHODIMP GetIcon(IShellItemArray*, PWSTR* icon) override {
    if (!icon) return E_POINTER;
    *icon = nullptr;
    const std::wstring exe = ExecutablePath();
    if (exe.empty()) return S_FALSE;
    return SHStrDupW(exe.c_str(), icon);
  }

  IFACEMETHODIMP GetToolTip(IShellItemArray*, PWSTR* tooltip) override {
    if (!tooltip) return E_POINTER;
    *tooltip = nullptr;
    return S_FALSE;
  }

  IFACEMETHODIMP GetCanonicalName(GUID* canonicalName) override {
    if (!canonicalName) return E_POINTER;
    *canonicalName = GUID_NULL;
    return S_OK;
  }

  IFACEMETHODIMP GetState(IShellItemArray*, BOOL, EXPCMDSTATE* state) override {
    StampMenuBuild();
    if (!state) return E_POINTER;
    *state = ECS_ENABLED;
    return S_OK;
  }

  IFACEMETHODIMP Invoke(IShellItemArray*, IBindCtx*) override {
    RequestNewNote();
    return S_OK;
  }

  IFACEMETHODIMP GetFlags(EXPCMDFLAGS* flags) override {
    if (!flags) return E_POINTER;
    *flags = ECF_DEFAULT;
    return S_OK;
  }

  IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** commands) override {
    if (!commands) return E_POINTER;
    *commands = nullptr;
    return E_NOTIMPL;
  }

  // IContextMenu: Windows 10 classic fallback, and Win11 "Show more options"
  // only if the OS explicitly loads the class through that path.
  IFACEMETHODIMP QueryContextMenu(HMENU menu, UINT index, UINT firstCommandId,
                                  UINT lastCommandId,
                                  UINT flags) override {
    StampMenuBuild();
    if (flags & CMF_DEFAULTONLY) return S_OK;
    if (firstCommandId > lastCommandId) return E_INVALIDARG;
    if (!InsertMenuW(menu, index,
                     MF_BYPOSITION | MF_STRING | MF_ENABLED, firstCommandId,
                     kCommandTitle)) {
      return HRESULT_FROM_WIN32(GetLastError());
    }
    return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 1);
  }

  IFACEMETHODIMP InvokeCommand(LPCMINVOKECOMMANDINFO info) override {
    if (!info) return E_POINTER;
    if (HIWORD(info->lpVerb) != 0) return E_INVALIDARG;
    if (LOWORD(info->lpVerb) != 0) return E_INVALIDARG;
    RequestNewNote();
    return S_OK;
  }

  IFACEMETHODIMP GetCommandString(UINT_PTR command, UINT type, UINT*,
                                  LPSTR, UINT) override {
    (void)command;
    (void)type;
    return E_NOTIMPL;
  }

  // IShellExtInit
  IFACEMETHODIMP Initialize(PCIDLIST_ABSOLUTE, IDataObject*, HKEY) override {
    return S_OK;
  }

 private:
  volatile LONG refs_ = 1;
};

class Factory final : public IClassFactory {
 public:
  Factory() { InterlockedIncrement(&g_factoryCount); }
  ~Factory() { InterlockedDecrement(&g_factoryCount); }

  IFACEMETHODIMP QueryInterface(REFIID riid, void** result) override {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
      *result = static_cast<IClassFactory*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  IFACEMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&refs_));
  }

  IFACEMETHODIMP_(ULONG) Release() override {
    const LONG count = InterlockedDecrement(&refs_);
    if (count == 0) delete this;
    return static_cast<ULONG>(count);
  }

  IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid,
                                void** result) override {
    if (outer || !result) return outer ? CLASS_E_NOAGGREGATION : E_POINTER;
    *result = nullptr;
    auto* command = new (std::nothrow) Command();
    if (!command) return E_OUTOFMEMORY;
    const HRESULT hr = command->QueryInterface(riid, result);
    command->Release();
    return hr;
  }

  IFACEMETHODIMP LockServer(BOOL lock) override {
    if (lock) {
      InterlockedIncrement(&g_lockCount);
    } else {
      InterlockedDecrement(&g_lockCount);
    }
    return S_OK;
  }

 private:
  volatile LONG refs_ = 1;
};

}  // namespace

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** result) {
  if (!result) return E_POINTER;
  *result = nullptr;
  // Both classes expose the same object: it implements IExplorerCommand and
  // IContextMenu together, so whichever interface the caller asks for works.
  if (!IsEqualCLSID(clsid, kCommandClsid) &&
      !IsEqualCLSID(clsid, kHandlerClsid)) {
    return CLASS_E_CLASSNOTAVAILABLE;
  }
  auto* factory = new (std::nothrow) Factory();
  if (!factory) return E_OUTOFMEMORY;
  const HRESULT hr = factory->QueryInterface(riid, result);
  factory->Release();
  return hr;
}

STDAPI DllCanUnloadNow() {
  return (g_objectCount == 0 && g_factoryCount == 0 && g_lockCount == 0)
             ? S_OK
             : S_FALSE;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = module;
    DisableThreadLibraryCalls(module);
  }
  return TRUE;
}
