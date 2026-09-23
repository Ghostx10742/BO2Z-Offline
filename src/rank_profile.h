#pragma once
#include <cstdint>
#include <vector>
#include <string>
namespace bo2lan::rank {
using Bytes=std::vector<std::uint8_t>;
struct Snapshot {std::uint64_t account{};std::uint32_t version{};Bytes raw,compressed;};
bool ValidAccount(std::uint64_t);
bool ReadyForCapture(std::uint64_t account,int signIn,int live,std::uint8_t fetched,
    std::uint8_t checksumValid,std::uint8_t ddlValid,std::uint8_t received);
bool ValidateRaw(const Bytes&,std::uint32_t,std::string&);
Bytes Pack(const Snapshot&);
bool Unpack(const Bytes&,Snapshot&,std::string&);
bool Compress(const Bytes&,Bytes&);
std::string Sha256(const Bytes&);
}
