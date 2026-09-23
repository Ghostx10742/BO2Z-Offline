#include "rank_profile.h"
#include <windows.h>
#include <bcrypt.h>
#include <zlib.h>
#include <array>
#include <algorithm>
#include <cstring>
namespace bo2lan::rank {
namespace {
template<class T>void Append(Bytes& b,T x){auto p=reinterpret_cast<const std::uint8_t*>(&x);b.insert(b.end(),p,p+sizeof x);}
template<class T>T At(const Bytes& b,std::size_t p){T x{};std::memcpy(&x,b.data()+p,sizeof x);return x;}
std::array<std::uint8_t,32> Digest(const Bytes& b) {
    std::array<std::uint8_t,32> result{};BCRYPT_ALG_HANDLE alg{};BCRYPT_HASH_HANDLE hash{};
    if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return result;
    bool ok=BCryptCreateHash(alg,&hash,nullptr,0,nullptr,0,0)>=0;
    if(ok)ok=BCryptHashData(hash,const_cast<PUCHAR>(b.data()),static_cast<ULONG>(b.size()),0)>=0 &&
        BCryptFinishHash(hash,result.data(),static_cast<ULONG>(result.size()),0)>=0;
    if(hash)BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(alg,0);
    if(!ok)result.fill(0);return result;
}
bool EmptyDigest(const std::array<std::uint8_t,32>& d){return std::all_of(d.begin(),d.end(),[](auto b){return b==0;});}
}
bool ValidAccount(std::uint64_t id){return (id>>32)==0x01100001ULL && static_cast<std::uint32_t>(id)!=0;}
bool ReadyForCapture(std::uint64_t account,int signIn,int live,std::uint8_t fetched,
    std::uint8_t checksumValid,std::uint8_t ddlValid,std::uint8_t received) {
    return ValidAccount(account)&&signIn==2&&live==10&&fetched==1&&checksumValid==1&&ddlValid==1&&received==1;
}
bool ValidateRaw(const Bytes& b,std::uint32_t version,std::string& error) {
    if(b.size()<48 || b.size()>0xc400){error="Invalid native DDL size";return false;}
    const auto storedVersion=(std::uint32_t(b[4])<<24)|(std::uint32_t(b[5])<<16)|(std::uint32_t(b[6])<<8)|b[7];
    if(!version || storedVersion!=version){error="DDL version mismatch";return false;}
    if(b[8]&3){error="Stats still dirty or invalid; return to the lobby and retry";return false;}
    // Native00854E60/007C6920: standard CRC32 over DDL bytes after the checksum.
    if(At<std::uint32_t>(b,0)!=crc32(0,b.data()+4,static_cast<uInt>(b.size()-4))){error="Native stats checksum mismatch; no import performed";return false;}
    return true;
}
bool Compress(const Bytes& raw,Bytes& out) {
    z_stream z{};
    // Native006C54C0: raw DEFLATE, windowBits=-13, level9, memLevel1.
    if(deflateInit2(&z,9,Z_DEFLATED,-13,1,Z_DEFAULT_STRATEGY)!=Z_OK)return false;
    out.resize(compressBound(static_cast<uLong>(raw.size()))+1024);
    z.next_in=const_cast<Bytef*>(raw.data());z.avail_in=static_cast<uInt>(raw.size());
    z.next_out=out.data();z.avail_out=static_cast<uInt>(out.size());
    const bool ok=deflate(&z,Z_FINISH)==Z_STREAM_END;const auto size=z.total_out;
    deflateEnd(&z);if(!ok){out.clear();return false;}out.resize(size);return true;
}
std::string Sha256(const Bytes& bytes) {
    const auto digest=Digest(bytes);if(EmptyDigest(digest))return {};
    constexpr char hex[]="0123456789ABCDEF";std::string out;
    for(auto b:digest){out+=hex[b>>4];out+=hex[b&15];}return out;
}
Bytes Pack(const Snapshot& s) {
    std::string error;if(!ValidAccount(s.account)||!ValidateRaw(s.raw,s.version,error)||s.compressed.size()>0x20000)return {};
    Bytes out{'B','O','2','R','A','N','K','1'};Append(out,s.account);Append(out,s.version);
    Append(out,static_cast<std::uint32_t>(s.raw.size()));Append(out,static_cast<std::uint32_t>(s.compressed.size()));
    out.insert(out.end(),s.raw.begin(),s.raw.end());out.insert(out.end(),s.compressed.begin(),s.compressed.end());
    const auto digest=Digest(out);if(EmptyDigest(digest))return {};
    out.insert(out.end(),digest.begin(),digest.end());return out;
}
bool Unpack(const Bytes& bytes,Snapshot& s,std::string& error) {
    s={};
    if(bytes.size()<60||bytes.size()>0x30000||std::memcmp(bytes.data(),"BO2RANK1",8)){error="Not a rank-import bundle";return false;}
    const auto rawSize=At<std::uint32_t>(bytes,20),compressedSize=At<std::uint32_t>(bytes,24);
    if(rawSize<48||rawSize>0xc400||!compressedSize||compressedSize>0x20000||bytes.size()!=60ULL+rawSize+compressedSize){error="Invalid bundle lengths";return false;}
    Bytes content(bytes.begin(),bytes.end()-32);auto hash=Digest(content);
    if(EmptyDigest(hash)||!std::equal(hash.begin(),hash.end(),bytes.end()-32)){error="Import bundle checksum failed";return false;}
    s.account=At<std::uint64_t>(bytes,8);s.version=At<std::uint32_t>(bytes,16);
    s.raw.assign(bytes.begin()+28,bytes.begin()+28+rawSize);
    s.compressed.assign(bytes.begin()+28+rawSize,bytes.end()-32);
    if(!ValidAccount(s.account)||!ValidateRaw(s.raw,s.version,error))return false;
    Bytes restored(rawSize+1);z_stream z{};if(inflateInit2(&z,-13)!=Z_OK){error="Inflater initialization failed";return false;}
    z.next_in=s.compressed.data();z.avail_in=compressedSize;z.next_out=restored.data();z.avail_out=static_cast<uInt>(restored.size());
    bool ok=inflate(&z,Z_FINISH)==Z_STREAM_END&&z.total_out==rawSize&&z.avail_in==0;
    inflateEnd(&z);restored.resize(rawSize);
    if(!ok||restored!=s.raw){error="Compressed profile does not match verified raw stats";return false;}
    return true;
}
}
