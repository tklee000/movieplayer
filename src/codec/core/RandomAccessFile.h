#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace movieplayer::codec {

class RandomAccessFile {
public:
    RandomAccessFile() = default;
    ~RandomAccessFile();

    RandomAccessFile(const RandomAccessFile&) = delete;
    RandomAccessFile& operator=(const RandomAccessFile&) = delete;

    bool Open(const std::wstring& path, std::wstring& error);
    void Close();
    bool Read(std::uint64_t offset, void* destination, std::size_t size,
              std::wstring& error) const;
    bool Read(std::uint64_t offset, std::size_t size,
              std::vector<std::uint8_t>& destination, std::wstring& error) const;
    bool IsOpen() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }
    std::uint64_t Size() const noexcept { return size_; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::uint64_t size_ = 0;
};

}  // namespace movieplayer::codec
