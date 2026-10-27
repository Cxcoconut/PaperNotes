#include <windows.h>
#include <cstdio>

static HHOOK g_hook = nullptr;
static volatile LONG64 g_events = 0;
static FILE* g_log = nullptr;

static void Log(const char* text) {
  if (!g_log) return;
  SYSTEMTIME st;
  GetLocalTime(&st);
  std::fprintf(g_log, "%02u:%02u:%02u.%03u %s\n", st.wHour, st.wMinute,
               st.wSecond, st.wMilliseconds, text);
  std::fflush(g_log);
}

static LRESULT CALLBACK HookProc(int code, WPARAM wparam, LPARAM lparam) {
  if (code == HC_ACTION) {
    InterlockedIncrement64(&g_events);
    if (wparam == WM_RBUTTONDOWN) {
      auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lparam);
      char buffer[128];
      sprintf(buffer, "RBUTTONDOWN x=%ld y=%ld flags=0x%lX", info->pt.x,
                     info->pt.y, info->flags);
      Log(buffer);
    }
  }
  return CallNextHookEx(g_hook, code, wparam, lparam);
}

static DWORD WINAPI InjectThread(LPVOID) {
  Sleep(2000);
  Log("injecting native click");
  SetLastError(0);
  BOOL moved = SetCursorPos(1100, 1100);
  POINT pt = {};
  GetCursorPos(&pt);
  char position[192];
  sprintf(position, "SetCursorPos=%ld gle=%lu cursor=%ld,%ld", moved,
          GetLastError(), pt.x, pt.y);
  Log(position);
  Sleep(150);
  mouse_event(MOUSEEVENTF_RIGHTDOWN, 0, 0, 0, 0);
  Sleep(120);
  mouse_event(MOUSEEVENTF_RIGHTUP, 0, 0, 0, 0);
  Sleep(1500);
  char buffer[128];
  sprintf(buffer, "native hook events=%lld", g_events);
  Log(buffer);
  return 0;
}

int wmain() {
  fopen_s(&g_log,
          "C:\\cx_ProjectCode\\PaperNotes\\out\\diagnostics\\hooknative.log",
          "w");
  g_hook = SetWindowsHookExW(WH_MOUSE_LL, HookProc, GetModuleHandleW(nullptr), 0);
  if (!g_hook) {
    char buffer[128];
    sprintf(buffer, "HOOK_FAILED gle=%lu", GetLastError());
    Log(buffer);
    return 1;
  }
  Log("HOOK_OK");
  CreateThread(nullptr, 0, InjectThread, nullptr, 0, nullptr);

  const DWORD deadline = GetTickCount() + 8000;
  MSG msg;
  while (GetTickCount() < deadline) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    Sleep(20);
  }
  UnhookWindowsHookEx(g_hook);
  Log("HOOK_END");
  std::fclose(g_log);
  return 0;
}




