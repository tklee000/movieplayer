#pragma once

#include "codec/audio/AudioDecoder.h"

#include <memory>

namespace movieplayer::codec::directshow {

// Adapter for registered external DirectShow E-AC-3 and DTS decoders.
// MoviePlayer supplies demuxed compressed packets and receives PCM without
// shipping or linking a particular third-party codec implementation.
class DirectShowAudioDecoder final : public IAudioDecoder {
public:
    DirectShowAudioDecoder();
    ~DirectShowAudioDecoder() override;

    DirectShowAudioDecoder(const DirectShowAudioDecoder&) = delete;
    DirectShowAudioDecoder& operator=(const DirectShowAudioDecoder&) = delete;

    bool Initialize(const TrackInfo& track) override;
    bool Decode(const EncodedSample& sample, AudioFrame& frame) override;
    void Reset() override;
    const std::wstring& Description() const noexcept override;
    const std::wstring& LastError() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace movieplayer::codec::directshow
