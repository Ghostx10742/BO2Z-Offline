#include "offline_services.h"
#include "offline_storage.h"
#include "offline_auth.h"
#include "offline_session.h"
#include "log.h"
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <ctime>
#include <thread>
namespace bo2lan::offline {
constexpr std::uint32_t kNoFile=0x3e8;
namespace { std::atomic<void(*)()> wakeCallback{}; void Wake(){if(auto f=wakeCallback.load())f();} }
void SetServiceWakeCallback(void(*f)()){wakeCallback=f;}

class LocalServer::Impl {
public:
    explicit Impl(ServerKind kind,bool worker):kind_(kind),workerMode_(worker) {
        if(workerMode_)worker_=std::thread([this]{
            for(;;){
                Bytes chunk;
                {std::unique_lock lock(pendingMutex_);
                 incoming_.wait(lock,[&]{return failed_ || !pending_.empty();});
                 if(failed_)return;
                 chunk=std::move(pending_.front());pending_.pop_front();pendingBytes_-=chunk.size();}
                Process(chunk.data(),chunk.size());
            }
        });
    }
    ~Impl(){Close();if(worker_.joinable())worker_.join();}
    int Send(const char* data,int length) {
        if(length<0 || (!data&&length)){WSASetLastError(WSAEFAULT);return SOCKET_ERROR;}
        if(failed_){WSASetLastError(WSAECONNRESET);return SOCKET_ERROR;}
        if(!length)return 0;
        if(workerMode_){
            std::lock_guard lock(pendingMutex_);
            if(failed_ || pendingBytes_+static_cast<std::size_t>(length)>4*1024*1024){WSASetLastError(WSAENOBUFS);return SOCKET_ERROR;}
            try{pending_.emplace_back(data,data+length);}
            catch(const std::exception&){WSASetLastError(WSAENOBUFS);return SOCKET_ERROR;}
            pendingBytes_+=length;incoming_.notify_one();
        }else Process(reinterpret_cast<const std::uint8_t*>(data),length);
        if(failed_){WSASetLastError(WSAECONNRESET);return SOCKET_ERROR;}
        return length;
    }
    void Process(const std::uint8_t* data,std::size_t length) {
        if(failed_)return;
        if(input_.size()+length>4*1024*1024+4){Fail("input queue limit");return;}
        try{input_.insert(input_.end(),data,data+length);ParseFrames();}
        catch(const std::exception& e){Log("offline-service: exception: %s",e.what());Fail("service exception");}
        buffered_=input_.size();Wake();
    }

    int Receive(char* destination, int capacity, int flags = 0, bool blocking = false) {
        if (capacity == 0) return 0;
        if (!destination || capacity < 0) { WSASetLastError(WSAEFAULT); return SOCKET_ERROR; }
        if (flags & ~MSG_PEEK) { WSASetLastError(WSAEOPNOTSUPP); return SOCKET_ERROR; }
        std::unique_lock lock(mutex_);
        if (blocking) available_.wait(lock,[&]{return !output_.empty() || failed_;});
        if (output_.empty()) {
            WSASetLastError(failed_ ? WSAECONNRESET : WSAEWOULDBLOCK);
            return SOCKET_ERROR;
        }
        const auto count = std::min<std::size_t>(capacity, output_.size());
        for (std::size_t i = 0; i < count; ++i) {
            destination[i] = static_cast<char>(output_[i]);
        }
        if (!(flags & MSG_PEEK)) output_.erase(output_.begin(), output_.begin() + count);
        if (++receiveCalls_ <= 16) Log("lan-wire: %s RX bytes=%u queued=%u peek=%d",
            kind_ == ServerKind::Auth ? "auth" : "lobby", static_cast<unsigned>(count),
            static_cast<unsigned>(output_.size()), (flags & MSG_PEEK) != 0);
        return static_cast<int>(count);
    }

    bool Readable() const {
        std::lock_guard lock(mutex_);
        return !output_.empty() || failed_;
    }
    std::size_t Available() const { std::lock_guard lock(mutex_); return output_.size(); }
    bool Ready() const { std::lock_guard lock(mutex_); return phase_ == Phase::Services && !failed_; }
    bool Failed() const { std::lock_guard lock(mutex_); return failed_; }
    void Close() {
        failed_=true;
        {std::lock_guard lock(mutex_);output_.clear();}
        {std::lock_guard lock(pendingMutex_);pending_.clear();pendingBytes_=0;}
        available_.notify_all();incoming_.notify_all();Wake();
    }
    void Dump(SOCKET socket) const {
        std::lock_guard lock(mutex_);
        Log("offline-snapshot: socket=%llu kind=%s phase=%u failed=%d input=%u output=%u requests=%u",
            static_cast<unsigned long long>(socket),kind_==ServerKind::Auth?"auth":"lobby",
            static_cast<unsigned>(phase_.load()),failed_.load(),static_cast<unsigned>(buffered_.load()),
            static_cast<unsigned>(output_.size()),requests_.load());
    }

private:
    void Fail(const char* reason) {
        if(!failed_.exchange(true))Log("offline-service: FAILED kind=%s phase=%u reason=%s",
            kind_==ServerKind::Auth?"auth":"lobby",static_cast<unsigned>(phase_.load()),reason);
        {std::lock_guard lock(mutex_);output_.clear();}
        available_.notify_all();incoming_.notify_all();Wake();
    }
    void Queue(const Bytes& bytes) {
        bool overflow=false;
        {std::lock_guard lock(mutex_);
         if(failed_)return;
         overflow=bytes.size()>20*1024*1024 || output_.size()>20*1024*1024-bytes.size();
         if(!overflow)output_.insert(output_.end(),bytes.begin(),bytes.end());}
        if(overflow)Fail("output queue limit");
        available_.notify_all();Wake();
    }

    void SendUnencrypted(std::uint8_t messageType, const Bytes& payload) {
        Bytes packet;
        const auto size = static_cast<std::int32_t>(payload.size() + 2);
        AppendRaw(packet, size);
        packet.push_back(0);
        packet.push_back(messageType);
        packet.insert(packet.end(), payload.begin(), payload.end());
        Queue(packet);
    }

    void SendEncrypted(std::uint8_t messageType, const Bytes& payload) {
        Bytes plain;
        const std::uint32_t checksum = 0xDEADBEEF;
        AppendRaw(plain, checksum);
        plain.push_back(messageType);
        plain.insert(plain.end(), payload.begin(), payload.end());
        plain.resize((plain.size() + 7) & ~std::size_t(7), 0);

        const std::uint32_t seed = 0x13371337;
        const auto iv = Tiger(&seed, sizeof(seed));
        const auto encrypted = TripleDes(plain, iv, encryptKey_, true);
        if (encrypted.empty()) return;

        Bytes packet;
        const auto size = static_cast<std::int32_t>(encrypted.size() + 5);
        AppendRaw(packet, size);
        packet.push_back(1);
        AppendRaw(packet, seed);
        packet.insert(packet.end(), encrypted.begin(), encrypted.end());
        Queue(packet);
    }

    void SendService(std::uint8_t operation, std::uint32_t error,
                     const std::vector<Bytes>& objects = {}) {
        if(currentRequestLogged_ || error) {
            auto& errors=errorCounts_[(static_cast<unsigned>(currentService_)<<8)|operation];
            if(!error || ++errors<=4 || (errors&(errors-1))==0)
                Log("offline-result: service=%u operation=%u error=%u objects=%u errors=%u",
                    currentService_,operation,error,static_cast<unsigned>(objects.size()),errors);
        }
        static std::atomic<std::uint64_t> next{0x8000000000000001ULL};
        ByteBuffer body;
        body.WriteU64(++next);
        body.WriteU32(error);
        body.WriteU8(operation);
        if (!error) {
            body.WriteU32(static_cast<std::uint32_t>(objects.size()));
            if (!objects.empty()) {
                body.WriteU32(static_cast<std::uint32_t>(objects.size()));
                for (const auto& object : objects) body.WriteRaw(object.data(), object.size());
            }
        } else {
            body.WriteU64(next.load());
        }
        SendEncrypted(1, body.Data());
    }

    void ParseFrames() {
        while (!failed_ && input_.size() >= 4) {
            std::int32_t size{};
            std::memcpy(&size, input_.data(), 4);
            if (size == 0) {
                input_.erase(input_.begin(), input_.begin() + 4);
                Queue(Bytes{0, 0, 0, 0});
                continue;
            }
            if (kind_ == ServerKind::Lobby && phase_ == Phase::Handshake) {
                // Native 009E3740 writes [uint32 200][uint32 connection+0x10].
                // Preserve fragmented headers and any coalesced type-7 hello.
                if (size != 200) { Fail("invalid lobby handshake marker"); return; }
                if (input_.size() < 8) return;
                std::uint32_t parameter{};
                std::memcpy(&parameter, input_.data() + 4, 4);
                input_.erase(input_.begin(), input_.begin() + 8);
                phase_ = Phase::Hello;
                ByteBuffer id;
                id.WriteU64(0xFD);
                SendUnencrypted(4, id.Data());
                Log("lan-dw: eight-byte lobby handshake consumed parameter=%u; connection id issued", parameter);
                continue;
            }
            if (size < 0 || size > 4 * 1024 * 1024) {
                Log("lan-dw: rejected oversized frame %d", size);
                input_.clear();
                Fail("oversized frame");
                return;
            }
            if (input_.size() < static_cast<std::size_t>(size) + 4) return;
            Bytes frame(input_.begin() + 4, input_.begin() + 4 + size);
            input_.erase(input_.begin(), input_.begin() + 4 + size);
            ParseMessage(frame);
        }
    }

    void ParseMessage(Bytes frame) {
        if (frame.size() < 2) {
            Fail("short frame");
            return;
        }
        ByteBuffer packet(std::move(frame));
        packet.Types(false);
        bool encrypted{};
        if (!packet.ReadBool(encrypted)) return;
        if (encrypted) {
            if (kind_ != ServerKind::Lobby || phase_ != Phase::Services) {
                Fail("encrypted request before lobby hello"); return;
            }
            std::uint32_t seed{};
            if (!packet.ReadU32(seed) || packet.Remaining()==0 || packet.Remaining()%8) { Fail("invalid encrypted frame length"); return; }
            const auto iv = Tiger(&seed, sizeof(seed));
            auto plain = TripleDes(packet.Rest(), iv, decryptKey_, false);
            if (plain.size() < 5) { Fail("encrypted frame decrypt failed"); return; }
            // Native 009F7850 uses HMAC-SHA1(session key, plain[5..end])[:4].
            // Only server replies use DEADBEEF (native receive 009F7EC0).
            if(!ValidateClientTask(plain,decryptKey_)) {
                Fail("client HMAC-SHA1 mismatch; refusing corrupt task");return;
            }
            requestPadding_=static_cast<std::uint8_t>(seed);
            packet = ByteBuffer(std::move(plain));
            packet.Types(false);
            std::uint32_t checksum{};
            packet.ReadU32(checksum);
            // Already validated the client MAC above; skip its four bytes.
        } else if(kind_==ServerKind::Lobby && phase_==Phase::Services) {
            Fail("plaintext request after encrypted session start");return;
        }
        std::uint8_t type{};
        if (!packet.ReadU8(type)) { Log("lan-dw: frame missing message/service type"); return; }
        const auto data = packet.Rest();
        const auto messageCount=++messageCounts_[type];
        if(messageCount<=4 || (messageCount&(messageCount-1))==0)Log("lan-dw: message server=%s type=%u encrypted=%d bytes=%u",
            kind_ == ServerKind::Auth ? "auth" : "lobby", type, encrypted ? 1 : 0,
            static_cast<unsigned>(data.size()));
        if (kind_ == ServerKind::Auth) HandleAuth(type, data);
        else HandleLobby(type, data);
    }

    void HandleAuth(std::uint8_t type, const Bytes& data) {
        if (type != 28) {
            Log("lan-dw: unsupported auth message %u", type);
            Fail("unsupported auth message");
            return;
        }
        BitBuffer request(data);
        request.Types(false);
        bool more{};
        if (!request.ReadBool(more)) { Fail("auth envelope"); return; }
        request.Types(true);
        std::uint32_t seed{}, title{}, ticketSize{};
        if (!request.ReadU32(seed) || !request.ReadU32(title) || !request.ReadU32(ticketSize) ||
            ticketSize > 4096) { Fail("auth ticket header"); return; }
        Bytes ticket(ticketSize);
        if (!request.ReadBytes(ticket.data(), ticket.size()) || ticket.size() < 56) {
            Log("lan-dw: auth ticket malformed size=%u", static_cast<unsigned>(ticket.size()));
            Fail("malformed auth ticket");
            return;
        }

#pragma pack(push, 1)
        struct AuthTicket {
            std::uint32_t magic;
            std::uint8_t type;
            std::uint32_t title;
            std::uint32_t issued;
            std::uint32_t expires;
            std::uint64_t license;
            std::uint64_t user;
            char username[64];
            std::uint8_t sessionKey[24];
            std::uint8_t hashMagic[3];
            std::uint8_t hash[4];
        };
#pragma pack(pop)

        static_assert(sizeof(AuthTicket)==128);
        static_assert(offsetof(AuthTicket,sessionKey)==97);
        std::array<std::uint8_t,24> requestKey{},sessionKey{};
        std::memcpy(requestKey.data(),ticket.data()+32,24);
        if(!IssueLocalSession(requestKey,title,seed,sessionKey)) {
            Fail("auth request key/title invalid");return;
        }
        AuthTicket auth{};
        std::memset(&auth, 0x0A, sizeof(auth));
        auth.magic = 0xEFBDADDE;
        auth.type = 0;
        auth.title = title;
        auth.issued = static_cast<std::uint32_t>(std::time(nullptr));
        auth.expires = auth.issued + 365u * 24u * 60u * 60u;
        auth.user = g_localUserId;
        strcpy_s(auth.username, "BO2Z-Offline Player");
        std::memcpy(auth.sessionKey, sessionKey.data(), sessionKey.size());

        const auto iv = Tiger(&seed, sizeof(seed));
        Bytes authBytes(reinterpret_cast<std::uint8_t*>(&auth),
                        reinterpret_cast<std::uint8_t*>(&auth) + sizeof(auth));
        const auto encryptedTicket = TripleDes(authBytes, iv, requestKey, true);
        if (encryptedTicket.empty()) { Fail("auth ticket encryption failed"); return; }

        std::array<std::uint8_t, 128> lsgTicket{};
        std::memcpy(lsgTicket.data(), sessionKey.data(), sessionKey.size());
        BitBuffer response;
        // T6's bdAuth reply decoder leaves data types disabled for the entire
        // payload.  Only the request turns them back on after its leading
        // envelope bit.  Adding type tags here shifts the success code, seed,
        // encrypted auth ticket, and LSG ticket by ten bits; the client then
        // rejects the reply and closes the auth socket immediately.
        response.Types(false);
        response.WriteBool(false);
        response.WriteU32(700);
        response.WriteU32(seed);
        response.WriteBytes(encryptedTicket.data(), encryptedTicket.size());
        response.WriteBytes(lsgTicket.data(), lsgTicket.size());
        SendUnencrypted(29, response.Data());
        Log("lan-dw: PC auth reply queued title=%u result=700 untyped=1 payload=%u",
            title, static_cast<unsigned>(response.Data().size()));
    }

    void HandleLobby(std::uint8_t service, const Bytes& data) {
        if (service == 7) {
            if (phase_ != Phase::Hello) { Fail("unexpected lobby hello"); return; }
            BitBuffer hello(data);
            hello.Types(false);
            bool more{};
            if (!hello.ReadBool(more)) { Fail("short lobby hello"); return; }
            hello.Types(true);
            std::uint32_t title{}, seed{};
            std::array<std::uint8_t, 128> ticket{};
            if (hello.ReadU32(title) && hello.ReadU32(seed) &&
                hello.ReadBytes(ticket.data(), ticket.size())) {
                if(!ValidateLocalHello(title,seed,ticket)) {
                    Fail("lobby hello/native authentication disagreement");return;
                }
                std::copy_n(ticket.begin(), 24, encryptKey_.begin());
                decryptKey_ = encryptKey_;
                phase_ = Phase::Services;
                Log("lan-dw: LSG key established title=%u", title);
            } else { Fail("malformed lobby hello"); }
            return;
        }

        // Raw bandwidth-control messages are not bdRemoteTask requests.
        // BO2 009E5050 routes server message 5 separately (no task completion).
        if(service==18) {
            if(data.size()>64){Fail("oversized bandwidth control message");return;}
            SendEncrypted(5,{});
            Log("offline-service: bandwidth control handled separately from task queue");
            return;
        }
        ByteBuffer request(data);
        std::uint8_t operation{};
        if (!request.ReadU8(operation)) {
            Log("lan-dw: service=%u missing operation id bytes=%u", service,
                static_cast<unsigned>(data.size()));
            SendService(0,4);return;
        }
        ++requests_;
        ObserveProgressRequest(service,operation,data);
        currentService_=service;
        const auto count=++operationCounts_[(static_cast<unsigned>(service)<<8)|operation];
        currentRequestLogged_=count<=4 || (count&(count-1))==0;
        if(currentRequestLogged_)Log("lan-dw: service=%u operation=%u payload=%u count=%u", service, operation,
            static_cast<unsigned>(request.Remaining()),count);
        // Only bounded, non-secret query shapes. Never dump auth tickets,
        // session keys, save/profile blobs or general service payloads.
        if(count<=2 && ((service==81&&(operation==6||operation==8)) ||
            (service==8&&operation==1)||(service==23&&operation==2))) {
            const auto raw=request.Rest();std::string shape;
            const auto append=[&](std::size_t begin,std::size_t end){
                for(auto i=begin;i<end;++i){char hex[4]{};sprintf_s(hex,"%02X ",raw[i]);shape+=hex;}
            };
            append(0,std::min<std::size_t>(raw.size(),48));
            if(raw.size()>48){shape+="... ";append(raw.size()-16,raw.size());}
            Log("offline-query-shape: service=%u op=%u bytes=%u sequenceByte=%02X data=%s",
                service,operation,static_cast<unsigned>(raw.size()),requestPadding_,shape.c_str());
        }
        if(HandleStartupService(service,operation,request))return;
        switch (service) {
        case 4: HandleStats(operation,request); break;
        case 10: HandleStorage(operation, request); break;
        case 12: HandleTitleUtilities(operation); break;
        case 15: {
            const auto reply=operation==2?ReadLocalKeyArchive(request,requestPadding_):ServiceReply{4,{}};
            SendService(operation,reply.error,reply.objects);break;
        }
        case 21: HandleMatchmaking(operation, request); break;
        case 27: HandleDml(operation); break;
        case 28: HandleGroups(operation, request); break;
        default:
            if(currentRequestLogged_)Log("lan-dw: unsupported service %u/%u rejected (no fabricated result)", service, operation);
            SendService(operation, 4);
            break;
        }
    }

    bool HandleStartupService(std::uint8_t service,std::uint8_t operation,ByteBuffer& request) {
        // Local-only services. No report is sent to Activision/Steam, and no
        // native anti-cheat/VAC function or readiness flag is patched here.
        if(service==38 && operation==4) {
            Bytes identifier,networkId;std::uint32_t platform{},build{};std::uint64_t a{},b{},c{};
            const bool valid=request.ReadBlob(identifier)&&identifier.size()==16&&
                request.ReadU32(platform)&&request.ReadU32(build)&&request.ReadU64(a)&&
                request.ReadU64(b)&&request.ReadU64(c)&&request.ReadBlob(networkId)&&networkId.size()==6&&request.IsTaskEnd(requestPadding_);
            if(valid)consoleReportAccepted_=true;
            SendService(operation,valid?0:4);
            Log("offline-service: local console report accepted=%d (identifiers not logged)",valid);
            return true;
        }
        if(service==68 && operation==1) {
            std::uint64_t owner{};Bytes presence;
            const bool valid=request.ReadU64(owner)&&request.ReadBlob(presence)&&presence.size()<=512&&request.IsTaskEnd(requestPadding_);
            if(valid)localPresence_=std::move(presence);
            SendService(operation,valid?0:4);return true;
        }
        if(service==81 && operation==1) {
            // 009F0590: untyped U64 array; bdGenericLeagueID is one typed U64.
            // Offline profiles have no online league team, represented by ID 0.
            Bytes members;
            if(!request.ReadArray(10,8,members)||!request.IsTaskEnd(requestPadding_)){SendService(operation,4);return true;}
            ByteBuffer noTeam;noTeam.WriteU64(0);SendService(operation,0,{noTeam.Data()});return true;
        }
        if(service==81 && operation==2) {
            std::uint64_t user{};std::uint8_t filter{};std::uint32_t offset{},maximum{};
            const bool valid=request.ReadU64(user)&&request.ReadU8(filter)&&request.ReadU32(offset)&&
                request.ReadU32(maximum)&&request.IsTaskEnd(requestPadding_);
            // Native list query: no online league memberships in local solo.
            SendService(operation,valid?0:4);return true;
        }
        if(service==81 && (operation==6 || operation==8)) {
            Bytes ids;
            if(!request.ReadArray(10,8,ids)||!request.IsTaskEnd(requestPadding_)){
                if(currentRequestLogged_)Log("offline-league: malformed array operation=%u remaining=%u nextType=%u",operation,
                    static_cast<unsigned>(request.Remaining()),request.PeekType());
                SendService(operation,4);return true;
            }
            if(currentRequestLogged_)Log("offline-league: operation=%u requestedIds=%u",operation,static_cast<unsigned>(ids.size()/8));
            std::vector<Bytes> results;
            if(operation==8) {
                // 009F0040: team ID followed by parallel raw arrays of member
                // IDs and member names. Local offline teams have no members.
                for(std::size_t at=0;at<ids.size();at+=8) {
                    std::uint64_t id{};std::memcpy(&id,ids.data()+at,8);
                    ByteBuffer object;object.WriteU64(id);
                    for(const std::uint8_t type: {std::uint8_t(110),std::uint8_t(116)}) {
                        object.WriteRaw(&type,1);object.WriteU32(0);
                        const std::uint32_t count=0;object.WriteRaw(&count,4);
                    }
                    results.push_back(object.Data());
                }
            }
            // 81/6 is a team-info lookup. There are no online league teams to
            // return. Successful empty lookup completes the native UI event.
            SendService(operation,0,results);return true;
        }
        if(service==23 && operation==2) {
            // 009F3430 / bdCounterValue 009F35F0: U32 counter ID, signed I64 value.
            std::vector<Bytes> values;
            while(!request.IsTaskEnd(requestPadding_) && values.size()<4096) {
                std::uint32_t id{};if(!request.ReadU32(id)){SendService(operation,4);return true;}
                ByteBuffer value;value.WriteU32(id);value.WriteScalar<std::int64_t>(9,0);values.push_back(value.Data());
            }
            if(!request.IsTaskEnd(requestPadding_)){SendService(operation,4);return true;}
            SendService(operation,0,values);return true;
        }
        if(service==8 && operation==1) {
            // Public profile lookup, distinct from persistent Zombies stats.
            // No remote profile directory is populated in an offline session.
            unsigned count=0;std::vector<Bytes> results;
            while(!request.IsTaskEnd(requestPadding_)){
                std::uint64_t id{};if(!request.ReadU64(id)||++count>4096){SendService(operation,4);return true;}
                Bytes data;if(ReadUserFile("public-profile.bin",data,id,"offline-public")) {
                    ByteBuffer saved(data);std::int32_t version{};Bytes blob;
                    if(saved.ReadI32(version)&&saved.ReadBlob(blob)&&!saved.More()&&blob.size()==1024) {
                        ByteBuffer object;object.WriteU64(id);object.WriteScalar(7,version);object.WriteBlob(blob);results.push_back(object.Data());
                    }
                }
            }
            SendService(operation,0,results);return true;
        }
        if(service==8 && operation==3) {
            // Game writer005A9F40: signed32 version + blob1024; base writer
            // 008F38E0 is a no-op. Replies005A9FA0 prepend the profile's ID.
            std::int32_t version{};Bytes blob;
            if(!request.ReadI32(version)||!request.ReadBlob(blob)||blob.size()!=1024||!request.IsTaskEnd(requestPadding_)){
                SendService(operation,4);return true;
            }
            ByteBuffer saved;saved.WriteScalar(7,version);saved.WriteBlob(blob);
            SendService(operation,WriteUserFile("public-profile.bin",saved.Data(),0,"offline-public")?0:2);return true;
        }
        if((service==31&&operation==3)||(service==33&&operation==2)) {
            // Twitch/YouTube registration query: offline accounts are unlinked.
            if(!request.IsTaskEnd(requestPadding_)){SendService(operation,4);return true;}
            ByteBuffer registered;registered.WriteBool(false);SendService(operation,0,{registered.Data()});return true;
        }
        return false;
    }

    void HandleStats(std::uint8_t operation,ByteBuffer& request) {
        // Keep leaderboard submissions separate from authoritative Zombies
        // progression (storage10/1,3). Preserve submitted values durably rather
        // than acknowledging and dropping them. No public rankings are invented.
        if(operation==1) {
            struct Row {std::uint32_t board{};std::uint64_t owner{};Bytes data;};
            std::vector<Row> rows;
            while(!request.IsTaskEnd(requestPadding_) && rows.size()<256) {
                Row row;std::uint8_t mode{};std::int64_t score{};ByteBuffer saved;
                if(!request.ReadU32(row.board)||!request.ReadU64(row.owner)||!request.ReadU8(mode)||!request.ReadI64(score)){
                    SendService(operation,4);return;
                }
                saved.WriteU8(mode);saved.WriteScalar(9,score);
                unsigned columns=0;
                while(!request.IsTaskEnd(requestPadding_)&&(request.PeekType()==7||request.PeekType()==0x14)&&columns<64) {
                    // Native post-match capture includes unset columns as
                    // bdByteBuffer NAN (tag20, no payload), not signed32 zero.
                    if(request.PeekType()==0x14) {
                        std::uint8_t tag{};if(!request.ReadRaw(&tag,1)){SendService(operation,4);return;}
                        saved.WriteRaw(&tag,1);
                    } else {
                        std::int32_t value{};if(!request.ReadI32(value)){SendService(operation,4);return;}
                        saved.WriteScalar(7,value);
                    }
                    ++columns;
                }
                row.data=saved.Data();rows.push_back(std::move(row));
            }
            if(rows.empty()||!request.IsTaskEnd(requestPadding_)){SendService(operation,4);return;}
            // Acknowledgement means all rows reached disk. This stores the
            // latest submitted row; backend-specific aggregation is not claimed.
            for(const auto& row:rows)if(!WriteUserFile("board-"+std::to_string(row.board)+".bin",row.data,row.owner,"offline-leaderboards")){
                SendService(operation,2);return;
            }
            SendService(operation,0);return;
        }
        if(operation==3) {
            std::uint32_t board{};if(!request.ReadU32(board)){SendService(operation,4);return;}
            std::vector<Bytes> results;unsigned count=0;
            while(!request.IsTaskEnd(requestPadding_)) {
                std::uint64_t id{};if(!request.ReadU64(id)||++count>4096){SendService(operation,4);return;}
                Bytes data;if(!ReadUserFile("board-"+std::to_string(board)+".bin",data,id,"offline-leaderboards"))continue;
                ByteBuffer saved(data);std::uint8_t mode{};std::int64_t score{};
                if(!saved.ReadU8(mode)||!saved.ReadI64(score))continue;
                ByteBuffer result;result.WriteU64(id);result.WriteScalar(9,score);
                result.WriteU64(0);result.WriteString("Offline Player");result.WriteU32(0);
                unsigned columns=0;bool valid=true;
                while(saved.More()&&columns<64){
                    if(saved.PeekType()==0x14){std::uint8_t tag{};saved.ReadRaw(&tag,1);result.WriteRaw(&tag,1);}
                    else {std::int32_t v{};if(!saved.ReadI32(v)){valid=false;break;}result.WriteScalar(7,v);}
                    ++columns;
                }
                if(valid&&!saved.More())results.push_back(result.Data());
            }
            SendService(operation,0,results);return;
        }
        if(operation==4||operation==5) {
            std::uint32_t board{},maximum{};std::uint64_t pivot{};
            if(!request.ReadU32(board)||!request.ReadU64(pivot)||!request.ReadU32(maximum)||!request.IsTaskEnd(requestPadding_)){
                SendService(operation,4);return;
            }
            SendService(operation,0);return; // no worldwide ranking directory
        }
        Log("offline-service: unsupported stats operation=%u",operation);SendService(operation,4);
    }

    void HandleStorage(std::uint8_t operation, ByteBuffer& request) {
        if (operation == 7) {
            std::string name;
            if (!request.ReadString(name)) { SendService(operation, kNoFile); return; }
            Bytes file;
            const auto path = PublisherPath(name);
            if (!path || !ReadFile(*path, file)) {
                Log("lan-dw: missing publisher file %s", name.c_str());
                SendService(operation, kNoFile);
                return;
            }
            ByteBuffer object; SerializeFileData(object, file);
            SendService(operation, 0, {object.Data()});
            Log("lan-dw: publisher file %s served (%u bytes)", name.c_str(),
                static_cast<unsigned>(file.size()));
            return;
        }
        if (operation == 6) {
            std::uint32_t date{}; std::uint16_t count{}, offset{}; std::string name;
            if (!request.ReadU32(date) || !request.ReadU16(count) ||
                !request.ReadU16(offset) || !request.ReadString(name)) {
                SendService(operation, 4); return;
            }
            Bytes file;
            const auto path = PublisherPath(name);
            if (count && !offset && path && ReadFile(*path, file)) {
                ByteBuffer object; SerializeFileInfo(object, name, file.size());
                SendService(operation, 0, {object.Data()});
            } else SendService(operation, 0);
            return;
        }
        if (operation == 1) {
            std::string name; bool visible{}; Bytes file;
            if (!request.ReadString(name) || !request.ReadBool(visible) || !request.ReadBlob(file)) {
                SendService(operation, 4); return;
            }
            const bool written = WriteUserFile(name,file);
            Log("lan-dw: user file write name=%s bytes=%u visible=%d result=%d", name.c_str(),
                static_cast<unsigned>(file.size()), visible ? 1 : 0, written ? 1 : 0);
            if (!written) { SendService(operation, 2); return; }
            ByteBuffer object; SerializeFileInfo(object, name, file.size(), g_localUserId, visible);
            SendService(operation, 0, {object.Data()});
            return;
        }
        if (operation == 3) {
            std::string name; Bytes file;
            if (!request.ReadString(name) ||
                !ReadUserFile(name,file)) {
                SendService(operation, kNoFile); return;
            }
            ByteBuffer object; SerializeFileData(object, file);
            SendService(operation, 0, {object.Data()});
            Log("lan-dw: user file read name=%s bytes=%u", name.c_str(),
                static_cast<unsigned>(file.size()));
            return;
        }
        if (operation == 10) {
            std::string game, name; bool visible{}; Bytes file; std::uint64_t owner{};
            if (!request.ReadString(game) || !request.ReadString(name) ||
                !request.ReadBool(visible) || !request.ReadBlob(file) || !request.ReadU64(owner)) {
                SendService(operation, 4); return;
            }
            const bool written = WriteUserFile(name,file,owner,game);
            Log("lan-dw: cross-title user write game=%s name=%s owner=%llu bytes=%u result=%d",
                game.c_str(), name.c_str(), static_cast<unsigned long long>(owner),
                static_cast<unsigned>(file.size()), written ? 1 : 0);
            if (!written) { SendService(operation, 2); return; }
            ByteBuffer object; SerializeFileInfo(object, name, file.size(), owner, visible);
            SendService(operation, 0, {object.Data()});
            return;
        }
        if (operation == 12) {
            std::string game, name, platform; std::uint64_t owner{}; Bytes file;
            if (!request.ReadString(game) || !request.ReadString(name) ||
                !request.ReadU64(owner) || !request.ReadString(platform) ||
                !ReadUserFile(name,file,owner,game)) {
                SendService(operation, kNoFile); return;
            }
            ByteBuffer object; SerializeFileData(object, file);
            SendService(operation, 0, {object.Data()});
            Log("lan-dw: cross-title user read game=%s name=%s owner=%llu platform=%s bytes=%u",
                game.c_str(), name.c_str(), static_cast<unsigned long long>(owner), platform.c_str(),
                static_cast<unsigned>(file.size()));
            return;
        }
        Log("lan-dw: unimplemented storage operation=%u rejected", operation);
        SendService(operation, 4);
    }

    void HandleTitleUtilities(std::uint8_t operation) {
        if (operation == 6) {
            ByteBuffer object;
            object.WriteU32(static_cast<std::uint32_t>(std::time(nullptr)));
            SendService(operation, 0, {object.Data()});
        } else { Log("offline-service: unsupported title operation=%u",operation);SendService(operation,4); }
    }

    void HandleDml(std::uint8_t operation) {
        if (operation == 2 || operation == 3) {
            ByteBuffer object;
            object.WriteString("US");
            object.WriteString("United States");
            object.WriteString("Local");
            object.WriteString("LAN");
            object.WriteFloat(0.0f);
            object.WriteFloat(0.0f);
            // T6 bdDMLHierarchicalInfo::deserialize at 009F27A0 extends
            // the six base fields with four U32 hierarchy values, not the
            // IW6 ASN/string-timezone extension previously used here.
            for (int i = 0; i < 4; ++i) object.WriteU32(0);
            SendService(operation, 0, {object.Data()});
        } else { Log("offline-service: unsupported DML operation=%u",operation);SendService(operation,4); }
    }

    void HandleGroups(std::uint8_t operation, ByteBuffer& request) {
        if(operation==1) {
            Bytes groups;
            if(!request.ReadArray(8,4,groups)||!request.IsTaskEnd(requestPadding_)){SendService(operation,4);return;}
            localGroups_=std::move(groups);SendService(operation,0);return;
        }
        Log("offline-service: unverified groups operation=%u",operation);
        SendService(operation, 4);
    }

    void HandleMatchmaking(std::uint8_t operation, ByteBuffer& request) {
        Log("lan-dw: matchmaking operation=%u request-bytes=%u", operation,
            static_cast<unsigned>(request.Remaining()));
        const auto reply=sessions_.Handle(operation,request,requestPadding_);
        SendService(operation,reply.error,reply.objects);
    }

    ServerKind kind_;
    std::map<unsigned,unsigned> operationCounts_,errorCounts_;
    std::array<unsigned,256> messageCounts_{};
    std::uint8_t currentService_{};
    bool currentRequestLogged_{};
    std::uint8_t requestPadding_{};
    bool consoleReportAccepted_{};
    Bytes localPresence_,localGroups_;
    enum class Phase { Handshake, Hello, Services };
    std::atomic<Phase> phase_{Phase::Handshake};
    std::atomic<bool> failed_{};
    std::atomic<std::size_t> buffered_{};
    std::atomic<unsigned> requests_{};
    bool workerMode_{};
    std::thread worker_;
    std::mutex pendingMutex_;
    std::condition_variable incoming_;
    std::deque<Bytes> pending_;
    std::size_t pendingBytes_{};
    unsigned sendCalls_{}, receiveCalls_{};
    LocalSessions sessions_;
    mutable std::mutex mutex_;
    std::condition_variable available_;
    Bytes input_;
    std::deque<std::uint8_t> output_;
    std::array<std::uint8_t, 24> encryptKey_{};
    std::array<std::uint8_t, 24> decryptKey_{};
};


LocalServer::LocalServer(ServerKind k,bool worker):impl_(std::make_unique<Impl>(k,worker)){}
LocalServer::~LocalServer()=default;
int LocalServer::Send(const char* p,int n){return impl_->Send(p,n);}
int LocalServer::Receive(char* p,int n,int f,bool b){return impl_->Receive(p,n,f,b);}
bool LocalServer::Readable()const{return impl_->Readable();}
std::size_t LocalServer::Available()const{return impl_->Available();}
bool LocalServer::Ready()const{return impl_->Ready();}
bool LocalServer::Failed()const{return impl_->Failed();}
void LocalServer::Close(){impl_->Close();}
void LocalServer::Dump(SOCKET s)const{impl_->Dump(s);}
}
