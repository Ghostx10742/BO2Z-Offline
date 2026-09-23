#include "progress_diagnostics.h"
#include "progress_mode.h"
#include "rank_profile.h"
#include "offline_storage.h"
#include <zlib.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <windows.h>
using bo2lan::offline::Bytes;
namespace d=bo2lan::diagnostics;
#define CHECK(x) do{if(!(x))throw std::runtime_error("Failed: " #x);}while(0)
static unsigned observed{};
static void Observe(const char*,const std::filesystem::path&,const Bytes&,std::uint64_t) noexcept {++observed;}
int main(int argc,char** argv) {try {
    CHECK(!d::UseLocalServices(true,true));CHECK(!d::UseLocalServices(false,true));
    CHECK(d::UseLocalServices(true,false));CHECK(!d::UseLocalServices(false,false));
    CHECK(d::Candidate(L"zmstatsCompressed"));CHECK(d::Candidate(L"zmdatabk0000"));CHECK(d::Candidate(L"user_zm.cgp"));
    CHECK(d::Candidate(L"USER_ZM.CGP"));CHECK(d::Candidate(L"remotecache.vdf"));
    CHECK(!d::Candidate(L"zmshot0001.jpg"));CHECK(!d::Candidate(L"bank.dll"));CHECK(!d::Candidate(L"stats.log"));
    CHECK(!d::Candidate(L"steam_api.dll"));
    CHECK(d::Candidate(L"1038aabc0099"));CHECK(d::Candidate(L"unknown-progress.cache"));
    Bytes raw(512,0);raw[7]=54;for(unsigned i=48;i<512;++i)raw[i]=static_cast<std::uint8_t>(i);
    auto crc=static_cast<std::uint32_t>(crc32(0,raw.data()+4,raw.size()-4));std::memcpy(raw.data(),&crc,4);
    Bytes decoded,compressed;std::string format,reason;
    CHECK(d::DecodeProfile(raw,decoded,format,reason)&&decoded==raw&&format=="native-raw-DDL");
    CHECK(bo2lan::rank::Compress(raw,compressed));CHECK(d::DecodeProfile(compressed,decoded,format,reason)&&decoded==raw&&format=="raw-DEFLATE-DDL");
    auto parity=raw;parity.resize(raw.size()+32);std::uint32_t length=raw.size(),magic=0x11292012;
    std::memcpy(parity.data()+parity.size()-8,&length,4);std::memcpy(parity.data()+parity.size()-4,&magic,4);
    CHECK(bo2lan::rank::Compress(parity,compressed));CHECK(d::DecodeProfile(compressed,decoded,format,reason)&&decoded==raw);
    auto invalid=raw;invalid[20]^=1;CHECK(!d::DecodeProfile(invalid,decoded,format,reason));
    invalid=raw;invalid[8]|=1;CHECK(!d::DecodeProfile(invalid,decoded,format,reason));
    CHECK(!d::DecodeProfile(Bytes{},decoded,format,reason));CHECK(!d::DecodeProfile(Bytes(3*1024*1024),decoded,format,reason));
    CHECK(bo2lan::rank::Compress(Bytes(256*1024),compressed));CHECK(!d::DecodeProfile(compressed,decoded,format,reason)); // bounded inflation
    bo2lan::offline::SetStorageObserver(&Observe);
    bo2lan::offline::ObserveProgressRequest(28,1,raw);CHECK(observed==0); // no authentication payload capture
    bo2lan::offline::ObserveProgressRequest(4,1,raw);bo2lan::offline::ObserveProgressRequest(8,3,raw);bo2lan::offline::ObserveProgressRequest(10,1,raw);CHECK(observed==3);
    bo2lan::offline::SetStorageObserver(nullptr);bo2lan::offline::ObserveProgressRequest(10,1,raw);CHECK(observed==3);
    if(argc==2){Bytes actual;CHECK(bo2lan::offline::ReadFile(std::filesystem::absolute(argv[1]),actual));CHECK(d::DecodeProfile(actual,decoded,format,reason));std::cout<<"Real native profile decoded, "<<decoded.size()<<" DDL bytes\n";}
    const auto isolated=bo2lan::offline::ExecutableDirectory()/("diagnostic-selftest-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    CHECK(d::CaptureSelfTest(isolated));std::cout<<"Real scanner/observer/blob capture verified; source file unchanged\n";
    std::cout<<"Candidate filtering, raw/DEFLATE, parity, CRC, dirty flag, bounded inflation and observer routing passed\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
