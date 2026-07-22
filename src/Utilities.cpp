#include "Utilities.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                              value.data(), static_cast<int>(value.size()),
                                              nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                        value.data(), static_cast<int>(value.size()),
                        result.data(), required, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                              value.data(), static_cast<int>(value.size()),
                                              nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring result(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        value.data(), static_cast<int>(value.size()),
                        result.data(), required);
    return result;
}

std::wstring FormatMediaTime(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        seconds = 0.0;
    }
    const auto total = static_cast<long long>(std::floor(seconds + 0.5));
    const long long hours = total / 3600;
    const long long minutes = (total / 60) % 60;
    const long long secs = total % 60;

    wchar_t buffer[32] = {};
    if (hours > 0) {
        swprintf_s(buffer, L"%lld:%02lld:%02lld", hours, minutes, secs);
    } else {
        swprintf_s(buffer, L"%02lld:%02lld", minutes, secs);
    }
    return buffer;
}

std::wstring FormatHResult(long hr) {
    wchar_t* message = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    FormatMessageW(flags, nullptr, static_cast<DWORD>(hr),
                   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring result = message ? message : L"Unknown Windows error";
    if (message) {
        LocalFree(message);
    }
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) {
        result.pop_back();
    }
    return result;
}
