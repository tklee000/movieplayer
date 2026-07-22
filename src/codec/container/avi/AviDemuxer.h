#pragma once

#include "codec/container/MediaDemuxer.h"

#include <memory>

namespace movieplayer::codec::avi {

// Classic RIFF AVI reader for indexed Xvid/DX50 + MP3 files.  It keeps only
// compact idx1 metadata in memory and reads media chunks on demand.
class AviDemuxer final : public IMediaDemuxer {
public:
    AviDemuxer();
    ~AviDemuxer() override;

    AviDemuxer(const AviDemuxer&) = delete;
    AviDemuxer& operator=(const AviDemuxer&) = delete;

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

}  // namespace movieplayer::codec::avi
