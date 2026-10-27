#include <windows.h>
#include <cstdio>

// Diagnostic probe: log window station/desktop, SetCursorPos result, hook
// install result, and hook delivery for both injected and real input.

static HHOOK g_hook = nullptr;
static volatile LONG g_events = 0;
static volatile LONG g_rightDowns = 0;
static FILE* g_log = nullptr;

static void Log(const char* text) {
  if (!g_log) return;
  SYSTEMTIME st;
  GetLocalTime(&st);
  std::fprintf(g_log, "%02u:%02u:%02u.%03u %s\n", st.wHour, st.wMinute,
               st.wSecond, st.wMilliseconds, text);
  std::fflush(g_log);
}

static void LogObjectName(HANDLE object, const char* label) {
  char name[256] = {};
  DWORD needed = 0;
  WCHAR wide[256] = {};
  if (GetUserObjectInformationW(object, UOI_NAME, wide, sizeof(wide),
                                &needed)) {
    WideCharToMultiByte(CP_ACP, 0, wide, -1, name, sizeof(name), nullptr,
                        nullptr);
  } else {
    std::sprintf(name, "<err %lu>", GetLastError());
  }
  char line[320];
  std::sprintf(line, "%s=%s", label, name);
  Log(line);
}

static LRESULT CALLBACK HookProc(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION) {
    InterlockedIncrement(&g_events);
    if (wparam == WM_RBUTTONDOWN) {
      InterlockedIncrement(&g_rightDowns);
    }
  }
  return CallNextHookEx(g_hook, code, wparam, lparam);
}

int main() {
  fopen_s(&g_log,
          "C:\\cx_ProjectCode\\PaperNotes\\out\\diagnostics\\hooknative2.log",
          "w");

  char line[512];
  std::sprintf(line, "pid=%lu session=%lu", GetCurrentProcessId(),
               (unsigned long)0);
  Log(line);
  LogObjectName(GetProcessWindowStation(), "station");
  LogObjectName(GetThreadDesktop(GetCurrentThreadId()), "desktop");

  SetLastError(0);
  BOOL moved = SetCursorPos(500, 500);
  POINT pt = {};
  GetCursorPos(&pt);
  std::sprintf(line, "SetCursorPos=%ld gle=%lu cursor=%ld,%ld", moved,
               GetLastError(), pt.x, pt.y);
  Log(line);

  SetLastError(0);
  g_hook = SetWindowsHookExW(WH_MOUSE_LL, HookProc, nullptr, 0);
  std::sprintf(line, "SetWindowsHookEx=%p gle=%lu", (void*)g_hook,
               GetLastError());
  Log(line);
  if (!g_hook) {
    Log("HOOK_INSTALL_FAILED");
    std::fclose(g_log);
    return 1;
  }

  // Self-inject twice, 1s apart, from this thread's own timer loop.
  DWORD start = GetTickCount();
  bool injected = false;
  MSG msg;
  while (GetTickCount() - start < 9000) {
    DWORD elapsed = GetTickCount() - start;
    if (!injected && elapsed > 1000) {
      injected = true;
      SetLastError(0);
      BOOL ok = SetCursorPos(900, 900);
      std::sprintf(line, "injecting: SetCursorPos=%ld gle=%lu", ok,
                   GetLastError());
      Log(line);
      mouse_event(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0);
      Sleep(60);
      mouse_event(MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0);
    }
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    Sleep(15);
  }

  std::sprintf(line, "RESULT events=%ld rightDowns=%ld", g_events, g_rightDowns);
  Log(line);
  UnhookWindowsHookEx(g_hook);
  std::fclose(g_log);
  return 0;
}