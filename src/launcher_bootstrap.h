#pragma once
#include <windows.h>

namespace bo2z {
constexpr DWORD kBootstrapMagic = 0x5A324F42; // "BO2Z"
struct LauncherBootstrap {
    DWORD magic{kBootstrapMagic};
    DWORD size{};
    DWORD showIndicator{1};
    wchar_t dataRoot[32768]{};
};
}
