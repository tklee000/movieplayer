#include "Localization.h"

#include <windows.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string_view>
#include <unordered_map>

#include "Utilities.h"

namespace Localization {
namespace {

constexpr wchar_t kRegistryPath[] = L"Software\\MoviePlayer";
constexpr wchar_t kRegistryValue[] = L"Language";

constexpr std::array<LanguageInfo, 12> kLanguages = {{
    {L"en", "language.english"},
    {L"ja", "language.japanese"},
    {L"ko", "language.korean"},
    {L"fr", "language.french"},
    {L"de", "language.german"},
    {L"zh-CN", "language.chinese_simplified"},
    {L"zh-TW", "language.chinese_traditional"},
    {L"es", "language.spanish"},
    {L"pt", "language.portuguese"},
    {L"hi", "language.hindi"},
    {L"id", "language.indonesian"},
    {L"ar", "language.arabic"},
}};

std::mutex g_mutex;
std::filesystem::path g_languageDirectory;
std::wstring g_currentLanguage = L"en";
std::unordered_map<std::string, std::wstring> g_english;
std::unordered_map<std::string, std::wstring> g_selected;

bool IsSupported(const std::wstring& code) {
    for (const auto& language : kLanguages) {
        if (_wcsicmp(language.code, code.c_str()) == 0) {
            return true;
        }
    }
    return false;
}

std::wstring CanonicalCode(const std::wstring& code) {
    for (const auto& language : kLanguages) {
        if (_wcsicmp(language.code, code.c_str()) == 0) {
            return language.code;
        }
    }
    return L"en";
}

std::wstring Unescape(const std::wstring& value) {
    std::wstring result;
    result.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        if (value[index] != L'\\' || index + 1 >= value.size()) {
            result.push_back(value[index]);
            continue;
        }
        const wchar_t escaped = value[++index];
        switch (escaped) {
        case L'n': result.push_back(L'\n'); break;
        case L'r': result.push_back(L'\r'); break;
        case L't': result.push_back(L'\t'); break;
        case L'\\': result.push_back(L'\\'); break;
        default:
            result.push_back(L'\\');
            result.push_back(escaped);
            break;
        }
    }
    return result;
}

bool LoadFile(const std::filesystem::path& path,
              std::unordered_map<std::string, std::wstring>& values,
              std::wstring& error) {
    values.clear();
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = L"Language file was not found: " + path.wstring();
        return false;
    }

    const std::string bytes((std::istreambuf_iterator<char>(stream)),
                            std::istreambuf_iterator<char>());
    std::string utf8 = bytes;
    if (utf8.size() >= 3 && static_cast<unsigned char>(utf8[0]) == 0xEF &&
        static_cast<unsigned char>(utf8[1]) == 0xBB &&
        static_cast<unsigned char>(utf8[2]) == 0xBF) {
        utf8.erase(0, 3);
    }
    const std::wstring content = Utf8ToWide(utf8);
    if (!utf8.empty() && content.empty()) {
        error = L"Language file is not valid UTF-8: " + path.wstring();
        return false;
    }

    std::wistringstream lines(content);
    std::wstring line;
    size_t lineNumber = 0;
    while (std::getline(lines, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }
        const size_t first = line.find_first_not_of(L" \t");
        if (first == std::wstring::npos || line[first] == L'#' ||
            line[first] == L';') {
            continue;
        }
        const size_t separator = line.find(L'=', first);
        if (separator == std::wstring::npos) {
            error = L"Invalid language entry at " + path.wstring() + L":" +
                    std::to_wstring(lineNumber);
            return false;
        }
        size_t keyEnd = separator;
        while (keyEnd > first && (line[keyEnd - 1] == L' ' ||
                                  line[keyEnd - 1] == L'\t')) {
            --keyEnd;
        }
        const std::wstring wideKey = line.substr(first, keyEnd - first);
        const std::string key = WideToUtf8(wideKey);
        if (key.empty()) {
            error = L"Empty language key at " + path.wstring() + L":" +
                    std::to_wstring(lineNumber);
            return false;
        }
        values[key] = Unescape(line.substr(separator + 1));
    }
    return true;
}

std::filesystem::path ExecutableDirectory() {
    std::array<wchar_t, 32768> buffer = {};
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
}

std::filesystem::path FindLanguageDirectory() {
    std::filesystem::path current = ExecutableDirectory();
    for (int depth = 0; depth < 6 && !current.empty(); ++depth) {
        const std::filesystem::path candidate = current / L"languages";
        if (std::filesystem::exists(candidate / L"en.lang")) {
            return candidate;
        }
        const std::filesystem::path parent = current.parent_path();
        if (parent == current) {
            break;
        }
        current = parent;
    }
    const std::filesystem::path candidate =
        std::filesystem::current_path() / L"languages";
    return std::filesystem::exists(candidate / L"en.lang")
               ? candidate
               : std::filesystem::path();
}

std::wstring ReadSavedLanguage() {
    wchar_t value[32] = {};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegistryPath, kRegistryValue,
                     RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS &&
        IsSupported(value)) {
        return CanonicalCode(value);
    }
    return {};
}

std::wstring DetectSystemLanguage() {
    wchar_t localeName[LOCALE_NAME_MAX_LENGTH] = {};
    if (!GetUserDefaultLocaleName(localeName, LOCALE_NAME_MAX_LENGTH)) {
        return L"en";
    }
    std::wstring locale = localeName;
    if (_wcsnicmp(locale.c_str(), L"ja", 2) == 0) return L"ja";
    if (_wcsnicmp(locale.c_str(), L"ko", 2) == 0) return L"ko";
    if (_wcsnicmp(locale.c_str(), L"fr", 2) == 0) return L"fr";
    if (_wcsnicmp(locale.c_str(), L"es", 2) == 0) return L"es";
    if (_wcsnicmp(locale.c_str(), L"pt", 2) == 0) return L"pt";
    if (_wcsnicmp(locale.c_str(), L"hi", 2) == 0) return L"hi";
    if (_wcsnicmp(locale.c_str(), L"id", 2) == 0) return L"id";
    if (_wcsnicmp(locale.c_str(), L"ar", 2) == 0) return L"ar";
    if (_wcsnicmp(locale.c_str(), L"zh", 2) == 0) {
        if (locale.find(L"TW") != std::wstring::npos ||
            locale.find(L"HK") != std::wstring::npos ||
            locale.find(L"MO") != std::wstring::npos ||
            locale.find(L"Hant") != std::wstring::npos) {
            return L"zh-TW";
        }
        return L"zh-CN";
    }
    return L"en";
}

bool SaveLanguage(const std::wstring& code, std::wstring& error) {
    HKEY key = nullptr;
    const LSTATUS createStatus = RegCreateKeyExW(
        HKEY_CURRENT_USER, kRegistryPath, 0, nullptr, REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE, nullptr, &key, nullptr);
    if (createStatus != ERROR_SUCCESS) {
        error = L"Could not save the UI language.\n\n" +
                FormatHResult(createStatus);
        return false;
    }
    const DWORD bytes = static_cast<DWORD>((code.size() + 1) * sizeof(wchar_t));
    const LSTATUS setStatus = RegSetValueExW(
        key, kRegistryValue, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(code.c_str()), bytes);
    RegCloseKey(key);
    if (setStatus != ERROR_SUCCESS) {
        error = L"Could not save the UI language.\n\n" +
                FormatHResult(setStatus);
        return false;
    }
    return true;
}

}  // namespace

bool Initialize(std::wstring& error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    error.clear();
    g_languageDirectory = FindLanguageDirectory();
    if (g_languageDirectory.empty()) {
        error = L"The languages directory could not be found.";
        return false;
    }
    if (!LoadFile(g_languageDirectory / L"en.lang", g_english, error)) {
        return false;
    }

    const std::wstring saved = ReadSavedLanguage();
    const std::wstring requested = saved.empty() ? DetectSystemLanguage() : saved;
    g_currentLanguage = CanonicalCode(requested);
    g_selected.clear();
    if (g_currentLanguage != L"en" &&
        !LoadFile(g_languageDirectory / (g_currentLanguage + L".lang"),
                  g_selected, error)) {
        g_currentLanguage = L"en";
        g_selected.clear();
        return false;
    }
    return true;
}

bool SetLanguage(const std::wstring& code, std::wstring& error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    error.clear();
    if (!IsSupported(code)) {
        error = L"Unsupported UI language: " + code;
        return false;
    }
    const std::wstring canonical = CanonicalCode(code);
    std::unordered_map<std::string, std::wstring> selected;
    if (canonical != L"en" &&
        !LoadFile(g_languageDirectory / (canonical + L".lang"),
                  selected, error)) {
        return false;
    }
    if (!SaveLanguage(canonical, error)) {
        return false;
    }
    g_currentLanguage = canonical;
    g_selected = std::move(selected);
    return true;
}

std::wstring CurrentLanguage() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_currentLanguage;
}

const LanguageInfo* Languages(size_t& count) {
    count = kLanguages.size();
    return kLanguages.data();
}

std::wstring Text(const char* key) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto selected = g_selected.find(key);
    if (selected != g_selected.end()) {
        return selected->second;
    }
    const auto english = g_english.find(key);
    if (english != g_english.end()) {
        return english->second;
    }
    return L"[" + Utf8ToWide(key ? key : "") + L"]";
}

std::wstring Format(
    const char* key,
    std::initializer_list<std::pair<std::wstring, std::wstring>> replacements) {
    std::wstring result = Text(key);
    for (const auto& replacement : replacements) {
        const std::wstring token = L"{" + replacement.first + L"}";
        size_t position = 0;
        while ((position = result.find(token, position)) != std::wstring::npos) {
            result.replace(position, token.size(), replacement.second);
            position += replacement.second.size();
        }
    }
    return result;
}

}  // namespace Localization
