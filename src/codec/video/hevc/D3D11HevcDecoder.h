#pragma once

#include "codec/video/VideoDecoder.h"
#include "codec/video/hevc/HevcBitstreamParser.h"

#include <memory>

namespace movieplayer::codec::hevc {

class D3D11HevcDecoder final : public IVideoDecoder {
public:
    D3D11HevcDecoder();
    ~D3D11HevcDecoder() override;

    D3D11HevcDecoder(const D3D11HevcDecoder&) = delete;
    D3D11HevcDecoder& operator=(const D3D11HevcDecoder&) = delete;

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

}  // namespace movieplayer::codec::hevc
