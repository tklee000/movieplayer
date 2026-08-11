#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>

#include <memory>
#include <string>

// NVIDIA Optical Flow SDK FRUC wrapper. The implementation is compiled as a
// runtime-unavailable stub when the separately licensed SDK header is absent.
// When present, it owns the three shared ARGB surfaces required by NvOFFRUC:
// two alternating source frames and one interpolated destination frame.
class NvidiaFrameInterpolator final {
public:
    NvidiaFrameInterpolator();
    ~NvidiaFrameInterpolator();

    NvidiaFrameInterpolator(const NvidiaFrameInterpolator&) = delete;
    NvidiaFrameInterpolator& operator=(const NvidiaFrameInterpolator&) = delete;

    bool Initialize(ID3D11Device* device);
    void Shutdown();

    // Recreates and registers source-sized ARGB resources when dimensions
    // change. The caller renders the decoded frame into InputTexture().
    bool Prepare(UINT width, UINT height);
    ID3D11Texture2D* InputTexture() const noexcept;
    ID3D11RenderTargetView* InputRenderTarget() const noexcept;

    // Submits the current input exactly once. On the first submission FRUC has
    // no previous frame, so hasHistory is false and the caller presents the
    // original. Later submissions produce a midpoint frame.
    bool Process(double inputTimestamp, double outputTimestamp,
                 bool& hasHistory);

    ID3D11Texture2D* CurrentTexture() const noexcept;
    ID3D11ShaderResourceView* CurrentView() const noexcept;
    ID3D11Texture2D* InterpolatedTexture() const noexcept;
    ID3D11ShaderResourceView* InterpolatedView() const noexcept;

    // Drops FRUC's cached previous frame after seek/discontinuity.
    void ResetHistory();

    bool IsAvailable() const noexcept;
    const std::wstring& Status() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
