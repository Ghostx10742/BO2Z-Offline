#include "offline_storage.h"
#include <windows.h>
#include <iostream>
#include <stdexcept>
namespace fs=std::filesystem;
namespace s=bo2lan::offline;
using bo2lan::offline::Bytes;
#define CHECK(x) do {if(!(x))throw std::runtime_error("Failed: " #x);}while(0)
static constexpr std::uint64_t first=0x01100001FFFFFFF0ULL, second=first+1;
static Bytes Payload(unsigned seed) {
    // Arbitrary complete profile bytes: the storage layer must not reinterpret
    // rank, bank, locker, or any other native fields or truncate them.
    Bytes data(41094);for(std::size_t n=0;n<data.size();++n)data[n]=static_cast<unsigned char>((n*17+seed)%251);
    return data;
}
static void Child(const std::wstring& mode,const fs::path& root) {
    wchar_t exe[32768]{};CHECK(GetModuleFileNameW(nullptr,exe,32768));
    auto command=L"\""+std::wstring(exe)+L"\" "+mode+L" \""+root.wstring()+L"\"";
    STARTUPINFOW si{};si.cb=sizeof si;PROCESS_INFORMATION pi{};
    CHECK(CreateProcessW(exe,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi));
    CloseHandle(pi.hThread);
    CHECK(WaitForSingleObject(pi.hProcess,10000)==WAIT_OBJECT_0);
    DWORD code{};CHECK(GetExitCodeProcess(pi.hProcess,&code));CloseHandle(pi.hProcess);CHECK(code==0);
}
int wmain(int argc,wchar_t** argv) {try {
    if(argc==3) {
        const std::wstring mode=argv[1];s::SetTestDataRoot(fs::absolute(argv[2]));
        s::ConfigureIsolatedProfilePolicy();s::InitializeLocalIdentity();
        Bytes data;CHECK(!s::ReadUserFile("zmstatsCompressed",data));
        CHECK(!s::WriteUserFile("before-auth",Bytes{1}));
        CHECK(!s::SelectPolicyAccount(0));
        const auto account=mode==L"second"?second:first;
        CHECK(s::SelectPolicyAccount(account));CHECK(s::SelectPolicyAccount(account));
        CHECK(!s::SelectPolicyAccount(account==first?second:first));
        CHECK(!s::WriteUserFile("wrong-owner",Bytes{1},account==first?second:first));
        CHECK(!s::ReadUserFile("zmstatsCompressed",data,account==first?second:first));
        CHECK(!s::ReadUserFile("legacy-only",data));
        CHECK(!s::ReadUserFile("imported-only",data));
        CHECK(!s::ReadUserFile("../outside",data));
        if(mode==L"write"||mode==L"second") {
            CHECK(!s::ReadUserFile("zmstatsCompressed",data));
            CHECK(s::WriteUserFile("zmstatsCompressed",Payload(mode==L"write"?1:3)));
            CHECK(s::WriteUserFile("zmdatabk0000",Payload(4)));
            CHECK(s::WriteUserFile("public-profile.bin",Bytes(1035,7),0,"offline-public"));
            CHECK(s::WriteUserFile("board-60004.bin",Bytes{5,4,3},0,"offline-leaderboards"));
        } else {
            CHECK(s::ReadUserFile("zmstatsCompressed",data)&&data==Payload(mode==L"read"?1:2));
            CHECK(s::ReadUserFile("zmdatabk0000",data)&&data==Payload(4));
            CHECK(s::ReadUserFile("public-profile.bin",data,0,"offline-public")&&data==Bytes(1035,7));
            CHECK(s::ReadUserFile("board-60004.bin",data,0,"offline-leaderboards")&&data==Bytes({5,4,3}));
            if(mode==L"read")CHECK(s::WriteUserFile("zmstatsCompressed",Payload(2)));
            else {
                CHECK(s::ReadFile(s::DataRoot()/L"offline-profiles"/L"01100001FFFFFFF0"/L"t6"/L"zmstatsCompressed.bak",data)&&data==Payload(1));
                CHECK(!s::WriteUserFile("zmstatsCompressed/invalid-parent",Bytes{9}));
                CHECK(s::ReadUserFile("zmstatsCompressed",data)&&data==Payload(2));
            }
        }
        return 0;
    }
    wchar_t temp[32768]{};CHECK(GetTempPathW(32768,temp));
    const auto root=fs::path(temp)/(L"bo2-independent-profiles-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    CHECK(!fs::exists(root));
    CHECK(s::WriteFile(root/L"profile-choice.bin",Bytes(9,3)));
    CHECK(s::WriteFile(root/L"user"/L"legacy-only",Bytes{8}));
    CHECK(s::WriteFile(root/L"profiles"/L"01100001FFFFFFF0"/L"t6"/L"imported-only",Bytes{9}));
    CHECK(s::WriteFile(root/L"stock-online-sentinel",Bytes{2,3,4}));
    Child(L"write",root);Child(L"read",root);Child(L"second",root);Child(L"verify",root);
    Bytes b;CHECK(s::ReadFile(root/L"profile-choice.bin",b)&&b==Bytes(9,3));
    CHECK(s::ReadFile(root/L"user"/L"legacy-only",b)&&b==Bytes{8});
    CHECK(s::ReadFile(root/L"profiles"/L"01100001FFFFFFF0"/L"t6"/L"imported-only",b)&&b==Bytes{9});
    CHECK(s::ReadFile(root/L"stock-online-sentinel",b)&&b==Bytes({2,3,4}));
    CHECK(!fs::exists(root/L"offline-identity.bin"));
    std::wcout<<L"PASS: independent accounts, four separate processes, byte-exact full-profile persistence, backups, write failure, no import/legacy fallback. Test evidence: "<<root<<L"\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
