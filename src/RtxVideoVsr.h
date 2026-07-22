#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>

#include <memory>
#include <string>

// Small lifetime-safe wrapper around the DirectX 11 RTX Video SDK VSR
// feature. The SDK is initialized once for the renderer's D3D11 device and
// evaluation is serialized by the renderer.
class RtxVideoVsr final {
public:
    RtxVideoVsr();
    ~RtxVideoVsr();

    RtxVideoVsr(const RtxVideoVsr&) = delete;
    RtxVideoVsr& operator=(const RtxVideoVsr&) = delete;
    RtxVideoVsr(RtxVideoVsr&&) = delete;
    RtxVideoVsr& operator=(RtxVideoVsr&&) = delete;

    // Failure only means that VSR is unavailable. The owning renderer can
    // continue using its normal D3D11 scaling path.
    bool Initialize(ID3D11Device* device);
    void Shutdown();

    bool Evaluate(ID3D11Texture2D* output, const RECT& outputRect,
                  ID3D11Texture2D* input, const RECT& inputRect);

    bool IsAvailable() const noexcept;
    const std::wstring& Status() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
