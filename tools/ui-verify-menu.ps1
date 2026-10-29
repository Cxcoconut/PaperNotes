# UI verification helper for the desktop context menu.
#
# Minimizes every normal window so the desktop is exposed, then a human/agent
# right-clicks the background and screenshots the menu.  It deliberately does
# NOT click anything in the menu: this script only prepares the desktop.
#
# Used on 2026-10-27 to prove that ExplorerCommandHandler cannot replace the
# MSIX package: with the package removed, the first-level menu entry disappeared
# in every variant (registry verb, COM handler, direct COM activation) while the
# classic menu still worked.  See the "工作原理" section of README.md.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\ui-verify-menu.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\ui-verify-menu.ps1 -Restore

param(
  [switch]$Restore
)

$ErrorActionPreference = 'Stop'

Add-Type @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class PaperNotesUiProbe {
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);

  const int SW_MINIMIZE = 6;
  const int SW_RESTORE = 9;

  // The desktop's own hosts must never be minimized, or the desktop vanishes.
  static bool Keep(string cls) {
    return cls == "Progman" || cls == "WorkerW" || cls == "Shell_TrayWnd" ||
           cls == "Shell_SecondaryTrayWnd" || cls == "SHELLDLL_DefView" ||
           cls.StartsWith("Windows.UI.Core") ||
           cls.StartsWith("XamlExplorerHost") ||
           cls.StartsWith("EdgeUiInputTopWndClass") ||
           cls.StartsWith("ThumbnailDeviceHelper") ||
           cls.StartsWith("DummyDWMListener") ||
           cls.StartsWith("ATL:") ||
           cls.StartsWith("WinUIDesktopWin32");
  }

  public static List<IntPtr> Minimize() {
    var done = new List<IntPtr>();
    EnumWindows((h, l) => {
      if (!IsWindowVisible(h)) return true;
      var c = new StringBuilder(256); GetClassNameW(h, c, 256);
      var t = new StringBuilder(256); GetWindowTextW(h, t, 256);
      if (Keep(c.ToString())) return true;
      if (t.Length == 0) return true;          // tool windows
      ShowWindow(h, SW_MINIMIZE);
      done.Add(h);
      return true;
    }, IntPtr.Zero);
    return done;
  }

  public static int RestoreIconic() {
    int n = 0;
    EnumWindows((h, l) => {
      if (IsWindowVisible(h) && IsIconic(h)) { ShowWindow(h, SW_RESTORE); n++; }
      return true;
    }, IntPtr.Zero);
    return n;
  }
}
'@

if ($Restore) {
  "restored $([PaperNotesUiProbe]::RestoreIconic()) window(s)"
  exit 0
}

"minimized $(([PaperNotesUiProbe]::Minimize()).Count) window(s)"
"desktop is now exposed - right-click the background to inspect the menu"
"run again with -Restore to bring the windows back"
