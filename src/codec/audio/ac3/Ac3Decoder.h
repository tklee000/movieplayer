#pragma once

#include "codec/audio/AudioDecoder.h"

#include <memory>

namespace movieplayer::codec::ac3 {

// Native ATSC A/52 AC-3 decoder. It accepts one complete raw AC-3 syncframe
// per EncodedSample and returns interleaved stereo float PCM.
class Ac3Decoder final : public IAudioDecoder {
public:
    Ac3Decoder();
    ~Ac3Decoder() override;

    Ac3Decoder(const Ac3Decoder&) = delete;
    Ac3Decoder& operator=(const Ac3Decoder&) = delete;

    bool Initialize(const TrackInfo& track) override;
    bool Decode(const EncodedSample& sample, AudioFrame& frame) override;
    void Reset() override;
    const std::wstring& Description() const noexcept override;
    const std::wstring& LastError() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace movieplayer::codec::ac3
