#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace movieplayer::codec {

// Inflates one RFC 1950 zlib stream. The bounded implementation is kept in
// MovieCodecCore so Matroska ContentCompression does not add a runtime DLL.
bool InflateZlib(const std::uint8_t* data, std::size_t size,
                 std::vector<std::uint8_t>& output, std::wstring& error);

}  // namespace movieplayer::codec
