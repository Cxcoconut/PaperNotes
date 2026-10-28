#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <commctrl.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <string>
#include <vector>

#include "../Common/click_state.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shcore.lib")

namespace {

using papernotes::ClickState;
using papernotes::kClickStateMappingName;

constexpr wchar_t kHiddenClass[] = L"PaperNotes.HiddenWindow.v1";
constexpr wchar_t kNoteClass[] = L"PaperNotes.NoteWindow.v1";
constexpr wchar_t kListClass[] = L"PaperNotes.ListWindow.v1";
constexpr wchar_t kAppTitle[] = L"纸笺便签";

constexpr UINT kMsgTray = WM_APP + 1;
constexpr UINT kMsgNewNote = WM_APP + 2;
constexpr UINT kMsgExit = WM_APP + 3;

constexpr UINT_PTR kTrayId = 1;
constexpr UINT kTrayMenuNew = 1001;
constexpr UINT kTrayMenuList = 1002;
constexpr UINT kTrayMenuExit = 1003;
constexpr UINT kTrayMenuOpenList = 1004;
constexpr UINT kTrayMenuHotkeys = 1005;
// Note entries occupy a contiguous command-id range above the fixed commands.
constexpr UINT kTrayMenuFirstNote = 2000;

// Sizes are device-independent pixels.  A note is deliberately small: it sits
// on top of real work, so it must not hide much of the screen.  310x245 keeps a
// usable writing area while giving roughly a tenth of the screen back compared
// with the original 340x270.
constexpr int kInitialWidth = 310;
constexpr int kInitialHeight = 245;
constexpr int kTitleHeight = 40;
constexpr int kEdge = 14;
constexpr int kMinWidth = 250;
constexpr int kMinHeight = 180;
constexpr int kCascadeStep = 26;
constexpr int kMaxCascade = 14;

// Must match the ICON id in PaperNotesApp.rc.
constexpr int kAppIconId = 101;

// ---------------------------------------------------------------- palette
//
// Single source of truth for the paper look.  Values come from the "Notes &
// Writing App" profile (warm ink + amber accent on cream), rounded to what GDI
// renders cleanly.
//
//   ink   on paper : ~15:1  (body text)
//   muted on paper : ~6:1   (secondary text, title)
//   edge  on paper : decorative only, never carries meaning
//
// Every surface (note, list, settings) must use these, otherwise the windows
// stop looking like one product.
constexpr COLORREF kPaper = RGB(255, 251, 235);       // #FFFBEB
constexpr COLORREF kPaperStrip = RGB(250, 240, 204);  // focused note header
constexpr COLORREF kPaperStripIdle = RGB(245, 239, 217);
constexpr COLORREF kPaperEdge = RGB(226, 214, 186);
constexpr COLORREF kPaperHover = RGB(242, 231, 197);
constexpr COLORREF kInk = RGB(58, 48, 34);
constexpr COLORREF kInkMuted = RGB(122, 108, 86);
constexpr COLORREF kAmber = RGB(217, 119, 6);  // #D97706
constexpr COLORREF kDanger = RGB(178, 44, 36);

// Buttons on the note header.
constexpr int kNoteButtonDip = 28;   // visual + hit square, >= 32px at 150%
constexpr int kNoteButtonGapDip = 4;
constexpr int kNoteTextPadDip = 12;  // inner padding of the text area

// How long an anchor stays usable.  The menu-build sample is taken while the
// menu is being built, so it is always within a few seconds of the click.
constexpr ULONGLONG kMaxAnchorAgeMs = 120 * 1000;

// Global hotkeys exist because the tray icon alone cannot be relied on to bring
// notes back.
constexpr int kHotkeyNewNote = 1;
constexpr int kHotkeyToggleList = 2;
bool g_hotkeyNewNote = false;
bool g_hotkeyToggleList = false;
void ShowHotkeySettings();// If the hook point and the menu-build point agree this closely, they describe
// the same physical click and the hook (the true right-button-down) wins.
constexpr LONG kAgreementTolerancePx = 64;

// All layout constants above are device-independent pixels. They must be scaled
// by the target monitor's DPI, otherwise the DPI-scaled fonts overflow a window
// whose size stayed at 96-DPI pixel counts.
int ScaleFor(UINT dpi, int value) {
  if (dpi == 0) dpi = 96;
  return MulDiv(value, static_cast<int>(dpi), 96);
}

UINT DpiForPoint(POINT point) {
  HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
  UINT dpiX = 96;
  UINT dpiY = 96;
  if (monitor &&
      SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) &&
      dpiX != 0) {
    return dpiX;
  }
  return 96;
}

struct NoteLayout {
  int edge;
  int titleHeight;
  int closeWidth;
  int deleteWidth;
  int buttonTop;
  int buttonBottom;
};

NoteLayout LayoutFor(UINT dpi) {
  NoteLayout layout = {};
  layout.edge = ScaleFor(dpi, kEdge);
  layout.titleHeight = ScaleFor(dpi, kTitleHeight);
  layout.closeWidth = ScaleFor(dpi, 38);
  layout.deleteWidth = ScaleFor(dpi, 34);
  layout.buttonTop = ScaleFor(dpi, 6);
  layout.buttonBottom = ScaleFor(dpi, 40);
  return layout;
}

HINSTANCE g_instance = nullptr;
HWND g_hidden = nullptr;
HHOOK g_mouseHook = nullptr;
volatile LONG64 g_hookEventCount = 0;
volatile LONG64 g_hookRightDownCount = 0;
HANDLE g_clickMapping = nullptr;
ClickState* g_click = nullptr;
NOTIFYICONDATAW g_tray = {};
bool g_trayAdded = false;
HBRUSH g_editBrush = nullptr;
HBRUSH g_listBrush = nullptr;

// Last anchor actually used, so a repeated command never reuses the same
// recorded click for two different notes.
LONG64 g_consumedRightDownTick = 0;
LONG64 g_consumedMenuBuildTick = 0;

POINT g_lastRightDown = {};
ULONGLONG g_lastRightDownTick = 0;

enum class AnchorSource { kHookRightDown, kMenuBuild, kFallback };

const wchar_t* AnchorSourceName(AnchorSource source) {
  switch (source) {
    case AnchorSource::kHookRightDown: return L"hook-right-down";
    case AnchorSource::kMenuBuild: return L"menu-build-cursor";
    default: return L"fallback";
  }
}

struct NoteState {
  HWND hwnd = nullptr;
  HWND edit = nullptr;
  HFONT font = nullptr;
  std::wstring info;
  int width = kInitialWidth;
  int height = kInitialHeight;
  UINT dpi = 96;
  std::wstring id;          // storage identity, empty until saved once
  std::wstring lastSaved;   // content already on disk, to avoid redundant writes
  std::wstring initialText; // content supplied when the window is created
  bool dirty = false;

  // Interaction state for the two header buttons. -1 = none, 0 = hide,
  // 1 = delete.  Held as plain ints so the points stay separate: a hover and a
  // press can disagree, and drawing needs both.
  int hoverButton = -1;
  int pressButton = -1;
  bool trackingMouse = false;
  bool active = false;  // window focused: tints the paper header
};

std::vector<NoteState*> g_notes;

// ------------------------------------------------------------------ storage
//
// One file per note. A single damaged file costs exactly one note instead of
// the whole set, and the format stays readable/repairable by hand:
//
//   x=100
//   y=200
//   w=340
//   h=270
//   <blank line>
//   note text (UTF-8, may contain newlines)

std::wstring DataDirectory();
std::wstring LocalAppDataDirectory();
std::string ToUtf8(const std::wstring& text);
std::wstring ToWide(const std::string& text);
void LogLine(const wchar_t* format, ...);
HICON LoadAppIcon(int width, int height);

int ParseInt(const std::string& text) {
  size_t index = 0;
  bool negative = false;
  if (index < text.size() && (text[index] == '-' || text[index] == '+')) {
    negative = text[index] == '-';
    ++index;
  }
  bool any = false;
  long value = 0;
  while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
    value = value * 10 + (text[index] - '0');
    if (value > 1000000) break;  // defensive: geometry never needs more
    any = true;
    ++index;
  }
  if (!any) return 0;
  return static_cast<int>(negative ? -value : value);
}

std::wstring ExeDirectory() {
  wchar_t path[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring result;
  if (n == 0 || n >= MAX_PATH) return DataDirectory();
  result.assign(path, n);
  const size_t slash = result.find_last_of(L'\\');
  if (slash != std::wstring::npos) result.resize(slash);
  return result;
}

std::wstring TempDirectory() {
  wchar_t path[MAX_PATH] = {};
  const DWORD n = GetTempPathW(MAX_PATH, path);
  std::wstring result;
  if (n == 0 || n >= MAX_PATH) return ExeDirectory();
  result.assign(path, n);
  if (!result.empty() && result.back() == L'\\') result.pop_back();
  result += L"\\PaperNotes";
  return result;
}

bool DirectoryIsWritable(const std::wstring& directory) {
  CreateDirectoryW(directory.c_str(), nullptr);
  const std::wstring probe = directory + L"\\.write-probe";
  HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  CloseHandle(file);
  DeleteFileW(probe.c_str());
  return true;
}

// User data must survive uninstall, so it is never placed inside the install
// directory unless nothing else is writable.
const std::wstring& StoreDirectory() {
  static std::wstring resolved;
  if (!resolved.empty()) return resolved;
  const std::wstring candidates[] = {
      LocalAppDataDirectory(),
      DataDirectory(),
      TempDirectory(),
      ExeDirectory() + L"\\PaperNotesData",
  };
  for (const std::wstring& candidate : candidates) {
    if (DirectoryIsWritable(candidate)) {
      resolved = candidate;
      return resolved;
    }
  }
  resolved = ExeDirectory() + L"\\PaperNotesData";
  CreateDirectoryW(resolved.c_str(), nullptr);
  return resolved;
}

std::wstring NoteFilesDirectory() {
  std::wstring directory = StoreDirectory() + L"\\notes";
  CreateDirectoryW(directory.c_str(), nullptr);
  return directory;
}

bool WriteAllBytes(const std::wstring& path, const std::string& bytes) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const BOOL ok = WriteFile(file, bytes.data(),
                            static_cast<DWORD>(bytes.size()), &written,
                            nullptr);
  CloseHandle(file);
  return ok != FALSE && written == bytes.size();
}

bool ReadAllBytes(const std::wstring& path, std::string* bytes) {
  if (!bytes) return false;
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size = {};
  if (!GetFileSizeEx(file, &size) || size.QuadPart > 8 * 1024 * 1024) {
    CloseHandle(file);
    return false;
  }
  bytes->assign(static_cast<size_t>(size.QuadPart), '\0');
  DWORD read = 0;
  const BOOL ok = bytes->empty()
                      ? TRUE
                      : ReadFile(file, bytes->data(),
                                 static_cast<DWORD>(bytes->size()), &read,
                                 nullptr);
  CloseHandle(file);
  return ok != FALSE;
}

void EnsureNoteId(NoteState* note) {
  if (!note || !note->id.empty()) return;
  wchar_t buffer[64] = {};
  _snwprintf_s(buffer, _countof(buffer), _TRUNCATE, L"%llu-%lu",
               GetTickCount64(), GetCurrentProcessId());
  note->id = buffer;
}

std::wstring NoteFilePath(const NoteState* note) {
  if (!note || note->id.empty()) return {};
  return NoteFilesDirectory() + L"\\" + note->id + L".note";
}

std::wstring EditText(HWND edit) {
  if (!edit) return {};
  const int length = GetWindowTextLengthW(edit);
  if (length <= 0) return {};
  std::wstring text(static_cast<size_t>(length), L'\0');
  GetWindowTextW(edit, text.data(), length + 1);
  return text;
}

// Persists content and geometry. Called from a debounce timer, so typing does
// not touch the disk on every keystroke.
void SaveNote(NoteState* note, bool force) {
  if (!note || !note->hwnd) return;
  const std::wstring text = EditText(note->edit);
  if (!force && text == note->lastSaved) {
    note->dirty = false;
    return;
  }
  EnsureNoteId(note);

  RECT rect = {};
  GetWindowRect(note->hwnd, &rect);
  wchar_t header[160] = {};
  _snwprintf_s(header, _countof(header), _TRUNCATE,
               L"x=%ld\ny=%ld\nw=%ld\nh=%ld\n\n", rect.left, rect.top,
               rect.right - rect.left, rect.bottom - rect.top);

  std::string bytes = ToUtf8(header);
  bytes += ToUtf8(text);
  const bool ok = WriteAllBytes(NoteFilePath(note), bytes);
  LogLine(L"save note id=%s chars=%zu bytes=%zu ok=%d path=%s",
          note->id.c_str(), text.size(), bytes.size(), ok ? 1 : 0,
          NoteFilePath(note).c_str());
  if (ok) {
    note->lastSaved = text;
    note->dirty = false;
  }
}

void DeleteNoteFile(const NoteState* note) {
  if (!note || note->id.empty()) return;
  DeleteFileW(NoteFilePath(note).c_str());
}

struct LoadedNote {
  std::wstring id;
  std::wstring text;
  RECT geometry = {0, 0, kInitialWidth, kInitialHeight};
};

void ParseNoteFile(const std::wstring& id, const std::string& bytes,
                   LoadedNote* out) {
  out->id = id;
  // Split header from body on the first blank line.
  const size_t split = bytes.find("\n\n");
  if (split == std::string::npos) {
    out->text = ToWide(bytes);
    return;
  }
  const std::string header = bytes.substr(0, split);
  out->text = ToWide(bytes.substr(split + 2));

  int x = 0, y = 0, w = kInitialWidth, h = kInitialHeight;
  size_t start = 0;
  while (start < header.size()) {
    const size_t end = header.find('\n', start);
    const std::string line =
        header.substr(start, end == std::string::npos ? std::string::npos
                                                      : end - start);
    const size_t equals = line.find('=');
    if (equals != std::string::npos && equals > 0) {
      const char key = line[0];
      const int value = ParseInt(line.substr(equals + 1));
      if (key == 'x') x = value;
      else if (key == 'y') y = value;
      else if (key == 'w' && value >= kMinWidth) w = value;
      else if (key == 'h' && value >= kMinHeight) h = value;
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  out->geometry.left = x;
  out->geometry.top = y;
  out->geometry.right = x + w;
  out->geometry.bottom = y + h;
}

std::vector<LoadedNote> LoadStoredNotes() {
  std::vector<LoadedNote> notes;
  const std::wstring directory = NoteFilesDirectory();
  WIN32_FIND_DATAW data = {};
  HANDLE find = FindFirstFileW((directory + L"\\*.note").c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) return notes;
  do {
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    std::wstring name = data.cFileName;
    const size_t dot = name.rfind(L'.');
    if (dot == std::wstring::npos) continue;
    const std::wstring id = name.substr(0, dot);
    std::string bytes;
    if (!ReadAllBytes(directory + L"\\" + name, &bytes)) continue;
    LoadedNote note;
    ParseNoteFile(id, bytes, &note);
    notes.push_back(note);
  } while (FindNextFileW(find, &data));
  FindClose(find);
  return notes;
}

std::wstring DataDirectory() {
  wchar_t profile[MAX_PATH] = {};
  DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
  std::wstring result;
  if (n > 0 && n < MAX_PATH) {
    result.assign(profile, n);
  } else {
    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    result.assign(temp);
  }
  result += L"\\PaperNotesData";
  return result;
}

void LogLine(const wchar_t* format, ...);

// Chosen so user notes live outside the program directory and survive
// uninstall. LOCALAPPDATA is the conventional per-user writable location.
std::wstring LocalAppDataDirectory() {
  wchar_t base[MAX_PATH] = {};
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
  std::wstring result;
  if (n > 0 && n < MAX_PATH) {
    result.assign(base, n);
  } else {
    result = DataDirectory();
    return result;
  }
  result += L"\\PaperNotes";
  return result;
}

// Records whether this process can actually create files in a directory. A
// restricted token can make a perfectly normal user path unwritable, and that
// must be discovered now rather than when the first note fails to save.
void ProbeWritableLocation(const std::wstring& directory) {
  CreateDirectoryW(directory.c_str(), nullptr);
  const std::wstring probe = directory + L"\\.write-probe";
  SetLastError(0);
  HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
  const DWORD error = GetLastError();
  const bool ok = file != INVALID_HANDLE_VALUE;
  if (ok) {
    CloseHandle(file);
    DeleteFileW(probe.c_str());
  }
  LogLine(L"writable dir=%s ok=%d gle=%lu", directory.c_str(), ok ? 1 : 0,
          error);
}

std::wstring UserObjectName(HANDLE object) {
  if (!object) return L"<null>";
  wchar_t name[256] = {};
  DWORD needed = 0;
  if (!GetUserObjectInformationW(object, UOI_NAME, name, sizeof(name),
                                 &needed)) {
    return L"<unavailable>";
  }
  return name;
}

std::string ToUtf8(const std::wstring& text) {
  if (text.empty()) return {};
  int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                 static_cast<int>(text.size()), nullptr, 0,
                                 nullptr, nullptr);
  if (size <= 0) return {};
  std::string result(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                      static_cast<int>(text.size()), result.data(), size,
                      nullptr, nullptr);
  return result;
}

std::wstring ToWide(const std::string& text) {
  if (text.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                       static_cast<int>(text.size()), nullptr,
                                       0);
  if (size <= 0) return {};
  std::wstring result(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      result.data(), size);
  return result;
}

void LogLine(const wchar_t* format, ...) {
  wchar_t message[2048] = {};
  va_list args;
  va_start(args, format);
  _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, args);
  va_end(args);

  SYSTEMTIME now = {};
  GetLocalTime(&now);
  wchar_t prefix[128] = {};
  _snwprintf_s(prefix, _countof(prefix), _TRUNCATE,
               L"%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu ",
               now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
               now.wSecond, now.wMilliseconds, GetCurrentProcessId());

  std::wstring line(prefix);
  line += message;
  line += L"\r\n";
  OutputDebugStringW(line.c_str());

  const std::string utf8 = ToUtf8(line);
  const std::wstring paths[] = {
      DataDirectory() + L"\\phase0.log",
      L"C:\\cx_ProjectCode\\PaperNotes\\out\\diagnostics\\phase0.log",
  };
  for (const std::wstring& path : paths) {
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) continue;
    DWORD written = 0;
    WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written,
              nullptr);
    CloseHandle(file);
  }
}

// ---------------------------------------------------------------- click state

void EnsureClickMapping() {
  if (g_click) return;
  g_clickMapping =
      CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                         sizeof(ClickState), kClickStateMappingName);
  if (!g_clickMapping) {
    LogLine(L"CreateFileMapping(ClickState) failed gle=%lu", GetLastError());
    return;
  }
  const bool created = GetLastError() != ERROR_ALREADY_EXISTS;
  g_click = static_cast<ClickState*>(
      MapViewOfFile(g_clickMapping, FILE_MAP_ALL_ACCESS, 0, 0,
                    sizeof(ClickState)));
  if (!g_click) {
    LogLine(L"MapViewOfFile(ClickState) failed gle=%lu", GetLastError());
    CloseHandle(g_clickMapping);
    g_clickMapping = nullptr;
    return;
  }
  if (created) SecureZeroMemory(g_click, sizeof(ClickState));
}

void CloseClickMapping() {
  if (g_click) {
    UnmapViewOfFile(g_click);
    g_click = nullptr;
  }
  if (g_clickMapping) {
    CloseHandle(g_clickMapping);
    g_clickMapping = nullptr;
  }
}

// The low-level hook callback must stay allocation-free and I/O-free: Windows
// silently removes a hook whose callback exceeds LowLevelHooksTimeout.
LRESULT CALLBACK MouseHookProc(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION) {
    InterlockedIncrement64(&g_hookEventCount);
  }
  if (code == HC_ACTION && wparam == WM_RBUTTONDOWN) {
    const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam);
    if (info) {
      const ULONGLONG tick = GetTickCount64();
      if (g_click) {
        InterlockedExchange(&g_click->rightX, info->pt.x);
        InterlockedExchange(&g_click->rightY, info->pt.y);
        InterlockedExchange(&g_click->rightFlags,
                            static_cast<LONG>(info->flags));
        InterlockedExchange64(&g_click->rightDownTick,
                              static_cast<LONG64>(tick));
      }
      InterlockedIncrement64(&g_hookRightDownCount);
      g_lastRightDown = info->pt;
      g_lastRightDownTick = tick;
    }
  }
  return CallNextHookEx(g_mouseHook, code, wparam, lparam);
}

void InstallMouseHook() {
  if (g_mouseHook) return;
  // Must be nullptr for an in-process low-level hook.  Passing the EXE module
  // makes USER32 look for an exported DLL hook and never call the callback.
  g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, nullptr, 0);
  LogLine(L"mouse hook install=%d gle=%lu", g_mouseHook ? 1 : 0,
          GetLastError());
}

void UninstallMouseHook() {
  if (g_mouseHook) {
    UnhookWindowsHookEx(g_mouseHook);
    g_mouseHook = nullptr;
  }
}

// ------------------------------------------------------------------- geometry

RECT WorkAreaForPoint(POINT point) {
  HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
  MONITORINFO info = {};
  info.cbSize = sizeof(info);
  if (monitor && GetMonitorInfoW(monitor, &info)) return info.rcWork;
  RECT fallback = {};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &fallback, 0);
  return fallback;
}

POINT ClampToWorkArea(POINT desired, int width, int height) {
  const RECT work = WorkAreaForPoint(desired);
  const int minX = work.left + 6;
  const int minY = work.top + 6;
  const int maxX =
      std::max(minX, static_cast<int>(work.right) - width - 6);
  const int maxY =
      std::max(minY, static_cast<int>(work.bottom) - height - 6);
  POINT result = {};
  result.x = std::min(std::max(static_cast<int>(desired.x), minX), maxX);
  result.y = std::min(std::max(static_cast<int>(desired.y), minY), maxY);
  return result;
}

bool RectsOverlap(const RECT& a, const RECT& b) {
  return a.left < b.right && b.left < a.right && a.top < b.bottom &&
         b.top < a.bottom;
}

RECT RectOf(const NoteState* note) {
  RECT rect = {};
  if (note && note->hwnd) GetWindowRect(note->hwnd, &rect);
  return rect;
}

// Notes opened at the same anchor must not hide each other.
POINT CascadeIfOverlapping(POINT desired, int width, int height,
                           const NoteState* ignore) {
  POINT candidate = desired;
  for (int step = 0; step <= kMaxCascade; ++step) {
    RECT probe = {candidate.x, candidate.y, candidate.x + width,
                  candidate.y + height};
    bool clash = false;
    for (NoteState* note : g_notes) {
      if (note == ignore || !note->hwnd) continue;
      if (!IsWindowVisible(note->hwnd)) continue;
      if (RectsOverlap(probe, RectOf(note))) {
        clash = true;
        break;
      }
    }
    if (!clash) return ClampToWorkArea(candidate, width, height);
    candidate.x += kCascadeStep;
    candidate.y += kCascadeStep;
    candidate = ClampToWorkArea(candidate, width, height);
  }
  return ClampToWorkArea(candidate, width, height);
}

// Places the note beside the click, flipping to the other side when the note
// would fall outside the monitor's work area.
POINT NotePositionForAnchor(POINT anchor, int width, int height) {
  const RECT work = WorkAreaForPoint(anchor);
  const int gap = 12;
  POINT desired = {anchor.x + gap, anchor.y + gap};
  if (desired.x + width > work.right) desired.x = anchor.x - width - gap;
  if (desired.y + height > work.bottom) desired.y = anchor.y - height - gap;
  return ClampToWorkArea(desired, width, height);
}

// ---------------------------------------------------------------- anchor choice

struct Anchor {
  POINT point = {};
  AnchorSource source = AnchorSource::kFallback;
  LONG64 tick = 0;
  POINT other = {};
  LONG otherDistance = -1;
  bool hasOther = false;
};

LONG Distance(POINT a, POINT b) {
  const LONG dx = a.x - b.x;
  const LONG dy = a.y - b.y;
  return static_cast<LONG>(sqrt(static_cast<double>(dx) * dx +
                                static_cast<double>(dy) * dy));
}

// Picks the original right-click point.  Never returns the position of the
// mouse at the moment the menu item was clicked: both candidates are sampled
// before or at the right-button-down, and each recording is consumed once.
Anchor ChooseAnchor() {
  Anchor anchor;
  const ULONGLONG now = GetTickCount64();

  LONG64 rightTick = 0;
  LONG64 buildTick = 0;
  POINT rightPoint = {};
  POINT buildPoint = {};
  if (g_click) {
    rightTick = InterlockedCompareExchange64(&g_click->rightDownTick, 0, 0);
    buildTick = InterlockedCompareExchange64(&g_click->menuBuildTick, 0, 0);
    rightPoint.x = InterlockedCompareExchange(&g_click->rightX, 0, 0);
    rightPoint.y = InterlockedCompareExchange(&g_click->rightY, 0, 0);
    buildPoint.x = InterlockedCompareExchange(&g_click->menuBuildX, 0, 0);
    buildPoint.y = InterlockedCompareExchange(&g_click->menuBuildY, 0, 0);
  }

  const bool hookFresh =
      rightTick != 0 && rightTick != g_consumedRightDownTick &&
      now >= static_cast<ULONGLONG>(rightTick) &&
      now - static_cast<ULONGLONG>(rightTick) <= kMaxAnchorAgeMs;
  const bool buildFresh =
      buildTick != 0 && buildTick != g_consumedMenuBuildTick &&
      now >= static_cast<ULONGLONG>(buildTick) &&
      now - static_cast<ULONGLONG>(buildTick) <= kMaxAnchorAgeMs;

  if (hookFresh && buildFresh) {
    anchor.hasOther = true;
    anchor.other = buildPoint;
    anchor.otherDistance = Distance(rightPoint, buildPoint);
    if (anchor.otherDistance <= kAgreementTolerancePx) {
      // The two independent sources describe the same click.
      anchor.point = rightPoint;
      anchor.source = AnchorSource::kHookRightDown;
      anchor.tick = rightTick;
      g_consumedRightDownTick = rightTick;
      g_consumedMenuBuildTick = buildTick;
      return anchor;
    }
    // They disagree: the hook may belong to an unrelated right-click while the
    // menu-build sample is tied to this exact menu.
    anchor.point = buildPoint;
    anchor.source = AnchorSource::kMenuBuild;
    anchor.tick = buildTick;
    g_consumedRightDownTick = rightTick;
    g_consumedMenuBuildTick = buildTick;
    return anchor;
  }

  if (hookFresh) {
    anchor.point = rightPoint;
    anchor.source = AnchorSource::kHookRightDown;
    anchor.tick = rightTick;
    g_consumedRightDownTick = rightTick;
    if (buildTick != 0) g_consumedMenuBuildTick = buildTick;
    return anchor;
  }

  if (buildFresh) {
    anchor.point = buildPoint;
    anchor.source = AnchorSource::kMenuBuild;
    anchor.tick = buildTick;
    g_consumedMenuBuildTick = buildTick;
    if (rightTick != 0) g_consumedRightDownTick = rightTick;
    return anchor;
  }

  // Nothing usable: fall back to a deterministic spot on the cursor's monitor
  // so the note is still visible and does not stack on the previous one.
  POINT cursor = {};
  GetCursorPos(&cursor);
  const RECT work = WorkAreaForPoint(cursor);
  anchor.point.x = work.left + 90 + static_cast<LONG>(g_notes.size()) * 20;
  anchor.point.y = work.top + 90 + static_cast<LONG>(g_notes.size()) * 20;
  anchor.source = AnchorSource::kFallback;
  return anchor;
}

// ---------------------------------------------------------------- note window

HFONT CreateNoteFont(HWND hwnd) {
  const UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 96;
  return CreateFontW(-MulDiv(11, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL,
                     FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                     DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

void ApplyRoundedCorners(HWND hwnd) {
  enum : DWORD { kCornerPreference = 33, kRound = 2 };
  DWORD preference = kRound;
  DwmSetWindowAttribute(hwnd, kCornerPreference, &preference,
                        sizeof(preference));
}

void LayoutNote(NoteState* state) {
  if (!state || !state->edit) return;
  const NoteLayout layout = LayoutFor(state->dpi);
  RECT client = {};
  GetClientRect(state->hwnd, &client);
  MoveWindow(state->edit, layout.edge, layout.titleHeight,
             client.right - layout.edge * 2,
             client.bottom - layout.titleHeight - layout.edge, TRUE);
}

// Header button geometry. Both buttons share one square size so the glyphs
// read as a control group; the square itself is the hit area, which is 42x42
// physical pixels at 150% -- comfortably above a 32px pointer target.
RECT NoteButtonRect(const NoteState* state, int which) {
  RECT client = {};
  GetClientRect(state->hwnd, &client);
  const NoteLayout layout = LayoutFor(state->dpi);
  const int size = ScaleFor(state->dpi, kNoteButtonDip);
  const int gap = ScaleFor(state->dpi, kNoteButtonGapDip);
  const int rightPad = ScaleFor(state->dpi, 8);
  const int right = client.right - rightPad - (which == 1 ? 0 : size + gap);
  const int top = (layout.titleHeight - size) / 2;
  RECT rect = {right - size, top, right, top + size};
  return rect;
}

// -1 = not over a button, 0 = hide, 1 = delete.
int NoteButtonAt(const NoteState* state, POINT pt) {
  for (int which = 0; which < 2; ++which) {
    RECT rect = NoteButtonRect(state, which);
    if (PtInRect(&rect, pt)) return which;
  }
  return -1;
}

// Inner padding for the text.  EM_SETMARGINS only does left/right, so the top
// and bottom breathing room has to come from the formatting rectangle.
void ApplyNoteTextRect(NoteState* state) {
  if (!state || !state->edit) return;
  RECT client = {};
  GetClientRect(state->edit, &client);
  const int pad = ScaleFor(state->dpi, kNoteTextPadDip);
  RECT format = {pad, pad, client.right - pad, client.bottom - pad};
  if (format.right <= format.left) format.right = format.left + 1;
  if (format.bottom <= format.top) format.bottom = format.top + 1;
  SendMessageW(state->edit, EM_SETRECT, 0,
               reinterpret_cast<LPARAM>(&format));
}

// A classic scrollbar is heavy on a paper note and most notes are short, so
// show it only when the wrapped text actually overflows.  EM_POSFROMCHAR past
// the last character returns the y the next line would start at, which accounts
// for wrapping and is what EM_GETLINECOUNT alone cannot tell us.
void UpdateNoteScrollbar(NoteState* state) {
  if (!state || !state->edit) return;

  const int length = GetWindowTextLengthW(state->edit);

  RECT client = {};
  GetClientRect(state->edit, &client);

  int lineHeight = 0;
  HDC dc = GetDC(state->edit);
  if (dc) {
    TEXTMETRICW metrics = {};
    HGDIOBJ previous = state->font ? SelectObject(dc, state->font) : nullptr;
    GetTextMetricsW(dc, &metrics);
    if (previous) SelectObject(dc, previous);
    lineHeight = metrics.tmHeight + metrics.tmExternalLeading;
  }

  // Measure the text the way the edit itself lays it out.  EM_POSFROMCHAR was
  // tried first and always returned -1 here, and EM_GETLINECOUNT only counts
  // hard line breaks -- it misses wrapping, which is the common case for a note.
  // DT_EDITCONTROL makes DrawText wrap and clip exactly like a multiline edit.
  const int pad = ScaleFor(state->dpi, kNoteTextPadDip);
  const int availableHeight = (client.bottom - pad * 2) > 0 ? client.bottom - pad * 2 : 1;
  bool needed = false;
  if (length > 0 && lineHeight > 0 && dc) {
    if (length > 65536) {
      needed = true;  // far beyond any plausible window; skip the measurement
    } else {
      std::wstring text(static_cast<size_t>(length) + 1, L'\0');
      GetWindowTextW(state->edit, text.data(), length + 1);
      text.resize(static_cast<size_t>(length));
      RECT calc = {0, 0, (client.right - pad * 2) > 0 ? client.right - pad * 2 : 1, 0};
      HGDIOBJ previous = state->font ? SelectObject(dc, state->font) : nullptr;
      DrawTextW(dc, text.c_str(), length, &calc,
                DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS |
                    DT_EDITCONTROL);
      if (previous) SelectObject(dc, previous);
      needed = calc.bottom > availableHeight;
    }
  }
  if (dc) ReleaseDC(state->edit, dc);

  const LONG_PTR style = GetWindowLongPtrW(state->edit, GWL_STYLE);
  const bool has = (style & WS_VSCROLL) != 0;
  if (needed == has) return;

  SetWindowLongPtrW(state->edit, GWL_STYLE,
                    needed ? (style | WS_VSCROLL) : (style & ~WS_VSCROLL));
  // A style change alone does not make the edit re-measure itself.
  SetWindowPos(state->edit, nullptr, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
  InvalidateRect(state->edit, nullptr, TRUE);
}

// Shared owner-draw for every BUTTON in the list and settings windows.  The
// stock Win32 button renders as a raised grey box, which looks like a different
// product next to the paper notes.
void PaintPaperButton(const DRAWITEMSTRUCT* item) {
  if (!item) return;
  RECT rect = item->rcItem;
  const bool disabled = (item->itemState & ODS_DISABLED) != 0;
  const bool pressed = (item->itemState & ODS_SELECTED) != 0;

  HBRUSH background = CreateSolidBrush(pressed ? kPaperEdge : kPaper);
  FillRect(item->hDC, &rect, background);
  DeleteObject(background);

  HPEN pen = CreatePen(PS_SOLID, 1, disabled ? kPaperEdge : kInkMuted);
  HGDIOBJ oldPen = SelectObject(item->hDC, pen);
  HGDIOBJ oldBrush = SelectObject(item->hDC, GetStockObject(NULL_BRUSH));
  RoundRect(item->hDC, rect.left, rect.top, rect.right, rect.bottom,
            ScaleFor(96, 6), ScaleFor(96, 6));
  SelectObject(item->hDC, oldBrush);
  SelectObject(item->hDC, oldPen);
  DeleteObject(pen);

  wchar_t label[128] = {};
  GetWindowTextW(item->hwndItem, label, _countof(label));

  const int previousMode = SetBkMode(item->hDC, TRANSPARENT);
  SetTextColor(item->hDC, disabled ? kInkMuted : kInk);
  HFONT font = reinterpret_cast<HFONT>(
      SendMessageW(item->hwndItem, WM_GETFONT, 0, 0));
  HGDIOBJ oldFont = font ? SelectObject(item->hDC, font) : nullptr;
  DrawTextW(item->hDC, label, -1, &rect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  if (oldFont) SelectObject(item->hDC, oldFont);

  // Keyboard focus must stay visible; a filled background alone does not say
  // which button has it.
  if ((item->itemState & ODS_FOCUS) != 0 && !disabled) {
    RECT inner = rect;
    InflateRect(&inner, -3, -3);
    DrawFocusRect(item->hDC, &inner);
  }
  SetBkMode(item->hDC, previousMode);
}

void PaintNoteButton(HDC dc, const NoteState* state, int which) {
  RECT rect = NoteButtonRect(state, which);
  const bool hot = state->hoverButton == which;
  const bool down = state->pressButton == which;

  // Press and hover only change fill, never geometry, so nothing shifts.
  if (hot || down) {
    HBRUSH brush = CreateSolidBrush(down ? kPaperEdge : kPaperHover);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom,
              ScaleFor(state->dpi, 6), ScaleFor(state->dpi, 6));
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(brush);
  }

  // Delete is the destructive one: warm it only on hover, otherwise the two
  // buttons would compete for attention on a surface that is mostly text.
  const COLORREF color = (which == 1 && hot) ? kDanger : kInkMuted;
  const int stroke = ScaleFor(state->dpi, 1) < 1 ? 1 : ScaleFor(state->dpi, 1);
  HPEN pen = CreatePen(PS_SOLID, stroke, color);
  HGDIOBJ oldPen = SelectObject(dc, pen);

  const int cx = (rect.left + rect.right) / 2;
  const int cy = (rect.top + rect.bottom) / 2;
  const int arm = ScaleFor(state->dpi, 5);

  // Drawn as lines rather than a font glyph: guarantees identical stroke weight
  // on both buttons and avoids depending on glyph coverage in YaHei.
  if (which == 0) {
    MoveToEx(dc, cx - arm, cy, nullptr);
    LineTo(dc, cx + arm + 1, cy);
  } else {
    MoveToEx(dc, cx - arm, cy - arm, nullptr);
    LineTo(dc, cx + arm + 1, cy + arm + 1);
    MoveToEx(dc, cx + arm, cy - arm, nullptr);
    LineTo(dc, cx - arm - 1, cy + arm + 1);
  }

  SelectObject(dc, oldPen);
  DeleteObject(pen);
}

void PaintNote(NoteState* state) {
  HWND hwnd = state->hwnd;
  PAINTSTRUCT ps = {};
  HDC dc = BeginPaint(hwnd, &ps);
  RECT client = {};
  GetClientRect(hwnd, &client);

  HBRUSH paperBrush = CreateSolidBrush(kPaper);
  FillRect(dc, &client, paperBrush);
  DeleteObject(paperBrush);

  const NoteLayout layout = LayoutFor(state->dpi);

  // The header tints with focus so several notes can be told apart at a glance.
  RECT stripRect = {0, 0, client.right, layout.titleHeight};
  HBRUSH stripBrush =
      CreateSolidBrush(state->active ? kPaperStrip : kPaperStripIdle);
  FillRect(dc, &stripRect, stripBrush);
  DeleteObject(stripBrush);

  // Hairline instead of a hard rule: separates title from text without cutting
  // the note in two.
  HPEN hairline = CreatePen(PS_SOLID, 1, kPaperEdge);
  HGDIOBJ oldPen = SelectObject(dc, hairline);
  MoveToEx(dc, 0, layout.titleHeight - 1, nullptr);
  LineTo(dc, client.right, layout.titleHeight - 1);
  SelectObject(dc, oldPen);
  DeleteObject(hairline);

  HPEN borderPen = CreatePen(PS_SOLID, 1, kPaperEdge);
  oldPen = SelectObject(dc, borderPen);
  HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
  Rectangle(dc, 0, 0, client.right, client.bottom);
  SelectObject(dc, oldBrush);
  SelectObject(dc, oldPen);
  DeleteObject(borderPen);

  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, kInkMuted);
  HFONT titleFont = CreateFontW(
      -MulDiv(10, static_cast<int>(GetDpiForWindow(hwnd)), 72), 0, 0, 0,
      FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
      CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
      L"Microsoft YaHei UI");
  HGDIOBJ oldFont = SelectObject(dc, titleFont);
  RECT titleRect = {layout.edge, 0,
                    NoteButtonRect(state, 0).left - ScaleFor(state->dpi, 6),
                    layout.titleHeight};
  // Escapes rather than literals: the title must not depend on how the compiler
  // decodes this file.
  DrawTextW(dc, L"\u4FBF\u7B7E", -1, &titleRect,
            DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
  SelectObject(dc, oldFont);
  DeleteObject(titleFont);

  PaintNoteButton(dc, state, 0);  // hide
  PaintNoteButton(dc, state, 1);  // delete

  EndPaint(hwnd, &ps);
}

void RemoveNoteFromList(NoteState* state) {
  g_notes.erase(std::remove(g_notes.begin(), g_notes.end(), state),
                g_notes.end());
}

LRESULT CALLBACK NoteWndProc(HWND hwnd, UINT message, WPARAM wparam,
                             LPARAM lparam) {
  NoteState* state =
      reinterpret_cast<NoteState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

  switch (message) {
    case WM_NCCREATE: {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
      state = static_cast<NoteState*>(cs->lpCreateParams);
      state->hwnd = hwnd;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(state));
      return TRUE;
    }

    case WM_CREATE:
      if (state) {
        state->edit = CreateWindowExW(
            0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
                ES_WANTRETURN | ES_NOHIDESEL,
            kEdge, kTitleHeight, state->width - kEdge * 2,
            state->height - kTitleHeight - kEdge, hwnd, nullptr, g_instance,
            nullptr);
        state->font = CreateNoteFont(hwnd);
        if (state->edit) {
          if (state->font) {
            SendMessageW(state->edit, WM_SETFONT,
                         reinterpret_cast<WPARAM>(state->font), TRUE);
          }
          SendMessageW(state->edit, EM_SETMARGINS,
                       EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(10, 10));
          SendMessageW(state->edit, EM_SETLIMITTEXT, 1024 * 1024, 0);
          if (!state->initialText.empty()) {
            SetWindowTextW(state->edit, state->initialText.c_str());
            // Restored content is already on disk; do not re-save immediately.
            state->lastSaved = state->initialText;
          } else {
            SetWindowTextW(state->edit, L"");
          }
        }
        ApplyRoundedCorners(hwnd);
        LayoutNote(state);
        ApplyNoteTextRect(state);
        UpdateNoteScrollbar(state);
      }
      return 0;

    case WM_SIZE:
      LayoutNote(state);
      ApplyNoteTextRect(state);
      UpdateNoteScrollbar(state);
      return 0;

    case WM_ACTIVATE:
      if (state) {
        const bool active = LOWORD(wparam) != WA_INACTIVE;
        if (state->active != active) {
          state->active = active;
          InvalidateRect(hwnd, nullptr, FALSE);
        }
      }
      return 0;

    case WM_DPICHANGED:
      if (state) {
        state->dpi = HIWORD(wparam);
        const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        HFONT replacement = CreateNoteFont(hwnd);
        if (replacement) {
          if (state->edit) {
            SendMessageW(state->edit, WM_SETFONT,
                         reinterpret_cast<WPARAM>(replacement), TRUE);
          }
          if (state->font) DeleteObject(state->font);
          state->font = replacement;
        }
        LayoutNote(state);
      }
      return 0;

    case WM_PAINT:
      if (state) PaintNote(state);
      return 0;

    case WM_ERASEBKGND:
      return 1;

    case WM_CTLCOLOREDIT: {
      HDC dc = reinterpret_cast<HDC>(wparam);
      SetTextColor(dc, kInk);
      SetBkColor(dc, kPaper);
      if (!g_editBrush) g_editBrush = CreateSolidBrush(kPaper);
      return reinterpret_cast<LRESULT>(g_editBrush);
    }

    // Hover feedback for the two header buttons. TrackMouseEvent is required or
    // WM_MOUSELEAVE never arrives and a hovered button stays lit after exit.
    case WM_MOUSEMOVE: {
      if (state) {
        POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const int which = NoteButtonAt(state, pt);
        if (which != state->hoverButton) {
          state->hoverButton = which;
          InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (!state->trackingMouse) {
          TRACKMOUSEEVENT track = {};
          track.cbSize = sizeof(track);
          track.dwFlags = TME_LEAVE;
          track.hwndTrack = hwnd;
          TrackMouseEvent(&track);
          state->trackingMouse = true;
        }
      }
      return 0;
    }

    case WM_MOUSELEAVE:
      if (state) {
        state->trackingMouse = false;
        if (state->hoverButton != -1 || state->pressButton != -1) {
          state->hoverButton = -1;
          state->pressButton = -1;
          InvalidateRect(hwnd, nullptr, FALSE);
        }
      }
      return 0;

    case WM_SETCURSOR:
      if (state && LOWORD(lparam) == HTCLIENT && state->hoverButton >= 0) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
      }
      break;

    case WM_LBUTTONDOWN: {
      if (state) {
        POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        const int which = NoteButtonAt(state, pt);
        if (which >= 0) {
          state->pressButton = which;
          InvalidateRect(hwnd, nullptr, FALSE);
        }
      }
      break;
    }

    case WM_LBUTTONUP: {
      POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      const int which = state ? NoteButtonAt(state, pt) : -1;
      if (state && (state->pressButton != -1 || which != -1)) {
        state->pressButton = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      if (which == 1) {
        // Delete, with confirmation: it is the only destructive action.
        if (MessageBoxW(hwnd,
                        L"\u5220\u9664\u8FD9\u5F20\u4FBF\u7B7E\uFF1F\n\n"
                        L"\u5220\u9664\u540E\u65E0\u6CD5\u6062\u590D\u3002",
                        L"\u5220\u9664\u4FBF\u7B7E",
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
          if (state) DeleteNoteFile(state);
          // WM_DESTROY unlinks the state from g_notes; do not do it here too.
          DestroyWindow(hwnd);
        }
        return 0;
      }
      if (which == 0) {
        // Hide. Content stays saved and the note stays in the list.
        SaveNote(state, true);
        ShowWindow(hwnd, SW_HIDE);
        return 0;
      }
      break;
    }

    case WM_NCHITTEST: {
      POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ScreenToClient(hwnd, &pt);
      RECT client = {};
      GetClientRect(hwnd, &client);
      const NoteLayout layout = LayoutFor(state ? state->dpi : 96);
      if (pt.y >= 0 && pt.y <= layout.buttonBottom) {
        if (pt.x >= client.right - layout.closeWidth) return HTCLIENT;
        if (pt.x >= client.right - layout.closeWidth - layout.deleteWidth) {
          return HTCLIENT;
        }
      }
      if (pt.y < layout.titleHeight) return HTCAPTION;
      return HTCLIENT;
    }

    case WM_COMMAND:
      if (state && HIWORD(wparam) == EN_CHANGE &&
          reinterpret_cast<HWND>(lparam) == state->edit) {
        state->dirty = true;
        // The scrollbar appears as soon as the text outgrows the window.
        UpdateNoteScrollbar(state);
        // Debounce: typing must never touch the disk per keystroke.
        SetTimer(hwnd, 2, 600, nullptr);
        return 0;
      }
      break;

    case WM_TIMER:
      if (wparam == 2) {
        KillTimer(hwnd, 2);
        SaveNote(state, false);
        return 0;
      }
      break;

    case WM_EXITSIZEMOVE:
      // Geometry lives in the same file, so a move or resize is persisted too.
      SaveNote(state, false);
      return 0;

    case WM_CLOSE:
      SaveNote(state, true);
      ShowWindow(hwnd, SW_HIDE);
      return 0;

    case WM_GETMINMAXINFO: {
      auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
      info->ptMinTrackSize.x = kMinWidth;
      info->ptMinTrackSize.y = kMinHeight;
      return 0;
    }

    case WM_DESTROY:
      if (state) {
        if (state->font) DeleteObject(state->font);
        RemoveNoteFromList(state);
        delete state;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      }
      return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

void RegisterNoteClass() {
  static bool registered = false;
  if (registered) return;
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.style = CS_DROPSHADOW;
  wc.lpfnWndProc = NoteWndProc;
  wc.hInstance = g_instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  // Without this the note falls back to the generic application icon in
  // Alt+Tab and in the window preview.
  wc.hIcon = LoadAppIcon(GetSystemMetrics(SM_CXICON),
                         GetSystemMetrics(SM_CYICON));
  wc.hIconSm = LoadAppIcon(GetSystemMetrics(SM_CXSMICON),
                           GetSystemMetrics(SM_CYSMICON));
  wc.lpszClassName = kNoteClass;
  RegisterClassExW(&wc);
  registered = true;
}

void BubbleNoteToFront(NoteState* note) {
  if (!note || !note->hwnd) return;
  ShowWindow(note->hwnd, SW_SHOW);
  // The app is not the foreground process when a menu item starts a note, so
  // SetForegroundWindow cannot be relied on. Raising through HWND_TOPMOST and
  // immediately dropping back to HWND_NOTOPMOST brings the window above the
  // other windows without stealing focus or leaving it always-on-top.
  SetWindowPos(note->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
  SetWindowPos(note->hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
  if (note->edit) SetFocus(note->edit);
}

// Every invocation creates a NEW note window; notes are independent.
// Creates the window for a note. Shared by "new note" and "restore at startup"
// so both paths get identical styling, autosave and deletion behaviour.
bool CreateNoteWindow(NoteState* state, POINT position, bool activate) {
  RegisterNoteClass();
  HWND hwnd = CreateWindowExW(
      WS_EX_TOOLWINDOW, kNoteClass, kAppTitle,
      WS_POPUP | WS_CLIPCHILDREN | WS_THICKFRAME, position.x, position.y,
      state->width, state->height, nullptr, nullptr, g_instance, state);
  if (!hwnd) {
    LogLine(L"CreateWindowEx(note) failed gle=%lu", GetLastError());
    delete state;
    return false;
  }
  g_notes.push_back(state);
  if (activate) {
    BubbleNoteToFront(state);
  }
  return true;
}

void CreateNoteFromAnchor() {

  const Anchor anchor = ChooseAnchor();
  // Latency the user actually feels: from the moment the menu item invoked the
  // command (stamped by the shell extension) to the note window existing.
  ULONGLONG clickToNoteMs = 0;
  if (g_click) {
    const LONG64 invokeTick =
        InterlockedCompareExchange64(&g_click->invokeTick, 0, 0);
    const ULONGLONG nowTick = GetTickCount64();
    if (invokeTick != 0 && nowTick >= static_cast<ULONGLONG>(invokeTick)) {
      clickToNoteMs = nowTick - static_cast<ULONGLONG>(invokeTick);
    }
  }
  // Size in device-independent pixels, scaled for the monitor being used, so
  // the note looks the same physical size on any display and its DPI-scaled
  // text always fits.
  const UINT dpi = DpiForPoint(anchor.point);
  const int width = ScaleFor(dpi, kInitialWidth);
  const int height = ScaleFor(dpi, kInitialHeight);

  const POINT base = NotePositionForAnchor(anchor.point, width, height);
  const POINT target =
      CascadeIfOverlapping(base, width, height, nullptr);

  auto* state = new (std::nothrow) NoteState();
  if (!state) {
    LogLine(L"note state allocation failed");
    return;
  }
  state->dpi = dpi;
  state->width = width;
  state->height = height;

  const ULONGLONG now = GetTickCount64();
  wchar_t info[256] = {};
  const ULONGLONG age =
      anchor.tick != 0 && now >= static_cast<ULONGLONG>(anchor.tick)
          ? now - static_cast<ULONGLONG>(anchor.tick)
          : 0;
  if (anchor.hasOther) {
    _snwprintf_s(info, _countof(info), _TRUNCATE,
                 L"%s · 点击(%ld,%ld) 锚点(%ld,%ld) 差 %ldpx",
                 AnchorSourceName(anchor.source), anchor.point.x,
                 anchor.point.y, anchor.other.x, anchor.other.y,
                 anchor.otherDistance);
  } else {
    _snwprintf_s(info, _countof(info), _TRUNCATE, L"%s · 锚点(%ld,%ld)",
                 AnchorSourceName(anchor.source), anchor.point.x,
                 anchor.point.y);
  }
  state->info = info;

  if (!CreateNoteWindow(state, target, true)) return;

  LogLine(L"note #%zu created source=%s anchor=(%ld,%ld) window=(%ld,%ld) "
          L"anchorAgeMs=%llu clickToNoteMs=%llu",
          g_notes.size(), AnchorSourceName(anchor.source), anchor.point.x,
          anchor.point.y, target.x, target.y, age, clickToNoteMs);
}

// Recreates every stored note WITHOUT showing it: the requirement is that a
// launch never scatters old notes across the desktop. They become reachable
// from the tray list instead.
void RestoreStoredNotes() {
  const std::vector<LoadedNote> stored = LoadStoredNotes();
  if (stored.empty()) {
    LogLine(L"restore: no stored notes in %s", StoreDirectory().c_str());
    return;
  }
  for (const LoadedNote& loaded : stored) {
    auto* state = new (std::nothrow) NoteState();
    if (!state) break;
    const POINT stored = {loaded.geometry.left, loaded.geometry.top};
    const UINT dpi = DpiForPoint(stored);
    const int width = std::max(ScaleFor(dpi, kMinWidth),
                               static_cast<int>(loaded.geometry.right -
                                                loaded.geometry.left));
    const int height = std::max(ScaleFor(dpi, kMinHeight),
                                static_cast<int>(loaded.geometry.bottom -
                                                 loaded.geometry.top));
    const POINT position = ClampToWorkArea(stored, width, height);
    state->dpi = dpi;
    state->width = width;
    state->height = height;
    state->id = loaded.id;
    state->initialText = loaded.text;
    state->lastSaved = loaded.text;
    state->info = L"已保存";
    if (!CreateNoteWindow(state, position, false)) break;
  }
  LogLine(L"restore: %zu note(s) loaded hidden from %s", g_notes.size(),
          StoreDirectory().c_str());
}

void SaveAllNotes() {
  for (NoteState* note : g_notes) SaveNote(note, true);
}

void CreateNoteNearCursor() {
  POINT cursor = {};
  GetCursorPos(&cursor);
  const UINT dpi = DpiForPoint(cursor);
  const int width = ScaleFor(dpi, kInitialWidth);
  const int height = ScaleFor(dpi, kInitialHeight);
  const POINT base = NotePositionForAnchor(cursor, width, height);
  const POINT target = CascadeIfOverlapping(base, width, height, nullptr);

  RegisterNoteClass();
  auto* state = new (std::nothrow) NoteState();
  if (!state) return;
  state->dpi = dpi;
  state->width = width;
  state->height = height;
  state->info = L"tray · 光标位置";

  HWND hwnd = CreateWindowExW(
      WS_EX_TOOLWINDOW, kNoteClass, kAppTitle,
      WS_POPUP | WS_CLIPCHILDREN | WS_THICKFRAME, target.x, target.y,
      state->width, state->height, nullptr, nullptr, g_instance, state);
  if (!hwnd) {
    delete state;
    return;
  }
  g_notes.push_back(state);
  BubbleNoteToFront(state);
  LogLine(L"note #%zu created from tray cursor=(%ld,%ld)", g_notes.size(),
          target.x, target.y);
}

size_t VisibleNoteCount() {
  size_t count = 0;
  for (NoteState* note : g_notes) {
    if (note->hwnd && IsWindowVisible(note->hwnd)) ++count;
  }
  return count;
}

// ----------------------------------------------------------------- tray icon

// Application icon, compiled in from PaperNotesApp.rc.  Loading it at an
// explicit size matters: the tray asks for SM_CXSMICON (16px at 100%, 24px at
// 150%), and letting Windows pick a different size makes the glyph look soft.
HICON LoadAppIcon(int width, int height) {
  return reinterpret_cast<HICON>(
      LoadImageW(g_instance, MAKEINTRESOURCEW(kAppIconId), IMAGE_ICON,
                 width > 0 ? width : 16, height > 0 ? height : 16,
                 LR_DEFAULTCOLOR));
}

void AddTrayIcon() {
  if (g_trayAdded) return;
  ZeroMemory(&g_tray, sizeof(g_tray));
  g_tray.cbSize = sizeof(g_tray);
  g_tray.hWnd = g_hidden;
  g_tray.uID = kTrayId;
  g_tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  g_tray.uCallbackMessage = kMsgTray;
  // Load the application icon at the size the tray actually uses.  It used to be
  // LoadIconW(nullptr, IDI_APPLICATION), i.e. the generic Windows application
  // icon, which is why the tray showed a blank look-alike instead of PaperNotes.
  g_tray.hIcon = LoadAppIcon(GetSystemMetrics(SM_CXSMICON),
                             GetSystemMetrics(SM_CYSMICON));
  if (!g_tray.hIcon) {
    g_tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  }
  wcscpy_s(g_tray.szTip, kAppTitle);
  if (Shell_NotifyIconW(NIM_ADD, &g_tray)) {
    g_trayAdded = true;
    LogLine(L"tray icon added");
  } else {
    LogLine(L"Shell_NotifyIcon(NIM_ADD) failed gle=%lu", GetLastError());
  }
}

void RemoveTrayIcon() {
  if (!g_trayAdded) return;
  Shell_NotifyIconW(NIM_DELETE, &g_tray);
  g_trayAdded = false;
}

std::wstring NotePreview(const NoteState* note) {
  std::wstring text = note ? EditText(note->edit) : std::wstring();
  if (text.empty()) text = note ? note->lastSaved : std::wstring();
  // First non-empty line, trimmed and shortened for the menu.
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find(L'\n', start);
    if (end == std::wstring::npos) end = text.size();
    std::wstring line = text.substr(start, end - start);
    while (!line.empty() && (line.back() == L'\r' || line.back() == L' ' ||
                             line.back() == L'\t')) {
      line.pop_back();
    }
    size_t first = line.find_first_not_of(L" \t");
    if (first != std::wstring::npos) {
      line = line.substr(first);
      if (line.size() > 24) line = line.substr(0, 24) + L"…";
      return line;
    }
    if (end == text.size()) break;
    start = end + 1;
  }
  return L"（空白便签）";
}

// ------------------------------------------------------------- notes list
//
// The tray icon is not a dependable entry point: Win11 hides new tray icons in
// the overflow area by default, and a restricted session can refuse
// Shell_NotifyIcon entirely. This window is the guaranteed way back to stored
// notes, reachable from a global hotkey and from the tray when it exists.

constexpr int kListIdListbox = 100;
constexpr int kListIdNew = 101;
constexpr int kListIdOpen = 102;
constexpr int kListIdDelete = 103;
constexpr int kListIdClose = 104;
constexpr int kListIdSettings = 105;

HWND g_list = nullptr;
HWND g_listListbox = nullptr;
HFONT g_listFont = nullptr;

void RefreshNotesList() {
  if (!g_listListbox) return;
  SendMessageW(g_listListbox, LB_RESETCONTENT, 0, 0);
  for (size_t index = 0; index < g_notes.size(); ++index) {
    NoteState* note = g_notes[index];
    std::wstring label = NotePreview(note);
    if (note->hwnd && IsWindowVisible(note->hwnd)) label = L"● " + label;
    const LRESULT item = SendMessageW(
        g_listListbox, LB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(label.c_str()));
    if (item >= 0) {
      SendMessageW(g_listListbox, LB_SETITEMDATA, static_cast<WPARAM>(item),
                   static_cast<LPARAM>(index));
    }
  }
}

int SelectedNoteIndex() {
  if (!g_listListbox) return -1;
  const LRESULT selection = SendMessageW(g_listListbox, LB_GETCURSEL, 0, 0);
  if (selection < 0) return -1;
  const LRESULT data =
      SendMessageW(g_listListbox, LB_GETITEMDATA, selection, 0);
  if (data < 0 || static_cast<size_t>(data) >= g_notes.size()) return -1;
  return static_cast<int>(data);
}

void LayoutNotesList() {
  if (!g_list) return;
  RECT client = {};
  GetClientRect(g_list, &client);
  const UINT dpi = GetDpiForWindow(g_list);
  const int margin = ScaleFor(dpi, 12);
  const int buttonHeight = ScaleFor(dpi, 30);
  const int buttonWidth = ScaleFor(dpi, 78);
  const int listBottom = client.bottom - margin - buttonHeight - margin;
  if (g_listListbox) {
    MoveWindow(g_listListbox, margin, margin, client.right - margin * 2,
               listBottom - margin, TRUE);
  }
  int x = margin;
  const int y = listBottom + margin;
  HWND buttons[] = {GetDlgItem(g_list, kListIdNew),
                    GetDlgItem(g_list, kListIdOpen),
                    GetDlgItem(g_list, kListIdSettings),
                    GetDlgItem(g_list, kListIdDelete)};
  for (HWND button : buttons) {
    if (!button) continue;
    MoveWindow(button, x, y, buttonWidth, buttonHeight, TRUE);
    x += buttonWidth + ScaleFor(dpi, 8);
  }
  HWND close = GetDlgItem(g_list, kListIdClose);
  if (close) {
    MoveWindow(close, client.right - margin - buttonWidth, y, buttonWidth,
               buttonHeight, TRUE);
  }
}

void OpenSelectedNote() {
  const int index = SelectedNoteIndex();
  if (index < 0) return;
  BubbleNoteToFront(g_notes[static_cast<size_t>(index)]);
  RefreshNotesList();
}

void DeleteSelectedNote() {
  const int index = SelectedNoteIndex();
  if (index < 0) return;
  NoteState* note = g_notes[static_cast<size_t>(index)];
  if (MessageBoxW(g_list, L"删除这张便签？\n\n删除后无法恢复。", L"删除便签",
                  MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
    return;
  }
  DeleteNoteFile(note);
  if (note->hwnd) DestroyWindow(note->hwnd);
  RefreshNotesList();
}

LRESULT CALLBACK ListWndProc(HWND hwnd, UINT message, WPARAM wparam,
                             LPARAM lparam) {
  switch (message) {
    case WM_CREATE: {
      const UINT dpi = GetDpiForWindow(hwnd);
      g_listFont = CreateFontW(
          -MulDiv(10, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL, FALSE,
          FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
          DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

      g_listListbox = CreateWindowExW(
          0, L"LISTBOX", L"",
          WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY |
              LBS_NOINTEGRALHEIGHT,
          0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListIdListbox)),
          g_instance, nullptr);
      if (g_listListbox && g_listFont) {
        SendMessageW(g_listListbox, WM_SETFONT,
                     reinterpret_cast<WPARAM>(g_listFont), TRUE);
      }

      struct ButtonSpec {
        int id;
        const wchar_t* text;
      };
      const ButtonSpec buttons[] = {
          {kListIdNew, L"新建"},
          {kListIdOpen, L"打开"},
          {kListIdSettings, L"设置"},
          {kListIdDelete, L"删除"},
          {kListIdClose, L"关闭"},
      };
      for (const ButtonSpec& spec : buttons) {
        HWND button = CreateWindowExW(
            0, L"BUTTON", spec.text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0,
            hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(spec.id)), g_instance, nullptr);
        if (button && g_listFont) {
          SendMessageW(button, WM_SETFONT,
                       reinterpret_cast<WPARAM>(g_listFont), TRUE);
        }
      }
      LayoutNotesList();
      RefreshNotesList();
      return 0;
    }

    case WM_SIZE:
      LayoutNotesList();
      return 0;

    // Paper background for the whole dialog, and owner-drawn buttons to match.
    case WM_ERASEBKGND: {
      HDC dc = reinterpret_cast<HDC>(wparam);
      RECT client = {};
      GetClientRect(hwnd, &client);
      HBRUSH brush = CreateSolidBrush(kPaper);
      FillRect(dc, &client, brush);
      DeleteObject(brush);
      return 1;
    }

    case WM_DRAWITEM:
      PaintPaperButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lparam));
      return TRUE;

    // The listbox is a native control; without this it stays white and looks
    // pasted on top of the paper background.
    case WM_CTLCOLORLISTBOX: {
      HDC dc = reinterpret_cast<HDC>(wparam);
      SetTextColor(dc, kInk);
      SetBkColor(dc, kPaper);
      if (!g_listBrush) g_listBrush = CreateSolidBrush(kPaper);
      return reinterpret_cast<LRESULT>(g_listBrush);
    }

    case WM_COMMAND: {
      const int id = LOWORD(wparam);
      if (id == kListIdNew) {
        CreateNoteFromAnchor();
        RefreshNotesList();
        return 0;
      }
      if (id == kListIdOpen) {
        OpenSelectedNote();
        return 0;
      }
      if (id == kListIdSettings) {
        ShowHotkeySettings();
        return 0;
      }
      if (id == kListIdDelete) {
        DeleteSelectedNote();
        return 0;
      }
      if (id == kListIdClose) {
        ShowWindow(hwnd, SW_HIDE);
        return 0;
      }
      if (id == kListIdListbox && HIWORD(wparam) == LBN_DBLCLK) {
        OpenSelectedNote();
        return 0;
      }
      break;
    }

    case WM_CLOSE:
      ShowWindow(hwnd, SW_HIDE);
      return 0;

    case WM_DESTROY:
      if (g_listFont) {
        DeleteObject(g_listFont);
        g_listFont = nullptr;
      }
      g_list = nullptr;
      g_listListbox = nullptr;
      return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

void EnsureListWindow() {
  if (g_list) return;
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ListWndProc;
    wc.hInstance = g_instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kListClass;
    RegisterClassExW(&wc);
    registered = true;
  }
  const UINT dpi = DpiForPoint({0, 0});
  RECT work = WorkAreaForPoint({0, 0});
  const int width = ScaleFor(dpi, 470);
  const int height = ScaleFor(dpi, 400);
  g_list = CreateWindowExW(
      WS_EX_TOOLWINDOW, kListClass, L"便签列表",
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX,
      work.left + ScaleFor(dpi, 60), work.top + ScaleFor(dpi, 60), width,
      height, nullptr, nullptr, g_instance, nullptr);
}

void ShowNotesList() {
  EnsureListWindow();
  if (!g_list) return;
  RefreshNotesList();
  ShowWindow(g_list, SW_SHOW);
  SetWindowPos(g_list, HWND_TOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
  SetWindowPos(g_list, HWND_NOTOPMOST, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
}

void ToggleNotesList() {
  if (g_list && IsWindowVisible(g_list)) {
    ShowWindow(g_list, SW_HIDE);
  } else {
    ShowNotesList();
  }
}

// ------------------------------------------------------------- hotkey setup
//
// Both shortcuts are configurable. A new chord is only accepted after it
// actually registers, so a combination already owned by another program is
// reported instead of silently doing nothing.

struct HotkeySpec {
  UINT modifiers = 0;
  UINT virtualKey = 0;
};

constexpr int kSettingsIdSave = 200;
constexpr int kSettingsIdCancel = 201;

HotkeySpec g_specNewNote = {MOD_CONTROL | MOD_ALT, 'N'};
HotkeySpec g_specList = {MOD_CONTROL | MOD_ALT, 'L'};

// Capture writes the pressed chord straight into the two specs above.  Cancel
// used to leave those mutations behind, so cancelling a change still altered
// what ResumeHotkeys registered afterwards.  These hold the values as they were
// when the dialog opened, and Cancel/Close restores them.
HotkeySpec g_specNewNoteBackup = {MOD_CONTROL | MOD_ALT, 'N'};
HotkeySpec g_specListBackup = {MOD_CONTROL | MOD_ALT, 'L'};

HWND g_settings = nullptr;
HFONT g_settingsFont = nullptr;
int g_armedSlot = -1;  // 0 = new note, 1 = list, -1 = nothing armed

std::wstring HotkeySettingsPath() {
  return StoreDirectory() + L"\\settings.ini";
}

std::wstring KeyDisplayName(UINT virtualKey) {
  if (virtualKey == 0) return L"未设置";
  const UINT scancode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
  wchar_t name[64] = {};
  if (GetKeyNameTextW(static_cast<LONG>(scancode) << 16, name,
                      static_cast<int>(_countof(name))) > 0 &&
      name[0] != L'\0') {
    return name;
  }
  wchar_t fallback[8] = {};
  _snwprintf_s(fallback, _countof(fallback), _TRUNCATE, L"%c",
               static_cast<wchar_t>(virtualKey));
  return fallback;
}

std::wstring FormatHotkey(const HotkeySpec& spec) {
  if (spec.virtualKey == 0) return L"未设置";
  std::wstring text;
  if (spec.modifiers & MOD_CONTROL) text += L"Ctrl + ";
  if (spec.modifiers & MOD_ALT) text += L"Alt + ";
  if (spec.modifiers & MOD_SHIFT) text += L"Shift + ";
  if (spec.modifiers & MOD_WIN) text += L"Win + ";
  text += KeyDisplayName(spec.virtualKey);
  return text;
}

UINT CurrentModifiers() {
  UINT modifiers = 0;
  if (GetKeyState(VK_CONTROL) & 0x8000) modifiers |= MOD_CONTROL;
  if (GetKeyState(VK_MENU) & 0x8000) modifiers |= MOD_ALT;
  if (GetKeyState(VK_SHIFT) & 0x8000) modifiers |= MOD_SHIFT;
  if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) {
    modifiers |= MOD_WIN;
  }
  return modifiers;
}

void LoadHotkeySettings() {
  std::string bytes;
  if (!ReadAllBytes(HotkeySettingsPath(), &bytes)) return;
  const std::wstring text = ToWide(bytes);
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find(L'\n', start);
    if (end == std::wstring::npos) end = text.size();
    std::wstring line = text.substr(start, end - start);
    while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) {
      line.pop_back();
    }
    const size_t equals = line.find(L'=');
    if (equals != std::wstring::npos) {
      const std::wstring key = line.substr(0, equals);
      const int value = ParseInt(ToUtf8(line.substr(equals + 1)));
      if (key == L"new_mods") {
        g_specNewNote.modifiers = static_cast<UINT>(value);
      } else if (key == L"new_vk") {
        g_specNewNote.virtualKey = static_cast<UINT>(value);
      } else if (key == L"list_mods") {
        g_specList.modifiers = static_cast<UINT>(value);
      } else if (key == L"list_vk") {
        g_specList.virtualKey = static_cast<UINT>(value);
      }
    }
    if (end == text.size()) break;
    start = end + 1;
  }
  LogLine(L"hotkey settings loaded: new=%s list=%s",
          FormatHotkey(g_specNewNote).c_str(),
          FormatHotkey(g_specList).c_str());
}

void SaveHotkeySettings() {
  wchar_t buffer[256] = {};
  _snwprintf_s(buffer, _countof(buffer), _TRUNCATE,
               L"new_mods=%u\nnew_vk=%u\nlist_mods=%u\nlist_vk=%u\n",
               g_specNewNote.modifiers, g_specNewNote.virtualKey,
               g_specList.modifiers, g_specList.virtualKey);
  WriteAllBytes(HotkeySettingsPath(), ToUtf8(buffer));
}

// Unregisters the current pair, then registers the requested one. On failure the
// previous pair is restored, so the user never loses a working shortcut.
bool ApplyHotkeys(const HotkeySpec& forNew, const HotkeySpec& forList,
                  std::wstring* error) {
  if (forNew.virtualKey == 0 || forList.virtualKey == 0) {
    if (error) *error = L"两个快捷键都必须设置。";
    return false;
  }
  if (forNew.virtualKey == forList.virtualKey &&
      forNew.modifiers == forList.modifiers) {
    if (error) *error = L"两个快捷键不能相同。";
    return false;
  }

  if (g_hotkeyNewNote) {
    UnregisterHotKey(g_hidden, kHotkeyNewNote);
    g_hotkeyNewNote = false;
  }
  if (g_hotkeyToggleList) {
    UnregisterHotKey(g_hidden, kHotkeyToggleList);
    g_hotkeyToggleList = false;
  }

  g_hotkeyNewNote =
      RegisterHotKey(g_hidden, kHotkeyNewNote, forNew.modifiers | MOD_NOREPEAT,
                     forNew.virtualKey) != FALSE;
  g_hotkeyToggleList =
      RegisterHotKey(g_hidden, kHotkeyToggleList,
                     forList.modifiers | MOD_NOREPEAT,
                     forList.virtualKey) != FALSE;

  if (!g_hotkeyNewNote || !g_hotkeyToggleList) {
    if (error) {
      *error = !g_hotkeyNewNote
                   ? (L"“" + FormatHotkey(forNew) +
                      L"”已被其他程序占用，请换一个组合。")
                   : (L"“" + FormatHotkey(forList) +
                      L"”已被其他程序占用，请换一个组合。");
    }
    // Roll back to the last known-good pair.
    if (g_hotkeyNewNote) {
      UnregisterHotKey(g_hidden, kHotkeyNewNote);
      g_hotkeyNewNote = false;
    }
    if (g_hotkeyToggleList) {
      UnregisterHotKey(g_hidden, kHotkeyToggleList);
      g_hotkeyToggleList = false;
    }
    g_hotkeyNewNote =
        RegisterHotKey(g_hidden, kHotkeyNewNote,
                       g_specNewNote.modifiers | MOD_NOREPEAT,
                       g_specNewNote.virtualKey) != FALSE;
    g_hotkeyToggleList =
        RegisterHotKey(g_hidden, kHotkeyToggleList,
                       g_specList.modifiers | MOD_NOREPEAT,
                       g_specList.virtualKey) != FALSE;
    return false;
  }

  g_specNewNote = forNew;
  g_specList = forList;
  SaveHotkeySettings();
  LogLine(L"hotkeys applied: new=%s list=%s",
          FormatHotkey(g_specNewNote).c_str(),
          FormatHotkey(g_specList).c_str());
  return true;
}

// While a hotkey box is armed the user is pressing exactly the chord that is
// registered system-wide.  RegisterHotKey swallows it and posts WM_HOTKEY, so
// the action fired instead of the chord being captured -- pressing the current
// combination created a note and the box never saw the key.  Unregistering for
// the duration turns the chord back into ordinary keystrokes.
void SuspendHotkeys() {
  if (g_hotkeyNewNote) {
    UnregisterHotKey(g_hidden, kHotkeyNewNote);
    g_hotkeyNewNote = false;
  }
  if (g_hotkeyToggleList) {
    UnregisterHotKey(g_hidden, kHotkeyToggleList);
    g_hotkeyToggleList = false;
  }
}

// Restores whatever is currently saved.  Deliberately silent: this runs on
// dialog teardown paths where a message box would be noise, and the saved pair
// was already validated when it was applied.
void ResumeHotkeys() {
  if (!g_hotkeyNewNote && g_specNewNote.virtualKey != 0) {
    g_hotkeyNewNote =
        RegisterHotKey(g_hidden, kHotkeyNewNote,
                       g_specNewNote.modifiers | MOD_NOREPEAT,
                       g_specNewNote.virtualKey) != FALSE;
  }
  if (!g_hotkeyToggleList && g_specList.virtualKey != 0) {
    g_hotkeyToggleList =
        RegisterHotKey(g_hidden, kHotkeyToggleList,
                       g_specList.modifiers | MOD_NOREPEAT,
                       g_specList.virtualKey) != FALSE;
  }
}

void SettingsBoxRects(HWND hwnd, RECT* boxNew, RECT* boxList, RECT* saveRect,
                      RECT* cancelRect) {
  RECT client = {};
  GetClientRect(hwnd, &client);
  const UINT dpi = GetDpiForWindow(hwnd);
  const int margin = ScaleFor(dpi, 18);
  const int rowGap = ScaleFor(dpi, 46);
  const int labelWidth = ScaleFor(dpi, 118);
  const int boxHeight = ScaleFor(dpi, 32);

  boxNew->left = margin + labelWidth;
  boxNew->top = margin + ScaleFor(dpi, 6);
  boxNew->right = client.right - margin;
  boxNew->bottom = boxNew->top + boxHeight;

  boxList->left = boxNew->left;
  boxList->top = boxNew->top + rowGap;
  boxList->right = boxNew->right;
  boxList->bottom = boxList->top + boxHeight;

  const int buttonWidth = ScaleFor(dpi, 84);
  const int buttonHeight = ScaleFor(dpi, 30);
  const int buttonTop = client.bottom - margin - buttonHeight;
  cancelRect->right = client.right - margin;
  cancelRect->left = cancelRect->right - buttonWidth;
  cancelRect->top = buttonTop;
  cancelRect->bottom = buttonTop + buttonHeight;
  saveRect->right = cancelRect->left - ScaleFor(dpi, 10);
  saveRect->left = saveRect->right - buttonWidth;
  saveRect->top = buttonTop;
  saveRect->bottom = buttonTop + buttonHeight;
}

void LayoutSettings(HWND hwnd) {
  RECT boxNew = {};
  RECT boxList = {};
  RECT saveRect = {};
  RECT cancelRect = {};
  SettingsBoxRects(hwnd, &boxNew, &boxList, &saveRect, &cancelRect);
  HWND save = GetDlgItem(hwnd, kSettingsIdSave);
  HWND cancel = GetDlgItem(hwnd, kSettingsIdCancel);
  if (save) {
    MoveWindow(save, saveRect.left, saveRect.top,
               saveRect.right - saveRect.left, saveRect.bottom - saveRect.top,
               TRUE);
  }
  if (cancel) {
    MoveWindow(cancel, cancelRect.left, cancelRect.top,
               cancelRect.right - cancelRect.left,
               cancelRect.bottom - cancelRect.top, TRUE);
  }
}

void PaintSettings(HWND hwnd) {
  PAINTSTRUCT ps = {};
  HDC dc = BeginPaint(hwnd, &ps);
  RECT client = {};
  GetClientRect(hwnd, &client);
  FillRect(dc, &client, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
  HBRUSH paper = CreateSolidBrush(kPaper);
  FillRect(dc, &client, paper);
  DeleteObject(paper);
  SetBkMode(dc, TRANSPARENT);

  const UINT dpi = GetDpiForWindow(hwnd);
  const int margin = ScaleFor(dpi, 18);

  RECT boxNew = {};
  RECT boxList = {};
  RECT saveRect = {};
  RECT cancelRect = {};
  SettingsBoxRects(hwnd, &boxNew, &boxList, &saveRect, &cancelRect);
  const RECT boxes[2] = {boxNew, boxList};
  const wchar_t* labels[2] = {L"新建便签", L"显示/隐藏列表"};
  const HotkeySpec specs[2] = {g_specNewNote, g_specList};

  HGDIOBJ oldFont = SelectObject(dc, g_settingsFont);

  for (int slot = 0; slot < 2; ++slot) {
    RECT labelRect = {margin, boxes[slot].top,
                      boxes[slot].left - ScaleFor(dpi, 8),
                      boxes[slot].bottom};
    SetTextColor(dc, kInk);
    DrawTextW(dc, labels[slot], -1, &labelRect,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    const bool armed = (g_armedSlot == slot);
    HBRUSH fill = CreateSolidBrush(armed ? kPaperStrip : kPaper);
    HPEN pen = CreatePen(PS_SOLID, armed ? 2 : 1, armed ? kAmber : kPaperEdge);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, fill);
    Rectangle(dc, boxes[slot].left, boxes[slot].top, boxes[slot].right,
              boxes[slot].bottom);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(fill);

    RECT textRect = boxes[slot];
    textRect.left += ScaleFor(dpi, 10);
    textRect.right -= ScaleFor(dpi, 10);
    SetTextColor(dc, armed ? RGB(150, 96, 0) : RGB(40, 40, 40));
    const std::wstring text =
        armed ? std::wstring(L"请按下新的组合键…") : FormatHotkey(specs[slot]);
    DrawTextW(dc, text.c_str(), -1, &textRect,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
  }

  SetTextColor(dc, RGB(120, 120, 120));
  RECT hint = {margin, boxList.bottom + ScaleFor(dpi, 14),
               client.right - margin, saveRect.top - ScaleFor(dpi, 6)};
  DrawTextW(dc, L"点击输入框后按下组合键即可修改；若组合被其他程序占用，保存时会提示。",
            -1, &hint, DT_LEFT | DT_WORDBREAK);

  const wchar_t* buttons[2] = {L"保存", L"取消"};
  const RECT rects[2] = {saveRect, cancelRect};
  for (int i = 0; i < 2; ++i) {
    RECT push = rects[i];
    DrawFrameControl(dc, &push, DFC_BUTTON, DFCS_BUTTONPUSH);
    SetTextColor(dc, RGB(30, 30, 30));
    RECT label = rects[i];
    DrawTextW(dc, buttons[i], -1, &label,
              DT_CENTER | DT_SINGLELINE | DT_VCENTER);
  }

  SelectObject(dc, oldFont);
  EndPaint(hwnd, &ps);
}

LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT message, WPARAM wparam,
                                 LPARAM lparam) {
  switch (message) {
    case WM_CREATE: {
      const UINT dpi = GetDpiForWindow(hwnd);
      g_settingsFont = CreateFontW(
          -MulDiv(10, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL, FALSE,
          FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
          DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
      struct ButtonSpec {
        int id;
        const wchar_t* text;
      };
      const ButtonSpec specs[] = {{kSettingsIdSave, L"保存"},
                                  {kSettingsIdCancel, L"取消"}};
      for (const ButtonSpec& spec : specs) {
        HWND button = CreateWindowExW(
            0, L"BUTTON", spec.text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0,
            hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(spec.id)), g_instance,
            nullptr);
        if (button && g_settingsFont) {
          SendMessageW(button, WM_SETFONT,
                       reinterpret_cast<WPARAM>(g_settingsFont), TRUE);
        }
      }
      LayoutSettings(hwnd);
      return 0;
    }

    case WM_SIZE:
      LayoutSettings(hwnd);
      return 0;

    case WM_PAINT:
      PaintSettings(hwnd);
      return 0;

    case WM_ERASEBKGND:
      return 1;

    case WM_DRAWITEM:
      PaintPaperButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lparam));
      return TRUE;

    case WM_LBUTTONUP: {
      POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      RECT boxNew = {};
      RECT boxList = {};
      RECT saveRect = {};
      RECT cancelRect = {};
      SettingsBoxRects(hwnd, &boxNew, &boxList, &saveRect, &cancelRect);
      const int previousSlot = g_armedSlot;
      if (PtInRect(&boxNew, pt)) {
        g_armedSlot = 0;
      } else if (PtInRect(&boxList, pt)) {
        g_armedSlot = 1;
      } else {
        g_armedSlot = -1;
      }
      // Arming swallows the global hotkeys so the chord can be captured; leaving
      // a box hands them back.
      if (g_armedSlot >= 0 && previousSlot < 0) SuspendHotkeys();
      if (g_armedSlot < 0 && previousSlot >= 0) ResumeHotkeys();
      SetFocus(hwnd);
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
      if (g_armedSlot < 0) break;
      const UINT key = static_cast<UINT>(wparam);
      // Modifier keys only arm a chord; it completes on a normal key.
      if (key == VK_CONTROL || key == VK_MENU || key == VK_SHIFT ||
          key == VK_LWIN || key == VK_RWIN || key == VK_APPS ||
          key == VK_ESCAPE) {
        return 0;
      }
      HotkeySpec spec;
      spec.modifiers = CurrentModifiers();
      spec.virtualKey = key;
      if (g_armedSlot == 0) {
        g_specNewNote = spec;
      } else {
        g_specList = spec;
      }
      g_armedSlot = -1;
      ResumeHotkeys();
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }

    case WM_COMMAND:
      if (LOWORD(wparam) == kSettingsIdSave) {
        std::wstring error;
        if (!ApplyHotkeys(g_specNewNote, g_specList, &error)) {
          MessageBoxW(hwnd, error.c_str(), L"快捷键不可用",
                      MB_OK | MB_ICONWARNING);
          return 0;
        }
        MessageBoxW(hwnd, L"快捷键已更新。", L"纸笺便签",
                    MB_OK | MB_ICONINFORMATION);
        // Refresh the snapshot so WM_DESTROY does not undo the saved values.
        g_specNewNoteBackup = g_specNewNote;
        g_specListBackup = g_specList;
        DestroyWindow(hwnd);
        return 0;
      }
      if (LOWORD(wparam) == kSettingsIdCancel) {
        DestroyWindow(hwnd);
        return 0;
      }
      break;

    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;

    case WM_DESTROY:
      if (g_settingsFont) {
        DeleteObject(g_settingsFont);
        g_settingsFont = nullptr;
      }
      g_settings = nullptr;
      g_armedSlot = -1;
      // Undo any capture the user did not confirm, then hand the hotkeys back.
      // On the Save path the values were already applied and the backup was
      // refreshed, so this is a no-op there.
      g_specNewNote = g_specNewNoteBackup;
      g_specList = g_specListBackup;
      ResumeHotkeys();
      return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

void ShowHotkeySettings() {
  if (g_settings) {
    SetForegroundWindow(g_settings);
    return;
  }
  // Snapshot what is currently in force so Cancel can undo capture edits.
  g_specNewNoteBackup = g_specNewNote;
  g_specListBackup = g_specList;
  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SettingsWndProc;
    wc.hInstance = g_instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"PaperNotes.SettingsWindow.v1";
    RegisterClassExW(&wc);
    registered = true;
  }
  const UINT dpi = DpiForPoint({0, 0});
  RECT work = WorkAreaForPoint({0, 0});
  const int width = ScaleFor(dpi, 430);
  const int height = ScaleFor(dpi, 300);
  g_settings = CreateWindowExW(
      WS_EX_TOOLWINDOW, L"PaperNotes.SettingsWindow.v1", L"快捷键设置",
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
      work.left + (work.right - work.left - width) / 2,
      work.top + (work.bottom - work.top - height) / 2, width, height, nullptr,
      nullptr, g_instance, nullptr);
  if (g_settings) {
    ShowWindow(g_settings, SW_SHOW);
    // A background process cannot take the foreground, so raise through
    // HWND_TOPMOST and immediately drop back.
    SetWindowPos(g_settings, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetWindowPos(g_settings, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(g_settings);
  }
}

void ShowTrayMenu() {
  HMENU menu = CreatePopupMenu();
  if (!menu) return;
  AppendMenuW(menu, MF_STRING, kTrayMenuNew, L"新建便签");

  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

  // The simplest possible "saved notes list": one entry per stored note.
  const size_t count = g_notes.size();
  for (size_t index = 0; index < count; ++index) {
    NoteState* note = g_notes[index];
    std::wstring label = NotePreview(note);
    if (note->hwnd && IsWindowVisible(note->hwnd)) label = L"● " + label;
    AppendMenuW(menu, MF_STRING, kTrayMenuFirstNote + static_cast<UINT>(index),
                label.c_str());
  }
  if (count == 0) {
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"（还没有便签）");
  }

  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING | (count == 0 ? MF_GRAYED : 0), kTrayMenuList,
              L"全部显示");
  AppendMenuW(menu, MF_STRING, kTrayMenuHotkeys, L"快捷键设置…");
  AppendMenuW(menu, MF_STRING, kTrayMenuOpenList, L"便签列表…");
  AppendMenuW(menu, MF_STRING, kTrayMenuExit, L"退出纸笺便签");

  SetForegroundWindow(g_hidden);
  POINT cursor = {};
  GetCursorPos(&cursor);
  const UINT command = TrackPopupMenu(
      menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, cursor.x, cursor.y,
      0, g_hidden, nullptr);
  DestroyMenu(menu);
  if (command == kTrayMenuNew) PostMessageW(g_hidden, kMsgNewNote, 0, 0);
  if (command == kTrayMenuList) {
    for (NoteState* note : g_notes) BubbleNoteToFront(note);
  }
  if (command == kTrayMenuOpenList) ShowNotesList();
  if (command == kTrayMenuHotkeys) ShowHotkeySettings();
  if (command >= kTrayMenuFirstNote &&
      command < kTrayMenuFirstNote + count) {
    BubbleNoteToFront(g_notes[command - kTrayMenuFirstNote]);
  }
  if (command == kTrayMenuExit) PostMessageW(g_hidden, kMsgExit, 0, 0);
}

LRESULT CALLBACK HiddenWndProc(HWND hwnd, UINT message, WPARAM wparam,
                               LPARAM lparam) {
  switch (message) {
    case kMsgTray:
      if (LOWORD(lparam) == WM_RBUTTONUP || LOWORD(lparam) == WM_CONTEXTMENU) {
        ShowTrayMenu();
      } else if (LOWORD(lparam) == WM_LBUTTONDBLCLK) {
        CreateNoteNearCursor();
      }
      return 0;

    case kMsgNewNote:
      CreateNoteFromAnchor();
      return 0;

    case kMsgExit:
      DestroyWindow(hwnd);
      return 0;

    case WM_HOTKEY:
      if (wparam == kHotkeyNewNote) {
        CreateNoteFromAnchor();
        return 0;
      }
      if (wparam == kHotkeyToggleList) {
        ToggleNotesList();
        return 0;
      }
      break;

    case WM_TIMER:
      if (wparam == 1) {
        POINT cursor = {};
        GetCursorPos(&cursor);
        const LONG64 rightDowns =
            InterlockedExchange64(&g_hookRightDownCount, 0);
        LogLine(L"heartbeat hookEvents=%lld rightDowns=%lld cursor=(%ld,%ld) "
                L"notes=%zu visible=%zu",
                InterlockedCompareExchange64(&g_hookEventCount, 0, 0),
                rightDowns, cursor.x, cursor.y, g_notes.size(),
                VisibleNoteCount());
        return 0;
      }
      break;

    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;

    case WM_DESTROY:
      RemoveTrayIcon();
      PostQuitMessage(0);
      return 0;

    default: {
      static UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
      if (taskbarCreated != 0 && message == taskbarCreated) {
        g_trayAdded = false;
        AddTrayIcon();
        return 0;
      }
    }
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

HWND FindExistingInstance() {
  for (int attempt = 0; attempt < 60; ++attempt) {
    HWND hwnd = FindWindowW(kHiddenClass, nullptr);
    if (hwnd) return hwnd;
    Sleep(50);
  }
  return nullptr;
}

bool HasArgument(const wchar_t* expected) {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) return false;
  bool found = false;
  for (int i = 1; i < argc; ++i) {
    if (_wcsicmp(argv[i], expected) == 0) {
      found = true;
      break;
    }
  }
  LocalFree(argv);
  return found;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
  g_instance = instance;
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  const std::wstring dataDir = DataDirectory();
  SetLastError(0);
  const BOOL madeDir = CreateDirectoryW(dataDir.c_str(), nullptr);
  const DWORD dirError = GetLastError();
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  LogLine(L"data dir=%s created=%d gle=%lu", dataDir.c_str(),
          madeDir ? 1 : 0, dirError);
  ProbeWritableLocation(dataDir);
  ProbeWritableLocation(LocalAppDataDirectory());

  const bool wantsNewNote = HasArgument(L"--new-note");
  // Started from HKCU\...\Run. Signing in must be silent: restore the notes
  // hidden and pop nothing, not even the fallback list window.
  const bool isAutostart = HasArgument(L"--autostart");

  HANDLE mutex =
      CreateMutexW(nullptr, FALSE, L"Local\\PaperNotes.SingleInstance.v1");
  if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
    HWND existing = FindExistingInstance();
    if (existing && wantsNewNote) {
      PostMessageW(existing, kMsgNewNote, 0, 0);
    }
    LogLine(L"second instance exited; forwarded=%d",
            (existing && wantsNewNote) ? 1 : 0);
    CloseHandle(mutex);
    CoUninitialize();
    return 0;
  }

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = HiddenWndProc;
  wc.hInstance = instance;
  wc.lpszClassName = kHiddenClass;
  RegisterClassExW(&wc);

  g_hidden = CreateWindowExW(WS_EX_TOOLWINDOW, kHiddenClass, kAppTitle,
                             WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance,
                             nullptr);
  if (!g_hidden) {
    LogLine(L"CreateWindowEx(hidden) failed gle=%lu", GetLastError());
    CloseHandle(mutex);
    CoUninitialize();
    return 1;
  }

  EnsureClickMapping();
  AddTrayIcon();
  InstallMouseHook();
  // Ctrl+Alt+N new note, Ctrl+Alt+L show/hide the notes list. MOD_NOREPEAT
  // stops auto-repeat while the keys are held.
  LoadHotkeySettings();
  ApplyHotkeys(g_specNewNote, g_specList, nullptr);
  LogLine(L"hotkeys newNote=%d list=%d", g_hotkeyNewNote ? 1 : 0,
          g_hotkeyToggleList ? 1 : 0);
  LogLine(L"desktop station=%s desktop=%s",
          UserObjectName(GetProcessWindowStation()).c_str(),
          UserObjectName(GetThreadDesktop(GetCurrentThreadId())).c_str());
  LogLine(L"phase0 app started; pid=%lu wantsNewNote=%d autostart=%d mapping=%d",
          GetCurrentProcessId(), wantsNewNote ? 1 : 0, isAutostart ? 1 : 0,
          g_click ? 1 : 0);
  SetTimer(g_hidden, 1, 5000, nullptr);

  // Notes come back as hidden windows so launching never scatters the desktop.
  RestoreStoredNotes();

  // Without a tray icon there would be no way back to those hidden notes, so
  // surface the list instead of stranding them -- except at sign-in, where
  // opening a window nobody asked for would be worse than staying silent.
  // Ctrl+Alt+L and the shell menu still reach the notes.
  if (!g_trayAdded && !wantsNewNote && !isAutostart) {
    LogLine(L"tray icon unavailable; opening notes list as the fallback entry");
    ShowNotesList();
  }

  if (wantsNewNote) {
    PostMessageW(g_hidden, kMsgNewNote, 0, 0);
  }

  MSG msg = {};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  SaveAllNotes();
  if (g_hotkeyNewNote) UnregisterHotKey(g_hidden, kHotkeyNewNote);
  if (g_hotkeyToggleList) UnregisterHotKey(g_hidden, kHotkeyToggleList);
  UninstallMouseHook();
  CloseClickMapping();
  if (g_editBrush) DeleteObject(g_editBrush);
  CloseHandle(mutex);
  CoUninitialize();
  return static_cast<int>(msg.wParam);
}
