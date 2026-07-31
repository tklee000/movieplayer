#pragma once

#include "codec/container/MediaDemuxer.h"

#include <memory>

namespace movieplayer::codec::ts {

class TsDemuxer final : public IMediaDemuxer {
public:
    TsDemuxer();
    ~TsDemuxer() override;

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

}  // namespace movieplayer::codec::ts
