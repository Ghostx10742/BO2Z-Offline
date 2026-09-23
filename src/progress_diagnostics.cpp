#include "progress_diagnostics.h"
#include "offline_storage.h"
#include "rank_profile.h"
#include "log.h"
#include <windows.h>
#include <zlib.h>
#include <atomic>
#include <deque>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <array>
#include <cwctype>

namespace bo2lan::diagnostics {
namespace fs=std::filesystem;
using offline::Bytes;
namespace {
constexpr std::size_t kFileLimit=2*1024*1024,kTotalLimit=64*1024*1024,kQueueLimit=4*1024*1024;
std::atomic<bool> active{false};
std::atomic<unsigned> dropped{0};
struct Event{std::string type;fs::path path;Bytes bytes;std::uint64_t owner{},tick{};};
std::mutex queueMutex;
std::deque<Event> queue;
std::size_t queuedBytes{};
fs::path output,game;
std::ofstream report;
std::set<std::string> blobs,seenFiles,schemas;
std::size_t savedBytes{};
unsigned events{},fileCount{};
bool backend{};
bool passiveOnline{};
std::string Hex(std::uint64_t n){std::ostringstream s;s<<std::hex<<std::uppercase<<n;return s.str();}
std::string Clean(std::string s){for(auto& c:s)if(static_cast<unsigned char>(c)<32)c=' ';return s;}
void Line(const std::string& text) {
    if(events==20000){report<<GetTickCount64()<<"\tREPORT-LINE-LIMIT-REACHED\n";report.flush();++events;return;}
    if(events++>20000)return;
    report<<GetTickCount64()<<'\t'<<text<<'\n';report.flush();
}
bool NoLinks(const fs::path& path) {
    if(!path.is_absolute())return false;fs::path part;
    for(const auto& c:path){if(c==L"..")return false;part/=c;auto a=GetFileAttributesW(part.c_str());if(a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_REPARSE_POINT))return false;}
    return true;
}
bool ReadDisk(const fs::path& path,Bytes& bytes) {
    bytes.clear();if(!NoLinks(path))return false;
    auto h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(h==INVALID_HANDLE_VALUE)return false;
    LARGE_INTEGER size{};BY_HANDLE_FILE_INFORMATION info{};
    bool ok=GetFileInformationByHandle(h,&info)&&!(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&GetFileSizeEx(h,&size)&&size.QuadPart>=0&&size.QuadPart<=kFileLimit;
    if(ok){bytes.resize(static_cast<std::size_t>(size.QuadPart));DWORD n{};ok=bytes.empty()||(::ReadFile(h,bytes.data(),static_cast<DWORD>(bytes.size()),&n,nullptr)&&n==bytes.size());}
    CloseHandle(h);if(!ok)bytes.clear();return ok;
}
std::string SaveBlob(const Bytes& b) {
    if(b.empty())return "empty";
    auto hash=rank::Sha256(b);if(hash.empty())return "hash-failed";
    if(blobs.count(hash))return hash;
    if(savedBytes+b.size()>kTotalLimit)return "CAPTURE-BUDGET-REACHED";
    auto path=output/L"blobs"/(hash+".bin");
    // Unique session folder; never overwrite a prior capture or a save.
    auto h=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE)return "capture-write-failed";
    DWORD written{};bool ok=::WriteFile(h,b.data(),static_cast<DWORD>(b.size()),&written,nullptr)&&written==b.size()&&FlushFileBuffers(h);CloseHandle(h);
    if(!ok)return "capture-write-failed";
    blobs.insert(hash);savedBytes+=b.size();return hash;
}
void Inspect(const std::string& source,const fs::path& path,const Bytes& bytes,std::uint64_t owner=0,std::uint64_t eventTick=0) {
    const auto hash=SaveBlob(bytes);
    Bytes ddl;std::string format,reason;const bool valid=DecodeProfile(bytes,ddl,format,reason);
    std::ostringstream s;s<<source<<"\tpath="<<Clean(path.u8string())<<"\towner="<<Hex(owner)<<"\teventTick="<<eventTick
        <<"\tbytes="<<bytes.size()<<"\tblob="<<hash<<"\tvalidDDL="<<valid<<"\tformat="<<format<<"\treason="<<Clean(reason);
    if(valid)s<<"\tddlBlob="<<SaveBlob(ddl)<<"\tddlBytes="<<ddl.size();
    Line(s.str());
}
void Observe(const char* event,const fs::path& path,const Bytes& bytes,std::uint64_t owner) noexcept {
    if(!active.load())return;
    try {
        std::lock_guard lock(queueMutex);
        if(bytes.size()>kFileLimit||queue.size()>=256||queuedBytes+bytes.size()>kQueueLimit){++dropped;return;}
        queue.push_back({event,path,bytes,owner,GetTickCount64()});queuedBytes+=bytes.size();
    }catch(...){++dropped;}
}
void Drain() {
    std::deque<Event> batch;
    {std::lock_guard lock(queueMutex);batch.swap(queue);queuedBytes=0;}
    for(const auto& e:batch)Inspect(e.type,e.path,e.bytes,e.owner,e.tick);
    if(const auto lost=dropped.exchange(0))Line("EVENTS-DROPPED\tcount="+std::to_string(lost));
}
void Scan(const fs::path& root,const char* label) {
    if(!NoLinks(root)){Line(std::string(label)+"\tlinked-root-skipped");return;}
    std::error_code ec;if(!fs::exists(root,ec)){Line(std::string(label)+"\troot-missing\t"+Clean(root.u8string()));return;}
    Line(std::string(label)+"\tscan-root\t"+Clean(root.u8string()));
    auto it=fs::recursive_directory_iterator(root,fs::directory_options::skip_permission_denied,ec);
    unsigned visited=0;
    for(;it!=fs::recursive_directory_iterator()&&!ec;it.increment(ec)) {
        if(++visited>3000){Line(std::string(label)+"\tSCAN-LIMIT");break;}
        const auto p=it->path();auto attr=GetFileAttributesW(p.c_str());
        if(attr==INVALID_FILE_ATTRIBUTES)continue;
        if(attr&FILE_ATTRIBUTE_REPARSE_POINT){it.disable_recursion_pending();Line("linked-entry-skipped\t"+Clean(p.u8string()));continue;}
        if(it->is_directory(ec)) {
            auto n=p.filename().wstring();if(it.depth()>5||n==L"import-backups"||n==L"pub"||n==L"diagnostics")it.disable_recursion_pending();
            continue;
        }
        if(!Candidate(p))continue;
        Bytes b;if(!ReadDisk(p,b)){Line(std::string(label)+"\tread-failed-or-oversize\t"+Clean(p.u8string()));continue;}
        const auto key=p.u8string()+":"+rank::Sha256(b);if(!seenFiles.insert(key).second)continue;
        ++fileCount;Inspect(label,p,b);
    }
    if(ec)Line(std::string(label)+"\tscan-error="+std::to_string(ec.value()));
}
fs::path SteamRoot() {
    wchar_t path[32768]{};DWORD bytes=sizeof path;
    if(RegGetValueW(HKEY_CURRENT_USER,L"Software\\Valve\\Steam",L"SteamPath",RRF_RT_REG_SZ,nullptr,path,&bytes)==ERROR_SUCCESS)return fs::path(path);
    auto root=game.parent_path().parent_path().parent_path();
    if(fs::exists(root/L"userdata"))return root;
    return {};
}
void ScanAll(const char* phase) {
    Line(std::string("SCAN-BEGIN\t")+phase);
    Scan(game/L"players","stock-game-players");
    Scan(game/L"players2","stock-game-players2");
    Scan(game/L"bo2lan-data"/L"profiles","MOD-OFFLINE-PROFILE-NOT-ONLINE");
    Scan(game/L"bo2lan-data"/L"user","MOD-LEGACY-PROFILE-NOT-ONLINE");
    auto steam=SteamRoot();std::error_code ec;
    if(!steam.empty()&&NoLinks(steam/L"userdata")) {
        unsigned users=0;auto it=fs::directory_iterator(steam/L"userdata",ec);
        for(;!ec&&it!=fs::directory_iterator()&&users<64;it.increment(ec)) {
            const auto n=it->path().filename().wstring();
            if(n.empty()||!std::all_of(n.begin(),n.end(),[](wchar_t c){return c>=L'0'&&c<=L'9';})||!NoLinks(it->path()))continue;
            ++users;Scan(it->path()/L"212910","steam-ZOMBIES-cache");
        }
    }else Line("steam-userdata-root-unavailable");
    wchar_t local[32768]{};
    if(GetEnvironmentVariableW(L"LOCALAPPDATA",local,32768)&&game.has_root_name()) {
        auto virtualGame=fs::path(local)/L"VirtualStore"/game.relative_path();
        Scan(virtualGame/L"players","virtualstore-players");
        Scan(virtualGame/L"players2","virtualstore-players2");
    }
    Line(std::string("SCAN-END\t")+phase+"\tuniqueFiles="+std::to_string(fileCount));
}
bool Memory(std::uintptr_t address,void* dst,std::size_t size) {
    if(address<0x10000||size>kFileLimit||address>UINTPTR_MAX-size)return false;
    SIZE_T n{};return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),dst,size,&n)&&n==size;
}
template<class T>bool Read(std::uintptr_t p,T& v){return Memory(p,&v,sizeof v);}
std::string Name(std::uint32_t p) {
    std::string s;for(unsigned i=0;i<160;++i){char c{};if(!Read(p+i,c))return "<unreadable>";if(!c)return Clean(s);if(static_cast<unsigned char>(c)<32||static_cast<unsigned char>(c)>126)return "<nontext>";s+=c;}
    return "<too-long>";
}
// T6 asset-layout reference: OpenAssetTools T6_Assets.h; definition +24 next
// and +4 bit size also independently confirmed by current BO2 disassembly.
// These are read-only candidate layout observations, not import offsets.
struct Definition{std::uint32_t version,bits,structs,structCount,enums,enumCount,next;};
struct Struct{std::uint32_t name,bits,count,members,hash;};
struct Member{std::uint32_t name,bits,offset,type,externalIndex,range,serverDelta,clientDelta,arraySize,enumIndex,permission;};
struct Enum{std::uint32_t name,count,members,hash;};
bool NativeOnlineGate(std::uintptr_t base) {
    std::array<std::uint8_t,5> call{};std::int32_t relative{};
    if(!Memory(base+0x0086F030-0x400000,call.data(),call.size())||call[0]!=0xE8)return false;
    std::memcpy(&relative,call.data()+1,4);
    return static_cast<std::uint32_t>(0x0086F035+relative)==0x00861960;
}
void PreserveOnline(std::uintptr_t base,const Bytes& raw,std::uint64_t account,std::uint32_t version,std::uint32_t definitionHead) {
    if(!passiveOnline||backend)return;
    const auto at=[base](std::uintptr_t a){return base+a-0x400000;};
    int live{},signin{};std::uint8_t fetched{},crc{},ddl{},received{},steam{};
    std::uint64_t afterAccount{};std::uint32_t afterHead{};
    bool ready=Read(at(0x02F51040),afterAccount)&&Read(at(0x02E7F51C),afterHead)&&
        Read(at(0x02F4FFF8),live)&&Read(at(0x02F50000),signin)&&Read(at(0x02E28F10),fetched)&&
        Read(at(0x02EBC978),crc)&&Read(at(0x02EBC97A),ddl)&&Read(at(0x02EBC97B),received)&&Read(at(0x02E82B54),steam)&&
        afterAccount==account&&afterHead==definitionHead&&steam==1&&NativeOnlineGate(base)&&
        rank::ReadyForCapture(account,signin,live,fetched,crc,ddl,received);
    std::string reason;ready=ready&&rank::ValidateRaw(raw,version,reason);
    static std::string pending,lastReadiness;
    static ULONGLONG pendingSince{};
    static std::set<std::string> captured;
    const std::string readiness=std::to_string(ready)+":"+Hex(account)+":"+std::to_string(version);
    if(readiness!=lastReadiness){Line("ONLINE-CAPTURE-READINESS\tready:account:version="+readiness);lastReadiness=readiness;}
    if(!ready){pending.clear();return;}
    const auto key=Hex(account)+":"+rank::Sha256(raw);
    if(key!=pending){pending=key;pendingSince=GetTickCount64();return;}
    // Two matching one-second polls, native validation and unpatched online
    // predicate. No native calls, code patching or forced profile download.
    if(GetTickCount64()-pendingSince<1000||captured.count(key)||captured.size()>=32)return;
    rank::Snapshot snapshot{account,version,raw,{}};
    if(!rank::Compress(raw,snapshot.compressed)){Line("ONLINE-CAPTURE\tcompression-failed");return;}
    const auto bundle=rank::Pack(snapshot);rank::Snapshot verified;
    if(bundle.empty()||!rank::Unpack(bundle,verified,reason)||verified.raw!=raw){Line("ONLINE-CAPTURE\tverification-failed");return;}
    const auto hash=SaveBlob(bundle);
    if(hash.size()!=64){Line("ONLINE-CAPTURE\tpreservation-failed\t"+hash);return;}
    captured.insert(key);
    Line("ONLINE-PROFILE-SNAPSHOT\taccount="+Hex(account)+"\tversion="+std::to_string(version)+
        "\tddlBytes="+std::to_string(raw.size())+"\tbundleBlob="+hash+"\trawBlob="+SaveBlob(raw)+
        "\toriginalOnlineCall=1\tstableValidated=1\tautoImported=0");
    Log("progress-diagnostics: verified native online profile preserved, version=%u bytes=%u; no import applied",version,static_cast<unsigned>(raw.size()));
}
void Schema(std::uint32_t pointer,const Definition& d) {
    auto key=Hex(pointer)+"-"+std::to_string(d.version);if(schemas.count(key)||schemas.size()>=16)return;
    if(!d.structCount||d.structCount>512||d.enumCount>1024)return;
    std::ostringstream s;s<<"layout=T6-candidate; verify before interpreting values\nversion="<<d.version<<" bits="<<d.bits<<" address="<<Hex(pointer)<<'\n';
    unsigned total=0;bool ok=true;
    for(unsigned i=0;i<d.structCount&&ok;++i) {
        Struct st{};if(!Read(d.structs+i*sizeof st,st)||st.count>4096){ok=false;break;}
        s<<"STRUCT\t"<<i<<'\t'<<Name(st.name)<<"\tbits="<<st.bits<<"\tcount="<<st.count<<'\n';
        for(unsigned j=0;j<st.count;++j) {
            Member m{};if(++total>16000||!Read(st.members+j*sizeof m,m)){ok=false;break;}
            s<<"MEMBER\t"<<i<<'\t'<<j<<'\t'<<Name(m.name)<<"\tbits="<<m.bits<<"\toffset="<<m.offset<<"\ttype="<<m.type
             <<"\texternal="<<m.externalIndex<<"\tarray="<<m.arraySize<<"\tenum="<<m.enumIndex<<"\trange="<<m.range
             <<"\tserverDelta="<<m.serverDelta<<"\tclientDelta="<<m.clientDelta<<"\tpermission="<<m.permission<<'\n';
        }
    }
    for(unsigned i=0;i<d.enumCount&&ok;++i) {
        Enum en{};if(!Read(d.enums+i*sizeof en,en)||en.count>4096){ok=false;break;}
        s<<"ENUM\t"<<i<<'\t'<<Name(en.name)<<'\n';
        for(unsigned j=0;j<en.count;++j){std::uint32_t name{};if(++total>32000||!Read(en.members+j*4,name)){ok=false;break;}s<<"ENUM-VALUE\t"<<i<<'\t'<<j<<'\t'<<Name(name)<<'\n';}
    }
    s<<"complete="<<ok<<'\n';const auto text=s.str();Bytes b(text.begin(),text.end());
    Line("DDL-SCHEMA\tversion="+std::to_string(d.version)+"\tcomplete="+std::to_string(ok)+"\tblob="+SaveBlob(b));
    // Capture each observed layout once, including a reported partial read.
    // Avoid repeatedly walking a bad/unavailable schema during gameplay.
    schemas.insert(key);
}
void NativeSnapshot(std::uintptr_t base) {
    const auto at=[base](std::uintptr_t a){return base+a-0x400000;};
    std::uint64_t account{};std::uint32_t head{};std::array<std::uint8_t,12> header{};
    int live{},signin{};std::uint8_t checksum{},ddlValid{},received{};
    if(!Read(at(0x02F51040),account)||!Read(at(0x02E7F51C),head)||!Memory(at(0x02EB0578),header.data(),header.size()))return;
    Read(at(0x02F4FFF8),live);Read(at(0x02F50000),signin);
    Read(at(0x02EBC978),checksum);Read(at(0x02EBC97A),ddlValid);Read(at(0x02EBC97B),received);
    const auto version=(std::uint32_t(header[4])<<24)|(std::uint32_t(header[5])<<16)|(std::uint32_t(header[6])<<8)|header[7];
    const auto state=Hex(account)+":"+std::to_string(live)+":"+std::to_string(signin)+":"+std::to_string(checksum)+":"+std::to_string(ddlValid)+":"+std::to_string(received)+":"+std::to_string(version)+":"+Hex(head);
    static std::string previousState,previousRaw;
    if(state!=previousState){Line("NATIVE-STATE\taccount:live:signin:crc:ddl:received:version:definition="+state);previousState=state;}
    const auto definitionHead=head;
    std::set<std::uint32_t> visited;
    for(unsigned n=0;head&&n<64&&visited.insert(head).second;++n) {
        Definition d{};if(!Read(head,d))break;
        if(d.version==version&&d.bits>0&&d.bits<=(0xc400-48)*8) {
            Schema(head,d);
            Bytes first((d.bits+384+7)/8),second(first.size());
            if(!Memory(at(0x02EB0578),first.data(),first.size())||!Memory(at(0x02EB0578),second.data(),second.size())||first!=second){Line("NATIVE-SNAPSHOT\tunstable-read-skipped");break;}
            const auto hash=rank::Sha256(first);
            if(hash!=previousRaw) {
                Inspect(backend?"NATIVE-BUFFER-WITH-LOCAL-EMULATOR":"NATIVE-BUFFER-NO-LOCAL-EMULATOR",fs::path("stats@02EB0578"),first,account);
                previousRaw=hash;
            }
            PreserveOnline(base,first,account,version,definitionHead);
            break;
        }
        head=d.next;
    }
}
DWORD WINAPI Worker(void*) {
    try {
        SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
        Line("BEGIN\tread-only automatic progress diagnostics\tbackendConfigured="+std::to_string(backend)+"\tpassiveOnline="+std::to_string(passiveOnline));
        Line("LIMITS\t30min; 64MiB blobs; 2MiB/file; 3000 entries/root/scan; no game-memory writes; no network; no automatic import");
        ScanAll("startup");
        auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        IMAGE_DOS_HEADER dos{};IMAGE_NT_HEADERS32 pe{};
        bool supported=Read(base,dos)&&dos.e_magic==IMAGE_DOS_SIGNATURE&&dos.e_lfanew>0&&dos.e_lfanew<0x100000&&Read(base+dos.e_lfanew,pe)&&
            pe.Signature==IMAGE_NT_SIGNATURE&&pe.FileHeader.Machine==IMAGE_FILE_MACHINE_I386&&pe.FileHeader.TimeDateStamp==0x6A8332D9&&pe.OptionalHeader.SizeOfImage==0x03F38000;
        Line("NATIVE-LAYOUT\tsupported="+std::to_string(supported));
        const auto started=GetTickCount64();unsigned scans=0;const unsigned scanAt[]{30,120,300,600};
        while(GetTickCount64()-started<30*60*1000) {
            Drain();if(supported)NativeSnapshot(base);
            if(scans<4&&GetTickCount64()-started>=scanAt[scans]*1000ULL){ScanAll("periodic");++scans;}
            Sleep(1000);
        }
        active=false;offline::SetStorageObserver(nullptr);Drain();Line("END\tcapture-window-complete");
    }catch(const std::exception& e){active=false;offline::SetStorageObserver(nullptr);Log("progress-diagnostics: worker stopped safely: %s",e.what());}
    catch(...){active=false;offline::SetStorageObserver(nullptr);Log("progress-diagnostics: worker stopped safely");}
    return 0;
}
}
bool Candidate(const fs::path& p) {
    auto n=p.filename().wstring();std::transform(n.begin(),n.end(),n.begin(),[](wchar_t c){return towlower(c);});
    const auto ext=fs::path(n).extension().wstring();
    if(ext==L".jpg"||ext==L".png"||ext==L".bmp"||ext==L".exe"||ext==L".dll"||ext==L".log"||
        ext==L".mp4"||ext==L".wav"||ext==L".ogg"||ext==L".ff"||ext==L".wad"||ext==L".svg"||ext==L".foo")return false;
    // Roots are already restricted to BO2 players and Steam app212910. Include
    // unknown/hashed filenames too: their format is exactly what we are seeking.
    return true;
}
bool DecodeProfile(const Bytes& input,Bytes& ddl,std::string& format,std::string& reason) {
    ddl.clear();format="unknown";reason.clear();if(input.size()<8||input.size()>kFileLimit){reason="size-outside-profile-probe";return false;}
    const auto validate=[&](Bytes b,const char* kind) {
        if(b.size()>=56){std::uint32_t marker{},len{};std::memcpy(&marker,b.data()+b.size()-4,4);std::memcpy(&len,b.data()+b.size()-8,4);
            if(marker==0x11292012&&len>=48&&len<b.size()-8)b.resize(len);}
        if(b.size()<48||b.size()>0xc400)return false;
        const auto version=(std::uint32_t(b[4])<<24)|(std::uint32_t(b[5])<<16)|(std::uint32_t(b[6])<<8)|b[7];
        if(!rank::ValidateRaw(b,version,reason))return false;
        ddl=std::move(b);format=kind;reason="checksum-valid; ownership and field semantics NOT established";return true;
    };
    if(validate(input,"native-raw-DDL"))return true;
    for(int window:{-13,15}) {
        Bytes inflated(0x20000);z_stream z{};if(inflateInit2(&z,window)!=Z_OK)continue;
        z.next_in=const_cast<Bytef*>(input.data());z.avail_in=static_cast<uInt>(input.size());z.next_out=inflated.data();z.avail_out=static_cast<uInt>(inflated.size());
        const auto result=inflate(&z,Z_FINISH);const auto n=z.total_out;const bool full=z.avail_in==0;inflateEnd(&z);
        if(result==Z_STREAM_END&&full){inflated.resize(n);if(validate(std::move(inflated),window==-13?"raw-DEFLATE-DDL":"zlib-DDL"))return true;}
    }
    reason="not a validated native DDL profile; retained for inspection";return false;
}
void Start(bool configured,bool onlineCapture) noexcept {
    try {
        if(active.exchange(true))return;backend=configured;passiveOnline=onlineCapture;game=offline::ExecutableDirectory();
        const auto root=game/L"BO2OfflineOp-diagnostics";
        if(!NoLinks(root)){active=false;return;}
        SYSTEMTIME t{};GetSystemTime(&t);wchar_t session[100]{};
        swprintf_s(session,L"%04u%02u%02u-%02u%02u%02u-%03u-pid%lu",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,GetCurrentProcessId());
        output=root/session;
        if(!fs::create_directories(output/L"blobs")){active=false;return;}
        report.open(output/L"progress.tsv",std::ios::out);if(!report){active=false;return;}
        offline::SetStorageObserver(&Observe);
        auto thread=CreateThread(nullptr,0,Worker,nullptr,0,nullptr);
        if(thread){CloseHandle(thread);Log("progress-diagnostics: automatic session %ls",output.c_str());}
        else{active=false;offline::SetStorageObserver(nullptr);Log("progress-diagnostics: thread start failed");}
    }catch(...){active=false;offline::SetStorageObserver(nullptr);Log("progress-diagnostics: initialization failed; gameplay unchanged");}
}
#ifdef OFFLINE_DIAGNOSTICS_TEST
bool CaptureSelfTest(const fs::path& isolatedRoot) {
    // Does not invoke Start/Worker, enumerate real Steam data, or read native
    // addresses. Exercise the actual scanner, storage queue, and blob writer.
    if(!isolatedRoot.is_absolute()||fs::exists(isolatedRoot)||!NoLinks(isolatedRoot))return false;
    game=isolatedRoot;output=isolatedRoot/L"capture";
    fs::create_directories(output/L"blobs");fs::create_directories(game/L"players");
    const auto path=game/L"players"/L"user_zm.cgp";Bytes source{1,2,3,4,5,6,7,8};
    if(!offline::WriteFile(path,source))return false;
    report.open(output/L"progress.tsv");if(!report)return false;
    active=true;offline::SetStorageObserver(&Observe);
    Scan(game/L"players","stock-game-players");
    const auto before=blobs.size();Scan(game/L"players","stock-game-players");if(blobs.size()!=before)return false;
    Observe("offline-read",path,source,0x0110000100000001ULL);
    offline::ObserveProgressRequest(10,3,source);Drain();
    active=false;offline::SetStorageObserver(nullptr);report.close();
    Bytes check;if(!offline::ReadFile(path,check)||check!=source)return false;
    if(!offline::ReadFile(output/L"blobs"/(rank::Sha256(source)+".bin"),check)||check!=source)return false;
    if(!offline::ReadFile(output/L"progress.tsv",check))return false;
    std::string text(check.begin(),check.end());
    return text.find("stock-game-players")!=text.npos&&text.find("offline-read")!=text.npos&&text.find("PROGRESS-SERVICE-REQUEST")!=text.npos;
}
#endif
}
