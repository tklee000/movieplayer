#include "codec/core/ByteReader.h"

#include <limits>

namespace movieplayer::codec {

bool ByteReader::Seek(std::size_t position) noexcept {
    if (position > size_) {
        return false;
    }
    position_ = position;
    return true;
}

bool ByteReader::Skip(std::size_t count) noexcept {
    if (count > Remaining()) {
        return false;
    }
    position_ += count;
    return true;
}

bool ByteReader::ReadU8(std::uint8_t& value) noexcept {
    if (Remaining() < 1) {
        return false;
    }
    value = data_[position_++];
    return true;
}

bool ByteReader::ReadU16(std::uint16_t& value) noexcept {
    if (Remaining() < 2) {
        return false;
    }
    value = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data_[position_]) << 8) |
        static_cast<std::uint16_t>(data_[position_ + 1]));
    position_ += 2;
    return true;
}

bool ByteReader::ReadU24(std::uint32_t& value) noexcept {
    if (Remaining() < 3) {
        return false;
    }
    value = (static_cast<std::uint32_t>(data_[position_]) << 16) |
            (static_cast<std::uint32_t>(data_[position_ + 1]) << 8) |
            static_cast<std::uint32_t>(data_[position_ + 2]);
    position_ += 3;
    return true;
}

bool ByteReader::ReadU32(std::uint32_t& value) noexcept {
    if (Remaining() < 4) {
        return false;
    }
    value = (static_cast<std::uint32_t>(data_[position_]) << 24) |
            (static_cast<std::uint32_t>(data_[position_ + 1]) << 16) |
            (static_cast<std::uint32_t>(data_[position_ + 2]) << 8) |
            static_cast<std::uint32_t>(data_[position_ + 3]);
    position_ += 4;
    return true;
}

bool ByteReader::ReadU64(std::uint64_t& value) noexcept {
    std::uint32_t high = 0;
    std::uint32_t low = 0;
    if (!ReadU32(high) || !ReadU32(low)) {
        return false;
    }
    value = (static_cast<std::uint64_t>(high) << 32) | low;
    return true;
}

bool ByteReader::ReadI32(std::int32_t& value) noexcept {
    std::uint32_t bits = 0;
    if (!ReadU32(bits)) {
        return false;
    }
    value = static_cast<std::int32_t>(bits);
    return true;
}

bool ByteReader::ReadI64(std::int64_t& value) noexcept {
    std::uint64_t bits = 0;
    if (!ReadU64(bits)) {
        return false;
    }
    value = static_cast<std::int64_t>(bits);
    return true;
}

bool ByteReader::ReadFourCC(std::string& value) {
    if (Remaining() < 4) {
        return false;
    }
    value.assign(reinterpret_cast<const char*>(data_ + position_), 4);
    position_ += 4;
    return true;
}

bool ByteReader::ReadBytes(std::size_t count, std::vector<std::uint8_t>& value) {
    if (count > Remaining()) {
        return false;
    }
    value.assign(data_ + position_, data_ + position_ + count);
    position_ += count;
    return true;
}

bool ByteReader::ReadView(std::size_t count, const std::uint8_t*& value) noexcept {
    if (count > Remaining()) {
        value = nullptr;
        return false;
    }
    value = data_ + position_;
    position_ += count;
    return true;
}

}  // namespace movieplayer::codec
