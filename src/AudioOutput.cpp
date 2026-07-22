#include "AudioOutput.h"

#include "Utilities.h"

#include <algorithm>
#include <cmath>
#include <cstring>

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
    hr = xaudio_->CreateMasteringVoice(&masteringVoice_, XAUDIO2_DEFAULT_CHANNELS,
                                       XAUDIO2_DEFAULT_SAMPLERATE, 0, nullptr, nullptr,
                                       AudioCategory_Movie);
    if (FAILED(hr)) {
        lastError_ = L"CreateMasteringVoice failed: " + FormatHResult(hr);
        xaudio_.Reset();
        return false;
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
    queueCv_.notify_all();
}

bool AudioOutput::Reset() {
    DestroySourceVoice();
    hasClock_.store(false);
    basePts_.store(0.0);
    return CreateSourceVoice();
}

bool AudioOutput::Submit(const uint8_t* data, size_t byteCount, double ptsSeconds) {
    if (!data || byteCount == 0 || abort_.load()) {
        return false;
    }

    std::unique_ptr<AudioBlock> block(new AudioBlock());
    block->bytes.assign(data, data + byteCount);

    {
        std::unique_lock<std::mutex> lock(queueMutex_);
        queueCv_.wait(lock, [this] {
            return abort_.load() || liveBlocks_.size() < kMaxQueuedBuffers;
        });
        if (abort_.load()) {
            return false;
        }
        liveBlocks_.insert(block.get());
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
        }
        queueCv_.notify_all();
        lastError_ = L"SubmitSourceBuffer failed: " + FormatHResult(hr);
        return false;
    }

    if (!hasClock_.exchange(true)) {
        basePts_.store(std::isfinite(ptsSeconds) ? ptsSeconds : 0.0);
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
    {
        std::lock_guard<std::mutex> lock(voiceMutex_);
        if (!sourceVoice_) {
            return basePts_.load();
        }
        sourceVoice_->GetState(&state);
    }
    return basePts_.load() + static_cast<double>(state.SamplesPlayed) /
                                 static_cast<double>(sampleRate_);
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
