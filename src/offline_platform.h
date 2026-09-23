#pragma once
#include <array>
#include <cstdint>

namespace bo2lan::offline {
// Current Steam 24784288 only. These adapt Internet connectivity, NOT Steam
// ownership, native local sign-in, content checks, or DW download readiness.
inline constexpr std::array<std::uintptr_t, 6> kLocalPlatformCallSites{
    0x0086F9E1, 0x0086FC15, 0x0086FC51, 0x00870079, 0x00870A85,
    0x0086F030 // IsSignedInToLive: previously left dependent on Internet Steam.
};
inline constexpr bool LocalPlatformAvailable(bool backendInstalled, bool steamInitialized) {
    return backendInstalled && steamInitialized;
}
}
