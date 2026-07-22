#include "SubtitleTrack.h"

#include "Utilities.h"
#include "Localization.h"

#include <windows.h>
#include <shlwapi.h>

#include <algorithm>
#include <cwctype>
#include <limits>
#include <sstream>

namespace {

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}

std::wstring Trim(const std::wstring& value) {
    size_t first = 0;
    while (first < value.size() && std::iswspace(value[first])) {
        ++first;
    }
    size_t last = value.size();
    while (last > first && std::iswspace(value[last - 1])) {
        --last;
    }
    return value.substr(first, last - first);
}

void NormalizeNewlines(std::wstring& text) {
    text.erase(std::remove(text.begin(), text.end(), L'\r'), text.end());
}

bool ParseUnsigned(const std::wstring& value, size_t begin, size_t length,
                   int& number) {
    if (begin + length > value.size() || length == 0) {
        return false;
    }
    int result = 0;
    for (size_t i = begin; i < begin + length; ++i) {
        if (value[i] < L'0' || value[i] > L'9') {
            return false;
        }
        result = result * 10 + (value[i] - L'0');
    }
    number = result;
    return true;
}

bool ParseSrtTime(std::wstring value, double& seconds) {
    value = Trim(value);
    const size_t colon1 = value.find(L':');
    const size_t colon2 = colon1 == std::wstring::npos
                              ? std::wstring::npos
                              : value.find(L':', colon1 + 1);
    const size_t decimal = colon2 == std::wstring::npos
                               ? std::wstring::npos
                               : value.find_first_of(L",.", colon2 + 1);
    if (colon1 == std::wstring::npos || colon2 == std::wstring::npos) {
        return false;
    }
    int hours = 0, minutes = 0, wholeSeconds = 0, milliseconds = 0;
    if (!ParseUnsigned(value, 0, colon1, hours) ||
        !ParseUnsigned(value, colon1 + 1, colon2 - colon1 - 1, minutes)) {
        return false;
    }
    const size_t secondEnd = decimal == std::wstring::npos ? value.size() : decimal;
    if (!ParseUnsigned(value, colon2 + 1, secondEnd - colon2 - 1, wholeSeconds)) {
        return false;
    }
    if (decimal != std::wstring::npos) {
        size_t digits = 0;
        size_t cursor = decimal + 1;
        while (cursor < value.size() && std::iswdigit(value[cursor]) && digits < 3) {
            milliseconds = milliseconds * 10 + (value[cursor] - L'0');
            ++cursor;
            ++digits;
        }
        while (digits < 3) {
            milliseconds *= 10;
            ++digits;
        }
    }
    if (minutes >= 60 || wholeSeconds >= 60) {
        return false;
    }
    seconds = hours * 3600.0 + minutes * 60.0 + wholeSeconds + milliseconds / 1000.0;
    return true;
}

bool ParseAssTime(const std::wstring& value, double& seconds) {
    return ParseSrtTime(value, seconds);
}

std::wstring DecodeEntities(std::wstring text) {
    struct Entity { const wchar_t* encoded; const wchar_t* decoded; };
    const Entity entities[] = {
        {L"&nbsp;", L" "}, {L"&amp;", L"&"}, {L"&lt;", L"<"},
        {L"&gt;", L">"}, {L"&quot;", L"\""}, {L"&#39;", L"'"},
    };
    std::wstring lowered = Lower(text);
    for (const auto& entity : entities) {
        const std::wstring needle = entity.encoded;
        size_t position = 0;
        while ((position = lowered.find(needle, position)) != std::wstring::npos) {
            text.replace(position, needle.size(), entity.decoded);
            lowered.replace(position, needle.size(), entity.decoded);
            position += std::wcslen(entity.decoded);
        }
    }
    return text;
}

std::wstring StripHtml(std::wstring text) {
    std::wstring output;
    output.reserve(text.size());
    bool inTag = false;
    std::wstring tag;
    for (wchar_t c : text) {
        if (c == L'<') {
            inTag = true;
            tag.clear();
        } else if (c == L'>' && inTag) {
            const std::wstring lowerTag = Lower(Trim(tag));
            if (lowerTag == L"br" || lowerTag == L"br/" ||
                lowerTag == L"/p" || lowerTag == L"p") {
                if (!output.empty() && output.back() != L'\n') {
                    output.push_back(L'\n');
                }
            }
            inTag = false;
        } else if (inTag) {
            tag.push_back(c);
        } else {
            output.push_back(c);
        }
    }
    output = DecodeEntities(output);
    std::wstringstream stream(output);
    std::wstring line;
    std::wstring cleaned;
    while (std::getline(stream, line)) {
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        if (!cleaned.empty()) {
            cleaned.push_back(L'\n');
        }
        cleaned += line;
    }
    return cleaned;
}

std::wstring StripAssOverrides(const std::wstring& text) {
    std::wstring output;
    output.reserve(text.size());
    bool inOverride = false;
    for (size_t index = 0; index < text.size(); ++index) {
        const wchar_t c = text[index];
        if (c == L'{') {
            inOverride = true;
            continue;
        }
        if (c == L'}' && inOverride) {
            inOverride = false;
            continue;
        }
        if (inOverride) {
            continue;
        }
        if (c == L'\\' && index + 1 < text.size()) {
            const wchar_t next = text[index + 1];
            if (next == L'N' || next == L'n') {
                output.push_back(L'\n');
                ++index;
                continue;
            }
            if (next == L'h') {
                output.push_back(L' ');
                ++index;
                continue;
            }
        }
        output.push_back(c);
    }
    return Trim(StripHtml(output));
}

void FinalizeCues(std::vector<SubtitleCue>& cues) {
    cues.erase(std::remove_if(cues.begin(), cues.end(), [](const SubtitleCue& cue) {
        return cue.text.empty() || !std::isfinite(cue.startSeconds) ||
               !std::isfinite(cue.endSeconds) || cue.endSeconds <= cue.startSeconds;
    }), cues.end());
    std::sort(cues.begin(), cues.end(), [](const SubtitleCue& left, const SubtitleCue& right) {
        if (left.startSeconds != right.startSeconds) {
            return left.startSeconds < right.startSeconds;
        }
        return left.endSeconds < right.endSeconds;
    });
}

}  // namespace

bool SubtitleTrack::DecodeFile(const std::wstring& path, std::wstring& text,
                               std::wstring& error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = Localization::Format(
            "error.subtitle_open",
            {{L"error", FormatHResult(HRESULT_FROM_WIN32(GetLastError()))}});
        return false;
    }

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 64LL * 1024 * 1024) {
        CloseHandle(file);
        error = Localization::Text("error.subtitle_size");
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size.QuadPart));
    size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD read = 0;
        const DWORD request = static_cast<DWORD>(std::min<size_t>(
            bytes.size() - offset, std::numeric_limits<DWORD>::max()));
        if (!ReadFile(file, bytes.data() + offset, request, &read, nullptr) || read == 0) {
            CloseHandle(file);
            error = Localization::Text("error.subtitle_read");
            return false;
        }
        offset += read;
    }
    CloseHandle(file);

    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        const size_t count = (bytes.size() - 2) / 2;
        text.resize(count);
        for (size_t i = 0; i < count; ++i) {
            text[i] = static_cast<wchar_t>(bytes[2 + i * 2] | (bytes[3 + i * 2] << 8));
        }
    } else if (bytes.size() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        const size_t count = (bytes.size() - 2) / 2;
        text.resize(count);
        for (size_t i = 0; i < count; ++i) {
            text[i] = static_cast<wchar_t>((bytes[2 + i * 2] << 8) | bytes[3 + i * 2]);
        }
    } else {
        size_t start = 0;
        if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
            start = 3;
        }
        const char* source = reinterpret_cast<const char*>(bytes.data() + start);
        const int sourceLength = static_cast<int>(bytes.size() - start);
        int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        source, sourceLength, nullptr, 0);
        UINT codePage = CP_UTF8;
        DWORD flags = MB_ERR_INVALID_CHARS;
        if (count <= 0) {
            codePage = 949;  // Korean Windows legacy encoding (CP949/UHC).
            flags = 0;
            count = MultiByteToWideChar(codePage, flags, source, sourceLength, nullptr, 0);
        }
        if (count <= 0) {
            error = Localization::Text("error.subtitle_encoding");
            return false;
        }
        text.resize(static_cast<size_t>(count));
        MultiByteToWideChar(codePage, flags, source, sourceLength, text.data(), count);
    }
    NormalizeNewlines(text);
    return true;
}

bool SubtitleTrack::ParseSrt(const std::wstring& content,
                             std::vector<SubtitleCue>& cues) {
    std::wstringstream stream(content);
    std::vector<std::wstring> lines;
    std::wstring line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }

    for (size_t index = 0; index < lines.size();) {
        const size_t arrow = lines[index].find(L"-->");
        if (arrow == std::wstring::npos) {
            ++index;
            continue;
        }
        double start = 0.0, end = 0.0;
        std::wstring right = lines[index].substr(arrow + 3);
        const size_t setting = right.find_first_of(L" \t", 1);
        if (setting != std::wstring::npos) {
            right = right.substr(0, setting);
        }
        if (!ParseSrtTime(lines[index].substr(0, arrow), start) ||
            !ParseSrtTime(right, end)) {
            ++index;
            continue;
        }
        ++index;
        std::wstring cueText;
        while (index < lines.size() && !Trim(lines[index]).empty()) {
            if (!cueText.empty()) {
                cueText.push_back(L'\n');
            }
            cueText += lines[index++];
        }
        cueText = StripHtml(cueText);
        if (!cueText.empty()) {
            cues.push_back({start, end, cueText});
        }
    }
    return !cues.empty();
}

bool SubtitleTrack::ParseAss(const std::wstring& content,
                             std::vector<SubtitleCue>& cues) {
    std::wstringstream stream(content);
    std::wstring line;
    while (std::getline(stream, line)) {
        const std::wstring trimmed = Trim(line);
        const std::wstring lower = Lower(trimmed);
        if (lower.rfind(L"dialogue:", 0) != 0) {
            continue;
        }
        const std::wstring payload = trimmed.substr(trimmed.find(L':') + 1);
        std::vector<std::wstring> fields;
        size_t startPosition = 0;
        for (int field = 0; field < 9; ++field) {
            const size_t comma = payload.find(L',', startPosition);
            if (comma == std::wstring::npos) {
                break;
            }
            fields.push_back(payload.substr(startPosition, comma - startPosition));
            startPosition = comma + 1;
        }
        if (fields.size() != 9) {
            continue;
        }
        fields.push_back(payload.substr(startPosition));
        double cueStart = 0.0, cueEnd = 0.0;
        if (!ParseAssTime(fields[1], cueStart) || !ParseAssTime(fields[2], cueEnd)) {
            continue;
        }
        const std::wstring cueText = StripAssOverrides(fields[9]);
        if (!cueText.empty()) {
            cues.push_back({cueStart, cueEnd, cueText});
        }
    }
    return !cues.empty();
}

bool SubtitleTrack::ParseSmi(const std::wstring& content,
                             std::vector<SubtitleCue>& cues) {
    const std::wstring lower = Lower(content);
    struct SyncBlock { double start; size_t contentBegin; size_t contentEnd; };
    std::vector<SyncBlock> blocks;
    size_t position = 0;
    while ((position = lower.find(L"<sync", position)) != std::wstring::npos) {
        const size_t close = lower.find(L'>', position + 5);
        if (close == std::wstring::npos) {
            break;
        }
        const std::wstring tag = lower.substr(position, close - position + 1);
        const size_t startKey = tag.find(L"start");
        if (startKey != std::wstring::npos) {
            const size_t equals = tag.find(L'=', startKey + 5);
            if (equals != std::wstring::npos) {
                size_t numberStart = equals + 1;
                while (numberStart < tag.size() && std::iswspace(tag[numberStart])) {
                    ++numberStart;
                }
                size_t numberEnd = numberStart;
                while (numberEnd < tag.size() && std::iswdigit(tag[numberEnd])) {
                    ++numberEnd;
                }
                int milliseconds = 0;
                if (numberEnd > numberStart &&
                    ParseUnsigned(tag, numberStart, numberEnd - numberStart, milliseconds)) {
                    if (!blocks.empty()) {
                        blocks.back().contentEnd = position;
                    }
                    blocks.push_back({milliseconds / 1000.0, close + 1, content.size()});
                }
            }
        }
        position = close + 1;
    }

    for (size_t i = 0; i < blocks.size(); ++i) {
        const auto& block = blocks[i];
        std::wstring blockText = content.substr(block.contentBegin,
                                                block.contentEnd - block.contentBegin);
        std::wstring cueText = StripHtml(blockText);
        if (Trim(Lower(cueText)) == L"&nbsp;" || Trim(cueText).empty()) {
            continue;
        }
        const double end = (i + 1 < blocks.size()) ? blocks[i + 1].start
                                                   : block.start + 5.0;
        cues.push_back({block.start, end, cueText});
    }
    return !cues.empty();
}

bool SubtitleTrack::Load(const std::wstring& filePath, std::wstring& error) {
    std::wstring content;
    if (!DecodeFile(filePath, content, error)) {
        return false;
    }

    const wchar_t* extensionPointer = PathFindExtensionW(filePath.c_str());
    const std::wstring extension = Lower(extensionPointer ? extensionPointer : L"");
    std::vector<SubtitleCue> parsed;
    bool success = false;
    if (extension == L".srt" || extension == L".vtt") {
        success = ParseSrt(content, parsed);
    } else if (extension == L".ass" || extension == L".ssa") {
        success = ParseAss(content, parsed);
    } else if (extension == L".smi" || extension == L".sami") {
        success = ParseSmi(content, parsed);
    } else {
        success = ParseSrt(content, parsed) || ParseAss(content, parsed) || ParseSmi(content, parsed);
    }
    FinalizeCues(parsed);
    if (!success || parsed.empty()) {
        error = Localization::Text("error.subtitle_cues");
        return false;
    }
    cues_ = std::move(parsed);
    filePath_ = filePath;
    error.clear();
    return true;
}

void SubtitleTrack::Clear() {
    filePath_.clear();
    cues_.clear();
}

std::wstring SubtitleTrack::TextAt(double seconds) const {
    if (cues_.empty() || !std::isfinite(seconds)) {
        return {};
    }
    const auto next = std::upper_bound(
        cues_.begin(), cues_.end(), seconds,
        [](double value, const SubtitleCue& cue) { return value < cue.startSeconds; });
    if (next == cues_.begin()) {
        return {};
    }

    std::wstring result;
    auto cursor = next;
    size_t checked = 0;
    while (cursor != cues_.begin() && checked < 8) {
        --cursor;
        ++checked;
        if (cursor->endSeconds <= seconds) {
            if (!result.empty()) {
                break;
            }
            continue;
        }
        if (cursor->startSeconds <= seconds) {
            if (!result.empty()) {
                result = cursor->text + L"\n" + result;
            } else {
                result = cursor->text;
            }
        }
    }
    return result;
}
