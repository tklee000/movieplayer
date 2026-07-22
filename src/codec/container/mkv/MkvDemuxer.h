#pragma once

#include "codec/container/MediaDemuxer.h"

#include <memory>

namespace movieplayer::codec::mkv {

// Focused Matroska reader for seekable files containing AVC/HEVC, AAC, and
// UTF-8/ASS text subtitles. It supports SimpleBlock/BlockGroup plus fixed,
// Xiph, and EBML lacing.
class MkvDemuxer final : public IMediaDemuxer {
public:
    MkvDemuxer();
    ~MkvDemuxer() override;

    MkvDemuxer(const MkvDemuxer&) = delete;
    MkvDemuxer& operator=(const MkvDemuxer&) = delete;

    bool Open(const std::wstring& path) override;
    void Close() override;
    const std::vector<TrackInfo>& Tracks() const noexcept override;
    double DurationSeconds() const noexcept override;
    bool SetTrackEnabled(std::uint32_t trackId, bool enabled) override;
    bool ReadNextSample(EncodedSample& sample, bool& endOfFile) override;
    bool Seek(double seconds, double& decodeStartSeconds) override;
    const std::wstring& LastError() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace movieplayer::codec::mkv
