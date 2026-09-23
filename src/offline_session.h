#pragma once
#include "offline_protocol.h"
#include <map>

namespace bo2lan::offline {
struct ServiceReply {
    std::uint32_t error{};
    std::vector<Bytes> objects;
};
// Owned by one lobby worker. These are backend records, not game parties.
// Only a native create/update/delete request can change them.
class LocalSessions {
public:
    ServiceReply Handle(std::uint8_t operation, ByteBuffer request, std::uint8_t padding);
    std::size_t Size() const { return records_.size(); }
private:
    struct Record {
        Bytes address, security;
        std::uint32_t gameType{}, maximumPlayers{};
        std::uint64_t owner{};
        std::array<std::int32_t,12> attributes{};
        float skill{};
    };
    static bool Decode(ByteBuffer&,std::uint8_t,Record&);
    std::map<std::uint64_t,Record> records_;
};
ServiceReply ReadLocalKeyArchive(ByteBuffer request,std::uint8_t padding);
}
