#pragma once

#include "codec/container/MediaDemuxer.h"
#include "codec/core/RandomAccessFile.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace movieplayer::codec::mp4 {

class Mp4Demuxer final : public IMediaDemuxer {
public:
    Mp4Demuxer();
    ~Mp4Demuxer() override;

    Mp4Demuxer(const Mp4Demuxer&) = delete;
    Mp4Demuxer& operator=(const Mp4Demuxer&) = delete;

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

}  // namespace movieplayer::codec::mp4
