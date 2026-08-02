#include "codec/video/h264/MfH264Decoder.h"

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
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
#include <queue>
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
// End-of-stream drain can expose many delayed pictures at once. Return them
// in queue-sized batches so callers can release or present each batch before
// the decoder needs another set of upload textures.
constexpr unsigned kFlushBatchSize = 8;
constexpr unsigned kMaximumDrainIterations = 64;

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

class SpsBitCursor {
public:
    explicit SpsBitCursor(const std::vector<std::uint8_t>& bits)
        : bits_(bits) {}

    std::size_t Position() const noexcept { return position_; }

    bool Read(unsigned count, std::uint32_t& value) {
        value = 0;
        if (count > 32 || count > bits_.size() - position_) return false;
        for (unsigned i = 0; i < count; ++i)
            value = (value << 1U) | bits_[position_++];
        return true;
    }

    bool ReadUe(std::uint32_t& value, std::size_t* begin = nullptr,
                std::size_t* end = nullptr) {
        if (begin) *begin = position_;
        unsigned zeros = 0;
        while (position_ < bits_.size() && bits_[position_] == 0) {
            ++zeros;
            ++position_;
            if (zeros > 31) return false;
        }
        if (position_ >= bits_.size()) return false;
        ++position_;
        std::uint32_t suffix = 0;
        if (zeros != 0 && !Read(zeros, suffix)) return false;
        value = ((std::uint32_t{1} << zeros) - 1U) + suffix;
        if (end) *end = position_;
        return true;
    }

private:
    const std::vector<std::uint8_t>& bits_;
    std::size_t position_ = 0;
};

std::vector<std::uint8_t> EbspToBits(const std::uint8_t* bytes,
                                     std::size_t size) {
    std::vector<std::uint8_t> bits;
    bits.reserve(size * 8U);
    unsigned zeros = 0;
    for (std::size_t i = 0; i < size; ++i) {
        if (zeros >= 2 && bytes[i] == 3) {
            zeros = 0;
            continue;
        }
        for (int bit = 7; bit >= 0; --bit)
            bits.push_back((bytes[i] >> bit) & 1U);
        zeros = bytes[i] == 0 ? zeros + 1U : 0U;
    }
    return bits;
}

std::vector<std::uint8_t> BitsToEbsp(
    const std::vector<std::uint8_t>& bits) {
    std::vector<std::uint8_t> rbsp((bits.size() + 7U) / 8U, 0);
    for (std::size_t i = 0; i < bits.size(); ++i)
        rbsp[i / 8U] |= bits[i] << (7U - i % 8U);
    std::vector<std::uint8_t> ebsp;
    ebsp.reserve(rbsp.size() + rbsp.size() / 32U);
    unsigned zeros = 0;
    for (std::uint8_t byte : rbsp) {
        if (zeros >= 2 && byte <= 3) {
            ebsp.push_back(3);
            zeros = 0;
        }
        ebsp.push_back(byte);
        zeros = byte == 0 ? zeros + 1U : 0U;
    }
    return ebsp;
}

bool RewriteHigh10Sps(const std::uint8_t* nal, std::size_t size,
                      std::vector<std::uint8_t>& rewritten) {
    rewritten.clear();
    if (size < 5 || (nal[0] & 0x1fU) != 7U) return false;
    std::vector<std::uint8_t> bits = EbspToBits(nal + 1, size - 1);
    SpsBitCursor cursor(bits);
    std::uint32_t profile = 0, ignored = 0, chroma = 0;
    if (!cursor.Read(8, profile) || !cursor.Read(8, ignored) ||
        !cursor.Read(8, ignored) || !cursor.ReadUe(ignored) ||
        !cursor.ReadUe(chroma)) {
        return false;
    }
    if (profile != 110 || chroma != 1) return false;
    std::size_t lumaBegin = 0, lumaEnd = 0;
    std::size_t chromaBegin = 0, chromaEnd = 0;
    std::uint32_t lumaDepth = 0, chromaDepth = 0;
    if (!cursor.ReadUe(lumaDepth, &lumaBegin, &lumaEnd) ||
        !cursor.ReadUe(chromaDepth, &chromaBegin, &chromaEnd) ||
        lumaDepth != 2 || chromaDepth != 2 ||
        lumaEnd > chromaBegin || chromaEnd > bits.size()) {
        return false;
    }

    std::vector<std::uint8_t> normalized;
    normalized.reserve(bits.size());
    normalized.insert(normalized.end(), bits.begin(), bits.begin() + lumaBegin);
    normalized.push_back(1);  // bit_depth_luma_minus8 = ue(0)
    normalized.insert(normalized.end(), bits.begin() + lumaEnd,
                      bits.begin() + chromaBegin);
    normalized.push_back(1);  // bit_depth_chroma_minus8 = ue(0)
    normalized.insert(normalized.end(), bits.begin() + chromaEnd, bits.end());
    // High 4:2:0 8-bit has the same coding tools as High 10 after the two
    // bit-depth fields are normalized. The QpBdOffset difference makes the
    // Windows decoder's reconstruction the 8-bit-scaled counterpart.
    for (unsigned bit = 0; bit < 8; ++bit)
        normalized[bit] = (100U >> (7U - bit)) & 1U;

    rewritten.push_back(nal[0]);
    std::vector<std::uint8_t> payload = BitsToEbsp(normalized);
    rewritten.insert(rewritten.end(), payload.begin(), payload.end());
    return true;
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
    ComPtr<IMFDXGIDeviceManager> dxgiDeviceManager;
    ComPtr<IMFTransform> transform;
    ComPtr<IMFSourceReader> sourceReader;
    ComPtr<IMFMediaType> outputType;
    std::vector<Surface> surfaces;
    // Retain submitted presentation times only as a per-frame fallback when
    // the Media Foundation decoder supplies no usable output timestamp. The
    // decoder can drop damaged pictures, so unconditionally assigning these
    // times to later output would attach stale timestamps to newer content.
    std::priority_queue<
        std::pair<LONGLONG, LONGLONG>,
        std::vector<std::pair<LONGLONG, LONGLONG>>,
        std::greater<std::pair<LONGLONG, LONGLONG>>>
        pendingPresentationTimes;
    TrackInfo track;
    std::vector<std::uint8_t> parameterSets;
    std::vector<std::uint8_t> mpeg4SequenceHeader;
    std::wstring description;
    std::wstring error;
    unsigned nalLengthSize = 0;
    UINT outputWidth = 0;
    UINT outputHeight = 0;
    LONG outputStride = 0;
    DWORD outputStreamFlags = 0;
    DWORD outputBufferSize = 0;
    UINT dxgiResetToken = 0;
    GUID outputSubtype = MFVideoFormat_NV12;
    bool parameterSetsPending = true;
    bool h264 = true;
    bool mpeg4Part2 = false;
    bool drainStarted = false;
    bool drainComplete = false;
    bool mediaFoundationStarted = false;
    bool comInitialized = false;
    bool sourceReaderNeedsSeek = true;
    bool d3dManagerAttached = false;
    DWORD sourceReaderStream =
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

    ~Impl() { Shutdown(); }

    bool Fail(const std::wstring& message) {
        error = message;
        return false;
    }

    void Shutdown() {
        if (transform && d3dManagerAttached) {
            transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
        }
        surfaces.clear();
        pendingPresentationTimes = {};
        outputType.Reset();
        sourceReader.Reset();
        transform.Reset();
        dxgiDeviceManager.Reset();
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
        drainStarted = false;
        drainComplete = false;
        sourceReaderNeedsSeek = true;
        d3dManagerAttached = false;
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
                std::vector<std::uint8_t> normalizedSps;
                if (expectedType == 7 &&
                    RewriteHigh10Sps(configuration.data() + position, size,
                                     normalizedSps)) {
                    parameterSets.insert(parameterSets.end(),
                                         normalizedSps.begin(),
                                         normalizedSps.end());
                } else {
                    parameterSets.insert(parameterSets.end(),
                                         configuration.begin() + position,
                                         configuration.begin() + position +
                                             size);
                }
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
        const auto hasStartCode = [](const std::vector<std::uint8_t>& bytes) {
            for (std::size_t i = 0; i + 3 < bytes.size(); ++i) {
                if (bytes[i] == 0 && bytes[i + 1] == 0 &&
                    (bytes[i + 2] == 1 ||
                     (bytes[i + 2] == 0 && bytes[i + 3] == 1))) {
                    return true;
                }
            }
            return false;
        };
        if (hasStartCode(sample.bytes)) {
            annexB.insert(annexB.end(), sample.bytes.begin(),
                          sample.bytes.end());
            return true;
        }
        if (nalLengthSize == 0) {
            return Fail(
                L"The AVI H.264 access unit has neither Annex-B start codes "
                L"nor an avcC NAL-length description");
        }
        std::size_t position = 0;
        unsigned nalCount = 0;
        while (position < sample.bytes.size()) {
            if (sample.bytes.size() - position < nalLengthSize) {
                // A few partially downloaded MP4s have an isolated damaged
                // access unit while the following sync sample is intact.
                // Keep complete NALs from this unit, or drop only this unit,
                // instead of terminating the entire playback session.
                if (nalCount == 0) annexB.clear();
                error.clear();
                return true;
            }
            const std::uint32_t size = ReadBigEndianLength(
                sample.bytes.data() + position, nalLengthSize);
            position += nalLengthSize;
            if (size == 0 || size > sample.bytes.size() - position) {
                if (nalCount == 0) annexB.clear();
                error.clear();
                return true;
            }
            annexB.insert(annexB.end(), {0, 0, 0, 1});
            annexB.insert(annexB.end(), sample.bytes.begin() + position,
                          sample.bytes.begin() + position + size);
            position += size;
            ++nalCount;
        }
        if (nalCount == 0)
            return Fail(L"The H.264 access unit contains no NAL units");
        return true;
    }

    GUID InputSubtype() const {
        switch (track.codec) {
        case CodecId::H264:
            return MFVideoFormat_H264;
        case CodecId::Mpeg4Part2:
            return MFVideoFormat_MP4V;
        case CodecId::Mpeg2Video:
            return MFVideoFormat_MPEG2;
        case CodecId::Wmv3:
            return MFVideoFormat_WMV3;
        case CodecId::Msmpeg4v3:
            return MFVideoFormat_MP43;
        default:
            return GUID_NULL;
        }
    }

    const wchar_t* CodecName() const {
        switch (track.codec) {
        case CodecId::H264:
            return L"H.264";
        case CodecId::Mpeg4Part2:
            return L"MPEG-4 Part 2";
        case CodecId::Mpeg2Video:
            return L"MPEG-2";
        case CodecId::Wmv3:
            return L"WMV3";
        case CodecId::Msmpeg4v3:
            return L"Microsoft MPEG-4 v3";
        default:
            return L"video";
        }
    }

    bool ConfigureOutputType() {
        ComPtr<IMFMediaType> selected;
        ComPtr<IMFMediaType> yuy2Fallback;
        for (DWORD index = 0;; ++index) {
            ComPtr<IMFMediaType> candidate;
            const HRESULT available =
                transform->GetOutputAvailableType(0, index, &candidate);
            if (available == MF_E_NO_MORE_TYPES) break;
            if (FAILED(available)) {
                return Fail(HresultText(L"GetOutputAvailableType(video)",
                                        available));
            }
            GUID subtype = {};
            if (SUCCEEDED(candidate->GetGUID(MF_MT_SUBTYPE, &subtype))) {
                if (subtype == MFVideoFormat_NV12) {
                    selected = candidate;
                    break;
                }
                if (subtype == MFVideoFormat_YUY2 && !yuy2Fallback)
                    yuy2Fallback = candidate;
            }
        }
        if (!selected) selected = yuy2Fallback;
        if (!selected) {
            return Fail(
                std::wstring(L"The Windows ") + CodecName() +
                L" decoder does not expose NV12 or YUY2 output");
        }
        selected->GetGUID(MF_MT_SUBTYPE, &outputSubtype);
        HRESULT hr = transform->SetOutputType(0, selected.Get(), 0);
        if (FAILED(hr)) {
            return Fail(HresultText(L"SetOutputType(video NV12)", hr));
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
            return Fail(L"The video decoder returned invalid NV12 dimensions");
        }
        UINT32 stride = 0;
        const UINT32 minimumStride =
            outputSubtype == MFVideoFormat_YUY2 ? width * 2U : width;
        if (FAILED(selected->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) ||
            stride < minimumStride) {
            stride = minimumStride;
        }

        MFT_OUTPUT_STREAM_INFO streamInfo = {};
        hr = transform->GetOutputStreamInfo(0, &streamInfo);
        if (FAILED(hr)) {
            return Fail(HresultText(L"GetOutputStreamInfo(video)", hr));
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

    void AttachD3D11DeviceManager() {
        d3dManagerAttached = false;
        if (!h264 || !transform || !device) return;

        ComPtr<IMFAttributes> attributes;
        UINT32 d3d11Aware = FALSE;
        if (FAILED(transform->GetAttributes(&attributes)) || !attributes ||
            FAILED(attributes->GetUINT32(MF_SA_D3D11_AWARE, &d3d11Aware)) ||
            !d3d11Aware) {
            return;
        }
        if (!dxgiDeviceManager) {
            HRESULT hr = MFCreateDXGIDeviceManager(
                &dxgiResetToken, &dxgiDeviceManager);
            if (FAILED(hr) || !dxgiDeviceManager) {
                dxgiDeviceManager.Reset();
                return;
            }
            hr = dxgiDeviceManager->ResetDevice(device.Get(), dxgiResetToken);
            if (FAILED(hr)) {
                dxgiDeviceManager.Reset();
                return;
            }
        }

        const HRESULT hr = transform->ProcessMessage(
            MFT_MESSAGE_SET_D3D_MANAGER,
            reinterpret_cast<ULONG_PTR>(dxgiDeviceManager.Get()));
        d3dManagerAttached = SUCCEEDED(hr);
    }

    bool CreateTransform() {
        if (transform && d3dManagerAttached) {
            transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
        }
        transform.Reset();
        d3dManagerAttached = false;
        HRESULT hr = E_FAIL;
        if (track.codec == CodecId::Wmv3) {
            hr = CoCreateInstance(CLSID_CWMVDecMediaObject, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&transform));
        } else if (track.codec == CodecId::Msmpeg4v3) {
            hr = CoCreateInstance(CLSID_CMpeg43DecMediaObject, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&transform));
        } else if (track.codec == CodecId::Mpeg2Video) {
            hr = CoCreateInstance(CLSID_MSMPEGDecoderMFT, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&transform));
        } else if (!h264) {
            MFT_REGISTER_TYPE_INFO inputRegistration = {
                MFMediaType_Video, InputSubtype()};
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
            if (!transform && mpeg4Part2) {
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
            return Fail(HresultText(L"Create Windows video decoder", hr));
        }

        AttachD3D11DeviceManager();

        ComPtr<ICodecAPI> codecApi;
        if (SUCCEEDED(transform.As(&codecApi))) {
            VARIANT acceleration = {};
            acceleration.vt = VT_BOOL;
            acceleration.boolVal = VARIANT_TRUE;
            // This is a preference. The Microsoft transform automatically
            // falls back to software when the driver cannot decode the stream.
            if (h264) {
                codecApi->SetValue(&CODECAPI_AVDecVideoAcceleration_H264,
                                   &acceleration);
            }
        }

        ComPtr<IMFMediaType> inputType;
        hr = MFCreateMediaType(&inputType);
        if (FAILED(hr) ||
            FAILED(inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
            FAILED(inputType->SetGUID(MF_MT_SUBTYPE, InputSubtype())) ||
            FAILED(MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE,
                                      static_cast<UINT32>(track.width),
                                      static_cast<UINT32>(track.height)))) {
            return Fail(FAILED(hr) ? HresultText(L"MFCreateMediaType(H.264)", hr)
                                   : L"Could not configure the H.264 input media type");
        }
        if (track.codec != CodecId::Mpeg2Video) {
            inputType->SetUINT32(MF_MT_INTERLACE_MODE,
                                 MFVideoInterlace_Progressive);
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
        if (!h264 && track.codec != CodecId::Mpeg2Video &&
            !track.codecPrivate.empty()) {
            inputType->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER,
                               track.codecPrivate.data(),
                               static_cast<UINT32>(track.codecPrivate.size()));
            if (track.codec == CodecId::Wmv3) {
                inputType->SetBlob(
                    MF_MT_USER_DATA, track.codecPrivate.data(),
                    static_cast<UINT32>(track.codecPrivate.size()));
            }
        }
        inputType->SetUINT32(MF_MT_COMPRESSED, TRUE);
        inputType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, FALSE);
        inputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, FALSE);
        hr = transform->SetInputType(0, inputType.Get(), 0);
        if (FAILED(hr) && track.codec == CodecId::Mpeg2Video) {
            // The system MPEG-2 MFT documents a minimal elementary-stream
            // media type and rejects some otherwise normal container hints on
            // particular Windows builds.
            ComPtr<IMFMediaType> minimalType;
            if (SUCCEEDED(MFCreateMediaType(&minimalType)) &&
                SUCCEEDED(minimalType->SetGUID(MF_MT_MAJOR_TYPE,
                                               MFMediaType_Video)) &&
                SUCCEEDED(minimalType->SetGUID(MF_MT_SUBTYPE,
                                               MFVideoFormat_MPEG2))) {
                if (!track.codecPrivate.empty()) {
                    minimalType->SetBlob(
                        MF_MT_MPEG_SEQUENCE_HEADER,
                        track.codecPrivate.data(),
                        static_cast<UINT32>(track.codecPrivate.size()));
                }
                hr = transform->SetInputType(0, minimalType.Get(), 0);
            }
        }
        if (FAILED(hr)) {
            return Fail(HresultText(L"SetInputType(video)", hr));
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

    bool CreateSourceReader() {
        ComPtr<IMFAttributes> attributes;
        HRESULT hr = MFCreateAttributes(&attributes, 2);
        if (FAILED(hr))
            return Fail(HresultText(L"Create video reader attributes", hr));
        attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        hr = MFCreateSourceReaderFromURL(track.sourcePath.c_str(),
                                         attributes.Get(), &sourceReader);
        if (FAILED(hr))
            return Fail(HresultText(L"Open video source reader", hr));

        sourceReader->SetStreamSelection(
            static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        bool found = false;
        for (DWORD stream = 0; stream < 128 && !found; ++stream) {
            ComPtr<IMFMediaType> nativeType;
            hr = sourceReader->GetNativeMediaType(stream, 0, &nativeType);
            if (hr == MF_E_INVALIDSTREAMNUMBER) break;
            if (FAILED(hr)) continue;
            GUID major = {};
            if (SUCCEEDED(nativeType->GetGUID(MF_MT_MAJOR_TYPE, &major)) &&
                major == MFMediaType_Video) {
                sourceReaderStream = stream;
                found = true;
            }
        }
        if (!found)
            return Fail(L"The source reader found no video stream");
        sourceReader->SetStreamSelection(sourceReaderStream, TRUE);

        ComPtr<IMFMediaType> decodedType;
        hr = MFCreateMediaType(&decodedType);
        if (FAILED(hr) ||
            FAILED(decodedType->SetGUID(MF_MT_MAJOR_TYPE,
                                        MFMediaType_Video)) ||
            FAILED(decodedType->SetGUID(MF_MT_SUBTYPE,
                                        MFVideoFormat_NV12))) {
            return Fail(L"Could not create the MPEG-2 NV12 output type");
        }
        hr = sourceReader->SetCurrentMediaType(sourceReaderStream, nullptr,
                                               decodedType.Get());
        if (FAILED(hr))
            return Fail(HresultText(L"Configure video source reader", hr));
        ComPtr<IMFMediaType> current;
        hr = sourceReader->GetCurrentMediaType(sourceReaderStream, &current);
        if (FAILED(hr))
            return Fail(HresultText(L"Read video output type", hr));
        UINT32 width = 0, height = 0;
        if (FAILED(MFGetAttributeSize(current.Get(), MF_MT_FRAME_SIZE, &width,
                                      &height)) ||
            width == 0 || height == 0) {
            width = static_cast<UINT32>(track.width);
            height = static_cast<UINT32>(track.height);
        }
        outputWidth = width;
        outputHeight = height;
        outputStride = static_cast<LONG>(width);
        outputSubtype = MFVideoFormat_NV12;
        outputType = current;
        surfaces.clear();
        sourceReaderNeedsSeek = true;
        description = std::wstring(L"Windows Media Foundation ") +
                      CodecName() + L" source reader (NV12)";
        error.clear();
        return true;
    }

    bool Initialize(ID3D11Device* sharedDevice, const TrackInfo& sourceTrack) {
        Shutdown();
        error.clear();
        description.clear();
        parameterSets.clear();
        mpeg4SequenceHeader.clear();
        parameterSetsPending = true;
        nalLengthSize = 0;
        h264 = sourceTrack.codec == CodecId::H264;
        mpeg4Part2 = sourceTrack.codec == CodecId::Mpeg4Part2;
        const bool supported =
            h264 || mpeg4Part2 ||
            sourceTrack.codec == CodecId::Mpeg2Video ||
            sourceTrack.codec == CodecId::Wmv3 ||
            sourceTrack.codec == CodecId::Msmpeg4v3;
        if (!sharedDevice ||
            !supported ||
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
        if (mpeg4Part2) RememberMpeg4SequenceHeader(track.codecPrivate);
        device = sharedDevice;
        device->GetImmediateContext(&immediateContext);
        if (!immediateContext) return Fail(L"The D3D11 device has no immediate context");
        const bool preferSourceReader =
            !track.sourcePath.empty() &&
            (sourceTrack.codec == CodecId::H264 ||
             sourceTrack.codec == CodecId::Mpeg2Video ||
             sourceTrack.codec == CodecId::Wmv3 ||
             sourceTrack.codec == CodecId::Msmpeg4v3 ||
             (sourceTrack.codec == CodecId::Mpeg4Part2 &&
              sourceTrack.sampleEntry != "mp4v"));
        if (preferSourceReader && CreateSourceReader()) {
            return true;
        }
        sourceReader.Reset();
        error.clear();
        if ((h264 && !track.codecPrivate.empty() &&
             !ParseConfiguration(track.codecPrivate)) ||
            !CreateTransform()) {
            return false;
        }

        std::wostringstream label;
        if (!h264) {
            label << L"Windows Media Foundation " << CodecName() << L" ("
                  << std::wstring(track.sampleEntry.begin(), track.sampleEntry.end())
                  << L", NV12)";
        } else {
            label << L"Windows Media Foundation H.264 (NV12, "
                  << (d3dManagerAttached ? L"D3D11/DXVA hardware"
                                         : L"software fallback")
                  << L")";
            if (track.codecPrivate.size() >= 4) {
                label << L" - profile "
                      << static_cast<unsigned>(track.codecPrivate[1])
                      << L", level "
                      << static_cast<unsigned>(track.codecPrivate[3]) / 10
                      << L"."
                      << static_cast<unsigned>(track.codecPrivate[3]) % 10;
            } else {
                label << L" - Annex-B";
            }
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

    bool RememberMpeg4SequenceHeader(
        const std::vector<std::uint8_t>& bytes) {
        std::size_t headerStart = bytes.size();
        std::size_t vopStart = bytes.size();
        bool completeSequenceHeader = false;
        for (std::size_t i = 0; i + 4U <= bytes.size(); ++i) {
            if (bytes[i] != 0 || bytes[i + 1U] != 0 ||
                bytes[i + 2U] != 1) {
                continue;
            }
            const std::uint8_t code = bytes[i + 3U];
            if (code == 0xb6) {
                vopStart = i;
                break;
            }
            if (code == 0xb0 || code == 0xb5)
                completeSequenceHeader = true;
            if (headerStart == bytes.size() &&
                (code == 0xb0 || code == 0xb5 ||
                 (code >= 0x20 && code <= 0x2f))) {
                headerStart = i;
            }
        }
        if (headerStart == bytes.size() || headerStart >= vopStart)
            return false;
        const std::size_t headerEnd =
            vopStart == bytes.size() ? bytes.size() : vopStart;
        if (mpeg4SequenceHeader.empty() || completeSequenceHeader) {
            mpeg4SequenceHeader.assign(bytes.begin() + headerStart,
                                       bytes.begin() + headerEnd);
        }
        return completeSequenceHeader;
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
        if (outputSubtype == MFVideoFormat_NV12 &&
            SUCCEEDED(buffer.As(&twoDimensional))) {
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
        const LONG minimumStride =
            outputSubtype == MFVideoFormat_YUY2
                ? static_cast<LONG>(outputWidth * 2U)
                : static_cast<LONG>(outputWidth);
        const std::size_t stride = static_cast<std::size_t>(
            std::max<LONG>(outputStride, minimumStride));
        const std::size_t required =
            outputSubtype == MFVideoFormat_YUY2
                ? stride * outputHeight
                : stride * outputHeight * 3U / 2U;
        if (length < required) {
            buffer->Unlock();
            return Fail(L"The H.264 NV12 output buffer is truncated");
        }
        if (outputSubtype == MFVideoFormat_YUY2) {
            for (UINT y = 0; y < outputHeight; ++y) {
                const BYTE* row =
                    source + static_cast<std::size_t>(y) * stride;
                BYTE* luma =
                    nv12.data() + static_cast<std::size_t>(y) * outputWidth;
                for (UINT x = 0; x < outputWidth; x += 2U) {
                    luma[x] = row[x * 2U];
                    luma[x + 1U] = row[x * 2U + 2U];
                }
            }
            for (UINT y = 0; y < outputHeight; y += 2U) {
                const BYTE* first =
                    source + static_cast<std::size_t>(y) * stride;
                const BYTE* second =
                    source +
                    static_cast<std::size_t>(
                        std::min(y + 1U, outputHeight - 1U)) *
                        stride;
                BYTE* chroma =
                    nv12.data() + lumaBytes +
                    static_cast<std::size_t>(y / 2U) * outputWidth;
                for (UINT x = 0; x < outputWidth; x += 2U) {
                    chroma[x] = static_cast<BYTE>(
                        (static_cast<unsigned>(first[x * 2U + 1U]) +
                         second[x * 2U + 1U] + 1U) /
                        2U);
                    chroma[x + 1U] = static_cast<BYTE>(
                        (static_cast<unsigned>(first[x * 2U + 3U]) +
                         second[x * 2U + 3U] + 1U) /
                        2U);
                }
            }
        } else {
            for (UINT y = 0; y < outputHeight; ++y) {
                std::memcpy(
                    nv12.data() + static_cast<std::size_t>(y) * outputWidth,
                    source + static_cast<std::size_t>(y) * stride,
                    outputWidth);
            }
            const BYTE* chroma = source + stride * outputHeight;
            for (UINT y = 0; y < outputHeight / 2U; ++y) {
                std::memcpy(
                    nv12.data() + lumaBytes +
                        static_cast<std::size_t>(y) * outputWidth,
                    chroma + static_cast<std::size_t>(y) * stride,
                    outputWidth);
            }
        }
        buffer->Unlock();
        return true;
    }

    bool GetDxgiSurface(IMFSample* sample,
                        ComPtr<ID3D11Texture2D>& texture,
                        UINT& subresource, bool& found) {
        found = false;
        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr = sample->GetBufferByIndex(0, &buffer);
        if (FAILED(hr) || !buffer) return true;

        ComPtr<IMFDXGIBuffer> dxgiBuffer;
        if (FAILED(buffer.As(&dxgiBuffer)) || !dxgiBuffer) return true;

        hr = dxgiBuffer->GetResource(IID_PPV_ARGS(&texture));
        if (FAILED(hr) || !texture) {
            return Fail(HresultText(L"Get H.264 DXGI output texture", hr));
        }
        subresource = 0;
        hr = dxgiBuffer->GetSubresourceIndex(&subresource);
        if (FAILED(hr)) {
            return Fail(HresultText(L"Get H.264 DXGI subresource", hr));
        }

        D3D11_TEXTURE2D_DESC textureDescription = {};
        texture->GetDesc(&textureDescription);
        if (textureDescription.Format != DXGI_FORMAT_NV12 ||
            textureDescription.MipLevels == 0 ||
            textureDescription.Width < outputWidth ||
            textureDescription.Height < outputHeight ||
            subresource >= textureDescription.MipLevels *
                               textureDescription.ArraySize) {
            return Fail(L"The H.264 hardware decoder returned an invalid "
                        L"NV12 surface");
        }

        found = true;
        return true;
    }

    bool MakeFrame(IMFSample* decoded,
                    std::vector<std::shared_ptr<VideoFrame>>& output) {
        auto frame = std::make_shared<VideoFrame>();
        bool dxgiSurface = false;
        ComPtr<ID3D11Texture2D> decodedTexture;
        UINT decodedSubresource = 0;
        if (!GetDxgiSurface(decoded, decodedTexture, decodedSubresource,
                            dxgiSurface)) {
            return false;
        }

        const int surfaceIndex = AcquireSurface();
        if (surfaceIndex < 0) {
            if (error.empty()) {
                error = L"The H.264 texture pool is exhausted (" +
                        std::to_wstring(surfaces.size()) +
                        L" surfaces in use)";
            }
            return false;
        }
        if (dxgiSurface) {
            // The Microsoft decoder's DXGI samples come from a small circular
            // pool and must be returned before the next ProcessInput call.
            // Keep decoding and transfer entirely on the GPU by copying into
            // the playback queue's own NV12 texture.
            immediateContext->CopySubresourceRegion(
                surfaces[surfaceIndex].texture.Get(), 0, 0, 0, 0,
                decodedTexture.Get(), decodedSubresource, nullptr);
        } else {
            std::vector<std::uint8_t> nv12;
            if (!CopyNv12Buffer(decoded, nv12)) return false;
            immediateContext->UpdateSubresource(
                surfaces[surfaceIndex].texture.Get(), 0, nullptr, nv12.data(),
                outputWidth, static_cast<UINT>(nv12.size()));
        }
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
        LONGLONG decodedTime = 0;
        LONGLONG decodedDuration = 0;
        const bool hasDecodedTime =
            SUCCEEDED(decoded->GetSampleTime(&decodedTime));
        const bool hasDecodedDuration =
            SUCCEEDED(decoded->GetSampleDuration(&decodedDuration));
        if (hasDecodedTime) {
            frame->decoderPts = static_cast<double>(decodedTime) /
                                kHundredNanosecondsPerSecond;
        }

        LONGLONG fallbackTime = 0;
        LONGLONG fallbackDuration = 0;
        const bool hasFallback = h264 && !pendingPresentationTimes.empty();
        if (hasFallback) {
            fallbackTime = pendingPresentationTimes.top().first;
            fallbackDuration = pendingPresentationTimes.top().second;
            pendingPresentationTimes.pop();
        }

        // The Microsoft decoder occasionally returns S_OK with a zero sample
        // time for the first picture at an internal discontinuity. After a
        // seek into a multi-minute file, zero is an absent timestamp rather
        // than a real regression to the beginning of the movie. Do not let
        // that single sentinel permanently select the legacy synthetic clock;
        // the following decoded picture carries the discontinuity that the
        // Media Foundation renderer itself honors.
        const bool decodedZeroIsMissing =
            h264 && hasDecodedTime && decodedTime == 0 && hasFallback &&
            std::abs(static_cast<double>(fallbackTime) /
                     kHundredNanosecondsPerSecond) > 1.0;
        const bool hasUsableDecodedTime =
            hasDecodedTime && !decodedZeroIsMissing;
        if (decodedZeroIsMissing) {
            frame->decoderPts = std::numeric_limits<double>::quiet_NaN();
        }

        // The Windows H.264 decoder returns pictures in display order and,
        // for a well-formed MP4 ctts timeline, its output timestamp identifies
        // the actual picture. A damaged run can drop decoder output, and B
        // pictures can make adjacent output timestamps briefly arrive out of
        // order. The presentation queue sorts by PTS, so use every valid
        // decoder timestamp independently instead of permanently switching to
        // a stale synthetic timeline after one regression.
        if (hasUsableDecodedTime) {
            frame->pts = frame->decoderPts;
            if (hasDecodedDuration) {
                frame->duration = static_cast<double>(decodedDuration) /
                                  kHundredNanosecondsPerSecond;
            } else if (hasFallback) {
                frame->duration = static_cast<double>(fallbackDuration) /
                                  kHundredNanosecondsPerSecond;
            }
        } else if (hasFallback) {
            frame->pts = static_cast<double>(fallbackTime) /
                         kHundredNanosecondsPerSecond;
            frame->duration = static_cast<double>(fallbackDuration) /
                              kHundredNanosecondsPerSecond;
            frame->synthesizedPts = true;
        } else {
            if (hasUsableDecodedTime) frame->pts = frame->decoderPts;
            if (hasDecodedDuration) {
                frame->duration = static_cast<double>(decodedDuration) /
                                  kHundredNanosecondsPerSecond;
            }
        }
        if (frame->duration <= 0.0 && track.frameRate.IsValid()) {
            frame->duration = track.frameRate.denominator /
                              static_cast<double>(track.frameRate.numerator);
        }
        surfaces[surfaceIndex].displayedFrame = frame;
        output.push_back(std::move(frame));
        return true;
    }

    bool DrainOutput(std::vector<std::shared_ptr<VideoFrame>>& output,
                     unsigned maximumFrames = kMaximumDrainIterations,
                     bool allowPartial = false,
                     bool* streamDrained = nullptr) {
        if (streamDrained) *streamDrained = false;
        for (unsigned iteration = 0; iteration < maximumFrames; ++iteration) {
            MFT_OUTPUT_DATA_BUFFER outputBuffer = {};
            outputBuffer.dwStreamID = 0;
            ComPtr<IMFSample> suppliedSample;
            const bool transformProvidesSamples =
                (outputStreamFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
            if (!transformProvidesSamples) {
                HRESULT hr = MFCreateSample(&suppliedSample);
                if (FAILED(hr)) return Fail(HresultText(L"MFCreateSample(NV12)", hr));
                ComPtr<IMFMediaBuffer> mediaBuffer;
                const DWORD required =
                    outputBufferSize != 0
                        ? outputBufferSize
                        : (outputSubtype == MFVideoFormat_YUY2
                               ? outputWidth * outputHeight * 2U
                               : outputWidth * outputHeight * 3U / 2U);
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
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                if (streamDrained) *streamDrained = true;
                return true;
            }
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (!ConfigureOutputType()) return false;
                continue;
            }
            if (FAILED(hr)) return Fail(HresultText(L"ProcessOutput(H.264)", hr));
            if (!decoded) return Fail(L"The H.264 decoder produced no output sample");
            if (!MakeFrame(decoded.Get(), output)) return false;
        }
        if (allowPartial) return true;
        return Fail(L"The H.264 decoder produced too many frames for one input sample");
    }

    bool Decode(const EncodedSample& encoded,
                std::vector<std::shared_ptr<VideoFrame>>& output) {
        output.clear();
        if (sourceReader) {
            if (encoded.trackId != track.trackId)
                return Fail(
                    L"The source reader received a sample for the wrong track");
            if (sourceReaderNeedsSeek) {
                PROPVARIANT position;
                PropVariantInit(&position);
                position.vt = VT_I8;
                position.hVal.QuadPart =
                    SecondsToMediaTime(encoded.PtsSeconds());
                const HRESULT seekResult =
                    sourceReader->SetCurrentPosition(GUID_NULL, position);
                PropVariantClear(&position);
                if (FAILED(seekResult)) {
                    // Truncated AVI files can expose a valid idx1/key VOP to
                    // the built-in demuxer while the Media Foundation byte
                    // stream refuses random positioning. Fall back to feeding
                    // that independently validated access unit directly to
                    // the matching decoder transform.
                    sourceReader.Reset();
                    outputType.Reset();
                    surfaces.clear();
                    sourceReaderNeedsSeek = true;
                    if (!CreateTransform()) return false;
                    description =
                        std::wstring(L"Windows Media Foundation ") +
                        CodecName() +
                        L" transform fallback after source seek (NV12)";
                    return Decode(encoded, output);
                }
                sourceReaderNeedsSeek = false;
            }
            for (unsigned attempt = 0; attempt < 16; ++attempt) {
                DWORD actualStream = 0;
                DWORD flags = 0;
                LONGLONG timestamp = 0;
                ComPtr<IMFSample> decoded;
                const HRESULT hr = sourceReader->ReadSample(
                    sourceReaderStream, 0, &actualStream, &flags, &timestamp,
                    &decoded);
                if (FAILED(hr))
                    return Fail(HresultText(L"Decode MPEG-2 frame", hr));
                if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                    error.clear();
                    return true;
                }
                if (decoded) {
                    if (!MakeFrame(decoded.Get(), output)) return false;
                    error.clear();
                    return true;
                }
            }
            return Fail(L"The video source reader produced no frame");
        }
        if (!transform || encoded.trackId != track.trackId) {
            return Fail(L"MfH264Decoder received a sample for the wrong track");
        }
        std::vector<std::uint8_t> inputBytes;
        if (!h264) {
            std::vector<std::uint8_t> mpeg4Bytes;
            const std::vector<std::uint8_t>* encodedBytes = &encoded.bytes;
            if (mpeg4Part2) {
                std::size_t elementaryStart = encoded.bytes.size();
                for (std::size_t i = 0; i + 4U <= encoded.bytes.size(); ++i) {
                    if (encoded.bytes[i] == 0 &&
                        encoded.bytes[i + 1U] == 0 &&
                        encoded.bytes[i + 2U] == 1) {
                        elementaryStart = i;
                        break;
                    }
                }
                if (elementaryStart != 0 &&
                    elementaryStart < encoded.bytes.size()) {
                    mpeg4Bytes.assign(encoded.bytes.begin() + elementaryStart,
                                      encoded.bytes.end());
                    encodedBytes = &mpeg4Bytes;
                }
            }
            const bool sampleHasMpeg4Header =
                mpeg4Part2 && RememberMpeg4SequenceHeader(*encodedBytes);
            if (mpeg4Part2 && encoded.sync && !sampleHasMpeg4Header &&
                !mpeg4SequenceHeader.empty()) {
                // Legacy AVI files commonly carry the MPEG-4 visual
                // sequence/VOL header only in the first packet. Preserve
                // that first-party parsed header and prefix it to a
                // random-access VOP after seeking.
                inputBytes.reserve(mpeg4SequenceHeader.size() +
                                   encoded.bytes.size());
                inputBytes.insert(inputBytes.end(),
                                  mpeg4SequenceHeader.begin(),
                                  mpeg4SequenceHeader.end());
                inputBytes.insert(inputBytes.end(),
                                  encodedBytes->begin(), encodedBytes->end());
            } else {
                inputBytes = *encodedBytes;
            }
        } else if (!ConvertToAnnexB(encoded, inputBytes)) {
            return false;
        }
        if (inputBytes.empty()) {
            error.clear();
            return true;
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
        if (h264) {
            pendingPresentationTimes.emplace(
                SecondsToMediaTime(encoded.PtsSeconds()),
                SecondsToMediaTime(encoded.DurationSeconds()));
        }
        if (h264) parameterSetsPending = false;
        if (!DrainOutput(output)) return false;
        error.clear();
        return true;
    }

    bool Flush(std::vector<std::shared_ptr<VideoFrame>>& output) {
        output.clear();
        if (sourceReader) {
            error.clear();
            return true;
        }
        if (!transform) return Fail(L"The H.264 decoder is not initialized");
        if (drainComplete) {
            error.clear();
            return true;
        }
        if (!drainStarted) {
            HRESULT hr =
                transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            if (FAILED(hr)) return Fail(HresultText(L"End H.264 stream", hr));
            hr = transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
            if (FAILED(hr)) return Fail(HresultText(L"Drain H.264 stream", hr));
            drainStarted = true;
        }
        bool streamDrained = false;
        if (!DrainOutput(output, kFlushBatchSize, true, &streamDrained))
            return false;
        if (streamDrained) {
            drainStarted = false;
            drainComplete = true;
        }
        error.clear();
        return true;
    }

    bool Reset() {
        if (sourceReader) {
            const HRESULT hr = sourceReader->Flush(sourceReaderStream);
            sourceReaderNeedsSeek = true;
            surfaces.clear();
            if (FAILED(hr))
                return Fail(HresultText(L"Flush video source reader", hr));
            error.clear();
            return true;
        }
        if (!transform) return Fail(L"The H.264 decoder is not initialized");
        if (!h264) {
            // The legacy MPEG-4/WMV decoder objects do not reliably resume
            // from a random-access picture after MFT_MESSAGE_COMMAND_FLUSH.
            // Recreate the transform while preserving the parsed track
            // format so the sequence header is applied again on every seek.
            surfaces.clear();
            outputType.Reset();
            transform.Reset();
            pendingPresentationTimes = {};
            drainStarted = false;
            drainComplete = false;
            return CreateTransform();
        }
        HRESULT hr = transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        if (FAILED(hr)) return Fail(HresultText(L"Flush H.264 decoder", hr));
        hr = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        if (FAILED(hr)) return Fail(HresultText(L"Restart H.264 stream", hr));
        parameterSetsPending = h264;
        surfaces.clear();
        pendingPresentationTimes = {};
        drainStarted = false;
        drainComplete = false;
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
