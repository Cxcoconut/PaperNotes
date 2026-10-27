$ErrorActionPreference = 'Stop'
$log = 'C:\cx_ProjectCode\PaperNotes\out\diagnostics\hook-compare.log'
New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null
if (Test-Path $log) { Remove-Item $log -Force }

Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Threading;

public static class HookCompare {
    public delegate IntPtr HookProc(int nCode, IntPtr wParam, IntPtr lParam);
    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int X; public int Y; }
    [StructLayout(LayoutKind.Sequential)]
    public struct MSLLHOOKSTRUCT {
        public POINT pt; public uint mouseData; public uint flags;
        public uint time; public UIntPtr dwExtraInfo;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct MSG {
        public IntPtr hwnd; public uint message; public UIntPtr wParam;
        public IntPtr lParam; public uint time; public POINT pt;
    }
    [DllImport("user32.dll", SetLastError=true)]
    public static extern IntPtr SetWindowsHookEx(int id, HookProc proc, IntPtr mod, uint thread);
    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool UnhookWindowsHookEx(IntPtr hook);
    [DllImport("user32.dll")]
    public static extern IntPtr CallNextHookEx(IntPtr hook, int code, IntPtr w, IntPtr l);
    [DllImport("user32.dll")]
    public static extern bool PeekMessage(out MSG msg, IntPtr h, uint lo, uint hi, uint remove);
    [DllImport("user32.dll")]
    public static extern bool TranslateMessage(ref MSG msg);
    [DllImport("user32.dll")]
    public static extern IntPtr DispatchMessage(ref MSG msg);
    [DllImport("user32.dll")]
    public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")]
    public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);

    public static HookProc Callback;
    public static IntPtr Hook;
    public static string LogPath;
    public static int Injected;
    public static int Real;
    public static string InjectedSummary = "(none)";
    public static string FirstRealSummary = "(none)";

    public static void Write(string t) {
        System.IO.File.AppendAllText(LogPath,
            DateTime.Now.ToString("HH:mm:ss.fff") + " " + t + Environment.NewLine);
    }

    public static IntPtr OnMouse(int code, IntPtr w, IntPtr l) {
        if (code >= 0) {
            uint msg = (uint)w.ToInt64();
            if (msg == 0x0204) {
                MSLLHOOKSTRUCT s = (MSLLHOOKSTRUCT)Marshal.PtrToStructure(l, typeof(MSLLHOOKSTRUCT));
                string line = "RBUTTONDOWN x=" + s.pt.X + " y=" + s.pt.Y + " flags=0x" + s.flags.ToString("X");
                bool injected = (s.flags & 0x1) != 0 || (s.flags & 0x2) != 0;
                if (injected) { Injected++; InjectedSummary = line; }
                else {
                    Real++;
                    if (FirstRealSummary == "(none)") FirstRealSummary = line;
                }
                Write(line + (injected ? " [INJECTED]" : " [REAL]"));
            }
        }
        return CallNextHookEx(Hook, code, w, l);
    }

    public static void InjectSoon() {
        Thread t = new Thread(() => {
            Thread.Sleep(2500);
            SetCursorPos(400, 400);
            Thread.Sleep(150);
            mouse_event(0x0008, 0, 0, 0, UIntPtr.Zero);
            Thread.Sleep(120);
            mouse_event(0x0010, 0, 0, 0, UIntPtr.Zero);
            Write("--- probe injected its own right-click at 400,400 ---");
        });
        t.IsBackground = true; t.Start();
    }
}
'@

[HookCompare]::LogPath = $log
[HookCompare]::Callback = [HookCompare+HookProc]{ param($c,$w,$l) [HookCompare]::OnMouse($c,$w,$l) }
[HookCompare]::Hook = [HookCompare]::SetWindowsHookEx(14, [HookCompare]::Callback, [IntPtr]::Zero, 0)
if ([HookCompare]::Hook -eq [IntPtr]::Zero) {
  "HOOK_FAILED gle=$([Runtime.InteropServices.Marshal]::GetLastWin32Error())" | Set-Content $log
  exit 1
}
"HOOK_OK - 12s window open (probe self-injects at ~2.5s)" | Set-Content $log
[HookCompare]::InjectSoon()

$deadline = (Get-Date).AddSeconds(12)
$msg = New-Object HookCompare+MSG
while ((Get-Date) -lt $deadline) {
  while ([HookCompare]::PeekMessage([ref]$msg, [IntPtr]::Zero, 0, 0, 1)) {
    [void][HookCompare]::TranslateMessage([ref]$msg)
    [void][HookCompare]::DispatchMessage([ref]$msg)
  }
  Start-Sleep -Milliseconds 15
}
[void][HookCompare]::UnhookWindowsHookEx([HookCompare]::Hook)

"RESULT injected_seen=$([HookCompare]::Injected) real_seen=$([HookCompare]::Real)" | Add-Content $log
"RESULT injected_sample=$([HookCompare]::InjectedSummary)" | Add-Content $log
"RESULT first_real_sample=$([HookCompare]::FirstRealSummary)" | Add-Content $log
Get-Content $log