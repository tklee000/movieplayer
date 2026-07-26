#pragma once

#include "codec/audio/AudioDecoder.h"

#include <memory>

namespace movieplayer::codec::opus {

class OpusDecoder final : public IAudioDecoder {
public:
    OpusDecoder();
    ~OpusDecoder() override;

    OpusDecoder(const OpusDecoder&) = delete;
    OpusDecoder& operator=(const OpusDecoder&) = delete;

    bool Initialize(const TrackInfo& track) override;
    bool Decode(const EncodedSample& sample, AudioFrame& frame) override;
    void Reset() override;
    const std::wstring& Description() const noexcept override;
    const std::wstring& LastError() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace movieplayer::codec::opus
