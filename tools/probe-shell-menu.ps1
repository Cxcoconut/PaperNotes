# Enumerate the REAL shell context menu for the desktop background by asking
# the shell to build it, without showing any UI.  This tells us whether the
# registry verb is visible to Explorer, independent of any third-party desktop
# replacement that may be drawing its own menu.

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class ShellMenuProbe {
    [DllImport("shell32.dll")]
    public static extern int SHGetDesktopFolder(out IntPtr ppshf);

    [DllImport("user32.dll")]
    public static extern IntPtr CreatePopupMenu();

    [DllImport("user32.dll")]
    public static extern bool DestroyMenu(IntPtr hMenu);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetMenuStringW(IntPtr hMenu, uint item, StringBuilder text, int max, uint flags);

    [DllImport("user32.dll")]
    public static extern int GetMenuItemCount(IntPtr hMenu);

    [DllImport("ole32.dll")]
    public static extern int CoInitializeEx(IntPtr reserved, uint flags);

    [DllImport("ole32.dll")]
    public static extern void CoUninitialize();

    [ComImport, Guid("000214E6-0000-0000-C000-000000000046"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IShellFolder {
        [PreserveSig] int ParseDisplayName(IntPtr hwnd, IntPtr pbc, string name,
            out uint eaten, out IntPtr pidl, out uint attributes);
        [PreserveSig] int EnumObjects(IntPtr hwnd, uint flags, out IntPtr enumerator);
        [PreserveSig] int BindToObject(IntPtr pidl, IntPtr pbc, ref Guid riid, out IntPtr result);
        [PreserveSig] int BindToStorage(IntPtr pidl, IntPtr pbc, ref Guid riid, out IntPtr result);
        [PreserveSig] int CompareIDs(IntPtr param, IntPtr pidl1, IntPtr pidl2);
        [PreserveSig] int CreateViewObject(IntPtr hwndOwner, ref Guid riid, out IntPtr result);
        [PreserveSig] int GetAttributesOf(uint count, IntPtr pidls, ref uint attributes);
        [PreserveSig] int GetUIObjectOf(IntPtr hwnd, uint count, IntPtr pidls,
            ref Guid riid, IntPtr reserved, out IntPtr result);
        [PreserveSig] int GetDisplayNameOf(IntPtr pidl, uint flags, IntPtr name);
        [PreserveSig] int SetNameOf(IntPtr hwnd, IntPtr pidl, string name, uint flags, out IntPtr pidlOut);
    }

    [ComImport, Guid("000214E4-0000-0000-C000-000000000046"),
     InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IContextMenu {
        [PreserveSig] int QueryContextMenu(IntPtr hMenu, uint index, uint idFirst,
            uint idLast, uint flags);
        [PreserveSig] int InvokeCommand(IntPtr info);
        [PreserveSig] int GetCommandString(UIntPtr id, uint type, IntPtr reserved,
            StringBuilder name, uint max);
    }

    public static string[] Enumerate() {
        int hrInit = CoInitializeEx(IntPtr.Zero, 0x2 | 0x4);
        try {
            IntPtr folderPtr;
            int hr = SHGetDesktopFolder(out folderPtr);
            if (hr != 0 || folderPtr == IntPtr.Zero)
                return new string[] { "SHGetDesktopFolder failed 0x" + hr.ToString("X8") };

            var folder = (IShellFolder)Marshal.GetObjectForIUnknown(folderPtr);
            Guid iid = new Guid("000214E4-0000-0000-C000-000000000046");
            IntPtr menuPtr;
            hr = folder.CreateViewObject(IntPtr.Zero, ref iid, out menuPtr);
            if (hr != 0 || menuPtr == IntPtr.Zero)
                return new string[] { "CreateViewObject(IContextMenu) failed 0x" + hr.ToString("X8") };

            var menu = (IContextMenu)Marshal.GetObjectForIUnknown(menuPtr);
            IntPtr hMenu = CreatePopupMenu();
            // CMF_NORMAL
            hr = menu.QueryContextMenu(hMenu, 0, 1, 0x7FFF, 0x00000000);
            int count = GetMenuItemCount(hMenu);
            var items = new System.Collections.Generic.List<string>();
            items.Add("QueryContextMenu hr=0x" + hr.ToString("X8") + " items=" + count);
            for (uint i = 0; i < count; i++) {
                var sb = new StringBuilder(512);
                // MF_BYPOSITION
                int len = GetMenuStringW(hMenu, i, sb, sb.Capacity, 0x400);
                string text = len > 0 ? sb.ToString() : "(separator)";
                items.Add("  [" + i + "] " + text);
            }
            DestroyMenu(hMenu);
            Marshal.ReleaseComObject(menu);
            Marshal.ReleaseComObject(folder);
            return items.ToArray();
        } finally {
            if (hrInit >= 0) CoUninitialize();
        }
    }
}
'@

$lines = [ShellMenuProbe]::Enumerate()
$lines | ForEach-Object { $_ }

$label = -join [char[]](0x65B0, 0x5EFA, 0x4FBF, 0x7B7E)
$hit = $lines | Where-Object { $_ -like "*$label*" }
"----"
if ($hit) {
  "VERB_VISIBLE_TO_SHELL: yes"
  $hit | ForEach-Object { $_ }
} else {
  "VERB_VISIBLE_TO_SHELL: no"
}