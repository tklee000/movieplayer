#include "codec/container/MediaDemuxer.h"

#include "codec/container/avi/AviDemuxer.h"
#include "codec/container/mkv/MkvDemuxer.h"
#include "codec/container/mp4/Mp4Demuxer.h"

#include <algorithm>
#include <cwctype>

namespace movieplayer::codec {

std::unique_ptr<IMediaDemuxer> CreateMediaDemuxer(const std::wstring& path) {
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
