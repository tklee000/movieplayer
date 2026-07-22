#include "FileAssociations.h"

#include <shellapi.h>
#include <shlobj.h>

#include <vector>

#include "Localization.h"
#include "MediaFileTools.h"
#include "Utilities.h"

namespace {

constexpr wchar_t kRegisteredApplicationName[] = L"MoviePlayer";
constexpr wchar_t kMediaProgId[] = L"MoviePlayer.Media";

bool SetRegistryValue(HKEY root, const std::wstring& subkey,
                      const wchar_t* valueName, DWORD type,
                      const BYTE* data, DWORD dataSize,
                      std::wstring& error) {
    HKEY key = nullptr;
    DWORD disposition = 0;
    LSTATUS status = RegCreateKeyExW(
        root, subkey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE, nullptr, &key, &disposition);
    if (status != ERROR_SUCCESS) {
        error = Localization::Format(
            "error.association_registry_key",
            {{L"error", FormatHResult(status)}});
        return false;
    }

    status = RegSetValueExW(key, valueName, 0, type, data, dataSize);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        error = Localization::Format(
            "error.association_registry_value",
            {{L"error", FormatHResult(status)}});
        return false;
    }
    return true;
}

bool SetRegistryString(HKEY root, const std::wstring& subkey,
                       const wchar_t* valueName, const std::wstring& value,
                       std::wstring& error) {
    const DWORD byteCount = static_cast<DWORD>(
        (value.size() + 1) * sizeof(wchar_t));
    return SetRegistryValue(root, subkey, valueName, REG_SZ,
                            reinterpret_cast<const BYTE*>(value.c_str()),
                            byteCount, error);
}

bool SetRegistryEmptyValue(HKEY root, const std::wstring& subkey,
                           const wchar_t* valueName, std::wstring& error) {
    return SetRegistryValue(root, subkey, valueName, REG_NONE,
                            nullptr, 0, error);
}

bool CurrentExecutablePath(std::wstring& path, std::wstring& error) {
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        const DWORD code = GetLastError();
        error = Localization::Format(
            "error.executable_path",
            {{L"error", FormatHResult(
                code == ERROR_SUCCESS ? ERROR_INSUFFICIENT_BUFFER : code)}});
        return false;
    }
    path.assign(buffer.data(), length);
    return true;
}

}  // namespace

bool RegisterVideoFileAssociations(std::wstring& error) {
    error.clear();
    std::wstring executable;
    if (!CurrentExecutablePath(executable, error)) {
        return false;
    }

    const std::wstring quotedExecutable = L"\"" + executable + L"\"";
    const std::wstring openCommand = quotedExecutable + L" \"%1\"";
    const std::wstring icon = executable + L",0";
    const std::wstring classes = L"Software\\Classes\\";
    const std::wstring progId = classes + kMediaProgId;
    const std::wstring application =
        classes + L"Applications\\MoviePlayer.exe";
    const std::wstring capabilities = L"Software\\MoviePlayer\\Capabilities";

    if (!SetRegistryString(HKEY_CURRENT_USER, progId, nullptr,
                           Localization::Text("association.media_name"), error) ||
        !SetRegistryString(HKEY_CURRENT_USER, progId + L"\\DefaultIcon", nullptr,
                           icon, error) ||
        !SetRegistryString(HKEY_CURRENT_USER,
                           progId + L"\\shell\\open\\command", nullptr,
                           openCommand, error) ||
        !SetRegistryString(HKEY_CURRENT_USER, application, L"FriendlyAppName",
                           L"MoviePlayer", error) ||
        !SetRegistryString(HKEY_CURRENT_USER, application + L"\\DefaultIcon", nullptr,
                           icon, error) ||
        !SetRegistryString(HKEY_CURRENT_USER,
                           application + L"\\shell\\open\\command", nullptr,
                           openCommand, error) ||
        !SetRegistryString(HKEY_CURRENT_USER, capabilities, L"ApplicationName",
                           L"MoviePlayer", error) ||
        !SetRegistryString(HKEY_CURRENT_USER, capabilities,
                           L"ApplicationDescription",
                           Localization::Text("association.description"), error) ||
        !SetRegistryString(HKEY_CURRENT_USER, capabilities, L"ApplicationIcon",
                           icon, error) ||
        !SetRegistryString(HKEY_CURRENT_USER,
                           L"Software\\RegisteredApplications",
                           kRegisteredApplicationName,
                           L"Software\\MoviePlayer\\Capabilities", error)) {
        return false;
    }

    for (const wchar_t* extension : SupportedVideoExtensions()) {
        if (!SetRegistryString(HKEY_CURRENT_USER,
                               capabilities + L"\\FileAssociations",
                               extension, kMediaProgId, error) ||
            !SetRegistryEmptyValue(HKEY_CURRENT_USER,
                                   classes + extension + L"\\OpenWithProgids",
                                   kMediaProgId, error) ||
            !SetRegistryString(HKEY_CURRENT_USER,
                               application + L"\\SupportedTypes",
                               extension, L"", error)) {
            return false;
        }
    }

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH,
                   nullptr, nullptr);
    return true;
}

bool OpenVideoDefaultAppsSettings(HWND owner, std::wstring& error) {
    error.clear();
    HINSTANCE result = ShellExecuteW(
        owner, L"open",
        L"ms-settings:defaultapps?registeredAppUser=MoviePlayer",
        nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        result = ShellExecuteW(owner, L"open", L"ms-settings:defaultapps",
                               nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        error = Localization::Text("error.default_apps_settings");
        return false;
    }
    return true;
}
