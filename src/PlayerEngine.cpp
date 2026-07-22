#include "PlayerEngine.h"

#include "AudioOutput.h"
#include "Localization.h"
#include "codec/audio/aac/AacLcDecoder.h"
#include "codec/audio/mp3/MfMp3Decoder.h"
#include "codec/container/MediaDemuxer.h"
#include "codec/subtitle/TextSubtitleDecoder.h"
#include "codec/video/h264/MfH264Decoder.h"
#include "codec/video/hevc/D3D11HevcDecoder.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

using movieplayer::codec::AudioFrame;
using movieplayer::codec::CodecId;
using movieplayer::codec::EncodedSample;
using movieplayer::codec::TrackInfo;
using movieplayer::codec::TrackType;

namespace {

class PacketQueue {
public:
    bool Push(EncodedSample sample) {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] {
            return aborted_ || packets_.size() < kMaximumPackets;
        });
        if (aborted_) return false;
        packets_.push_back(std::move(sample));
        condition_.notify_all();
        return true;
    }

    bool PushEof() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (aborted_) return false;
        eof_ = true;
        condition_.notify_all();
        return true;
    }

    bool Pop(EncodedSample& sample, bool& eof) {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] {
            return aborted_ || !packets_.empty() || eof_;
        });
        if (aborted_) return false;
        if (packets_.empty()) {
            eof = eof_;
            return true;
        }
        sample = std::move(packets_.front());
        packets_.pop_front();
        eof = false;
        condition_.notify_all();
        return true;
    }

    void Abort() {
        std::lock_guard<std::mutex> lock(mutex_);
        aborted_ = true;
        condition_.notify_all();
    }

    void Reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        packets_.clear();
        aborted_ = false;
        eof_ = false;
        condition_.notify_all();
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        packets_.clear();
        eof_ = false;
        condition_.notify_all();
    }

private:
    static constexpr std::size_t kMaximumPackets = 96;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<EncodedSample> packets_;
    bool aborted_ = false;
    bool eof_ = false;
};

class VideoFrameQueue {
public:
    bool Push(std::shared_ptr<DecodedVideoFrame> frame) {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] {
            return aborted_ || frames_.size() < kMaximumFrames;
        });
        if (aborted_) return false;
        const auto position = std::upper_bound(
            frames_.begin(), frames_.end(), frame->pts,
            [](double pts, const std::shared_ptr<DecodedVideoFrame>& candidate) {
                return pts < candidate->pts;
            });
        frames_.insert(position, std::move(frame));
        condition_.notify_all();
        return true;
    }

    std::shared_ptr<DecodedVideoFrame> Acquire(double clock, bool paused,
                                                bool force) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (frames_.empty()) return nullptr;
        if (!force && !paused && frames_.front()->pts > clock + 0.010) return nullptr;
        std::shared_ptr<DecodedVideoFrame> result = frames_.front();
        frames_.pop_front();
        if (!paused) {
            while (!frames_.empty() && frames_.front()->pts <= clock + 0.010) {
                result = frames_.front();
                frames_.pop_front();
            }
        }
        condition_.notify_all();
        return result;
    }

    void Abort() {
        std::lock_guard<std::mutex> lock(mutex_);
        aborted_ = true;
        condition_.notify_all();
    }

    void Reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        frames_.clear();
        aborted_ = false;
        condition_.notify_all();
    }

    bool Empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return frames_.empty();
    }

private:
    static constexpr std::size_t kMaximumFrames = 8;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::shared_ptr<DecodedVideoFrame>> frames_;
    bool aborted_ = false;
};

std::vector<std::uint8_t> FloatToPcm16(const AudioFrame& frame) {
    std::vector<std::uint8_t> result(frame.samples.size() * sizeof(std::int16_t));
    auto* destination = reinterpret_cast<std::int16_t*>(result.data());
    for (std::size_t i = 0; i < frame.samples.size(); ++i) {
        const float limited = std::max(-1.0F, std::min(1.0F, frame.samples[i]));
        const long sample = std::lround(limited * (limited < 0.0F ? 32768.0F : 32767.0F));
        destination[i] = static_cast<std::int16_t>(
            std::max<long>(-32768, std::min<long>(32767, sample)));
    }
    return result;
}

}  // namespace

struct PlayerEngine::Impl {
    std::unique_ptr<movieplayer::codec::IMediaDemuxer> demuxer;
    std::unique_ptr<movieplayer::codec::IVideoDecoder> videoDecoder;
    std::unique_ptr<movieplayer::codec::IAudioDecoder> audioDecoder;
    TrackInfo videoTrack;
    TrackInfo audioTrack;
    TrackInfo subtitleTrack;
    std::vector<TrackInfo> audioTracks;
    std::vector<TrackInfo> subtitleTracks;
    bool hasAudio = false;
    bool hasEmbeddedSubtitle = false;
    AudioOutput audioOutput;

    struct EmbeddedCue {
        double start = 0.0;
        double end = 0.0;
        std::wstring text;
    };
    std::deque<EmbeddedCue> embeddedCues;
    mutable std::mutex subtitleMutex;

    PacketQueue videoPackets;
    PacketQueue audioPackets;
    VideoFrameQueue videoFrames;
    std::thread demuxThread;
    std::thread videoThread;
    std::thread audioThread;

    std::atomic<bool> abort{false};
    std::atomic<bool> opened{false};
    std::atomic<bool> paused{false};
    std::atomic<bool> demuxFinished{false};
    std::atomic<bool> videoFinished{false};
    std::atomic<bool> audioFinished{true};
    std::atomic<bool> forceNextFrame{false};
    std::atomic<double> seekFloor{0.0};
    std::atomic<double> displayedPts{0.0};
    std::atomic<double> pausedPosition{0.0};
    std::atomic<double> externalClockBaseSeconds{0.0};
    std::chrono::steady_clock::time_point wallClockBase =
        std::chrono::steady_clock::now();

    double duration = 0.0;
    int width = 0;
    int height = 0;
    float volume = 1.0F;
    bool muted = false;
    float speed = 1.0F;
    std::wstring mediaDescription;
    std::wstring decoderDescription;
    std::wstring lastError;
    std::wstring asyncError;
    mutable std::mutex errorMutex;
    std::mutex controlMutex;

    void SetError(const std::wstring& value) {
        std::lock_guard<std::mutex> lock(errorMutex);
        lastError = value;
    }

    void SetAsyncError(const std::wstring& value) {
        std::lock_guard<std::mutex> lock(errorMutex);
        if (asyncError.empty()) asyncError = value;
    }

    double ExternalClock() const {
        if (paused.load()) return pausedPosition.load();
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - wallClockBase).count();
        return externalClockBaseSeconds.load() + elapsed * speed;
    }

    void ResetExternalClock(double position) {
        externalClockBaseSeconds.store(position);
        wallClockBase = std::chrono::steady_clock::now();
        pausedPosition.store(position);
    }

    double CurrentPosition() const {
        if (!opened.load()) return 0.0;
        double result = paused.load() ? pausedPosition.load()
                                      : (hasAudio && audioOutput.HasClock()
                                             ? audioOutput.ClockSeconds()
                                             : ExternalClock());
        if (!std::isfinite(result)) result = 0.0;
        return std::max(0.0, std::min(duration, result));
    }

    void BuildDescription() {
        std::wostringstream out;
        const wchar_t* videoName = videoTrack.codec == CodecId::H264
                                       ? L"H.264"
                                       : (videoTrack.codec == CodecId::Hevc
                                              ? L"HEVC Main 10"
                                              : L"MPEG-4 Part 2");
        out << videoName
            << L"  " << width << L"×" << height;
        if (videoTrack.frameRate.IsValid()) {
            out.setf(std::ios::fixed);
            out.precision(3);
            out << L"  " << videoTrack.frameRate.ToDouble() << L" fps";
        }
        if (hasAudio) {
            out << L"  ·  "
                << (audioTrack.codec == CodecId::Mp3 ? L"MP3 " : L"AAC-LC ")
                << audioTrack.sampleRate << L" Hz "
                << audioTrack.channels << L" ch";
        }
        mediaDescription = out.str();
        decoderDescription = videoDecoder ? videoDecoder->Description() : L"";
    }

    std::unique_ptr<movieplayer::codec::IAudioDecoder> CreateAudioDecoder(
        const TrackInfo& track, std::wstring& failure) {
        std::unique_ptr<movieplayer::codec::IAudioDecoder> decoder;
        if (track.codec == CodecId::Mp3) {
            decoder = std::make_unique<movieplayer::codec::mp3::MfMp3Decoder>();
        } else if (track.codec == CodecId::Aac) {
            decoder = std::make_unique<movieplayer::codec::aac::AacLcDecoder>();
        } else {
            failure = L"The selected audio codec is not supported";
            return nullptr;
        }
        if (!decoder->Initialize(track)) {
            failure = decoder->LastError();
            return nullptr;
        }
        failure.clear();
        return decoder;
    }

    bool SetDemuxTrackSelection(TrackType type, std::uint32_t selectedId) {
        for (const TrackInfo& track : demuxer->Tracks()) {
            if (track.type != type) continue;
            if (!demuxer->SetTrackEnabled(track.trackId,
                                          track.trackId == selectedId)) {
                return false;
            }
        }
        return true;
    }

    bool Open(const std::wstring& path, ID3D11Device* d3dDevice) {
        std::lock_guard<std::mutex> lock(controlMutex);
        CloseUnlocked();
        {
            std::lock_guard<std::mutex> errorLock(errorMutex);
            lastError.clear();
            asyncError.clear();
        }
        if (!d3dDevice) {
            SetError(L"The video renderer did not provide a D3D11 device");
            return false;
        }
        demuxer = movieplayer::codec::CreateMediaDemuxer(path);
        if (!demuxer || !demuxer->Open(path)) {
            SetError(Localization::Format(
                "error.media_open",
                {{L"error", demuxer ? demuxer->LastError()
                                     : L"No media demuxer is available"}}));
            return false;
        }
        bool foundVideo = false;
        hasAudio = false;
        hasEmbeddedSubtitle = false;
        audioTracks.clear();
        subtitleTracks.clear();
        bool selectedDefaultAudio = false;
        bool selectedDefaultSubtitle = false;
        for (const TrackInfo& track : demuxer->Tracks()) {
            if (!foundVideo && track.type == TrackType::Video &&
                (track.codec == CodecId::H264 || track.codec == CodecId::Hevc ||
                 track.codec == CodecId::Mpeg4Part2)) {
                videoTrack = track;
                foundVideo = true;
            } else if (track.type == TrackType::Subtitle &&
                       (track.codec == CodecId::SubRip ||
                        track.codec == CodecId::Ass)) {
                subtitleTracks.push_back(track);
                if (!hasEmbeddedSubtitle ||
                    (track.defaultTrack && !selectedDefaultSubtitle)) {
                    subtitleTrack = track;
                    hasEmbeddedSubtitle = true;
                    if (track.defaultTrack) selectedDefaultSubtitle = true;
                }
            } else if (track.type == TrackType::Audio &&
                       (track.codec == CodecId::Aac ||
                        track.codec == CodecId::Mp3)) {
                audioTracks.push_back(track);
                if (!hasAudio || (track.defaultTrack && !selectedDefaultAudio)) {
                    audioTrack = track;
                    hasAudio = true;
                    if (track.defaultTrack) selectedDefaultAudio = true;
                }
            }
        }
        if (!SetDemuxTrackSelection(
                TrackType::Audio, hasAudio ? audioTrack.trackId : 0) ||
            !SetDemuxTrackSelection(
                TrackType::Subtitle,
                hasEmbeddedSubtitle ? subtitleTrack.trackId : 0)) {
            SetError(demuxer->LastError());
            CloseUnlocked();
            return false;
        }
        if (!foundVideo) {
            SetError(Localization::Text("error.no_video_stream"));
            CloseUnlocked();
            return false;
        }
        if (videoTrack.codec == CodecId::H264 ||
            videoTrack.codec == CodecId::Mpeg4Part2) {
            videoDecoder =
                std::make_unique<movieplayer::codec::h264::MfH264Decoder>();
        } else {
            videoDecoder =
                std::make_unique<movieplayer::codec::hevc::D3D11HevcDecoder>();
        }
        if (!videoDecoder->Initialize(d3dDevice, videoTrack)) {
            SetError(Localization::Format(
                "error.video_decoder_open", {{L"error", videoDecoder->LastError()}}));
            CloseUnlocked();
            return false;
        }
        if (hasAudio) {
            std::wstring decoderError;
            audioDecoder = CreateAudioDecoder(audioTrack, decoderError);
            if (!audioDecoder) {
                SetError(Localization::Format(
                    "error.audio_decoder_open", {{L"error", decoderError}}));
                CloseUnlocked();
                return false;
            }
            if (!audioOutput.Initialize(audioTrack.sampleRate, 2)) {
                SetError(Localization::Format(
                    "error.audio_output_init", {{L"error", audioOutput.LastError()}}));
                CloseUnlocked();
                return false;
            }
            audioOutput.SetVolume(volume);
            audioOutput.SetMuted(muted);
            audioOutput.SetSpeed(speed);
        }
        duration = demuxer->DurationSeconds();
        width = videoTrack.width;
        height = videoTrack.height;
        BuildDescription();
        paused.store(false);
        opened.store(true);
        if (!StartWorkers(0.0)) {
            CloseUnlocked();
            return false;
        }
        return true;
    }

    void CloseUnlocked() {
        StopWorkers();
        opened.store(false);
        audioOutput.Shutdown();
        audioDecoder.reset();
        videoDecoder.reset();
        if (demuxer) {
            demuxer->Close();
            demuxer.reset();
        }
        hasAudio = false;
        hasEmbeddedSubtitle = false;
        audioTracks.clear();
        subtitleTracks.clear();
        {
            std::lock_guard<std::mutex> subtitleLock(subtitleMutex);
            embeddedCues.clear();
        }
        duration = 0.0;
        width = height = 0;
        mediaDescription.clear();
        decoderDescription.clear();
    }

    void Close() {
        std::lock_guard<std::mutex> lock(controlMutex);
        CloseUnlocked();
    }

    bool StartWorkers(double target) {
        abort.store(false);
        demuxFinished.store(false);
        videoFinished.store(false);
        audioFinished.store(!hasAudio);
        seekFloor.store(target);
        displayedPts.store(target);
        forceNextFrame.store(true);
        videoPackets.Reset();
        audioPackets.Reset();
        videoFrames.Reset();
        {
            std::lock_guard<std::mutex> subtitleLock(subtitleMutex);
            embeddedCues.clear();
        }
        ResetExternalClock(target);
        if (hasAudio) {
            audioOutput.SetAbort(false);
            if (!audioOutput.Reset()) {
                SetError(Localization::Format(
                    "error.audio_output_reset", {{L"error", audioOutput.LastError()}}));
                return false;
            }
            audioOutput.SetVolume(volume);
            audioOutput.SetMuted(muted);
            audioOutput.SetSpeed(speed);
            audioOutput.SetPaused(paused.load());
        }
        try {
            demuxThread = std::thread([this] { DemuxLoop(); });
            videoThread = std::thread([this] { VideoDecodeLoop(); });
            if (hasAudio) audioThread = std::thread([this] { AudioDecodeLoop(); });
        } catch (const std::exception&) {
            SetError(Localization::Text("error.playback_thread"));
            StopWorkers();
            return false;
        }
        return true;
    }

    void StopWorkers() {
        abort.store(true);
        videoPackets.Abort();
        audioPackets.Abort();
        videoFrames.Abort();
        audioOutput.SetAbort(true);
        if (demuxThread.joinable()) demuxThread.join();
        if (videoThread.joinable()) videoThread.join();
        if (audioThread.joinable()) audioThread.join();
        videoPackets.Clear();
        audioPackets.Clear();
    }

    void DemuxLoop() {
        while (!abort.load()) {
            EncodedSample sample;
            bool eof = false;
            if (!demuxer->ReadNextSample(sample, eof)) {
                if (!abort.load()) {
                    SetAsyncError(Localization::Format(
                        "error.media_read", {{L"error", demuxer->LastError()}}));
                }
                break;
            }
            if (eof) break;
            bool accepted = true;
            if (sample.trackId == videoTrack.trackId)
                accepted = videoPackets.Push(std::move(sample));
            else if (hasAudio && sample.trackId == audioTrack.trackId)
                accepted = audioPackets.Push(std::move(sample));
            else if (hasEmbeddedSubtitle &&
                     sample.trackId == subtitleTrack.trackId) {
                std::wstring text;
                std::wstring subtitleError;
                if (movieplayer::codec::subtitle::DecodeTextSample(
                        subtitleTrack, sample, text, subtitleError) &&
                    !text.empty()) {
                    EmbeddedCue cue;
                    cue.start = sample.PtsSeconds();
                    const double subtitleDuration = sample.DurationSeconds();
                    cue.end = cue.start +
                              (subtitleDuration > 0.0 ? subtitleDuration : 5.0);
                    cue.text = std::move(text);
                    std::lock_guard<std::mutex> subtitleLock(subtitleMutex);
                    embeddedCues.push_back(std::move(cue));
                    while (embeddedCues.size() > 256U) embeddedCues.pop_front();
                }
            }
            if (!accepted) break;
        }
        if (!abort.load()) {
            videoPackets.PushEof();
            if (hasAudio) audioPackets.PushEof();
            demuxFinished.store(true);
        }
    }

    void VideoDecodeLoop() {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        EncodedSample sample;
        bool eof = false;
        while (!abort.load() && videoPackets.Pop(sample, eof)) {
            std::vector<std::shared_ptr<DecodedVideoFrame>> decoded;
            if (eof) {
                if (!videoDecoder->Flush(decoded) && !abort.load()) {
                    SetAsyncError(Localization::Format(
                        "error.video_decode", {{L"error", videoDecoder->LastError()}}));
                }
            } else if (!videoDecoder->Decode(sample, decoded)) {
                if (!abort.load()) {
                    SetAsyncError(Localization::Format(
                        "error.video_decode", {{L"error", videoDecoder->LastError()}}));
                }
                break;
            }
            for (auto& frame : decoded) {
                if (frame->pts + frame->duration < seekFloor.load() - 0.010) continue;
                if (!videoFrames.Push(std::move(frame))) break;
            }
            if (eof) break;
        }
        if (!abort.load()) videoFinished.store(true);
        if (SUCCEEDED(comResult)) CoUninitialize();
    }

    void AudioDecodeLoop() {
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        EncodedSample sample;
        bool eof = false;
        while (!abort.load() && audioPackets.Pop(sample, eof)) {
            if (eof) break;
            AudioFrame frame;
            if (!audioDecoder->Decode(sample, frame)) {
                if (!abort.load()) {
                    SetAsyncError(Localization::Format(
                        "error.audio_decode", {{L"error", audioDecoder->LastError()}}));
                }
                break;
            }
            if (frame.samples.empty()) continue;
            const double frameDuration = frame.channels > 0 && frame.sampleRate > 0
                                             ? static_cast<double>(frame.samples.size() /
                                                                   frame.channels) /
                                                   frame.sampleRate
                                             : 0.0;
            if (frame.pts + frameDuration < seekFloor.load() - 0.010) continue;
            const auto pcm = FloatToPcm16(frame);
            if (!audioOutput.Submit(pcm.data(), pcm.size(), frame.pts)) {
                if (!abort.load()) {
                    SetAsyncError(Localization::Format(
                        "error.audio_output_submit",
                        {{L"error", audioOutput.LastError()}}));
                }
                break;
            }
        }
        if (!abort.load()) audioFinished.store(true);
        if (SUCCEEDED(comResult)) CoUninitialize();
    }

    bool Seek(double target) {
        std::lock_guard<std::mutex> lock(controlMutex);
        if (!opened.load()) return false;
        target = std::max(0.0, std::min(duration, target));
        const bool wasPaused = paused.load();
        const double previous = CurrentPosition();
        StopWorkers();
        double decodeStart = 0.0;
        if (!demuxer->Seek(target, decodeStart)) {
            SetError(Localization::Format(
                "error.seek_engine", {{L"error", demuxer->LastError()}}));
            abort.store(false);
            paused.store(wasPaused);
            StartWorkers(previous);
            return false;
        }
        videoDecoder->Reset();
        if (audioDecoder) audioDecoder->Reset();
        paused.store(wasPaused);
        pausedPosition.store(target);
        return StartWorkers(target);
    }

    bool RestartAt(double target, bool wasPaused) {
        double decodeStart = 0.0;
        if (!demuxer->Seek(target, decodeStart)) {
            return false;
        }
        videoDecoder->Reset();
        if (audioDecoder) audioDecoder->Reset();
        paused.store(wasPaused);
        pausedPosition.store(target);
        return StartWorkers(target);
    }

    bool SelectAudioTrack(std::uint32_t trackId) {
        if (!opened.load()) return false;
        const auto found = std::find_if(
            audioTracks.begin(), audioTracks.end(),
            [trackId](const TrackInfo& track) { return track.trackId == trackId; });
        if (found == audioTracks.end()) {
            SetError(L"The requested audio track is not supported");
            return false;
        }
        if (audioTrack.trackId == trackId) return true;

        std::wstring decoderError;
        auto replacementDecoder = CreateAudioDecoder(*found, decoderError);
        if (!replacementDecoder) {
            SetError(Localization::Format(
                "error.audio_decoder_open", {{L"error", decoderError}}));
            return false;
        }

        const TrackInfo previousTrack = audioTrack;
        const double position = CurrentPosition();
        const bool wasPaused = paused.load();
        StopWorkers();

        if (!SetDemuxTrackSelection(TrackType::Audio, trackId)) {
            const std::wstring switchError = demuxer->LastError();
            SetDemuxTrackSelection(TrackType::Audio, previousTrack.trackId);
            RestartAt(position, wasPaused);
            SetError(switchError);
            return false;
        }

        double decodeStart = 0.0;
        if (!demuxer->Seek(position, decodeStart)) {
            const std::wstring switchError = demuxer->LastError();
            SetDemuxTrackSelection(TrackType::Audio, previousTrack.trackId);
            RestartAt(position, wasPaused);
            SetError(switchError);
            return false;
        }

        audioOutput.Shutdown();
        if (!audioOutput.Initialize(found->sampleRate, 2)) {
            const std::wstring outputError = Localization::Format(
                "error.audio_output_init", {{L"error", audioOutput.LastError()}});
            SetDemuxTrackSelection(TrackType::Audio, previousTrack.trackId);
            demuxer->Seek(position, decodeStart);
            if (audioOutput.Initialize(previousTrack.sampleRate, 2)) {
                audioOutput.SetVolume(volume);
                audioOutput.SetMuted(muted);
                audioOutput.SetSpeed(speed);
                RestartAt(position, wasPaused);
            }
            SetError(outputError);
            return false;
        }

        audioTrack = *found;
        audioDecoder = std::move(replacementDecoder);
        audioOutput.SetVolume(volume);
        audioOutput.SetMuted(muted);
        audioOutput.SetSpeed(speed);
        videoDecoder->Reset();
        paused.store(wasPaused);
        pausedPosition.store(position);
        BuildDescription();
        if (!StartWorkers(position)) {
            return false;
        }
        return true;
    }

    bool SelectEmbeddedSubtitleTrack(std::uint32_t trackId) {
        if (!opened.load()) return false;
        const auto found = std::find_if(
            subtitleTracks.begin(), subtitleTracks.end(),
            [trackId](const TrackInfo& track) { return track.trackId == trackId; });
        if (found == subtitleTracks.end()) {
            SetError(L"The requested subtitle track is not supported");
            return false;
        }
        if (subtitleTrack.trackId == trackId) return true;

        const TrackInfo previousTrack = subtitleTrack;
        const double position = CurrentPosition();
        const bool wasPaused = paused.load();
        StopWorkers();

        if (!SetDemuxTrackSelection(TrackType::Subtitle, trackId)) {
            const std::wstring switchError = demuxer->LastError();
            SetDemuxTrackSelection(TrackType::Subtitle, previousTrack.trackId);
            RestartAt(position, wasPaused);
            SetError(switchError);
            return false;
        }

        subtitleTrack = *found;
        if (!RestartAt(position, wasPaused)) {
            const std::wstring switchError = demuxer->LastError();
            subtitleTrack = previousTrack;
            SetDemuxTrackSelection(TrackType::Subtitle, previousTrack.trackId);
            RestartAt(position, wasPaused);
            SetError(switchError);
            return false;
        }
        return true;
    }

    void SetPaused(bool value) {
        if (!opened.load() || paused.load() == value) return;
        if (value) {
            const double position = CurrentPosition();
            pausedPosition.store(position);
            paused.store(true);
            audioOutput.SetPaused(true);
        } else {
            const double position = pausedPosition.load();
            ResetExternalClock(position);
            paused.store(false);
            audioOutput.SetPaused(false);
        }
    }

    std::wstring EmbeddedSubtitleText() const {
        if (!hasEmbeddedSubtitle || !opened.load()) return {};
        const double position = CurrentPosition();
        std::lock_guard<std::mutex> lock(subtitleMutex);
        std::wstring result;
        for (const EmbeddedCue& cue : embeddedCues) {
            if (cue.start <= position && position < cue.end) {
                if (!result.empty()) result += L'\n';
                result += cue.text;
            }
        }
        return result;
    }

    std::wstring EmbeddedSubtitleDescription() const {
        if (!hasEmbeddedSubtitle) return {};
        std::wstring result = subtitleTrack.codec == CodecId::Ass
                                  ? L"Embedded ASS"
                                  : L"Embedded UTF-8";
        if (!subtitleTrack.language.empty()) {
            result += L" (";
            result.append(subtitleTrack.language.begin(),
                          subtitleTrack.language.end());
            result += L")";
        }
        return result;
    }
};

PlayerEngine::PlayerEngine() : impl_(std::make_unique<Impl>()) {}
PlayerEngine::~PlayerEngine() { impl_->Close(); }

bool PlayerEngine::Open(const std::wstring& path, ID3D11Device* device) {
    return impl_->Open(path, device);
}
void PlayerEngine::Close() { impl_->Close(); }
void PlayerEngine::Play() { impl_->SetPaused(false); }
void PlayerEngine::Pause() { impl_->SetPaused(true); }
void PlayerEngine::TogglePause() { impl_->SetPaused(!impl_->paused.load()); }
void PlayerEngine::Stop() {
    if (!impl_->opened.load()) return;
    impl_->SetPaused(true);
    impl_->Seek(0.0);
}
bool PlayerEngine::Seek(double seconds) { return impl_->Seek(seconds); }
bool PlayerEngine::SeekRelative(double delta) {
    return impl_->Seek(impl_->CurrentPosition() + delta);
}
void PlayerEngine::StepFrame() {
    if (!impl_->opened.load()) return;
    impl_->SetPaused(true);
    impl_->forceNextFrame.store(true);
}
void PlayerEngine::SetVolume(float value) {
    impl_->volume = std::max(0.0F, std::min(1.0F, value));
    impl_->audioOutput.SetVolume(impl_->volume);
}
float PlayerEngine::Volume() const { return impl_->volume; }
void PlayerEngine::SetMuted(bool value) {
    impl_->muted = value;
    impl_->audioOutput.SetMuted(value);
}
bool PlayerEngine::IsMuted() const { return impl_->muted; }
void PlayerEngine::SetSpeed(float value) {
    value = std::max(0.25F, std::min(4.0F, value));
    const double position = impl_->CurrentPosition();
    impl_->speed = value;
    impl_->audioOutput.SetSpeed(value);
    impl_->ResetExternalClock(position);
}
float PlayerEngine::Speed() const { return impl_->speed; }
bool PlayerEngine::IsOpen() const { return impl_->opened.load(); }
bool PlayerEngine::IsPaused() const { return impl_->paused.load(); }
bool PlayerEngine::IsEnded() const {
    return impl_->opened.load() && impl_->demuxFinished.load() &&
           impl_->videoFinished.load() && impl_->audioFinished.load() &&
           impl_->videoFrames.Empty() &&
           (!impl_->hasAudio || impl_->audioOutput.QueuedBuffers() == 0);
}
bool PlayerEngine::HasAudio() const { return impl_->hasAudio; }
std::vector<TrackInfo> PlayerEngine::AudioTracks() const {
    std::lock_guard<std::mutex> lock(impl_->controlMutex);
    return impl_->audioTracks;
}
std::uint32_t PlayerEngine::SelectedAudioTrackId() const {
    std::lock_guard<std::mutex> lock(impl_->controlMutex);
    return impl_->hasAudio ? impl_->audioTrack.trackId : 0;
}
bool PlayerEngine::SelectAudioTrack(std::uint32_t trackId) {
    std::lock_guard<std::mutex> lock(impl_->controlMutex);
    return impl_->SelectAudioTrack(trackId);
}
bool PlayerEngine::HasEmbeddedSubtitles() const {
    return impl_->hasEmbeddedSubtitle;
}
std::vector<TrackInfo> PlayerEngine::EmbeddedSubtitleTracks() const {
    std::lock_guard<std::mutex> lock(impl_->controlMutex);
    return impl_->subtitleTracks;
}
std::uint32_t PlayerEngine::SelectedEmbeddedSubtitleTrackId() const {
    std::lock_guard<std::mutex> lock(impl_->controlMutex);
    return impl_->hasEmbeddedSubtitle ? impl_->subtitleTrack.trackId : 0;
}
bool PlayerEngine::SelectEmbeddedSubtitleTrack(std::uint32_t trackId) {
    std::lock_guard<std::mutex> lock(impl_->controlMutex);
    return impl_->SelectEmbeddedSubtitleTrack(trackId);
}
std::wstring PlayerEngine::EmbeddedSubtitleText() const {
    return impl_->EmbeddedSubtitleText();
}
std::wstring PlayerEngine::EmbeddedSubtitleDescription() const {
    return impl_->EmbeddedSubtitleDescription();
}
double PlayerEngine::Duration() const { return impl_->duration; }
double PlayerEngine::CurrentPosition() const { return impl_->CurrentPosition(); }
int PlayerEngine::VideoWidth() const { return impl_->width; }
int PlayerEngine::VideoHeight() const { return impl_->height; }

std::shared_ptr<DecodedVideoFrame> PlayerEngine::AcquireVideoFrame() {
    if (!impl_->opened.load()) return nullptr;
    const bool force = impl_->forceNextFrame.exchange(false);
    auto frame = impl_->videoFrames.Acquire(impl_->CurrentPosition(),
                                            impl_->paused.load(), force);
    if (frame) {
        impl_->displayedPts.store(frame->pts);
        if (impl_->paused.load()) impl_->pausedPosition.store(frame->pts);
    }
    return frame;
}

std::wstring PlayerEngine::MediaDescription() const { return impl_->mediaDescription; }
std::wstring PlayerEngine::DecoderDescription() const {
    std::lock_guard<std::mutex> lock(impl_->errorMutex);
    return impl_->decoderDescription;
}
std::wstring PlayerEngine::LastError() const {
    std::lock_guard<std::mutex> lock(impl_->errorMutex);
    return impl_->lastError;
}
std::wstring PlayerEngine::TakeAsyncError() {
    std::lock_guard<std::mutex> lock(impl_->errorMutex);
    std::wstring result;
    result.swap(impl_->asyncError);
    return result;
}
