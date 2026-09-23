#include <windows.h>
#include "launcher_bootstrap.h"

extern "C" __declspec(dllexport) DWORD WINAPI OfflineOpBootstrap(void* parameter) {
    if (!parameter) return 0;
    const auto config = *static_cast<const bo2z::LauncherBootstrap*>(parameter);
    if (config.magic != bo2z::kBootstrapMagic || config.size != sizeof(config)) return 0;
    if (HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, config.dataRoot)) {
        SetEvent(event);
        CloseHandle(event);
        return 1;
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) {
    return TRUE;
}
