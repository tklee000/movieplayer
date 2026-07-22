#pragma once

#include <array>
#include <string>

using VideoExtensionList = std::array<const wchar_t*, 3>;

const VideoExtensionList& SupportedVideoExtensions();
bool IsSupportedVideoPath(const std::wstring& path);
std::wstring FindNextEpisodePath(const std::wstring& currentPath);
