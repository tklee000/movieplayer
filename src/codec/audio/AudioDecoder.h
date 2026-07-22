#pragma once

#include "codec/core/CodecTypes.h"

#include <string>

namespace movieplayer::codec {

// Container-independent audio decoder contract. Codec-specific syntax stays
// below audio/<codec>, while PlayerEngine consumes interleaved float PCM.
class IAudioDecoder {
public:
    virtual ~IAudioDecoder() = default;

    virtual bool Initialize(const TrackInfo& track) = 0;
    virtual bool Decode(const EncodedSample& sample, AudioFrame& frame) = 0;
    virtual void Reset() = 0;
    virtual const std::wstring& Description() const noexcept = 0;
    virtual const std::wstring& LastError() const noexcept = 0;
};

}  // namespace movieplayer::codec
