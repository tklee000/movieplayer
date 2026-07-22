#pragma once

#include "codec/core/CodecTypes.h"

#include <d3d11.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using DecodedVideoFrame = movieplayer::codec::VideoFrame;

class PlayerEngine {
public:
    PlayerEngine();
    ~PlayerEngine();

    PlayerEngine(const PlayerEngine&) = delete;
    PlayerEngine& operator=(const PlayerEngine&) = delete;

    bool Open(const std::wstring& filePath, ID3D11Device* sharedD3DDevice);
    void Close();

    void Play();
    void Pause();
    void TogglePause();
    void Stop();
    bool Seek(double seconds);
    bool SeekRelative(double deltaSeconds);
    void StepFrame();

    void SetVolume(float volume);
    float Volume() const;
    void SetMuted(bool muted);
    bool IsMuted() const;
    void SetSpeed(float speed);
    float Speed() const;

    bool IsOpen() const;
    bool IsPaused() const;
    bool IsEnded() const;
    bool HasAudio() const;
    std::vector<movieplayer::codec::TrackInfo> AudioTracks() const;
    std::uint32_t SelectedAudioTrackId() const;
    bool SelectAudioTrack(std::uint32_t trackId);
    bool HasEmbeddedSubtitles() const;
    std::vector<movieplayer::codec::TrackInfo> EmbeddedSubtitleTracks() const;
    std::uint32_t SelectedEmbeddedSubtitleTrackId() const;
    bool SelectEmbeddedSubtitleTrack(std::uint32_t trackId);
    std::wstring EmbeddedSubtitleText() const;
    std::wstring EmbeddedSubtitleDescription() const;
    double Duration() const;
    double CurrentPosition() const;
    int VideoWidth() const;
    int VideoHeight() const;

    std::shared_ptr<DecodedVideoFrame> AcquireVideoFrame();
    std::wstring MediaDescription() const;
    std::wstring DecoderDescription() const;
    std::wstring LastError() const;
    std::wstring TakeAsyncError();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
