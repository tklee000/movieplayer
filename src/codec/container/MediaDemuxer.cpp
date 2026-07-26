#include "codec/container/MediaDemuxer.h"

#include "codec/container/avi/AviDemuxer.h"
#include "codec/container/mkv/MkvDemuxer.h"
#include "codec/container/mp4/Mp4Demuxer.h"
#include "codec/core/RandomAccessFile.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>

namespace movieplayer::codec {
namespace {

enum class ContainerKind {
    Unknown,
    Matroska,
    Avi,
    Mp4,
};

ContainerKind DetectContainer(const std::wstring& path) {
    RandomAccessFile file;
    std::wstring error;
    if (!file.Open(path, error) || file.Size() < 8) {
        return ContainerKind::Unknown;
    }
    std::array<std::uint8_t, 12> bytes = {};
    const std::size_t size = static_cast<std::size_t>(
        std::min<std::uint64_t>(bytes.size(), file.Size()));
    if (!file.Read(0, bytes.data(), size, error)) {
        return ContainerKind::Unknown;
    }
    if (bytes[0] == 0x1a && bytes[1] == 0x45 &&
        bytes[2] == 0xdf && bytes[3] == 0xa3) {
        return ContainerKind::Matroska;
    }
    if (size >= 12 &&
        std::equal(bytes.begin(), bytes.begin() + 4, "RIFF") &&
        std::equal(bytes.begin() + 8, bytes.begin() + 12, "AVI ")) {
        return ContainerKind::Avi;
    }
    const std::array<std::array<char, 4>, 6> isoBoxTypes = {{
        {{'f', 't', 'y', 'p'}},
        {{'m', 'o', 'o', 'v'}},
        {{'m', 'd', 'a', 't'}},
        {{'f', 'r', 'e', 'e'}},
        {{'s', 'k', 'i', 'p'}},
        {{'w', 'i', 'd', 'e'}},
    }};
    for (const auto& type : isoBoxTypes) {
        if (std::equal(type.begin(), type.end(), bytes.begin() + 4)) {
            return ContainerKind::Mp4;
        }
    }
    return ContainerKind::Unknown;
}

}  // namespace

std::unique_ptr<IMediaDemuxer> CreateMediaDemuxer(const std::wstring& path) {
    const ContainerKind detected = DetectContainer(path);
    if (detected == ContainerKind::Matroska) {
        return std::make_unique<mkv::MkvDemuxer>();
    }
    if (detected == ContainerKind::Avi) {
        return std::make_unique<avi::AviDemuxer>();
    }
    if (detected == ContainerKind::Mp4) {
        return std::make_unique<mp4::Mp4Demuxer>();
    }

    const std::size_t dot = path.find_last_of(L'.');
    std::wstring extension = dot == std::wstring::npos ? L"" : path.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t value) { return std::towlower(value); });
    if (extension == L".mkv" || extension == L".webm") {
        return std::make_unique<mkv::MkvDemuxer>();
    }
    if (extension == L".avi") {
        return std::make_unique<avi::AviDemuxer>();
    }
    return std::make_unique<mp4::Mp4Demuxer>();
}

}  // namespace movieplayer::codec
