#include <windows.h>
#include <filesystem>
#include <iostream>
int main() {
    wchar_t file[32768]{};GetModuleFileNameW(nullptr,file,32768);
    auto path=std::filesystem::path(file).parent_path()/L"BO2Z-Offline.dll";
    auto mod=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!mod){std::cerr<<"Module load failed "<<GetLastError();return 1;}
    auto start=reinterpret_cast<void(__cdecl*)()>(GetProcAddress(mod,"OfflineOpStart"));
    if(!start){std::cerr<<"Entry point unavailable";return 1;}
    if(!GetProcAddress(mod,"OfflineOpBootstrap")){std::cerr<<"Direct bootstrap unavailable";return 1;}
    start(); // Must do nothing at all outside t6zm.exe, including no dialogs.
    if(GetModuleHandleW(L"openxr_loader.dll")||GetModuleHandleW(L"openvr_api.dll"))return 1;
    FreeLibrary(mod);
    std::cout<<"Independent module loads, exposes stable entry, and ignores non-Zombies host\n";
    return 0;
}
