#include "dw_backend.h"
#include "offline_auth.h"
#include "offline_services.h"
#include "offline_storage.h"
#include "offline_platform.h"
#include "offline_session.h"
#include <windows.h>
#include <chrono>
#include <thread>
#include <future>

#include <iostream>

namespace {
using namespace bo2lan::offline;
#define CHECK(x) do{if(!(x)){std::cerr<<"FAILED line "<<__LINE__<<": "<<#x<<"\n";return false;}}while(0)
void* __fastcall PrepareFixture(void* owner,void*,const char*,std::uint32_t* size) {
    auto* p=static_cast<std::uint8_t*>(owner)+0x150;
    for(unsigned i=0;i<24;++i)p[i]=static_cast<std::uint8_t>(0x31+i);
    *size=88;return p;
}
Bytes Frame(std::uint8_t type,const Bytes& payload) {
    Bytes out;AppendRaw(out,static_cast<std::uint32_t>(payload.size()+2));
    out.push_back(0);out.push_back(type);out.insert(out.end(),payload.begin(),payload.end());return out;
}
bool WaitReadable(LocalServer& server) {
    auto deadline=GetTickCount64()+2000;
    while(!server.Readable()&&GetTickCount64()<deadline)Sleep(1);
    return server.Readable();
}
bool fixtureOfflineAdapter{},fixtureTransport{},fixtureSteamInit{};
bool __cdecl FixturePlatformConnection() {
    return fixtureOfflineAdapter && LocalPlatformAvailable(fixtureTransport,fixtureSteamInit);
}
bool NativeMenuSignInRegression() {
    // Exact 0086F030..0086F050 instruction sequence from Steam24784288.
    // Execute only this isolated leaf, relocated to a test callback and a
    // test-owned sign-in array. No game executable is loaded or launched.
    std::array<std::uint8_t,33> leaf{
        0xE8,0,0,0,0,0x84,0xC0,0x75,0x01,0xC3,
        0x8B,0x44,0x24,0x04,0x69,0xC0,0x70,0x10,0,0,
        0x33,0xC9,0x83,0xB8,0,0,0,0,0x02,0x0F,0x94,0xC0,0xC3};
    std::array<std::uint32_t,4*0x41c> states{};
    void* memory=VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    CHECK(memory);
    struct Release {void* value;~Release(){VirtualFree(value,0,MEM_RELEASE);}} release{memory};
    static_assert(sizeof(void*)==4,"Native test is for the Win32 campaign/Zombies adapter");
    const auto relative=static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&FixturePlatformConnection)-
        (reinterpret_cast<std::uintptr_t>(memory)+5));
    const auto stateAddress=static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(states.data()));
    std::memcpy(leaf.data()+1,&relative,4);std::memcpy(leaf.data()+24,&stateAddress,4);
    std::memcpy(memory,leaf.data(),leaf.size());DWORD previous{};
    CHECK(VirtualProtect(memory,4096,PAGE_EXECUTE_READ,&previous));
    CHECK(FlushInstructionCache(GetCurrentProcess(),memory,leaf.size()));
    const auto native=reinterpret_cast<bool (__cdecl*)(int)>(memory);
    states[0]=2;fixtureTransport=true;fixtureSteamInit=true;
    fixtureOfflineAdapter=false;
    // v1.99: native signed-in and completed DW downloads still produce a
    // FALSE menu predicate, which closes/reopens popup_connectingdw.
    for(int timerTicks=0;timerTicks<8;++timerTicks)CHECK(!native(0));
    fixtureOfflineAdapter=std::find(kLocalPlatformCallSites.begin(),kLocalPlatformCallSites.end(),
        std::uintptr_t{0x0086F030})!=kLocalPlatformCallSites.end();
    CHECK(native(0)); // v2.00: actual native predicate now accepts signed-in client
    states[0]=1;CHECK(!native(0));states[0]=0;CHECK(!native(0));
    states[0]=2;fixtureTransport=false;CHECK(!native(0));
    fixtureTransport=true;fixtureSteamInit=false;CHECK(!native(0));
    fixtureSteamInit=true;states[0x41c]=2;CHECK(native(1));CHECK(!native(2));
    return true;
}
ByteBuffer SessionFixture() {
    ByteBuffer request;request.WriteBlob(Bytes(37));request.WriteU32(1000);request.WriteU32(4);
    request.WriteU64(0x01100001FFFFFFFEULL);request.WriteBlob(Bytes(16,0x42));
    for(int i=0;i<8;++i) {
        if(i%2) {const std::uint8_t nullTag=20;request.WriteRaw(&nullTag,1);}
        else request.WriteScalar<std::int32_t>(7,i);
    }
    request.WriteFloat(1.25f);
    for(int i=0;i<4;++i){const std::uint8_t nullTag=20;request.WriteRaw(&nullTag,1);}
    return request;
}
bool SessionLifecycleRegression() {
    LocalSessions sessions;
    const auto framed=[](ByteBuffer request) {const std::uint8_t end=0;request.WriteRaw(&end,1);return request;};
    auto valid=framed(SessionFixture());
    for(std::size_t n=0;n<SessionFixture().Data().size();++n) {
        ByteBuffer truncated(Bytes(valid.Data().begin(),valid.Data().begin()+n));
        CHECK(sessions.Handle(1,framed(truncated),0).error==4);CHECK(sessions.Size()==0);
    }
    const auto create=sessions.Handle(1,valid,0);CHECK(create.error==0&&create.objects.size()==1&&sessions.Size()==1);
    ByteBuffer result(create.objects[0]);Bytes id;CHECK(result.ReadBlob(id)&&id.size()==8);
    ByteBuffer update;update.WriteBlob(id);const auto fields=SessionFixture().Data();update.WriteRaw(fields.data(),fields.size());
    CHECK(sessions.Handle(2,framed(update),0).error==0);
    ByteBuffer brokenUpdate;brokenUpdate.WriteBlob(id);CHECK(sessions.Handle(2,framed(brokenUpdate),0).error==4);
    CHECK(sessions.Size()==1);
    ByteBuffer remove;remove.WriteBlob(id);
    auto badRemove=remove;badRemove.WriteU32(1);CHECK(sessions.Handle(3,framed(badRemove),0).error==4&&sessions.Size()==1);
    CHECK(sessions.Handle(3,framed(remove),0).error==0&&sessions.Size()==0);
    CHECK(sessions.Handle(2,framed(update),0).error==4); // deleted handle cannot mutate another session
    CHECK(sessions.Handle(3,framed(remove),0).error==4);
    CHECK(sessions.Handle(1,valid,0).error==0); // return to menu/new solo session
    ByteBuffer unset;unset.WriteBlob(Bytes(37));
    const std::uint8_t nullTag=20;
    for(int i=0;i<3;++i)unset.WriteRaw(&nullTag,1); // native U32,U32,U64 sentinel
    unset.WriteBlob(Bytes(16));
    for(int i=0;i<13;++i)unset.WriteRaw(&nullTag,1); // I32/float sentinels
    CHECK(sessions.Handle(1,framed(unset),0).error==0);
    // Zero is NOT a valid unset field. Reject the previous synthetic shape.
    auto wrong=unset.Data();wrong.back()=0;
    CHECK(sessions.Handle(1,framed(ByteBuffer(wrong)),0).error==4);
    LocalSessions disconnected;CHECK(disconnected.Size()==0); // no stale handles on reconnect
    return true;
}
bool Contracts(bool isolated=false) {
    const wchar_t* profileNamespace=isolated?L"offline-profiles":L"profiles";
    if(isolated){ConfigureIsolatedProfilePolicy();CHECK(SelectPolicyAccount(0x01100001FFFFFFFEULL));}
    CHECK(NativeMenuSignInRegression());
    CHECK(SessionLifecycleRegression());
    for(unsigned padding=0;padding<8;++padding)for(unsigned sequence: {0u,7u,8u,13u,255u}) {
        Bytes end(1+padding,static_cast<std::uint8_t>(sequence));end[0]=0;
        CHECK(ByteBuffer(end).IsTaskEnd(static_cast<std::uint8_t>(sequence)));
    }
    CHECK(!ByteBuffer(Bytes{}).IsTaskEnd(13));
    CHECK(!ByteBuffer(Bytes{13,13}).IsTaskEnd(13));
    CHECK(!ByteBuffer(Bytes{0,13,12}).IsTaskEnd(13));
    CHECK(!ByteBuffer(Bytes(9,0)).IsTaskEnd(0));
    // A valid tagged scalar is not swallowed even when its tag equals padding.
    CHECK(!ByteBuffer(Bytes{8,8,8,8,8}).IsTaskEnd(8));
    // Fixed native-shaped request, with MAC calculated independently using
    // .NET HMACSHA1: key 00..17, data 03 06 00 -> 56347AEC... .
    // Native 009F7850 excludes service 0C and includes the sequence-byte pad.
    std::array<std::uint8_t,24> vectorKey{};
    for(unsigned i=0;i<vectorKey.size();++i)vectorKey[i]=static_cast<std::uint8_t>(i);
    Bytes nativeTime{0x56,0x34,0x7a,0xec,0x0c,0x03,0x06,0x00};
    CHECK(ValidateClientTask(nativeTime,vectorKey));
    auto tampered=nativeTime;tampered.back()^=1;CHECK(!ValidateClientTask(tampered,vectorKey));
    tampered=nativeTime;tampered[0]^=1;CHECK(!ValidateClientTask(tampered,vectorKey));
    tampered=nativeTime;std::uint32_t oldMarker=0xDEADBEEF;
    std::memcpy(tampered.data(),&oldMarker,4);CHECK(!ValidateClientTask(tampered,vectorKey));
    struct Reset{~Reset(){ResetLocalAuthentication();}}reset;
    ResetLocalAuthentication();
    std::array<std::uint8_t,0x2b8> native{};
    std::array<std::uint8_t,128> ticket{};std::uint32_t written{};
    CHECK(!CopyLocalTicket(ticket.data(),128,&written));
    CHECK(PrepareLocalAuthentication(native.data(),reinterpret_cast<PrepareNativePayload>(&PrepareFixture)));
    CHECK(CopyLocalTicket(ticket.data(),128,&written)&&written==128);
    CHECK(!CopyLocalTicket(ticket.data(),127,&written));
    CHECK(!std::memcmp(ticket.data()+32,native.data()+0x150,24));
    native[0x150]^=1;CHECK(!CopyLocalTicket(ticket.data(),128,&written));native[0x150]^=1;
    const std::uint32_t seed=0x12345678,title=18397;
    BitBuffer request;request.Types(false);request.WriteBool(true);request.Types(true);
    request.WriteU32(seed);request.WriteU32(title);request.WriteU32(128);request.WriteBytes(ticket.data(),128);
    auto frame=Frame(28,request.Data());
    LocalServer auth(ServerKind::Auth);
    CHECK(auth.Send(reinterpret_cast<const char*>(frame.data()),static_cast<int>(frame.size()))==frame.size());
    std::array<char,65536> wire{};const int length=auth.Receive(wire.data(),1024);
    CHECK(length==271&&wire[4]==0&&wire[5]==29);
    // Independent bit offsets from native 009EE300/009F4DF0, deliberately NOT
    // BitBuffer reading back BitBuffer output. The native game is not executed.
    const auto extract=[&](unsigned start,unsigned count){
        Bytes result((count+7)/8);
        for(unsigned i=0;i<count;++i){auto b=static_cast<unsigned char>(wire[6+(start+i)/8]);result[i/8]|=((b>>((start+i)%8))&1)<<(i%8);}
        return result;
    };
    auto result=extract(1,32);std::uint32_t status{};std::memcpy(&status,result.data(),4);CHECK(status==700);
    auto replySeed=extract(33,32);CHECK(!std::memcmp(replySeed.data(),&seed,4));
    auto cipher=extract(65,1024),lsg=extract(1089,1024);
    std::array<std::uint8_t,24> key{},wrong{};std::memcpy(key.data(),native.data()+0x150,24);
    auto clear=TripleDes(cipher,Tiger(&seed,4),key,false);CHECK(clear.size()==128);
    CHECK(clear[0]==0xde&&clear[1]==0xad&&clear[2]==0xbd&&clear[3]==0xef);
    CHECK(!std::memcmp(clear.data()+5,&title,4));CHECK(!std::memcmp(clear.data()+97,lsg.data(),24));
    auto wrongClear=TripleDes(cipher,Tiger(&seed,4),wrong,false);
    CHECK(wrongClear.size()==128&&std::memcmp(clear.data(),wrongClear.data(),4));
    std::array<std::uint8_t,128> lsgArray{};std::copy(lsg.begin(),lsg.end(),lsgArray.begin());
    CHECK(!ValidateLocalHello(title,seed,lsgArray)); // v1.94 missing native acceptance regression
    std::memcpy(native.data()+0x24,clear.data()+5,4);std::memcpy(native.data()+0x28,&seed,4);
    std::memcpy(native.data()+0xac,clear.data()+97,24);
    CHECK(ValidateLocalHello(title,seed,lsgArray));
    CHECK(!ValidateLocalHello(0,seed,lsgArray));
    lsgArray[0]^=1;CHECK(!ValidateLocalHello(title,seed,lsgArray));lsgArray[0]^=1;

    Bytes handshake;AppendRaw(handshake,std::uint32_t{200});AppendRaw(handshake,std::uint32_t{65535});
    BitBuffer hello;hello.Types(false);hello.WriteBool(true);hello.Types(true);
    hello.WriteU32(title);hello.WriteU32(seed);hello.WriteBytes(lsg.data(),128);
    auto helloFrame=Frame(7,hello.Data());
    LocalServer lobby(ServerKind::Lobby,true);
    CHECK(lobby.Send(reinterpret_cast<const char*>(handshake.data()),8)==8);
    CHECK(WaitReadable(lobby));CHECK(lobby.Receive(wire.data(),1024)==15);
    CHECK(lobby.Send(reinterpret_cast<const char*>(helloFrame.data()),static_cast<int>(helloFrame.size()))==helloFrame.size());
    auto deadline=GetTickCount64()+2000;while(!lobby.Ready()&&!lobby.Failed()&&GetTickCount64()<deadline)Sleep(1);
    CHECK(lobby.Ready());
    std::array<std::uint8_t,24> session{};std::copy_n(lsg.begin(),24,session.begin());
    // Regression: valid client request reaches the service worker, whose reply
    // retains the *different* native server-to-client DEADBEEF envelope.
    auto validPlain=nativeTime;std::array<std::uint8_t,4> mac{};
    CHECK(ClientTaskMac(validPlain.data()+5,validPlain.size()-5,session,mac));
    std::copy(mac.begin(),mac.end(),validPlain.begin());
    auto validCipher=TripleDes(validPlain,Tiger(&seed,4),session,true);
    Bytes valid;AppendRaw(valid,static_cast<std::uint32_t>(validCipher.size()+5));valid.push_back(1);
    AppendRaw(valid,seed);valid.insert(valid.end(),validCipher.begin(),validCipher.end());
    CHECK(lobby.Send(reinterpret_cast<const char*>(valid.data()),static_cast<int>(valid.size()))==valid.size());
    CHECK(WaitReadable(lobby));CHECK(!lobby.Failed());
    const auto replyLength=lobby.Receive(wire.data(),1024);CHECK(replyLength>9);
    std::uint32_t replyIv{};std::memcpy(&replyIv,wire.data()+5,4);
    Bytes replyCipher(reinterpret_cast<std::uint8_t*>(wire.data()+9),reinterpret_cast<std::uint8_t*>(wire.data()+replyLength));
    auto replyClear=TripleDes(replyCipher,Tiger(&replyIv,4),session,false);
    CHECK(replyClear.size()>=5);CHECK(!std::memcmp(replyClear.data(),&oldMarker,4)&&replyClear[4]==1);
    auto task=[&](std::uint8_t service,std::uint8_t operation,const ByteBuffer& parameters,
                  std::uint32_t expectedError,Bytes& objects,std::uint32_t expectedCount=1,std::uint32_t taskSeed=0x12345678,bool captured=false)->bool {
        ByteBuffer payload;payload.WriteU8(operation);payload.WriteRaw(parameters.Data().data(),parameters.Data().size());
        Bytes plain(4);plain.push_back(service);plain.insert(plain.end(),payload.Data().begin(),payload.Data().end());
        // Native submit009ED315 calls009DFF00 to append a zero type/end marker
        // BEFORE009F7850 fills the remaining cipher block with sequence bytes.
        // Captured payloads already contain this marker and their exact pad.
        if(!captured)plain.push_back(0);
        plain.resize((plain.size()+7)&~std::size_t(7),static_cast<std::uint8_t>(taskSeed));
        std::array<std::uint8_t,4> tag{};
        CHECK(ClientTaskMac(plain.data()+5,plain.size()-5,session,tag));std::copy(tag.begin(),tag.end(),plain.begin());
        auto cipher=TripleDes(plain,Tiger(&taskSeed,4),session,true);
        Bytes packet;AppendRaw(packet,static_cast<std::uint32_t>(cipher.size()+5));packet.push_back(1);
        AppendRaw(packet,taskSeed);packet.insert(packet.end(),cipher.begin(),cipher.end());
        CHECK(lobby.Send(reinterpret_cast<const char*>(packet.data()),static_cast<int>(packet.size()))==packet.size());
        CHECK(WaitReadable(lobby));CHECK(!lobby.Failed());
        auto n=lobby.Receive(wire.data(),static_cast<int>(wire.size()));CHECK(n>9);std::uint32_t responseSeed{};
        std::memcpy(&responseSeed,wire.data()+5,4);
        Bytes encrypted(reinterpret_cast<std::uint8_t*>(wire.data()+9),reinterpret_cast<std::uint8_t*>(wire.data()+n));
        auto decrypted=TripleDes(encrypted,Tiger(&responseSeed,4),session,false);
        CHECK(decrypted.size()>=5&&!std::memcmp(decrypted.data(),&oldMarker,4)&&decrypted[4]==1);
        ByteBuffer response(Bytes(decrypted.begin()+5,decrypted.end()));
        std::uint64_t transaction{};std::uint32_t error{},count{},total{};std::uint8_t responseOp{};
        CHECK(response.ReadU64(transaction)&&transaction&&response.ReadU32(error)&&error==expectedError&&
            response.ReadU8(responseOp)&&responseOp==operation);
        objects.clear();
        if(!error){CHECK(response.ReadU32(count)&&count==expectedCount);if(count)CHECK(response.ReadU32(total)&&total==count);objects=response.Rest();}
        return true;
    };
    const auto testTitle="contract-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64());
    constexpr std::uint64_t testOwner=0x01100001FFFFFFFELL;
    const Bytes profile{7,0,4,5,0,3};Bytes objects;
    ByteBuffer archive;archive.WriteU64(testOwner);archive.WriteU16(3);archive.WriteBool(false);
    archive.WriteU16(1000);archive.WriteU16(1001);
    CHECK(task(15,2,archive,0,objects,2));
    // Independent golden record bytes: type6 U16 key, type9 signed64 value.
    const Bytes emptyArchive{6,0xe8,3,9,0,0,0,0,0,0,0,0,6,0xe9,3,9,0,0,0,0,0,0,0,0};
    CHECK(objects.size()>=emptyArchive.size()&&std::equal(emptyArchive.begin(),emptyArchive.end(),objects.begin()));
    ByteBuffer badArchive;badArchive.WriteU64(testOwner);badArchive.WriteScalar<std::int16_t>(5,3);badArchive.WriteBool(false);
    CHECK(task(15,2,badArchive,4,objects));
    CHECK(task(15,2,ByteBuffer{},4,objects));
    CHECK(task(15,1,ByteBuffer{},4,objects)); // unverified cloud-write format is not silently acknowledged
    CHECK(task(21,1,SessionFixture(),0,objects));
    ByteBuffer createdSession(objects);Bytes createdId;CHECK(createdSession.ReadBlob(createdId)&&createdId.size()==8);
    ByteBuffer sessionUpdate;sessionUpdate.WriteBlob(createdId);const auto fixture=SessionFixture().Data();
    sessionUpdate.WriteRaw(fixture.data(),fixture.size());CHECK(task(21,2,sessionUpdate,0,objects,0));
    ByteBuffer sessionDelete;sessionDelete.WriteBlob(createdId);CHECK(task(21,3,sessionDelete,0,objects,0));
    ByteBuffer save;save.WriteString(testTitle);save.WriteString("profile.bin");save.WriteBool(false);
    save.WriteBlob(profile);save.WriteU64(testOwner);
    CHECK(task(10,10,save,0,objects));
    ByteBuffer fetch;fetch.WriteString(testTitle);fetch.WriteString("profile.bin");fetch.WriteU64(testOwner);fetch.WriteString("pc");
    CHECK(task(10,12,fetch,0,objects));ByteBuffer stored(objects);Bytes loadedProfile;
    CHECK(stored.ReadBlob(loadedProfile)&&loadedProfile==profile);
    ByteBuffer absent;absent.WriteString("nonexistent-publisher-contract-file");
    CHECK(task(10,7,absent,1000,objects));
    CHECK(task(255,1,ByteBuffer{},4,objects));
    // Reproduce the complete observed v1.96 startup service set, including
    // operations previously rejected. Queries describe a local solo account.
    ByteBuffer console;console.WriteBlob(Bytes(16,0x31));console.WriteU32(3);console.WriteU32(24784288);
    console.WriteU64(0);console.WriteU64(0);console.WriteU64(testOwner);console.WriteBlob(Bytes(6));
    CHECK(task(38,4,console,0,objects,0));
    CHECK(task(38,4,ByteBuffer{},4,objects));
    ByteBuffer presence;presence.WriteU64(0);presence.WriteBlob(Bytes{1,2,3});
    CHECK(task(68,1,presence,0,objects,0));
    ByteBuffer members;const std::uint8_t arrayU64=110;members.WriteRaw(&arrayU64,1);members.WriteU32(8);
    const std::uint32_t one=1;members.WriteRaw(&one,4);members.WriteRaw(&testOwner,8);
    CHECK(task(81,1,members,0,objects));ByteBuffer team(objects);std::uint64_t teamId=123;CHECK(team.ReadU64(teamId)&&teamId==0);
    CHECK(task(81,6,members,0,objects,0));
    CHECK(task(81,8,members,0,objects));
    ByteBuffer teamMembers(objects);Bytes memberIds;std::uint64_t returnedTeam{};
    CHECK(teamMembers.ReadU64(returnedTeam)&&returnedTeam==testOwner);
    CHECK(teamMembers.ReadArray(10,8,memberIds)&&memberIds.empty());
    const Bytes emptyNames{116,8,0,0,0,0,0,0,0,0};
    Bytes names(emptyNames.size());CHECK(teamMembers.ReadRaw(names.data(),names.size())&&names==emptyNames);
    CHECK(task(81,8,ByteBuffer{},4,objects));
    // Empty raw arrays yield the 17-byte padded parameter length observed in
    // the real log. This is a separate case from a one-ID (25-byte) request.
    ByteBuffer emptyIds;emptyIds.WriteRaw(&arrayU64,1);emptyIds.WriteU32(0);
    const std::uint32_t zero=0;emptyIds.WriteRaw(&zero,4);
    CHECK(task(81,6,emptyIds,0,objects,0));
    CHECK(task(81,8,emptyIds,0,objects,0));
    // Verbatim parameter bytes from the native1.98.1 capture (no user IDs).
    ByteBuffer capturedLeague(Bytes{0x6e,8,0,0,0,0,0,0,0,0,0,13,13,13,13,13,13});
    CHECK(task(81,8,capturedLeague,0,objects,0,13,true));
    ByteBuffer capturedLeagueInfo(Bytes{0x6e,8,0,0,0,0,0,0,0,0,0,14,14,14,14,14,14});
    CHECK(task(81,6,capturedLeagueInfo,0,objects,0,14,true));
    auto badLeague=capturedLeague.Data();badLeague[10]=1;
    CHECK(task(81,8,ByteBuffer(badLeague),4,objects,0,13,true));
    badLeague=capturedLeague.Data();badLeague.back()=12;
    CHECK(task(81,8,ByteBuffer(badLeague),4,objects,0,13,true));
    ByteBuffer memberships;memberships.WriteU64(testOwner);memberships.WriteU8(1);memberships.WriteU32(0);memberships.WriteU32(10);
    CHECK(task(81,2,memberships,0,objects,0));
    ByteBuffer groups;const std::uint8_t arrayU32=108;groups.WriteRaw(&arrayU32,1);groups.WriteU32(4);
    groups.WriteRaw(&one,4);groups.WriteRaw(&one,4);CHECK(task(28,1,groups,0,objects,0));
    ByteBuffer counter;counter.WriteU32(123);CHECK(task(23,2,counter,0,objects));
    ByteBuffer counterResult(objects);std::uint32_t counterId{};std::int64_t counterValue=-1;
    CHECK(counterResult.ReadU32(counterId)&&counterId==123&&counterResult.ReadI64(counterValue)&&counterValue==0);
    ByteBuffer lookup;lookup.WriteU64(testOwner);CHECK(task(8,1,lookup,0,objects,0));
    // Native single-profile query has the maximum eight-byte trailer:
    // zero marker + seven sequence bytes. Replace only the captured owner ID.
    ByteBuffer capturedProfile;capturedProfile.WriteU64(testOwner);
    const Bytes profileTail{0,4,4,4,4,4,4,4};capturedProfile.WriteRaw(profileTail.data(),profileTail.size());
    CHECK(task(8,1,capturedProfile,0,objects,0,4,true));
    // Native-controlled local session lifecycle: no menu/map command is called.
    ByteBuffer incompleteCreate;incompleteCreate.WriteBlob(Bytes(37,0x41));incompleteCreate.WriteU32(0);incompleteCreate.WriteU32(1);
    incompleteCreate.WriteU32(55); // old fixture omitted the native derived record
    CHECK(task(21,1,incompleteCreate,4,objects));
    auto create=SessionFixture();
    CHECK(task(21,1,create,0,objects));ByteBuffer created(objects);Bytes sessionId;
    CHECK(created.ReadBlob(sessionId)&&sessionId.size()==8);
    ByteBuffer update;update.WriteBlob(sessionId);update.WriteRaw(create.Data().data(),create.Data().size());
    CHECK(task(21,2,update,0,objects,0));
    ByteBuffer remove;remove.WriteBlob(sessionId);CHECK(task(21,3,remove,0,objects,0));
    CHECK(task(21,2,update,4,objects));
    CHECK(task(21,1,ByteBuffer{},4,objects));
    // Persistent local leaderboard snapshot, distinct from native progression.
    const auto board=static_cast<std::uint32_t>(GetTickCount64());
    ByteBuffer row;row.WriteU32(board);row.WriteU64(testOwner);row.WriteU8(0);row.WriteScalar<std::int64_t>(9,42);
    for(std::int32_t n=0;n<10;++n)row.WriteScalar(7,n);
    CHECK(task(4,1,row,0,objects,0));
    ByteBuffer readRow;readRow.WriteU32(board);readRow.WriteU64(testOwner);
    CHECK(task(4,3,readRow,0,objects));ByteBuffer loadedRow(objects);
    std::uint64_t rowOwner{},rank{};std::int64_t score{};std::string rowName;std::uint32_t seconds{};
    CHECK(loadedRow.ReadU64(rowOwner)&&rowOwner==testOwner&&loadedRow.ReadI64(score)&&score==42);
    CHECK(loadedRow.ReadU64(rank)&&loadedRow.ReadString(rowName)&&loadedRow.ReadU32(seconds));
    for(std::int32_t n=0;n<10;++n){std::int32_t v=-1;CHECK(loadedRow.ReadI32(v)&&v==n);}
    CHECK(task(4,1,ByteBuffer{},4,objects));
    // Shape of the captured579-byte post-match task: twelve boards with ten
    // columns each; most columns are native tag20 NAN with no payload. IDs
    // and values here are synthetic, not a shipped personal profile.
    ByteBuffer batch;
    for(unsigned n=0;n<12;++n){
        batch.WriteU32(board+1+n);batch.WriteU64(testOwner);batch.WriteU8(0);batch.WriteScalar<std::int64_t>(9,n);
        const unsigned present=(n==6||n==9)?4:3;
        for(unsigned c=0;c<10;++c){if(c<present)batch.WriteScalar<std::int32_t>(7,c);else {const std::uint8_t nan=20;batch.WriteRaw(&nan,1);}}
    }
    CHECK(task(4,1,batch,0,objects,0,38));
    for(unsigned n=0;n<12;++n){
        ByteBuffer query;query.WriteU32(board+1+n);query.WriteU64(testOwner);
        CHECK(task(4,3,query,0,objects));ByteBuffer value(objects);
        CHECK(value.ReadU64(rowOwner)&&rowOwner==testOwner&&value.ReadI64(score)&&score==n);
        CHECK(value.ReadU64(rank)&&value.ReadString(rowName)&&value.ReadU32(seconds));
        for(unsigned c=0;c<10;++c){
            if(c<((n==6||n==9)?4u:3u)){std::int32_t v{};CHECK(value.ReadI32(v)&&v==c);}
            else {std::uint8_t tag{};CHECK(value.ReadRaw(&tag,1)&&tag==20);}
        }
        auto stored=DataRoot()/profileNamespace/L"01100001FFFFFFFE"/L"offline-leaderboards"/("board-"+std::to_string(board+1+n)+".bin");
        CHECK(DeleteFileW(stored.c_str()));
    }
    // The observed 256-counter reply is larger than the former 1KB fixture.
    ByteBuffer manyCounters;for(unsigned n=0;n<256;++n)manyCounters.WriteU32(n);
    CHECK(task(23,2,manyCounters,0,objects,256));
    // A padding byte equal to the U32 type tag must not become another ID.
    CHECK(task(23,2,manyCounters,0,objects,256,8));
    // The real1281-byte query is255 IDs, a zero marker and five pad bytes,
    // not256 IDs. Test the observed length as well as the larger256-ID query.
    ByteBuffer nativeCounterShape;for(unsigned n=0;n<255;++n)nativeCounterShape.WriteU32(n);
    CHECK(task(23,2,nativeCounterShape,0,objects,255,5));
    CHECK(task(23,2,ByteBuffer(Bytes{0x77}),4,objects));
    {
        // Public profile is isolated from the real offline identity/save.
        const auto previousOwner=g_localUserId;
        struct RestoreOwner {std::uint64_t value;~RestoreOwner(){g_localUserId=value;}} restore{previousOwner};
        g_localUserId=testOwner;
        Bytes publicData(1024,0x42);ByteBuffer publicWrite;
        publicWrite.WriteScalar<std::int32_t>(7,3);publicWrite.WriteBlob(publicData);
        CHECK(task(8,3,publicWrite,0,objects,0));
        CHECK(task(8,1,lookup,0,objects));ByteBuffer publicRead(objects);
        std::uint64_t publicOwner{};std::int32_t publicVersion{};Bytes publicBlob;
        CHECK(publicRead.ReadU64(publicOwner)&&publicOwner==testOwner&&publicRead.ReadI32(publicVersion)&&publicVersion==3);
        CHECK(publicRead.ReadBlob(publicBlob)&&publicBlob==publicData);
        ByteBuffer invalidPublic;invalidPublic.WriteScalar<std::int32_t>(7,4);invalidPublic.WriteBlob(Bytes{1});
        CHECK(task(8,3,invalidPublic,4,objects));
        CHECK(task(8,1,lookup,0,objects));ByteBuffer unchanged(objects);
        CHECK(unchanged.ReadU64(publicOwner)&&unchanged.ReadI32(publicVersion)&&publicVersion==3);
        CHECK(unchanged.ReadBlob(publicBlob)&&publicBlob==publicData);
        auto publicFile=DataRoot()/profileNamespace/L"01100001FFFFFFFE"/L"offline-public"/L"public-profile.bin";
        CHECK(DeleteFileW(publicFile.c_str()));CHECK(RemoveDirectoryW(publicFile.parent_path().c_str()));
    }
    auto rowFile=DataRoot()/profileNamespace/L"01100001FFFFFFFE"/L"offline-leaderboards"/("board-"+std::to_string(board)+".bin");
    CHECK(DeleteFileW(rowFile.c_str()));
    RemoveDirectoryW(rowFile.parent_path().c_str()); // only succeeds if empty
    for(auto social: {std::pair<std::uint8_t,std::uint8_t>{31,3},{33,2}}){
        CHECK(task(social.first,social.second,ByteBuffer{},0,objects));
        ByteBuffer linked(objects);bool registered=true;CHECK(linked.ReadBool(registered)&&!registered);
    }
    // Raw bandwidth probe must never enqueue a task result (type 1).
    Bytes probePlain(24,0);probePlain[4]=18;
    CHECK(ClientTaskMac(probePlain.data()+5,probePlain.size()-5,session,mac));std::copy(mac.begin(),mac.end(),probePlain.begin());
    auto probeCipher=TripleDes(probePlain,Tiger(&seed,4),session,true);Bytes probe;
    AppendRaw(probe,static_cast<std::uint32_t>(probeCipher.size()+5));probe.push_back(1);AppendRaw(probe,seed);
    probe.insert(probe.end(),probeCipher.begin(),probeCipher.end());
    CHECK(lobby.Send(reinterpret_cast<const char*>(probe.data()),static_cast<int>(probe.size()))==probe.size());
    CHECK(WaitReadable(lobby));const auto probeReplySize=lobby.Receive(wire.data(),1024);CHECK(probeReplySize>9);
    std::memcpy(&replyIv,wire.data()+5,4);
    replyCipher.assign(reinterpret_cast<std::uint8_t*>(wire.data()+9),reinterpret_cast<std::uint8_t*>(wire.data()+probeReplySize));
    replyClear=TripleDes(replyCipher,Tiger(&replyIv,4),session,false);CHECK(replyClear.size()>=5&&replyClear[4]==5);
    CHECK(task(31,3,ByteBuffer{},0,objects)); // queue still aligned after probe
    auto profileDirectory=DataRoot()/profileNamespace/L"01100001FFFFFFFE"/testTitle;
    CHECK(DeleteFileW((profileDirectory/L"profile.bin").c_str()));CHECK(RemoveDirectoryW(profileDirectory.c_str()));
    // Correctly encrypted but invalid client MAC must close without dispatch.
    Bytes badPlain(8);
    auto badCipher=TripleDes(badPlain,Tiger(&seed,4),session,true);
    Bytes bad;AppendRaw(bad,static_cast<std::uint32_t>(badCipher.size()+5));bad.push_back(1);AppendRaw(bad,seed);bad.insert(bad.end(),badCipher.begin(),badCipher.end());
    lobby.Send(reinterpret_cast<const char*>(bad.data()),static_cast<int>(bad.size()));
    CHECK(WaitReadable(lobby));CHECK(lobby.Failed());CHECK(lobby.Receive(wire.data(),1024)==SOCKET_ERROR);
    {
        LocalServer waiting(ServerKind::Lobby,true);
        auto reader=std::async(std::launch::async,[&]{char b{};return waiting.Receive(&b,1,0,true);});
        waiting.Close();CHECK(reader.wait_for(std::chrono::seconds(1))==std::future_status::ready);
        CHECK(reader.get()==SOCKET_ERROR);
    }
    CHECK(SafeName("zmstatsCompressed")=="zmstatsCompressed");
    for(auto name:{"../escape","NUL","con.txt","a:b","a/b","a\\b","trailing.",""})CHECK(SafeName(name).empty());
    wchar_t temp[32768]{};CHECK(GetTempPathW(32768,temp));
    auto root=std::filesystem::path(temp)/(L"bo2-offline-contract-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    auto file=root/L"save.bin";Bytes first{1,2,3},second{4,5,6,7},loaded;
    CHECK(WriteFile(file,first));CHECK(ReadFile(file,loaded)&&loaded==first);
    CHECK(WriteFile(file,second));CHECK(ReadFile(file,loaded)&&loaded==second);
    auto backup=file;backup+=L".bak";CHECK(ReadFile(backup,loaded)&&loaded==first);
    CHECK(!WriteFile(file/L"invalid-parent",first));
    CHECK(ReadFile(file,loaded)&&loaded==second);
    // Only these test-created files are removed; no recursive cleanup.
    CHECK(DeleteFileW(file.c_str()));CHECK(DeleteFileW(backup.c_str()));CHECK(RemoveDirectoryW(root.c_str()));
    return true;
}
}
int main(int argc,char** argv) {
    if(argc>1){if(!Contracts(std::string(argv[1])=="--isolated-contracts"))return 1;std::cout<<"Offline auth/state/worker/storage contracts passed\n";return 0;}
    if (!bo2lan::RunBackendSelfTests()) {
        std::cerr << "BO2 LAN protocol self-test failed\n";
        return 1;
    }
    std::cout << "BO2 LAN protocol self-test passed\n";
    return 0;
}
