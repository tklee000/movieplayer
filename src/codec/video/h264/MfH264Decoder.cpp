#include "codec/video/h264/MfH264Decoder.h"

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

namespace movieplayer::codec::h264 {
namespace {

using Microsoft::WRL::ComPtr;

constexpr LONGLONG kHundredNanosecondsPerSecond = 10'000'000;
// PlayerEngine can retain eight queued frames plus the currently displayed
// frame. The Media Foundation transform may also return a burst of reordered
// pictures before that batch reaches the playback queue, so twelve surfaces
// are not sufficient even for the focused High@4.2 stream.
constexpr std::size_t kMaximumSurfaces = 32;

std::wstring HresultText(const wchar_t* operation, HRESULT result) {
    std::wostringstream out;
    out << operation << L" failed (HRESULT 0x" << std::hex << std::setw(8)
        << std::setfill(L'0') << static_cast<unsigned long>(result) << L")";
    return out.str();
}

std::uint32_t ReadBigEndianLength(const std::uint8_t* data, unsigned size) {
    std::uint32_t result = 0;
    for (unsigned i = 0; i < size; ++i) {
        result = (result << 8U) | data[i];
    }
    return result;
}

LONGLONG SecondsToMediaTime(double seconds) {
    if (!std::isfinite(seconds)) return 0;
    const double scaled = seconds * kHundredNanosecondsPerSecond;
    const double limited = std::max(
        static_cast<double>((std::numeric_limits<LONGLONG>::min)()),
        std::min(static_cast<double>((std::numeric_limits<LONGLONG>::max)()),
                 scaled));
    return static_cast<LONGLONG>(std::llround(limited));
}

}  // namespace

struct MfH264Decoder::Impl {
    struct Surface {
        ComPtr<ID3D11Texture2D> texture;
        std::weak_ptr<VideoFrame> displayedFrame;
    };

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> immediateContext;
    ComPtr<IMFTransform> transform;
    ComPtr<IMFMediaType> outputType;
    std::vector<Surface> surfaces;
    TrackInfo track;
    std::vector<std::uint8_t> parameterSets;
    std::wstring description;
    std::wstring error;
    unsigned nalLengthSize = 0;
    UINT outputWidth = 0;
    UINT outputHeight = 0;
    LONG outputStride = 0;
    DWORD outputStreamFlags = 0;
    DWORD outputBufferSize = 0;
    bool parameterSetsPending = true;
    bool mpeg4Part2 = false;
    bool mediaFoundationStarted = false;
    bool comInitialized = false;

    ~Impl() { Shutdown(); }

    bool Fail(const std::wstring& message) {
        error = message;
        return false;
    }

    void Shutdown() {
        surfaces.clear();
        outputType.Reset();
        transform.Reset();
        immediateContext.Reset();
        device.Reset();
        if (mediaFoundationStarted) {
            MFShutdown();
            mediaFoundationStarted = false;
        }
        if (comInitialized) {
            CoUninitialize();
            comInitialized = false;
        }
        outputWidth = outputHeight = 0;
        outputStride = 0;
        outputStreamFlags = outputBufferSize = 0;
    }

    bool ParseConfiguration(const std::vector<std::uint8_t>& configuration) {
        parameterSets.clear();
        if (configuration.size() < 7 || configuration[0] != 1) {
            return Fail(L"The H.264 avcC configuration record is invalid");
        }
        nalLengthSize = (configuration[4] & 0x03U) + 1U;
        if (nalLengthSize == 3) {
            return Fail(L"Three-byte H.264 NAL lengths are not supported");
        }
        std::size_t position = 6;
        const unsigned spsCount = configuration[5] & 0x1fU;
        unsigned ppsCount = 0;
        auto appendUnits = [&](unsigned count, std::uint8_t expectedType,
                               const wchar_t* label) -> bool {
            for (unsigned i = 0; i < count; ++i) {
                if (position + 2 > configuration.size()) {
                    return Fail(std::wstring(L"Truncated H.264 ") + label +
                                L" in avcC");
                }
                const std::uint32_t size =
                    ReadBigEndianLength(configuration.data() + position, 2);
                position += 2;
                if (size == 0 || position + size > configuration.size() ||
                    (configuration[position] & 0x1fU) != expectedType) {
                    return Fail(std::wstring(L"Invalid H.264 ") + label +
                                L" in avcC");
                }
                parameterSets.insert(parameterSets.end(), {0, 0, 0, 1});
                parameterSets.insert(parameterSets.end(),
                                     configuration.begin() + position,
                                     configuration.begin() + position + size);
                position += size;
            }
            return true;
        };
        if (spsCount == 0 || !appendUnits(spsCount, 7, L"SPS")) return false;
        if (position >= configuration.size()) {
            return Fail(L"The H.264 avcC record has no PPS count");
        }
        ppsCount = configuration[position++];
        if (ppsCount == 0 || !appendUnits(ppsCount, 8, L"PPS")) return false;
        return true;
    }

    bool ConvertToAnnexB(const EncodedSample& sample,
                         std::vector<std::uint8_t>& annexB) {
        annexB.clear();
        if (parameterSetsPending || sample.sync) {
            annexB.insert(annexB.end(), parameterSets.begin(), parameterSets.end());
        }
        std::size_t position = 0;
        unsigned nalCount = 0;
        while (position < sample.bytes.size()) {
            if (sample.bytes.size() - position < nalLengthSize) {
                return Fail(L"A truncated H.264 NAL length was found in an MP4 sample");
            }
            const std::uint32_t size = ReadBigEndianLength(
                sample.bytes.data() + position, nalLengthSize);
            position += nalLengthSize;
            if (size == 0 || size > sample.bytes.size() - position) {
                return Fail(L"An invalid H.264 NAL size was found in an MP4 sample");
            }
            annexB.insert(annexB.end(), {0, 0, 0, 1});
            annexB.insert(annexB.end(), sample.bytes.begin() + position,
                          sample.bytes.begin() + position + size);
            position += size;
            ++nalCount;
        }
        if (nalCount == 0) return Fail(L"The H.264 MP4 sample contains no NAL units");
        return true;
    }

    bool ConfigureOutputType() {
        ComPtr<IMFMediaType> selected;
        for (DWORD index = 0;; ++index) {
            ComPtr<IMFMediaType> candidate;
            const HRESULT available =
                transform->GetOutputAvailableType(0, index, &candidate);
            if (available == MF_E_NO_MORE_TYPES) break;
            if (FAILED(available)) {
                return Fail(HresultText(L"GetOutputAvailableType(H.264)",
                                        available));
            }
            GUID subtype = {};
            if (SUCCEEDED(candidate->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
                subtype == MFVideoFormat_NV12) {
                selected = candidate;
                break;
            }
        }
        if (!selected) {
            return Fail(L"The Windows H.264 decoder does not expose NV12 output");
        }
        HRESULT hr = transform->SetOutputType(0, selected.Get(), 0);
        if (FAILED(hr)) {
            return Fail(HresultText(L"SetOutputType(H.264 NV12)", hr));
        }

        UINT32 width = 0;
        UINT32 height = 0;
        if (FAILED(MFGetAttributeSize(selected.Get(), MF_MT_FRAME_SIZE, &width,
                                      &height)) ||
            width == 0 || height == 0) {
            width = static_cast<UINT32>(track.width);
            height = static_cast<UINT32>(track.height);
        }
        if (width == 0 || height == 0 || (width & 1U) != 0 ||
            (height & 1U) != 0) {
            return Fail(L"The H.264 decoder returned invalid NV12 dimensions");
        }
        UINT32 stride = 0;
        if (FAILED(selected->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ||
            stride < width) {
            stride = width;
        }

        MFT_OUTPUT_STREAM_INFO streamInfo = {};
        hr = transform->GetOutputStreamInfo(0, &streamInfo);
        if (FAILED(hr)) {
            return Fail(HresultText(L"GetOutputStreamInfo(H.264)", hr));
        }
        outputType = selected;
        outputWidth = width;
        outputHeight = height;
        outputStride = static_cast<LONG>(stride);
        outputStreamFlags = streamInfo.dwFlags;
        outputBufferSize = streamInfo.cbSize;
        surfaces.clear();
        return true;
    }

    bool CreateTransform() {
        HRESULT hr = E_FAIL;
        if (mpeg4Part2) {
            MFT_REGISTER_TYPE_INFO inputRegistration = {
                MFMediaType_Video, MFVideoFormat_MP4V};
            MFT_REGISTER_TYPE_INFO outputRegistration = {
                MFMediaType_Video, MFVideoFormat_NV12};
            IMFActivate** activations = nullptr;
            UINT32 activationCount = 0;
            hr = MFTEnumEx(
                MFT_CATEGORY_VIDEO_DECODER,
                MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT |
                    MFT_ENUM_FLAG_SORTANDFILTER,
                &inputRegistration, &outputRegistration,
                &activations, &activationCount);
            if (SUCCEEDED(hr) && activationCount != 0) {
                hr = activations[0]->ActivateObject(IID_PPV_ARGS(&transform));
            }
            for (UINT32 i = 0; i < activationCount; ++i) activations[i]->Release();
            CoTaskMemFree(activations);
            if (!transform) {
                hr = CoCreateInstance(CLSID_CMpeg4sDecMFT, nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&transform));
            }
        } else {
            hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&transform));
        }
        if (FAILED(hr)) {
            return Fail(HresultText(mpeg4Part2
                                        ? L"Create Windows MPEG-4 Part 2 decoder"
                                        : L"Create Windows H.264 decoder",
                                    hr));
        }

        ComPtr<ICodecAPI> codecApi;
        if (SUCCEEDED(transform.As(&codecApi))) {
            VARIANT acceleration = {};
            acceleration.vt = VT_BOOL;
            acceleration.boolVal = VARIANT_TRUE;
            // This is a preference. The Microsoft transform automatically
            // falls back to software when the driver cannot decode the stream.
            if (!mpeg4Part2) {
                codecApi->SetValue(&CODECAPI_AVDecVideoAcceleration_H264,
                                   &acceleration);
            }
        }

        ComPtr<IMFMediaType> inputType;
        hr = MFCreateMediaType(&inputType);
        if (FAILED(hr) ||
            FAILED(inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
            FAILED(inputType->SetGUID(
                MF_MT_SUBTYPE,
                mpeg4Part2 ? MFVideoFormat_MP4V : MFVideoFormat_H264)) ||
            FAILED(MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE,
                                      static_cast<UINT32>(track.width),
                                      static_cast<UINT32>(track.height))) ||
            FAILED(inputType->SetUINT32(MF_MT_INTERLACE_MODE,
                                        MFVideoInterlace_Progressive))) {
            return Fail(FAILED(hr) ? HresultText(L"MFCreateMediaType(H.264)", hr)
                                   : L"Could not configure the H.264 input media type");
        }
        if (track.frameRate.IsValid() && track.frameRate.numerator <= UINT_MAX &&
            track.frameRate.denominator <= UINT_MAX) {
            MFSetAttributeRatio(
                inputType.Get(), MF_MT_FRAME_RATE,
                static_cast<UINT32>(track.frameRate.numerator),
                static_cast<UINT32>(track.frameRate.denominator));
        }
        const Rational sar = track.sampleAspectRatio.IsValid()
                                 ? track.sampleAspectRatio
                                 : Rational{1, 1};
        if (sar.numerator <= UINT_MAX && sar.denominator <= UINT_MAX) {
            MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO,
                                static_cast<UINT32>(sar.numerator),
                                static_cast<UINT32>(sar.denominator));
        }
        if (mpeg4Part2 && !track.codecPrivate.empty()) {
            inputType->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER,
                               track.codecPrivate.data(),
                               static_cast<UINT32>(track.codecPrivate.size()));
        }
        hr = transform->SetInputType(0, inputType.Get(), 0);
        if (FAILED(hr)) {
            return Fail(HresultText(mpeg4Part2 ? L"SetInputType(MPEG-4 Part 2)"
                                              : L"SetInputType(H.264)",
                                    hr));
        }
        if (!ConfigureOutputType()) return false;

        hr = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        if (FAILED(hr)) {
            return Fail(HresultText(L"Begin H.264 streaming", hr));
        }
        hr = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        if (FAILED(hr)) {
            return Fail(HresultText(L"Start H.264 stream", hr));
        }
        return true;
    }

    bool Initialize(ID3D11Device* sharedDevice, const TrackInfo& sourceTrack) {
        Shutdown();
        error.clear();
        description.clear();
        parameterSets.clear();
        parameterSetsPending = true;
        nalLengthSize = 0;
        mpeg4Part2 = sourceTrack.codec == CodecId::Mpeg4Part2;
        if (!sharedDevice ||
            (sourceTrack.codec != CodecId::H264 && !mpeg4Part2) ||
            sourceTrack.width <= 0 || sourceTrack.height <= 0) {
            return Fail(L"The Media Foundation video decoder received an invalid track");
        }

        const HRESULT comResult =
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(comResult)) {
            comInitialized = true;
        } else if (comResult != RPC_E_CHANGED_MODE) {
            return Fail(HresultText(L"CoInitializeEx", comResult));
        }
        HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(hr)) return Fail(HresultText(L"MFStartup", hr));
        mediaFoundationStarted = true;

        track = sourceTrack;
        device = sharedDevice;
        device->GetImmediateContext(&immediateContext);
        if (!immediateContext) return Fail(L"The D3D11 device has no immediate context");
        if ((!mpeg4Part2 && !ParseConfiguration(track.codecPrivate)) ||
            !CreateTransform()) {
            return false;
        }

        std::wostringstream label;
        if (mpeg4Part2) {
            label << L"Windows Media Foundation MPEG-4 Part 2 ("
                  << std::wstring(track.sampleEntry.begin(), track.sampleEntry.end())
                  << L", NV12)";
        } else {
            label << L"Windows Media Foundation H.264 (NV12, DXVA when available)"
                  << L" - High-compatible profile "
                  << static_cast<unsigned>(track.codecPrivate[1]) << L", level "
                  << static_cast<unsigned>(track.codecPrivate[3]) / 10 << L"."
                  << static_cast<unsigned>(track.codecPrivate[3]) % 10;
        }
        description = label.str();
        error.clear();
        return true;
    }

    bool CreateInputSample(const std::vector<std::uint8_t>& bytes,
                           const EncodedSample& encoded,
                           ComPtr<IMFSample>& sample) {
        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(bytes.size()), &buffer);
        if (FAILED(hr)) return Fail(HresultText(L"MFCreateMemoryBuffer(H.264)", hr));
        BYTE* destination = nullptr;
        DWORD capacity = 0;
        hr = buffer->Lock(&destination, &capacity, nullptr);
        if (FAILED(hr) || !destination || capacity < bytes.size()) {
            if (SUCCEEDED(hr)) buffer->Unlock();
            return Fail(FAILED(hr) ? HresultText(L"Lock H.264 input buffer", hr)
                                   : L"The H.264 input buffer is undersized");
        }
        std::memcpy(destination, bytes.data(), bytes.size());
        buffer->Unlock();
        hr = buffer->SetCurrentLength(static_cast<DWORD>(bytes.size()));
        if (FAILED(hr)) return Fail(HresultText(L"Set H.264 input length", hr));
        hr = MFCreateSample(&sample);
        if (FAILED(hr)) return Fail(HresultText(L"MFCreateSample(H.264)", hr));
        if (FAILED(sample->AddBuffer(buffer.Get())) ||
            FAILED(sample->SetSampleTime(SecondsToMediaTime(encoded.PtsSeconds()))) ||
            FAILED(sample->SetSampleDuration(
                SecondsToMediaTime(encoded.DurationSeconds())))) {
            return Fail(L"Could not timestamp the H.264 input sample");
        }
        if (encoded.sync) {
            sample->SetUINT32(MFSampleExtension_CleanPoint, TRUE);
        }
        return true;
    }

    int AcquireSurface() {
        for (std::size_t i = 0; i < surfaces.size(); ++i) {
            if (surfaces[i].displayedFrame.expired()) return static_cast<int>(i);
        }
        if (surfaces.size() >= kMaximumSurfaces) return -1;
        D3D11_TEXTURE2D_DESC textureDescription = {};
        textureDescription.Width = outputWidth;
        textureDescription.Height = outputHeight;
        textureDescription.MipLevels = 1;
        textureDescription.ArraySize = 1;
        textureDescription.Format = DXGI_FORMAT_NV12;
        textureDescription.SampleDesc.Count = 1;
        textureDescription.Usage = D3D11_USAGE_DEFAULT;
        textureDescription.BindFlags = 0;
        Surface surface;
        const HRESULT hr = device->CreateTexture2D(
            &textureDescription, nullptr, &surface.texture);
        if (FAILED(hr)) {
            Fail(HresultText(L"CreateTexture2D(H.264 NV12)", hr));
            return -1;
        }
        surfaces.push_back(std::move(surface));
        return static_cast<int>(surfaces.size() - 1);
    }

    bool CopyNv12Buffer(IMFSample* sample, std::vector<std::uint8_t>& nv12) {
        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr = sample->ConvertToContiguousBuffer(&buffer);
        if (FAILED(hr)) {
            return Fail(HresultText(L"Convert H.264 output buffer", hr));
        }
        const std::size_t lumaBytes =
            static_cast<std::size_t>(outputWidth) * outputHeight;
        nv12.resize(lumaBytes + lumaBytes / 2U);

        ComPtr<IMF2DBuffer> twoDimensional;
        if (SUCCEEDED(buffer.As(&twoDimensional))) {
            BYTE* scanline = nullptr;
            LONG pitch = 0;
            hr = twoDimensional->Lock2D(&scanline, &pitch);
            if (SUCCEEDED(hr) && scanline && pitch >= static_cast<LONG>(outputWidth)) {
                for (UINT y = 0; y < outputHeight; ++y) {
                    std::memcpy(nv12.data() + static_cast<std::size_t>(y) * outputWidth,
                                scanline + static_cast<std::size_t>(y) * pitch,
                                outputWidth);
                }
                BYTE* chroma = scanline + static_cast<std::size_t>(pitch) * outputHeight;
                for (UINT y = 0; y < outputHeight / 2U; ++y) {
                    std::memcpy(nv12.data() + lumaBytes +
                                    static_cast<std::size_t>(y) * outputWidth,
                                chroma + static_cast<std::size_t>(y) * pitch,
                                outputWidth);
                }
                twoDimensional->Unlock2D();
                return true;
            }
            if (SUCCEEDED(hr)) twoDimensional->Unlock2D();
        }

        BYTE* source = nullptr;
        DWORD capacity = 0;
        DWORD length = 0;
        hr = buffer->Lock(&source, &capacity, &length);
        if (FAILED(hr) || !source) {
            return Fail(FAILED(hr) ? HresultText(L"Lock H.264 output buffer", hr)
                                   : L"The H.264 output buffer is empty");
        }
        const std::size_t stride = static_cast<std::size_t>(
            std::max<LONG>(outputStride, static_cast<LONG>(outputWidth)));
        const std::size_t required = stride * outputHeight * 3U / 2U;
        if (length < required) {
            buffer->Unlock();
            return Fail(L"The H.264 NV12 output buffer is truncated");
        }
        for (UINT y = 0; y < outputHeight; ++y) {
            std::memcpy(nv12.data() + static_cast<std::size_t>(y) * outputWidth,
                        source + static_cast<std::size_t>(y) * stride,
                        outputWidth);
        }
        const BYTE* chroma = source + stride * outputHeight;
        for (UINT y = 0; y < outputHeight / 2U; ++y) {
            std::memcpy(nv12.data() + lumaBytes +
                            static_cast<std::size_t>(y) * outputWidth,
                        chroma + static_cast<std::size_t>(y) * stride,
                        outputWidth);
        }
        buffer->Unlock();
        return true;
    }

    bool MakeFrame(IMFSample* decoded,
                   std::vector<std::shared_ptr<VideoFrame>>& output) {
        std::vector<std::uint8_t> nv12;
        if (!CopyNv12Buffer(decoded, nv12)) return false;
        const int surfaceIndex = AcquireSurface();
        if (surfaceIndex < 0) {
            if (error.empty()) {
                error = L"The H.264 texture pool is exhausted (" +
                        std::to_wstring(surfaces.size()) + L" surfaces in use)";
            }
            return false;
        }
        immediateContext->UpdateSubresource(surfaces[surfaceIndex].texture.Get(), 0,
                                            nullptr, nv12.data(), outputWidth,
                                            static_cast<UINT>(nv12.size()));

        auto frame = std::make_shared<VideoFrame>();
        frame->texture = surfaces[surfaceIndex].texture;
        frame->arraySlice = 0;
        frame->format = DXGI_FORMAT_NV12;
        frame->width = track.width;
        frame->height = track.height;
        frame->sampleAspectRatio = track.sampleAspectRatio.IsValid()
                                       ? track.sampleAspectRatio
                                       : Rational{1, 1};
        frame->color = track.color;
        if (frame->color.range == ColorRange::Unspecified)
            frame->color.range = ColorRange::Limited;
        if (frame->color.matrix == ColorMatrix::Unspecified)
            frame->color.matrix = frame->height > 576 ? ColorMatrix::Bt709
                                                       : ColorMatrix::Bt601;
        if (frame->color.primaries == ColorPrimaries::Unspecified)
            frame->color.primaries = ColorPrimaries::Bt709;
        if (frame->color.transfer == TransferCharacteristic::Unspecified)
            frame->color.transfer = TransferCharacteristic::Bt709;
        if (frame->color.chromaLocation == ChromaLocation::Unspecified)
            frame->color.chromaLocation = ChromaLocation::Left;
        LONGLONG time = 0;
        LONGLONG duration = 0;
        if (SUCCEEDED(decoded->GetSampleTime(&time))) {
            frame->pts = static_cast<double>(time) /
                         kHundredNanosecondsPerSecond;
        }
        if (SUCCEEDED(decoded->GetSampleDuration(&duration))) {
            frame->duration = static_cast<double>(duration) /
                              kHundredNanosecondsPerSecond;
        } else if (track.frameRate.IsValid()) {
            frame->duration = track.frameRate.denominator /
                              static_cast<double>(track.frameRate.numerator);
        }
        surfaces[surfaceIndex].displayedFrame = frame;
        output.push_back(std::move(frame));
        return true;
    }

    bool DrainOutput(std::vector<std::shared_ptr<VideoFrame>>& output) {
        for (unsigned iteration = 0; iteration < 64; ++iteration) {
            MFT_OUTPUT_DATA_BUFFER outputBuffer = {};
            outputBuffer.dwStreamID = 0;
            ComPtr<IMFSample> suppliedSample;
            const bool transformProvidesSamples =
                (outputStreamFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
            if (!transformProvidesSamples) {
                HRESULT hr = MFCreateSample(&suppliedSample);
                if (FAILED(hr)) return Fail(HresultText(L"MFCreateSample(NV12)", hr));
                ComPtr<IMFMediaBuffer> mediaBuffer;
                const DWORD required = outputBufferSize != 0
                                           ? outputBufferSize
                                           : outputWidth * outputHeight * 3U / 2U;
                hr = MFCreateMemoryBuffer(required, &mediaBuffer);
                if (FAILED(hr) || FAILED(suppliedSample->AddBuffer(mediaBuffer.Get()))) {
                    return Fail(FAILED(hr)
                                    ? HresultText(L"MFCreateMemoryBuffer(NV12)", hr)
                                    : L"Could not attach the NV12 output buffer");
                }
                outputBuffer.pSample = suppliedSample.Get();
            }

            DWORD status = 0;
            HRESULT hr = transform->ProcessOutput(0, 1, &outputBuffer, &status);
            if (outputBuffer.pEvents) outputBuffer.pEvents->Release();
            ComPtr<IMFSample> decoded;
            if (transformProvidesSamples && outputBuffer.pSample) {
                decoded.Attach(outputBuffer.pSample);
            } else {
                decoded = suppliedSample;
            }
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (!ConfigureOutputType()) return false;
                continue;
            }
            if (FAILED(hr)) return Fail(HresultText(L"ProcessOutput(H.264)", hr));
            if (!decoded) return Fail(L"The H.264 decoder produced no output sample");
            if (!MakeFrame(decoded.Get(), output)) return false;
        }
        return Fail(L"The H.264 decoder produced too many frames for one input sample");
    }

    bool Decode(const EncodedSample& encoded,
                std::vector<std::shared_ptr<VideoFrame>>& output) {
        output.clear();
        if (!transform || encoded.trackId != track.trackId) {
            return Fail(L"MfH264Decoder received a sample for the wrong track");
        }
        std::vector<std::uint8_t> inputBytes;
        if (mpeg4Part2) {
            inputBytes = encoded.bytes;
        } else if (!ConvertToAnnexB(encoded, inputBytes)) {
            return false;
        }
        if (inputBytes.size() > MAXDWORD) {
            return Fail(L"The compressed video access unit is too large");
        }
        ComPtr<IMFSample> inputSample;
        if (!CreateInputSample(inputBytes, encoded, inputSample)) return false;

        HRESULT hr = transform->ProcessInput(0, inputSample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            if (!DrainOutput(output)) return false;
            hr = transform->ProcessInput(0, inputSample.Get(), 0);
        }
        if (FAILED(hr)) return Fail(HresultText(L"ProcessInput(H.264)", hr));
        if (!mpeg4Part2) parameterSetsPending = false;
        if (!DrainOutput(output)) return false;
        error.clear();
        return true;
    }

    bool Flush(std::vector<std::shared_ptr<VideoFrame>>& output) {
        output.clear();
        if (!transform) return Fail(L"The H.264 decoder is not initialized");
        HRESULT hr = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        if (FAILED(hr)) return Fail(HresultText(L"End H.264 stream", hr));
        hr = transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        if (FAILED(hr)) return Fail(HresultText(L"Drain H.264 stream", hr));
        if (!DrainOutput(output)) return false;
        error.clear();
        return true;
    }

    bool Reset() {
        if (!transform) return Fail(L"The H.264 decoder is not initialized");
        HRESULT hr = transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        if (FAILED(hr)) return Fail(HresultText(L"Flush H.264 decoder", hr));
        hr = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        if (FAILED(hr)) return Fail(HresultText(L"Restart H.264 stream", hr));
        parameterSetsPending = !mpeg4Part2;
        surfaces.clear();
        error.clear();
        return true;
    }
};

MfH264Decoder::MfH264Decoder() : impl_(std::make_unique<Impl>()) {}
MfH264Decoder::~MfH264Decoder() = default;

bool MfH264Decoder::Initialize(ID3D11Device* device, const TrackInfo& track) {
    return impl_->Initialize(device, track);
}

bool MfH264Decoder::Decode(
    const EncodedSample& sample,
    std::vector<std::shared_ptr<VideoFrame>>& frames) {
    return impl_->Decode(sample, frames);
}

bool MfH264Decoder::Flush(std::vector<std::shared_ptr<VideoFrame>>& frames) {
    return impl_->Flush(frames);
}

bool MfH264Decoder::Reset() { return impl_->Reset(); }

const std::wstring& MfH264Decoder::Description() const noexcept {
    return impl_->description;
}

const std::wstring& MfH264Decoder::LastError() const noexcept {
    return impl_->error;
}

}  // namespace movieplayer::codec::h264
