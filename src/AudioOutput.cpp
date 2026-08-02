#include "AudioOutput.h"

#include "Utilities.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace {

// CurrentLatencyInSamples is documented as a minimum, approximate path
// delay. HDMI/display endpoints can add another device-side stage after the
// cursor reported by XAudio2. Keep enough presentation margin for that stage;
// otherwise the picture reaches the display before the corresponding sound.
constexpr double kAdditionalOutputLatencySeconds = 0.050;

}  // namespace

AudioOutput::AudioOutput() : callback_(this) {}

AudioOutput::~AudioOutput() {
    Shutdown();
}

bool AudioOutput::Initialize(int sampleRate, int channels) {
    Shutdown();
    sampleRate_ = sampleRate;
    channels_ = channels;
    blockAlign_ = channels_ * static_cast<int>(sizeof(int16_t));

    HRESULT hr = XAudio2Create(xaudio_.GetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        lastError_ = L"XAudio2Create failed: " + FormatHResult(hr);
        return false;
    }
    // Bind directly to the physical default endpoint.  XAudio2's virtual
    // endpoint adds a device-switching layer whose downstream delay is not
    // guaranteed to be represented by CurrentLatencyInSamples.  That makes
    // video driven from the reported audible-audio cursor run ahead on some
    // devices.
    hr = xaudio_->CreateMasteringVoice(
        &masteringVoice_, XAUDIO2_DEFAULT_CHANNELS,
        XAUDIO2_DEFAULT_SAMPLERATE, XAUDIO2_NO_VIRTUAL_AUDIO_CLIENT, nullptr,
        nullptr, AudioCategory_Movie);
    if (FAILED(hr)) {
        lastError_ = L"CreateMasteringVoice failed: " + FormatHResult(hr);
        xaudio_.Reset();
        return false;
    }
    XAUDIO2_VOICE_DETAILS masteringDetails = {};
    masteringVoice_->GetVoiceDetails(&masteringDetails);
    masteringSampleRate_ = masteringDetails.InputSampleRate != 0
                               ? static_cast<int>(masteringDetails.InputSampleRate)
                               : sampleRate_;
    // This diagnostic build always records the audio clock so a normal
    // Explorer launch captures long-running A/V drift without extra setup.
    wchar_t temporaryPath[MAX_PATH] = {};
    const DWORD pathLength = GetTempPathW(
        static_cast<DWORD>(std::size(temporaryPath)), temporaryPath);
    if (pathLength != 0 && pathLength < std::size(temporaryPath)) {
        std::lock_guard<std::mutex> diagnosticsLock(diagnosticsMutex_);
        diagnosticsPath_ =
            (std::filesystem::path(temporaryPath) / L"MoviePlayer-sync.csv")
                .wstring();
        diagnostics_.str({});
        diagnostics_.clear();
        diagnostics_
            << "wall_ms,samples_played,raw_latency_samples,"
               "filtered_latency_samples,buffers_queued,glitches,"
               "queued_sample_frames,queued_audio_ms,"
               "total_submitted_sample_frames,last_submitted_end_pts,"
               "source_sample_rate,mastering_sample_rate,block_align,"
               "clock_seconds,base_pts,speed,applied_latency_ms\n";
    }
    return CreateSourceVoice();
}

void AudioOutput::Shutdown() {
    SetAbort(true);
    DestroySourceVoice();
    if (masteringVoice_) {
        masteringVoice_->DestroyVoice();
        masteringVoice_ = nullptr;
    }
    xaudio_.Reset();
    hasClock_.store(false);
    latencyInitialized_.store(false);
    filteredLatencySamples_.store(0.0);
    lastClockSeconds_.store(0.0);
    lastDiagnosticsTick_.store(0);
    queuedSampleFrames_.store(0);
    totalSubmittedSampleFrames_.store(0);
    lastSubmittedEndPts_.store(0.0);
    {
        std::lock_guard<std::mutex> diagnosticsLock(diagnosticsMutex_);
        if (!diagnosticsPath_.empty()) {
            std::ofstream output(std::filesystem::path(diagnosticsPath_),
                                 std::ios::out | std::ios::trunc);
            if (output) output << diagnostics_.str();
        }
        diagnosticsPath_.clear();
        diagnostics_.str({});
        diagnostics_.clear();
    }
}

bool AudioOutput::CreateSourceVoice() {
    if (!xaudio_) {
        return false;
    }

    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = static_cast<WORD>(channels_);
    format.nSamplesPerSec = static_cast<DWORD>(sampleRate_);
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(blockAlign_);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    std::lock_guard<std::mutex> lock(voiceMutex_);
    HRESULT hr = xaudio_->CreateSourceVoice(&sourceVoice_, &format, 0,
                                             XAUDIO2_MAX_FREQ_RATIO, &callback_);
    if (FAILED(hr)) {
        lastError_ = L"CreateSourceVoice failed: " + FormatHResult(hr);
        sourceVoice_ = nullptr;
        return false;
    }
    sourceVoice_->SetVolume(EffectiveVolume());
    sourceVoice_->SetFrequencyRatio(speed_.load());
    if (!paused_.load()) {
        sourceVoice_->Start();
    }
    return true;
}

void AudioOutput::DestroySourceVoice() {
    {
        std::lock_guard<std::mutex> lock(voiceMutex_);
        if (sourceVoice_) {
            sourceVoice_->Stop();
            sourceVoice_->FlushSourceBuffers();
            sourceVoice_->DestroyVoice();
            sourceVoice_ = nullptr;
        }
    }

    std::unordered_set<AudioBlock*> remaining;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        remaining.swap(liveBlocks_);
    }
    for (AudioBlock* block : remaining) {
        delete block;
    }
    queuedSampleFrames_.store(0);
    queueCv_.notify_all();
}

bool AudioOutput::Reset() {
    DestroySourceVoice();
    hasClock_.store(false);
    basePts_.store(0.0);
    latencyInitialized_.store(false);
    filteredLatencySamples_.store(0.0);
    lastClockSeconds_.store(0.0);
    lastDiagnosticsTick_.store(0);
    queuedSampleFrames_.store(0);
    totalSubmittedSampleFrames_.store(0);
    lastSubmittedEndPts_.store(0.0);
    return CreateSourceVoice();
}

bool AudioOutput::Submit(const uint8_t* data, size_t byteCount, double ptsSeconds) {
    if (!data || byteCount == 0 || abort_.load()) {
        return false;
    }
    if (blockAlign_ <= 0 || byteCount % static_cast<size_t>(blockAlign_) != 0) {
        lastError_ = L"PCM buffer is not aligned to a complete sample frame";
        return false;
    }

    std::unique_ptr<AudioBlock> block(new AudioBlock());
    block->bytes.assign(data, data + byteCount);
    block->sampleFrames =
        static_cast<std::uint64_t>(byteCount / static_cast<size_t>(blockAlign_));

    {
        std::unique_lock<std::mutex> lock(queueMutex_);
        queueCv_.wait(lock, [this] {
            return abort_.load() || liveBlocks_.size() < kMaxQueuedBuffers;
        });
        if (abort_.load()) {
            return false;
        }
        liveBlocks_.insert(block.get());
        queuedSampleFrames_.fetch_add(block->sampleFrames);
    }

    XAUDIO2_BUFFER buffer = {};
    buffer.AudioBytes = static_cast<UINT32>(block->bytes.size());
    buffer.pAudioData = block->bytes.data();
    buffer.pContext = block.get();

    HRESULT hr = E_FAIL;
    {
        std::lock_guard<std::mutex> lock(voiceMutex_);
        if (sourceVoice_) {
            hr = sourceVoice_->SubmitSourceBuffer(&buffer);
        }
    }
    if (FAILED(hr)) {
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            liveBlocks_.erase(block.get());
            queuedSampleFrames_.fetch_sub(block->sampleFrames);
        }
        queueCv_.notify_all();
        lastError_ = L"SubmitSourceBuffer failed: " + FormatHResult(hr);
        return false;
    }

    totalSubmittedSampleFrames_.fetch_add(block->sampleFrames);
    if (std::isfinite(ptsSeconds) && sampleRate_ > 0) {
        lastSubmittedEndPts_.store(
            ptsSeconds + static_cast<double>(block->sampleFrames) /
                             static_cast<double>(sampleRate_));
    }

    if (!hasClock_.load()) {
        const double base = std::isfinite(ptsSeconds) ? ptsSeconds : 0.0;
        basePts_.store(base);
        lastClockSeconds_.store(base);
        // Submit runs on one audio decode thread. Publish hasClock only after
        // both clock anchors are visible to the UI thread.
        hasClock_.store(true);
    }
    block.release();
    return true;
}

void AudioOutput::SetPaused(bool paused) {
    paused_.store(paused);
    std::lock_guard<std::mutex> lock(voiceMutex_);
    if (sourceVoice_) {
        if (paused) {
            sourceVoice_->Stop();
        } else {
            sourceVoice_->Start();
        }
    }
}

float AudioOutput::EffectiveVolume() const {
    return muted_.load() ? 0.0f : std::max(0.0f, std::min(1.0f, volume_.load()));
}

void AudioOutput::SetVolume(float volume) {
    volume_.store(std::max(0.0f, std::min(1.0f, volume)));
    std::lock_guard<std::mutex> lock(voiceMutex_);
    if (sourceVoice_) {
        sourceVoice_->SetVolume(EffectiveVolume());
    }
}

void AudioOutput::SetMuted(bool muted) {
    muted_.store(muted);
    std::lock_guard<std::mutex> lock(voiceMutex_);
    if (sourceVoice_) {
        sourceVoice_->SetVolume(EffectiveVolume());
    }
}

void AudioOutput::SetSpeed(float speed) {
    speed = std::max(XAUDIO2_MIN_FREQ_RATIO,
                     std::min(XAUDIO2_MAX_FREQ_RATIO, speed));
    speed_.store(speed);
    std::lock_guard<std::mutex> lock(voiceMutex_);
    if (sourceVoice_) {
        sourceVoice_->SetFrequencyRatio(speed);
    }
}

void AudioOutput::SetAbort(bool abort) {
    abort_.store(abort);
    queueCv_.notify_all();
}

double AudioOutput::ClockSeconds() const {
    if (!hasClock_.load()) {
        return 0.0;
    }
    XAUDIO2_VOICE_STATE state = {};
    XAUDIO2_PERFORMANCE_DATA performance = {};
    {
        std::lock_guard<std::mutex> lock(voiceMutex_);
        if (!sourceVoice_) {
            return basePts_.load();
        }
        sourceVoice_->GetState(&state);
        if (xaudio_) xaudio_->GetPerformanceData(&performance);
    }
    const double processedSeconds =
        static_cast<double>(state.SamplesPlayed) /
        static_cast<double>(sampleRate_);
    // CurrentLatencyInSamples is a variable, approximate distance to the
    // speakers. Apply an unbiased low-pass filter so transient observations
    // cannot jump video in either direction. A zero or implausibly large
    // observation is ignored.
    const double rawLatencySamples = performance.CurrentLatencyInSamples;
    const double maximumSaneLatency =
        static_cast<double>(std::max(masteringSampleRate_, 1)) * 2.0;
    if (state.SamplesPlayed != 0 && rawLatencySamples > 0.0 &&
        rawLatencySamples <= maximumSaneLatency) {
        if (!latencyInitialized_.exchange(true)) {
            filteredLatencySamples_.store(rawLatencySamples);
        } else {
            double filtered = filteredLatencySamples_.load();
            filtered += (rawLatencySamples - filtered) * 0.10;
            filteredLatencySamples_.store(filtered);
        }
    }
    const double latencySeconds =
        masteringSampleRate_ > 0
            ? (filteredLatencySamples_.load() /
                   static_cast<double>(masteringSampleRate_) +
               kAdditionalOutputLatencySeconds) *
                  speed_.load()
            : 0.0;
    const double candidate =
        basePts_.load() + std::max(0.0, processedSeconds - latencySeconds);

    // UI and subtitle queries can occur between XAudio2 processing quanta.
    // Never let a late latency capture or device observation move media time
    // backwards; video remains slaved to a monotonic audible-audio clock.
    double previous = lastClockSeconds_.load();
    while (candidate > previous &&
           !lastClockSeconds_.compare_exchange_weak(previous, candidate)) {
    }
    const double clock = std::max(candidate, previous);

    const std::uint64_t now = GetTickCount64();
    std::uint64_t lastDiagnostics = lastDiagnosticsTick_.load();
    if (now - lastDiagnostics >= 1000U &&
        lastDiagnosticsTick_.compare_exchange_strong(lastDiagnostics, now)) {
        std::lock_guard<std::mutex> diagnosticsLock(diagnosticsMutex_);
        if (!diagnosticsPath_.empty()) {
            diagnostics_ << now << ',' << state.SamplesPlayed << ','
                         << performance.CurrentLatencyInSamples << ','
                         << filteredLatencySamples_.load() << ','
                         << state.BuffersQueued << ','
                         << performance.GlitchesSinceEngineStarted << ','
                         << queuedSampleFrames_.load() << ','
                         << (sampleRate_ > 0
                                 ? static_cast<double>(
                                       queuedSampleFrames_.load()) /
                                       static_cast<double>(sampleRate_) * 1000.0
                                 : 0.0)
                         << ',' << totalSubmittedSampleFrames_.load() << ','
                         << lastSubmittedEndPts_.load() << ',' << sampleRate_
                         << ',' << masteringSampleRate_ << ',' << blockAlign_
                         << ','
                         << clock << ',' << basePts_.load() << ','
                         << speed_.load() << ','
                         << latencySeconds * 1000.0 << '\n';
        }
    }
    return clock;
}

bool AudioOutput::HasClock() const {
    return hasClock_.load();
}

uint32_t AudioOutput::QueuedBuffers() const {
    std::lock_guard<std::mutex> lock(queueMutex_);
    return static_cast<uint32_t>(liveBlocks_.size());
}

void STDMETHODCALLTYPE AudioOutput::VoiceCallback::OnBufferEnd(void* context) {
    owner_->HandleBufferEnd(static_cast<AudioBlock*>(context));
}

void STDMETHODCALLTYPE AudioOutput::VoiceCallback::OnVoiceError(void*, HRESULT error) {
    owner_->HandleVoiceError(error);
}

void AudioOutput::HandleBufferEnd(AudioBlock* block) {
    if (!block) {
        return;
    }
    bool owned = false;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        const auto it = liveBlocks_.find(block);
        if (it != liveBlocks_.end()) {
            queuedSampleFrames_.fetch_sub(block->sampleFrames);
            liveBlocks_.erase(it);
            owned = true;
        }
    }
    if (owned) {
        delete block;
    }
    queueCv_.notify_all();
}

void AudioOutput::HandleVoiceError(HRESULT error) {
    lastError_ = L"XAudio2 voice error: " + FormatHResult(error);
    SetAbort(true);
}
