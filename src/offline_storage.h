#pragma once
#include "offline_protocol.h"
#include <filesystem>
#include <optional>
namespace bo2lan::offline {
extern std::uint64_t g_localUserId;
std::uint64_t StableId(const std::string&);
std::filesystem::path ExecutableDirectory();
std::filesystem::path DataRoot();
std::string SafeName(const std::string&);
bool ReadFile(const std::filesystem::path&,Bytes&);
bool WriteFile(const std::filesystem::path&,const Bytes&);
void InitializeLocalIdentity();
bool ImportedAccountAvailable(const std::filesystem::path& root,std::uint64_t nativeAccount);
void SelectImportedAccount(std::uint64_t nativeAccount);
// Setup choice is never shipped with releases. 1=import, 2=new separate
// per-Steam profile (explicit consent), 3=retain an existing legacy profile.
bool ConfigureProfilePolicy();
// 2.05 default: fresh independent offline namespace, selected by native Steam
// identity before authentication. No import, legacy fallback or setup file.
void ConfigureIsolatedProfilePolicy();
#ifdef OFFLINE_STORAGE_TEST
void SetTestDataRoot(const std::filesystem::path&);
#endif
std::uint64_t ProfilePolicyOwner(unsigned mode,std::uint64_t chosen,std::uint64_t native,bool imported,bool legacyExists);
bool SelectPolicyAccount(std::uint64_t nativeAccount);
bool ReadUserFile(const std::string&,Bytes&,std::uint64_t owner=0,const std::string& title="");
bool WriteUserFile(const std::string&,const Bytes&,std::uint64_t owner=0,const std::string& title="");
using StorageObserver=void(*)(const char* event,const std::filesystem::path&,const Bytes&,std::uint64_t owner) noexcept;
void SetStorageObserver(StorageObserver);
void ObserveProgressRequest(unsigned service,unsigned operation,const Bytes&);
std::optional<std::filesystem::path> PublisherPath(std::string);
void SerializeFileData(ByteBuffer&,const Bytes&);
void SerializeFileInfo(ByteBuffer&,const std::string&,std::size_t,std::uint64_t owner=0,bool visible=false);
}
