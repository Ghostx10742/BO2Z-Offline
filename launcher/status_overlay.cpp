#include "status_overlay.h"
#include <windows.h>

namespace bo2z::launcher {
namespace {
constexpr wchar_t kOverlayClass[] = L"BO2ZOfflineStatusOverlay";
constexpr int kWidth = 192;
constexpr int kHeight = 30;

struct GameWindowSearch {
    DWORD processId{};
    HWND window{};
};

BOOL CALLBACK FindGameWindow(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<GameWindowSearch*>(parameter);
    DWORD processId{};
    GetWindowThreadProcessId(window, &processId);
    if (processId != search.processId || !IsWindowVisible(window) ||
        GetWindow(window, GW_OWNER) || IsIconic(window)) return TRUE;
    RECT client{};
    if (!GetClientRect(window, &client) || client.right < 100 || client.bottom < 100)
        return TRUE;
    search.window = window;
    return FALSE;
}

LRESULT CALLBACK OverlayProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT bounds{0, 0, kWidth, kHeight};
        HBRUSH background = CreateSolidBrush(RGB(13, 13, 13));
        FillRect(dc, &bounds, background);
        DeleteObject(background);
        RECT stripe{0, 0, 4, kHeight};
        HBRUSH orange = CreateSolidBrush(RGB(232, 76, 15));
        FillRect(dc, &stripe, orange);
        DeleteObject(orange);
        HFONT font = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
            FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, FF_DONTCARE, L"Bahnschrift SemiCondensed");
        HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(245, 245, 240));
        RECT label{12, 3, kWidth - 5, kHeight - 3};
        DrawTextW(dc, L"In Offline Mode", -1, &label,
                  DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        if (oldFont) SelectObject(dc, oldFont);
        if (font) DeleteObject(font);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
}

HWND CreateStatusOverlay(HINSTANCE instance) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = instance;
    wc.lpszClassName = kOverlayClass;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return nullptr;
    HWND window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT |
            WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            kOverlayClass, L"BO2Z Offline Status", WS_POPUP,
            0, 0, kWidth, kHeight, nullptr, nullptr, instance, nullptr);
    if (!window) return nullptr;
    if (!SetLayeredWindowAttributes(window, 0, 238, LWA_ALPHA)) {
        DestroyWindow(window);
        return nullptr;
    }
    return window;
}

void UpdateStatusOverlay(HWND overlay, DWORD gameProcessId) {
    if (!overlay || !gameProcessId) return;
    GameWindowSearch search{gameProcessId};
    EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&search));
    if (!search.window) { ShowWindow(overlay, SW_HIDE); return; }

    DWORD foregroundProcess{};
    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundProcess);
    if (foregroundProcess != gameProcessId) {
        ShowWindow(overlay, SW_HIDE);
        return;
    }
    POINT topLeft{0, 0};
    if (!ClientToScreen(search.window, &topLeft)) {
        ShowWindow(overlay, SW_HIDE);
        return;
    }
    SetWindowPos(overlay, HWND_TOPMOST, topLeft.x + 12, topLeft.y + 12,
                 kWidth, kHeight, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}
}
