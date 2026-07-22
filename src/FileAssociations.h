#pragma once

#include <windows.h>

#include <string>

bool RegisterVideoFileAssociations(std::wstring& error);
bool OpenVideoDefaultAppsSettings(HWND owner, std::wstring& error);
