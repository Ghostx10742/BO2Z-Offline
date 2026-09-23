#pragma once
#include "offline_protocol.h"
namespace bo2lan::offline {
using PrepareNativePayload = void* (__thiscall*)(void*, const char*, std::uint32_t*);
bool Accessible(const void*,std::size_t,bool write=false);
bool PrepareLocalAuthentication(void* nativeObject,PrepareNativePayload);
bool CopyLocalTicket(void*,std::size_t,std::uint32_t*);
bool IssueLocalSession(const std::array<std::uint8_t,24>& requestKey,
    std::uint32_t title,std::uint32_t seed,std::array<std::uint8_t,24>& sessionKey);
bool ValidateLocalHello(std::uint32_t title,std::uint32_t seed,
    const std::array<std::uint8_t,128>& ticket);
std::uint64_t AuthenticationGeneration();
void ResetLocalAuthentication();
void RequireNativeAuthentication();
}
