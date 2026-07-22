#pragma once

#include <string>
#include <vector>

struct SubtitleCue {
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    std::wstring text;
};

class SubtitleTrack {
public:
    bool Load(const std::wstring& filePath, std::wstring& error);
    void Clear();

    std::wstring TextAt(double seconds) const;
    bool Empty() const { return cues_.empty(); }
    const std::wstring& FilePath() const { return filePath_; }
    size_t CueCount() const { return cues_.size(); }

private:
    static bool DecodeFile(const std::wstring& path, std::wstring& text,
                           std::wstring& error);
    static bool ParseSrt(const std::wstring& content,
                         std::vector<SubtitleCue>& cues);
    static bool ParseAss(const std::wstring& content,
                         std::vector<SubtitleCue>& cues);
    static bool ParseSmi(const std::wstring& content,
                         std::vector<SubtitleCue>& cues);

    std::wstring filePath_;
    std::vector<SubtitleCue> cues_;
};
