#pragma once

#include "codec/core/CodecTypes.h"

#include <memory>
#include <string>
#include <vector>

namespace movieplayer::codec {

// Container-neutral interface. A future Matroska parser or another ISO BMFF
// variant can be added without changing PlayerEngine or the codec modules.
class IMediaDemuxer {
public:
    virtual ~IMediaDemuxer() = default;

    virtual bool Open(const std::wstring& path) = 0;
    virtual void Close() = 0;
    virtual const std::vector<TrackInfo>& Tracks() const noexcept = 0;
    virtual double DurationSeconds() const noexcept = 0;
    virtual bool SetTrackEnabled(std::uint32_t trackId, bool enabled) = 0;
    virtual bool ReadNextSample(EncodedSample& sample, bool& endOfFile) = 0;
    virtual bool Seek(double seconds, double& decodeStartSeconds) = 0;
    virtual const std::wstring& LastError() const noexcept = 0;
};

// Chooses the built-in container reader from the path extension. Content is
// still validated by the selected reader before tracks are exposed.
std::unique_ptr<IMediaDemuxer> CreateMediaDemuxer(const std::wstring& path);

}  // namespace movieplayer::codec
