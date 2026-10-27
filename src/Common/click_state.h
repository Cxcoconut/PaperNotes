#pragma once

#include <windows.h>

// Cross-process hand-off between the shell command DLL (runs inside the File
// Explorer surrogate) and the tray app (owns the notes).
//
// The product must place a new note near the ORIGINAL right-click position,
// never at the position of the mouse when the menu item was clicked.  Two
// independent sources are recorded:
//
//   hook       the tray app's low-level mouse hook records every physical
//              right-button-down.  Authoritative, but it needs a hook that
//              some locked-down environments refuse to deliver.
//   menuBuild  the shell DLL samples the cursor while File Explorer builds
//              OUR menu.  That happens immediately after the right-click and
//              before the menu is on screen, and it needs no hook at all.
//
// Both binaries always ship together, so this layout is frozen by the
// static_assert.  64-bit fields come first so InterlockedExchange64 is valid.
namespace papernotes {

inline constexpr wchar_t kClickStateMappingName[] =
    L"Local\\PaperNotes.ClickState.v1";

// Lets the shell extension wake an already-running app directly, so the common
// case costs one PostMessage instead of a full process creation round trip.
inline constexpr wchar_t kHiddenWindowClass[] = L"PaperNotes.HiddenWindow.v1";
inline constexpr UINT kNewNoteMessage = WM_APP + 2;

struct alignas(8) ClickState {
  volatile LONG64 rightDownTick;  // app hook: physical right-button-down
  volatile LONG64 invokeTick;     // shell: menu item invoked
  volatile LONG64 menuBuildTick;  // shell: first cursor sample of this menu

  volatile LONG menuBuildX;   // shell: physical cursor when the menu opened
  volatile LONG menuBuildY;
  volatile LONG menuBuildPid;  // shell: process that sampled it

  volatile LONG rightX;  // app hook: physical right-down point
  volatile LONG rightY;
  volatile LONG rightFlags;  // app hook: MSLLHOOKSTRUCT flags
  volatile LONG invokePid;   // shell: process that invoked the command
};

static_assert(sizeof(ClickState) == 56, "ClickState ABI changed");

}  // namespace papernotes