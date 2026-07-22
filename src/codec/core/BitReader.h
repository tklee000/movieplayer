#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace movieplayer::codec {

class BitReader {
public:
    BitReader() = default;
    BitReader(const std::uint8_t* data, std::size_t size)
        : data_(data), bitCount_(size * 8) {}
    explicit BitReader(const std::vector<std::uint8_t>& data)
        : BitReader(data.data(), data.size()) {}

    std::size_t BitsRemaining() const noexcept {
        return bitPosition_ <= bitCount_ ? bitCount_ - bitPosition_ : 0;
    }
    std::size_t BitPosition() const noexcept { return bitPosition_; }

    bool ReadBit(bool& value) noexcept;
    bool ReadBits(unsigned count, std::uint32_t& value) noexcept;
    bool ReadBits64(unsigned count, std::uint64_t& value) noexcept;
    bool SkipBits(std::size_t count) noexcept;
    bool ReadUnsignedExpGolomb(std::uint32_t& value) noexcept;
    bool ReadSignedExpGolomb(std::int32_t& value) noexcept;
    bool ByteAlign() noexcept;

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t bitCount_ = 0;
    std::size_t bitPosition_ = 0;
};

std::vector<std::uint8_t> RemoveEmulationPreventionBytes(
    const std::uint8_t* data, std::size_t size);

}  // namespace movieplayer::codec
