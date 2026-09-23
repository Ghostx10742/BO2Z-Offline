#include "offline_protocol.h"
#include <tomcrypt.h>
namespace bo2lan::offline {
std::once_flag g_cryptoOnce;
void InitCrypto() {
    std::call_once(g_cryptoOnce, [] { register_cipher(&des3_desc); register_hash(&sha1_desc); });
}

std::array<std::uint8_t, 24> Tiger(const void* data, std::size_t size) {
    std::array<std::uint8_t, 24> result{};
    hash_state state{};
    tiger_init(&state);
    tiger_process(&state, static_cast<const unsigned char*>(data), static_cast<unsigned long>(size));
    tiger_done(&state, result.data());
    return result;
}

Bytes TripleDes(const Bytes& input, const std::array<std::uint8_t, 24>& ivHash,
                const std::array<std::uint8_t, 24>& key, bool encrypt) {
    if(input.empty() || input.size()%8 || input.size()>20*1024*1024)return {};
    InitCrypto();
    Bytes output(input.size());
    symmetric_CBC state{};
    const int cipher = find_cipher("3des");
    if (cipher < 0 || cbc_start(cipher, ivHash.data(), key.data(), 24, 0, &state) != CRYPT_OK)
        return {};
    const int rc = encrypt
        ? cbc_encrypt(input.data(), output.data(), static_cast<unsigned long>(input.size()), &state)
        : cbc_decrypt(input.data(), output.data(), static_cast<unsigned long>(input.size()), &state);
    cbc_done(&state);
    return rc == CRYPT_OK ? output : Bytes{};
}

bool ClientTaskMac(const std::uint8_t* data,std::size_t size,
    const std::array<std::uint8_t,24>& key,std::array<std::uint8_t,4>& result) {
    if(!data || size>4*1024*1024)return false;
    InitCrypto();const auto hash=find_hash("sha1");if(hash<0)return false;
    unsigned long count=static_cast<unsigned long>(result.size());
    return hmac_memory(hash,key.data(),static_cast<unsigned long>(key.size()),
        data,static_cast<unsigned long>(size),result.data(),&count)==CRYPT_OK && count==4;
}
bool ValidateClientTask(const Bytes& plain,const std::array<std::uint8_t,24>& key) {
    if(plain.size()<8 || plain.size()%8)return false;
    std::array<std::uint8_t,4> expected{};
    if(!ClientTaskMac(plain.data()+5,plain.size()-5,key,expected))return false;
    unsigned different=0;
    for(unsigned i=0;i<4;++i)different|=expected[i]^plain[i];
    return different==0;
}
}
