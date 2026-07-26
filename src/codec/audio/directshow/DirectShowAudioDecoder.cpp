#include "codec/audio/directshow/DirectShowAudioDecoder.h"

#include <dshow.h>
#include <mmreg.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace movieplayer::codec::directshow {
namespace {

using Microsoft::WRL::ComPtr;

constexpr REFERENCE_TIME kMediaTimePerSecond = 10'000'000;
constexpr DWORD kStereoChannelMask = 0x3;
constexpr long kInitialInputBufferBytes = 4 * 1024 * 1024;

// Common DirectShow subtype for raw DTS core frames.
const GUID kMediaSubtypeDts2 = {
    0x00002001, 0x0000, 0x0010,
    {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

const GUID kMediaSubtypeDolbyDdPlus = {
    0xa7fb87af, 0x2d02, 0x42fb,
    {0xa4, 0xd4, 0x05, 0xcd, 0x93, 0x84, 0x3b, 0xdd}};

std::wstring HresultText(const wchar_t* operation, HRESULT result) {
    std::wostringstream out;
    out << operation << L" failed (HRESULT 0x" << std::hex << std::setw(8)
        << std::setfill(L'0') << static_cast<unsigned long>(result) << L")";
    return out.str();
}

REFERENCE_TIME SecondsToMediaTime(double seconds) {
    if (!std::isfinite(seconds)) return 0;
    const double value = seconds * kMediaTimePerSecond;
    return static_cast<REFERENCE_TIME>(std::llround(std::max(
        static_cast<double>((std::numeric_limits<REFERENCE_TIME>::min)()),
        std::min(static_cast<double>(
                     (std::numeric_limits<REFERENCE_TIME>::max)()),
                 value))));
}

void FreeMediaType(AM_MEDIA_TYPE& type) {
    if (type.cbFormat != 0) {
        CoTaskMemFree(type.pbFormat);
        type.cbFormat = 0;
        type.pbFormat = nullptr;
    }
    if (type.pUnk) {
        type.pUnk->Release();
        type.pUnk = nullptr;
    }
}

HRESULT CopyMediaType(AM_MEDIA_TYPE& destination,
                      const AM_MEDIA_TYPE& source) {
    FreeMediaType(destination);
    destination = source;
    destination.pbFormat = nullptr;
    destination.pUnk = nullptr;
    if (source.cbFormat != 0) {
        destination.pbFormat = static_cast<BYTE*>(
            CoTaskMemAlloc(source.cbFormat));
        if (!destination.pbFormat) {
            destination.cbFormat = 0;
            return E_OUTOFMEMORY;
        }
        std::memcpy(destination.pbFormat, source.pbFormat, source.cbFormat);
    }
    if (source.pUnk) {
        destination.pUnk = source.pUnk;
        destination.pUnk->AddRef();
    }
    return S_OK;
}

void DeleteAllocatedMediaType(AM_MEDIA_TYPE* type) {
    if (!type) return;
    FreeMediaType(*type);
    CoTaskMemFree(type);
}

struct PcmFormat {
    WORD formatTag = 0;
    WORD channels = 0;
    DWORD sampleRate = 0;
    WORD bitsPerSample = 0;
    DWORD channelMask = 0;
};

bool ParsePcmType(const AM_MEDIA_TYPE& type, int expectedRate,
                  PcmFormat* parsed = nullptr) {
    if (type.majortype != MEDIATYPE_Audio ||
        type.formattype != FORMAT_WaveFormatEx ||
        type.cbFormat < sizeof(WAVEFORMATEX) || !type.pbFormat) {
        return false;
    }
    const auto* format =
        reinterpret_cast<const WAVEFORMATEX*>(type.pbFormat);
    WORD formatTag = format->wFormatTag;
    DWORD channelMask = 0;
    if (formatTag == WAVE_FORMAT_EXTENSIBLE) {
        if (type.cbFormat < sizeof(WAVEFORMATEXTENSIBLE) ||
            format->cbSize <
                sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
            return false;
        }
        const auto* extended =
            reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        channelMask = extended->dwChannelMask;
        if (extended->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
            formatTag = WAVE_FORMAT_IEEE_FLOAT;
        else if (extended->SubFormat == KSDATAFORMAT_SUBTYPE_PCM)
            formatTag = WAVE_FORMAT_PCM;
        else
            return false;
    }
    const bool supported =
        (formatTag == WAVE_FORMAT_IEEE_FLOAT &&
         format->wBitsPerSample == 32) ||
        (formatTag == WAVE_FORMAT_PCM &&
         (format->wBitsPerSample == 16 ||
          format->wBitsPerSample == 24 ||
          format->wBitsPerSample == 32));
    const WORD bytesPerSample =
        static_cast<WORD>((format->wBitsPerSample + 7U) / 8U);
    if (!supported || format->nChannels == 0 || format->nChannels > 8 ||
        format->nBlockAlign != format->nChannels * bytesPerSample ||
        format->nSamplesPerSec != static_cast<DWORD>(expectedRate)) {
        return false;
    }
    if (parsed) {
        parsed->formatTag = formatTag;
        parsed->channels = format->nChannels;
        parsed->sampleRate = format->nSamplesPerSec;
        parsed->bitsPerSample = format->wBitsPerSample;
        parsed->channelMask = channelMask;
    }
    return true;
}

float ReadPcmValue(const BYTE* bytes, const PcmFormat& format) {
    if (format.formatTag == WAVE_FORMAT_IEEE_FLOAT) {
        float value = 0.0F;
        std::memcpy(&value, bytes, sizeof(value));
        return std::isfinite(value) ? value : 0.0F;
    }
    if (format.bitsPerSample == 16) {
        std::int16_t value = 0;
        std::memcpy(&value, bytes, sizeof(value));
        return static_cast<float>(value) / 32768.0F;
    }
    if (format.bitsPerSample == 24) {
        std::int32_t value =
            static_cast<std::int32_t>(bytes[0]) |
            (static_cast<std::int32_t>(bytes[1]) << 8) |
            (static_cast<std::int32_t>(bytes[2]) << 16);
        if ((value & 0x00800000) != 0) value |= ~0x00ffffff;
        return static_cast<float>(value) / 8388608.0F;
    }
    std::int32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return static_cast<float>(
        static_cast<double>(value) / 2147483648.0);
}

void AppendStereoPcm(const BYTE* bytes, std::size_t length,
                     const PcmFormat& format,
                     std::vector<float>& destination) {
    const std::size_t bytesPerSample =
        (format.bitsPerSample + 7U) / 8U;
    const std::size_t blockAlign =
        bytesPerSample * format.channels;
    if (blockAlign == 0 || (length % blockAlign) != 0) return;
    const std::size_t frames = length / blockAlign;
    destination.reserve(destination.size() + frames * 2U);
    constexpr float kSurround = 0.70710678F;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const BYTE* source = bytes + frame * blockAlign;
        const auto channel = [&](unsigned index) {
            return index < format.channels
                       ? ReadPcmValue(source + index * bytesPerSample, format)
                       : 0.0F;
        };
        float left = channel(0);
        float right = format.channels == 1 ? left : channel(1);
        if (format.channels == 3 || format.channels >= 5) {
            left += channel(2) * kSurround;
            right += channel(2) * kSurround;
        }
        if (format.channels == 4) {
            left += channel(2) * kSurround;
            right += channel(3) * kSurround;
        } else if (format.channels >= 5) {
            left += channel(4) * kSurround;
            if (format.channels >= 6)
                right += channel(5) * kSurround;
            else
                right += channel(4) * kSurround;
            if (format.channels >= 7) left += channel(6) * 0.5F;
            if (format.channels >= 8) right += channel(7) * 0.5F;
        }
        destination.push_back(std::clamp(left, -1.0F, 1.0F));
        destination.push_back(std::clamp(right, -1.0F, 1.0F));
    }
}

AM_MEDIA_TYPE MakeWaveMediaType(const GUID& subtype,
                                const WAVEFORMATEX& wave,
                                bool compressed) {
    AM_MEDIA_TYPE type = {};
    type.majortype = MEDIATYPE_Audio;
    type.subtype = subtype;
    type.bFixedSizeSamples = compressed ? FALSE : TRUE;
    type.bTemporalCompression = compressed ? TRUE : FALSE;
    type.lSampleSize = compressed ? 1 : wave.nBlockAlign;
    type.formattype = FORMAT_WaveFormatEx;
    type.cbFormat = sizeof(WAVEFORMATEX);
    type.pbFormat =
        static_cast<BYTE*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
    if (type.pbFormat)
        std::memcpy(type.pbFormat, &wave, sizeof(WAVEFORMATEX));
    return type;
}

class SourcePin final : public IPin {
public:
    explicit SourcePin(const AM_MEDIA_TYPE& type) {
        CopyMediaType(mediaType_, type);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,
                                             void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (id == IID_IUnknown || id == IID_IPin) {
            *object = static_cast<IPin*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE Connect(
        IPin* receivePin, const AM_MEDIA_TYPE* type) override {
        if (!receivePin || !type) return E_POINTER;
        if (connected_) return VFW_E_ALREADY_CONNECTED;
        PIN_DIRECTION direction = PINDIR_OUTPUT;
        HRESULT hr = receivePin->QueryDirection(&direction);
        if (FAILED(hr) || direction != PINDIR_INPUT)
            return VFW_E_INVALID_DIRECTION;
        hr = receivePin->ReceiveConnection(this, type);
        if (FAILED(hr)) return hr;
        connected_ = receivePin;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ReceiveConnection(
        IPin*, const AM_MEDIA_TYPE*) override {
        return VFW_E_INVALID_DIRECTION;
    }

    HRESULT STDMETHODCALLTYPE Disconnect() override {
        connected_.Reset();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ConnectedTo(IPin** pin) override {
        if (!pin) return E_POINTER;
        *pin = connected_.Get();
        if (!*pin) return VFW_E_NOT_CONNECTED;
        (*pin)->AddRef();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ConnectionMediaType(
        AM_MEDIA_TYPE* type) override {
        if (!type) return E_POINTER;
        if (!connected_) return VFW_E_NOT_CONNECTED;
        *type = {};
        return CopyMediaType(*type, mediaType_);
    }

    HRESULT STDMETHODCALLTYPE QueryPinInfo(PIN_INFO* info) override {
        if (!info) return E_POINTER;
        *info = {};
        info->dir = PINDIR_OUTPUT;
        wcscpy_s(info->achName, L"MoviePlayer compressed audio");
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryDirection(
        PIN_DIRECTION* direction) override {
        if (!direction) return E_POINTER;
        *direction = PINDIR_OUTPUT;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryId(LPWSTR* id) override {
        if (!id) return E_POINTER;
        constexpr wchar_t value[] = L"MoviePlayerAudioOut";
        *id = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(value)));
        if (!*id) return E_OUTOFMEMORY;
        std::memcpy(*id, value, sizeof(value));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryAccept(
        const AM_MEDIA_TYPE* type) override {
        return type && type->majortype == mediaType_.majortype &&
                       type->subtype == mediaType_.subtype
                   ? S_OK
                   : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE EnumMediaTypes(
        IEnumMediaTypes**) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE QueryInternalConnections(
        IPin**, ULONG*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE EndOfStream() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE BeginFlush() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE EndFlush() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE NewSegment(
        REFERENCE_TIME, REFERENCE_TIME, double) override {
        return S_OK;
    }

private:
    ~SourcePin() {
        connected_.Reset();
        FreeMediaType(mediaType_);
    }

    std::atomic<ULONG> references_{1};
    ComPtr<IPin> connected_;
    AM_MEDIA_TYPE mediaType_ = {};
};

class PcmSinkPin final : public IPin, public IMemInputPin {
public:
    explicit PcmSinkPin(int expectedRate) : expectedRate_(expectedRate) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,
                                             void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (id == IID_IUnknown || id == IID_IPin)
            *object = static_cast<IPin*>(this);
        else if (id == IID_IMemInputPin)
            *object = static_cast<IMemInputPin*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE Connect(
        IPin*, const AM_MEDIA_TYPE*) override {
        return VFW_E_INVALID_DIRECTION;
    }

    HRESULT STDMETHODCALLTYPE ReceiveConnection(
        IPin* connector, const AM_MEDIA_TYPE* type) override {
        if (!connector || !type) return E_POINTER;
        if (connected_) return VFW_E_ALREADY_CONNECTED;
        PIN_DIRECTION direction = PINDIR_INPUT;
        HRESULT hr = connector->QueryDirection(&direction);
        if (FAILED(hr) || direction != PINDIR_OUTPUT)
            return VFW_E_INVALID_DIRECTION;
        PcmFormat format;
        if (!ParsePcmType(*type, expectedRate_, &format))
            return VFW_E_TYPE_NOT_ACCEPTED;
        hr = CopyMediaType(mediaType_, *type);
        if (FAILED(hr)) return hr;
        pcmFormat_ = format;
        connected_ = connector;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Disconnect() override {
        connected_.Reset();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ConnectedTo(IPin** pin) override {
        if (!pin) return E_POINTER;
        *pin = connected_.Get();
        if (!*pin) return VFW_E_NOT_CONNECTED;
        (*pin)->AddRef();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ConnectionMediaType(
        AM_MEDIA_TYPE* type) override {
        if (!type) return E_POINTER;
        if (!connected_) return VFW_E_NOT_CONNECTED;
        *type = {};
        return CopyMediaType(*type, mediaType_);
    }

    HRESULT STDMETHODCALLTYPE QueryPinInfo(PIN_INFO* info) override {
        if (!info) return E_POINTER;
        *info = {};
        info->dir = PINDIR_INPUT;
        wcscpy_s(info->achName, L"MoviePlayer PCM");
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryDirection(
        PIN_DIRECTION* direction) override {
        if (!direction) return E_POINTER;
        *direction = PINDIR_INPUT;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryId(LPWSTR* id) override {
        if (!id) return E_POINTER;
        constexpr wchar_t value[] = L"MoviePlayerPcmIn";
        *id = static_cast<LPWSTR>(CoTaskMemAlloc(sizeof(value)));
        if (!*id) return E_OUTOFMEMORY;
        std::memcpy(*id, value, sizeof(value));
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE QueryAccept(
        const AM_MEDIA_TYPE* type) override {
        return type && ParsePcmType(*type, expectedRate_)
                   ? S_OK
                   : S_FALSE;
    }

    HRESULT STDMETHODCALLTYPE EnumMediaTypes(
        IEnumMediaTypes**) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE QueryInternalConnections(
        IPin**, ULONG*) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE EndOfStream() override { return S_OK; }

    HRESULT STDMETHODCALLTYPE BeginFlush() override {
        std::lock_guard<std::mutex> lock(mutex_);
        flushing_ = true;
        ClearUnlocked();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE EndFlush() override {
        std::lock_guard<std::mutex> lock(mutex_);
        flushing_ = false;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE NewSegment(
        REFERENCE_TIME, REFERENCE_TIME, double) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ClearUnlocked();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetAllocator(
        IMemAllocator** allocator) override {
        if (!allocator) return E_POINTER;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!allocator_) {
            HRESULT hr = CoCreateInstance(
                CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&allocator_));
            if (FAILED(hr)) return hr;
        }
        *allocator = allocator_.Get();
        (*allocator)->AddRef();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE NotifyAllocator(
        IMemAllocator* allocator, BOOL) override {
        if (!allocator) return E_POINTER;
        std::lock_guard<std::mutex> lock(mutex_);
        allocator_ = allocator;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetAllocatorRequirements(
        ALLOCATOR_PROPERTIES*) override {
        return E_NOTIMPL;
    }

    HRESULT STDMETHODCALLTYPE Receive(IMediaSample* sample) override {
        if (!sample) return E_POINTER;
        std::lock_guard<std::mutex> lock(mutex_);
        if (flushing_) return S_FALSE;

        AM_MEDIA_TYPE* changed = nullptr;
        if (sample->GetMediaType(&changed) == S_OK && changed) {
            PcmFormat changedFormat;
            if (!ParsePcmType(*changed, expectedRate_, &changedFormat)) {
                DeleteAllocatedMediaType(changed);
                return VFW_E_TYPE_NOT_ACCEPTED;
            }
            const HRESULT copyResult = CopyMediaType(mediaType_, *changed);
            DeleteAllocatedMediaType(changed);
            if (FAILED(copyResult)) return copyResult;
            pcmFormat_ = changedFormat;
        }

        BYTE* bytes = nullptr;
        HRESULT hr = sample->GetPointer(&bytes);
        if (FAILED(hr)) return hr;
        const long length = sample->GetActualDataLength();
        if (length < 0) return E_FAIL;
        if (length == 0) return S_OK;

        const std::size_t bytesPerSample =
            (pcmFormat_.bitsPerSample + 7U) / 8U;
        const std::size_t blockAlign =
            bytesPerSample * pcmFormat_.channels;
        if (blockAlign == 0 ||
            (static_cast<std::size_t>(length) % blockAlign) != 0) {
            return E_FAIL;
        }
        AppendStereoPcm(bytes, static_cast<std::size_t>(length),
                        pcmFormat_, pending_);
        if (!hasTimestamp_) {
            REFERENCE_TIME start = 0, stop = 0;
            if (SUCCEEDED(sample->GetTime(&start, &stop))) {
                pendingPts_ =
                    static_cast<double>(start) / kMediaTimePerSecond;
                hasTimestamp_ = true;
            }
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ReceiveMultiple(
        IMediaSample** samples, long count, long* processed) override {
        if (!samples || !processed) return E_POINTER;
        *processed = 0;
        for (long i = 0; i < count; ++i) {
            const HRESULT hr = Receive(samples[i]);
            if (FAILED(hr)) return hr;
            ++*processed;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ReceiveCanBlock() override { return S_FALSE; }

    void Take(AudioFrame& output, double fallbackPts) {
        std::lock_guard<std::mutex> lock(mutex_);
        output = {};
        output.sampleRate = expectedRate_;
        output.channels = 2;
        output.channelMask = kStereoChannelMask;
        output.pts = hasTimestamp_ ? pendingPts_ : fallbackPts;
        output.samples = std::move(pending_);
        pending_.clear();
        pendingPts_ = 0.0;
        hasTimestamp_ = false;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        ClearUnlocked();
    }

private:
    ~PcmSinkPin() {
        connected_.Reset();
        allocator_.Reset();
        FreeMediaType(mediaType_);
    }

    void ClearUnlocked() {
        pending_.clear();
        pendingPts_ = 0.0;
        hasTimestamp_ = false;
    }

    std::atomic<ULONG> references_{1};
    int expectedRate_ = 0;
    ComPtr<IPin> connected_;
    ComPtr<IMemAllocator> allocator_;
    AM_MEDIA_TYPE mediaType_ = {};
    PcmFormat pcmFormat_;
    std::mutex mutex_;
    std::vector<float> pending_;
    double pendingPts_ = 0.0;
    bool hasTimestamp_ = false;
    bool flushing_ = false;
};

bool FindDecoderPins(IBaseFilter* filter, ComPtr<IPin>& input,
                     ComPtr<IPin>& output) {
    ComPtr<IEnumPins> enumerator;
    if (!filter || FAILED(filter->EnumPins(&enumerator))) return false;
    for (;;) {
        ComPtr<IPin> pin;
        ULONG fetched = 0;
        const HRESULT hr = enumerator->Next(1, &pin, &fetched);
        if (hr != S_OK || fetched != 1) break;
        PIN_DIRECTION direction = PINDIR_INPUT;
        if (FAILED(pin->QueryDirection(&direction))) continue;
        if (direction == PINDIR_INPUT && !input)
            input = pin;
        else if (direction == PINDIR_OUTPUT && !output)
            output = pin;
    }
    return input && output;
}

bool EnumerateAudioDecoders(const GUID& inputSubtype,
                            std::vector<ComPtr<IBaseFilter>>& filters,
                            std::wstring& error) {
    ComPtr<IFilterMapper2> mapper;
    HRESULT hr = CoCreateInstance(
        CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&mapper));
    if (FAILED(hr)) {
        error = HresultText(L"Create DirectShow filter mapper", hr);
        return false;
    }

    const GUID inputTypes[] = {MEDIATYPE_Audio, inputSubtype};
    const GUID outputTypes[] = {MEDIATYPE_Audio, GUID_NULL};
    ComPtr<IEnumMoniker> monikers;
    hr = mapper->EnumMatchingFilters(
        &monikers, 0, FALSE, MERIT_DO_NOT_USE, TRUE, 1, inputTypes,
        nullptr, nullptr, FALSE, TRUE, 1, outputTypes, nullptr, nullptr);
    if (FAILED(hr)) {
        error = HresultText(L"Enumerate external DirectShow audio decoders",
                            hr);
        return false;
    }

    for (;;) {
        ComPtr<IMoniker> moniker;
        ULONG fetched = 0;
        hr = monikers->Next(1, &moniker, &fetched);
        if (hr != S_OK || fetched != 1) break;
        ComPtr<IBaseFilter> filter;
        if (SUCCEEDED(moniker->BindToObject(
                nullptr, nullptr, IID_PPV_ARGS(&filter))) &&
            filter) {
            filters.push_back(std::move(filter));
        }
    }
    if (filters.empty()) {
        error =
            L"No registered external DirectShow audio decoder accepted this "
            L"E-AC-3 or DTS format";
        return false;
    }
    error.clear();
    return true;
}

HRESULT ConnectPcmOutput(IPin* output, IPin* sink, int sampleRate) {
    if (!output || !sink) return E_POINTER;

    WAVEFORMATEX preferredWave = {};
    preferredWave.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    preferredWave.nChannels = 2;
    preferredWave.nSamplesPerSec = static_cast<DWORD>(sampleRate);
    preferredWave.nBlockAlign = 2 * sizeof(float);
    preferredWave.nAvgBytesPerSec =
        preferredWave.nSamplesPerSec * preferredWave.nBlockAlign;
    preferredWave.wBitsPerSample = 32;
    AM_MEDIA_TYPE preferred =
        MakeWaveMediaType(MEDIASUBTYPE_IEEE_FLOAT, preferredWave, false);
    if (!preferred.pbFormat) return E_OUTOFMEMORY;
    HRESULT lastResult = output->Connect(sink, &preferred);
    FreeMediaType(preferred);
    if (SUCCEEDED(lastResult)) return lastResult;

    ComPtr<IEnumMediaTypes> types;
    HRESULT hr = output->EnumMediaTypes(&types);
    if (FAILED(hr)) return lastResult;
    for (;;) {
        AM_MEDIA_TYPE* type = nullptr;
        ULONG fetched = 0;
        hr = types->Next(1, &type, &fetched);
        if (hr != S_OK || fetched != 1) break;
        if (type && ParsePcmType(*type, sampleRate)) {
            lastResult = output->Connect(sink, type);
            DeleteAllocatedMediaType(type);
            if (SUCCEEDED(lastResult)) return lastResult;
        } else {
            DeleteAllocatedMediaType(type);
        }
    }
    return lastResult;
}

}  // namespace

struct DirectShowAudioDecoder::Impl {
    TrackInfo track;
    ComPtr<IBaseFilter> filter;
    ComPtr<IPin> decoderInput;
    ComPtr<IPin> decoderOutput;
    ComPtr<IMemInputPin> memoryInput;
    ComPtr<IMemAllocator> inputAllocator;
    ComPtr<IPin> sourcePin;
    ComPtr<IPin> sinkPin;
    PcmSinkPin* sink = nullptr;
    bool comInitialized = false;
    bool firstSample = true;
    std::wstring description;
    std::wstring error;

    ~Impl() { Shutdown(); }

    bool Fail(const std::wstring& message) {
        error = message;
        return false;
    }

    void DisconnectDecoder() {
        if (filter) filter->Stop();
        if (inputAllocator) inputAllocator->Decommit();
        if (decoderOutput) decoderOutput->Disconnect();
        if (sinkPin) sinkPin->Disconnect();
        if (decoderInput) decoderInput->Disconnect();
        if (sourcePin) sourcePin->Disconnect();
        inputAllocator.Reset();
        memoryInput.Reset();
        sourcePin.Reset();
        sinkPin.Reset();
        sink = nullptr;
        decoderInput.Reset();
        decoderOutput.Reset();
        filter.Reset();
        firstSample = true;
    }

    void Shutdown() {
        DisconnectDecoder();
        if (comInitialized) {
            CoUninitialize();
            comInitialized = false;
        }
        track = {};
    }

    bool TryFilter(ComPtr<IBaseFilter> candidate) {
        DisconnectDecoder();
        filter = std::move(candidate);
        if (!FindDecoderPins(filter.Get(), decoderInput, decoderOutput)) {
            error =
                L"An external DirectShow audio decoder did not expose "
                L"usable input/output pins";
            return false;
        }
        WAVEFORMATEX inputWave = {};
        inputWave.wFormatTag =
            track.codec == CodecId::Dts ? WAVE_FORMAT_DTS2 : 0;
        inputWave.nChannels = static_cast<WORD>(track.channels);
        inputWave.nSamplesPerSec =
            static_cast<DWORD>(track.sampleRate);
        inputWave.nBlockAlign = 1;
        inputWave.cbSize = 0;
        const GUID& inputSubtype =
            track.codec == CodecId::Eac3
                ? kMediaSubtypeDolbyDdPlus
                : kMediaSubtypeDts2;
        AM_MEDIA_TYPE inputType =
            MakeWaveMediaType(inputSubtype, inputWave, true);
        if (!inputType.pbFormat)
            return Fail(L"Could not allocate the DirectShow audio input type");
        SourcePin* source = new (std::nothrow) SourcePin(inputType);
        if (!source) {
            FreeMediaType(inputType);
            return Fail(L"Could not allocate the DirectShow audio source pin");
        }
        sourcePin.Attach(source);
        HRESULT hr = sourcePin->Connect(decoderInput.Get(), &inputType);
        FreeMediaType(inputType);
        if (FAILED(hr)) {
            error = HresultText(
                L"Connect an external DirectShow audio decoder input", hr);
            return false;
        }

        sink = new (std::nothrow) PcmSinkPin(track.sampleRate);
        if (!sink)
            return Fail(L"Could not allocate the DirectShow PCM sink pin");
        sinkPin.Attach(static_cast<IPin*>(sink));
        hr = ConnectPcmOutput(
            decoderOutput.Get(), sinkPin.Get(), track.sampleRate);
        if (FAILED(hr)) {
            error = HresultText(
                L"Connect external DirectShow decoder PCM output", hr);
            return false;
        }

        hr = decoderInput.As(&memoryInput);
        if (FAILED(hr))
            return Fail(HresultText(
                L"Query DirectShow decoder memory input", hr));
        hr = memoryInput->GetAllocator(&inputAllocator);
        if (FAILED(hr))
            return Fail(HresultText(
                L"Get DirectShow decoder input allocator", hr));
        ALLOCATOR_PROPERTIES requested = {};
        ALLOCATOR_PROPERTIES actual = {};
        requested.cBuffers = 8;
        requested.cbBuffer = kInitialInputBufferBytes;
        requested.cbAlign = 1;
        hr = inputAllocator->SetProperties(&requested, &actual);
        if (FAILED(hr) || actual.cbBuffer < requested.cbBuffer)
            return Fail(FAILED(hr)
                            ? HresultText(
                                  L"Configure DirectShow decoder input "
                                  L"allocator",
                                  hr)
                            : L"DirectShow decoder input allocator is "
                              L"undersized");
        hr = memoryInput->NotifyAllocator(inputAllocator.Get(), FALSE);
        if (FAILED(hr))
            return Fail(HresultText(
                L"Notify DirectShow decoder allocator", hr));
        hr = inputAllocator->Commit();
        if (FAILED(hr))
            return Fail(HresultText(
                L"Commit DirectShow decoder input allocator", hr));
        hr = filter->Pause();
        if (FAILED(hr))
            return Fail(HresultText(
                L"Pause external DirectShow audio decoder", hr));
        hr = filter->Run(0);
        if (FAILED(hr))
            return Fail(HresultText(
                L"Run external DirectShow audio decoder", hr));

        firstSample = true;
        description =
            std::wstring(L"External DirectShow ") +
            (track.codec == CodecId::Eac3 ? L"E-AC-3 " : L"DTS ") +
            std::to_wstring(track.sampleRate) + L" Hz " +
            std::to_wstring(track.channels) + L" ch -> stereo PCM";
        error.clear();
        return true;
    }

    bool Initialize(const TrackInfo& sourceTrack) {
        Shutdown();
        error.clear();
        description.clear();
        if ((sourceTrack.codec != CodecId::Eac3 &&
             sourceTrack.codec != CodecId::Dts) ||
            sourceTrack.sampleRate <= 0 ||
            sourceTrack.channels <= 0 || sourceTrack.channels > 8) {
            return Fail(
                L"DirectShowAudioDecoder received an invalid E-AC-3 or DTS "
                L"track");
        }

        const HRESULT comResult =
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(comResult))
            comInitialized = true;
        else if (comResult != RPC_E_CHANGED_MODE)
            return Fail(HresultText(
                L"Initialize DirectShow audio decoding", comResult));

        track = sourceTrack;
        const GUID& inputSubtype =
            track.codec == CodecId::Eac3
                ? kMediaSubtypeDolbyDdPlus
                : kMediaSubtypeDts2;
        std::vector<ComPtr<IBaseFilter>> candidates;
        if (!EnumerateAudioDecoders(inputSubtype, candidates, error))
            return false;
        std::wstring lastCandidateError;
        for (auto& candidate : candidates) {
            if (TryFilter(std::move(candidate))) return true;
            lastCandidateError = error;
        }
        DisconnectDecoder();
        error =
            L"No registered external DirectShow audio decoder could decode "
            L"this E-AC-3 or DTS track";
        if (!lastCandidateError.empty())
            error += L". Last attempt: " + lastCandidateError;
        return false;
    }

    bool Decode(const EncodedSample& encoded, AudioFrame& frame) {
        frame = {};
        if (!memoryInput || !inputAllocator || !sink ||
            encoded.trackId != track.trackId)
            return Fail(
                L"DirectShowAudioDecoder received a sample for the wrong "
                L"track");
        if (encoded.bytes.empty() ||
            encoded.bytes.size() >
                static_cast<std::size_t>(kInitialInputBufferBytes)) {
            return Fail(
                L"The DirectShow audio input packet is empty or too large");
        }

        REFERENCE_TIME start =
            SecondsToMediaTime(encoded.PtsSeconds());
        const REFERENCE_TIME duration = std::max<REFERENCE_TIME>(
            1, SecondsToMediaTime(encoded.DurationSeconds()));
        REFERENCE_TIME stop = start + duration;
        if (firstSample) {
            const HRESULT segmentResult = decoderInput->NewSegment(
                start, (std::numeric_limits<REFERENCE_TIME>::max)(), 1.0);
            if (FAILED(segmentResult))
                return Fail(HresultText(
                    L"Start DirectShow audio segment", segmentResult));
        }

        ComPtr<IMediaSample> sample;
        HRESULT hr = inputAllocator->GetBuffer(
            &sample, nullptr, nullptr, 0);
        if (FAILED(hr))
            return Fail(HresultText(
                L"Allocate DirectShow audio sample", hr));
        BYTE* destination = nullptr;
        hr = sample->GetPointer(&destination);
        if (FAILED(hr) || !destination ||
            sample->GetSize() <
                static_cast<long>(encoded.bytes.size())) {
            return Fail(FAILED(hr)
                            ? HresultText(
                                  L"Access DirectShow audio sample buffer", hr)
                            : L"DirectShow audio sample buffer is undersized");
        }
        std::memcpy(destination, encoded.bytes.data(), encoded.bytes.size());
        if (FAILED(sample->SetActualDataLength(
                       static_cast<long>(encoded.bytes.size()))) ||
            FAILED(sample->SetTime(&start, &stop)) ||
            FAILED(sample->SetSyncPoint(TRUE)) ||
            FAILED(sample->SetPreroll(FALSE)) ||
            FAILED(sample->SetDiscontinuity(firstSample ? TRUE : FALSE))) {
            return Fail(
                L"Could not prepare the DirectShow audio input sample");
        }

        sink->Clear();
        hr = memoryInput->Receive(sample.Get());
        if (FAILED(hr))
            return Fail(HresultText(
                L"Decode with external DirectShow audio decoder", hr));
        sink->Take(frame, encoded.PtsSeconds());
        firstSample = false;
        error.clear();
        return true;
    }

    void Reset() {
        if (!decoderInput || !sink) return;
        HRESULT hr = decoderInput->BeginFlush();
        if (SUCCEEDED(hr)) hr = decoderInput->EndFlush();
        sink->Clear();
        firstSample = true;
        if (FAILED(hr))
            error = HresultText(L"Reset DirectShow audio decoder", hr);
        else
            error.clear();
    }
};

DirectShowAudioDecoder::DirectShowAudioDecoder()
    : impl_(std::make_unique<Impl>()) {}
DirectShowAudioDecoder::~DirectShowAudioDecoder() = default;

bool DirectShowAudioDecoder::Initialize(const TrackInfo& track) {
    return impl_->Initialize(track);
}

bool DirectShowAudioDecoder::Decode(const EncodedSample& sample,
                                    AudioFrame& frame) {
    return impl_->Decode(sample, frame);
}

void DirectShowAudioDecoder::Reset() { impl_->Reset(); }

const std::wstring& DirectShowAudioDecoder::Description() const noexcept {
    return impl_->description;
}

const std::wstring& DirectShowAudioDecoder::LastError() const noexcept {
    return impl_->error;
}

}  // namespace movieplayer::codec::directshow
