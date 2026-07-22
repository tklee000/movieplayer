#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace movieplayer::codec {

class ByteReader {
public:
    ByteReader() = default;
    ByteReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}
    explicit ByteReader(const std::vector<std::uint8_t>& bytes)
        : ByteReader(bytes.data(), bytes.size()) {}

    std::size_t Position() const noexcept { return position_; }
    std::size_t Size() const noexcept { return size_; }
    std::size_t Remaining() const noexcept {
        return position_ <= size_ ? size_ - position_ : 0;
    }
    bool Empty() const noexcept { return Remaining() == 0; }

    bool Seek(std::size_t position) noexcept;
    bool Skip(std::size_t count) noexcept;
    bool ReadU8(std::uint8_t& value) noexcept;
    bool ReadU16(std::uint16_t& value) noexcept;
    bool ReadU24(std::uint32_t& value) noexcept;
    bool ReadU32(std::uint32_t& value) noexcept;
    bool ReadU64(std::uint64_t& value) noexcept;
    bool ReadI32(std::int32_t& value) noexcept;
    bool ReadI64(std::int64_t& value) noexcept;
    bool ReadFourCC(std::string& value);
    bool ReadBytes(std::size_t count, std::vector<std::uint8_t>& value);
    bool ReadView(std::size_t count, const std::uint8_t*& value) noexcept;

    const std::uint8_t* CurrentData() const noexcept {
        return position_ <= size_ ? data_ + position_ : nullptr;
    }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t position_ = 0;
};

}  // namespace movieplayer::codec
