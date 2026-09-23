#include <windows.h>
#include <filesystem>
#include <iostream>
#include <string>
#include "injector.h"

int main() {
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, 32768);
    const auto directory = std::filesystem::path(executable).parent_path();
    const auto hostPath = directory / L"BO2Z-Offline-Launcher.exe";
    const auto probePath = directory / L"hook_probe.dll";
    const std::wstring eventName = L"Local\\BO2ZHookTest_" + std::to_wstring(GetCurrentProcessId());
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
    if (!event) return 1;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + hostPath.wstring() + L"\"";
    const BOOL created = CreateProcessW(hostPath.c_str(), command.data(), nullptr, nullptr, FALSE,
                                        0, nullptr, directory.c_str(), &startup, &process);
    if (!created) {
        std::cerr << "Unable to launch hook host: " << GetLastError() << " path="
                  << hostPath.string() << "\n";
        CloseHandle(event);
        return 2;
    }

    HWND window{};
    for (int i = 0; i < 400 && !window; ++i) {
        window = FindWindowW(L"BO2ZOfflineLauncherWindow", L"BO2Z-Offline");
        if (!window) Sleep(10);
    }
    std::wstring error;
    const bool loaded = window && bo2z::launcher::BootstrapGame(
        process.hProcess, process.dwProcessId, probePath, eventName, true, error);
    const bool signaled = loaded && WaitForSingleObject(event, 2000) == WAIT_OBJECT_0;
    if (window) PostMessageW(window, WM_CLOSE, 0, 0);
    if (WaitForSingleObject(process.hProcess, 2000) != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 3);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(event);
    if (!signaled) {
        std::wcerr << L"Direct verified module loader failed: " << error << L"\n";
        return 4;
    }
    std::cout << "Direct verified module loader succeeded\n";
    return 0;
}
