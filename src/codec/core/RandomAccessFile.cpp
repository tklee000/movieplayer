#include "codec/core/RandomAccessFile.h"

#include <algorithm>
#include <limits>
#include <sstream>

namespace movieplayer::codec {
namespace {

std::wstring Win32Error(const wchar_t* operation, DWORD code) {
    std::wostringstream out;
    out << operation << L" failed (Win32 " << code << L")";
    return out.str();
}

}  // namespace

RandomAccessFile::~RandomAccessFile() {
    Close();
}

bool RandomAccessFile::Open(const std::wstring& path, std::wstring& error) {
    Close();
    handle_ = CreateFileW(path.c_str(), GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        error = Win32Error(L"CreateFileW", GetLastError());
        return false;
    }
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(handle_, &size) || size.QuadPart < 0) {
        error = Win32Error(L"GetFileSizeEx", GetLastError());
        Close();
        return false;
    }
    size_ = static_cast<std::uint64_t>(size.QuadPart);
    return true;
}

void RandomAccessFile::Close() {
    if (handle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
    size_ = 0;
}

bool RandomAccessFile::Read(std::uint64_t offset, void* destination,
                            std::size_t size, std::wstring& error) const {
    if (!IsOpen() || !destination || offset > size_ || size > size_ - offset) {
        error = L"The requested file range is invalid";
        return false;
    }
    auto* output = static_cast<std::uint8_t*>(destination);
    std::size_t completed = 0;
    while (completed < size) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
            size - completed, std::numeric_limits<DWORD>::max()));
        OVERLAPPED overlapped = {};
        const std::uint64_t absolute = offset + completed;
        overlapped.Offset = static_cast<DWORD>(absolute & 0xffffffffULL);
        overlapped.OffsetHigh = static_cast<DWORD>(absolute >> 32);
        DWORD bytesRead = 0;
        if (!ReadFile(handle_, output + completed, chunk, &bytesRead, &overlapped)) {
            error = Win32Error(L"ReadFile", GetLastError());
            return false;
        }
        if (bytesRead == 0) {
            error = L"Unexpected end of file";
            return false;
        }
        completed += bytesRead;
    }
    return true;
}

bool RandomAccessFile::Read(std::uint64_t offset, std::size_t size,
                            std::vector<std::uint8_t>& destination,
                            std::wstring& error) const {
    destination.resize(size);
    if (size == 0) {
        return true;
    }
    if (!Read(offset, destination.data(), size, error)) {
        destination.clear();
        return false;
    }
    return true;
}

}  // namespace movieplayer::codec
