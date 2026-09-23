#include "dw_backend.h"
#include "offline_protocol.h"
#include "offline_storage.h"
#include "offline_services.h"
#include "offline_auth.h"
#include "offline_platform.h"

#include "log.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bo2lan {
namespace {
using namespace offline;

constexpr std::uint32_t kAuthAddress = 0x01007F7F;   // 127.127.0.1 in network byte order
constexpr std::uint32_t kLobbyAddress = 0x02007F7F;  // 127.127.0.2
constexpr std::uint32_t kNoFile = 0x3E8;
constexpr std::array<const char*, 4> kStunHosts{
    "cod7-stun.us.demonware.net",
    "cod7-stun.eu.demonware.net",
    "cod7-stun.jp.demonware.net",
    "cod7-stun.au.demonware.net"
};

// Current owned Steam Zombies executable (Steam build 24784288).  The image is
// encrypted on disk and is unpacked after this DXGI proxy's worker begins.
// Every runtime patch below is therefore deferred until Steam is initialized,
// then guarded by both the PE fingerprint and the exact decoded CALL target.
// A changed executable fails closed.
constexpr std::uintptr_t kPreferredImageBase = 0x00400000;
constexpr std::uint32_t kSteamZmTimestamp = 0x6A8332D9;
constexpr std::uint32_t kSteamZmImageSize = 0x03F38000;
constexpr std::uintptr_t kSteamLoggedOn = 0x00861960;
constexpr std::uintptr_t kSteamApiInitialized = 0x02E82B54;
constexpr std::uintptr_t kSteamTicketStateMachine = 0x00862310;
constexpr std::uintptr_t kSteamTicketStateCall = 0x0086FCEF;
constexpr std::uintptr_t kSteamTicketReader = 0x008624E0;
constexpr std::uintptr_t kSteamTicketReaderCall = 0x0086FD86;
constexpr std::uintptr_t kSteamTicketStateValue = 0x00F606FC;
constexpr std::uintptr_t kSteamTicketResultObject = 0x02E82B58;
constexpr std::uintptr_t kCanPlayOnline = 0x0083FDE0;
constexpr std::uint32_t kCanPlayOnlineRequiredMask = 0x000411EE;
constexpr std::uintptr_t kLiveConnectionState = 0x02F4FFF8;
constexpr std::uintptr_t kDemonwareNetworkState = 0x00C880A4;
constexpr std::uintptr_t kLocalSigninState = 0x02F50000;
constexpr std::uintptr_t kCachedCanPlayOnline = 0x02E28F10;
constexpr auto kSteamOnlineCallSites = kLocalPlatformCallSites;
constexpr std::array<std::uintptr_t, 3> kCanPlayOnlineCallSites{
    0x00736F58, // UI script predicate (reuses the two preceding stack arguments)
    0x008400AD, // prerequisite/status formatter missed by v1.92
    0x00840558  // online-play availability update
};

// Native function promises AL only, not a zero-extended EAX. Treating it as
// int made diagnostics interpret stale upper register bits as an online login.
using SteamLoggedOnFn = bool (__cdecl*)();
using LiveSignedInFn = bool (__cdecl*)(int);
using CanPlayOnlineFn = bool (__cdecl*)(int, std::uint32_t*, char);
SteamLoggedOnFn g_realSteamLoggedOn{};
CanPlayOnlineFn g_realCanPlayOnline{};
LiveSignedInFn g_realLiveSignedIn{};
std::atomic<bool> g_backendTransportInstalled{false};
std::uintptr_t g_runtimeImageBase{};
std::atomic<int> g_lastRealSteamState{-1};
std::atomic<bool> g_offlineSteamEligibilityInstalled{false};
std::atomic<std::uint32_t> g_localTicketStatusCalls{0};
std::atomic<std::uint32_t> g_localTicketReadCalls{0};
std::array<std::atomic<std::uint32_t>, kSteamOnlineCallSites.size()> g_steamGateCalls{};
std::array<std::atomic<std::uint32_t>, kCanPlayOnlineCallSites.size()> g_canPlayGateCalls{};
std::atomic<bool> g_loggedType14Probe{false};

std::uint32_t LocalLanAddress() {
    wchar_t configured[64]{};
    const auto ini = ExecutableDirectory() / L"BO2Z-Offline.ini";
    GetPrivateProfileStringW(L"Offline", L"LanAddress", L"", configured, 64, ini.c_str());
    IN_ADDR explicitAddress{};
    if (*configured && InetPtonW(AF_INET, configured, &explicitAddress) == 1)
        return explicitAddress.s_addr;
    ULONG size = 16384;
    Bytes storage(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    ULONG result = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, nullptr, adapters, &size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        storage.resize(size); adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        result = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, nullptr, adapters, &size);
    }
    std::uint32_t best = htonl(INADDR_LOOPBACK); ULONG bestMetric = ULONG_MAX;
    if (result == NO_ERROR) for (auto* a = adapters; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            const auto ip = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr;
            const ULONG metric = a->Ipv4Metric + (a->FirstGatewayAddress ? 0 : 10000);
            if (ip && metric < bestMetric) { best = ip; bestMetric = metric; }
        }
    }
    return best;
}

struct SocketState {
    std::shared_ptr<LocalServer> server;
    bool blocking{true};
};

std::mutex g_socketMutex;
std::unordered_map<SOCKET, SocketState> g_sockets;
std::unordered_map<SOCKET, bool> g_socketBlocking;
std::unordered_map<SOCKET, std::deque<std::pair<sockaddr_in, Bytes>>> g_datagrams;
bool g_runningTests=false;
std::mutex g_wakeMutex;
std::condition_variable g_wake;
std::atomic<std::uint64_t> g_wakeGeneration{0};
void WakeSockets(){++g_wakeGeneration;g_wake.notify_all();}

struct PatchRecord { void* address; Bytes original,replacement; };
std::vector<PatchRecord> g_installedPatches;
bool WriteOwnedPatch(void* address,const void* replacement,std::size_t size) {
    if(!Accessible(address,size))return false;
    PatchRecord record{address,Bytes(size),Bytes(size)};
    std::memcpy(record.original.data(),address,size);std::memcpy(record.replacement.data(),replacement,size);
    // Reserve before changing memory so allocation failure cannot lose the undo record.
    g_installedPatches.reserve(g_installedPatches.size()+1);
    DWORD previous{};if(!VirtualProtect(address,size,PAGE_EXECUTE_READWRITE,&previous))return false;
    std::memcpy(address,replacement,size);
    DWORD ignored{};const bool restored=VirtualProtect(address,size,previous,&ignored)!=0;
    FlushInstructionCache(GetCurrentProcess(),address,size);
    g_installedPatches.push_back(std::move(record));return restored;
}
void RollbackOwnedPatches() {
    for(auto it=g_installedPatches.rbegin();it!=g_installedPatches.rend();++it) {
        if(!Accessible(it->address,it->replacement.size()) || std::memcmp(it->address,it->replacement.data(),it->replacement.size()))continue;
        DWORD previous{};if(!VirtualProtect(it->address,it->original.size(),PAGE_EXECUTE_READWRITE,&previous))continue;
        std::memcpy(it->address,it->original.data(),it->original.size());DWORD ignored{};
        VirtualProtect(it->address,it->original.size(),previous,&ignored);
        FlushInstructionCache(GetCurrentProcess(),it->address,it->original.size());
    }
    g_installedPatches.clear();Log("offline: incomplete hook transaction removed; original code retained");
}

std::uintptr_t RuntimeAddress(std::uintptr_t preferred) {
    return g_runtimeImageBase + (preferred - kPreferredImageBase);
}

const char* LiveStateName(int state) {
    switch (state) {
    case 0: return "ERROR";
    case 1: return "NOT_CONNECTED";
    case 2: return "RESOLVING_AUTH";
    case 3: return "AUTH_SOCKET_READY";
    case 4: return "STEAM_OFFLINE";
    case 5: return "REQUESTING_STEAM_TICKET";
    case 6: return "STEAM_TICKET_READY";
    case 7: return "STEAM_TICKET_ERROR";
    case 8: return "AUTHORIZING_TICKET";
    case 9: return "CONNECTING_DEMONWARE";
    case 10: return "CONNECTED";
    case 11: return "DISCONNECT_WAIT";
    case 12: return "CONNECT_DISABLED";
    default: return "UNKNOWN";
    }
}

template <typename T>
bool ReadRuntime(std::uintptr_t preferred, T& value) {
    const auto address = RuntimeAddress(preferred);
    if (!Accessible(reinterpret_cast<const void*>(address),sizeof(value))) return false;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(value));
    return true;
}

bool __cdecl HookCanPlayOnlineForLocalBackend(int controller, std::uint32_t* flags,
                                               char logChanges) {
    // Observation only. Socket connectivity does NOT imply playlists/stats
    // finished loading. Let the actual callbacks own readiness and the cache.
    const bool nativeResult = g_realCanPlayOnline &&
        g_realCanPlayOnline(controller, flags, logChanges);

    const auto returnAddress = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const auto preferredReturn = returnAddress - g_runtimeImageBase + kPreferredImageBase;
    for (std::size_t i = 0; i < kCanPlayOnlineCallSites.size(); ++i) {
        if (preferredReturn != kCanPlayOnlineCallSites[i] + 5) continue;
        const auto count = ++g_canPlayGateCalls[i];
        if (count == 1) {
            Log("lan-state: CanPlayOnline caller[%u] entered at %08X nativeResult=%d",
                static_cast<unsigned>(i), static_cast<unsigned>(kCanPlayOnlineCallSites[i]),
                nativeResult ? 1 : 0);
        }
        break;
    }

    static std::atomic<std::uint32_t> previous{UINT32_MAX};
    if (controller == 0 && flags && previous.exchange(*flags) != *flags) {
        Log("lan-state: native prerequisites flags=%08X missing=%08X ready=%d "
            "stats=%d ffotd=%d playlists=%d validFFOTD=%d geo=%d",
            *flags, kCanPlayOnlineRequiredMask & ~*flags, nativeResult,
            !!(*flags & 8), !!(*flags & 0x40), !!(*flags & 0x80),
            !!(*flags & 0x100), !!(*flags & 0x1000));
    }
    return nativeResult;
}

int __cdecl HookSteamLoggedOnForLocalBackend() {
    const int real = g_realSteamLoggedOn ? (g_realSteamLoggedOn() != 0) : 0;
    const int previous = g_lastRealSteamState.exchange(real);
    if (previous != real) {
        Log("lan-state: real Steam BLoggedOn=%d (AL); local connectivity uses installed backend", real);
    }

    const auto returnAddress = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const auto preferredReturn = returnAddress - g_runtimeImageBase + kPreferredImageBase;
    for (std::size_t i = 0; i < kSteamOnlineCallSites.size(); ++i) {
        if (preferredReturn != kSteamOnlineCallSites[i] + 5) continue;
        const auto count = ++g_steamGateCalls[i];
        if (count <= 4 || (count & (count - 1)) == 0) {
            Log("lan-state: local platform gate at %08X real=%d count=%u",
                static_cast<unsigned>(kSteamOnlineCallSites[i]), real, count);
        }
        break;
    }
    std::uint8_t initialized{};
    return LocalPlatformAvailable(g_backendTransportInstalled.load(),
        ReadRuntime(kSteamApiInitialized, initialized) && initialized != 0);
}

bool __cdecl ObserveLiveSignedIn(int controller) {
    const bool result = g_realLiveSignedIn && g_realLiveSignedIn(controller);
    // Observes the actual predicate used by mainlobby.lua. Never substitutes
    // success, writes sign-in state, or opens/closes a menu.
    static std::array<std::atomic<int>,4> last{{-1,-1,-1,-1}};
    if(controller >= 0 && controller < 4 && last[controller].exchange(result ? 1 : 0) != (result ? 1 : 0)) {
        int nativeSignin{}; std::uint8_t fetched{};
        ReadRuntime(kLocalSigninState + controller * 0x1070, nativeSignin);
        ReadRuntime(kCachedCanPlayOnline, fetched);
        Log("offline-ui: IsSignedInToLive controller=%d result=%d nativeSignin=%d cachedFetchDone=%u",
            controller,result,nativeSignin,fetched);
    }
    return result;
}

int __cdecl HookLocalSteamTicketReady(void* nativeObject) {
    std::uint64_t nativeAccount{};
    //0086F060 reads the native local account at02F51040. This is available
    //after Steam sign-in, including Steam Offline Mode, not a cloud query.
    ReadRuntime(0x02F51040,nativeAccount);
    if(!SelectPolicyAccount(nativeAccount))return -1;
    const auto fn=reinterpret_cast<PrepareNativePayload>(RuntimeAddress(0x009F4B70));
    if(!PrepareLocalAuthentication(nativeObject,fn)) {
        Log("offline-auth: native key preparation refused; not reporting ready");return -1;
    }
    const int ready=0;
    auto* state=reinterpret_cast<void*>(RuntimeAddress(kSteamTicketStateValue));
    if(!Accessible(state,sizeof(ready),true))return -1;
    std::memcpy(state,&ready,sizeof(ready));
    ++g_localTicketStatusCalls;
    return 1;
}

int __cdecl HookReadLocalSteamTicket(void* destination, std::uint32_t capacity,
                                     std::uint32_t* written) {
    if(!CopyLocalTicket(destination,capacity,written)) {
        Log("offline-auth: ticket read refused: unprepared or changed native key");return 0;
    }
    ++g_localTicketReadCalls;return 1;
}

bool PatchRelativeCall(std::uintptr_t preferredCall, std::uintptr_t preferredExpectedTarget,
                       void* replacement, const char* label) {
    auto* call = reinterpret_cast<std::uint8_t*>(RuntimeAddress(preferredCall));
    if (call[0] != 0xE8) {
        Log("lan-state: %s patch refused at %08X: opcode=%02X expected=E8", label,
            static_cast<unsigned>(preferredCall), call[0]);
        return false;
    }
    std::int32_t oldDisplacement{};
    std::memcpy(&oldDisplacement, call + 1, sizeof(oldDisplacement));
    const auto actualTarget = reinterpret_cast<std::uintptr_t>(call + 5) + oldDisplacement;
    const auto expectedTarget = RuntimeAddress(preferredExpectedTarget);
    if (actualTarget != expectedTarget) {
        Log("lan-state: %s patch refused at %08X: target=%p expected=%p", label,
            static_cast<unsigned>(preferredCall), reinterpret_cast<void*>(actualTarget),
            reinterpret_cast<void*>(expectedTarget));
        return false;
    }

    const auto destination = reinterpret_cast<std::uintptr_t>(replacement);
    const auto delta = static_cast<std::int64_t>(destination) -
        static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(call + 5));
    if (delta < INT32_MIN || delta > INT32_MAX) {
        Log("lan-state: %s patch refused: replacement out of rel32 range", label);
        return false;
    }
    const auto displacement = static_cast<std::int32_t>(delta);
    if(!WriteOwnedPatch(call+1,&displacement,sizeof(displacement)))return false;
    Log("lan-state: %s redirected at %08X", label, static_cast<unsigned>(preferredCall));
    return true;
}

bool PrepareOfflineSteamEligibility() {
    const auto module = GetModuleHandleW(nullptr);
    if (!module) return false;
    g_runtimeImageBase = reinterpret_cast<std::uintptr_t>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
        reinterpret_cast<const std::uint8_t*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        nt->FileHeader.TimeDateStamp != kSteamZmTimestamp ||
        nt->OptionalHeader.SizeOfImage != kSteamZmImageSize) {
        Log("lan-state: offline Steam eligibility unsupported image timestamp=%08X size=%08X",
            nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
        return false;
    }

    return true;
}

enum class OfflineEligibilityResult { Waiting, Installed, Fatal };

OfflineEligibilityResult TryInstallOfflineSteamEligibility() {
    // The Steam executable is protected. Seeing all required original CALL
    // opcodes at once is our signal that this narrow part of the image has
    // finished unpacking.  Never diagnose a partially decoded image as a
    // version mismatch and never write a partial set of redirects.
    for (const auto preferredCall : kSteamOnlineCallSites) {
        std::uint8_t opcode{};
        if (!ReadRuntime(preferredCall, opcode) || opcode != 0xE8)
            return OfflineEligibilityResult::Waiting;
    }
    for (const auto preferredCall : kCanPlayOnlineCallSites) {
        std::uint8_t opcode{};
        if (!ReadRuntime(preferredCall, opcode) || opcode != 0xE8)
            return OfflineEligibilityResult::Waiting;
    }
    std::uint8_t ticketStateOpcode{}, ticketReaderOpcode{};
    if (!ReadRuntime(kSteamTicketStateCall, ticketStateOpcode) ||
        !ReadRuntime(kSteamTicketReaderCall, ticketReaderOpcode) ||
        ticketStateOpcode != 0xE8 || ticketReaderOpcode != 0xE8) {
        return OfflineEligibilityResult::Waiting;
    }
    std::uint8_t uiPredicateOpcode{};
    if(!ReadRuntime(0x00736BCF,uiPredicateOpcode) || uiPredicateOpcode!=0xE8)
        return OfflineEligibilityResult::Waiting;

    for (const auto preferredCall : kSteamOnlineCallSites) {
        const auto* call = reinterpret_cast<const std::uint8_t*>(RuntimeAddress(preferredCall));
        std::int32_t displacement{};
        std::memcpy(&displacement, call + 1, sizeof(displacement));
        const auto target = reinterpret_cast<std::uintptr_t>(call + 5) + displacement;
        if (target != RuntimeAddress(kSteamLoggedOn)) {
            Log("lan-state: decoded Steam gate mismatch at %08X target=%p expected=%p",
                static_cast<unsigned>(preferredCall), reinterpret_cast<void*>(target),
                reinterpret_cast<void*>(RuntimeAddress(kSteamLoggedOn)));
            return OfflineEligibilityResult::Fatal;
        }
    }
    for (const auto preferredCall : kCanPlayOnlineCallSites) {
        const auto* call = reinterpret_cast<const std::uint8_t*>(RuntimeAddress(preferredCall));
        std::int32_t displacement{};
        std::memcpy(&displacement, call + 1, sizeof(displacement));
        const auto target = reinterpret_cast<std::uintptr_t>(call + 5) + displacement;
        if (target != RuntimeAddress(kCanPlayOnline)) {
            Log("lan-state: decoded CanPlayOnline seam mismatch at %08X target=%p expected=%p",
                static_cast<unsigned>(preferredCall), reinterpret_cast<void*>(target),
                reinterpret_cast<void*>(RuntimeAddress(kCanPlayOnline)));
            return OfflineEligibilityResult::Fatal;
        }
    }
    const std::array<std::pair<std::uintptr_t, std::uintptr_t>, 3> ticketCalls{{
        {kSteamTicketStateCall, kSteamTicketStateMachine},
        {kSteamTicketReaderCall, kSteamTicketReader},
        {0x00736BCF, 0x0086F030} // native UI predicate observation
    }};
    for (const auto& [preferredCall, preferredTarget] : ticketCalls) {
        const auto* call = reinterpret_cast<const std::uint8_t*>(RuntimeAddress(preferredCall));
        std::int32_t displacement{};
        std::memcpy(&displacement, call + 1, sizeof(displacement));
        const auto target = reinterpret_cast<std::uintptr_t>(call + 5) + displacement;
        if (target != RuntimeAddress(preferredTarget)) {
            Log("lan-state: decoded local-ticket seam mismatch at %08X target=%p expected=%p",
                static_cast<unsigned>(preferredCall), reinterpret_cast<void*>(target),
                reinterpret_cast<void*>(RuntimeAddress(preferredTarget)));
            return OfflineEligibilityResult::Fatal;
        }
    }

    // Verify the native preparation routine, not just the old ticket call sites.
    static constexpr std::uint8_t prepareSignature[]{0x55,0x8b,0xec,0x53,0x56,0x8b,0xf1,0x57,0x8d,0x9e,0x50,0x01,0x00,0x00,0x53};
    const auto* prepare=reinterpret_cast<const void*>(RuntimeAddress(0x009f4b70));
    if(!Accessible(prepare,sizeof(prepareSignature)) || std::memcmp(prepare,prepareSignature,sizeof(prepareSignature))) {
        Log("offline-auth: native preparation signature mismatch");return OfflineEligibilityResult::Fatal;
    }
    g_realSteamLoggedOn = reinterpret_cast<SteamLoggedOnFn>(RuntimeAddress(kSteamLoggedOn));
    g_realCanPlayOnline = reinterpret_cast<CanPlayOnlineFn>(RuntimeAddress(kCanPlayOnline));
    g_realLiveSignedIn = reinterpret_cast<LiveSignedInFn>(RuntimeAddress(0x0086F030));
    bool ok = true;
    for (std::size_t i = 0; i < kSteamOnlineCallSites.size(); ++i) {
        char label[64]{};
        sprintf_s(label, "Steam-offline eligibility[%u]", static_cast<unsigned>(i));
        ok &= PatchRelativeCall(kSteamOnlineCallSites[i], kSteamLoggedOn,
                                reinterpret_cast<void*>(&HookSteamLoggedOnForLocalBackend), label);
    }
    ok &= PatchRelativeCall(kSteamTicketStateCall, kSteamTicketStateMachine,
                            reinterpret_cast<void*>(&HookLocalSteamTicketReady),
                            "local-ticket state");
    ok &= PatchRelativeCall(kSteamTicketReaderCall, kSteamTicketReader,
                            reinterpret_cast<void*>(&HookReadLocalSteamTicket),
                            "local-ticket payload");
    ok &= PatchRelativeCall(0x00736BCF, 0x0086F030,
                            reinterpret_cast<void*>(&ObserveLiveSignedIn),
                            "native IsSignedInToLive observation");
    for (std::size_t i = 0; i < kCanPlayOnlineCallSites.size(); ++i) {
        char label[64]{};
        sprintf_s(label, "local-backend CanPlayOnline[%u]", static_cast<unsigned>(i));
        ok &= PatchRelativeCall(kCanPlayOnlineCallSites[i], kCanPlayOnline,
                                reinterpret_cast<void*>(&HookCanPlayOnlineForLocalBackend), label);
    }
    if (!ok) return OfflineEligibilityResult::Fatal;
    g_offlineSteamEligibilityInstalled = true;
    return OfflineEligibilityResult::Installed;
}

DWORD WINAPI OfflineEligibilityThread(void*) {
    const auto started = GetTickCount64();
    Log("lan-state: waiting for protected Steam/LIVE code to finish unpacking");
    for (;;) {
        std::uint8_t steamInitialized{};
        if (ReadRuntime(kSteamApiInitialized, steamInitialized) && steamInitialized) {
            const auto result = TryInstallOfflineSteamEligibility();
            if (result == OfflineEligibilityResult::Installed) {
                Log("lan-state: Steam Offline Mode compatibility enabled after %llu ms "
                    "(six scoped platform checks, native UI sign-in observer, three online-readiness observers)",
                    GetTickCount64() - started);
                return 0;
            }
            if (result == OfflineEligibilityResult::Fatal) {
                g_backendTransportInstalled=false;
                RollbackOwnedPatches();
                Log("lan-state: Steam Offline Mode compatibility FAILED after code unpack");
                return 1;
            }
        }
        if (GetTickCount64() - started >= 120000) {
            g_backendTransportInstalled=false;
            RollbackOwnedPatches();
            Log("lan-state: Steam Offline Mode compatibility timed out waiting for decoded code");
            return 2;
        }
        Sleep(10);
    }
}

bool ArmOfflineSteamEligibility() {
    if (!PrepareOfflineSteamEligibility()) return false;
    HANDLE thread = CreateThread(nullptr, 0, &OfflineEligibilityThread, nullptr, 0, nullptr);
    if (!thread) {
        Log("lan-state: failed to arm deferred Steam eligibility patch error=%lu", GetLastError());
        return false;
    }
    CloseHandle(thread);
    Log("lan-state: deferred Steam eligibility patch armed");
    return true;
}

DWORD WINAPI DiagnosticsThread(void*) {
    int lastLive = INT32_MIN;
    int lastDw = INT32_MIN;
    int lastSignin = INT32_MIN;
    int lastTicket = INT32_MIN;
    std::uint32_t lastClientFlags = UINT32_MAX;
    std::uint8_t lastSteamInitialized = 0xFF;
    std::uint8_t lastCanPlayCached = 0xFF;
    ULONGLONG lastSnapshot{};
    for (;;) {
        int live{}, dw{}, signin{}, ticket{};
        std::uint32_t clientFlags{};
        std::uint8_t steamInitialized{}, canPlayCached{};
        if (ReadRuntime(0x02F4FFF8, live) && live != lastLive) {
            Log("lan-state: LIVE connection=%d (%s)", live, LiveStateName(live));
            lastLive = live;
        }
        if (ReadRuntime(0x00C880A4, dw) && dw != lastDw) {
            Log("lan-state: Demonware network status=%d", dw);
            lastDw = dw;
        }
        if (ReadRuntime(0x02F50000, signin) && signin != lastSignin) {
            Log("lan-state: local client sign-in state=%d", signin);
            lastSignin = signin;
        }
        if (ReadRuntime(0x00F606FC, ticket) && ticket != lastTicket) {
            Log("lan-state: Steam ticket callback state=%d", ticket);
            lastTicket = ticket;
        }
        if (ReadRuntime(0x02F4E99C, clientFlags) && clientFlags != lastClientFlags) {
            Log("lan-state: local client LIVE flags=0x%08X", clientFlags);
            lastClientFlags = clientFlags;
        }
        if (ReadRuntime(kSteamApiInitialized, steamInitialized) &&
            steamInitialized != lastSteamInitialized) {
            Log("lan-state: Steam API initialized=%u", static_cast<unsigned>(steamInitialized));
            lastSteamInitialized = steamInitialized;
        }
        if (ReadRuntime(0x02E28F10, canPlayCached) && canPlayCached != lastCanPlayCached) {
            Log("lan-state: BO2 cached CanPlayOnline=%u", static_cast<unsigned>(canPlayCached));
            lastCanPlayCached = canPlayCached;
        }
        const auto tick=GetTickCount64();
        if(tick-lastSnapshot>=5000 || (GetAsyncKeyState(VK_F10)&1)) {
            lastSnapshot=tick;
            std::vector<std::pair<SOCKET,std::shared_ptr<LocalServer>>> sockets;
            {std::lock_guard lock(g_socketMutex);for(const auto& entry:g_sockets)sockets.push_back({entry.first,entry.second.server});}
            for(const auto& entry:sockets)entry.second->Dump(entry.first);
            if(g_offlineSteamEligibilityInstalled.load()) {
                std::uint8_t ffotd{},valid{},playlist{},wad{},parsed{};
                ReadRuntime(0x02f1da24,ffotd);ReadRuntime(0x02f1da38,valid);
                ReadRuntime(0x02f1da2a,playlist);ReadRuntime(0x02f1dc50,wad);ReadRuntime(0x02f1da39,parsed);
                Log("lan-snapshot: prerequisites ffotdRetrieved=%u ffotdStageReady=%u playlistParsed=%u wadDownloaded=%u wadProcessed=%u",
                    ffotd,valid,playlist,wad,parsed);
                // 005A0940 owns this instance; observe it without calling the
                // lazy constructor or mutating native network readiness.
                std::uintptr_t network{};
                if(ReadRuntime(0x03335D30,network) && network) {
                    const auto field=[network](std::size_t offset) {
                        std::uint32_t value=UINT32_MAX; SIZE_T copied{};
                        if(network>UINTPTR_MAX-offset || !ReadProcessMemory(GetCurrentProcess(),
                            reinterpret_cast<const void*>(network+offset),&value,sizeof(value),&copied) || copied!=sizeof(value))return UINT32_MAX;
                        return value;
                    };
                    Log("lan-snapshot: native-network status=%d hosts=%u dnsCompleted=%u resolved=%u selected=%u dnsActive=%d stunActive=%d natActive=%d",
                        static_cast<int>(field(0xC4)),field(0x18),field(0x4788),field(0x4780),field(0x4784),
                        field(0xD0)!=0,field(0xC8)!=0,field(0xCC)!=0);
                }
            }
        }
        Sleep(100);
    }
}

void StartDiagnostics() {
    HANDLE thread = CreateThread(nullptr, 0, &DiagnosticsThread, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
        Log("lan-state: transition diagnostics armed (100 ms, changes only)");
    } else {
        Log("lan-state: failed to start transition diagnostics error=%lu", GetLastError());
    }
}

std::uint32_t JenkinsOneAtATime(const char* text) {
    std::uint32_t hash{};
    for (const auto* p = reinterpret_cast<const unsigned char*>(text); p && *p; ++p) {
        hash += *p;
        hash += hash << 10;
        hash ^= hash >> 6;
    }
    hash += hash << 3;
    hash ^= hash >> 11;
    hash += hash << 15;
    return hash;
}

std::uint32_t StunAddressForName(const char* name) {
    if (!name) return 0;
    for (const auto* host : kStunHosts) {
        if (_stricmp(name, host) == 0) return JenkinsOneAtATime(host);
    }
    return 0;
}

bool IsStun(std::uint32_t address) {
    return std::any_of(kStunHosts.begin(), kStunHosts.end(), [address](const char* host) {
        return JenkinsOneAtATime(host) == address;
    });
}
bool IsFake(SOCKET socket) {
    std::lock_guard lock(g_socketMutex);
    return g_sockets.find(socket) != g_sockets.end();
}
std::shared_ptr<LocalServer> FindServer(SOCKET socket) {
    std::lock_guard lock(g_socketMutex);
    const auto it = g_sockets.find(socket);
    return it == g_sockets.end() ? nullptr : it->second.server;
}

hostent* WSAAPI HookGetHostByName(const char* name) {
    if (!name) return ::gethostbyname(name);
    std::uint32_t address{};
    if (_stricmp(name, "ops2-pc-auth.prod.demonware.net") == 0) address = kAuthAddress;
    else if (_stricmp(name, "ops2-pc-lobby.prod.demonware.net") == 0) address = kLobbyAddress;
    else address = StunAddressForName(name);
    if (!address) {
        const std::string host=name;
        const std::string suffix=".demonware.net";
        if(host.size()>=suffix.size() && _stricmp(host.c_str()+host.size()-suffix.size(),suffix.c_str())==0) {
            Log("lan-dw: unsupported DW host refused locally: %s",name);
            WSASetLastError(WSAHOST_NOT_FOUND);return nullptr;
        }
        return ::gethostbyname(name);
    }

    thread_local in_addr ip{};
    thread_local char* list[2]{};
    thread_local hostent result{};
    ip.s_addr = address;
    list[0] = reinterpret_cast<char*>(&ip);
    list[1] = nullptr;
    result.h_name = const_cast<char*>(name);
    result.h_aliases = nullptr;
    result.h_addrtype = AF_INET;
    result.h_length = sizeof(ip);
    result.h_addr_list = list;
    Log("lan-dw: redirected DNS %s address=%08X", name, static_cast<unsigned>(address));
    return &result;
}

int WSAAPI HookConnect(SOCKET socket, const sockaddr* address, int length) {
    if (address && length >= static_cast<int>(sizeof(sockaddr_in)) && address->sa_family == AF_INET) {
        const auto ip = reinterpret_cast<const sockaddr_in*>(address)->sin_addr.s_addr;
        std::shared_ptr<LocalServer> server;
        if (ip == kAuthAddress) server = std::make_shared<LocalServer>(ServerKind::Auth,!g_runningTests);
        else if (ip == kLobbyAddress) server = std::make_shared<LocalServer>(ServerKind::Lobby,!g_runningTests);
        if (server) {
            std::lock_guard lock(g_socketMutex);
            const auto mode=g_socketBlocking.find(socket);
            g_sockets[socket] = {std::move(server), mode==g_socketBlocking.end()?true:mode->second};
            Log("lan-dw: linked TCP socket %llu", static_cast<unsigned long long>(socket));
            return 0;
        }
    }
    return ::connect(socket, address, length);
}

int WSAAPI HookSend(SOCKET socket, const char* buffer, int length, int flags) {
    if (auto server = FindServer(socket)) return server->Send(buffer, length);
    return ::send(socket, buffer, length, flags);
}

int WSAAPI HookRecv(SOCKET socket, char* buffer, int length, int flags) {
    std::shared_ptr<LocalServer> server;bool blocking=false;
    {std::lock_guard lock(g_socketMutex);const auto it=g_sockets.find(socket);
        if(it!=g_sockets.end()){server=it->second.server;blocking=it->second.blocking;}}
    if (server) return server->Receive(buffer, length, flags, blocking);
    return ::recv(socket, buffer, length, flags);
}

int WSAAPI HookCloseSocket(SOCKET socket) {
    bool wasLocal = false;
    auto server=FindServer(socket);
    if(server){server->Dump(socket);server->Close();}
    {
        std::lock_guard lock(g_socketMutex);
        wasLocal = g_sockets.erase(socket) != 0;
        g_datagrams.erase(socket);
        g_socketBlocking.erase(socket);
    }
    if (wasLocal) Log("lan-dw: local service socket %llu closed",
                      static_cast<unsigned long long>(socket));
    return ::closesocket(socket);
}

int WSAAPI HookIoctlSocket(SOCKET socket, long command, u_long* value) {
    if (command == FIONREAD && value) {
        if (auto server = FindServer(socket)) { *value = static_cast<u_long>(server->Available()); return 0; }
        std::lock_guard lock(g_socketMutex);
        const auto it = g_datagrams.find(socket);
        if (it != g_datagrams.end() && !it->second.empty()) {
            *value = static_cast<u_long>(it->second.front().second.size()); return 0;
        }
    }
    if (command == FIONBIO && value) {
        const int result=::ioctlsocket(socket,command,value);
        if(result==SOCKET_ERROR)return result;
        std::lock_guard lock(g_socketMutex);
        g_socketBlocking[socket]=*value==0;
        const auto it = g_sockets.find(socket);
        if (it != g_sockets.end()) it->second.blocking = *value == 0;
        return 0;
    }
    return ::ioctlsocket(socket, command, value);
}

int WSAAPI HookGetSockOpt(SOCKET socket, int level, int option, char* value, int* length) {
    if (IsFake(socket) && level == SOL_SOCKET && option == SO_ERROR && value && length && *length >= 4) {
        auto server = FindServer(socket);
        const int noError = server && server->Failed() ? WSAECONNRESET : 0;
        std::memcpy(value, &noError, 4);
        *length = 4;
        return 0;
    }
    return ::getsockopt(socket, level, option, value, length);
}

int WSAAPI HookGetSockName(SOCKET socket, sockaddr* name, int* length) {
    if (IsFake(socket) && name && length && *length >= static_cast<int>(sizeof(sockaddr_in))) {
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        local.sin_port = htons(3074);
        std::memcpy(name, &local, sizeof(local));
        *length = sizeof(local);
        return 0;
    }
    return ::getsockname(socket, name, length);
}

bool RemoveSocket(fd_set* set, SOCKET socket) {
    if (!set) return false;
    for (u_int i = 0; i < set->fd_count; ++i) {
        if (set->fd_array[i] == socket) {
            for (u_int j = i + 1; j < set->fd_count; ++j) set->fd_array[j - 1] = set->fd_array[j];
            --set->fd_count;
            return true;
        }
    }
    return false;
}

int WSAAPI HookSelect(int nfds,fd_set* readSet,fd_set* writeSet,fd_set* exceptSet,const timeval* timeout) {
    if(timeout && (timeout->tv_sec<0 || timeout->tv_usec<0 || timeout->tv_usec>=1000000)){WSASetLastError(WSAEINVAL);return SOCKET_ERROR;}
    fd_set wantRead{},wantWrite{},wantExcept{};
    if(readSet)wantRead=*readSet;if(writeSet)wantWrite=*writeSet;if(exceptSet)wantExcept=*exceptSet;
    const auto started=std::chrono::steady_clock::now();
    const auto budget=timeout ? std::chrono::microseconds(static_cast<std::int64_t>(timeout->tv_sec)*1000000+timeout->tv_usec) : std::chrono::microseconds::max();
    for(;;){
        const auto epoch=g_wakeGeneration.load();
        fd_set reads=wantRead,writes=wantWrite,errors=wantExcept;
        std::vector<std::pair<SOCKET,std::shared_ptr<LocalServer>>> local;
        {std::lock_guard lock(g_socketMutex);for(const auto& e:g_sockets)local.push_back({e.first,e.second.server});}
        bool virtualRequested=false;
        for(const auto& e:local){
            virtualRequested|=FD_ISSET(e.first,&wantRead)||FD_ISSET(e.first,&wantWrite)||FD_ISSET(e.first,&wantExcept);
            RemoveSocket(&reads,e.first);RemoveSocket(&writes,e.first);RemoveSocket(&errors,e.first);
        }
        bool queuedDatagram=false;
        {std::lock_guard lock(g_socketMutex);for(const auto& e:g_datagrams)queuedDatagram|=!e.second.empty()&&FD_ISSET(e.first,&wantRead);}
        // Preserve the OS path exactly when this call has no local descriptors.
        if(!virtualRequested&&!queuedDatagram)return ::select(nfds,readSet,writeSet,exceptSet,timeout);
        timeval zero{};
        if(reads.fd_count||writes.fd_count||errors.fd_count){
            if(::select(nfds,reads.fd_count?&reads:nullptr,writes.fd_count?&writes:nullptr,errors.fd_count?&errors:nullptr,&zero)==SOCKET_ERROR)return SOCKET_ERROR;
        }
        for(const auto& e:local){
            if(FD_ISSET(e.first,&wantRead)&&e.second->Readable())FD_SET(e.first,&reads);
            if(FD_ISSET(e.first,&wantWrite))FD_SET(e.first,&writes);
            if(FD_ISSET(e.first,&wantExcept)&&e.second->Failed())FD_SET(e.first,&errors);
        }
        {std::lock_guard lock(g_socketMutex);for(const auto& e:g_datagrams)
            if(!e.second.empty()&&FD_ISSET(e.first,&wantRead))FD_SET(e.first,&reads);}
        const int ready=reads.fd_count+writes.fd_count+errors.fd_count;
        const auto elapsed=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started);
        if(ready || (timeout&&elapsed>=budget)){
            if(readSet)*readSet=reads;if(writeSet)*writeSet=writes;if(exceptSet)*exceptSet=errors;return ready;
        }
        // Service replies wake immediately. Mixed OS sockets are checked at most
        // 5ms later, without busy-spinning or ignoring the caller's deadline.
        auto wait=std::chrono::microseconds(5000);
        if(timeout)wait=std::min(wait,budget-elapsed);
        std::unique_lock lock(g_wakeMutex);
        g_wake.wait_for(lock,wait,[&]{return g_wakeGeneration.load()!=epoch;});
    }
}

int WSAAPI HookSendTo(SOCKET socket, const char* buffer, int length, int flags,
                      const sockaddr* to, int toLength) {
    if (to && toLength >= static_cast<int>(sizeof(sockaddr_in)) && to->sa_family == AF_INET &&
        IsStun(reinterpret_cast<const sockaddr_in*>(to)->sin_addr.s_addr) && length >= 3) {
        const auto type = static_cast<std::uint8_t>(buffer[0]);
        const auto serverAddress = reinterpret_cast<const sockaddr_in*>(to)->sin_addr.s_addr;
        Bytes response;
        if (type == 30 || type == 20) {
            response.push_back(type == 30 ? 31 : 21);
            response.push_back(2);
            response.push_back(0);
            sockaddr_in bound{}; int boundSize = sizeof(bound);
            if (::getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &boundSize) != 0) {
                WSASetLastError(WSAEADDRNOTAVAIL); return SOCKET_ERROR;
            }
            const std::uint32_t localAddress = bound.sin_addr.s_addr ? bound.sin_addr.s_addr : LocalLanAddress();
            const std::uint16_t port = ntohs(bound.sin_port);
            if (!port) { Log("lan-dw: STUN request on unbound socket"); WSASetLastError(WSAEADDRNOTAVAIL); return SOCKET_ERROR; }
            AppendRaw(response, localAddress);
            AppendRaw(response, port);
            if (type == 20) {
                // NAT discovery compares replies from the four configured STUN
                // endpoints. Preserve the unique address of the queried server
                // instead of collapsing every region to one synthetic host.
                AppendRaw(response, serverAddress);
                AppendRaw(response, ntohs(reinterpret_cast<const sockaddr_in*>(to)->sin_port));
            }
            const auto responseSize = response.size();
            std::lock_guard lock(g_socketMutex);
            if(g_datagrams[socket].size()>=64){WSASetLastError(WSAENOBUFS);return SOCKET_ERROR;}
            g_datagrams[socket].push_back({*reinterpret_cast<const sockaddr_in*>(to), std::move(response)});
            WakeSockets();
            Log("lan-dw: STUN request type=%u server=%08X bytes=%u LAN=%08X port=%u",
                type, static_cast<unsigned>(serverAddress), static_cast<unsigned>(responseSize), localAddress, port);
        } else if (type == 14) {
            // Static reconstruction of the current client's bdNAT packet path
            // shows type 14 is a 29-byte outbound QoS/discovery announcement.
            // The receive dispatcher only consumes types 11, 12, 13, and 21;
            // synthesizing a type-14/15 reply cannot advance the state machine.
            if (!g_loggedType14Probe.exchange(true)) {
                Log("lan-dw: NAT discovery announcement type=14 bytes=%d accepted (one-way)",
                    length);
            }
        } else {
            Log("lan-dw: unsupported STUN request type=%u bytes=%d", type, length);
        }
        return length;
    }
    return ::sendto(socket, buffer, length, flags, to, toLength);
}

int WSAAPI HookRecvFrom(SOCKET socket, char* buffer, int length, int flags,
                        sockaddr* from, int* fromLength) {
    {
        std::lock_guard lock(g_socketMutex);
        auto it = g_datagrams.find(socket);
        if (it != g_datagrams.end() && !it->second.empty()) {
            if(!buffer || length<0 || (from && (!fromLength || *fromLength<static_cast<int>(sizeof(sockaddr_in))))) {
                WSASetLastError(WSAEFAULT);return SOCKET_ERROR;
            }
            if(flags & ~MSG_PEEK){WSASetLastError(WSAEOPNOTSUPP);return SOCKET_ERROR;}
            const auto& packet = it->second.front();
            const auto count = std::min<int>(length, static_cast<int>(packet.second.size()));
            std::memcpy(buffer, packet.second.data(), count);
            if (from && fromLength && *fromLength >= static_cast<int>(sizeof(sockaddr_in))) {
                std::memcpy(from, &packet.first, sizeof(sockaddr_in));
                *fromLength = sizeof(sockaddr_in);
            }
            const bool truncated = static_cast<std::size_t>(length)<packet.second.size();
            if(!(flags & MSG_PEEK))it->second.pop_front();
            if(truncated){WSASetLastError(WSAEMSGSIZE);return SOCKET_ERROR;}
            return count;
        }
    }
    return ::recvfrom(socket, buffer, length, flags, from, fromLength);
}

bool PatchImport(HMODULE module, const char* importedDll, const char* function,
                 std::uint16_t targetOrdinal, void* replacement) {
    const auto base = reinterpret_cast<std::uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return false;
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; descriptor->Name; ++descriptor) {
        const auto* dllName = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(dllName, importedDll) != 0) continue;
        auto* names = descriptor->OriginalFirstThunk
            ? reinterpret_cast<IMAGE_THUNK_DATA32*>(base + descriptor->OriginalFirstThunk)
            : reinterpret_cast<IMAGE_THUNK_DATA32*>(base + descriptor->FirstThunk);
        auto* addresses = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addresses) {
            bool matches = false;
            if (IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)) {
                matches = targetOrdinal != 0 && IMAGE_ORDINAL32(names->u1.Ordinal) == targetOrdinal;
            } else {
                const auto* import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(
                    base + names->u1.AddressOfData);
                matches = std::strcmp(reinterpret_cast<const char*>(import->Name), function) == 0;
            }
            if (!matches) continue;
            const auto value=reinterpret_cast<std::uint32_t>(replacement);
            return WriteOwnedPatch(&addresses->u1.Function,&value,sizeof(value));
        }
    }
    return false;
}

bool HookWinsock(const char* name, void* replacement) {
    struct Entry { const char* name; std::uint16_t ordinal; };
    static constexpr Entry ordinals[]{
        {"closesocket", 3}, {"connect", 4}, {"getsockname", 6},
        {"getsockopt", 7}, {"recv", 16}, {"recvfrom", 17},
        {"select", 18}, {"send", 19}, {"sendto", 20},
        {"ioctlsocket", 12}, {"gethostbyname", 52}
    };
    std::uint16_t ordinal{};
    for (const auto& entry : ordinals) {
        if (std::strcmp(entry.name, name) == 0) { ordinal = entry.ordinal; break; }
    }
    const auto exe = GetModuleHandleW(nullptr);
    const bool a = PatchImport(exe, "WSOCK32.dll", name, ordinal, replacement);
    const bool b = PatchImport(exe, "WS2_32.dll", name, ordinal, replacement);
    Log("lan-dw: hook %-14s wsock32=%d ws2_32=%d", name, a ? 1 : 0, b ? 1 : 0);
    return a || b;
}

} // namespace

bool StartBackend() {
    RequireNativeAuthentication();
    InitializeLocalIdentity();
    SetServiceWakeCallback(&WakeSockets);
    Log("offline: production traffic only; protocol regression tests run separately");
    std::error_code ec;
    std::filesystem::create_directories(DataRoot() / L"pub", ec);
    std::filesystem::create_directories(DataRoot() / L"user", ec);
    InitCrypto();

    // Steam Offline Mode makes SteamUser::BLoggedOn false. Stock T6 checks
    // that predicate before it starts Demonware and throughout the LIVE state
    // machine AND the separate UI sign-in predicate. Adapt these six verified
    // sites together; adapting just connection startup caused a popup cycle.
    // Steam initialization/ownership and BO2's menu/party/match flow stay native.
    if(!PrepareOfflineSteamEligibility())return false;
    bool ok = true;
    ok &= HookWinsock("gethostbyname", reinterpret_cast<void*>(&HookGetHostByName));
    ok &= HookWinsock("connect", reinterpret_cast<void*>(&HookConnect));
    ok &= HookWinsock("send", reinterpret_cast<void*>(&HookSend));
    ok &= HookWinsock("recv", reinterpret_cast<void*>(&HookRecv));
    ok &= HookWinsock("closesocket", reinterpret_cast<void*>(&HookCloseSocket));
    ok &= HookWinsock("ioctlsocket", reinterpret_cast<void*>(&HookIoctlSocket));
    ok &= HookWinsock("getsockopt", reinterpret_cast<void*>(&HookGetSockOpt));
    ok &= HookWinsock("getsockname", reinterpret_cast<void*>(&HookGetSockName));
    ok &= HookWinsock("select", reinterpret_cast<void*>(&HookSelect));
    ok &= HookWinsock("sendto", reinterpret_cast<void*>(&HookSendTo));
    ok &= HookWinsock("recvfrom", reinterpret_cast<void*>(&HookRecvFrom));
    g_backendTransportInstalled=ok;
    if(!ok || !ArmOfflineSteamEligibility()){
        g_backendTransportInstalled=false;RollbackOwnedPatches();return false;
    }
    StartDiagnostics();
    Log("lan-dw: local offline backend %s; BO2 menu/party state remains native",
        ok ? "enabled" : "FAILED mandatory hooks");
    return ok;
}

bool RunBackendSelfTests() {
    struct TestScope{TestScope(){g_runningTests=true;}~TestScope(){g_runningTests=false;}} testScope;
    InitCrypto();

    static constexpr std::array<std::uint8_t, 24> kEmptyTiger{
        0x32,0x93,0xAC,0x63,0x0C,0x13,0xF0,0x24,
        0x5F,0x92,0xBB,0xB1,0x76,0x6E,0x16,0x16,
        0x7A,0x4E,0x58,0x49,0x2D,0xDE,0x73,0xF3
    };
    const std::uint8_t emptyInput{};
    if (Tiger(&emptyInput, 0) != kEmptyTiger) return false;

    std::array<std::uint32_t, kStunHosts.size()> stunAddresses{};
    for (std::size_t i = 0; i < kStunHosts.size(); ++i) {
        stunAddresses[i] = StunAddressForName(kStunHosts[i]);
        if (!stunAddresses[i] || stunAddresses[i] == kAuthAddress ||
            stunAddresses[i] == kLobbyAddress || !IsStun(stunAddresses[i])) return false;
        for (std::size_t j = 0; j < i; ++j) {
            if (stunAddresses[i] == stunAddresses[j]) return false;
        }
    }

    std::array<std::uint8_t, 24> key{};
    for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(i + 1);
    const std::uint32_t seed = 0x12345678;
    const auto iv = Tiger(&seed, sizeof(seed));
    Bytes clear(16);
    for (std::size_t i = 0; i < clear.size(); ++i) clear[i] = static_cast<std::uint8_t>(i * 7);
    const auto encrypted = TripleDes(clear, iv, key, true);
    if (encrypted.size() != clear.size() || TripleDes(encrypted, iv, key, false) != clear) return false;

    ByteBuffer typed;
    typed.WriteU32(700);
    typed.WriteString("t6");
    ByteBuffer typedRead(typed.Data());
    std::uint32_t result{};
    std::string game;
    if (!typedRead.ReadU32(result) || result != 700 || !typedRead.ReadString(game) || game != "t6")
        return false;

    std::array<std::uint8_t, 64> steamTicket{};
    std::copy(key.begin(), key.end(), steamTicket.begin() + 32);
    BitBuffer authBody;
    authBody.Types(false);
    authBody.WriteBool(false);
    authBody.Types(true);
    authBody.WriteU32(seed);
    authBody.WriteU32(18397);
    authBody.WriteU32(static_cast<std::uint32_t>(steamTicket.size()));
    authBody.WriteBytes(steamTicket.data(), steamTicket.size());
    Bytes authFrame;
    const auto authSize = static_cast<std::int32_t>(authBody.Data().size() + 2);
    AppendRaw(authFrame, authSize);
    authFrame.push_back(0);
    authFrame.push_back(28);
    authFrame.insert(authFrame.end(), authBody.Data().begin(), authBody.Data().end());
    LocalServer auth(ServerKind::Auth);
    if (auth.Send(reinterpret_cast<const char*>(authFrame.data()), static_cast<int>(authFrame.size())) <= 0)
        return false;
    std::array<char, 1024> response{};
    const int responseSize = auth.Receive(response.data(), static_cast<int>(response.size()));
    if (responseSize < 7 || static_cast<std::uint8_t>(response[5]) != 29) return false;

    // Match T6's native bdAuth response decoder: the leading flag, result,
    // seed, encrypted 128-byte bdAuthTicket, and 128-byte LSG ticket are one
    // continuous untyped bit stream.  This specifically guards against the
    // type-tag regression that made the stock client remain on Connecting.
    std::int32_t responseFrameSize{};
    std::memcpy(&responseFrameSize, response.data(), sizeof(responseFrameSize));
    if (responseFrameSize != responseSize - 4 || response[4] != 0) return false;
    Bytes responseBody(reinterpret_cast<std::uint8_t*>(response.data() + 6),
                       reinterpret_cast<std::uint8_t*>(response.data() + responseSize));
    BitBuffer authReply(std::move(responseBody));
    authReply.Types(false);
    bool replyEnvelope{};
    std::uint32_t replyResult{}, replySeed{};
    std::array<std::uint8_t, 128> encryptedAuthTicket{};
    std::array<std::uint8_t, 128> replyLsgTicket{};
    std::array<std::uint8_t,24> expectedSessionKey{};
    if (!authReply.ReadBool(replyEnvelope) || replyEnvelope ||
        !authReply.ReadU32(replyResult) || replyResult != 700 ||
        !authReply.ReadU32(replySeed) || replySeed != seed ||
        !authReply.ReadBytes(encryptedAuthTicket.data(), encryptedAuthTicket.size()) ||
        !authReply.ReadBytes(replyLsgTicket.data(), replyLsgTicket.size()))
        return false;

    auto decodedTicket=TripleDes(Bytes(encryptedAuthTicket.begin(),encryptedAuthTicket.end()),iv,key,false);
    if(decodedTicket.size()!=128)return false;
    std::uint32_t magic{},ticketTitle{};std::memcpy(&magic,decodedTicket.data(),4);std::memcpy(&ticketTitle,decodedTicket.data()+5,4);
    if(magic!=0xEFBDADDE || ticketTitle!=18397)return false;
    std::copy_n(decodedTicket.begin()+97,24,expectedSessionKey.begin());
    if(!std::equal(expectedSessionKey.begin(),expectedSessionKey.end(),replyLsgTicket.begin()))return false;

    // Reconstructed from current native 009E3740: 8-byte handshake followed
    // by a separately framed bit-buffer hello. Exercise every stream split.
    Bytes handshake;
    AppendRaw(handshake, std::uint32_t{200});
    AppendRaw(handshake, std::uint32_t{65536});
    BitBuffer hello;
    hello.Types(false); hello.WriteBool(true); hello.Types(true);
    hello.WriteU32(18397); hello.WriteU32(seed);
    hello.WriteBytes(replyLsgTicket.data(), replyLsgTicket.size());
    Bytes helloFrame;
    AppendRaw(helloFrame, static_cast<std::uint32_t>(hello.Data().size() + 2));
    helloFrame.push_back(0); helloFrame.push_back(7);
    helloFrame.insert(helloFrame.end(), hello.Data().begin(), hello.Data().end());
    Bytes start = handshake;
    start.insert(start.end(), helloFrame.begin(), helloFrame.end());
    for (std::size_t split = 0; split <= start.size(); ++split) {
        LocalServer fragmented(ServerKind::Lobby);
        if (fragmented.Send(reinterpret_cast<const char*>(start.data()), static_cast<int>(split)) < 0 ||
            fragmented.Send(reinterpret_cast<const char*>(start.data()+split), static_cast<int>(start.size()-split)) < 0 ||
            !fragmented.Ready()) return false;
    }
    LocalServer lobby(ServerKind::Lobby);
    if (lobby.Send(reinterpret_cast<const char*>(handshake.data()), 4) != 4 || lobby.Readable()) return false;
    if (lobby.Send(reinterpret_cast<const char*>(handshake.data()+4), 4) != 4 || !lobby.Readable()) return false;
    const int lobbySize = lobby.Receive(response.data(), static_cast<int>(response.size()), MSG_PEEK);
    if (lobbySize != 15 || static_cast<std::uint8_t>(response[5]) != 4 || !lobby.Readable()) return false;
    std::array<char,1024> consumed{};
    if (lobby.Receive(consumed.data(), 1024) != lobbySize ||
        std::memcmp(consumed.data(), response.data(), lobbySize) || lobby.Readable()) return false;
    if (lobby.Send(reinterpret_cast<const char*>(helloFrame.data()), static_cast<int>(helloFrame.size())) < 0 ||
        !lobby.Ready()) return false;

    const auto requestFrame = [&](std::uint8_t service, std::uint8_t op, const Bytes& payload = Bytes{}) {
        ByteBuffer opData; opData.WriteU8(op); opData.WriteRaw(payload.data(), payload.size());
        Bytes plain; AppendRaw(plain, std::uint32_t{0xDEADBEEF}); plain.push_back(service);
        plain.insert(plain.end(),opData.Data().begin(),opData.Data().end());
        plain.push_back(0);
        plain.resize((plain.size()+7)&~std::size_t(7),static_cast<std::uint8_t>(seed));
        std::array<std::uint8_t,4> mac{};
        if(!ClientTaskMac(plain.data()+5,plain.size()-5,expectedSessionKey,mac))return Bytes{};
        std::copy(mac.begin(),mac.end(),plain.begin());
        const auto cipher = TripleDes(plain, iv, expectedSessionKey, true);
        Bytes frame; AppendRaw(frame,static_cast<std::uint32_t>(cipher.size()+5)); frame.push_back(1);
        AppendRaw(frame,seed); frame.insert(frame.end(),cipher.begin(),cipher.end()); return frame;
    };
    const auto replyBody = [&](LocalServer& server, std::uint8_t op, std::uint32_t count, ByteBuffer& objects) {
        std::array<char,8192> wire{}; int n=server.Receive(wire.data(),static_cast<int>(wire.size()));
        if(n<17 || wire[4]!=1)return false;
        std::uint32_t length{},replyIv{};std::memcpy(&length,wire.data(),4);std::memcpy(&replyIv,wire.data()+5,4);
        if(length!=static_cast<std::uint32_t>(n-4))return false;
        Bytes cipher(reinterpret_cast<std::uint8_t*>(wire.data()+9),reinterpret_cast<std::uint8_t*>(wire.data()+n));
        auto plain=TripleDes(cipher,Tiger(&replyIv,4),expectedSessionKey,false);
        if(plain.size()<5 || plain[4]!=1)return false;
        ByteBuffer body(Bytes(plain.begin()+5,plain.end()));
        std::uint64_t tx{};std::uint32_t error{},total{},returned{};std::uint8_t actualOp{};
        if(!body.ReadU64(tx)||!tx||!body.ReadU32(error)||error||!body.ReadU8(actualOp)||actualOp!=op||
            !body.ReadU32(returned)||returned!=count)return false;
        if(count && (!body.ReadU32(total)||total!=count))return false;
        objects=ByteBuffer(body.Rest());return true;
    };
    const auto timeFrame=requestFrame(12,6);
    for(const auto byte:timeFrame)if(lobby.Send(reinterpret_cast<const char*>(&byte),1)!=1)return false;
    ByteBuffer object;std::uint32_t now{};
    if(!replyBody(lobby,6,1,object)||!object.ReadU32(now)||now<1700000000)return false;
    const auto dmlFrame=requestFrame(27,3);
    lobby.Send(reinterpret_cast<const char*>(dmlFrame.data()),static_cast<int>(dmlFrame.size()));
    if(!replyBody(lobby,3,1,object))return false;
    std::string text;float latitude{},longitude{};
    for(int i=0;i<4;++i)if(!object.ReadString(text))return false;
    if(!object.ReadFloat(latitude)||!object.ReadFloat(longitude))return false;
    for(int i=0;i<4;++i)if(!object.ReadU32(now))return false;

    // Matchmaking contracts: native 009E2290/23A0/24B0, not Xbox variants.
    ByteBuffer sessionFields;
    sessionFields.WriteBlob(Bytes(37,0));sessionFields.WriteU32(1000);sessionFields.WriteU32(4);
    sessionFields.WriteU64(g_localUserId);sessionFields.WriteBlob(Bytes(16,0));
    for(unsigned i=0;i<8;++i)sessionFields.WriteScalar<std::int32_t>(7,0);
    sessionFields.WriteFloat(0);
    for(unsigned i=0;i<4;++i)sessionFields.WriteScalar<std::int32_t>(7,0);
    const auto create=requestFrame(21,1,sessionFields.Data());
    lobby.Send(reinterpret_cast<const char*>(create.data()),static_cast<int>(create.size()));
    Bytes sessionId;
    if(!replyBody(lobby,1,1,object)||!object.ReadBlob(sessionId)||sessionId.size()!=8)return false;
    ByteBuffer update;update.WriteBlob(sessionId);update.WriteRaw(sessionFields.Data().data(),sessionFields.Data().size());
    const auto updateFrame=requestFrame(21,2,update.Data());
    lobby.Send(reinterpret_cast<const char*>(updateFrame.data()),static_cast<int>(updateFrame.size()));
    if(!replyBody(lobby,2,0,object))return false;
    ByteBuffer remove;remove.WriteBlob(sessionId);
    const auto removeFrame=requestFrame(21,3,remove.Data());
    lobby.Send(reinterpret_cast<const char*>(removeFrame.data()),static_cast<int>(removeFrame.size()));
    if(!replyBody(lobby,3,0,object))return false;
    // A normal 200-byte frame after the handshake is NOT a second marker.
    LocalServer normalLength(ServerKind::Auth);
    Bytes sized(204);std::uint32_t twoHundred=200;std::memcpy(sized.data(),&twoHundred,4);sized[5]=99;
    normalLength.Send(reinterpret_cast<const char*>(sized.data()),static_cast<int>(sized.size()));
    if(!normalLength.Failed())return false; // Unsupported complete frame is rejected, not mistaken for a handshake.

    // A fresh connection cannot inherit partial bytes or encryption keys.
    LocalServer fresh(ServerKind::Lobby);
    fresh.Send(reinterpret_cast<const char*>(handshake.data()),3);
    if(fresh.Readable()||fresh.Ready())return false;
    LocalServer replacement(ServerKind::Lobby);
    replacement.Send(reinterpret_cast<const char*>(start.data()),static_cast<int>(start.size()));
    if(!replacement.Ready())return false;

    // Winsock readiness must never add descriptors the caller did not request.
    WSADATA wsa{};if(WSAStartup(MAKEWORD(2,2),&wsa))return false;
    const SOCKET a=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    const SOCKET b=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=kLobbyAddress;address.sin_port=htons(3074);
    bool socketsOk=a!=INVALID_SOCKET&&b!=INVALID_SOCKET;
    if(socketsOk){
        socketsOk=HookConnect(a,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0 &&
            HookConnect(b,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0;
        HookSend(a,reinterpret_cast<const char*>(handshake.data()),8,0);
        fd_set reads{},writes{};FD_SET(b,&reads);FD_SET(b,&writes);timeval timeout{};
        socketsOk=socketsOk && HookSelect(0,&reads,&writes,nullptr,&timeout)==1 && reads.fd_count==0 &&
            writes.fd_count==1 && FD_ISSET(b,&writes) && !FD_ISSET(a,&writes);
        u_long available{};socketsOk=socketsOk&&HookIoctlSocket(a,FIONREAD,&available)==0&&available==15;
        // An idle local connection must honor a finite read deadline.
        FD_ZERO(&reads);FD_SET(b,&reads);timeval shortWait{0,20000};
        const auto waitStart=std::chrono::steady_clock::now();
        const auto idleResult=HookSelect(0,&reads,nullptr,nullptr,&shortWait);
        const auto waited=std::chrono::steady_clock::now()-waitStart;
        socketsOk=socketsOk&&idleResult==0&&reads.fd_count==0&&
            waited>=std::chrono::milliseconds(15)&&waited<std::chrono::seconds(2);
    }
    if(a!=INVALID_SOCKET)HookCloseSocket(a);if(b!=INVALID_SOCKET)HookCloseSocket(b);WSACleanup();
    if(!socketsOk)return false;
    return true;
}

} // namespace bo2lan
