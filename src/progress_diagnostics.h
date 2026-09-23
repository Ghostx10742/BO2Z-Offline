#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <cstdint>
namespace bo2lan::diagnostics {
// One automatic, bounded read-only capture per game launch. No import,
// profile selection, game function calls, network requests or memory writes.
void Start(bool offlineBackendConfigured,bool onlineCapture=false) noexcept;
bool Candidate(const std::filesystem::path&);
bool DecodeProfile(const std::vector<std::uint8_t>& input,std::vector<std::uint8_t>& ddl,std::string& format,std::string& reason);
#ifdef OFFLINE_DIAGNOSTICS_TEST
bool CaptureSelfTest(const std::filesystem::path& isolatedRoot);
#endif
}
