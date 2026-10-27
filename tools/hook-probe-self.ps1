$ErrorActionPreference = 'Stop'
$log = 'C:\cx_ProjectCode\PaperNotes\out\diagnostics\hookprobe.log'
New-Item -ItemType Directory -Force (Split-Path $log) | Out-Null

Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
using System.Threading;

public static class HookProbe2 {
    public delegate IntPtr HookProc(int nCode, IntPtr wParam, IntPtr lParam);
    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int X; public int Y; }
    [StructLayout(LayoutKind.Sequential)]
    public struct MSLLHOOKSTRUCT {
        public POINT pt;
        public uint mouseData;
        public uint flags;
        public uint time;
        public UIntPtr dwExtraInfo;
    }
    [StructLayout(LayoutKind.Sequential)]
    public struct MSG {
        public IntPtr hwnd;
        public uint message;
        public UIntPtr wParam;
        public IntPtr lParam;
        public uint time;
        public POINT pt;
    }
    [DllImport("user32.dll", SetLastError = true)]
    public static extern IntPtr SetWindowsHookEx(int idHook, HookProc proc, IntPtr hMod, uint threadId);
    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool UnhookWindowsHookEx(IntPtr hook);
    [DllImport("user32.dll")]
    public static extern IntPtr CallNextHookEx(IntPtr hook, int nCode, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")]
    public static extern bool PeekMessage(out MSG msg, IntPtr hwnd, uint min, uint max, uint remove);
    [DllImport("user32.dll")]
    public static extern bool TranslateMessage(ref MSG msg);
    [DllImport("user32.dll")]
    public static extern IntPtr DispatchMessage(ref MSG msg);
    [DllImport("user32.dll")]
    public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")]
    public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);

    public static HookProc Callback;
    public static IntPtr Hook;
    public static string LogPath;

    public static void Write(string text) {
        System.IO.File.AppendAllText(LogPath,
            DateTime.Now.ToString("HH:mm:ss.fff") + " " + text + Environment.NewLine);
    }

    public static IntPtr OnMouse(int code, IntPtr wParam, IntPtr lParam) {
        if (code >= 0) {
            uint message = (uint)wParam.ToInt64();
            if (message == 0x0204) {
                MSLLHOOKSTRUCT info = (MSLLHOOKSTRUCT)Marshal.PtrToStructure(lParam, typeof(MSLLHOOKSTRUCT));
                Write("RBUTTONDOWN x=" + info.pt.X + " y=" + info.pt.Y + " flags=0x" + info.flags.ToString("X"));
            }
        }
        return CallNextHookEx(Hook, code, wParam, lParam);
    }

    public static void InjectSoon() {
        Thread t = new Thread(() => {
            Thread.Sleep(1800);
            Write("injecting at 640,780");
            SetCursorPos(640, 780);
            Thread.Sleep(120);
            mouse_event(0x0008, 0, 0, 0, UIntPtr.Zero);
            Thread.Sleep(100);
            mouse_event(0x0010, 0, 0, 0, UIntPtr.Zero);
        });
        t.IsBackground = true;
        t.Start();
    }
}
'@

[HookProbe2]::LogPath = $log
[HookProbe2]::Callback = [HookProbe2+HookProc]{ param($code,$wparam,$lparam) [HookProbe2]::OnMouse($code,$wparam,$lparam) }
[HookProbe2]::Hook = [HookProbe2]::SetWindowsHookEx(14, [HookProbe2]::Callback, [IntPtr]::Zero, 0)
if ([HookProbe2]::Hook -eq [IntPtr]::Zero) {
  "HOOK_FAILED gle=$([Runtime.InteropServices.Marshal]::GetLastWin32Error())" | Set-Content $log
  exit 1
}
"HOOK_OK" | Set-Content $log
[HookProbe2]::InjectSoon()

$deadline = (Get-Date).AddSeconds(6)
$msg = New-Object HookProbe2+MSG
while ((Get-Date) -lt $deadline) {
  while ([HookProbe2]::PeekMessage([ref]$msg, [IntPtr]::Zero, 0, 0, 1)) {
    [void][HookProbe2]::TranslateMessage([ref]$msg)
    [void][HookProbe2]::DispatchMessage([ref]$msg)
  }
  Start-Sleep -Milliseconds 20
}

[void][HookProbe2]::UnhookWindowsHookEx([HookProbe2]::Hook)
"HOOK_END" | Add-Content $log
Get-Content $log
