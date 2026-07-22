#pragma once

#include "codec/core/CodecTypes.h"

#include <d3d11.h>

#include <memory>
#include <string>
#include <vector>

namespace movieplayer::codec {

// Container-independent contract shared by the H.264 and HEVC decoders.
class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;

    virtual bool Initialize(ID3D11Device* device, const TrackInfo& track) = 0;
    virtual bool Decode(const EncodedSample& sample,
                        std::vector<std::shared_ptr<VideoFrame>>& frames) = 0;
    virtual bool Flush(std::vector<std::shared_ptr<VideoFrame>>& frames) = 0;
    virtual bool Reset() = 0;
    virtual const std::wstring& Description() const noexcept = 0;
    virtual const std::wstring& LastError() const noexcept = 0;
};

}  // namespace movieplayer::codec
