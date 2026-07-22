#include "codec/subtitle/TextSubtitleDecoder.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <limits>
#include <utility>

namespace movieplayer::codec::subtitle {
namespace {

bool Utf8ToWide(const std::uint8_t* bytes, std::size_t size,
                std::wstring& output) {
    output.clear();
    if (size == 0) return true;
    if (size > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
        return false;
    const int inputSize = static_cast<int>(size);
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                          reinterpret_cast<const char*>(bytes),
                                          inputSize, nullptr, 0);
    if (count <= 0) return false;
    output.resize(static_cast<std::size_t>(count));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                               reinterpret_cast<const char*>(bytes), inputSize,
                               output.data(), count) == count;
}

bool Utf16ToWide(const std::uint8_t* bytes, std::size_t size,
                 std::wstring& output) {
    if (size < 2 || (size & 1U) != 0) return false;
    bool bigEndian = bytes[0] == 0xfe && bytes[1] == 0xff;
    std::size_t position =
        ((bytes[0] == 0xff && bytes[1] == 0xfe) || bigEndian) ? 2U : 0U;
    output.clear();
    output.reserve((size - position) / 2U);
    while (position + 1 < size) {
        const std::uint16_t value = bigEndian
                                        ? static_cast<std::uint16_t>(
                                              (bytes[position] << 8U) |
                                              bytes[position + 1])
                                        : static_cast<std::uint16_t>(
                                              bytes[position] |
                                              (bytes[position + 1] << 8U));
        if (value != 0) output.push_back(static_cast<wchar_t>(value));
        position += 2;
    }
    return true;
}

std::wstring AssEventText(const std::wstring& event) {
    std::size_t position = 0;
    for (unsigned field = 0; field < 8; ++field) {
        position = event.find(L',', position);
        if (position == std::wstring::npos) return event;
        ++position;
    }
    return event.substr(position);
}

std::wstring PlainText(const std::wstring& input) {
    std::wstring result;
    result.reserve(input.size());
    bool overrideBlock = false;
    for (std::size_t i = 0; i < input.size(); ++i) {
        const wchar_t value = input[i];
        if (value == L'{') {
            overrideBlock = true;
            continue;
        }
        if (overrideBlock) {
            if (value == L'}') overrideBlock = false;
            continue;
        }
        if (value == L'\\' && i + 1 < input.size()) {
            const wchar_t escape = input[i + 1];
            if (escape == L'N' || escape == L'n') {
                result.push_back(L'\n');
                ++i;
                continue;
            }
            if (escape == L'h') {
                result.push_back(L' ');
                ++i;
                continue;
            }
        }
        if (value != L'\r') result.push_back(value);
    }
    while (!result.empty() &&
           (result.back() == L'\n' || result.back() == L' ' ||
            result.back() == L'\t')) {
        result.pop_back();
    }
    return result;
}

}  // namespace

bool DecodeTextSample(const TrackInfo& track, const EncodedSample& sample,
                      std::wstring& text, std::wstring& error) {
    std::wstring decoded;
    if (!Utf8ToWide(sample.bytes.data(), sample.bytes.size(), decoded)) {
        if (!Utf16ToWide(sample.bytes.data(), sample.bytes.size(), decoded)) {
            error = L"The embedded subtitle sample has an invalid text encoding";
            return false;
        }
    } else if (decoded.find(L'\0') != std::wstring::npos) {
        // A few legacy Korean Matroska files label UTF-16LE text as
        // S_TEXT/UTF8.  Embedded NULs make that signature unambiguous.
        std::wstring utf16;
        if (Utf16ToWide(sample.bytes.data(), sample.bytes.size(), utf16))
            decoded = std::move(utf16);
    }
    if (track.codec == CodecId::Ass) decoded = AssEventText(decoded);
    text = PlainText(decoded);
    error.clear();
    return true;
}

}  // namespace movieplayer::codec::subtitle
