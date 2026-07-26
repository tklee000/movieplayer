#include "codec/audio/aac/AacLcDecoder.h"
#include "codec/audio/mp3/MfMp3Decoder.h"
#include "codec/audio/opus/OpusDecoder.h"
#include "codec/container/MediaDemuxer.h"
#include "codec/subtitle/TextSubtitleDecoder.h"
#include "codec/subtitle/VobSubDecoder.h"
#include "codec/video/h264/MfH264Decoder.h"
#include "codec/video/hevc/D3D11HevcDecoder.h"

#include <d3d10.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
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

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 && argc != 3) {
        std::wcerr << L"usage: MovieCodecSmoke <input> [option]\n"
                      L"       MovieCodecSmoke [option] <input>\n"
                      L"options: --probe | --audio-track=N | "
                      L"--subtitle-probe[=SECONDS] | "
                      L"--timeline-probe[=SAMPLES] | "
                      L"--frame-probe[=FRAMES] | "
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
    bool probeOnly = false;
    bool timelineProbeOnly = false;
    unsigned timelineProbeLimit = 120;
    bool frameProbeOnly = false;
    unsigned frameProbeLimit = 300;
    bool subtitleProbeOnly = false;
    double subtitleProbeTime = 0.0;
    std::filesystem::path audioDumpPath;
    std::ofstream audioDump;
    if (!option.empty()) {
        constexpr wchar_t audioTrackPrefix[] = L"--audio-track=";
        constexpr wchar_t audioDumpPrefix[] = L"--dump-audio=";
        constexpr wchar_t timelineProbePrefix[] = L"--timeline-probe=";
        constexpr wchar_t frameProbePrefix[] = L"--frame-probe=";
        if (option == L"--probe") {
            probeOnly = true;
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
                       << L" fps=" << track.frameRate.ToDouble();
        } else if (track.type == TrackType::Audio) {
            std::wcout << L" rate=" << track.sampleRate
                       << L" channels=" << track.channels;
        } else if (track.type == TrackType::Subtitle) {
            std::wcout << L" language="
                       << std::wstring(track.language.begin(), track.language.end())
                       << L" default=" << (track.defaultTrack ? 1 : 0);
        }
        std::wcout << L"\n";
        if (!videoTrack &&
            (track.codec == CodecId::H264 || track.codec == CodecId::Hevc ||
             track.codec == CodecId::Mpeg4Part2))
            videoTrack = &track;
        if ((track.codec == CodecId::Aac || track.codec == CodecId::Mp3 ||
             track.codec == CodecId::Opus) &&
            ((!audioTrack && requestedAudioTrack == 0) ||
             track.trackId == requestedAudioTrack)) {
            audioTrack = &track;
        }
        if (track.type == TrackType::Subtitle &&
            (track.codec == CodecId::Ass || track.codec == CodecId::SubRip ||
             track.codec == CodecId::VobSub) &&
            (!subtitleTrack || track.defaultTrack)) {
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
    if (!videoTrack || !audioTrack) {
        std::wcerr << L"expected a supported video and audio track\n";
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
                       << text.size() << L"\n";
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
        videoTrack->codec == CodecId::Mpeg4Part2)
        video = std::make_unique<h264::MfH264Decoder>();
    else
        video = std::make_unique<hevc::D3D11HevcDecoder>();
    if (!video->Initialize(device.Get(), *videoTrack)) {
        std::wcerr << L"video init failed: " << video->LastError() << L"\n";
        return 6;
    }
    std::wcout << L"decoder: " << video->Description() << L"\n";
    std::unique_ptr<IAudioDecoder> audio;
    if (audioTrack->codec == CodecId::Mp3)
        audio = std::make_unique<mp3::MfMp3Decoder>();
    else if (audioTrack->codec == CodecId::Opus)
        audio = std::make_unique<opus::OpusDecoder>();
    else
        audio = std::make_unique<aac::AacLcDecoder>();
    if (!audio->Initialize(*audioTrack)) {
        std::wcerr << L"audio init failed: " << audio->LastError() << L"\n";
        return 7;
    }
    std::wcout << L"audio decoder: " << audio->Description() << L"\n";
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
            } else if (sample.trackId == audioTrack->trackId) {
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
        return videoFrames >= wantedVideo && audioFrames >= wantedAudio;
    };

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

    if (!decodeSpan(hevcVideo ? 1440U : 600U,
                    hevcVideo ? 2820U : 500U,
                    videoTrack->codec == CodecId::Mpeg4Part2
                        ? L"start-MPEG4-Part2"
                        : (hevcVideo ? L"start-HEVC" : L"start-H264")))
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
