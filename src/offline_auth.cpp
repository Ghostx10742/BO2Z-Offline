#include "offline_auth.h"
#include "log.h"
#include <windows.h>
#include <bcrypt.h>
#include <limits>
namespace bo2lan::offline {
namespace {
std::mutex authMutex;
void* nativeOwner{};
std::uint64_t generation{};
std::array<std::uint8_t,24> preparedKey{},issuedKey{};
std::uint32_t issuedTitle{},issuedSeed{};
bool prepared{},issued{};
bool runtimeRequired{};
}
bool Accessible(const void* p,std::size_t n,bool write) {
    auto at=reinterpret_cast<std::uintptr_t>(p);
    if(!at || n>std::numeric_limits<std::uintptr_t>::max()-at)return false;
    const auto end=at+n;
    while(at<end) {
        MEMORY_BASIC_INFORMATION m{};
        if(!VirtualQuery(reinterpret_cast<void*>(at),&m,sizeof(m)) || m.State!=MEM_COMMIT ||
            (m.Protect&(PAGE_GUARD|PAGE_NOACCESS)))return false;
        const DWORD mode=m.Protect&0xff;
        const bool writable=mode==PAGE_READWRITE || mode==PAGE_WRITECOPY ||
            mode==PAGE_EXECUTE_READWRITE || mode==PAGE_EXECUTE_WRITECOPY;
        if(write ? !writable : !(writable || mode==PAGE_READONLY || mode==PAGE_EXECUTE_READ))return false;
        const auto next=reinterpret_cast<std::uintptr_t>(m.BaseAddress)+m.RegionSize;
        if(next<=at)return false;
        at=next;
    }
    return true;
}
bool PrepareLocalAuthentication(void* object,PrepareNativePayload fn) {
    std::lock_guard lock(authMutex);
    prepared=issued=false;nativeOwner=nullptr;++generation;
    if(!fn || !Accessible(object,0x2b8,true))return false;
    // Only the already-native ticket request calls this. This does not start
    // authentication, a menu, a party or a match of its own accord.
    std::uint32_t size{};
    void* payload=fn(object,"Offline Player",&size);
    auto* expected=static_cast<std::uint8_t*>(object)+0x150;
    if(payload!=expected || size!=88 || !Accessible(payload,size)) {
        Log("offline-auth: native payload contract failed generation=%llu",generation);return false;
    }
    std::memcpy(preparedKey.data(),payload,24);
    if(std::all_of(preparedKey.begin(),preparedKey.end(),[](auto b){return b==0;}))return false;
    nativeOwner=object;prepared=true;
    Log("offline-auth: native request key prepared generation=%llu bytes=24",generation);
    return true;
}
bool CopyLocalTicket(void* destination,std::size_t capacity,std::uint32_t* written) {
    std::lock_guard lock(authMutex);
    if(!prepared || capacity<128 || !Accessible(destination,128,true) || !Accessible(written,4,true) ||
        !Accessible(nativeOwner,0x168) ||
        std::memcmp(static_cast<std::uint8_t*>(nativeOwner)+0x150,preparedKey.data(),24))return false;
    std::array<std::uint8_t,128> ticket{};
    constexpr char marker[]="BO2-LOCAL-OFFLINE";
    std::memcpy(ticket.data(),marker,sizeof(marker));
    std::memcpy(ticket.data()+32,preparedKey.data(),24);
    std::memcpy(destination,ticket.data(),ticket.size());*written=128;
    Log("offline-auth: local ticket shares native key generation=%llu",generation);
    return true;
}
bool IssueLocalSession(const std::array<std::uint8_t,24>& requestKey,std::uint32_t title,
    std::uint32_t seed,std::array<std::uint8_t,24>& key) {
    std::lock_guard lock(authMutex);
    if(title!=18397 || (runtimeRequired&&!nativeOwner) || (nativeOwner && (!prepared || preparedKey!=requestKey)))return false;
    if(BCryptGenRandom(nullptr,key.data(),static_cast<ULONG>(key.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)return false;
    issuedKey=key;issuedTitle=title;issuedSeed=seed;issued=true;
    return true;
}
bool ValidateLocalHello(std::uint32_t title,std::uint32_t seed,const std::array<std::uint8_t,128>& ticket) {
    std::lock_guard lock(authMutex);
    if(!issued || title!=issuedTitle || seed!=issuedSeed || !std::equal(issuedKey.begin(),issuedKey.end(),ticket.begin()))return false;
    if(nativeOwner) {
        if(!Accessible(nativeOwner,0x168))return false;
        auto* p=static_cast<std::uint8_t*>(nativeOwner);
        std::uint32_t nativeTitle{},nativeSeed{};
        std::memcpy(&nativeTitle,p+0x24,4);std::memcpy(&nativeSeed,p+0x28,4);
        if(nativeTitle!=title || nativeSeed!=seed || std::memcmp(p+0xac,issuedKey.data(),24)) {
            Log("offline-auth: native acceptance FAILED generation=%llu title=%u expected=%u",generation,nativeTitle,title);return false;
        }
        Log("offline-auth: native title, seed, session key and lobby hello agree generation=%llu",generation);
    }
    return true;
}
std::uint64_t AuthenticationGeneration(){std::lock_guard lock(authMutex);return generation;}
void ResetLocalAuthentication(){std::lock_guard lock(authMutex);nativeOwner=nullptr;prepared=issued=false;preparedKey.fill(0);issuedKey.fill(0);}
void RequireNativeAuthentication(){std::lock_guard lock(authMutex);runtimeRequired=true;}
}
