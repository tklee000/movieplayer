#pragma once

#include "codec/audio/AudioDecoder.h"

#include <memory>

namespace movieplayer::codec::mp3 {

class MfMp3Decoder final : public IAudioDecoder {
public:
    MfMp3Decoder();
    ~MfMp3Decoder() override;

    MfMp3Decoder(const MfMp3Decoder&) = delete;
    MfMp3Decoder& operator=(const MfMp3Decoder&) = delete;

    bool Initialize(const TrackInfo& track) override;
    bool Decode(const EncodedSample& sample, AudioFrame& frame) override;
    void Reset() override;
    const std::wstring& Description() const noexcept override;
    const std::wstring& LastError() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace movieplayer::codec::mp3
