#include "MediaFileTools.h"

#include <windows.h>
#include <shlwapi.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <limits>
#include <vector>

namespace {

const VideoExtensionList kVideoExtensions = {
    L".mp4",
    L".mkv",
    L".avi",
};

struct EpisodeFileInfo {
    int episode = -1;
    size_t episodeTokenIndex = std::wstring::npos;
    std::vector<std::wstring> tokens;
    std::vector<std::wstring> stableTokens;
};

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}

std::wstring TrimAndLower(const std::wstring& value) {
    size_t first = 0;
    while (first < value.size() && std::iswspace(value[first])) {
        ++first;
    }
    size_t last = value.size();
    while (last > first && std::iswspace(value[last - 1])) {
        --last;
    }
    return ToLower(value.substr(first, last - first));
}

std::vector<std::wstring> SplitDotTokens(const std::wstring& stem) {
    std::vector<std::wstring> result;
    size_t start = 0;
    while (start <= stem.size()) {
        const size_t dot = stem.find(L'.', start);
        const size_t end = dot == std::wstring::npos ? stem.size() : dot;
        std::wstring token = TrimAndLower(stem.substr(start, end - start));
        if (!token.empty()) {
            result.push_back(std::move(token));
        }
        if (dot == std::wstring::npos) {
            break;
        }
        start = dot + 1;
    }
    return result;
}

bool ParseEpisodeToken(const std::wstring& token, int& episode) {
    size_t digitStart = 0;
    if (token.size() >= 2 && token[0] == L'e' && token[1] == L'p') {
        digitStart = 2;
    } else if (!token.empty() && token[0] == L'e') {
        digitStart = 1;
    } else {
        return false;
    }
    if (digitStart == token.size() || token.size() - digitStart > 4) {
        return false;
    }

    int value = 0;
    for (size_t index = digitStart; index < token.size(); ++index) {
        if (!std::iswdigit(token[index])) {
            return false;
        }
        value = value * 10 + static_cast<int>(token[index] - L'0');
    }
    episode = value;
    return true;
}

bool IsDateToken(const std::wstring& token) {
    if (token.size() != 6 && token.size() != 8) {
        return false;
    }
    return std::all_of(token.begin(), token.end(),
                       [](wchar_t c) { return std::iswdigit(c) != 0; });
}

EpisodeFileInfo ParseEpisodeFileInfo(const std::wstring& path) {
    EpisodeFileInfo result;
    const wchar_t* fileNamePointer = PathFindFileNameW(path.c_str());
    const std::wstring fileName = fileNamePointer ? fileNamePointer : path;
    const wchar_t* extension = PathFindExtensionW(fileName.c_str());
    const size_t stemLength = extension
                                  ? static_cast<size_t>(extension - fileName.c_str())
                                  : fileName.size();
    result.tokens = SplitDotTokens(fileName.substr(0, stemLength));

    for (size_t index = 0; index < result.tokens.size(); ++index) {
        int episode = -1;
        if (ParseEpisodeToken(result.tokens[index], episode)) {
            result.episode = episode;
            result.episodeTokenIndex = index;
            break;
        }
    }
    if (result.episode < 0) {
        return result;
    }

    for (size_t index = 0; index < result.tokens.size(); ++index) {
        if (index != result.episodeTokenIndex && !IsDateToken(result.tokens[index])) {
            result.stableTokens.push_back(result.tokens[index]);
        }
    }
    return result;
}

size_t CountCommonTokens(const std::vector<std::wstring>& left,
                         const std::vector<std::wstring>& right) {
    std::vector<bool> used(right.size(), false);
    size_t matches = 0;
    for (const std::wstring& token : left) {
        for (size_t index = 0; index < right.size(); ++index) {
            if (!used[index] && token == right[index]) {
                used[index] = true;
                ++matches;
                break;
            }
        }
    }
    return matches;
}

size_t CountLeadingTitleTokens(const EpisodeFileInfo& current,
                               const EpisodeFileInfo& candidate) {
    const size_t count = std::min(current.episodeTokenIndex,
                                  candidate.episodeTokenIndex);
    size_t matches = 0;
    while (matches < count && current.tokens[matches] == candidate.tokens[matches]) {
        ++matches;
    }
    return matches;
}

size_t CountCommonTitleTokens(const EpisodeFileInfo& current,
                              const EpisodeFileInfo& candidate) {
    std::vector<bool> used(candidate.episodeTokenIndex, false);
    size_t matches = 0;
    for (size_t left = 0; left < current.episodeTokenIndex; ++left) {
        for (size_t right = 0; right < candidate.episodeTokenIndex; ++right) {
            if (!used[right] && current.tokens[left] == candidate.tokens[right]) {
                used[right] = true;
                ++matches;
                break;
            }
        }
    }
    return matches;
}

int EpisodeSimilarityScore(const EpisodeFileInfo& current,
                           const EpisodeFileInfo& candidate,
                           bool sameExtension) {
    if (current.episode < 0 || candidate.episode != current.episode + 1 ||
        current.stableTokens.empty() || candidate.stableTokens.empty()) {
        return -1;
    }

    const size_t common = CountCommonTokens(current.stableTokens,
                                            candidate.stableTokens);
    const size_t denominator = std::max(current.stableTokens.size(),
                                        candidate.stableTokens.size());
    const double similarity = denominator > 0
                                  ? static_cast<double>(common) / denominator
                                  : 0.0;
    const size_t leading = CountLeadingTitleTokens(current, candidate);
    const size_t commonTitle = CountCommonTitleTokens(current, candidate);

    if (commonTitle == 0) {
        return -1;
    }

    if (leading > 0) {
        const size_t requiredCommon = std::min<size_t>(2, current.stableTokens.size());
        if (common < requiredCommon || similarity < 0.40) {
            return -1;
        }
    } else if (common < 3 || similarity < 0.75) {
        return -1;
    }

    const int tokenDifference = static_cast<int>(
        std::max(current.stableTokens.size(), candidate.stableTokens.size()) -
        std::min(current.stableTokens.size(), candidate.stableTokens.size()));
    return static_cast<int>(leading) * 10000 +
           static_cast<int>(std::lround(similarity * 1000.0)) +
           static_cast<int>(common) * 100 - tokenDifference * 10 +
           (sameExtension ? 5 : 0);
}

std::wstring DirectoryOf(const std::wstring& path) {
    const size_t separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        return L".";
    }
    if (separator == 2 && path.size() >= 3 && path[1] == L':') {
        return path.substr(0, 3);
    }
    return path.substr(0, separator);
}

std::wstring JoinPath(const std::wstring& directory, const std::wstring& fileName) {
    if (!directory.empty() &&
        (directory.back() == L'\\' || directory.back() == L'/')) {
        return directory + fileName;
    }
    return directory + L"\\" + fileName;
}

}  // namespace

const VideoExtensionList& SupportedVideoExtensions() {
    return kVideoExtensions;
}

bool IsSupportedVideoPath(const std::wstring& path) {
    const wchar_t* extensionPointer = PathFindExtensionW(path.c_str());
    const std::wstring extension = ToLower(extensionPointer ? extensionPointer : L"");
    return std::any_of(kVideoExtensions.begin(), kVideoExtensions.end(),
                       [&extension](const wchar_t* supported) {
                           return extension == supported;
                       });
}

std::wstring FindNextEpisodePath(const std::wstring& currentPath) {
    const EpisodeFileInfo current = ParseEpisodeFileInfo(currentPath);
    if (current.episode < 0 || current.episode == 9999) {
        return {};
    }

    const std::wstring directory = DirectoryOf(currentPath);
    const std::wstring pattern = JoinPath(directory, L"*");
    WIN32_FIND_DATAW data = {};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        return {};
    }

    const std::wstring currentExtension = ToLower(
        PathFindExtensionW(currentPath.c_str()));
    int bestScore = std::numeric_limits<int>::min();
    std::wstring bestPath;
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
            !IsSupportedVideoPath(data.cFileName)) {
            continue;
        }
        const std::wstring candidatePath = JoinPath(directory, data.cFileName);
        if (_wcsicmp(candidatePath.c_str(), currentPath.c_str()) == 0) {
            continue;
        }

        const EpisodeFileInfo candidate = ParseEpisodeFileInfo(data.cFileName);
        const std::wstring candidateExtension = ToLower(
            PathFindExtensionW(data.cFileName));
        const int score = EpisodeSimilarityScore(
            current, candidate, currentExtension == candidateExtension);
        if (score > bestScore ||
            (score == bestScore && !bestPath.empty() &&
             _wcsicmp(candidatePath.c_str(), bestPath.c_str()) < 0)) {
            bestScore = score;
            bestPath = candidatePath;
        }
    } while (FindNextFileW(find, &data));
    FindClose(find);

    return bestScore >= 0 ? bestPath : std::wstring();
}
