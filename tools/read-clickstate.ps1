param(
  [string]$MappingName = 'Local\PaperNotes.ClickState.v1'
)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

Add-Type @'
using System;
using System.Runtime.InteropServices;

[StructLayout(LayoutKind.Sequential, Pack = 8)]
public struct ClickState {
    public long rightDownTick;
    public long invokeTick;
    public long menuBuildTick;
    public int menuBuildX;
    public int menuBuildY;
    public int menuBuildPid;
    public int rightX;
    public int rightY;
    public int rightFlags;
    public int invokePid;
}

public static class ClickStateReader {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr OpenFileMapping(uint access, bool inherit, string name);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr MapViewOfFile(IntPtr mapping, uint access, uint offsetHigh, uint offsetLow, UIntPtr bytes);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool UnmapViewOfFile(IntPtr address);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll")]
    public static extern ulong GetTickCount64();

    public static string Read(string name) {
        IntPtr mapping = OpenFileMapping(0x0004, false, name);
        if (mapping == IntPtr.Zero) return "ABSENT gle=" + Marshal.GetLastWin32Error();
        try {
            IntPtr view = MapViewOfFile(mapping, 0x0004, 0, 0, (UIntPtr)Marshal.SizeOf(typeof(ClickState)));
            if (view == IntPtr.Zero) return "MAP_FAILED gle=" + Marshal.GetLastWin32Error();
            try {
                ClickState s = (ClickState)Marshal.PtrToStructure(view, typeof(ClickState));
                ulong now = GetTickCount64();
                long ra = s.rightDownTick == 0 ? -1 : (long)(now - (ulong)s.rightDownTick);
                long ba = s.menuBuildTick == 0 ? -1 : (long)(now - (ulong)s.menuBuildTick);
                long ia = s.invokeTick == 0 ? -1 : (long)(now - (ulong)s.invokeTick);
                return string.Format(
                    "hook  : tick={0} ageMs={1} x={2} y={3} flags=0x{4:X}\r\n" +
                    "build : tick={5} ageMs={6} x={7} y={8} pid={9}\r\n" +
                    "invoke: tick={10} ageMs={11} pid={12}",
                    s.rightDownTick, ra, s.rightX, s.rightY, s.rightFlags,
                    s.menuBuildTick, ba, s.menuBuildX, s.menuBuildY, s.menuBuildPid,
                    s.invokeTick, ia, s.invokePid);
            } finally { UnmapViewOfFile(view); }
        } finally { CloseHandle(mapping); }
    }
}
'@

[ClickStateReader]::Read($MappingName)