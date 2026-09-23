#include "injector.h"
#include <tlhelp32.h>
#include <cstdint>
#include "launcher_bootstrap.h"

namespace bo2z::launcher {
namespace {
std::wstring ErrorText(const wchar_t* operation, DWORD code = GetLastError()) {
    wchar_t* message{};
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                   reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring result = operation;
    result += L" failed (" + std::to_wstring(code) + L")";
    if (message) { result += L": "; result += message; LocalFree(message); }
    return result;
}

std::uintptr_t RemoteModule(DWORD processId, const wchar_t* name) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, processId);
        if (snapshot != INVALID_HANDLE_VALUE) {
            MODULEENTRY32W entry{sizeof(entry)};
            if (Module32FirstW(snapshot, &entry)) {
                do {
                    if (!_wcsicmp(entry.szModule, name)) {
                        const auto base = reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
                        CloseHandle(snapshot);
                        return base;
                    }
                } while (Module32NextW(snapshot, &entry));
            }
            CloseHandle(snapshot);
        }
        Sleep(20);
    }
    return 0;
}
}

bool InjectLibrary(HANDLE process, DWORD processId, const std::filesystem::path& library,
                   std::wstring& error, std::uintptr_t* remoteModule) {
    const std::wstring path = library.wstring();
    const SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);
    const auto localKernel = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(L"kernel32.dll"));
    const auto localLoad = reinterpret_cast<std::uintptr_t>(GetProcAddress(
        reinterpret_cast<HMODULE>(localKernel), "LoadLibraryW"));
    const auto remoteKernel = RemoteModule(processId, L"kernel32.dll");
    if (!localKernel || !localLoad || !remoteKernel) {
        error = L"The game initialized without a usable Windows loader address.";
        return false;
    }
    const auto remoteLoad = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        remoteKernel + (localLoad - localKernel));
    void* remotePath = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath) { error = ErrorText(L"Allocating the module path in the game"); return false; }
    SIZE_T written{};
    if (!WriteProcessMemory(process, remotePath, path.c_str(), bytes, &written) || written != bytes) {
        error = ErrorText(L"Writing the module path into the game");
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, remoteLoad, remotePath, 0, nullptr);
    if (!thread) {
        error = ErrorText(L"Loading BO2Z-Offline.dll in the game");
        VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
        return false;
    }
    const DWORD wait = WaitForSingleObject(thread, 15000);
    DWORD moduleHandle{};
    const bool completed = wait == WAIT_OBJECT_0 && GetExitCodeThread(thread, &moduleHandle) && moduleHandle;
    if (!completed) {
        error = wait == WAIT_TIMEOUT ? L"Loading BO2Z-Offline.dll timed out."
                                     : ErrorText(L"Confirming BO2Z-Offline.dll in the game");
    }
    CloseHandle(thread);
    VirtualFreeEx(process, remotePath, 0, MEM_RELEASE);
    if (completed && remoteModule) *remoteModule = moduleHandle;
    return completed;
}

bool BootstrapGame(HANDLE process, DWORD processId, const std::filesystem::path& library,
                   const std::filesystem::path& dataRoot, bool showIndicator,
                   std::wstring& error) {
    std::uintptr_t remoteModule{};
    if (!InjectLibrary(process, processId, library, error, &remoteModule)) return false;

    HMODULE localModule = LoadLibraryExW(library.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!localModule) { error = ErrorText(L"Inspecting the offline bootstrap export"); return false; }
    const auto localProcedure = reinterpret_cast<std::uintptr_t>(
        GetProcAddress(localModule, "OfflineOpBootstrap"));
    if (!localProcedure) {
        error = L"BO2Z-Offline.dll does not expose its direct bootstrap entry.";
        FreeLibrary(localModule);
        return false;
    }
    const auto remoteProcedure = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        remoteModule + (localProcedure - reinterpret_cast<std::uintptr_t>(localModule)));
    bo2z::LauncherBootstrap config{};
    config.size = sizeof(config);
    config.showIndicator = showIndicator ? 1u : 0u;
    if (dataRoot.wstring().size() >= _countof(config.dataRoot)) {
        error = L"The BO2Z-Offline data path is too long.";
        FreeLibrary(localModule);
        return false;
    }
    wcscpy_s(config.dataRoot, dataRoot.c_str());
    FreeLibrary(localModule);

    void* remoteConfig = VirtualAllocEx(process, nullptr, sizeof(config),
                                         MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteConfig) { error = ErrorText(L"Allocating the offline bootstrap data"); return false; }
    SIZE_T written{};
    if (!WriteProcessMemory(process, remoteConfig, &config, sizeof(config), &written) ||
        written != sizeof(config)) {
        error = ErrorText(L"Writing the offline bootstrap data");
        VirtualFreeEx(process, remoteConfig, 0, MEM_RELEASE);
        return false;
    }
    HANDLE thread = CreateRemoteThread(process, nullptr, 0, remoteProcedure, remoteConfig, 0, nullptr);
    if (!thread) {
        error = ErrorText(L"Starting the offline service backend");
        VirtualFreeEx(process, remoteConfig, 0, MEM_RELEASE);
        return false;
    }
    const DWORD wait = WaitForSingleObject(thread, 60000);
    DWORD result{};
    const bool completed = wait == WAIT_OBJECT_0 && GetExitCodeThread(thread, &result) && result == 1;
    if (!completed) {
        error = wait == WAIT_TIMEOUT
            ? L"The offline service backend did not finish initializing."
            : L"The offline module loaded, but its service backend rejected initialization. See BO2Z-Offline.log.";
    }
    CloseHandle(thread);
    VirtualFreeEx(process, remoteConfig, 0, MEM_RELEASE);
    return completed;
}
}
