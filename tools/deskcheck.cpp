#include <windows.h>
#include <cstdio>

static void Name(HANDLE object, const char* label, FILE* log) {
  WCHAR wide[256] = {};
  DWORD needed = 0;
  char name[256] = "<err>";
  if (GetUserObjectInformationW(object, UOI_NAME, wide, sizeof(wide),
                                &needed)) {
    WideCharToMultiByte(CP_ACP, 0, wide, -1, name, sizeof(name), nullptr,
                        nullptr);
  }
  std::fprintf(log, "%-22s = %s\n", label, name);
  std::fflush(log);
}

int main() {
  FILE* log = nullptr;
  fopen_s(&log,
          "C:\\cx_ProjectCode\\PaperNotes\\out\\diagnostics\\deskcheck.log",
          "a");
  SYSTEMTIME st;
  GetLocalTime(&st);
  std::fprintf(log, "\n=== %02u:%02u:%02u pid=%lu ===\n", st.wHour, st.wMinute,
               st.wSecond, GetCurrentProcessId());
  std::fflush(log);

  Name(GetProcessWindowStation(), "process window station", log);
  Name(GetThreadDesktop(GetCurrentThreadId()), "thread desktop", log);

  HDESK input = OpenInputDesktop(0, FALSE,
                                 DESKTOP_READOBJECTS | DESKTOP_SWITCHDESKTOP);
  if (input) {
    Name(input, "INPUT desktop", log);
    CloseDesktop(input);
  } else {
    std::fprintf(log, "OpenInputDesktop FAILED gle=%lu\n", GetLastError());
    std::fflush(log);
  }

  SetLastError(0);
  BOOL moved = SetCursorPos(700, 700);
  POINT pt = {};
  GetCursorPos(&pt);
  std::fprintf(log, "SetCursorPos=%ld gle=%lu cursor=%ld,%ld\n", moved,
               GetLastError(), pt.x, pt.y);

  // Physical screen metrics as this process sees them.
  std::fprintf(log, "SM_CXSCREEN=%d SM_CYSCREEN=%d\n",
               GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
  std::fflush(log);
  std::fclose(log);
  return 0;
}