#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
namespace bo2lan::offline {
using Bytes = std::vector<std::uint8_t>;

template <typename T>
void AppendRaw(Bytes& out, const T& value) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&value);
    out.insert(out.end(), p, p + sizeof(T));
}

class ByteBuffer {
public:
    ByteBuffer() = default;
    explicit ByteBuffer(Bytes data) : data_(std::move(data)) {}

    void Types(bool enabled) { types_ = enabled; }
    bool More() const { return cursor_ < data_.size(); }
    std::size_t Remaining() const { return data_.size() - std::min(cursor_, data_.size()); }
    const Bytes& Data() const { return data_; }
    Bytes Rest() const {
        if (cursor_ >= data_.size()) return {};
        return Bytes(data_.begin() + cursor_, data_.end());
    }

    bool ReadRaw(void* dst, std::size_t n) {
        if (n > Remaining()) return false;
        if(!n)return true;
        if(!dst)return false;
        std::memcpy(dst, data_.data() + cursor_, n);
        cursor_ += n;
        return true;
    }
    template <typename T> bool ReadScalar(std::uint8_t type, T& value) {
        if (types_) {
            std::uint8_t actual{};
            if (!ReadRaw(&actual, 1) || actual != type) return false;
        }
        return ReadRaw(&value, sizeof(value));
    }
    bool ReadU8(std::uint8_t& v) { return ReadScalar(3, v); }
    bool ReadBool(bool& v) { std::uint8_t b{}; if (!ReadScalar(1, b)) return false; v = b != 0; return true; }
    bool ReadU16(std::uint16_t& v) { return ReadScalar(6, v); }
    bool ReadU32(std::uint32_t& v) { return ReadScalar(8, v); }
    bool ReadU64(std::uint64_t& v) { return ReadScalar(10, v); }
    bool ReadI64(std::int64_t& v) { return ReadScalar(9, v); }
    bool ReadI32(std::int32_t& v) { return ReadScalar(7, v); }
    std::uint8_t PeekType() const { return More()?data_[cursor_]:0; }
    bool IsTaskEnd(std::uint8_t byte) const {
        // Native009ED315 ->009DFF00 appends type0 after the service fields.
        //009F7850 then pads with0..7 copies of the sequence's low byte.
        // Check only at a schema field boundary; never strip zero bytes from
        // arbitrary blobs/strings or treat ciphertext padding as another field.
        return Remaining()>=1 && Remaining()<=8 && data_[cursor_]==0 &&
            std::all_of(data_.begin()+cursor_+1,data_.end(),
            [byte](std::uint8_t v){return v==byte;});
    }
    bool ReadArray(std::uint8_t elementType,std::uint32_t width,Bytes& elements) {
        std::uint8_t tag{};std::uint32_t bytes{},count{};
        if(!ReadRaw(&tag,1)||tag!=elementType+100||!ReadU32(bytes)||!ReadRaw(&count,4)||
            !width||count>4096||static_cast<std::uint64_t>(bytes)!=static_cast<std::uint64_t>(count)*width||bytes>Remaining())return false;
        elements.resize(bytes);return ReadRaw(elements.data(),bytes);
    }
    bool ReadFloat(float& v) { return ReadScalar(13, v); }
    bool ReadString(std::string& value) {
        if (types_) { std::uint8_t t{}; if (!ReadRaw(&t, 1) || t != 16) return false; }
        const auto begin = cursor_;
        while (cursor_ < data_.size() && data_[cursor_] != 0) ++cursor_;
        if (cursor_ >= data_.size()) return false;
        value.assign(reinterpret_cast<const char*>(data_.data() + begin), cursor_ - begin);
        ++cursor_;
        return true;
    }
    bool ReadBlob(Bytes& value) {
        if (types_) { std::uint8_t t{}; if (!ReadRaw(&t, 1) || t != 0x13) return false; }
        std::uint32_t n{};
        if (!ReadU32(n) || n > Remaining()) return false;
        value.assign(data_.begin() + cursor_, data_.begin() + cursor_ + n);
        cursor_ += n;
        return true;
    }

    void WriteRaw(const void* src, std::size_t n) {
        if(!n)return;
        const auto* p = static_cast<const std::uint8_t*>(src);
        data_.insert(data_.end(), p, p + n);
    }
    template <typename T> void WriteScalar(std::uint8_t type, const T& value) {
        if (types_) data_.push_back(type);
        WriteRaw(&value, sizeof(value));
    }
    void WriteU8(std::uint8_t v) { WriteScalar(3, v); }
    void WriteBool(bool v) { const std::uint8_t b = v ? 1 : 0; WriteScalar(1, b); }
    void WriteU16(std::uint16_t v) { WriteScalar(6, v); }
    void WriteU32(std::uint32_t v) { WriteScalar(8, v); }
    void WriteU64(std::uint64_t v) { WriteScalar(10, v); }
    void WriteFloat(float v) { WriteScalar(13, v); }
    void WriteString(const std::string& value) {
        if (types_) data_.push_back(16);
        WriteRaw(value.c_str(), value.size() + 1);
    }
    void WriteBlob(const Bytes& value) {
        if (types_) data_.push_back(0x13);
        WriteU32(static_cast<std::uint32_t>(value.size()));
        WriteRaw(value.data(), value.size());
    }

private:
    Bytes data_;
    std::size_t cursor_{};
    bool types_{true};
};

class BitBuffer {
public:
    BitBuffer() = default;
    explicit BitBuffer(Bytes data) : data_(std::move(data)) {}
    void Types(bool enabled) { types_ = enabled; }
    bool ReadBits(unsigned count, void* destination) {
        if(!count)return true;
        if (!destination || bit_ + count > data_.size() * 8) return false;
        auto* out = static_cast<std::uint8_t*>(destination);
        std::memset(out, 0, (count + 7) / 8);
        for (unsigned i = 0; i < count; ++i) {
            const auto sourceBit = (data_[(bit_ + i) / 8] >> ((bit_ + i) & 7)) & 1;
            out[i / 8] |= static_cast<std::uint8_t>(sourceBit << (i & 7));
        }
        bit_ += count;
        return true;
    }
    bool ReadType(std::uint8_t expected) {
        if (!types_) return true;
        std::uint8_t actual{};
        return ReadBits(5, &actual) && actual == expected;
    }
    bool ReadBool(bool& value) {
        if (!ReadType(1)) return false;
        std::uint8_t v{}; if (!ReadBits(1, &v)) return false; value = v != 0; return true;
    }
    bool ReadU32(std::uint32_t& value) { return ReadType(8) && ReadBits(32, &value); }
    bool ReadBytes(void* dst, std::size_t n) { return ReadBits(static_cast<unsigned>(n * 8), dst); }

    void WriteBits(unsigned count, const void* source) {
        const auto* in = static_cast<const std::uint8_t*>(source);
        const auto need = (bit_ + count + 7) / 8;
        if (data_.size() < need) data_.resize(need);
        for (unsigned i = 0; i < count; ++i) {
            const auto value = (in[i / 8] >> (i & 7)) & 1;
            const auto mask = static_cast<std::uint8_t>(1u << ((bit_ + i) & 7));
            if (value) data_[(bit_ + i) / 8] |= mask;
            else data_[(bit_ + i) / 8] &= static_cast<std::uint8_t>(~mask);
        }
        bit_ += count;
    }
    void WriteType(std::uint8_t type) { if (types_) WriteBits(5, &type); }
    void WriteBool(bool value) { WriteType(1); const std::uint8_t v = value ? 1 : 0; WriteBits(1, &v); }
    void WriteU32(std::uint32_t value) { WriteType(8); WriteBits(32, &value); }
    void WriteBytes(const void* src, std::size_t n) { WriteBits(static_cast<unsigned>(n * 8), src); }
    const Bytes& Data() const { return data_; }

private:
    Bytes data_;
    std::size_t bit_{};
    bool types_{true};
};


void InitCrypto();
std::array<std::uint8_t,24> Tiger(const void*,std::size_t);
Bytes TripleDes(const Bytes&,const std::array<std::uint8_t,24>&,const std::array<std::uint8_t,24>&,bool);
// BO2 client task packets have a truncated HMAC-SHA1, not the server's
// DEADBEEF marker. MAC covers bytes after the raw service ID, including pad.
bool ClientTaskMac(const std::uint8_t*,std::size_t,const std::array<std::uint8_t,24>&,
                   std::array<std::uint8_t,4>&);
bool ValidateClientTask(const Bytes&,const std::array<std::uint8_t,24>&);
}
