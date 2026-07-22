#pragma once

#include <memory>
#include <string>

// Runs the project-local Whisper subtitle worker without blocking the Win32
// message thread.  The worker reports its state through a small UTF-8
// key=value file; Poll returns a new snapshot only when that file changes or
// when the child process reaches a terminal state.
class WhisperSubtitleJob final {
public:
    struct Update {
        // Percentage in the inclusive range [0, 100].
        double progress = 0.0;
        std::wstring message;
        std::wstring output;
        std::wstring error;
        bool finished = false;
    };

    struct RuntimeInfo {
        bool ready = false;
        std::wstring rootPath;
        std::wstring installerPath;
        std::wstring missingComponent;
        std::wstring error;
    };

    WhisperSubtitleJob();
    ~WhisperSubtitleJob();

    WhisperSubtitleJob(const WhisperSubtitleJob&) = delete;
    WhisperSubtitleJob& operator=(const WhisperSubtitleJob&) = delete;
    WhisperSubtitleJob(WhisperSubtitleJob&&) = delete;
    WhisperSubtitleJob& operator=(WhisperSubtitleJob&&) = delete;

    // Locates either a packaged runtime next to MoviePlayer.exe or the source
    // tree above build-vs2019, then checks all files needed by the worker.
    static RuntimeInfo InspectRuntime();

    // Starts the native whisper.cpp/CTranslate2 subtitle worker. Relative
    // input/output paths are made absolute using the process working
    // directory. A successful call only means that the asynchronous worker
    // was launched.
    bool Start(const std::wstring& videoPath,
               const std::wstring& outputPath,
               double totalDuration,
               const std::wstring& targetLanguage,
               std::wstring& error);

    // Returns true when update contains a state that has not previously been
    // returned.  Call this periodically from the UI thread.
    bool Poll(Update& update);

    // This also observes an already-exited child even if Poll has not yet been
    // called to collect its final status.
    bool IsRunning() const;

    // Safe to call repeatedly.  The Job Object terminates the entire worker
    // process tree, and all handles are released before this method returns.
    void Cancel();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
