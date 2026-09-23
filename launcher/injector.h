#pragma once
#include <windows.h>
#include <filesystem>
#include <cstdint>
#include <string>

namespace bo2z::launcher {
bool InjectLibrary(HANDLE process, DWORD processId, const std::filesystem::path& library,
                   std::wstring& error, std::uintptr_t* remoteModule = nullptr);
bool BootstrapGame(HANDLE process, DWORD processId, const std::filesystem::path& library,
                   const std::filesystem::path& dataRoot, bool showIndicator,
                   std::wstring& error);
}
