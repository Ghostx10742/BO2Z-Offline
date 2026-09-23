#include "offline_session.h"
#include "offline_storage.h"
#include "log.h"
#include <windows.h>
#include <bcrypt.h>
#include <atomic>
#include <cmath>

namespace bo2lan::offline {
namespace {
bool ReadUnset(ByteBuffer& input) {
    //009DFEF0 emits type20 (0x14). Type0 is the distinct task terminator.
    if(!input.More() || input.PeekType()!=20)return false;
    std::uint8_t tag{};return input.ReadRaw(&tag,1);
}
bool ReadNullableI32(ByteBuffer& input,std::int32_t& value) {
    if(ReadUnset(input)){value=-1;return true;}
    return input.ReadI32(value);
}
bool ReadNullableU32(ByteBuffer& input,std::uint32_t& value) {
    if(ReadUnset(input)){value=UINT32_MAX;return true;}
    return input.ReadU32(value);
}
bool ReadNullableU64(ByteBuffer& input,std::uint64_t& value) {
    if(ReadUnset(input)){value=UINT64_MAX;return true;}
    return input.ReadU64(value);
}
bool ReadNullableFloat(ByteBuffer& input,float& value) {
    //009DFFD0 compares to the 0x4F000000 float at00B3C180.
    if(ReadUnset(input)){value=2147483648.0f;return true;}
    return input.ReadFloat(value) && std::isfinite(value);
}
bool ReadId(ByteBuffer& input,std::uint64_t& id) {
    Bytes blob;if(!input.ReadBlob(blob)||blob.size()!=8)return false;
    std::memcpy(&id,blob.data(),8);return id!=0;
}
}
bool LocalSessions::Decode(ByteBuffer& input,std::uint8_t padding,Record& record) {
    // Native005A6D80 extends009E1F30: address,U32,U32,U64,blob16,
    // eight nullable I32,float,four nullable I32. Do not store cipher padding
    // as session attributes, or acknowledge a truncated derived record.
    const auto reject=[&](const char* field) {
        static std::atomic<unsigned> failures{};
        const auto count=++failures;
        if(count<=8 || (count&(count-1))==0)
            Log("offline-session: invalid field=%s remaining=%u nextType=%u count=%u (no key/address data dumped)",
                field,static_cast<unsigned>(input.Remaining()),input.PeekType(),count);
        return false;
    };
    if(!input.ReadBlob(record.address)||record.address.empty()||record.address.size()>256)return reject("address");
    if(!ReadNullableU32(input,record.gameType))return reject("gameType");
    if(!ReadNullableU32(input,record.maximumPlayers)||
        (record.maximumPlayers!=UINT32_MAX && record.maximumPlayers>64))return reject("maximumPlayers");
    if(!ReadNullableU64(input,record.owner))return reject("owner");
    if(!input.ReadBlob(record.security)||record.security.size()!=16)return reject("security-size");
    for(unsigned i=0;i<8;++i)if(!ReadNullableI32(input,record.attributes[i]))return reject("attributes-before-skill");
    if(!ReadNullableFloat(input,record.skill))return reject("skill");
    for(unsigned i=8;i<12;++i)if(!ReadNullableI32(input,record.attributes[i]))return reject("attributes-after-skill");
    return input.IsTaskEnd(padding) || reject("task-end");
}
ServiceReply LocalSessions::Handle(std::uint8_t operation,ByteBuffer request,std::uint8_t padding) {
    if(operation==1) {
        Record record;
        if(records_.size()>=16 || !Decode(request,padding,record))return {4,{}};
        std::uint64_t id{};
        do {
            if(BCryptGenRandom(nullptr,reinterpret_cast<PUCHAR>(&id),sizeof(id),
                BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)return {2,{}};
        }while(!id || records_.count(id));
        records_.emplace(id,std::move(record));
        Bytes blob(8);std::memcpy(blob.data(),&id,8);
        ByteBuffer result;result.WriteBlob(blob);
        Log("offline-session: native create accepted records=%u",static_cast<unsigned>(records_.size()));
        return {0,{result.Data()}};
    }
    if(operation==2 || operation==3) {
        std::uint64_t id{};
        if(!ReadId(request,id))return {4,{}};
        const auto found=records_.find(id);
        if(found==records_.end())return {4,{}};
        if(operation==2) {
            Record replacement;
            if(!Decode(request,padding,replacement))return {4,{}};
            found->second=std::move(replacement);
        }else {
            if(!request.IsTaskEnd(padding))return {4,{}};
            records_.erase(found);
        }
        Log("offline-session: native %s accepted records=%u",operation==2?"update":"delete",
            static_cast<unsigned>(records_.size()));
        return {};
    }
    if(operation==5) {
        std::uint32_t query{},offset{},maximum{};
        if(!ReadNullableU32(request,query)||!ReadNullableU32(request,offset)||!ReadNullableU32(request,maximum))return {4,{}};
        // No Internet/peer directory. Consume the game's bounded filter fields
        // rather than silently accepting arbitrary trailing bytes.
        unsigned count=0;
        while(!request.IsTaskEnd(padding) && ++count<=64) {
            switch(request.PeekType()) {
            case 20: case 7: {std::int32_t value{};if(!ReadNullableI32(request,value))return {4,{}};break;}
            case 8: {std::uint32_t value{};if(!request.ReadU32(value))return {4,{}};break;}
            case 10: {std::uint64_t value{};if(!request.ReadU64(value))return {4,{}};break;}
            case 13: {float value{};if(!request.ReadFloat(value)||!std::isfinite(value))return {4,{}};break;}
            default:return {4,{}};
            }
        }
        if(!request.IsTaskEnd(padding))return {4,{}};
        Log("offline-session: search type=%u offset=%u maximum=%u; local solo has no remote results",query,offset,maximum);
        return {};
    }
    return {4,{}};
}

ServiceReply ReadLocalKeyArchive(ByteBuffer request,std::uint8_t padding) {
    //009EC170 request;009F3BA0 reply: unsigned16 key + signed64 value.
    std::uint64_t owner{};std::uint16_t category{};bool mode{};
    if(!request.ReadU64(owner)||!request.ReadU16(category)||!request.ReadBool(mode))return {4,{}};
    std::vector<std::uint16_t> keys;
    while(!request.IsTaskEnd(padding) && keys.size()<20) {
        std::uint16_t key{};if(!request.ReadU16(key))return {4,{}};
        keys.push_back(key);
    }
    if(!request.IsTaskEnd(padding))return {4,{}};
    // This offline realm has no cloud archive. Native constructors initialize
    // these counters to zero. Persisted local values, if present, take priority;
    // no remote progress, content entitlements, or rankings are invented.
    ServiceReply reply;
    for(auto key:keys) {
        std::int64_t value{};Bytes stored;
        const auto name="archive-"+std::to_string(category)+"-"+std::to_string(key)+".bin";
        if(ReadUserFile(name,stored,owner,"offline-key-archive")) {
            ByteBuffer reader(stored);
            if(!reader.ReadI64(value)||reader.More())return {2,{}};
        }
        ByteBuffer result;result.WriteU16(key);result.WriteScalar(9,value);
        reply.objects.push_back(result.Data());
    }
    Log("offline-archive: read category=%u keys=%u mode=%d (local values, no cloud archive)",
        category,static_cast<unsigned>(keys.size()),mode);
    return reply;
}
}
