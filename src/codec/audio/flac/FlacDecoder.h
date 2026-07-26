#pragma once

#include "codec/audio/AudioDecoder.h"

#include <memory>

namespace movieplayer::codec::flac {

// Native IETF RFC 9639 FLAC decoder. Matroska supplies one complete FLAC
// frame per EncodedSample; decoded integer PCM is returned as interleaved
// stereo float PCM for the player audio pipeline.
class FlacDecoder final : public IAudioDecoder {
public:
    FlacDecoder();
    ~FlacDecoder() override;

    FlacDecoder(const FlacDecoder&) = delete;
    FlacDecoder& operator=(const FlacDecoder&) = delete;

    bool Initialize(const TrackInfo& track) override;
    bool Decode(const EncodedSample& sample, AudioFrame& frame) override;
    void Reset() override;
    const std::wstring& Description() const noexcept override;
    const std::wstring& LastError() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace movieplayer::codec::flac
