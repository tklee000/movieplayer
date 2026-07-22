#pragma once

#include <windows.h>
#include <xaudio2.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();

    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    bool Initialize(int sampleRate = 48000, int channels = 2);
    void Shutdown();
    bool Reset();

    bool Submit(const uint8_t* data, size_t byteCount, double ptsSeconds);
    void SetPaused(bool paused);
    void SetVolume(float volume);
    void SetMuted(bool muted);
    void SetSpeed(float speed);
    void SetAbort(bool abort);

    double ClockSeconds() const;
    bool HasClock() const;
    uint32_t QueuedBuffers() const;
    const std::wstring& LastError() const { return lastError_; }

private:
    struct AudioBlock {
        std::vector<uint8_t> bytes;
    };

    class VoiceCallback final : public IXAudio2VoiceCallback {
    public:
        explicit VoiceCallback(AudioOutput* owner) : owner_(owner) {}
        void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
        void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
        void STDMETHODCALLTYPE OnStreamEnd() override {}
        void STDMETHODCALLTYPE OnBufferStart(void*) override {}
        void STDMETHODCALLTYPE OnBufferEnd(void* context) override;
        void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
        void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT error) override;

    private:
        AudioOutput* owner_;
    };

    bool CreateSourceVoice();
    void DestroySourceVoice();
    void HandleBufferEnd(AudioBlock* block);
    void HandleVoiceError(HRESULT error);
    float EffectiveVolume() const;

    Microsoft::WRL::ComPtr<IXAudio2> xaudio_;
    IXAudio2MasteringVoice* masteringVoice_ = nullptr;
    IXAudio2SourceVoice* sourceVoice_ = nullptr;
    VoiceCallback callback_;

    int sampleRate_ = 48000;
    int channels_ = 2;
    int blockAlign_ = 4;

    mutable std::mutex voiceMutex_;
    mutable std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::unordered_set<AudioBlock*> liveBlocks_;
    static constexpr size_t kMaxQueuedBuffers = 24;

    std::atomic<bool> abort_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> muted_{false};
    std::atomic<float> volume_{1.0f};
    std::atomic<float> speed_{1.0f};
    std::atomic<bool> hasClock_{false};
    std::atomic<double> basePts_{0.0};
    std::wstring lastError_;
};
