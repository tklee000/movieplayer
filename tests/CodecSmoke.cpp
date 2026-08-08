#include "codec/audio/aac/AacLcDecoder.h"
#include "codec/audio/ac3/Ac3Decoder.h"
#include "codec/audio/flac/FlacDecoder.h"
#include "codec/audio/directshow/DirectShowAudioDecoder.h"
#include "codec/audio/mp3/MfMp3Decoder.h"
#include "codec/audio/opus/OpusDecoder.h"
#include "codec/container/MediaDemuxer.h"
#include "codec/subtitle/TextSubtitleDecoder.h"
#include "codec/subtitle/VobSubDecoder.h"
#include "codec/video/h264/MfH264Decoder.h"
#include "codec/video/hevc/D3D11HevcDecoder.h"

#include <d3d10.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <unordered_set>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace movieplayer::codec;

namespace {

bool ReadNv12LumaMetrics(ID3D11Device* device, ID3D11DeviceContext* context,
                         const VideoFrame& frame, unsigned& range,
                         std::uint64_t& signature, std::wstring& error) {
    if (!frame.texture || frame.format != DXGI_FORMAT_NV12) return false;
    D3D11_TEXTURE2D_DESC sourceDescription = {};
    frame.texture->GetDesc(&sourceDescription);
    if (frame.arraySlice >= sourceDescription.ArraySize) {
        error = L"invalid NV12 array slice";
        return false;
    }
    D3D11_TEXTURE2D_DESC stagingDescription = sourceDescription;
    stagingDescription.ArraySize = 1;
    stagingDescription.Usage = D3D11_USAGE_STAGING;
    stagingDescription.BindFlags = 0;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDescription.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = device->CreateTexture2D(&stagingDescription, nullptr, &staging);
    if (FAILED(hr)) {
        error = L"could not create NV12 readback texture";
        return false;
    }
    const UINT sourceSubresource =
        D3D11CalcSubresource(0, frame.arraySlice, sourceDescription.MipLevels);
    context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0,
                                   frame.texture.Get(), sourceSubresource,
                                   nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr) || !mapped.pData) {
        error = L"could not map NV12 readback texture";
        return false;
    }
    unsigned minimum = 255;
    unsigned maximum = 0;
    signature = 14'695'981'039'346'656'037ULL;
    const auto* bytes = static_cast<const std::uint8_t*>(mapped.pData);
    for (int y = 0; y < frame.height; y += 8) {
        const std::uint8_t* row = bytes + static_cast<std::size_t>(y) * mapped.RowPitch;
        for (int x = 0; x < frame.width; x += 8) {
            minimum = std::min<unsigned>(minimum, row[x]);
            maximum = std::max<unsigned>(maximum, row[x]);
            signature ^= row[x];
            signature *= 1'099'511'628'211ULL;
        }
    }
    context->Unmap(staging.Get(), 0);
    range = maximum - minimum;
    return true;
}

bool PathsReferToSameFile(const std::filesystem::path& input,
                          const std::filesystem::path& output) {
    std::error_code error;
    if (std::filesystem::exists(output, error)) {
        error.clear();
        if (std::filesystem::equivalent(input, output, error) && !error) {
            return true;
        }
    }
    error.clear();
    const auto absoluteInput = std::filesystem::absolute(input, error);
    if (error) return false;
    error.clear();
    const auto absoluteOutput = std::filesystem::absolute(output, error);
    if (error) return false;
    const std::wstring normalizedInput =
        absoluteInput.lexically_normal().native();
    const std::wstring normalizedOutput =
        absoluteOutput.lexically_normal().native();
    return _wcsicmp(normalizedInput.c_str(), normalizedOutput.c_str()) == 0;
}

std::unique_ptr<IAudioDecoder> CreateAudioDecoder(const TrackInfo& track) {
    if (track.codec == CodecId::Mp3)
        return std::make_unique<mp3::MfMp3Decoder>();
    if (track.codec == CodecId::Opus)
        return std::make_unique<opus::OpusDecoder>();
    if (track.codec == CodecId::Flac)
        return std::make_unique<flac::FlacDecoder>();
    if (track.codec == CodecId::Ac3)
        return std::make_unique<ac3::Ac3Decoder>();
    if (track.codec == CodecId::Eac3 || track.codec == CodecId::Dts)
        return std::make_unique<directshow::DirectShowAudioDecoder>();
    return std::make_unique<aac::AacLcDecoder>();
}

bool DecodeCustomAudioWindow(IMediaDemuxer& demuxer, const TrackInfo& track,
                             double start, double duration,
                             std::vector<float>& mono, std::uint64_t& filled,
                             std::wstring& error) {
    for (const TrackInfo& candidate : demuxer.Tracks()) {
        if (!demuxer.SetTrackEnabled(candidate.trackId,
                                     candidate.trackId == track.trackId)) {
            error = demuxer.LastError();
            return false;
        }
    }
    double decodeStart = 0.0;
    if (!demuxer.Seek(start, decodeStart)) {
        error = demuxer.LastError();
        return false;
    }
    auto decoder = CreateAudioDecoder(track);
    if (!decoder || !decoder->Initialize(track)) {
        error = decoder ? decoder->LastError() : L"audio decoder unavailable";
        return false;
    }
    if (track.sampleRate <= 0) {
        error = L"invalid audio sample rate";
        return false;
    }
    const std::size_t wantedFrames = static_cast<std::size_t>(
        std::ceil(duration * static_cast<double>(track.sampleRate)));
    mono.assign(wantedFrames, 0.0F);
    std::vector<std::uint8_t> present(wantedFrames, 0);
    const double end = start + duration;
    for (;;) {
        EncodedSample sample;
        bool eof = false;
        if (!demuxer.ReadNextSample(sample, eof)) {
            error = demuxer.LastError();
            return false;
        }
        if (eof) break;
        if (sample.trackId != track.trackId) continue;
        AudioFrame frame;
        if (!decoder->Decode(sample, frame)) {
            error = decoder->LastError();
            return false;
        }
        if (frame.pts >= end + 1.0) break;
        if (frame.channels <= 0 || frame.sampleRate != track.sampleRate ||
            frame.samples.size() %
                    static_cast<std::size_t>(frame.channels) !=
                0) {
            error = L"custom decoder returned an invalid PCM frame";
            return false;
        }
        const std::size_t frameCount =
            frame.samples.size() / static_cast<std::size_t>(frame.channels);
        for (std::size_t i = 0; i < frameCount; ++i) {
            const double time =
                frame.pts + static_cast<double>(i) / frame.sampleRate;
            if (time < start || time >= end) continue;
            const auto destination = static_cast<std::int64_t>(std::llround(
                (time - start) * static_cast<double>(track.sampleRate)));
            if (destination < 0 ||
                static_cast<std::size_t>(destination) >= mono.size()) {
                continue;
            }
            double sum = 0.0;
            for (int channel = 0; channel < frame.channels; ++channel) {
                sum += frame.samples[i * static_cast<std::size_t>(frame.channels) +
                                     static_cast<std::size_t>(channel)];
            }
            mono[static_cast<std::size_t>(destination)] =
                static_cast<float>(sum / frame.channels);
            present[static_cast<std::size_t>(destination)] = 1;
        }
    }
    filled = static_cast<std::uint64_t>(
        std::count(present.begin(), present.end(), std::uint8_t{1}));
    return filled != 0;
}

bool DecodeMfAudioWindow(const std::wstring& path, int sampleRate, double start,
                         double duration, std::vector<float>& mono,
                         std::uint64_t& filled, std::wstring& error) {
    const HRESULT comResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitializeCom = SUCCEEDED(comResult);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        error = L"CoInitializeEx failed";
        return false;
    }
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) {
        if (uninitializeCom) CoUninitialize();
        error = L"MFStartup failed";
        return false;
    }
    bool succeeded = false;
    {
        ComPtr<IMFSourceReader> reader;
        hr = MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader);
        if (FAILED(hr)) {
            error = L"MFCreateSourceReaderFromURL failed";
        } else {
            reader->SetStreamSelection(
                static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
            hr = reader->SetStreamSelection(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE);
            if (FAILED(hr)) error = L"MF audio stream selection failed";
        }

        ComPtr<IMFMediaType> requested;
        if (SUCCEEDED(hr)) hr = MFCreateMediaType(&requested);
        if (SUCCEEDED(hr))
            hr = requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (SUCCEEDED(hr))
            hr = requested->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        if (SUCCEEDED(hr)) hr = requested->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        if (SUCCEEDED(hr))
            hr = requested->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND,
                                      static_cast<UINT32>(sampleRate));
        if (SUCCEEDED(hr))
            hr = requested->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
        if (SUCCEEDED(hr))
            hr = requested->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 8);
        if (SUCCEEDED(hr))
            hr = requested->SetUINT32(
                MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                static_cast<UINT32>(sampleRate * 2 * sizeof(float)));
        if (SUCCEEDED(hr)) {
            hr = reader->SetCurrentMediaType(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM),
                nullptr, requested.Get());
        }
        if (FAILED(hr) && error.empty()) {
            error = L"MF AAC decoder could not produce 48 kHz stereo float PCM";
        }

        PROPVARIANT position;
        PropVariantInit(&position);
        position.vt = VT_I8;
        position.hVal.QuadPart = static_cast<LONGLONG>(
            std::llround(start * 10'000'000.0));
        if (SUCCEEDED(hr)) {
            hr = reader->SetCurrentPosition(GUID_NULL, position);
        }
        PropVariantClear(&position);
        if (FAILED(hr) && error.empty()) error = L"MF audio seek failed";

        const std::size_t wantedFrames = static_cast<std::size_t>(
            std::ceil(duration * static_cast<double>(sampleRate)));
        mono.assign(wantedFrames, 0.0F);
        std::vector<std::uint8_t> present(wantedFrames, 0);
        const double end = start + duration;
        while (SUCCEEDED(hr)) {
            DWORD actualStream = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;
            hr = reader->ReadSample(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0,
                &actualStream, &flags, &timestamp, &sample);
            if (FAILED(hr)) {
                error = L"MF audio decode failed";
                break;
            }
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) break;
            if (!sample) continue;
            const double sampleStart =
                static_cast<double>(timestamp) / 10'000'000.0;
            if (sampleStart >= end + 1.0) break;
            ComPtr<IMFMediaBuffer> buffer;
            hr = sample->ConvertToContiguousBuffer(&buffer);
            if (FAILED(hr)) {
                error = L"MF PCM buffer conversion failed";
                break;
            }
            BYTE* bytes = nullptr;
            DWORD maximumLength = 0;
            DWORD currentLength = 0;
            hr = buffer->Lock(&bytes, &maximumLength, &currentLength);
            if (FAILED(hr) || !bytes) {
                error = L"MF PCM buffer lock failed";
                break;
            }
            const std::size_t frameCount =
                currentLength / (2U * sizeof(float));
            const auto* samples = reinterpret_cast<const float*>(bytes);
            for (std::size_t i = 0; i < frameCount; ++i) {
                const double time =
                    sampleStart + static_cast<double>(i) / sampleRate;
                if (time < start || time >= end) continue;
                const auto destination = static_cast<std::int64_t>(std::llround(
                    (time - start) * static_cast<double>(sampleRate)));
                if (destination < 0 ||
                    static_cast<std::size_t>(destination) >= mono.size()) {
                    continue;
                }
                mono[static_cast<std::size_t>(destination)] =
                    (samples[i * 2U] + samples[i * 2U + 1U]) * 0.5F;
                present[static_cast<std::size_t>(destination)] = 1;
            }
            buffer->Unlock();
        }
        if (SUCCEEDED(hr)) {
            filled = static_cast<std::uint64_t>(
                std::count(present.begin(), present.end(), std::uint8_t{1}));
            succeeded = filled != 0;
            if (!succeeded && error.empty()) error = L"MF returned no PCM data";
        }
    }
    MFShutdown();
    if (uninitializeCom) CoUninitialize();
    return succeeded;
}

std::vector<double> AudioEnergyEnvelope(const std::vector<float>& samples,
                                        int sampleRate) {
    const std::size_t framesPerBin =
        static_cast<std::size_t>((std::max)(1, sampleRate / 100));
    const std::size_t bins = samples.size() / framesPerBin;
    std::vector<double> result(bins, 0.0);
    for (std::size_t bin = 0; bin < bins; ++bin) {
        double energy = 0.0;
        const std::size_t begin = bin * framesPerBin;
        for (std::size_t i = 0; i < framesPerBin; ++i) {
            const double value = samples[begin + i];
            energy += value * value;
        }
        result[bin] = std::log1p(std::sqrt(energy / framesPerBin) * 1000.0);
    }
    return result;
}

std::pair<int, double> BestEnvelopeLag(const std::vector<double>& custom,
                                       const std::vector<double>& reference,
                                       std::size_t begin, std::size_t count,
                                       int maximumLag) {
    int bestLag = 0;
    double bestCorrelation = -2.0;
    for (int lag = -maximumLag; lag <= maximumLag; ++lag) {
        if (lag < 0 && begin < static_cast<std::size_t>(-lag)) continue;
        const std::size_t referenceBegin =
            lag < 0 ? begin - static_cast<std::size_t>(-lag)
                    : begin + static_cast<std::size_t>(lag);
        if (begin + count > custom.size() ||
            referenceBegin + count > reference.size()) {
            continue;
        }
        double customMean = 0.0;
        double referenceMean = 0.0;
        for (std::size_t i = 0; i < count; ++i) {
            customMean += custom[begin + i];
            referenceMean += reference[referenceBegin + i];
        }
        customMean /= count;
        referenceMean /= count;
        double numerator = 0.0;
        double customEnergy = 0.0;
        double referenceEnergy = 0.0;
        for (std::size_t i = 0; i < count; ++i) {
            const double a = custom[begin + i] - customMean;
            const double b = reference[referenceBegin + i] - referenceMean;
            numerator += a * b;
            customEnergy += a * a;
            referenceEnergy += b * b;
        }
        const double denominator = std::sqrt(customEnergy * referenceEnergy);
        const double correlation = denominator > 0.0 ? numerator / denominator
                                                      : -1.0;
        if (correlation > bestCorrelation) {
            bestCorrelation = correlation;
            bestLag = lag;
        }
    }
    return {bestLag, bestCorrelation};
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 && argc != 3) {
        std::wcerr << L"usage: MovieCodecSmoke <input> [option]\n"
                      L"       MovieCodecSmoke [option] <input>\n"
                      L"options: --probe | --audio-track=N | "
                      L"--subtitle-track=N | "
                      L"--subtitle-probe[=SECONDS] | "
                      L"--timeline-probe[=SAMPLES] | "
                      L"--frame-probe[=FRAMES] | "
                      L"--component-probe[=AUDIO_TRACK] | "
                      L"--video-component-probe | "
                      L"--video-throughput-probe=SECONDS | "
                      L"--audio-timeline-probe | "
                      L"--audio-reference-probe=START,DURATION | "
                      L"--end-probe | "
                      L"--dump-audio=OUTPUT.f32le\n";
        return 2;
    }
    std::wstring inputPath;
    std::wstring option;
    if (argc == 2) {
        inputPath = argv[1];
    } else {
        const std::wstring first = argv[1];
        const std::wstring second = argv[2];
        const bool firstIsOption = first.rfind(L"--", 0) == 0;
        const bool secondIsOption = second.rfind(L"--", 0) == 0;
        if (firstIsOption == secondIsOption) {
            std::wcerr << L"exactly one input file and one recognized option "
                          L"are required\n";
            return 2;
        }
        inputPath = firstIsOption ? second : first;
        option = firstIsOption ? first : second;
    }

    std::uint32_t requestedAudioTrack = 0;
    std::uint32_t requestedSubtitleTrack = 0;
    bool probeOnly = false;
    bool timelineProbeOnly = false;
    unsigned timelineProbeLimit = 120;
    bool frameProbeOnly = false;
    unsigned frameProbeLimit = 300;
    bool componentProbeOnly = false;
    bool videoComponentProbeOnly = false;
    bool videoThroughputProbeOnly = false;
    double videoThroughputProbeTime = 0.0;
    bool audioTimelineProbeOnly = false;
    bool audioReferenceProbeOnly = false;
    double audioReferenceStart = 0.0;
    double audioReferenceDuration = 0.0;
    bool endProbeOnly = false;
    bool subtitleProbeOnly = false;
    double subtitleProbeTime = 0.0;
    std::filesystem::path audioDumpPath;
    std::ofstream audioDump;
    if (!option.empty()) {
        constexpr wchar_t audioTrackPrefix[] = L"--audio-track=";
        constexpr wchar_t subtitleTrackPrefix[] = L"--subtitle-track=";
        constexpr wchar_t audioDumpPrefix[] = L"--dump-audio=";
        constexpr wchar_t timelineProbePrefix[] = L"--timeline-probe=";
        constexpr wchar_t frameProbePrefix[] = L"--frame-probe=";
        constexpr wchar_t componentProbePrefix[] = L"--component-probe=";
        constexpr wchar_t throughputProbePrefix[] =
            L"--video-throughput-probe=";
        constexpr wchar_t audioReferenceProbePrefix[] =
            L"--audio-reference-probe=";
        if (option == L"--probe") {
            probeOnly = true;
        } else if (option == L"--end-probe") {
            endProbeOnly = true;
        } else if (option == L"--timeline-probe") {
            timelineProbeOnly = true;
        } else if (option.rfind(timelineProbePrefix, 0) == 0) {
            try {
                const unsigned long value = std::stoul(
                    option.substr(std::size(timelineProbePrefix) - 1U));
                if (value == 0 || value > 100'000) throw std::out_of_range("limit");
                timelineProbeLimit = static_cast<unsigned>(value);
                timelineProbeOnly = true;
            } catch (const std::exception&) {
                std::wcerr << L"invalid timeline probe sample count\n";
                return 2;
            }
        } else if (option == L"--frame-probe") {
            frameProbeOnly = true;
        } else if (option.rfind(frameProbePrefix, 0) == 0) {
            try {
                const unsigned long value = std::stoul(
                    option.substr(std::size(frameProbePrefix) - 1U));
                if (value == 0 || value > 10'000) throw std::out_of_range("limit");
                frameProbeLimit = static_cast<unsigned>(value);
                frameProbeOnly = true;
            } catch (const std::exception&) {
                std::wcerr << L"invalid frame probe count\n";
                return 2;
            }
        } else if (option == L"--component-probe") {
            componentProbeOnly = true;
        } else if (option.rfind(componentProbePrefix, 0) == 0) {
            try {
                requestedAudioTrack = static_cast<std::uint32_t>(
                    std::stoul(
                        option.substr(std::size(componentProbePrefix) - 1U)));
                if (requestedAudioTrack == 0)
                    throw std::out_of_range("track");
                componentProbeOnly = true;
            } catch (const std::exception&) {
                std::wcerr << L"invalid component probe audio track\n";
                return 2;
            }
        } else if (option == L"--video-component-probe") {
            videoComponentProbeOnly = true;
        } else if (option.rfind(throughputProbePrefix, 0) == 0) {
            try {
                videoThroughputProbeTime = std::stod(
                    option.substr(std::size(throughputProbePrefix) - 1U));
                if (!std::isfinite(videoThroughputProbeTime) ||
                    videoThroughputProbeTime < 0.0) {
                    throw std::out_of_range("time");
                }
                videoThroughputProbeOnly = true;
            } catch (const std::exception&) {
                std::wcerr << L"invalid video throughput probe time\n";
                return 2;
            }
        } else if (option == L"--audio-timeline-probe") {
            audioTimelineProbeOnly = true;
        } else if (option.rfind(audioReferenceProbePrefix, 0) == 0) {
            try {
                const std::wstring values = option.substr(
                    std::size(audioReferenceProbePrefix) - 1U);
                const std::size_t comma = values.find(L',');
                if (comma == std::wstring::npos) {
                    throw std::invalid_argument("missing duration");
                }
                audioReferenceStart = std::stod(values.substr(0, comma));
                audioReferenceDuration = std::stod(values.substr(comma + 1U));
                if (!std::isfinite(audioReferenceStart) ||
                    !std::isfinite(audioReferenceDuration) ||
                    audioReferenceStart < 0.0 ||
                    audioReferenceDuration < 20.0 ||
                    audioReferenceDuration > 600.0) {
                    throw std::out_of_range("audio reference window");
                }
                audioReferenceProbeOnly = true;
            } catch (const std::exception&) {
                std::wcerr << L"invalid audio reference probe window\n";
                return 2;
            }
        } else if (option == L"--subtitle-probe") {
            subtitleProbeOnly = true;
        } else if (option.rfind(L"--subtitle-probe=", 0) == 0) {
            try {
                subtitleProbeTime = std::stod(option.substr(17));
                subtitleProbeOnly = true;
            } catch (const std::exception&) {
                std::wcerr << L"invalid subtitle probe time\n";
                return 2;
            }
        } else if (option.rfind(audioTrackPrefix, 0) == 0) {
            try {
                requestedAudioTrack = static_cast<std::uint32_t>(
                    std::stoul(
                        option.substr(std::size(audioTrackPrefix) - 1U)));
            } catch (const std::exception&) {
                std::wcerr << L"invalid audio track option\n";
                return 2;
            }
        } else if (option.rfind(subtitleTrackPrefix, 0) == 0) {
            try {
                requestedSubtitleTrack = static_cast<std::uint32_t>(
                    std::stoul(
                        option.substr(std::size(subtitleTrackPrefix) - 1U)));
                subtitleProbeOnly = true;
            } catch (const std::exception&) {
                std::wcerr << L"invalid subtitle track option\n";
                return 2;
            }
        } else if (option.rfind(audioDumpPrefix, 0) == 0) {
            audioDumpPath =
                option.substr(std::size(audioDumpPrefix) - 1U);
            if (audioDumpPath.empty()) {
                std::wcerr << L"audio dump path is empty\n";
                return 2;
            }
        } else {
            std::wcerr << L"unknown option: " << option << L"\n";
            return 2;
        }
    }
    if (!audioDumpPath.empty() &&
        PathsReferToSameFile(std::filesystem::path(inputPath), audioDumpPath)) {
        std::wcerr << L"refusing to overwrite the input media file with an "
                      L"audio dump\n";
        return 2;
    }

    std::unique_ptr<IMediaDemuxer> demuxer = CreateMediaDemuxer(inputPath);
    if (!demuxer || !demuxer->Open(inputPath)) {
        std::wcerr << L"demux open failed: "
                   << (demuxer ? demuxer->LastError()
                               : L"No media demuxer is available")
                   << L"\n";
        return 3;
    }

    const TrackInfo* videoTrack = nullptr;
    const TrackInfo* audioTrack = nullptr;
    const TrackInfo* subtitleTrack = nullptr;
    for (const TrackInfo& track : demuxer->Tracks()) {
        const std::wstring sampleEntry(track.sampleEntry.begin(),
                                       track.sampleEntry.end());
        std::wcout << L"track " << track.trackId << L" entry="
                   << sampleEntry << L" samples=" << track.sampleCount
                   << L" timescale=" << track.timeScale << L" duration="
                   << track.DurationSeconds();
        if (track.type == TrackType::Video) {
            std::wcout << L" dimensions=" << track.width << L"x" << track.height
                       << L" fps=" << track.frameRate.ToDouble()
                       << L" private=" << track.codecPrivate.size()
                       << L" source=" << track.sourcePath.size();
        } else if (track.type == TrackType::Audio) {
            std::wcout << L" rate=" << track.sampleRate
                       << L" channels=" << track.channels
                       << L" bits=" << track.bitsPerSample
                       << L" private=" << track.codecPrivate.size();
        } else if (track.type == TrackType::Subtitle) {
            std::wcout << L" language="
                       << std::wstring(track.language.begin(), track.language.end())
                       << L" name="
                       << std::wstring(track.name.begin(), track.name.end())
                       << L" default=" << (track.defaultTrack ? 1 : 0)
                       << L" forced=" << (track.forcedTrack ? 1 : 0);
        }
        std::wcout << L"\n";
        if (!videoTrack &&
            (track.codec == CodecId::H264 || track.codec == CodecId::Hevc ||
             track.codec == CodecId::Mpeg4Part2 ||
             track.codec == CodecId::Mpeg2Video ||
             track.codec == CodecId::Wmv3 ||
             track.codec == CodecId::Msmpeg4v3))
            videoTrack = &track;
        if ((track.codec == CodecId::Aac || track.codec == CodecId::Mp3 ||
             track.codec == CodecId::Opus || track.codec == CodecId::Flac ||
             track.codec == CodecId::Ac3 || track.codec == CodecId::Eac3 ||
             track.codec == CodecId::Dts) &&
            ((!audioTrack && requestedAudioTrack == 0) ||
             track.trackId == requestedAudioTrack)) {
            audioTrack = &track;
        }
        if (track.type == TrackType::Subtitle &&
            (track.codec == CodecId::Ass || track.codec == CodecId::SubRip ||
             track.codec == CodecId::VobSub) &&
            ((requestedSubtitleTrack != 0 &&
              track.trackId == requestedSubtitleTrack) ||
             (requestedSubtitleTrack == 0 &&
              (!subtitleTrack ||
               (track.defaultTrack && !subtitleTrack->defaultTrack) ||
               (track.defaultTrack == subtitleTrack->defaultTrack &&
                !track.forcedTrack && subtitleTrack->forcedTrack))))) {
            subtitleTrack = &track;
        }
    }
    if (probeOnly) return 0;
    if (timelineProbeOnly) {
        if (!videoTrack) {
            std::wcerr << L"no supported video track for timeline probe\n";
            return 4;
        }
        for (const TrackInfo& track : demuxer->Tracks()) {
            demuxer->SetTrackEnabled(track.trackId,
                                     track.trackId == videoTrack->trackId);
        }
        std::unordered_set<std::int64_t> presentationTimes;
        std::int64_t previousDts = (std::numeric_limits<std::int64_t>::min)();
        std::int64_t previousPts = (std::numeric_limits<std::int64_t>::min)();
        std::int64_t minimumOffset = (std::numeric_limits<std::int64_t>::max)();
        std::int64_t maximumOffset = (std::numeric_limits<std::int64_t>::min)();
        unsigned duplicatePts = 0;
        unsigned backwardsDts = 0;
        unsigned backwardsPts = 0;
        unsigned samplesRead = 0;
        for (; samplesRead < timelineProbeLimit; ++samplesRead) {
            EncodedSample sample;
            bool eof = false;
            if (!demuxer->ReadNextSample(sample, eof)) {
                std::wcerr << L"timeline probe demux failed: "
                           << demuxer->LastError() << L"\n";
                return 4;
            }
            if (eof) break;
            if (sample.trackId != videoTrack->trackId) {
                continue;
            }
            const std::int64_t offset =
                sample.presentationTime - sample.decodeTime;
            minimumOffset = std::min(minimumOffset, offset);
            maximumOffset = std::max(maximumOffset, offset);
            if (!presentationTimes.insert(sample.presentationTime).second) {
                ++duplicatePts;
            }
            if (previousDts != (std::numeric_limits<std::int64_t>::min)() &&
                sample.decodeTime < previousDts) {
                ++backwardsDts;
            }
            if (previousPts != (std::numeric_limits<std::int64_t>::min)() &&
                sample.presentationTime < previousPts) {
                ++backwardsPts;
            }
            if (samplesRead < 48) {
                std::wcout << L"sample " << samplesRead
                           << L" dts=" << sample.decodeTime
                           << L" pts=" << sample.presentationTime
                           << L" ctts=" << offset
                           << L" duration=" << sample.duration
                           << L" sync=" << (sample.sync ? 1 : 0)
                           << L" bytes=" << sample.bytes.size() << L"\n";
            }
            previousDts = sample.decodeTime;
            previousPts = sample.presentationTime;
        }
        std::wcout << L"timeline-summary samples=" << samplesRead
                   << L" duplicate-pts=" << duplicatePts
                   << L" backwards-dts=" << backwardsDts
                   << L" backwards-pts=" << backwardsPts
                   << L" min-ctts=" << minimumOffset
                   << L" max-ctts=" << maximumOffset << L"\n";
        return 0;
    }
    if (audioTimelineProbeOnly) {
        if (!audioTrack) {
            std::wcerr << L"no supported audio track for timeline probe\n";
            return 30;
        }
        for (const TrackInfo& track : demuxer->Tracks()) {
            demuxer->SetTrackEnabled(track.trackId,
                                     track.trackId == audioTrack->trackId);
        }
        auto audio = CreateAudioDecoder(*audioTrack);
        if (!audio->Initialize(*audioTrack)) {
            std::wcerr << L"audio timeline decoder init failed: "
                       << audio->LastError() << L"\n";
            return 30;
        }

        std::uint64_t packets = 0;
        std::uint64_t decodedFrames = 0;
        std::uint64_t decodedSamples = 0;
        std::uint64_t failures = 0;
        bool haveTimeline = false;
        double firstPts = 0.0;
        double expectedPts = 0.0;
        double maximumGap = 0.0;
        double maximumOverlap = 0.0;
        double maximumPacketPtsError = 0.0;
        for (;;) {
            EncodedSample sample;
            bool eof = false;
            if (!demuxer->ReadNextSample(sample, eof)) {
                std::wcerr << L"audio timeline demux failed: "
                           << demuxer->LastError() << L"\n";
                return 31;
            }
            if (eof) break;
            if (sample.trackId != audioTrack->trackId) continue;
            ++packets;
            AudioFrame frame;
            if (!audio->Decode(sample, frame)) {
                ++failures;
                std::wcerr << L"audio timeline decode failure packet="
                           << packets << L" pts=" << sample.PtsSeconds()
                           << L" error=" << audio->LastError() << L"\n";
                audio->Reset();
                continue;
            }
            if (frame.samples.empty()) continue;
            if (frame.channels <= 0 || frame.sampleRate <= 0 ||
                frame.samples.size() % static_cast<std::size_t>(frame.channels) != 0) {
                std::wcerr << L"audio timeline decoder returned an invalid frame\n";
                return 32;
            }
            const std::uint64_t frameSamples =
                frame.samples.size() / static_cast<std::size_t>(frame.channels);
            const double frameDuration =
                static_cast<double>(frameSamples) / frame.sampleRate;
            maximumPacketPtsError = std::max(
                maximumPacketPtsError,
                std::abs(frame.pts - sample.PtsSeconds()));
            if (!haveTimeline) {
                firstPts = frame.pts;
                haveTimeline = true;
            } else {
                const double discontinuity = frame.pts - expectedPts;
                maximumGap = std::max(maximumGap, discontinuity);
                maximumOverlap = std::max(maximumOverlap, -discontinuity);
            }
            expectedPts = frame.pts + frameDuration;
            decodedSamples += frameSamples;
            ++decodedFrames;
            if ((packets % 20000U) == 0U) {
                std::wcout << L"audio-timeline-progress packets=" << packets
                           << L" pts=" << frame.pts << L"\n";
            }
        }
        const double decodedDuration =
            audioTrack->sampleRate > 0
                ? static_cast<double>(decodedSamples) / audioTrack->sampleRate
                : 0.0;
        const double trackEnd = firstPts + decodedDuration;
        const double durationError =
            std::abs(trackEnd - audioTrack->DurationSeconds());
        std::wcout << L"audio-timeline-summary packets=" << packets
                   << L" frames=" << decodedFrames
                   << L" samples=" << decodedSamples
                   << L" first-pts=" << firstPts
                   << L" last-end=" << expectedPts
                   << L" max-gap-ms=" << maximumGap * 1000.0
                   << L" max-overlap-ms=" << maximumOverlap * 1000.0
                   << L" max-packet-pts-error-ms="
                   << maximumPacketPtsError * 1000.0
                   << L" duration-error-ms=" << durationError * 1000.0
                   << L" failures=" << failures << L"\n";
        return failures == 0 && packets == decodedFrames &&
                       maximumGap < 0.0001 && maximumOverlap < 0.0001 &&
                       maximumPacketPtsError < 0.0001 && durationError < 0.001
                   ? 0
                   : 33;
    }
    if (audioReferenceProbeOnly) {
        if (!audioTrack || audioTrack->sampleRate <= 0) {
            std::wcerr << L"no supported audio track for reference probe\n";
            return 34;
        }
        std::vector<float> custom;
        std::vector<float> reference;
        std::uint64_t customFilled = 0;
        std::uint64_t referenceFilled = 0;
        std::wstring error;
        if (!DecodeCustomAudioWindow(*demuxer, *audioTrack,
                                     audioReferenceStart,
                                     audioReferenceDuration, custom,
                                     customFilled, error)) {
            std::wcerr << L"custom audio window decode failed: " << error
                       << L"\n";
            return 35;
        }
        if (!DecodeMfAudioWindow(inputPath, audioTrack->sampleRate,
                                 audioReferenceStart,
                                 audioReferenceDuration, reference,
                                 referenceFilled, error)) {
            std::wcerr << L"MF audio window decode failed: " << error
                       << L"\n";
            return 36;
        }
        const auto customEnvelope =
            AudioEnergyEnvelope(custom, audioTrack->sampleRate);
        const auto referenceEnvelope =
            AudioEnergyEnvelope(reference, audioTrack->sampleRate);
        double customEnergy = 0.0;
        double referenceEnergy = 0.0;
        double crossEnergy = 0.0;
        double customPeak = 0.0;
        double referencePeak = 0.0;
        const std::size_t comparedSamples =
            std::min(custom.size(), reference.size());
        for (std::size_t i = 0; i < comparedSamples; ++i) {
            const double customValue = custom[i];
            const double referenceValue = reference[i];
            customEnergy += customValue * customValue;
            referenceEnergy += referenceValue * referenceValue;
            crossEnergy += customValue * referenceValue;
            customPeak = std::max(customPeak, std::abs(customValue));
            referencePeak = std::max(referencePeak, std::abs(referenceValue));
        }
        const double levelRatio = referenceEnergy > 0.0
                                      ? std::sqrt(customEnergy / referenceEnergy)
                                      : 0.0;
        const double sampleCorrelation =
            customEnergy > 0.0 && referenceEnergy > 0.0
                ? crossEnergy / std::sqrt(customEnergy * referenceEnergy)
                : 0.0;
        constexpr std::size_t kAnalysisBins = 1200U;
        constexpr std::size_t kStepBins = 1000U;
        constexpr int kMaximumLagBins = 200;
        int minimumDelay = (std::numeric_limits<int>::max)();
        int maximumDelay = (std::numeric_limits<int>::min)();
        double minimumCorrelation = 1.0;
        unsigned windows = 0;
        for (std::size_t begin = 200U;
             begin + kAnalysisBins +
                     static_cast<std::size_t>(kMaximumLagBins) <=
                 customEnvelope.size() &&
             begin + kAnalysisBins +
                     static_cast<std::size_t>(kMaximumLagBins) <=
                 referenceEnvelope.size();
             begin += kStepBins) {
            const auto [lag, correlation] = BestEnvelopeLag(
                customEnvelope, referenceEnvelope, begin, kAnalysisBins,
                kMaximumLagBins);
            const int customDelayMilliseconds = -lag * 10;
            minimumDelay = std::min(minimumDelay, customDelayMilliseconds);
            maximumDelay = std::max(maximumDelay, customDelayMilliseconds);
            minimumCorrelation = std::min(minimumCorrelation, correlation);
            ++windows;
            std::wcout << L"audio-reference-window media="
                       << audioReferenceStart +
                              static_cast<double>(begin) / 100.0
                       << L" custom-content-delay-ms="
                       << customDelayMilliseconds << L" correlation="
                       << correlation << L"\n";
        }
        std::wcout << L"audio-reference-summary start="
                   << audioReferenceStart << L" duration="
                   << audioReferenceDuration << L" custom-filled="
                   << customFilled << L" reference-filled="
                   << referenceFilled << L" windows=" << windows
                   << L" min-delay-ms=" << minimumDelay
                   << L" max-delay-ms=" << maximumDelay
                   << L" delay-span-ms=" << maximumDelay - minimumDelay
                   << L" min-correlation=" << minimumCorrelation
                   << L" level-ratio=" << levelRatio
                   << L" sample-correlation=" << sampleCorrelation
                   << L" custom-peak=" << customPeak
                   << L" reference-peak=" << referencePeak << L"\n";
        const bool levelMatchesReference =
            audioTrack->channels != 2 ||
            (levelRatio >= 0.85 && levelRatio <= 1.15);
        return windows != 0 && minimumCorrelation > 0.50 &&
                       maximumDelay - minimumDelay <= 100 &&
                       levelMatchesReference
                   ? 0
                   : 37;
    }
    const bool needsAudio =
        !frameProbeOnly && !videoComponentProbeOnly &&
        !videoThroughputProbeOnly && !endProbeOnly;
    if (!videoTrack || (needsAudio && !audioTrack)) {
        std::wcerr << L"expected the requested supported media tracks\n";
        return 4;
    }
    if (requestedAudioTrack != 0) {
        for (const TrackInfo& track : demuxer->Tracks()) {
            if (track.type == TrackType::Audio)
                demuxer->SetTrackEnabled(track.trackId,
                                         track.trackId == requestedAudioTrack);
        }
    }
    if (subtitleTrack) {
        for (const TrackInfo& track : demuxer->Tracks()) {
            if (track.type == TrackType::Subtitle)
                demuxer->SetTrackEnabled(track.trackId,
                                         track.trackId == subtitleTrack->trackId);
        }
    }
    if (subtitleProbeOnly) {
        if (!subtitleTrack) {
            std::wcerr << L"no supported embedded subtitle track\n";
            return 18;
        }
        for (const TrackInfo& track : demuxer->Tracks()) {
            demuxer->SetTrackEnabled(track.trackId,
                                     track.trackId == subtitleTrack->trackId);
        }
        if (subtitleProbeTime > 0.0) {
            double decodeStart = 0.0;
            if (!demuxer->Seek(subtitleProbeTime, decodeStart)) {
                std::wcerr << L"subtitle probe seek failed: "
                           << demuxer->LastError() << L"\n";
                return 19;
            }
        }
        for (unsigned i = 0; i < 10000; ++i) {
            EncodedSample sample;
            bool eof = false;
            if (!demuxer->ReadNextSample(sample, eof)) {
                std::wcerr << L"subtitle demux failed: "
                           << demuxer->LastError() << L"\n";
                return 19;
            }
            if (eof) break;
            if (sample.trackId != subtitleTrack->trackId) continue;
            std::wstring subtitleError;
            if (subtitleTrack->codec == CodecId::VobSub) {
                subtitle::VobSubFrame decoded;
                if (!subtitle::DecodeVobSubSample(*subtitleTrack, sample,
                                                  decoded, subtitleError)) {
                    std::wcerr << L"VobSub decode failed: " << subtitleError
                               << L"\n";
                    return 20;
                }
                const std::size_t visible = static_cast<std::size_t>(
                    std::count_if(decoded.bitmap.bgra.begin() + 3,
                                  decoded.bitmap.bgra.end(),
                                  [index = std::size_t{3}](std::uint8_t value) mutable {
                                      const bool alpha = (index & 3U) == 3U &&
                                                         value != 0;
                                      ++index;
                                      return alpha;
                                  }));
                std::wcout << L"subtitle-ok track=" << subtitleTrack->trackId
                           << L" pts=" << sample.PtsSeconds() << L" canvas="
                           << decoded.bitmap.canvasWidth << L"x"
                           << decoded.bitmap.canvasHeight << L" rect="
                           << decoded.bitmap.x << L"," << decoded.bitmap.y
                           << L" " << decoded.bitmap.width << L"x"
                           << decoded.bitmap.height << L" visible=" << visible
                           << L" duration="
                           << (decoded.endDelaySeconds -
                               decoded.startDelaySeconds)
                           << L"\n";
                return visible != 0 ? 0 : 21;
            }
            std::wstring text;
            if (!subtitle::DecodeTextSample(*subtitleTrack, sample, text,
                                            subtitleError)) {
                std::wcerr << L"text subtitle decode failed: "
                           << subtitleError << L"\n";
                return 20;
            }
            std::wcout << L"subtitle-ok track=" << subtitleTrack->trackId
                       << L" pts=" << sample.PtsSeconds() << L" length="
                       << text.size() << L" duration="
                       << sample.DurationSeconds() << L"\n";
            return text.empty() ? 21 : 0;
        }
        std::wcerr << L"no subtitle sample was found\n";
        return 22;
    }

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,
                                        D3D_FEATURE_LEVEL_11_0};
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL selected = {};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                   levels, static_cast<UINT>(std::size(levels)),
                                   D3D11_SDK_VERSION, &device, &selected, &context);
    if (FAILED(hr)) {
        std::wcerr << L"D3D11CreateDevice failed: 0x" << std::hex << hr << L"\n";
        return 5;
    }
    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(context.As(&multithread))) multithread->SetMultithreadProtected(TRUE);

    std::unique_ptr<IVideoDecoder> video;
    if (videoTrack->codec == CodecId::H264 ||
        videoTrack->codec == CodecId::Mpeg4Part2 ||
        videoTrack->codec == CodecId::Mpeg2Video ||
        videoTrack->codec == CodecId::Wmv3 ||
        videoTrack->codec == CodecId::Msmpeg4v3)
        video = std::make_unique<h264::MfH264Decoder>();
    else
        video = std::make_unique<hevc::D3D11HevcDecoder>();
    if (!video->Initialize(device.Get(), *videoTrack)) {
        std::wcerr << L"video init failed: " << video->LastError() << L"\n";
        return 6;
    }
    std::wcout << L"decoder: " << video->Description() << L"\n";
    if (videoThroughputProbeOnly) {
        for (const TrackInfo& track : demuxer->Tracks()) {
            demuxer->SetTrackEnabled(track.trackId,
                                     track.trackId == videoTrack->trackId);
        }
        double decodeStart = 0.0;
        if (!demuxer->Seek(videoThroughputProbeTime, decodeStart) ||
            !video->Reset()) {
            std::wcerr << L"throughput probe seek/reset failed: "
                       << demuxer->LastError() << L"\n";
            return 34;
        }
        constexpr double probeMediaDuration = 60.0;
        const double probeEnd = std::min(
            demuxer->DurationSeconds(),
            videoThroughputProbeTime + probeMediaDuration);
        std::deque<std::shared_ptr<VideoFrame>> retainedFrames;
        std::uint64_t decodedFrames = 0;
        std::uint64_t timestampAnomalies = 0;
        double previousDecoderPts =
            std::numeric_limits<double>::quiet_NaN();
        const auto beginning = std::chrono::steady_clock::now();
        bool complete = false;
        while (!complete) {
            EncodedSample sample;
            bool eof = false;
            if (!demuxer->ReadNextSample(sample, eof)) {
                std::wcerr << L"throughput probe demux failed: "
                           << demuxer->LastError() << L"\n";
                return 35;
            }
            if (eof) break;
            if (sample.trackId != videoTrack->trackId) continue;
            std::vector<std::shared_ptr<VideoFrame>> frames;
            if (!video->Decode(sample, frames)) {
                std::wcerr << L"throughput probe decode failed: "
                           << video->LastError() << L"\n";
                return 36;
            }
            for (const auto& frame : frames) {
                if (!frame || !frame->texture) return 36;
                const bool decoderRegressed =
                    std::isfinite(frame->decoderPts) &&
                    std::isfinite(previousDecoderPts) &&
                    frame->decoderPts + 0.001 < previousDecoderPts;
                const bool timestampDiverged =
                    std::isfinite(frame->decoderPts) &&
                    std::abs(frame->decoderPts - frame->pts) > 0.050;
                if ((decoderRegressed || timestampDiverged ||
                     frame->synthesizedPts) &&
                    timestampAnomalies < 120U) {
                    std::wcout << L"video-timestamp-anomaly index="
                               << decodedFrames << L" pts=" << frame->pts
                               << L" decoder-pts=" << frame->decoderPts
                               << L" previous-decoder-pts="
                               << previousDecoderPts << L" regressed="
                               << (decoderRegressed ? 1 : 0)
                               << L" synthesized="
                               << (frame->synthesizedPts ? 1 : 0) << L"\n";
                    ++timestampAnomalies;
                }
                if (std::isfinite(frame->decoderPts))
                    previousDecoderPts = frame->decoderPts;
                retainedFrames.push_back(frame);
                while (retainedFrames.size() > 9U)
                    retainedFrames.pop_front();
                if (frame->pts >= videoThroughputProbeTime) ++decodedFrames;
                if (frame->pts >= probeEnd) {
                    complete = true;
                    break;
                }
            }
        }

        D3D11_QUERY_DESC queryDescription = {};
        queryDescription.Query = D3D11_QUERY_EVENT;
        ComPtr<ID3D11Query> completionQuery;
        if (FAILED(device->CreateQuery(&queryDescription, &completionQuery)))
            return 37;
        context->End(completionQuery.Get());
        context->Flush();
        while (context->GetData(completionQuery.Get(), nullptr, 0, 0) ==
               S_FALSE) {
            Sleep(1);
        }
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - beginning).count();
        const double framesPerSecond =
            elapsed > 0.0 ? decodedFrames / elapsed : 0.0;
        std::wcout << L"video-throughput-probe target="
                   << videoThroughputProbeTime
                   << L" decode-start=" << decodeStart
                   << L" media-seconds="
                   << (probeEnd - videoThroughputProbeTime)
                   << L" frames=" << decodedFrames
                   << L" elapsed=" << elapsed
                   << L" fps=" << framesPerSecond << L"\n";
        return complete && framesPerSecond >= 30.0 ? 0 : 38;
    }
    if (endProbeOnly) {
        for (const TrackInfo& track : demuxer->Tracks()) {
            demuxer->SetTrackEnabled(track.trackId,
                                     track.trackId == videoTrack->trackId);
        }
        const double target =
            std::max(0.0, demuxer->DurationSeconds() - 20.0);
        double decodeStart = 0.0;
        if (!demuxer->Seek(target, decodeStart) || !video->Reset()) {
            std::wcerr << L"end probe seek/reset failed: "
                       << demuxer->LastError() << L"\n";
            return 24;
        }

        std::deque<std::shared_ptr<VideoFrame>> retainedFrames;
        unsigned decodedFrames = 0;
        unsigned flushedFrames = 0;
        const auto retain = [&](const std::shared_ptr<VideoFrame>& frame) {
            if (!frame || !frame->texture) return false;
            retainedFrames.push_back(frame);
            while (retainedFrames.size() > 9U) retainedFrames.pop_front();
            return true;
        };

        for (;;) {
            EncodedSample sample;
            bool eof = false;
            if (!demuxer->ReadNextSample(sample, eof)) {
                std::wcerr << L"end probe demux failed: "
                           << demuxer->LastError() << L"\n";
                return 25;
            }
            if (eof) break;
            if (sample.trackId != videoTrack->trackId) continue;
            std::vector<std::shared_ptr<VideoFrame>> frames;
            if (!video->Decode(sample, frames)) {
                std::wcerr << L"end probe decode failed: "
                           << video->LastError() << L"\n";
                return 26;
            }
            for (const auto& frame : frames) {
                if (!retain(frame)) {
                    std::wcerr << L"end probe received an invalid frame\n";
                    return 26;
                }
                ++decodedFrames;
            }
        }

        unsigned flushBatches = 0;
        for (; flushBatches < 64U; ++flushBatches) {
            std::vector<std::shared_ptr<VideoFrame>> frames;
            if (!video->Flush(frames)) {
                std::wcerr << L"end probe flush failed: "
                           << video->LastError() << L"\n";
                return 27;
            }
            if (frames.empty()) {
                std::wcout << L"end-probe-ok target=" << target
                           << L" decode-start=" << decodeStart
                           << L" decoded=" << decodedFrames
                           << L" flushed=" << flushedFrames
                           << L" batches=" << flushBatches << L"\n";
                return decodedFrames != 0 ? 0 : 28;
            }
            for (const auto& frame : frames) {
                if (!retain(frame)) {
                    std::wcerr << L"end probe flush returned an invalid frame\n";
                    return 27;
                }
                ++flushedFrames;
            }
        }
        std::wcerr << L"end probe did not finish draining the decoder\n";
        return 27;
    }

    std::unique_ptr<IAudioDecoder> audio;
    if (needsAudio) {
        audio = CreateAudioDecoder(*audioTrack);
        if (!audio->Initialize(*audioTrack)) {
            std::wcerr << L"audio init failed: " << audio->LastError()
                       << L"\n";
            return 7;
        }
        std::wcout << L"audio decoder: " << audio->Description() << L"\n";
    }
    if (!audioDumpPath.empty()) {
        audioDump.open(audioDumpPath, std::ios::binary | std::ios::trunc);
        if (!audioDump) {
            std::wcerr << L"could not create decoded audio dump\n";
            return 7;
        }
    }

    const bool hevcVideo = videoTrack->codec == CodecId::Hevc;
    const bool hevcMain10 =
        hevcVideo && videoTrack->codecPrivate.size() > 17U &&
        (videoTrack->codecPrivate[17] & 7U) == 2U;
    const DXGI_FORMAT expectedVideoFormat =
        hevcMain10 ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    const bool nv12Video = expectedVideoFormat == DXGI_FORMAT_NV12;
    double audioEnergy = 0.0;
    std::uint64_t audioValues = 0;
    unsigned bestLumaRange = 0;
    std::vector<std::uint64_t> frameSignatures;
    std::vector<double> framePresentationTimes;
    unsigned subtitleSamples = 0;
    std::wstring firstSubtitle;
    double firstSubtitleStart = 0.0;
    double firstSubtitleDuration = 0.0;
    std::deque<std::shared_ptr<VideoFrame>> retainedPlaybackFrames;
    std::shared_ptr<VideoFrame> retainedDisplayedFrame;
    const auto decodeSpan = [&](unsigned wantedVideo, unsigned wantedAudio,
                                const wchar_t* label) -> bool {
        unsigned videoFrames = 0;
        unsigned audioFrames = 0;
        double previousVideoPts = -std::numeric_limits<double>::infinity();
        for (unsigned i = 0;
              i < 20000 &&
              (videoFrames < wantedVideo || audioFrames < wantedAudio ||
               (!frameProbeOnly && subtitleTrack && subtitleSamples == 0));
             ++i) {
            EncodedSample sample;
            bool eof = false;
            if (!demuxer->ReadNextSample(sample, eof)) {
                std::wcerr << L"demux read failed: " << demuxer->LastError() << L"\n";
                return false;
            }
            if (eof) break;
            if (std::wcscmp(label, L"seek") == 0 && i < 4U) {
                std::wcout << L"seek-sample track=" << sample.trackId
                           << L" pts=" << sample.PtsSeconds()
                           << L" sync=" << (sample.sync ? 1 : 0)
                           << L" bytes=" << sample.bytes.size() << L"\n";
                if (sample.type == TrackType::Video) {
                    unsigned shown = 0;
                    for (std::size_t position = 0;
                         position + 4U <= sample.bytes.size() && shown < 8U;
                         ++position) {
                        if (sample.bytes[position] == 0 &&
                            sample.bytes[position + 1U] == 0 &&
                            sample.bytes[position + 2U] == 1) {
                            std::wcout << L"  start-code=0x" << std::hex
                                       << static_cast<unsigned>(
                                              sample.bytes[position + 3U])
                                       << std::dec << L" at=" << position;
                            if (sample.bytes[position + 3U] == 0xb6 &&
                                position + 4U < sample.bytes.size()) {
                                std::wcout << L" vop-type="
                                           << (sample.bytes[position + 4U] >> 6U);
                            }
                            std::wcout << L"\n";
                            ++shown;
                        }
                    }
                }
            }
            if (sample.trackId == videoTrack->trackId) {
                std::vector<std::shared_ptr<VideoFrame>> frames;
                if (!video->Decode(sample, frames)) {
                    std::wcerr << L"video decode failed in " << label << L": "
                               << video->LastError() << L"\n";
                    return false;
                }
                for (const auto& frame : frames) {
                    if (!frame || !frame->texture ||
                        frame->format != expectedVideoFormat ||
                        frame->width != videoTrack->width ||
                        frame->height != videoTrack->height ||
                        !std::isfinite(frame->pts) ||
                        // Matroska stores presentation timestamps while Block
                        // order follows decode dependencies. HEVC B pictures
                        // in the target title can move about four frames back;
                        // PlayerEngine sorts this bounded reordering by PTS.
                        frame->pts + 0.500 < previousVideoPts) {
                        std::wcerr << L"video decoder returned an invalid or unordered surface"
                                   << L" texture=" << (frame && frame->texture ? 1 : 0)
                                   << L" format=" << (frame ? frame->format : DXGI_FORMAT_UNKNOWN)
                                   << L" size=" << (frame ? frame->width : 0) << L"x"
                                   << (frame ? frame->height : 0) << L" pts="
                                   << (frame ? frame->pts : -1.0) << L" previous="
                                   << previousVideoPts << L"\n";
                        return false;
                    }
                    previousVideoPts = frame->pts;
                    if (nv12Video &&
                        (frameProbeOnly ||
                         (bestLumaRange < 8 && (videoFrames % 30U) == 0))) {
                        unsigned range = 0;
                        std::uint64_t signature = 0;
                        std::wstring readbackError;
                        if (!ReadNv12LumaMetrics(
                                device.Get(), context.Get(), *frame, range,
                                signature, readbackError)) {
                            std::wcerr << L"NV12 content validation failed: "
                                       << readbackError << L"\n";
                            return false;
                        }
                        bestLumaRange = std::max(bestLumaRange, range);
                        if (frameProbeOnly) {
                            frameSignatures.push_back(signature);
                            framePresentationTimes.push_back(frame->pts);
                            if (frameSignatures.size() <= 48U) {
                                std::wcout << L"frame "
                                           << (frameSignatures.size() - 1U)
                                           << L" pts=" << frame->pts
                                           << L" duration=" << frame->duration
                                           << L" signature=0x" << std::hex
                                           << signature << std::dec
                                           << L" interlaced="
                                           << (frame->interlaced ? 1 : 0)
                                           << L"\n";
                            }
                        }
                    }
                    if (videoTrack->codec != CodecId::Hevc) {
                        // Mirror PlayerEngine's eight-frame queue plus the
                        // currently displayed frame. This catches decoder
                        // surface-pool exhaustion hidden by a smoke test that
                        // releases every output batch immediately.
                        retainedPlaybackFrames.push_back(frame);
                        while (retainedPlaybackFrames.size() > 9U)
                            retainedPlaybackFrames.pop_front();
                    }
                    // The GUI keeps its last displayed frame alive while the
                    // decoder is reset.  Preserve that lifetime here too so a
                    // seek exercises the same D3D11 resource transition.
                    retainedDisplayedFrame = frame;
                    ++videoFrames;
                }
            } else if (audio && audioTrack &&
                       sample.trackId == audioTrack->trackId) {
                // A frame probe verifies the video component in isolation.
                // Damaged audio in the same source must not hide a successful
                // video demux/decode result.
                if (frameProbeOnly) continue;
                AudioFrame frame;
                if (!audio->Decode(sample, frame)) {
                    std::wcerr << L"audio decode failed in " << label << L": "
                               << audio->LastError() << L"\n";
                    return false;
                }
                if (frame.samples.empty()) continue;
                if (frame.channels != 2 ||
                    frame.sampleRate != audioTrack->sampleRate) {
                    std::wcerr << L"audio decoder returned an invalid PCM frame\n";
                    return false;
                }
                for (float value : frame.samples) {
                    if (!std::isfinite(value)) {
                        std::wcerr << L"audio decoder returned a non-finite sample\n";
                        return false;
                    }
                    audioEnergy += static_cast<double>(value) * value;
                    ++audioValues;
                }
                if (audioDump) {
                    audioDump.write(
                        reinterpret_cast<const char*>(frame.samples.data()),
                        static_cast<std::streamsize>(frame.samples.size() *
                                                     sizeof(float)));
                }
                ++audioFrames;
            } else if (subtitleTrack &&
                       sample.trackId == subtitleTrack->trackId) {
                std::wstring subtitleError;
                if (subtitleTrack->codec == CodecId::VobSub) {
                    subtitle::VobSubFrame decoded;
                    if (!subtitle::DecodeVobSubSample(
                            *subtitleTrack, sample, decoded, subtitleError)) {
                        std::wcerr << L"embedded VobSub decode failed: "
                                   << subtitleError << L"\n";
                        return false;
                    }
                    if (!decoded.bitmap.bgra.empty()) ++subtitleSamples;
                } else {
                    std::wstring text;
                    if (!subtitle::DecodeTextSample(*subtitleTrack, sample, text,
                                                    subtitleError)) {
                        std::wcerr << L"embedded subtitle decode failed: "
                                   << subtitleError << L"\n";
                        return false;
                    }
                    if (!text.empty()) {
                        if (firstSubtitle.empty() && text.size() >= 4U) {
                            firstSubtitle = text;
                            firstSubtitleStart = sample.PtsSeconds();
                            firstSubtitleDuration = sample.DurationSeconds();
                        }
                        ++subtitleSamples;
                    }
                }
            }
        }
        std::wcout << label << L": video=" << videoFrames
                   << L" audio=" << audioFrames << L"\n";
        const bool complete =
            videoFrames >= wantedVideo && audioFrames >= wantedAudio;
        if (!complete) {
            std::wcerr << label << L" produced too few decoded frames "
                       << L"(video " << videoFrames << L"/" << wantedVideo
                       << L", audio " << audioFrames << L"/" << wantedAudio
                       << L")\n";
        }
        return complete;
    };

    if (componentProbeOnly || videoComponentProbeOnly) {
        const unsigned startAudio = videoComponentProbeOnly ? 0U : 80U;
        const unsigned middleAudio = videoComponentProbeOnly ? 0U : 64U;
        if (!decodeSpan(12U, startAudio, L"component-start")) return 29;
        const double target = demuxer->DurationSeconds() * 0.5;
        double decodeStart = 0.0;
        retainedPlaybackFrames.clear();
        if (!demuxer->Seek(target, decodeStart) || !video->Reset()) {
            std::wcerr << L"component seek/reset failed: "
                       << demuxer->LastError() << L"\n";
            return 29;
        }
        if (audio) audio->Reset();
        if (!decodeSpan(8U, middleAudio, L"component-middle")) return 29;
        if (videoComponentProbeOnly) {
            std::wcout << L"video-component-probe-ok seek-start="
                       << decodeStart << L"\n";
        } else {
            std::wcout << L"component-probe-ok audio-track="
                       << audioTrack->trackId << L" seek-start="
                       << decodeStart << L"\n";
        }
        return 0;
    }

    if (frameProbeOnly) {
        if (!decodeSpan(frameProbeLimit, 0, L"frame-probe")) return 8;
        std::unordered_set<std::uint64_t> uniqueSignatures;
        unsigned adjacentDuplicates = 0;
        unsigned twoBackDuplicates = 0;
        unsigned duplicatePts = 0;
        unsigned backwardsPts = 0;
        for (std::size_t i = 0; i < frameSignatures.size(); ++i) {
            uniqueSignatures.insert(frameSignatures[i]);
            if (i > 0) {
                if (frameSignatures[i] == frameSignatures[i - 1])
                    ++adjacentDuplicates;
                if (framePresentationTimes[i] == framePresentationTimes[i - 1])
                    ++duplicatePts;
                if (framePresentationTimes[i] < framePresentationTimes[i - 1])
                    ++backwardsPts;
            }
            if (i > 1 && frameSignatures[i] == frameSignatures[i - 2] &&
                frameSignatures[i] != frameSignatures[i - 1]) {
                ++twoBackDuplicates;
            }
        }
        std::wcout << L"frame-probe-summary frames=" << frameSignatures.size()
                   << L" unique=" << uniqueSignatures.size()
                   << L" adjacent-duplicates=" << adjacentDuplicates
                   << L" two-back-duplicates=" << twoBackDuplicates
                   << L" duplicate-pts=" << duplicatePts
                   << L" backwards-pts=" << backwardsPts << L"\n";
        return videoTrack->codec == CodecId::H264 &&
                       (duplicatePts != 0 || backwardsPts != 0)
                   ? 23
                   : 0;
    }

    const wchar_t* startLabel =
        videoTrack->codec == CodecId::Mpeg4Part2
            ? L"start-MPEG4-Part2"
            : (videoTrack->codec == CodecId::Mpeg2Video
                   ? L"start-MPEG2"
                   : (videoTrack->codec == CodecId::Wmv3
                          ? L"start-WMV3"
                          : (videoTrack->codec == CodecId::Msmpeg4v3
                                 ? L"start-Microsoft-MPEG4-v3"
                                 : (hevcVideo ? L"start-HEVC"
                                              : L"start-H264"))));
    if (!decodeSpan(hevcVideo ? 1440U : 600U,
                    hevcVideo ? 2820U : 500U, startLabel))
        return 8;
    if (nv12Video && bestLumaRange < 8) {
        std::wcerr << L"decoded NV12 frames contain no measurable image contrast\n";
        return 12;
    }
    double decodeStart = 0.0;
    const bool matroska = videoTrack->sampleEntry.rfind("V_", 0) == 0;
    const double seekTarget = matroska
                                  ? demuxer->DurationSeconds() * 0.5
                                  : std::min(3600.0,
                                             demuxer->DurationSeconds() * 0.5);
    retainedPlaybackFrames.clear();
    if (!demuxer->Seek(seekTarget, decodeStart) || !video->Reset()) {
        std::wcerr << L"seek/reset failed: " << demuxer->LastError() << L"\n";
        return 9;
    }
    audio->Reset();
    if (!decodeSpan(hevcVideo ? 48U : 120U, 80, L"seek")) return 10;

    if (matroska && videoTrack->codec == CodecId::Hevc) {
        // Cover several Matroska cues, including the quarter point that used
        // to expose an open-GOP RASL reference after a reset and the midpoint
        // that previously exposed DecoderBeginFrame E_PENDING in the GUI.
        constexpr double fractions[] = {0.25, 0.50, 0.75, 0.50};
        for (double fraction : fractions) {
            const double target = demuxer->DurationSeconds() * fraction;
            retainedPlaybackFrames.clear();
            if (!demuxer->Seek(target, decodeStart) || !video->Reset()) {
                std::wcerr << L"repeated seek/reset failed at " << target
                           << L": " << demuxer->LastError() << L"\n";
                return 13;
            }
            audio->Reset();
            if (!decodeSpan(24U, 40U, L"repeated-Matroska-seek")) return 14;
        }
    }

    if (!matroska && videoTrack->codec == CodecId::Hevc) {
        // Exercise open-GOP HEVC random access repeatedly.  MP4 sync samples
        // produced by x265 are commonly CRA pictures followed (in decode
        // order) by RASL leading pictures, which is a different path from a
        // one-off seek to an IDR picture.
        // These positions are the deterministic slider sequence used by the
        // UI regression test (range 0..10000).  In particular, 1658 used to
        // expose a missing-reference error in Memories of Matsuko.
        constexpr double fractions[] = {
            0.4005, 0.6525, 0.5541, 0.6202, 0.1560,
            0.5406, 0.8340, 0.3286, 0.2997, 0.4242,
            0.3956, 0.0574, 0.5065, 0.1915, 0.1658,
            0.8145, 0.3977, 0.4714, 0.7101, 0.4922,
        };
        for (double fraction : fractions) {
            const double target = demuxer->DurationSeconds() * fraction;
            std::wcout << L"random-seek target=" << target
                       << L" fraction=" << fraction << L"\n";
            retainedPlaybackFrames.clear();
            if (!demuxer->Seek(target, decodeStart) || !video->Reset()) {
                std::wcerr << L"random seek/reset failed at " << target
                           << L": " << demuxer->LastError() << L"\n";
                return 16;
            }
            audio->Reset();
            if (!decodeSpan(48U, 80U, L"random-seek")) return 17;
        }
    }

    const double rms = audioValues != 0
                           ? std::sqrt(audioEnergy / static_cast<double>(audioValues))
                           : 0.0;
    std::wcout << L"ok: duration=" << demuxer->DurationSeconds()
                << L" seek-start=" << decodeStart << L" audio-rms=" << rms
                << L" subtitles=" << subtitleSamples;
    if (!firstSubtitle.empty()) {
        std::wcout << L" first-subtitle-at=" << firstSubtitleStart
                   << L" duration=" << firstSubtitleDuration
                   << L" length=" << firstSubtitle.size();
        const bool printableAscii = std::all_of(
            firstSubtitle.begin(), firstSubtitle.end(), [](wchar_t value) {
                return value >= 0x20 && value < 0x7f;
            });
        if (printableAscii) std::wcout << L" text='" << firstSubtitle << L"'";
        else std::wcout << L" text=<non-ASCII>";
    }
    std::wcout << L"\n";
    if (subtitleTrack && subtitleSamples == 0) return 15;
    return rms > 1.0e-5 && rms < 1.0 ? 0 : 11;
}
