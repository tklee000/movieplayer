#pragma once

#include "codec/video/VideoDecoder.h"

#include <memory>

namespace movieplayer::codec::h264 {

// Feeds Annex-B access units to the Windows Media Foundation H.264 transform
// and uploads its NV12 output to textures owned by the player's D3D11 device.
// The transform uses DXVA when the installed Windows decoder and GPU allow it,
// with the Windows software decoder as a compatibility fallback.
class MfH264Decoder final : public IVideoDecoder {
public:
    MfH264Decoder();
    ~MfH264Decoder() override;

    MfH264Decoder(const MfH264Decoder&) = delete;
    MfH264Decoder& operator=(const MfH264Decoder&) = delete;

    bool Initialize(ID3D11Device* device, const TrackInfo& track) override;
    bool Decode(const EncodedSample& sample,
                std::vector<std::shared_ptr<VideoFrame>>& frames) override;
    bool Flush(std::vector<std::shared_ptr<VideoFrame>>& frames) override;
    bool Reset() override;
    const std::wstring& Description() const noexcept override;
    const std::wstring& LastError() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace movieplayer::codec::h264
