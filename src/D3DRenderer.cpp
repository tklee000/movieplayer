#include "D3DRenderer.h"
#include "RtxVideoVsr.h"

#include <d3d10.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <vector>

#pragma comment(lib, "d3d10.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

using Microsoft::WRL::ComPtr;

namespace {

constexpr DXGI_FORMAT kSwapChainFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr DXGI_FORMAT kVsrInputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kHdrPqFormat = DXGI_FORMAT_R10G10B10A2_UNORM;

std::wstring Utf8ToWide(const char* text)
{
    if (!text || !*text) {
        return {};
    }

    const int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (count <= 1) {
        std::wstring result;
        while (*text) {
            result.push_back(static_cast<unsigned char>(*text++));
        }
        return result;
    }

    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, &result[0], count);
    result.resize(static_cast<size_t>(count - 1));
    return result;
}

std::wstring HResultText(HRESULT hr)
{
    wchar_t* systemText = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageW(flags, nullptr, static_cast<DWORD>(hr),
                                        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                        reinterpret_cast<wchar_t*>(&systemText), 0, nullptr);

    std::wstring message;
    if (length && systemText) {
        message.assign(systemText, length);
        while (!message.empty() &&
               (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
            message.pop_back();
        }
    }
    if (systemText) {
        LocalFree(systemText);
    }

    std::wostringstream stream;
    if (!message.empty()) {
        stream << message << L" ";
    }
    stream << L"(HRESULT 0x" << std::uppercase << std::hex
           << std::setw(8) << std::setfill(L'0') << static_cast<unsigned long>(hr) << L")";
    return stream.str();
}

bool IsHdrTransfer(const movieplayer::codec::VideoFrame& frame)
{
    return frame.color.transfer == movieplayer::codec::TransferCharacteristic::Pq ||
           frame.color.transfer == movieplayer::codec::TransferCharacteristic::Hlg;
}

bool IsBt2020(const movieplayer::codec::VideoFrame& frame)
{
    using namespace movieplayer::codec;
    return frame.color.primaries == ColorPrimaries::Bt2020 ||
           frame.color.matrix == ColorMatrix::Bt2020NonConstant ||
           frame.color.matrix == ColorMatrix::Bt2020Constant;
}

bool IsRgbFormat(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return true;
    default:
        return false;
    }
}

bool SelectInputColorSpace(const movieplayer::codec::VideoFrame& frame,
                           DXGI_FORMAT textureFormat,
                           DXGI_COLOR_SPACE_TYPE& colorSpace)
{
    using namespace movieplayer::codec;
    const bool full = frame.color.range == ColorRange::Full;
    const bool bt2020 = IsBt2020(frame) || IsHdrTransfer(frame);
    const bool rgb = IsRgbFormat(textureFormat);

    if (frame.color.transfer == TransferCharacteristic::Pq) {
        colorSpace = rgb ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                         : ((frame.color.chromaLocation == ChromaLocation::TopLeft)
                                ? DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020
                                : DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020);
        // DXGI has no full-range PQ YCbCr value.  Falling back to the software
        // path is preferable to displaying the wrong range.
        return rgb || !full;
    }

    if (frame.color.transfer == TransferCharacteristic::Hlg) {
        if (rgb) {
            return false; // DXGI 1.4 defines HLG only for YCbCr inputs.
        }
        colorSpace = full ? DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020
                          : DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020;
        return true;
    }

    if (rgb) {
        colorSpace = bt2020 ? DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020
                            : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
        return true;
    }

    if (bt2020) {
        colorSpace = full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020
                          : ((frame.color.chromaLocation == ChromaLocation::TopLeft)
                                 ? DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_TOPLEFT_P2020
                                 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020);
    } else if (frame.color.matrix == ColorMatrix::Bt709 ||
               frame.color.primaries == ColorPrimaries::Bt709 ||
               frame.color.matrix == ColorMatrix::Unspecified) {
        colorSpace = full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
                          : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    } else {
        colorSpace = full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_NONE_P709_X601
                          : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
    }
    return true;
}

RECT LetterboxRect(int sourceWidth, int sourceHeight,
                   movieplayer::codec::Rational sar,
                   UINT targetWidth, UINT targetHeight)
{
    RECT result = { 0, 0, static_cast<LONG>(targetWidth), static_cast<LONG>(targetHeight) };
    if (sourceWidth <= 0 || sourceHeight <= 0 || !targetWidth || !targetHeight) {
        return result;
    }

    double pixelAspect = 1.0;
    if (sar.IsValid()) {
        pixelAspect = sar.ToDouble(1.0);
        if (!std::isfinite(pixelAspect) || pixelAspect < 0.01 || pixelAspect > 100.0) {
            pixelAspect = 1.0;
        }
    }

    const double sourceAspect = (static_cast<double>(sourceWidth) * pixelAspect) /
                                static_cast<double>(sourceHeight);
    const double targetAspect = static_cast<double>(targetWidth) /
                                static_cast<double>(targetHeight);

    LONG width = static_cast<LONG>(targetWidth);
    LONG height = static_cast<LONG>(targetHeight);
    if (sourceAspect > targetAspect) {
        height = static_cast<LONG>(std::llround(static_cast<double>(targetWidth) / sourceAspect));
    } else {
        width = static_cast<LONG>(std::llround(static_cast<double>(targetHeight) * sourceAspect));
    }

    width = (std::max)(1L, (std::min)(width, static_cast<LONG>(targetWidth)));
    height = (std::max)(1L, (std::min)(height, static_cast<LONG>(targetHeight)));
    result.left = (static_cast<LONG>(targetWidth) - width) / 2;
    result.top = (static_cast<LONG>(targetHeight) - height) / 2;
    result.right = result.left + width;
    result.bottom = result.top + height;
    return result;
}

bool SameComObject(IUnknown* first, IUnknown* second)
{
    if (!first || !second) {
        return false;
    }
    ComPtr<IUnknown> firstIdentity;
    ComPtr<IUnknown> secondIdentity;
    if (FAILED(first->QueryInterface(IID_PPV_ARGS(&firstIdentity))) ||
        FAILED(second->QueryInterface(IID_PPV_ARGS(&secondIdentity)))) {
        return false;
    }
    return firstIdentity.Get() == secondIdentity.Get();
}

} // namespace

struct D3DRenderer::Impl {
    HWND window = nullptr;
    UINT clientWidth = 0;
    UINT clientHeight = 0;
    std::wstring lastError;
    std::mutex mutex;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D10Multithread> multithread;
    ComPtr<IDXGISwapChain> swapChain;
    ComPtr<ID3D11Texture2D> backBuffer;
    ComPtr<ID3D11RenderTargetView> renderTarget;

    std::wstring subtitleText;
    std::shared_ptr<const movieplayer::codec::SubtitleBitmap> subtitleBitmap;
    std::wstring subtitleFontFamily;
    bool fullscreenSubtitleScale = false;
    RECT lastVideoRect = {};

    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11VideoContext> videoContext;
    ComPtr<ID3D11VideoContext1> videoContext1;
    ComPtr<ID3D11VideoProcessorEnumerator> videoEnumerator;
    ComPtr<ID3D11VideoProcessor> videoProcessor;
    ComPtr<ID3D11VideoProcessorOutputView> videoOutput;
    ComPtr<ID3D11Texture2D> videoOutputTexture;
    UINT videoInputWidth = 0;
    UINT videoInputHeight = 0;
    UINT videoOutputWidth = 0;
    UINT videoOutputHeight = 0;
    D3D11_VIDEO_FRAME_FORMAT videoFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;

    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader;
    ComPtr<ID3D11PixelShader> hdrToneMapPixelShader;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11Texture2D> hdrPqTexture;
    ComPtr<ID3D11ShaderResourceView> hdrPqView;
    UINT hdrPqWidth = 0;
    UINT hdrPqHeight = 0;
    RtxVideoVsr rtxVideoVsr;
    bool rtxVideoUpscalingEnabled = false;
    ComPtr<ID3D11Texture2D> vsrInputTexture;
    ComPtr<ID3D11RenderTargetView> vsrInputRenderTarget;
    ComPtr<ID3D11ShaderResourceView> vsrInputView;
    UINT vsrInputWidth = 0;
    UINT vsrInputHeight = 0;
    ComPtr<ID3D11Texture2D> vsrOutputTexture;
    ComPtr<ID3D11ShaderResourceView> vsrOutputView;
    UINT vsrOutputWidth = 0;
    UINT vsrOutputHeight = 0;

    ComPtr<ID3D11Texture2D> subtitleTexture;
    ComPtr<ID3D11ShaderResourceView> subtitleView;
    ComPtr<ID3D11BlendState> subtitleBlendState;
    UINT subtitleTextureWidth = 0;
    UINT subtitleTextureHeight = 0;
    std::wstring renderedSubtitleText;
    std::shared_ptr<const movieplayer::codec::SubtitleBitmap>
        renderedSubtitleBitmap;
    RECT renderedSubtitleVideoRect = {};
    float renderedSubtitleFontSize = 0.0f;

    ~Impl()
    {
        rtxVideoVsr.Shutdown();
        if (context) {
            context->ClearState();
            context->Flush();
        }
    }

    bool Fail(const wchar_t* operation, HRESULT hr)
    {
        lastError = operation;
        lastError += L": ";
        lastError += HResultText(hr);
        return false;
    }

    bool Fail(const std::wstring& message)
    {
        lastError = message;
        return false;
    }

    void InvalidateVideoProcessor()
    {
        videoOutput.Reset();
        videoOutputTexture.Reset();
        videoProcessor.Reset();
        videoEnumerator.Reset();
        videoInputWidth = 0;
        videoInputHeight = 0;
        videoOutputWidth = 0;
        videoOutputHeight = 0;
        videoFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    }

    void ResetD3D()
    {
        rtxVideoUpscalingEnabled = false;
        rtxVideoVsr.Shutdown();
        if (context) {
            context->ClearState();
            context->Flush();
        }
        InvalidateVideoProcessor();
        hdrPqView.Reset();
        hdrPqTexture.Reset();
        hdrPqWidth = hdrPqHeight = 0;
        vsrInputView.Reset();
        vsrInputRenderTarget.Reset();
        vsrInputTexture.Reset();
        vsrInputWidth = vsrInputHeight = 0;
        vsrOutputView.Reset();
        vsrOutputTexture.Reset();
        vsrOutputWidth = vsrOutputHeight = 0;
        subtitleView.Reset();
        subtitleTexture.Reset();
        subtitleBlendState.Reset();
        subtitleTextureWidth = subtitleTextureHeight = 0;
        renderedSubtitleText.clear();
        renderedSubtitleBitmap.reset();
        sampler.Reset();
        hdrToneMapPixelShader.Reset();
        pixelShader.Reset();
        vertexShader.Reset();
        renderTarget.Reset();
        backBuffer.Reset();
        swapChain.Reset();
        videoContext1.Reset();
        videoContext.Reset();
        videoDevice.Reset();
        multithread.Reset();
        context.Reset();
        device.Reset();
        clientWidth = clientHeight = 0;
        window = nullptr;
    }

    bool BuildGdiSubtitleTexture(UINT width, UINT height, float fontSize)
    {
        if (!width || !height) {
            return false;
        }
        HDC dc = CreateCompatibleDC(nullptr);
        if (!dc) {
            return Fail(L"CreateCompatibleDC failed for subtitle rendering");
        }
        BITMAPINFO bitmapInfo = {};
        bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(width);
        bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(height); // top-down BGRA
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;
        void* dibBits = nullptr;
        HBITMAP bitmap = CreateDIBSection(dc, &bitmapInfo, DIB_RGB_COLORS,
                                          &dibBits, nullptr, 0);
        if (!bitmap || !dibBits) {
            DeleteDC(dc);
            return Fail(L"CreateDIBSection failed for subtitle rendering");
        }
        HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
        memset(dibBits, 0, static_cast<size_t>(width) * height * 4U);

        const bool hasDevanagari = std::any_of(
            subtitleText.begin(), subtitleText.end(), [](wchar_t character) {
                return character >= 0x0900 && character <= 0x097F;
            });
        const bool hasCjk = std::any_of(
            subtitleText.begin(), subtitleText.end(), [](wchar_t character) {
                return (character >= 0x3040 && character <= 0x30FF) ||
                       (character >= 0x3400 && character <= 0x9FFF) ||
                       (character >= 0xAC00 && character <= 0xD7AF);
            });
        const bool rightToLeft = std::any_of(
            subtitleText.begin(), subtitleText.end(), [](wchar_t character) {
                return (character >= 0x0590 && character <= 0x08FF) ||
                       (character >= 0xFB1D && character <= 0xFDFF) ||
                       (character >= 0xFE70 && character <= 0xFEFF);
            });
        const wchar_t* fontFamily =
            hasDevanagari
                ? L"Nirmala UI"
                : !subtitleFontFamily.empty()
                      ? subtitleFontFamily.c_str()
                      : hasCjk ? L"Malgun Gothic" : L"Segoe UI";
        HFONT font = CreateFontW(-static_cast<int>(std::lround(fontSize)), 0, 0, 0,
                                 FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                 fontFamily);
        if (!font) {
            font = CreateFontW(-static_cast<int>(std::lround(fontSize)), 0, 0, 0,
                               FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                               L"Segoe UI");
        }
        HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));

        const int padding = (std::max)(8, static_cast<int>(std::lround(fontSize * 0.18f)));
        RECT calculate = {padding, 0, static_cast<LONG>(width) - padding,
                          static_cast<LONG>(height)};
        const UINT drawFlags = DT_CENTER | DT_WORDBREAK | DT_NOPREFIX |
                               DT_EDITCONTROL |
                               (rightToLeft ? DT_RTLREADING : 0);
        DrawTextW(dc, subtitleText.c_str(), static_cast<int>(subtitleText.size()),
                  &calculate, drawFlags | DT_CALCRECT);
        const LONG textHeight = (std::max)(1L, calculate.bottom - calculate.top);
        const LONG drawBottom = static_cast<LONG>(height) - padding;
        const LONG drawTop = (std::max)(static_cast<LONG>(padding),
                                        drawBottom - textHeight);
        RECT drawRect = {padding,
                         drawTop,
                         static_cast<LONG>(width) - padding,
                         drawBottom};
        DrawTextW(dc, subtitleText.c_str(), static_cast<int>(subtitleText.size()),
                  &drawRect, drawFlags);

        const size_t pixelCount = static_cast<size_t>(width) * height;
        const auto* source = static_cast<const uint8_t*>(dibBits);
        std::vector<uint8_t> mask(pixelCount, 0);
        for (size_t i = 0; i < pixelCount; ++i) {
            mask[i] = (std::max)(source[i * 4],
                                 (std::max)(source[i * 4 + 1], source[i * 4 + 2]));
        }
        std::vector<uint8_t> output(pixelCount * 4U, 0);
        const int radius = (std::max)(2, static_cast<int>(std::lround(fontSize / 18.0f)));
        for (UINT y = 0; y < height; ++y) {
            for (UINT x = 0; x < width; ++x) {
                const uint8_t value = mask[static_cast<size_t>(y) * width + x];
                if (!value) {
                    continue;
                }
                const int minY = (std::max)(0, static_cast<int>(y) - radius);
                const int maxY = (std::min)(static_cast<int>(height) - 1,
                                            static_cast<int>(y) + radius);
                const int minX = (std::max)(0, static_cast<int>(x) - radius);
                const int maxX = (std::min)(static_cast<int>(width) - 1,
                                            static_cast<int>(x) + radius);
                for (int outlineY = minY; outlineY <= maxY; ++outlineY) {
                    for (int outlineX = minX; outlineX <= maxX; ++outlineX) {
                        const int dx = outlineX - static_cast<int>(x);
                        const int dy = outlineY - static_cast<int>(y);
                        if (dx * dx + dy * dy > radius * radius) {
                            continue;
                        }
                        uint8_t& alpha = output[(static_cast<size_t>(outlineY) * width +
                                                 static_cast<UINT>(outlineX)) * 4U + 3U];
                        alpha = (std::max)(alpha, value);
                    }
                }
            }
        }
        for (size_t i = 0; i < pixelCount; ++i) {
            const uint8_t value = mask[i];
            output[i * 4] = value;
            output[i * 4 + 1] = value;
            output[i * 4 + 2] = value;
            output[i * 4 + 3] = (std::max)(output[i * 4 + 3], value);
        }
        if (oldFont) {
            SelectObject(dc, oldFont);
        }
        if (font) {
            DeleteObject(font);
        }
        SelectObject(dc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);

        if (!subtitleTexture || subtitleTextureWidth != width ||
            subtitleTextureHeight != height) {
            subtitleView.Reset();
            subtitleTexture.Reset();
            D3D11_TEXTURE2D_DESC textureDesc = {};
            textureDesc.Width = width;
            textureDesc.Height = height;
            textureDesc.MipLevels = 1;
            textureDesc.ArraySize = 1;
            textureDesc.Format = kSwapChainFormat;
            textureDesc.SampleDesc.Count = 1;
            textureDesc.Usage = D3D11_USAGE_DYNAMIC;
            textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            textureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            HRESULT hr = device->CreateTexture2D(&textureDesc, nullptr, &subtitleTexture);
            if (FAILED(hr)) {
                return Fail(L"ID3D11Device::CreateTexture2D(subtitle)", hr);
            }
            hr = device->CreateShaderResourceView(subtitleTexture.Get(), nullptr, &subtitleView);
            if (FAILED(hr)) {
                return Fail(L"ID3D11Device::CreateShaderResourceView(subtitle)", hr);
            }
            subtitleTextureWidth = width;
            subtitleTextureHeight = height;
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = context->Map(subtitleTexture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) {
            return Fail(L"ID3D11DeviceContext::Map(subtitle)", hr);
        }
        const size_t rowBytes = static_cast<size_t>(width) * 4U;
        for (UINT y = 0; y < height; ++y) {
            memcpy(static_cast<uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                   output.data() + static_cast<size_t>(y) * rowBytes, rowBytes);
        }
        context->Unmap(subtitleTexture.Get(), 0);
        renderedSubtitleText = subtitleText;
        renderedSubtitleBitmap.reset();
        renderedSubtitleVideoRect = lastVideoRect;
        return true;
    }

    bool BuildBitmapSubtitleTexture()
    {
        if (!subtitleBitmap || subtitleBitmap->width <= 0 ||
            subtitleBitmap->height <= 0) {
            return false;
        }
        const UINT width = static_cast<UINT>(subtitleBitmap->width);
        const UINT height = static_cast<UINT>(subtitleBitmap->height);
        const size_t rowBytes = static_cast<size_t>(width) * 4U;
        if (subtitleBitmap->bgra.size() != rowBytes * height) {
            return Fail(L"The decoded subtitle bitmap has an invalid size");
        }
        if (!subtitleTexture || subtitleTextureWidth != width ||
            subtitleTextureHeight != height) {
            subtitleView.Reset();
            subtitleTexture.Reset();
            D3D11_TEXTURE2D_DESC textureDesc = {};
            textureDesc.Width = width;
            textureDesc.Height = height;
            textureDesc.MipLevels = 1;
            textureDesc.ArraySize = 1;
            textureDesc.Format = kSwapChainFormat;
            textureDesc.SampleDesc.Count = 1;
            textureDesc.Usage = D3D11_USAGE_DYNAMIC;
            textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            textureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            HRESULT hr = device->CreateTexture2D(&textureDesc, nullptr,
                                                  &subtitleTexture);
            if (FAILED(hr)) {
                return Fail(L"ID3D11Device::CreateTexture2D(bitmap subtitle)",
                            hr);
            }
            hr = device->CreateShaderResourceView(subtitleTexture.Get(), nullptr,
                                                  &subtitleView);
            if (FAILED(hr)) {
                return Fail(L"ID3D11Device::CreateShaderResourceView(bitmap subtitle)",
                            hr);
            }
            subtitleTextureWidth = width;
            subtitleTextureHeight = height;
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        const HRESULT hr = context->Map(subtitleTexture.Get(), 0,
                                        D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) {
            return Fail(L"ID3D11DeviceContext::Map(bitmap subtitle)", hr);
        }
        for (UINT y = 0; y < height; ++y) {
            memcpy(static_cast<uint8_t*>(mapped.pData) +
                       static_cast<size_t>(y) * mapped.RowPitch,
                   subtitleBitmap->bgra.data() +
                       static_cast<size_t>(y) * rowBytes,
                   rowBytes);
        }
        context->Unmap(subtitleTexture.Get(), 0);
        renderedSubtitleBitmap = subtitleBitmap;
        renderedSubtitleText.clear();
        return true;
    }

    bool CompositeSubtitleTexture(const D3D11_VIEWPORT& viewport)
    {
        const float blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        context->OMSetRenderTargets(1, renderTarget.GetAddressOf(), nullptr);
        context->OMSetBlendState(subtitleBlendState.Get(), blendFactor,
                                 0xFFFFFFFFU);
        context->RSSetViewports(1, &viewport);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertexShader.Get(), nullptr, 0);
        context->PSSetShader(pixelShader.Get(), nullptr, 0);
        context->PSSetSamplers(0, 1, sampler.GetAddressOf());
        context->PSSetShaderResources(0, 1, subtitleView.GetAddressOf());
        context->Draw(3, 0);
        ID3D11ShaderResourceView* nullView = nullptr;
        context->PSSetShaderResources(0, 1, &nullView);
        context->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFFU);
        return true;
    }

    bool DrawSubtitleD3D()
    {
        if (subtitleText.empty() && !subtitleBitmap) {
            return true;
        }
        RECT videoRect = lastVideoRect;
        if (videoRect.right <= videoRect.left || videoRect.bottom <= videoRect.top) {
            videoRect = {0, 0, static_cast<LONG>(clientWidth), static_cast<LONG>(clientHeight)};
        }
        const UINT videoWidth = static_cast<UINT>(videoRect.right - videoRect.left);
        const UINT videoHeight = static_cast<UINT>(videoRect.bottom - videoRect.top);
        if (subtitleBitmap) {
            if (subtitleBitmap->canvasWidth <= 0 ||
                subtitleBitmap->canvasHeight <= 0 ||
                subtitleBitmap->x < 0 || subtitleBitmap->y < 0 ||
                subtitleBitmap->x + subtitleBitmap->width >
                    subtitleBitmap->canvasWidth ||
                subtitleBitmap->y + subtitleBitmap->height >
                    subtitleBitmap->canvasHeight) {
                return Fail(L"The decoded subtitle bitmap is outside its canvas");
            }
            if (!subtitleView || renderedSubtitleBitmap != subtitleBitmap ||
                subtitleTextureWidth !=
                    static_cast<UINT>(subtitleBitmap->width) ||
                subtitleTextureHeight !=
                    static_cast<UINT>(subtitleBitmap->height)) {
                if (!BuildBitmapSubtitleTexture()) return false;
            }
            float scaleX = static_cast<float>(videoWidth) /
                           subtitleBitmap->canvasWidth;
            float scaleY = static_cast<float>(videoHeight) /
                           subtitleBitmap->canvasHeight;
            float canvasLeft = static_cast<float>(videoRect.left);
            float canvasTop = static_cast<float>(videoRect.top);
            if (subtitleBitmap->canvasWidth > 720 ||
                subtitleBitmap->canvasHeight > 576) {
                // HD VobSub canvases use square pixels and commonly retain the
                // uncropped 16:9 source size while x265 video is cropped to the
                // active film area. Preserve the glyph aspect and center-crop
                // the subtitle canvas in exactly the same way.
                const float scale = (std::max)(scaleX, scaleY);
                scaleX = scaleY = scale;
                canvasLeft +=
                    (videoWidth - subtitleBitmap->canvasWidth * scale) * 0.5f;
                canvasTop +=
                    (videoHeight - subtitleBitmap->canvasHeight * scale) * 0.5f;
            }
            D3D11_VIEWPORT viewport = {};
            viewport.TopLeftX = canvasLeft + subtitleBitmap->x * scaleX;
            viewport.TopLeftY = canvasTop + subtitleBitmap->y * scaleY;
            viewport.Width = (std::max)(1.0f,
                                        subtitleBitmap->width * scaleX);
            viewport.Height = (std::max)(1.0f,
                                         subtitleBitmap->height * scaleY);
            viewport.MinDepth = 0.0f;
            viewport.MaxDepth = 1.0f;
            return CompositeSubtitleTexture(viewport);
        }
        const UINT textureWidth = (std::max)(1U, static_cast<UINT>(std::lround(videoWidth * 0.90)));
        const UINT textureHeight = (std::max)(1U, static_cast<UINT>(std::lround(videoHeight * 0.30)));
        const float minimumFontSize = fullscreenSubtitleScale ? 26.0f : 22.0f;
        const float maximumFontSize = fullscreenSubtitleScale ? 86.0f : 58.0f;
        const float scaleDivisor = fullscreenSubtitleScale ? 15.5f : 18.0f;
        const float fontSize = (std::max)(
            minimumFontSize,
            (std::min)(maximumFontSize, videoHeight / scaleDivisor));
        if (!subtitleView || renderedSubtitleText != subtitleText ||
            subtitleTextureWidth != textureWidth || subtitleTextureHeight != textureHeight ||
            std::fabs(renderedSubtitleFontSize - fontSize) > 0.1f ||
            memcmp(&renderedSubtitleVideoRect, &videoRect, sizeof(RECT)) != 0) {
            if (!BuildGdiSubtitleTexture(textureWidth, textureHeight, fontSize)) {
                return false;
            }
            renderedSubtitleFontSize = fontSize;
        }

        const float marginBottom = (std::max)(10.0f, videoHeight * 0.045f);
        D3D11_VIEWPORT viewport = {};
        viewport.TopLeftX = static_cast<float>(videoRect.left) +
                            (videoWidth - textureWidth) * 0.5f;
        viewport.TopLeftY = (std::max)(static_cast<float>(videoRect.top),
                                      static_cast<float>(videoRect.bottom) -
                                          marginBottom - textureHeight);
        viewport.Width = static_cast<float>(textureWidth);
        viewport.Height = static_cast<float>(textureHeight);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;

        return CompositeSubtitleTexture(viewport);
    }

    bool CreateShaders()
    {
        static const char vertexSource[] = R"(
struct VSOutput {
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};

VSOutput main(uint vertexId : SV_VertexID) {
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    VSOutput output;
    output.position = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);
    output.texcoord = uv;
    return output;
}
)";

        static const char pixelSource[] = R"(
Texture2D frameTexture : register(t0);
SamplerState frameSampler : register(s0);

float4 main(float4 position : SV_POSITION, float2 texcoord : TEXCOORD0) : SV_TARGET {
    return frameTexture.Sample(frameSampler, texcoord);
}
)";

        // The video processor converts P010 YCbCr into PQ-encoded BT.2020 RGB
        // without reducing it to SDR.  This shader then performs deterministic
        // HDR10-to-SDR mapping instead of relying on vendor-specific driver
        // tone mapping.  203 nits is the ITU HDR reference white, while the
        // 1000-nit shoulder matches the common HDR10 mastering target and the
        // bundled 4K regression title.
        static const char hdrToneMapPixelSource[] = R"(
Texture2D frameTexture : register(t0);
SamplerState frameSampler : register(s0);

float3 PqToNits(float3 value) {
    const float m1 = 2610.0f / 16384.0f;
    const float m2 = 2523.0f / 32.0f;
    const float c1 = 3424.0f / 4096.0f;
    const float c2 = 2413.0f / 128.0f;
    const float c3 = 2392.0f / 128.0f;
    float3 p = pow(saturate(value), 1.0f / m2);
    float3 numerator = max(p - c1, 0.0f);
    float3 denominator = max(c2 - c3 * p, 1.0e-6f);
    return pow(numerator / denominator, 1.0f / m1) * 10000.0f;
}

float3 Bt2020ToBt709(float3 value) {
    return float3(
        1.660496f * value.r - 0.587656f * value.g - 0.072840f * value.b,
       -0.124547f * value.r + 1.132895f * value.g - 0.008348f * value.b,
       -0.018154f * value.r - 0.100597f * value.g + 1.118751f * value.b);
}

float4 main(float4 position : SV_POSITION, float2 texcoord : TEXCOORD0) : SV_TARGET {
    const float sdrReferenceWhiteNits = 203.0f;
    const float masteringPeakNits = 1000.0f;
    float3 linear709 = max(Bt2020ToBt709(PqToNits(
        frameTexture.Sample(frameSampler, texcoord).rgb)), 0.0f);
    linear709 /= sdrReferenceWhiteNits;

    float luminance = dot(linear709, float3(0.2126f, 0.7152f, 0.0722f));
    float whitePoint = masteringPeakNits / sdrReferenceWhiteNits;
    float mappedLuminance = luminance *
        (1.0f + luminance / (whitePoint * whitePoint)) /
        (1.0f + luminance);
    float3 mapped = luminance > 1.0e-6f
        ? linear709 * (mappedLuminance / luminance)
        : 0.0f;

    // The swap chain advertises G22 BT.709, so write gamma-2.2 encoded RGB.
    float3 output = pow(saturate(mapped), 1.0f / 2.2f);
    return float4(output, 1.0f);
}
)";

        const UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
        ComPtr<ID3DBlob> vertexBytecode;
        ComPtr<ID3DBlob> pixelBytecode;
        ComPtr<ID3DBlob> errors;

        HRESULT hr = D3DCompile(vertexSource, sizeof(vertexSource) - 1, "D3DRendererVS",
                                nullptr, nullptr, "main", "vs_4_0", compileFlags, 0,
                                &vertexBytecode, &errors);
        if (FAILED(hr)) {
            lastError = L"Vertex shader compilation failed: ";
            lastError += errors ? Utf8ToWide(static_cast<const char*>(errors->GetBufferPointer()))
                                : HResultText(hr);
            return false;
        }

        errors.Reset();
        hr = D3DCompile(pixelSource, sizeof(pixelSource) - 1, "D3DRendererPS",
                        nullptr, nullptr, "main", "ps_4_0", compileFlags, 0,
                        &pixelBytecode, &errors);
        if (FAILED(hr)) {
            lastError = L"Pixel shader compilation failed: ";
            lastError += errors ? Utf8ToWide(static_cast<const char*>(errors->GetBufferPointer()))
                                : HResultText(hr);
            return false;
        }

        ComPtr<ID3DBlob> hdrToneMapPixelBytecode;
        errors.Reset();
        hr = D3DCompile(hdrToneMapPixelSource,
                        sizeof(hdrToneMapPixelSource) - 1,
                        "D3DRendererHdrToneMapPS", nullptr, nullptr, "main",
                        "ps_4_0", compileFlags, 0,
                        &hdrToneMapPixelBytecode, &errors);
        if (FAILED(hr)) {
            lastError = L"HDR tone-map pixel shader compilation failed: ";
            lastError += errors ? Utf8ToWide(static_cast<const char*>(errors->GetBufferPointer()))
                                : HResultText(hr);
            return false;
        }

        hr = device->CreateVertexShader(vertexBytecode->GetBufferPointer(),
                                        vertexBytecode->GetBufferSize(), nullptr, &vertexShader);
        if (FAILED(hr)) {
            return Fail(L"ID3D11Device::CreateVertexShader", hr);
        }
        hr = device->CreatePixelShader(pixelBytecode->GetBufferPointer(),
                                       pixelBytecode->GetBufferSize(), nullptr, &pixelShader);
        if (FAILED(hr)) {
            return Fail(L"ID3D11Device::CreatePixelShader", hr);
        }
        hr = device->CreatePixelShader(hdrToneMapPixelBytecode->GetBufferPointer(),
                                       hdrToneMapPixelBytecode->GetBufferSize(),
                                       nullptr, &hdrToneMapPixelShader);
        if (FAILED(hr)) {
            return Fail(L"ID3D11Device::CreatePixelShader(HDR tone map)", hr);
        }

        D3D11_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
        hr = device->CreateSamplerState(&samplerDesc, &sampler);
        if (FAILED(hr)) {
            return Fail(L"ID3D11Device::CreateSamplerState", hr);
        }

        D3D11_BLEND_DESC blendDesc = {};
        blendDesc.RenderTarget[0].BlendEnable = TRUE;
        blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&blendDesc, &subtitleBlendState);
        if (FAILED(hr)) {
            return Fail(L"ID3D11Device::CreateBlendState(subtitle)", hr);
        }
        return true;
    }

    bool CreateBackBufferViews()
    {
        backBuffer.Reset();
        renderTarget.Reset();

        HRESULT hr = swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (FAILED(hr)) {
            return Fail(L"IDXGISwapChain::GetBuffer", hr);
        }
        hr = device->CreateRenderTargetView(backBuffer.Get(), nullptr, &renderTarget);
        if (FAILED(hr)) {
            backBuffer.Reset();
            return Fail(L"ID3D11Device::CreateRenderTargetView", hr);
        }

        ComPtr<IDXGISwapChain3> swapChain3;
        if (SUCCEEDED(swapChain.As(&swapChain3))) {
            UINT support = 0;
            if (SUCCEEDED(swapChain3->CheckColorSpaceSupport(
                    DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, &support)) &&
                (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
                swapChain3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
            }
        }
        return true;
    }

    bool Initialize(HWND hwnd)
    {
        ResetD3D();
        lastError.clear();
        if (!hwnd || !IsWindow(hwnd)) {
            return Fail(L"D3DRenderer::Initialize received an invalid window handle");
        }
        window = hwnd;

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        const D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        D3D_FEATURE_LEVEL createdLevel = D3D_FEATURE_LEVEL_10_0;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                       featureLevels, ARRAYSIZE(featureLevels),
                                       D3D11_SDK_VERSION, &device, &createdLevel, &context);
        if (hr == E_INVALIDARG) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   featureLevels + 1, ARRAYSIZE(featureLevels) - 1,
                                   D3D11_SDK_VERSION, &device, &createdLevel, &context);
        }
        if (FAILED(hr)) {
            // WARP keeps the player usable on Remote Desktop/VMs.  It is a
            // software fallback and will normally have no video interfaces.
            flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                                   featureLevels + 1, ARRAYSIZE(featureLevels) - 1,
                                   D3D11_SDK_VERSION, &device, &createdLevel, &context);
        }
        if (FAILED(hr)) {
            ResetD3D();
            return Fail(L"D3D11CreateDevice", hr);
        }

        hr = context.As(&multithread);
        if (FAILED(hr) || !multithread) {
            ResetD3D();
            return Fail(L"QueryInterface(ID3D10Multithread)", FAILED(hr) ? hr : E_NOINTERFACE);
        }
        multithread->SetMultithreadProtected(TRUE);

        // These interfaces are optional on WARP, but present on a hardware
        // device created with D3D11_CREATE_DEVICE_VIDEO_SUPPORT.
        device.As(&videoDevice);
        context.As(&videoContext);
        context.As(&videoContext1);

        RECT client = {};
        GetClientRect(window, &client);
        const UINT requestedWidth = client.right > client.left
                                        ? static_cast<UINT>(client.right - client.left)
                                        : 0;
        const UINT requestedHeight = client.bottom > client.top
                                         ? static_cast<UINT>(client.bottom - client.top)
                                         : 0;

        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory> factory;
        hr = device.As(&dxgiDevice);
        if (SUCCEEDED(hr)) {
            hr = dxgiDevice->GetAdapter(&adapter);
        }
        if (SUCCEEDED(hr)) {
            hr = adapter->GetParent(IID_PPV_ARGS(&factory));
        }
        if (FAILED(hr)) {
            ResetD3D();
            return Fail(L"Obtaining the DXGI factory", hr);
        }

        DXGI_SWAP_CHAIN_DESC swapDesc = {};
        swapDesc.BufferDesc.Width = (std::max)(1U, requestedWidth);
        swapDesc.BufferDesc.Height = (std::max)(1U, requestedHeight);
        swapDesc.BufferDesc.Format = kSwapChainFormat;
        swapDesc.BufferDesc.RefreshRate.Numerator = 0;
        swapDesc.BufferDesc.RefreshRate.Denominator = 1;
        swapDesc.SampleDesc.Count = 1;
        swapDesc.SampleDesc.Quality = 0;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.BufferCount = 2;
        swapDesc.OutputWindow = window;
        swapDesc.Windowed = TRUE;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        hr = factory->CreateSwapChain(device.Get(), &swapDesc, &swapChain);
        if (FAILED(hr)) {
            ResetD3D();
            return Fail(L"IDXGIFactory::CreateSwapChain", hr);
        }
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);

        if (!CreateBackBufferViews() || !CreateShaders()) {
            ResetD3D();
            return false;
        }
        clientWidth = requestedWidth;
        clientHeight = requestedHeight;
        // VSR is optional. Initialization failure is retained as feature
        // status but must not prevent normal video playback.
        rtxVideoVsr.Initialize(device.Get());
        lastError.clear();
        return true;
    }

    bool Resize(UINT width, UINT height)
    {
        if (!device || !swapChain) {
            return Fail(L"D3DRenderer is not initialized");
        }

        clientWidth = width;
        clientHeight = height;
        InvalidateVideoProcessor();
        vsrOutputView.Reset();
        vsrOutputTexture.Reset();
        vsrOutputWidth = vsrOutputHeight = 0;
        context->OMSetRenderTargets(0, nullptr, nullptr);
        renderTarget.Reset();
        backBuffer.Reset();

        if (!width || !height) {
            lastError.clear();
            return true;
        }

        context->Flush();
        const HRESULT hr = swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            return Fail(L"IDXGISwapChain::ResizeBuffers", hr);
        }
        if (!CreateBackBufferViews()) {
            return false;
        }
        lastError.clear();
        return true;
    }

    bool Present()
    {
        if (!subtitleText.empty() || subtitleBitmap) {
            if (!DrawSubtitleD3D()) {
                return false;
            }
        }
        const HRESULT hr = swapChain->Present(1, 0);
        if (FAILED(hr)) {
            return Fail(L"IDXGISwapChain::Present", hr);
        }
        return true;
    }

    bool EnsureVideoProcessor(UINT inputWidth, UINT inputHeight,
                              D3D11_VIDEO_FRAME_FORMAT frameFormat,
                              ID3D11Texture2D* outputTexture,
                              UINT outputWidth, UINT outputHeight)
    {
        if (!videoDevice || !videoContext) {
            return Fail(L"This D3D11 device does not expose video-processing interfaces");
        }
        if (!outputTexture || !outputWidth || !outputHeight) {
            return Fail(L"The video output surface is unavailable");
        }

        if (videoEnumerator && videoProcessor && videoOutput &&
            SameComObject(videoOutputTexture.Get(), outputTexture) &&
            videoInputWidth == inputWidth && videoInputHeight == inputHeight &&
            videoOutputWidth == outputWidth && videoOutputHeight == outputHeight &&
            videoFrameFormat == frameFormat) {
            return true;
        }

        InvalidateVideoProcessor();
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
        content.InputFrameFormat = frameFormat;
        content.InputFrameRate.Numerator = 60;
        content.InputFrameRate.Denominator = 1;
        content.InputWidth = inputWidth;
        content.InputHeight = inputHeight;
        content.OutputFrameRate.Numerator = 60;
        content.OutputFrameRate.Denominator = 1;
        content.OutputWidth = outputWidth;
        content.OutputHeight = outputHeight;
        content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

        HRESULT hr = videoDevice->CreateVideoProcessorEnumerator(&content, &videoEnumerator);
        if (FAILED(hr)) {
            return Fail(L"ID3D11VideoDevice::CreateVideoProcessorEnumerator", hr);
        }
        hr = videoDevice->CreateVideoProcessor(videoEnumerator.Get(), 0, &videoProcessor);
        if (FAILED(hr)) {
            InvalidateVideoProcessor();
            return Fail(L"ID3D11VideoDevice::CreateVideoProcessor", hr);
        }

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outputDesc = {};
        outputDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        outputDesc.Texture2D.MipSlice = 0;
        hr = videoDevice->CreateVideoProcessorOutputView(outputTexture, videoEnumerator.Get(),
                                                         &outputDesc, &videoOutput);
        if (FAILED(hr)) {
            InvalidateVideoProcessor();
            return Fail(L"ID3D11VideoDevice::CreateVideoProcessorOutputView", hr);
        }

        videoInputWidth = inputWidth;
        videoInputHeight = inputHeight;
        videoOutputWidth = outputWidth;
        videoOutputHeight = outputHeight;
        videoFrameFormat = frameFormat;
        videoOutputTexture = outputTexture;
        return true;
    }

    bool EnsureVsrInputTexture(UINT width, UINT height)
    {
        if (vsrInputTexture && vsrInputRenderTarget && vsrInputView &&
            vsrInputWidth == width && vsrInputHeight == height) {
            return true;
        }

        InvalidateVideoProcessor();
        vsrInputView.Reset();
        vsrInputRenderTarget.Reset();
        vsrInputTexture.Reset();
        vsrInputWidth = vsrInputHeight = 0;

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = kVsrInputFormat;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET |
                         D3D11_BIND_SHADER_RESOURCE |
                         D3D11_BIND_UNORDERED_ACCESS;
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &vsrInputTexture);
        if (FAILED(hr)) {
            return Fail(L"ID3D11Device::CreateTexture2D(RTX VSR input)", hr);
        }
        hr = device->CreateRenderTargetView(vsrInputTexture.Get(), nullptr,
                                            &vsrInputRenderTarget);
        if (FAILED(hr)) {
            vsrInputTexture.Reset();
            return Fail(L"ID3D11Device::CreateRenderTargetView(RTX VSR input)", hr);
        }
        hr = device->CreateShaderResourceView(vsrInputTexture.Get(), nullptr,
                                              &vsrInputView);
        if (FAILED(hr)) {
            vsrInputRenderTarget.Reset();
            vsrInputTexture.Reset();
            return Fail(L"ID3D11Device::CreateShaderResourceView(RTX VSR input)", hr);
        }
        vsrInputWidth = width;
        vsrInputHeight = height;
        return true;
    }

    bool EnsureHdrPqTexture(UINT width, UINT height)
    {
        if (hdrPqTexture && hdrPqView &&
            hdrPqWidth == width && hdrPqHeight == height) {
            return true;
        }

        InvalidateVideoProcessor();
        hdrPqView.Reset();
        hdrPqTexture.Reset();
        hdrPqWidth = hdrPqHeight = 0;

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = kHdrPqFormat;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET |
                         D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &hdrPqTexture);
        if (FAILED(hr)) {
            return Fail(L"ID3D11Device::CreateTexture2D(HDR PQ intermediate)", hr);
        }
        hr = device->CreateShaderResourceView(hdrPqTexture.Get(), nullptr,
                                              &hdrPqView);
        if (FAILED(hr)) {
            hdrPqTexture.Reset();
            return Fail(L"ID3D11Device::CreateShaderResourceView(HDR PQ intermediate)", hr);
        }
        hdrPqWidth = width;
        hdrPqHeight = height;
        return true;
    }

    bool EnsureVsrOutputTexture(UINT width, UINT height)
    {
        if (vsrOutputTexture && vsrOutputView &&
            vsrOutputWidth == width && vsrOutputHeight == height) {
            return true;
        }

        vsrOutputView.Reset();
        vsrOutputTexture.Reset();
        vsrOutputWidth = vsrOutputHeight = 0;

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = kSwapChainFormat;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET |
                         D3D11_BIND_SHADER_RESOURCE |
                         D3D11_BIND_UNORDERED_ACCESS;
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &vsrOutputTexture);
        if (FAILED(hr)) {
            return Fail(L"ID3D11Device::CreateTexture2D(RTX VSR output)", hr);
        }
        hr = device->CreateShaderResourceView(vsrOutputTexture.Get(), nullptr,
                                              &vsrOutputView);
        if (FAILED(hr)) {
            vsrOutputTexture.Reset();
            return Fail(L"ID3D11Device::CreateShaderResourceView(RTX VSR output)", hr);
        }
        vsrOutputWidth = width;
        vsrOutputHeight = height;
        return true;
    }

    bool DrawTextureToTarget(ID3D11ShaderResourceView* view,
                             ID3D11RenderTargetView* target,
                             UINT targetWidth, UINT targetHeight,
                             const RECT& destination,
                             ID3D11PixelShader* shader)
    {
        if (!view || !target || !targetWidth || !targetHeight || !shader ||
            destination.right <= destination.left ||
            destination.bottom <= destination.top) {
            return Fail(L"The D3D11 texture draw parameters are invalid");
        }

        const FLOAT clearColor[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        context->ClearRenderTargetView(target, clearColor);
        context->OMSetRenderTargets(1, &target, nullptr);

        D3D11_VIEWPORT viewport = {};
        viewport.TopLeftX = static_cast<FLOAT>(destination.left);
        viewport.TopLeftY = static_cast<FLOAT>(destination.top);
        viewport.Width = static_cast<FLOAT>(destination.right - destination.left);
        viewport.Height = static_cast<FLOAT>(destination.bottom - destination.top);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        context->RSSetViewports(1, &viewport);

        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertexShader.Get(), nullptr, 0);
        context->PSSetShader(shader, nullptr, 0);
        context->PSSetSamplers(0, 1, sampler.GetAddressOf());
        context->PSSetShaderResources(0, 1, &view);
        context->Draw(3, 0);

        ID3D11ShaderResourceView* nullView = nullptr;
        context->PSSetShaderResources(0, 1, &nullView);
        return true;
    }

    bool DrawTexture(ID3D11ShaderResourceView* view, const RECT& destination,
                     ID3D11PixelShader* shader = nullptr)
    {
        return DrawTextureToTarget(view, renderTarget.Get(), clientWidth,
                                   clientHeight, destination,
                                   shader ? shader : pixelShader.Get());
    }

    bool ShouldUseVsr(int sourceWidth, int sourceHeight,
                      const RECT& destination) const
    {
        const LONG outputWidth = destination.right - destination.left;
        const LONG outputHeight = destination.bottom - destination.top;
        return rtxVideoUpscalingEnabled && rtxVideoVsr.IsAvailable() &&
               (outputWidth > sourceWidth || outputHeight > sourceHeight);
    }

    bool RenderVsrTexture(ID3D11Texture2D* input, int sourceWidth,
                          int sourceHeight, const RECT& destination)
    {
        const UINT outputWidth =
            static_cast<UINT>(destination.right - destination.left);
        const UINT outputHeight =
            static_cast<UINT>(destination.bottom - destination.top);
        if (!EnsureVsrOutputTexture(outputWidth, outputHeight)) {
            return false;
        }

        // Ensure the VSR destination is not still bound by the graphics
        // pipeline from the previous frame before NGX writes to its UAV.
        ID3D11ShaderResourceView* nullView = nullptr;
        context->PSSetShaderResources(0, 1, &nullView);
        context->OMSetRenderTargets(0, nullptr, nullptr);

        const RECT inputRect = {0, 0, sourceWidth, sourceHeight};
        const RECT outputRect = {0, 0, static_cast<LONG>(outputWidth),
                                 static_cast<LONG>(outputHeight)};
        if (!rtxVideoVsr.Evaluate(vsrOutputTexture.Get(), outputRect,
                                  input, inputRect)) {
            return false;
        }
        lastVideoRect = destination;
        return DrawTexture(vsrOutputView.Get(), destination) && Present();
    }

    bool RenderHardware(const movieplayer::codec::VideoFrame& frame)
    {
        if (!frame.texture || frame.width <= 0 || frame.height <= 0) {
            return Fail(L"The decoded frame does not contain a valid D3D11 texture");
        }

        ID3D11Texture2D* texture = frame.texture.Get();
        ComPtr<ID3D11Device> frameDevice;
        texture->GetDevice(&frameDevice);
        if (!SameComObject(frameDevice.Get(), device.Get())) {
            return Fail(L"The decoded texture belongs to a different D3D11 device");
        }

        D3D11_TEXTURE2D_DESC textureDesc = {};
        texture->GetDesc(&textureDesc);
        if (frame.arraySlice >= textureDesc.ArraySize) {
            return Fail(L"The decoded frame contains an invalid texture array slice");
        }
        if (static_cast<UINT>(frame.width) > textureDesc.Width ||
            static_cast<UINT>(frame.height) > textureDesc.Height) {
            return Fail(L"The decoded dimensions exceed the D3D11 texture dimensions");
        }

        const D3D11_VIDEO_FRAME_FORMAT frameFormat =
            frame.interlaced
                ? (frame.topFieldFirst
                       ? D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST
                       : D3D11_VIDEO_FRAME_FORMAT_INTERLACED_BOTTOM_FIELD_FIRST)
                : D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;

        const RECT sourceRect = {0, 0, frame.width, frame.height};
        const RECT destinationRect = LetterboxRect(frame.width, frame.height,
                                                   frame.sampleAspectRatio,
                                                   clientWidth, clientHeight);
        lastVideoRect = destinationRect;
        const bool useVsr = ShouldUseVsr(frame.width, frame.height, destinationRect);
        if (useVsr && !EnsureVsrInputTexture(static_cast<UINT>(frame.width),
                                             static_cast<UINT>(frame.height))) {
            return false;
        }
        bool explicitPqToneMapping =
            frame.color.transfer == movieplayer::codec::TransferCharacteristic::Pq;
        if (explicitPqToneMapping &&
            !EnsureHdrPqTexture(static_cast<UINT>(frame.width),
                                static_cast<UINT>(frame.height))) {
            explicitPqToneMapping = false;
        }

        ID3D11Texture2D* processorOutput = nullptr;
        UINT processorOutputWidth = 0;
        UINT processorOutputHeight = 0;
        RECT processorDestinationRect = {};
        HRESULT hr = S_OK;
        const auto prepareProcessor = [&]() -> bool {
            if (explicitPqToneMapping) {
                processorOutput = hdrPqTexture.Get();
                processorOutputWidth = static_cast<UINT>(frame.width);
                processorOutputHeight = static_cast<UINT>(frame.height);
                processorDestinationRect = sourceRect;
            } else if (useVsr) {
                processorOutput = vsrInputTexture.Get();
                processorOutputWidth = static_cast<UINT>(frame.width);
                processorOutputHeight = static_cast<UINT>(frame.height);
                processorDestinationRect = sourceRect;
            } else {
                processorOutput = backBuffer.Get();
                processorOutputWidth = clientWidth;
                processorOutputHeight = clientHeight;
                processorDestinationRect = destinationRect;
            }

            if (!EnsureVideoProcessor(static_cast<UINT>(frame.width),
                                      static_cast<UINT>(frame.height), frameFormat,
                                      processorOutput, processorOutputWidth,
                                      processorOutputHeight)) {
                return false;
            }

            UINT inputSupport = 0;
            hr = videoEnumerator->CheckVideoProcessorFormat(textureDesc.Format,
                                                             &inputSupport);
            if (FAILED(hr) ||
                !(inputSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT)) {
                return Fail(L"The decoder texture format is not supported by the D3D11 video processor");
            }
            UINT outputSupport = 0;
            D3D11_TEXTURE2D_DESC processorOutputDesc = {};
            processorOutput->GetDesc(&processorOutputDesc);
            hr = videoEnumerator->CheckVideoProcessorFormat(processorOutputDesc.Format,
                                                             &outputSupport);
            if (FAILED(hr) ||
                !(outputSupport & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT)) {
                return Fail(L"RGB output is not supported by the D3D11 video processor");
            }
            return true;
        };

        if (!prepareProcessor()) {
            if (!explicitPqToneMapping) {
                return false;
            }
            // Older video processors may not accept a 10-bit RGB output
            // surface. Keep playback functional by falling back to their SDR
            // conversion path on those devices.
            explicitPqToneMapping = false;
            InvalidateVideoProcessor();
            if (!prepareProcessor()) {
                return false;
            }
        }

        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inputDesc = {};
        inputDesc.FourCC = 0;
        inputDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inputDesc.Texture2D.MipSlice = 0;
        inputDesc.Texture2D.ArraySlice = frame.arraySlice;
        ComPtr<ID3D11VideoProcessorInputView> inputView;
        hr = videoDevice->CreateVideoProcessorInputView(texture, videoEnumerator.Get(),
                                                        &inputDesc, &inputView);
        if (FAILED(hr)) {
            return Fail(L"ID3D11VideoDevice::CreateVideoProcessorInputView", hr);
        }

        if ((IsHdrTransfer(frame) || IsBt2020(frame)) && !videoContext1) {
            return Fail(L"ID3D11VideoContext1 is required for BT.2020/HDR color conversion");
        }

        const RECT outputRect = {0, 0, static_cast<LONG>(processorOutputWidth),
                                 static_cast<LONG>(processorOutputHeight)};

        const FLOAT clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        context->ClearRenderTargetView(renderTarget.Get(), clearColor);
        context->OMSetRenderTargets(0, nullptr, nullptr);

        D3D11_VIDEO_COLOR background = {};
        background.RGBA.A = 1.0f;
        videoContext->VideoProcessorSetOutputTargetRect(videoProcessor.Get(), TRUE, &outputRect);
        videoContext->VideoProcessorSetOutputBackgroundColor(videoProcessor.Get(), FALSE, &background);
        videoContext->VideoProcessorSetOutputAlphaFillMode(
            videoProcessor.Get(), D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE_OPAQUE, 0);
        videoContext->VideoProcessorSetStreamFrameFormat(videoProcessor.Get(), 0, frameFormat);
        videoContext->VideoProcessorSetStreamSourceRect(videoProcessor.Get(), 0, TRUE, &sourceRect);
        videoContext->VideoProcessorSetStreamDestRect(videoProcessor.Get(), 0, TRUE,
                                                      &processorDestinationRect);
        videoContext->VideoProcessorSetStreamAutoProcessingMode(videoProcessor.Get(), 0, TRUE);

        if (videoContext1) {
            DXGI_COLOR_SPACE_TYPE inputColorSpace = DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
            if (!SelectInputColorSpace(frame, textureDesc.Format, inputColorSpace)) {
                return Fail(L"The frame color space has no lossless DXGI video-processor mapping");
            }
            videoContext1->VideoProcessorSetStreamColorSpace1(videoProcessor.Get(), 0,
                                                              inputColorSpace);
            videoContext1->VideoProcessorSetOutputColorSpace1(
                videoProcessor.Get(),
                explicitPqToneMapping
                    ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                    : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        } else {
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColor = {};
            inputColor.Usage = 0;
            inputColor.YCbCr_Matrix =
                frame.color.matrix == movieplayer::codec::ColorMatrix::Bt709 ? 1 : 0;
            inputColor.Nominal_Range =
                frame.color.range == movieplayer::codec::ColorRange::Full
                                           ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255
                                           : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColor = {};
            outputColor.Usage = 0;
            outputColor.RGB_Range = 0;
            outputColor.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
            videoContext->VideoProcessorSetStreamColorSpace(videoProcessor.Get(), 0, &inputColor);
            videoContext->VideoProcessorSetOutputColorSpace(videoProcessor.Get(), &outputColor);
        }

        D3D11_VIDEO_PROCESSOR_STREAM stream = {};
        stream.Enable = TRUE;
        stream.OutputIndex = 0;
        stream.InputFrameOrField = 0;
        stream.PastFrames = 0;
        stream.FutureFrames = 0;
        stream.pInputSurface = inputView.Get();
        hr = videoContext->VideoProcessorBlt(videoProcessor.Get(), videoOutput.Get(), 0, 1, &stream);
        if (FAILED(hr)) {
            return Fail(L"ID3D11VideoContext::VideoProcessorBlt", hr);
        }
        if (explicitPqToneMapping) {
            if (useVsr) {
                const RECT toneMapRect = {0, 0, frame.width, frame.height};
                if (!DrawTextureToTarget(hdrPqView.Get(),
                                         vsrInputRenderTarget.Get(),
                                         static_cast<UINT>(frame.width),
                                         static_cast<UINT>(frame.height),
                                         toneMapRect,
                                         hdrToneMapPixelShader.Get())) {
                    return false;
                }
                if (RenderVsrTexture(vsrInputTexture.Get(), frame.width,
                                     frame.height, destinationRect)) {
                    return true;
                }
                // A transient SDK or resource error must not stop playback.
                rtxVideoUpscalingEnabled = false;
                InvalidateVideoProcessor();
                return RenderHardware(frame);
            }
            return DrawTexture(hdrPqView.Get(), destinationRect,
                               hdrToneMapPixelShader.Get()) && Present();
        }
        if (useVsr) {
            if (RenderVsrTexture(vsrInputTexture.Get(), frame.width,
                                 frame.height, destinationRect)) {
                return true;
            }
            // A transient SDK or resource error must not stop playback.
            // Disable the feature for this session and immediately render the
            // frame through the original video-processor path.
            rtxVideoUpscalingEnabled = false;
            InvalidateVideoProcessor();
            return RenderHardware(frame);
        }
        return Present();
    }

    bool RenderFrame(const movieplayer::codec::VideoFrame& frame)
    {
        if (!device || !context || !swapChain) {
            return Fail(L"D3DRenderer is not initialized");
        }
        if (!clientWidth || !clientHeight || !renderTarget) {
            lastError.clear();
            return true; // The window is minimized.
        }
        if (RenderHardware(frame)) {
            lastError.clear();
            return true;
        }
        return false;
    }

    void Clear()
    {
        if (!device || !context || !swapChain) {
            Fail(L"D3DRenderer is not initialized");
            return;
        }
        if (!renderTarget || !clientWidth || !clientHeight) {
            lastError.clear();
            return;
        }
        const FLOAT black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        lastVideoRect = {0, 0, static_cast<LONG>(clientWidth), static_cast<LONG>(clientHeight)};
        context->ClearRenderTargetView(renderTarget.Get(), black);
        if (Present()) {
            lastError.clear();
        }
    }
};

D3DRenderer::D3DRenderer()
    : impl_(std::make_unique<Impl>())
{
}

D3DRenderer::~D3DRenderer() = default;

bool D3DRenderer::Initialize(HWND window)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->Initialize(window);
}

bool D3DRenderer::Resize()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->window || !IsWindow(impl_->window)) {
        return impl_->Fail(L"D3DRenderer has no valid output window");
    }
    RECT client = {};
    GetClientRect(impl_->window, &client);
    const UINT width = client.right > client.left
                           ? static_cast<UINT>(client.right - client.left)
                           : 0;
    const UINT height = client.bottom > client.top
                            ? static_cast<UINT>(client.bottom - client.top)
                            : 0;
    return impl_->Resize(width, height);
}

bool D3DRenderer::Resize(UINT width, UINT height)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->Resize(width, height);
}

ID3D11Device* D3DRenderer::Device() const noexcept
{
    return impl_->device.Get();
}

bool D3DRenderer::RenderFrame(const movieplayer::codec::VideoFrame& frame)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->RenderFrame(frame);
}

void D3DRenderer::SetSubtitleText(const std::wstring& text)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->subtitleText = text;
    impl_->subtitleBitmap.reset();
    impl_->renderedSubtitleText.clear();
}

void D3DRenderer::SetSubtitleBitmap(
    std::shared_ptr<const movieplayer::codec::SubtitleBitmap> bitmap)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->subtitleText.clear();
    impl_->subtitleBitmap = std::move(bitmap);
    impl_->renderedSubtitleBitmap.reset();
}

void D3DRenderer::SetSubtitleFontFamily(const std::wstring& fontFamily)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->subtitleFontFamily == fontFamily) {
        return;
    }
    impl_->subtitleFontFamily = fontFamily;
    impl_->renderedSubtitleText.clear();
}

void D3DRenderer::SetFullscreenSubtitleScale(bool enabled)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->fullscreenSubtitleScale == enabled) {
        return;
    }
    impl_->fullscreenSubtitleScale = enabled;
    impl_->renderedSubtitleText.clear();
    impl_->renderedSubtitleFontSize = 0.0f;
}

bool D3DRenderer::SetRtxVideoUpscalingEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (enabled && !impl_->rtxVideoVsr.IsAvailable()) {
        impl_->rtxVideoUpscalingEnabled = false;
        return false;
    }
    if (impl_->rtxVideoUpscalingEnabled == enabled) {
        return true;
    }

    impl_->rtxVideoUpscalingEnabled = enabled;
    impl_->InvalidateVideoProcessor();
    impl_->vsrOutputView.Reset();
    impl_->vsrOutputTexture.Reset();
    impl_->vsrOutputWidth = impl_->vsrOutputHeight = 0;
    return true;
}

bool D3DRenderer::IsRtxVideoUpscalingEnabled() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->rtxVideoUpscalingEnabled;
}

bool D3DRenderer::IsRtxVideoUpscalingAvailable() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->rtxVideoVsr.IsAvailable();
}

std::wstring D3DRenderer::RtxVideoUpscalingStatus() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->rtxVideoVsr.Status();
}

void D3DRenderer::Clear()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->Clear();
}

const std::wstring& D3DRenderer::LastError() const noexcept
{
    return impl_->lastError;
}
