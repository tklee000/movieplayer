#pragma once

#include <initializer_list>
#include <string>
#include <utility>

namespace Localization {

struct LanguageInfo {
    const wchar_t* code;
    const char* displayNameKey;
};

bool Initialize(std::wstring& error);
bool SetLanguage(const std::wstring& code, std::wstring& error);

std::wstring CurrentLanguage();
const LanguageInfo* Languages(size_t& count);
std::wstring Text(const char* key);
std::wstring Format(
    const char* key,
    std::initializer_list<std::pair<std::wstring, std::wstring>> replacements);

}  // namespace Localization
