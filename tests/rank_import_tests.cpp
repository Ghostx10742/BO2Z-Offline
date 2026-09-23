#include "rank_profile.h"
#include "offline_storage.h"
#include <windows.h>
#include <zlib.h>
#include <iostream>
#include <stdexcept>
namespace rank=bo2lan::rank;
namespace store=bo2lan::offline;
#define CHECK(x) do {if(!(x))throw std::runtime_error("Failed: " #x);}while(0)
int main(int argc,char** argv){try {
    if(argc==3&&std::string(argv[1])=="--native-compressed") {
        rank::Bytes compressed;CHECK(store::ReadFile(std::filesystem::absolute(argv[2]),compressed));
        rank::Bytes raw(0xc401);z_stream z{};CHECK(inflateInit2(&z,-13)==Z_OK);
        z.next_in=compressed.data();z.avail_in=static_cast<uInt>(compressed.size());z.next_out=raw.data();z.avail_out=static_cast<uInt>(raw.size());
        const auto result=inflate(&z,Z_FINISH);const auto size=z.total_out;const auto remaining=z.avail_in;inflateEnd(&z);
        CHECK(result==Z_STREAM_END&&remaining==0);raw.resize(size);CHECK(raw.size()>=48);
        // Native00865740 may append a parity-recovery bitmap and an8-byte
        // footer: original DDL size, 0x11292012. Only the DDL is CRC protected.
        std::uint32_t footer{},ddlSize{};std::memcpy(&footer,raw.data()+raw.size()-4,4);
        if(footer==0x11292012) {
            std::memcpy(&ddlSize,raw.data()+raw.size()-8,4);CHECK(ddlSize>=48&&ddlSize<raw.size()-8);
            raw.resize(ddlSize);
        }
        const auto version=(std::uint32_t(raw[4])<<24)|(std::uint32_t(raw[5])<<16)|(std::uint32_t(raw[6])<<8)|raw[7];
        std::string error;
        if(!rank::ValidateRaw(raw,version,error)) {
            std::uint32_t stored{};std::memcpy(&stored,raw.data(),4);
            std::cerr<<error<<" size="<<raw.size()<<" flags="<<unsigned(raw[8])<<" stored="<<std::hex<<stored
                <<" calculated="<<crc32(0,raw.data()+4,static_cast<uInt>(raw.size()-4))<<std::dec<<"\n";
            auto crc=crc32(0,nullptr,0);
            for(std::size_t n=4;n<raw.size();++n){crc=crc32(crc,raw.data()+n,1);if(crc==stored)std::cerr<<"CRC matches prefix length="<<(n+1)<<"\n";}
            return 1;
        }
        CHECK(rank::Compress(raw,compressed));
        rank::Snapshot actual{0x01100001FFFFFFFEULL,version,raw,compressed},decoded;
        CHECK(rank::Unpack(rank::Pack(actual),decoded,error));CHECK(decoded.raw==raw);
        CHECK(rank::Compress(raw,actual.compressed));CHECK(rank::Unpack(rank::Pack(actual),decoded,error));CHECK(decoded.raw==raw);
        std::cout<<"Native engine-written DDL validated and recompressed without DDL data changes; version="<<version<<" DDL bytes="<<raw.size()<<" full source bytes="<<size<<"\n";return 0;
    }
    rank::Snapshot s;s.account=0x01100001FFFFFFFEULL;s.version=147;
    CHECK(store::ProfilePolicyOwner(0,0,s.account,false,false)==0);
    CHECK(store::ProfilePolicyOwner(1,s.account,s.account,true,false)==s.account);
    CHECK(store::ProfilePolicyOwner(1,s.account,s.account,false,false)==0);
    CHECK(store::ProfilePolicyOwner(1,s.account,s.account-1,true,false)==0);
    CHECK(store::ProfilePolicyOwner(2,0,s.account,false,false)==s.account);
    CHECK(store::ProfilePolicyOwner(2,0,0,false,false)==0);
    CHECK(store::ProfilePolicyOwner(3,s.account,0,false,true)==s.account);
    CHECK(store::ProfilePolicyOwner(3,s.account,0,false,false)==0);
    CHECK(store::ProfilePolicyOwner(3,0,0,false,true)==0);
    CHECK(rank::ReadyForCapture(s.account,2,10,1,1,1,1));
    CHECK(!rank::ReadyForCapture(s.account,2,10,1,1,0,1));
    CHECK(!rank::ReadyForCapture(s.account,2,10,1,0,1,1));
    CHECK(!rank::ReadyForCapture(s.account,2,10,0,1,1,1));
    CHECK(!rank::ReadyForCapture(s.account,1,10,1,1,1,1));
    CHECK(!rank::ReadyForCapture(s.account,2,9,1,1,1,1));
    CHECK(!rank::ReadyForCapture(s.account,2,10,1,1,1,0));
    CHECK(!rank::ReadyForCapture(0,2,10,1,1,1,1));
    s.raw.resize(512);for(std::size_t n=9;n<s.raw.size();++n)s.raw[n]=static_cast<std::uint8_t>(n);
    s.raw[7]=147;const auto crc=static_cast<std::uint32_t>(crc32(0,s.raw.data()+4,s.raw.size()-4));
    std::memcpy(s.raw.data(),&crc,4);std::string error;
    CHECK(rank::ValidateRaw(s.raw,147,error));CHECK(!rank::ValidateRaw(s.raw,146,error));
    auto corrupt=s.raw;corrupt[70]^=1;CHECK(!rank::ValidateRaw(corrupt,147,error));
    corrupt=s.raw;corrupt[8]|=1;CHECK(!rank::ValidateRaw(corrupt,147,error));
    CHECK(rank::Compress(s.raw,s.compressed));auto bundle=rank::Pack(s);CHECK(!bundle.empty());
    if(argc==3&&std::string(argv[1])=="--workflow-fixture") {
        const auto path=std::filesystem::absolute(argv[2]);CHECK(!std::filesystem::exists(path));
        CHECK(store::WriteFile(path,bundle));std::cout<<"Synthetic isolated-workflow test fixture written\n";return 0;
    }
    rank::Snapshot read;CHECK(rank::Unpack(bundle,read,error));CHECK(read.account==s.account&&read.raw==s.raw&&read.compressed==s.compressed);
    for(std::size_t n=0;n<bundle.size();++n){auto truncated=rank::Bytes(bundle.begin(),bundle.begin()+n);CHECK(!rank::Unpack(truncated,read,error));}
    for(auto at:{0u,8u,16u,20u,24u,28u,70u}){auto bad=bundle;bad[at]^=1;CHECK(!rank::Unpack(bad,read,error));}
    auto bad=bundle;bad.back()^=1;CHECK(!rank::Unpack(bad,read,error));
    s.compressed[0]^=1;bad=rank::Pack(s);CHECK(!rank::Unpack(bad,read,error)); // valid bundle hash, invalid DEFLATE
    CHECK(!rank::ValidAccount(0));CHECK(!rank::ValidAccount(0x0110000100000000ULL));
    CHECK(!rank::ValidAccount(0x1234000100000001ULL));CHECK(rank::ValidAccount(0x0110000100000001ULL));
    CHECK(rank::Sha256(rank::Bytes{'a','b','c'})=="BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD");
    wchar_t temp[32768]{};CHECK(GetTempPathW(32768,temp));
    auto root=std::filesystem::path(temp)/(L"bo2-rank-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    auto profile=root/L"profiles"/L"01100001FFFFFFFE";rank::Bytes marker(8);std::memcpy(marker.data(),&s.account,8);
    CHECK(!store::ImportedAccountAvailable(root,s.account));
    CHECK(store::WriteFile(profile/L"import-account.bin",marker));
    CHECK(!store::ImportedAccountAvailable(root,s.account));
    CHECK(store::WriteFile(profile/L"t6"/L"zmstatsCompressed",rank::Bytes{1,2}));
    CHECK(store::ImportedAccountAvailable(root,s.account));CHECK(!store::ImportedAccountAvailable(root,s.account-1));
    marker[0]^=1;CHECK(store::WriteFile(profile/L"import-account.bin",marker));CHECK(!store::ImportedAccountAvailable(root,s.account));
    // Delete only explicitly named test artifacts; never recurse through user data.
    CHECK(DeleteFileW((profile/L"import-account.bin").c_str()));CHECK(DeleteFileW((profile/L"import-account.bin.bak").c_str()));
    CHECK(DeleteFileW((profile/L"t6"/L"zmstatsCompressed").c_str()));CHECK(RemoveDirectoryW((profile/L"t6").c_str()));
    CHECK(RemoveDirectoryW(profile.c_str()));CHECK(RemoveDirectoryW((root/L"profiles").c_str()));CHECK(RemoveDirectoryW(root.c_str()));
    // Real storage lookup regression: imported accounts do not inherit the
    // machine-wide legacy recovery file. These files are in the TEST binary's
    // data root, never in the installed game's directory.
    auto isolated=store::DataRoot()/L"profiles"/L"01100001FFFFFFFD";
    const std::uint64_t account=0x01100001FFFFFFFDULL;
    const auto name="rank-legacy-test-"+std::to_string(GetCurrentProcessId());
    const auto legacy=store::DataRoot()/L"user"/name;
    CHECK(!std::filesystem::exists(isolated));CHECK(!std::filesystem::exists(legacy));
    rank::Bytes accountMarker(8);std::memcpy(accountMarker.data(),&account,8);
    CHECK(store::WriteFile(isolated/L"t6"/L"zmstatsCompressed",rank::Bytes{1}));
    CHECK(store::WriteFile(isolated/L"import-account.bin",accountMarker));
    CHECK(store::WriteFile(legacy,rank::Bytes{9}));
    const auto previous=store::g_localUserId;store::g_localUserId=account;
    rank::Bytes lookup;CHECK(!store::ReadUserFile(name,lookup));
    CHECK(DeleteFileW((isolated/L"import-account.bin").c_str()));
    CHECK(store::ReadUserFile(name,lookup)&&lookup==rank::Bytes{9});store::g_localUserId=previous;
    CHECK(DeleteFileW(legacy.c_str()));CHECK(DeleteFileW((isolated/L"t6"/L"zmstatsCompressed").c_str()));
    CHECK(RemoveDirectoryW((isolated/L"t6").c_str()));CHECK(RemoveDirectoryW(isolated.c_str()));
    std::cout<<"Rank bundle, CRC, compression, corruption, account-isolation tests passed\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
