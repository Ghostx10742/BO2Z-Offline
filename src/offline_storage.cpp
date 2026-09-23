#include "offline_storage.h"
#include "log.h"
#include <windows.h>
#include <atomic>
#include <ctime>
namespace bo2lan::offline {
std::uint64_t g_localUserId=0x0110000100000001ULL;
static unsigned g_profileMode=0;
static std::uint64_t g_chosenProfile=0;
static std::mutex g_accountMutex;
static std::atomic<std::uint64_t> g_sessionAccount{0};
static std::atomic<StorageObserver> g_storageObserver{nullptr};
void SetStorageObserver(StorageObserver observer){g_storageObserver.store(observer);}
static void Observe(const char* event,const std::filesystem::path& path,const Bytes& data,std::uint64_t owner) {
    if(auto observer=g_storageObserver.load())observer(event,path,data,owner?owner:g_localUserId);
}
void ObserveProgressRequest(unsigned service,unsigned operation,const Bytes& data) {
    if(service!=4&&service!=8&&service!=10)return; // stats, public profile, storage only; no auth tickets
    if(auto observer=g_storageObserver.load()) {
        const auto label="service-"+std::to_string(service)+"-operation-"+std::to_string(operation);
        observer("PROGRESS-SERVICE-REQUEST",std::filesystem::path(label),data,g_localUserId);
    }
}
std::uint64_t ProfilePolicyOwner(unsigned mode,std::uint64_t chosen,std::uint64_t native,bool imported,bool legacyExists) {
    const auto valid=[](std::uint64_t id){return (id>>32)==0x01100001ULL&&static_cast<std::uint32_t>(id)!=0;};
    if(mode==1)return valid(native)&&native==chosen&&imported?native:0;
    if(mode==2 || mode==4)return valid(native)?native:0;
    if(mode==3)return valid(chosen)&&legacyExists?chosen:0;
    return 0;
}
bool ConfigureProfilePolicy() {
    Bytes choice;
    if(!ReadFile(DataRoot()/L"profile-choice.bin",choice)||choice.size()!=9)return false;
    g_profileMode=choice[0];std::memcpy(&g_chosenProfile,choice.data()+1,8);
    if(g_profileMode==2)return g_chosenProfile==0;
    if(g_profileMode==1)return ImportedAccountAvailable(DataRoot(),g_chosenProfile);
    if(g_profileMode==3) {
        wchar_t id[32]{};swprintf_s(id,L"%016llX",g_chosenProfile);Bytes stats;
        return ProfilePolicyOwner(3,g_chosenProfile,0,false,
            ReadFile(DataRoot()/L"profiles"/id/L"t6"/L"zmstatsCompressed",stats)&&!stats.empty())!=0;
    }
    return false;
}
void ConfigureIsolatedProfilePolicy() {
    g_profileMode=4;
    g_chosenProfile=0;
    g_localUserId=0;
    Log("offline-store: independent saves in the configured data root under offline-profiles/<SteamID>; no transfer, no legacy fallback");
}
bool SelectPolicyAccount(std::uint64_t nativeAccount) {
    std::lock_guard lock(g_accountMutex);
    if(g_profileMode==4) {
        if(!ProfilePolicyOwner(4,0,nativeAccount,false,false))return false;
        // Never change the owner underneath live service workers.
        if(g_sessionAccount)return g_sessionAccount==nativeAccount;
        wchar_t id[32]{};swprintf_s(id,L"%016llX",nativeAccount);
        const auto root=DataRoot()/L"offline-profiles"/id;
        Bytes marker(8),check;std::memcpy(marker.data(),&nativeAccount,8);
        const auto identity=root/L"owner.bin";
        const auto attributes=GetFileAttributesW(identity.c_str());
        const auto error=GetLastError();
        if(attributes!=INVALID_FILE_ATTRIBUTES) {
            if(!ReadFile(identity,check)||check!=marker){Log("offline-store: owner marker invalid; refusing profile");return false;}
        } else if((error!=ERROR_FILE_NOT_FOUND&&error!=ERROR_PATH_NOT_FOUND)||
                  !WriteFile(identity,marker)||!ReadFile(identity,check)||check!=marker) {
            Log("offline-store: cannot persist account marker; authentication refused, no temporary profile");return false;
        }
        g_localUserId=nativeAccount;
        g_sessionAccount=nativeAccount;
        Log("offline-store: independent account selected=%016llX; native game initializes missing stats and resumes existing saves",nativeAccount);
        return true;
    }
    // Cache ownership after successful selection: never switch profiles during
    // a session. A failed/mismatched import cannot produce a blank replacement.
    const auto owner=ProfilePolicyOwner(g_profileMode,g_chosenProfile,nativeAccount,
        ImportedAccountAvailable(DataRoot(),nativeAccount),g_profileMode==3);
    if(!owner) {
        static LONG reported=0;
        if(!InterlockedCompareExchange(&reported,1,0)) {
            Log("Rank protection: account mismatch/unavailable native=%016llX chosen=%016llX mode=%u; authentication refused",nativeAccount,g_chosenProfile,g_profileMode);
            MessageBoxW(nullptr,L"The selected historical profile does not match this account. No profile will be replaced.",L"BO2Z-Offline - account protection",MB_OK|MB_ICONWARNING);
        }
        return false;
    }
    if(g_localUserId!=owner)Log("offline-store: selected protected profile=%016llX mode=%u",owner,g_profileMode);
    g_localUserId=owner;return true;
}
std::uint64_t StableId(const std::string& text) {
    auto digest=Tiger(text.data(),text.size());std::uint64_t id{};std::memcpy(&id,digest.data(),8);return id;
}
std::filesystem::path ExecutableDirectory() {
    wchar_t path[32768]{};if(!GetModuleFileNameW(nullptr,path,32768))return {};
    return std::filesystem::path(path).parent_path();
}
#ifdef OFFLINE_STORAGE_TEST
static std::filesystem::path testDataRoot;
void SetTestDataRoot(const std::filesystem::path& root){testDataRoot=root;}
#endif
std::filesystem::path DataRoot(){
#ifdef OFFLINE_STORAGE_TEST
    if(!testDataRoot.empty())return testDataRoot;
#endif
    wchar_t configured[32768]{};
    if(GetEnvironmentVariableW(L"BO2Z_OFFLINE_DATA_ROOT",configured,32768)) {
        std::filesystem::path path(configured);
        if(path.is_absolute())return path.lexically_normal();
    }
    return ExecutableDirectory()/L"BO2Z-Offline-data";
}
std::string SafeName(const std::string& name) {
    if(name.empty() || name.size()>128 || name=="." || name==".." || name.back()=='.')return {};
    for(unsigned char c:name)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'))return {};
    auto base=name.substr(0,name.find('.'));
    for(auto& c:base)if(c>='a'&&c<='z')c-=32;
    if(base=="CON"||base=="PRN"||base=="AUX"||base=="NUL"||
       (base.size()==4 && (base.substr(0,3)=="COM"||base.substr(0,3)=="LPT")&&base[3]>='1'&&base[3]<='9'))return {};
    return name;
}
namespace {
bool NoReparse(const std::filesystem::path& path) {
    if(!path.is_absolute())return false;
    std::filesystem::path partial;
    for(const auto& component:path) {
        if(component==L".." || component==L".")return false;
        partial/=component;
        DWORD attributes=GetFileAttributesW(partial.c_str());
        if(attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_REPARSE_POINT))return false;
    }
    return true;
}
std::optional<std::filesystem::path> UserPath(const std::string& name,std::uint64_t owner,std::string title) {
    if(g_profileMode==4 && (!g_sessionAccount || (owner&&owner!=g_sessionAccount)))return std::nullopt;
    if(SafeName(name).empty())return std::nullopt;
    if(title.empty()||title=="t6zm"||title=="t6")title="t6";
    if(SafeName(title).empty())return std::nullopt;
    wchar_t id[32]{};swprintf_s(id,L"%016llX",owner?owner:g_localUserId);
    return DataRoot()/(g_profileMode==4?L"offline-profiles":L"profiles")/id/std::filesystem::path(title)/std::filesystem::path(name);
}
}
bool ReadFile(const std::filesystem::path& path,Bytes& data) {
    data.clear();if(!NoReparse(path))return false;
    HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(h==INVALID_HANDLE_VALUE)return false;
    BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
    bool ok=GetFileInformationByHandle(h,&info) && !(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))&&
        GetFileSizeEx(h,&size)&&size.QuadPart>=0&&size.QuadPart<=16*1024*1024;
    if(ok) {
        data.resize(static_cast<std::size_t>(size.QuadPart));DWORD got{};
        ok=data.empty() || (::ReadFile(h,data.data(),static_cast<DWORD>(data.size()),&got,nullptr) && got==data.size());
    }
    CloseHandle(h);if(!ok)data.clear();return ok;
}
bool WriteFile(const std::filesystem::path& path,const Bytes& data) {
    static std::mutex writeMutex;static std::atomic<unsigned> serial{0};
    std::lock_guard lock(writeMutex);
    if(data.size()>16*1024*1024 || !NoReparse(path))return false;
    std::error_code ec;std::filesystem::create_directories(path.parent_path(),ec);
    if(ec || !NoReparse(path))return false;
    auto temporary=path;temporary+=L".pending-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++serial);
    HANDLE h=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_FLAG_WRITE_THROUGH,nullptr);
    if(h==INVALID_HANDLE_VALUE)return false;
    DWORD written{};bool ok=data.empty() || (::WriteFile(h,data.data(),static_cast<DWORD>(data.size()),&written,nullptr)&&written==data.size());
    ok=ok && FlushFileBuffers(h);CloseHandle(h);
    if(ok) {
        auto backup=path;backup+=L".bak";
        if(!NoReparse(backup))ok=false;
        else if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES)
            ok=ReplaceFileW(path.c_str(),temporary.c_str(),backup.c_str(),0,nullptr,nullptr)!=0;
        else ok=MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH)!=0;
    }
    if(!ok){const auto error=GetLastError();DeleteFileW(temporary.c_str());Log("offline-store: atomic write failed error=%lu; previous save retained",error);}
    return ok;
}
void InitializeLocalIdentity() {
    if(g_profileMode==4)return; // selected from native Steam identity, not machine ID
    Bytes prior;
    const auto identityPath=DataRoot()/L"offline-identity.bin";
    if(ReadFile(identityPath,prior) && prior.size()==8) {
        std::uint64_t value{};std::memcpy(&value,prior.data(),8);
        if((value>>32)==0x01100001ULL && static_cast<std::uint32_t>(value)!=0){g_localUserId=value;return;}
    }
    wchar_t machine[256]{};DWORD size=256;GetComputerNameW(machine,&size);
    wchar_t configured[256]{};auto ini=ExecutableDirectory()/L"BO2Z-Offline.ini";
    GetPrivateProfileStringW(L"Offline",L"PlayerId",machine,configured,256,ini.c_str());
    auto digest=Tiger(configured,wcslen(configured)*sizeof(wchar_t));
    std::uint32_t account{};std::memcpy(&account,digest.data(),4);
    g_localUserId=0x0110000100000000ULL|(account?account:1);
    Bytes data(8);std::memcpy(data.data(),&g_localUserId,8);
    if(!WriteFile(identityPath,data))Log("offline-store: WARNING identity persistence unavailable");
    Log("offline-store: local profile=%016llX",g_localUserId);
}
bool ImportedAccountAvailable(const std::filesystem::path& root,std::uint64_t nativeAccount) {
    if((nativeAccount>>32)!=0x01100001ULL || static_cast<std::uint32_t>(nativeAccount)==0)return false;
    wchar_t id[32]{};swprintf_s(id,L"%016llX",nativeAccount);
    const auto profile=root/L"profiles"/id;
    Bytes marker,stats;
    if(!ReadFile(profile/L"import-account.bin",marker)||marker.size()!=8 ||
        !ReadFile(profile/L"t6"/L"zmstatsCompressed",stats)||stats.empty())return false;
    std::uint64_t owner{};std::memcpy(&owner,marker.data(),8);return owner==nativeAccount;
}
void SelectImportedAccount(std::uint64_t nativeAccount) {
    // Called synchronously BEFORE local authentication issues any services.
    // Existing users without an import keep the exact2.00 storage namespace.
    // Never apply one account's imported rank to another Steam account.
    if(!ImportedAccountAvailable(DataRoot(),nativeAccount))return;
    if(g_localUserId!=nativeAccount) {
        Log("offline-store: using verified imported profile for native account=%016llX; legacy profile retained",nativeAccount);
        g_localUserId=nativeAccount;
    }
}
bool ReadUserFile(const std::string& name,Bytes& data,std::uint64_t owner,const std::string& title) {
    auto path=UserPath(name,owner,title);if(!path)return false;
    if(ReadFile(*path,data)){Observe("offline-read",*path,data,owner);return true;}
    auto backup=*path;backup+=L".bak";
    if(ReadFile(backup,data)){Observe("offline-recovery-read",backup,data,owner);Log("offline-store: recovered readable backup name=%s",name.c_str());return true;}
    // An imported account must never fall back to an unrelated machine-wide
    // backup/profile. In particular, stale legacy zmdatabk0000 can undo an import.
    if(g_profileMode==4 || g_profileMode==1 || g_profileMode==2 || ImportedAccountAvailable(DataRoot(),owner?owner:g_localUserId)){Observe("offline-missing",*path,{},owner);return false;}
    // Read-only compatibility with pre-1.95 saves. Never rename/delete them.
    if((!owner||owner==g_localUserId)&&(title.empty()||title=="t6"||title=="t6zm")) {
        auto legacy=DataRoot()/L"user"/std::filesystem::path(name);
        if(ReadFile(legacy,data)){Observe("offline-legacy-read",legacy,data,owner);return true;}
    }
    Observe("offline-missing",*path,{},owner);
    return false;
}
bool WriteUserFile(const std::string& name,const Bytes& data,std::uint64_t owner,const std::string& title) {
    auto path=UserPath(name,owner,title);if(!path)return false;
    if(g_storageObserver.load()) {
        Bytes prior;if(ReadFile(*path,prior))Observe("offline-before-write",*path,prior,owner);
    }
    const bool result=WriteFile(*path,data);
    Observe(result?"offline-write-ok":"offline-write-failed",*path,data,owner);return result;
}
std::optional<std::filesystem::path> PublisherPath(std::string name) {
    if(SafeName(name).empty())return std::nullopt;
    std::vector<std::string> names{name};
    const auto at=name.find("_english");if(at!=std::string::npos){name.erase(at,8);names.push_back(name);}
    for(const auto& candidateName:names) {
        auto path=DataRoot()/L"pub"/std::filesystem::path(candidateName);
        std::error_code ec;if(NoReparse(path)&&std::filesystem::is_regular_file(path,ec)&&!ec)return path;
    }
    return std::nullopt;
}
void SerializeFileData(ByteBuffer& out, const Bytes& data) { out.WriteBlob(data); }
void SerializeFileInfo(ByteBuffer& out, const std::string& name, std::size_t size,
                       std::uint64_t owner, bool visible) {
    const auto now = static_cast<std::uint32_t>(std::time(nullptr));
    out.WriteU32(static_cast<std::uint32_t>(size));
    out.WriteU64(StableId(name));
    out.WriteU32(now);
    out.WriteU32(now);
    out.WriteBool(visible);
    out.WriteU64(owner);
    out.WriteString(name);
}

}
