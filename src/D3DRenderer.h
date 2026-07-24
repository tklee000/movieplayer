#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>

#include <memory>
#include <string>

#include "codec/core/CodecTypes.h"

// Win32 D3D11 video renderer. The decoder and renderer share Device(), so
// decoded NV12/P010 surfaces stay on the GPU through presentation.
class D3DRenderer final {
public:
    D3DRenderer();
    ~D3DRenderer();

    D3DRenderer(const D3DRenderer&) = delete;
    D3DRenderer& operator=(const D3DRenderer&) = delete;
    D3DRenderer(D3DRenderer&&) = delete;
    D3DRenderer& operator=(D3DRenderer&&) = delete;

    bool Initialize(HWND window);

    // Recreates the swap-chain back buffer from the current client area.
    // A minimized (zero-sized) window is accepted and simply defers rendering.
    bool Resize();

    // Convenience overload for WM_SIZE.  Zero dimensions behave like a
    // minimized window; non-zero dimensions are used directly.
    bool Resize(UINT width, UINT height);

    ID3D11Device* Device() const noexcept;

    // Renders and presents one decoded NV12 or P010 frame.
    bool RenderFrame(const movieplayer::codec::VideoFrame& frame);

    // Drawn after HDR/SDR video conversion and before Present. Script-aware
    // Windows UI fonts keep CJK, Devanagari, and right-to-left text readable.
    void SetSubtitleText(const std::wstring& text);
    void SetSubtitleBitmap(
        std::shared_ptr<const movieplayer::codec::SubtitleBitmap> bitmap);
    void SetSubtitleFontFamily(const std::wstring& fontFamily);
    void SetFullscreenSubtitleScale(bool enabled);

    // Enables NVIDIA RTX Video Super Resolution for frames that are being
    // enlarged. Returns false when the SDK, GPU, driver, or feature runtime is
    // unavailable; normal D3D11 scaling remains usable in that case.
    bool SetRtxVideoUpscalingEnabled(bool enabled);
    bool IsRtxVideoUpscalingEnabled() const;
    bool IsRtxVideoUpscalingAvailable() const;
    std::wstring RtxVideoUpscalingStatus() const;

    // Presents a black frame.  Errors, if any, are available through
    // LastError().
    void Clear();

    const std::wstring& LastError() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
