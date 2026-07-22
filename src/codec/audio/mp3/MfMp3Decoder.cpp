#include "codec/audio/mp3/MfMp3Decoder.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

namespace movieplayer::codec::mp3 {
namespace {

using Microsoft::WRL::ComPtr;
constexpr LONGLONG kHundredNanosecondsPerSecond = 10'000'000;

std::wstring HresultText(const wchar_t* operation, HRESULT result) {
    std::wostringstream out;
    out << operation << L" failed (HRESULT 0x" << std::hex << std::setw(8)
        << std::setfill(L'0') << static_cast<unsigned long>(result) << L")";
    return out.str();
}

LONGLONG SecondsToMediaTime(double seconds) {
    if (!std::isfinite(seconds)) return 0;
    const double value = seconds * kHundredNanosecondsPerSecond;
    return static_cast<LONGLONG>(std::llround(std::max(
        static_cast<double>((std::numeric_limits<LONGLONG>::min)()),
        std::min(static_cast<double>((std::numeric_limits<LONGLONG>::max)()),
                 value))));
}

}  // namespace

struct MfMp3Decoder::Impl {
    ComPtr<IMFTransform> transform;
    TrackInfo track;
    DWORD outputStreamFlags = 0;
    DWORD outputBufferSize = 0;
    int outputRate = 0;
    int outputChannels = 0;
    std::wstring description;
    std::wstring error;
    bool mediaFoundationStarted = false;
    bool comInitialized = false;

    ~Impl() { Shutdown(); }

    bool Fail(const std::wstring& message) {
        error = message;
        return false;
    }

    void Shutdown() {
        transform.Reset();
        if (mediaFoundationStarted) {
            MFShutdown();
            mediaFoundationStarted = false;
        }
        if (comInitialized) {
            CoUninitialize();
            comInitialized = false;
        }
        outputStreamFlags = outputBufferSize = 0;
        outputRate = outputChannels = 0;
    }

    bool ConfigureOutput() {
        ComPtr<IMFMediaType> output;
        HRESULT hr = MFCreateMediaType(&output);
        if (FAILED(hr) ||
            FAILED(output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) ||
            FAILED(output->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM)) ||
            FAILED(output->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,
                                     static_cast<UINT32>(track.channels))) ||
            FAILED(output->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
                                     static_cast<UINT32>(track.sampleRate))) ||
            FAILED(output->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16)) ||
            FAILED(output->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,
                                     static_cast<UINT32>(track.channels * 2))) ||
            FAILED(output->SetUINT32(
                MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                static_cast<UINT32>(track.sampleRate * track.channels * 2)))) {
            return Fail(FAILED(hr) ? HresultText(L"MFCreateMediaType(MP3 PCM)", hr)
                                   : L"Could not configure MP3 PCM output");
        }
        hr = transform->SetOutputType(0, output.Get(), 0);
        if (FAILED(hr)) return Fail(HresultText(L"SetOutputType(MP3 PCM)", hr));
        MFT_OUTPUT_STREAM_INFO streamInfo = {};
        hr = transform->GetOutputStreamInfo(0, &streamInfo);
        if (FAILED(hr)) return Fail(HresultText(L"GetOutputStreamInfo(MP3)", hr));
        outputStreamFlags = streamInfo.dwFlags;
        outputBufferSize = streamInfo.cbSize;
        outputRate = track.sampleRate;
        outputChannels = track.channels;
        return true;
    }

    bool Initialize(const TrackInfo& sourceTrack) {
        Shutdown();
        error.clear();
        description.clear();
        if (sourceTrack.codec != CodecId::Mp3 || sourceTrack.sampleRate <= 0 ||
            sourceTrack.channels <= 0 || sourceTrack.channels > 2) {
            return Fail(L"MfMp3Decoder received an invalid MP3 track");
        }
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(comResult)) comInitialized = true;
        else if (comResult != RPC_E_CHANGED_MODE)
            return Fail(HresultText(L"CoInitializeEx(MP3)", comResult));
        HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(hr)) return Fail(HresultText(L"MFStartup(MP3)", hr));
        mediaFoundationStarted = true;
        track = sourceTrack;
        hr = CoCreateInstance(CLSID_CMP3DecMediaObject, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&transform));
        if (FAILED(hr)) return Fail(HresultText(L"Create Windows MP3 decoder", hr));

        ComPtr<IMFMediaType> input;
        hr = MFCreateMediaType(&input);
        if (FAILED(hr)) return Fail(HresultText(L"MFCreateMediaType(MP3)", hr));
        bool initializedFromWaveFormat = false;
        if (track.codecPrivate.size() >= sizeof(WAVEFORMATEX)) {
            const auto* wave = reinterpret_cast<const WAVEFORMATEX*>(
                track.codecPrivate.data());
            const UINT32 waveBytes = static_cast<UINT32>(std::min<std::size_t>(
                track.codecPrivate.size(), sizeof(WAVEFORMATEX) + wave->cbSize));
            initializedFromWaveFormat =
                SUCCEEDED(MFInitMediaTypeFromWaveFormatEx(input.Get(), wave, waveBytes));
        }
        if (!initializedFromWaveFormat) {
            if (FAILED(input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) ||
                FAILED(input->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_MP3)) ||
                FAILED(input->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS,
                                        static_cast<UINT32>(track.channels))) ||
                FAILED(input->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
                                        static_cast<UINT32>(track.sampleRate)))) {
                return Fail(L"Could not configure the MP3 input media type");
            }
        }
        hr = transform->SetInputType(0, input.Get(), 0);
        if (FAILED(hr)) return Fail(HresultText(L"SetInputType(MP3)", hr));
        if (!ConfigureOutput()) return false;
        hr = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        if (FAILED(hr)) return Fail(HresultText(L"Begin MP3 streaming", hr));
        hr = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        if (FAILED(hr)) return Fail(HresultText(L"Start MP3 stream", hr));
        description = L"Windows Media Foundation MP3 " +
                      std::to_wstring(outputRate) + L" Hz " +
                      std::to_wstring(outputChannels) + L" ch decoder";
        error.clear();
        return true;
    }

    bool CreateInputSample(const EncodedSample& encoded,
                           ComPtr<IMFSample>& sample) {
        if (encoded.bytes.empty() || encoded.bytes.size() > MAXDWORD)
            return Fail(L"The MP3 input packet is empty or too large");
        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(encoded.bytes.size()),
                                          &buffer);
        if (FAILED(hr)) return Fail(HresultText(L"MFCreateMemoryBuffer(MP3)", hr));
        BYTE* destination = nullptr;
        DWORD capacity = 0;
        hr = buffer->Lock(&destination, &capacity, nullptr);
        if (FAILED(hr) || !destination || capacity < encoded.bytes.size()) {
            if (SUCCEEDED(hr)) buffer->Unlock();
            return Fail(FAILED(hr) ? HresultText(L"Lock MP3 input buffer", hr)
                                   : L"The MP3 input buffer is undersized");
        }
        std::memcpy(destination, encoded.bytes.data(), encoded.bytes.size());
        buffer->Unlock();
        if (FAILED(buffer->SetCurrentLength(
                static_cast<DWORD>(encoded.bytes.size()))) ||
            FAILED(MFCreateSample(&sample)) ||
            FAILED(sample->AddBuffer(buffer.Get())) ||
            FAILED(sample->SetSampleTime(SecondsToMediaTime(encoded.PtsSeconds()))) ||
            FAILED(sample->SetSampleDuration(
                SecondsToMediaTime(encoded.DurationSeconds())))) {
            return Fail(L"Could not create or timestamp the MP3 input sample");
        }
        return true;
    }

    bool AppendPcm(IMFSample* decoded, AudioFrame& frame, bool& hasTimestamp) {
        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr = decoded->ConvertToContiguousBuffer(&buffer);
        if (FAILED(hr)) return Fail(HresultText(L"Convert MP3 output buffer", hr));
        BYTE* bytes = nullptr;
        DWORD capacity = 0;
        DWORD length = 0;
        hr = buffer->Lock(&bytes, &capacity, &length);
        if (FAILED(hr) || !bytes) {
            return Fail(FAILED(hr) ? HresultText(L"Lock MP3 output buffer", hr)
                                   : L"The MP3 decoder returned an empty buffer");
        }
        const std::size_t sampleBytes = sizeof(std::int16_t);
        if (length % (sampleBytes * static_cast<std::size_t>(outputChannels)) != 0) {
            buffer->Unlock();
            return Fail(L"The MP3 decoder returned misaligned PCM");
        }
        const auto* pcm = reinterpret_cast<const std::int16_t*>(bytes);
        const std::size_t count = length / sampleBytes;
        const std::size_t previous = frame.samples.size();
        frame.samples.resize(previous + count);
        for (std::size_t i = 0; i < count; ++i)
            frame.samples[previous + i] = pcm[i] / 32768.0F;
        buffer->Unlock();
        if (!hasTimestamp) {
            LONGLONG time = 0;
            if (SUCCEEDED(decoded->GetSampleTime(&time))) {
                frame.pts = static_cast<double>(time) /
                            kHundredNanosecondsPerSecond;
                hasTimestamp = true;
            }
        }
        return true;
    }

    bool Drain(AudioFrame& frame, bool& hasTimestamp) {
        for (unsigned iteration = 0; iteration < 64; ++iteration) {
            MFT_OUTPUT_DATA_BUFFER output = {};
            ComPtr<IMFSample> supplied;
            const bool provides =
                (outputStreamFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
            if (!provides) {
                HRESULT hr = MFCreateSample(&supplied);
                if (FAILED(hr)) return Fail(HresultText(L"MFCreateSample(MP3 PCM)", hr));
                ComPtr<IMFMediaBuffer> buffer;
                const DWORD required = outputBufferSize != 0
                                           ? outputBufferSize
                                           : static_cast<DWORD>(outputRate *
                                                                 outputChannels * 2);
                hr = MFCreateMemoryBuffer(required, &buffer);
                if (FAILED(hr) || FAILED(supplied->AddBuffer(buffer.Get()))) {
                    return Fail(FAILED(hr)
                                    ? HresultText(L"MFCreateMemoryBuffer(MP3 PCM)", hr)
                                    : L"Could not attach an MP3 PCM buffer");
                }
                output.pSample = supplied.Get();
            }
            DWORD status = 0;
            HRESULT hr = transform->ProcessOutput(0, 1, &output, &status);
            if (output.pEvents) output.pEvents->Release();
            ComPtr<IMFSample> decoded;
            if (provides && output.pSample) decoded.Attach(output.pSample);
            else decoded = supplied;
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (!ConfigureOutput()) return false;
                continue;
            }
            if (FAILED(hr)) return Fail(HresultText(L"ProcessOutput(MP3)", hr));
            if (!decoded || !AppendPcm(decoded.Get(), frame, hasTimestamp)) return false;
        }
        return Fail(L"The MP3 decoder produced too much output for one packet");
    }

    bool Decode(const EncodedSample& encoded, AudioFrame& frame) {
        frame = {};
        frame.sampleRate = outputRate;
        frame.channels = outputChannels;
        frame.channelMask = outputChannels == 2 ? 3U : 4U;
        frame.pts = encoded.PtsSeconds();
        if (!transform || encoded.trackId != track.trackId)
            return Fail(L"MfMp3Decoder received a sample for the wrong track");
        ComPtr<IMFSample> input;
        if (!CreateInputSample(encoded, input)) return false;
        bool hasTimestamp = false;
        HRESULT hr = transform->ProcessInput(0, input.Get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            if (!Drain(frame, hasTimestamp)) return false;
            hr = transform->ProcessInput(0, input.Get(), 0);
        }
        if (FAILED(hr)) return Fail(HresultText(L"ProcessInput(MP3)", hr));
        if (!Drain(frame, hasTimestamp)) return false;
        // The Microsoft transform may buffer the first compressed frame while
        // it establishes the MPEG audio stream state.  An empty result is a
        // successful priming step; a following packet produces the PCM.
        error.clear();
        return true;
    }

    void Reset() {
        if (!transform) return;
        HRESULT hr = transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        if (SUCCEEDED(hr))
            hr = transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        if (FAILED(hr)) error = HresultText(L"Reset MP3 decoder", hr);
        else error.clear();
    }
};

MfMp3Decoder::MfMp3Decoder() : impl_(std::make_unique<Impl>()) {}
MfMp3Decoder::~MfMp3Decoder() = default;
bool MfMp3Decoder::Initialize(const TrackInfo& track) {
    return impl_->Initialize(track);
}
bool MfMp3Decoder::Decode(const EncodedSample& sample, AudioFrame& frame) {
    return impl_->Decode(sample, frame);
}
void MfMp3Decoder::Reset() { impl_->Reset(); }
const std::wstring& MfMp3Decoder::Description() const noexcept {
    return impl_->description;
}
const std::wstring& MfMp3Decoder::LastError() const noexcept {
    return impl_->error;
}

}  // namespace movieplayer::codec::mp3
