#pragma once
#include <windows.h>

namespace bo2z::launcher {
// A small click-through window owned by the launcher. It never runs inside BO2's
// renderer and is destroyed when the game process exits.
HWND CreateStatusOverlay(HINSTANCE instance);
void UpdateStatusOverlay(HWND overlay, DWORD gameProcessId);
}
