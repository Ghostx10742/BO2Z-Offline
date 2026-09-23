#include <windows.h>
#include <windowsx.h>
#include <tlhelp32.h>
#include <bcrypt.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <process.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "resource.h"
#include "injector.h"
#include "status_overlay.h"

using namespace Gdiplus;
namespace fs = std::filesystem;

namespace {
constexpr wchar_t kClassName[] = L"BO2ZOfflineLauncherWindow";
constexpr wchar_t kTitle[] = L"BO2Z-Offline";
constexpr UINT kLaunchFinished = WM_APP + 17;
constexpr int kBaseWidth = 1200;
constexpr int kBaseHeight = 690;

HINSTANCE g_instance{};
std::unique_ptr<Bitmap> g_cover;
ULONG_PTR g_gdiplusToken{};
bool g_playHover{};
bool g_toggleHover{};
bool g_closeHover{};
bool g_minHover{};
bool g_indicatorEnabled{true};
bool g_launching{};
std::atomic<DWORD> g_launchedProcessId{};
HANDLE g_gameProcess{};
HWND g_statusOverlay{};
std::wstring g_status = L"STEAM OFFLINE MODE RECOMMENDED";

struct UiLayout {
    RectF play;
    RectF toggle;
    RectF close;
    RectF minimize;
    RectF tooltip;
};

UiLayout Layout(float width, float height) {
    const float scale = std::min(width / kBaseWidth, height / kBaseHeight);
    const float playW = 330.0f * scale;
    const float playH = 60.0f * scale;
    const float gap = 22.0f * scale;
    const float toggleW = 255.0f * scale;
    const float toggleH = 54.0f * scale;
    const float totalW = playW + gap + toggleW;
    const float x = (width - totalW) * 0.5f;
    const float y = height - 108.0f * scale;
    UiLayout out{};
    out.play = RectF(x, y, playW, playH);
    out.toggle = RectF(x + playW + gap, y + 3.0f * scale, toggleW, toggleH);
    out.close = RectF(width - 47.0f * scale, 13.0f * scale, 32.0f * scale, 28.0f * scale);
    out.minimize = RectF(width - 86.0f * scale, 13.0f * scale, 32.0f * scale, 28.0f * scale);
    out.tooltip = RectF(out.toggle.X - 76.0f * scale, out.toggle.Y - 91.0f * scale,
                        342.0f * scale, 75.0f * scale);
    return out;
}

bool Contains(const RectF& r, float x, float y) {
    return x >= r.X && y >= r.Y && x < r.X + r.Width && y < r.Y + r.Height;
}

std::unique_ptr<Bitmap> LoadCover() {
    HRSRC res = FindResourceW(g_instance, MAKEINTRESOURCEW(IDR_COVER_PNG), RT_RCDATA);
    if (!res) return {};
    const DWORD bytes = SizeofResource(g_instance, res);
    HGLOBAL loaded = LoadResource(g_instance, res);
    const void* source = loaded ? LockResource(loaded) : nullptr;
    if (!source || !bytes) return {};
    HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!copy) return {};
    void* target = GlobalLock(copy);
    memcpy(target, source, bytes);
    GlobalUnlock(copy);
    IStream* stream{};
    if (FAILED(CreateStreamOnHGlobal(copy, TRUE, &stream))) {
        GlobalFree(copy);
        return {};
    }
    std::unique_ptr<Bitmap> decoded(Bitmap::FromStream(stream));
    std::unique_ptr<Bitmap> detached;
    if (decoded && decoded->GetLastStatus() == Ok) {
        detached.reset(decoded->Clone(0, 0, decoded->GetWidth(), decoded->GetHeight(), PixelFormat32bppARGB));
    }
    stream->Release();
    return detached;
}

std::unique_ptr<Font> MakeFont(float size, INT style = FontStyleRegular) {
    auto font = std::make_unique<Font>(L"Bahnschrift SemiCondensed", size, style, UnitPixel);
    if (font->GetLastStatus() != Ok)
        font = std::make_unique<Font>(L"Arial Narrow", size, style, UnitPixel);
    return font;
}

void DrawCentered(Graphics& graphics, const wchar_t* text, const RectF& rect, Font& font, Color color) {
    StringFormat format;
    format.SetAlignment(StringAlignmentCenter);
    format.SetLineAlignment(StringAlignmentCenter);
    SolidBrush brush(color);
    graphics.DrawString(text, -1, &font, rect, &format, &brush);
}

void DrawCover(Graphics& graphics, float width, float height) {
    if (!g_cover) {
        graphics.Clear(Color(255, 10, 10, 10));
        return;
    }
    const float imageW = static_cast<float>(g_cover->GetWidth());
    const float imageH = static_cast<float>(g_cover->GetHeight());
    const float scale = std::max(width / imageW, height / imageH);
    const float srcW = width / scale;
    const float srcH = height / scale;
    const float srcX = (imageW - srcW) * 0.5f;
    const float srcY = (imageH - srcH) * 0.5f;
    graphics.DrawImage(g_cover.get(), RectF(0, 0, width, height), srcX, srcY, srcW, srcH, UnitPixel);
}

void RenderUi(Graphics& graphics, int width, int height, bool preview = false) {
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    graphics.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    DrawCover(graphics, static_cast<float>(width), static_cast<float>(height));
    const float scale = std::min(width / static_cast<float>(kBaseWidth), height / static_cast<float>(kBaseHeight));
    const auto ui = Layout(static_cast<float>(width), static_cast<float>(height));

    SolidBrush overallDim(Color(50, 0, 0, 0));
    graphics.FillRectangle(&overallDim, 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    LinearGradientBrush footer(PointF(0, height - 165.0f * scale), PointF(0, static_cast<float>(height)),
        Color(10, 0, 0, 0), Color(225, 3, 3, 3));
    graphics.FillRectangle(&footer, 0.0f, height - 180.0f * scale, static_cast<float>(width), 180.0f * scale);

    auto tiny = MakeFont(14.0f * scale, FontStyleBold);
    SolidBrush topShadow(Color(190, 0, 0, 0));
    graphics.FillRectangle(&topShadow, 18.0f * scale, 16.0f * scale, 190.0f * scale, 27.0f * scale);
    SolidBrush orange(Color(255, 230, 78, 12));
    graphics.FillRectangle(&orange, 18.0f * scale, 16.0f * scale, 4.0f * scale, 27.0f * scale);
    SolidBrush white(Color(245, 245, 245, 240));
    graphics.DrawString(L"BO2Z  //  OFFLINE", -1, tiny.get(), PointF(30.0f * scale, 20.0f * scale), &white);

    const bool playHover = preview || g_playHover;
    SolidBrush playFill(playHover ? Color(238, 191, 54, 5) : Color(226, 12, 12, 12));
    graphics.FillRectangle(&playFill, ui.play);
    Pen playBorder(playHover ? Color(255, 255, 135, 38) : Color(220, 126, 126, 126), playHover ? 2.0f : 1.0f);
    graphics.DrawRectangle(&playBorder, ui.play);
    graphics.FillRectangle(&orange, ui.play.X, ui.play.Y, 7.0f * scale, ui.play.Height);
    auto playFont = MakeFont(27.0f * scale, FontStyleBold);
    DrawCentered(graphics, g_launching ? L"STARTING..." : L"PLAY OFFLINE", ui.play, *playFont,
                 g_launching ? Color(220, 190, 190, 190) : Color(255, 255, 255, 255));

    const bool toggleHover = preview || g_toggleHover;
    SolidBrush toggleFill(toggleHover ? Color(238, 28, 28, 28) : Color(218, 9, 9, 9));
    graphics.FillRectangle(&toggleFill, ui.toggle);
    Pen toggleBorder(toggleHover ? Color(255, 230, 78, 12) : Color(190, 100, 100, 100), 1.0f);
    graphics.DrawRectangle(&toggleBorder, ui.toggle);
    auto toggleFont = MakeFont(13.0f * scale, FontStyleBold);
    SolidBrush muted(Color(240, 225, 225, 220));
    graphics.DrawString(L"OFFLINE STATUS", -1, toggleFont.get(),
                        PointF(ui.toggle.X + 14.0f * scale, ui.toggle.Y + 9.0f * scale), &muted);
    auto stateFont = MakeFont(11.0f * scale, FontStyleRegular);
    SolidBrush stateBrush(g_indicatorEnabled ? Color(255, 242, 117, 34) : Color(220, 150, 150, 150));
    graphics.DrawString(g_indicatorEnabled ? L"VISIBLE IN GAME" : L"HIDDEN IN GAME", -1, stateFont.get(),
                        PointF(ui.toggle.X + 14.0f * scale, ui.toggle.Y + 30.0f * scale), &stateBrush);
    RectF switchRect(ui.toggle.GetRight() - 67.0f * scale, ui.toggle.Y + 15.0f * scale,
                     50.0f * scale, 24.0f * scale);
    SolidBrush switchBack(g_indicatorEnabled ? Color(255, 191, 54, 5) : Color(255, 65, 65, 65));
    graphics.FillRectangle(&switchBack, switchRect);
    const float knobX = g_indicatorEnabled ? switchRect.GetRight() - 21.0f * scale : switchRect.X + 3.0f * scale;
    SolidBrush knob(Color(255, 245, 241, 230));
    graphics.FillRectangle(&knob, knobX, switchRect.Y + 3.0f * scale, 18.0f * scale, 18.0f * scale);

    auto noteFont = MakeFont(12.5f * scale, FontStyleBold);
    RectF noteRect(ui.play.X, ui.play.Y - 30.0f * scale, ui.play.Width, 20.0f * scale);
    DrawCentered(graphics, g_status.c_str(), noteRect, *noteFont, Color(235, 225, 225, 218));

    if (toggleHover) {
        SolidBrush tipBack(Color(245, 7, 7, 7));
        graphics.FillRectangle(&tipBack, ui.tooltip);
        Pen tipBorder(Color(245, 230, 78, 12), 1.0f);
        graphics.DrawRectangle(&tipBorder, ui.tooltip);
        auto tipFont = MakeFont(12.0f * scale, FontStyleRegular);
        StringFormat tipFormat;
        tipFormat.SetTrimming(StringTrimmingWord);
        SolidBrush tipText(Color(255, 238, 238, 233));
        RectF textRect(ui.tooltip.X + 12.0f * scale, ui.tooltip.Y + 9.0f * scale,
                       ui.tooltip.Width - 24.0f * scale, ui.tooltip.Height - 18.0f * scale);
        graphics.DrawString(L"Shows 'In Offline Mode' in the top-left corner while you play. This is only a visual reminder and does not affect your offline save.",
                            -1, tipFont.get(), textRect, &tipFormat, &tipText);
    }

    SolidBrush windowButton(Color(160, 0, 0, 0));
    graphics.FillRectangle(&windowButton, ui.minimize);
    graphics.FillRectangle(&windowButton, ui.close);
    if (g_minHover) { SolidBrush h(Color(220, 70, 70, 70)); graphics.FillRectangle(&h, ui.minimize); }
    if (g_closeHover) { SolidBrush h(Color(235, 153, 38, 15)); graphics.FillRectangle(&h, ui.close); }
    Pen glyph(Color(255, 238, 238, 238), 1.5f * scale);
    graphics.DrawLine(&glyph, ui.minimize.X + 10.0f * scale, ui.minimize.Y + 18.0f * scale,
                      ui.minimize.GetRight() - 10.0f * scale, ui.minimize.Y + 18.0f * scale);
    graphics.DrawLine(&glyph, ui.close.X + 10.0f * scale, ui.close.Y + 8.0f * scale,
                      ui.close.GetRight() - 10.0f * scale, ui.close.GetBottom() - 8.0f * scale);
    graphics.DrawLine(&glyph, ui.close.GetRight() - 10.0f * scale, ui.close.Y + 8.0f * scale,
                      ui.close.X + 10.0f * scale, ui.close.GetBottom() - 8.0f * scale);
}

fs::path ExecutableDirectory() {
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    return length ? fs::path(std::wstring(path.data(), length)).parent_path() : fs::path{};
}

bool ProcessRunning(const wchar_t* executable) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{sizeof(entry)};
    bool found = false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (!_wcsicmp(entry.szExeFile, executable)) { found = true; break; }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return found;
}

bool WaitForProcess(const wchar_t* executable, HANDLE& process, DWORD& processId,
                    std::wstring& error) {
    const ULONGLONG deadline = GetTickCount64() + 60000;
    while (GetTickCount64() < deadline) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W entry{sizeof(entry)};
            if (Process32FirstW(snapshot, &entry)) {
                do {
                    if (_wcsicmp(entry.szExeFile, executable)) continue;
                    HANDLE candidate = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE,
                        FALSE, entry.th32ProcessID);
                    if (!candidate) continue;
                    Sleep(350);
                    if (WaitForSingleObject(candidate, 0) == WAIT_TIMEOUT) {
                        process = candidate;
                        processId = entry.th32ProcessID;
                        CloseHandle(snapshot);
                        return true;
                    }
                    CloseHandle(candidate);
                } while (Process32NextW(snapshot, &entry));
            }
            CloseHandle(snapshot);
        }
        Sleep(50);
    }
    error = L"Steam did not start the Black Ops II Zombies process within 60 seconds.";
    return false;
}

struct GameWindowSearch {
    DWORD processId{};
    HWND window{};
    DWORD threadId{};
};

BOOL CALLBACK FindGameWindow(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<GameWindowSearch*>(parameter);
    DWORD processId{};
    const DWORD threadId = GetWindowThreadProcessId(window, &processId);
    if (processId != search.processId || !threadId || !IsWindowVisible(window) ||
        GetWindow(window, GW_OWNER) != nullptr) return TRUE;
    search.window = window;
    search.threadId = threadId;
    return FALSE;
}

bool WaitForGameWindow(HANDLE process, DWORD processId, HWND& window, DWORD& threadId) {
    const ULONGLONG deadline = GetTickCount64() + 30000;
    while (GetTickCount64() < deadline) {
        if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) return false;
        GameWindowSearch search{processId};
        EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&search));
        if (search.window && search.threadId) {
            window = search.window;
            threadId = search.threadId;
            return true;
        }
        Sleep(25);
    }
    return false;
}

std::wstring Win32Error(const wchar_t* operation) {
    const DWORD error = GetLastError();
    wchar_t* message{};
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
                   reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring result = operation;
    result += L" failed (" + std::to_wstring(error) + L")";
    if (message) { result += L": "; result += message; LocalFree(message); }
    return result;
}

bool Sha256File(const fs::path& path, std::wstring& digest) {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    DWORD objectBytes{}, hashBytes{}, received{};
    std::vector<unsigned char> object;
    std::vector<unsigned char> result;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) goto done;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes),
                          sizeof(objectBytes), &received, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashBytes),
                          sizeof(hashBytes), &received, 0) < 0) goto done;
    object.resize(objectBytes); result.resize(hashBytes);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0) < 0) goto done;
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) goto done;
        // Keep this off the worker-thread stack.  The 32-bit launcher's default
        // thread stack is only 1 MiB, so the old 1 MiB local array overflowed
        // before BO2 could even be created.
        std::vector<char> buffer(64 * 1024);
        while (file) {
            file.read(buffer.data(), buffer.size());
            const auto count = file.gcount();
            if (count > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()),
                                            static_cast<ULONG>(count), 0) < 0) goto done;
        }
        if (!file.eof()) goto done;
    }
    if (BCryptFinishHash(hash, result.data(), hashBytes, 0) < 0) goto done;
    {
        static constexpr wchar_t hex[] = L"0123456789ABCDEF";
        digest.clear(); digest.reserve(result.size() * 2);
        for (unsigned char byte : result) { digest.push_back(hex[byte >> 4]); digest.push_back(hex[byte & 15]); }
    }
    ok = true;
done:
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

void RestoreEnvironment(const wchar_t* name, const std::wstring& oldValue, bool existed) {
    SetEnvironmentVariableW(name, existed ? oldValue.c_str() : nullptr);
}

std::pair<bool, std::wstring> ReadEnvironment(const wchar_t* name) {
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (!needed) return {false, {}};
    std::wstring value(needed, L'\0');
    GetEnvironmentVariableW(name, value.data(), needed);
    if (!value.empty() && value.back() == L'\0') value.pop_back();
    return {true, value};
}

bool LaunchOffline(std::wstring& error, DWORD& launchedProcessId) {
    const fs::path directory = ExecutableDirectory();
    const fs::path game = directory / L"t6zm.exe";
    const fs::path moduleRoot = directory / L"BO2Z-Offline";
    const fs::path module = moduleRoot / L"BO2Z-Offline.dll";
    const fs::path dataRoot = moduleRoot / L"data";
    const fs::path publisher = dataRoot / L"pub" / L"online_tu17_zm.wad";
    if (!fs::is_regular_file(game)) {
        error = L"t6zm.exe was not found. Copy the BO2Z-Offline files into the Black Ops II game folder.";
        return false;
    }
    std::wstring executableHash;
    if (!Sha256File(game, executableHash) ||
        executableHash != L"F6F7104AF2BD0C2B931EA1E739969E85845E499AFFBF686144C6B490B344AB1E") {
        error = L"This t6zm.exe build is not supported. Verify the original Steam game files before using offline mode.";
        return false;
    }
    if (!fs::is_regular_file(module)) {
        error = L"BO2Z-Offline.dll is missing. Reinstall the complete BO2Z-Offline package.";
        return false;
    }
    if (!fs::is_regular_file(publisher)) {
        error = L"BO2Z-Offline\\data is incomplete. Reinstall the complete package before playing.";
        return false;
    }
    if (ProcessRunning(L"t6zm.exe")) {
        error = L"Black Ops II Zombies is already running. Close it before starting offline mode.";
        return false;
    }

    SHELLEXECUTEINFOW launch{sizeof(launch)};
    launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    launch.lpVerb = L"open";
    launch.lpFile = L"steam://run/212910";
    launch.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&launch)) {
        error = Win32Error(L"Asking Steam to start Black Ops II Zombies");
        return false;
    }
    if (launch.hProcess) CloseHandle(launch.hProcess);

    HANDLE process{};
    DWORD processId{};
    if (!WaitForProcess(L"t6zm.exe", process, processId, error)) return false;
    // The status text belongs to the launcher overlay, never to BO2's renderer.
    if (!bo2z::launcher::BootstrapGame(process, processId, module, dataRoot,
                                       false, error)) {
        TerminateProcess(process, 0xB02);
        CloseHandle(process);
        return false;
    }
    launchedProcessId = processId;
    CloseHandle(process);
    return true;
}

unsigned __stdcall LaunchWorker(void* parameter) {
    const HWND window = static_cast<HWND>(parameter);
    auto* message = new std::wstring;
    DWORD processId{};
    const bool success = LaunchOffline(*message, processId);
    if (success) g_launchedProcessId.store(processId, std::memory_order_release);
    PostMessageW(window, kLaunchFinished, success ? 1 : 0, reinterpret_cast<LPARAM>(message));
    return 0;
}

int GetEncoderClsid(const WCHAR* format, CLSID* clsid) {
    UINT count{}, bytes{};
    GetImageEncodersSize(&count, &bytes);
    if (!bytes) return -1;
    std::vector<BYTE> storage(bytes);
    auto* info = reinterpret_cast<ImageCodecInfo*>(storage.data());
    GetImageEncoders(count, bytes, info);
    for (UINT i = 0; i < count; ++i) {
        if (!wcscmp(info[i].MimeType, format)) { *clsid = info[i].Clsid; return static_cast<int>(i); }
    }
    return -1;
}

bool SavePreview(const fs::path& target) {
    Bitmap bitmap(kBaseWidth, kBaseHeight, PixelFormat32bppARGB);
    Graphics graphics(&bitmap);
    RenderUi(graphics, kBaseWidth, kBaseHeight, true);
    CLSID png{};
    return GetEncoderClsid(L"image/png", &png) >= 0 && bitmap.Save(target.c_str(), &png) == Ok;
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{}; GetClientRect(window, &client);
        HDC memory = CreateCompatibleDC(dc);
        HBITMAP buffer = CreateCompatibleBitmap(dc, client.right, client.bottom);
        HGDIOBJ old = SelectObject(memory, buffer);
        { Graphics graphics(memory); RenderUi(graphics, client.right, client.bottom); }
        BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
        SelectObject(memory, old); DeleteObject(buffer); DeleteDC(memory);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_MOUSEMOVE: {
        RECT client{}; GetClientRect(window, &client);
        const auto ui = Layout(static_cast<float>(client.right), static_cast<float>(client.bottom));
        const float x = static_cast<float>(GET_X_LPARAM(lParam));
        const float y = static_cast<float>(GET_Y_LPARAM(lParam));
        const bool play = Contains(ui.play, x, y);
        const bool toggle = Contains(ui.toggle, x, y);
        const bool close = Contains(ui.close, x, y);
        const bool minimize = Contains(ui.minimize, x, y);
        if (play != g_playHover || toggle != g_toggleHover || close != g_closeHover || minimize != g_minHover) {
            g_playHover = play; g_toggleHover = toggle; g_closeHover = close; g_minHover = minimize;
            InvalidateRect(window, nullptr, FALSE);
        }
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0}; TrackMouseEvent(&track);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_playHover = g_toggleHover = g_closeHover = g_minHover = false;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        RECT client{}; GetClientRect(window, &client);
        const auto ui = Layout(static_cast<float>(client.right), static_cast<float>(client.bottom));
        const float x = static_cast<float>(GET_X_LPARAM(lParam));
        const float y = static_cast<float>(GET_Y_LPARAM(lParam));
        if (Contains(ui.close, x, y)) { DestroyWindow(window); return 0; }
        if (Contains(ui.minimize, x, y)) { ShowWindow(window, SW_MINIMIZE); return 0; }
        if (Contains(ui.toggle, x, y) && !g_launching) {
            g_indicatorEnabled = !g_indicatorEnabled;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (Contains(ui.play, x, y) && !g_launching) {
            g_launching = true; g_status = L"STARTING LOCAL OFFLINE SERVICES";
            InvalidateRect(window, nullptr, FALSE);
            const uintptr_t thread = _beginthreadex(nullptr, 0, LaunchWorker, window, 0, nullptr);
            if (thread) CloseHandle(reinterpret_cast<HANDLE>(thread));
            else PostMessageW(window, kLaunchFinished, 0,
                reinterpret_cast<LPARAM>(new std::wstring(Win32Error(L"Creating launcher worker"))));
            return 0;
        }
        break;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) { DestroyWindow(window); return 0; }
        if (wParam == 'T' && !g_launching) { g_indicatorEnabled = !g_indicatorEnabled; InvalidateRect(window, nullptr, FALSE); return 0; }
        if ((wParam == VK_RETURN || wParam == VK_SPACE) && !g_launching) {
            RECT client{}; GetClientRect(window, &client);
            SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(static_cast<int>(client.right / 2 - 120), client.bottom - 78));
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        const LRESULT hit = DefWindowProcW(window, message, wParam, lParam);
        if (hit != HTCLIENT) return hit;
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)}; ScreenToClient(window, &point);
        RECT client{}; GetClientRect(window, &client);
        const auto ui = Layout(static_cast<float>(client.right), static_cast<float>(client.bottom));
        if (!Contains(ui.play, static_cast<float>(point.x), static_cast<float>(point.y)) &&
            !Contains(ui.toggle, static_cast<float>(point.x), static_cast<float>(point.y)) &&
            !Contains(ui.close, static_cast<float>(point.x), static_cast<float>(point.y)) &&
            !Contains(ui.minimize, static_cast<float>(point.x), static_cast<float>(point.y))) return HTCAPTION;
        return HTCLIENT;
    }
    case kLaunchFinished: {
        std::unique_ptr<std::wstring> detail(reinterpret_cast<std::wstring*>(lParam));
        if (wParam) {
            g_status = L"OFFLINE MODE STARTED";
            InvalidateRect(window, nullptr, FALSE);
            if (g_indicatorEnabled) {
                const DWORD processId = g_launchedProcessId.load(std::memory_order_acquire);
                g_gameProcess = OpenProcess(SYNCHRONIZE, FALSE, processId);
                g_statusOverlay = bo2z::launcher::CreateStatusOverlay(g_instance);
                if (g_gameProcess && g_statusOverlay) {
                    ShowWindow(window, SW_HIDE);
                    bo2z::launcher::UpdateStatusOverlay(g_statusOverlay, processId);
                    SetTimer(window, 2, 100, nullptr);
                } else {
                    MessageBoxW(window, L"Offline play started, but the status label could not be displayed.",
                                L"BO2Z-Offline", MB_OK | MB_ICONWARNING);
                    SetTimer(window, 1, 650, nullptr);
                }
            } else {
                SetTimer(window, 1, 650, nullptr);
            }
        } else {
            g_launching = false;
            g_status = L"COULD NOT START OFFLINE MODE";
            InvalidateRect(window, nullptr, FALSE);
            MessageBoxW(window, detail && !detail->empty() ? detail->c_str() : L"Unknown launcher error.",
                        L"BO2Z-Offline", MB_OK | MB_ICONERROR);
        }
        return 0;
    }
    case WM_TIMER:
        if (wParam == 1) { KillTimer(window, 1); DestroyWindow(window); }
        if (wParam == 2) {
            if (!g_gameProcess || WaitForSingleObject(g_gameProcess, 0) != WAIT_TIMEOUT) {
                KillTimer(window, 2);
                DestroyWindow(window);
            } else {
                bo2z::launcher::UpdateStatusOverlay(
                    g_statusOverlay, g_launchedProcessId.load(std::memory_order_acquire));
            }
        }
        return 0;
    case WM_DESTROY:
        if (g_statusOverlay) { DestroyWindow(g_statusOverlay); g_statusOverlay = nullptr; }
        if (g_gameProcess) { CloseHandle(g_gameProcess); g_gameProcess = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    g_instance = instance;
    SetProcessDPIAware();
    if (commandLine && wcsstr(commandLine, L"--verify-game")) {
        std::wstring digest;
        const fs::path game = ExecutableDirectory() / L"t6zm.exe";
        return Sha256File(game, digest) &&
               digest == L"F6F7104AF2BD0C2B931EA1E739969E85845E499AFFBF686144C6B490B344AB1E" ? 0 : 5;
    }
    GdiplusStartupInput startup;
    if (GdiplusStartup(&g_gdiplusToken, &startup, nullptr) != Ok) return 1;
    g_cover = LoadCover();
    if (commandLine && wcsstr(commandLine, L"--preview")) {
        fs::path output = ExecutableDirectory() / L"BO2Z-Offline-launcher-preview.png";
        const wchar_t* quote = wcschr(commandLine, L'\"');
        if (quote) {
            const wchar_t* end = wcschr(quote + 1, L'\"');
            if (end) output = std::wstring(quote + 1, end);
        }
        const bool saved = SavePreview(output);
        g_cover.reset(); GdiplusShutdown(g_gdiplusToken);
        return saved ? 0 : 2;
    }

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = kClassName;
    if (!RegisterClassExW(&windowClass)) return 3;

    RECT workArea{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0)) {
        workArea.right = GetSystemMetrics(SM_CXSCREEN);
        workArea.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    const int screenW = workArea.right - workArea.left;
    const int screenH = workArea.bottom - workArea.top;
    const float fit = std::min(1.0f, std::min((screenW - 80.0f) / kBaseWidth, (screenH - 80.0f) / kBaseHeight));
    const int width = static_cast<int>(kBaseWidth * fit);
    const int height = static_cast<int>(kBaseHeight * fit);
    const int x = workArea.left + (screenW - width) / 2;
    const int y = workArea.top + (screenH - height) / 2;
    HWND window = CreateWindowExW(WS_EX_APPWINDOW, kClassName, kTitle, WS_POPUP,
                                  x, y, width, height, nullptr, nullptr, instance, nullptr);
    if (!window) return 4;
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    g_cover.reset();
    GdiplusShutdown(g_gdiplusToken);
    return static_cast<int>(message.wParam);
}
