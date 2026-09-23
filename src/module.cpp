#include "dw_backend.h"
#include "offline_storage.h"
#include "log.h"
#include "launcher_bootstrap.h"
#include <windows.h>

extern "C" __declspec(dllexport) void __cdecl OfflineOpStart();

namespace {
LONG g_started{};

bool StartOffline() {
    if(InterlockedCompareExchange(&g_started,1,0))return false;
    wchar_t path[32768]{};GetModuleFileNameW(nullptr,path,32768);
    const wchar_t* name=wcsrchr(path,L'\\');name=name?name+1:path;
    if(_wcsicmp(name,L"t6zm.exe")){InterlockedExchange(&g_started,3);return false;}
    Log_Init();
    Log("BO2Z-Offline 2.9.4 Steam-launched direct bootstrap; launcher-owned status indicator");
    bo2lan::offline::ConfigureIsolatedProfilePolicy();
    const bool started=bo2lan::StartBackend();
    if(!started)Log("Offline backend initialization failed; no success handshake was sent");
    InterlockedExchange(&g_started,started?2:3);
    return started;
}

}

// Explicit initialization outside loader lock, in Zombies only. No VR code,
// OpenXR loader, rendering hooks, or Steam DLL replacements in this module.
extern "C" __declspec(dllexport) void __cdecl OfflineOpStart() {
    StartOffline();
}
extern "C" __declspec(dllexport) DWORD WINAPI OfflineOpBootstrap(void* parameter) {
    if(!parameter)return 0;
    const auto config=*static_cast<const bo2z::LauncherBootstrap*>(parameter);
    if(config.magic!=bo2z::kBootstrapMagic || config.size!=sizeof(config) || !config.dataRoot[0])return 0;
    SetEnvironmentVariableW(L"BO2Z_OFFLINE_LAUNCHER",L"1");
    SetEnvironmentVariableW(L"BO2Z_OFFLINE_INDICATOR",config.showIndicator?L"1":L"0");
    SetEnvironmentVariableW(L"BO2Z_OFFLINE_DATA_ROOT",config.dataRoot);
    return StartOffline()?1u:0u;
}
BOOL APIENTRY DllMain(HMODULE mod,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
    }
    return TRUE;
}
