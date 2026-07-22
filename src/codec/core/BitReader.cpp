#include "codec/core/BitReader.h"

#include <limits>

namespace movieplayer::codec {

bool BitReader::ReadBit(bool& value) noexcept {
    if (bitPosition_ >= bitCount_) {
        return false;
    }
    const std::size_t byteIndex = bitPosition_ >> 3;
    const unsigned shift = 7U - static_cast<unsigned>(bitPosition_ & 7U);
    value = ((data_[byteIndex] >> shift) & 1U) != 0;
    ++bitPosition_;
    return true;
}

bool BitReader::ReadBits(unsigned count, std::uint32_t& value) noexcept {
    if (count > 32 || count > BitsRemaining()) {
        return false;
    }
    value = 0;
    for (unsigned i = 0; i < count; ++i) {
        bool bit = false;
        ReadBit(bit);
        value = (value << 1) | static_cast<std::uint32_t>(bit);
    }
    return true;
}

bool BitReader::ReadBits64(unsigned count, std::uint64_t& value) noexcept {
    if (count > 64 || count > BitsRemaining()) {
        return false;
    }
    value = 0;
    for (unsigned i = 0; i < count; ++i) {
        bool bit = false;
        ReadBit(bit);
        value = (value << 1) | static_cast<std::uint64_t>(bit);
    }
    return true;
}

bool BitReader::SkipBits(std::size_t count) noexcept {
    if (count > BitsRemaining()) {
        return false;
    }
    bitPosition_ += count;
    return true;
}

bool BitReader::ReadUnsignedExpGolomb(std::uint32_t& value) noexcept {
    unsigned leadingZeroBits = 0;
    bool bit = false;
    while (ReadBit(bit) && !bit) {
        if (++leadingZeroBits > 31) {
            return false;
        }
    }
    if (!bit) {
        return false;
    }
    std::uint32_t suffix = 0;
    if (leadingZeroBits != 0 && !ReadBits(leadingZeroBits, suffix)) {
        return false;
    }
    value = ((std::uint32_t{1} << leadingZeroBits) - 1U) + suffix;
    return true;
}

bool BitReader::ReadSignedExpGolomb(std::int32_t& value) noexcept {
    std::uint32_t code = 0;
    if (!ReadUnsignedExpGolomb(code)) {
        return false;
    }
    if (code & 1U) {
        value = static_cast<std::int32_t>((code + 1U) >> 1U);
    } else {
        value = -static_cast<std::int32_t>(code >> 1U);
    }
    return true;
}

bool BitReader::ByteAlign() noexcept {
    const std::size_t aligned = (bitPosition_ + 7U) & ~std::size_t{7U};
    return SkipBits(aligned - bitPosition_);
}

std::vector<std::uint8_t> RemoveEmulationPreventionBytes(
    const std::uint8_t* data, std::size_t size) {
    std::vector<std::uint8_t> result;
    result.reserve(size);
    unsigned zeroCount = 0;
    for (std::size_t i = 0; i < size; ++i) {
        const std::uint8_t value = data[i];
        if (zeroCount >= 2 && value == 0x03) {
            zeroCount = 0;
            continue;
        }
        result.push_back(value);
        zeroCount = value == 0 ? zeroCount + 1 : 0;
    }
    return result;
}

}  // namespace movieplayer::codec
