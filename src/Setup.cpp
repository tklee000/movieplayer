#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>

#include <array>
#include <string>
#include <vector>

namespace {

std::wstring ApplicationDirectory() {
    std::array<wchar_t, 32768> path = {};
    const DWORD length = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return {};
    }
    std::wstring directory(path.data(), length);
    const size_t separator = directory.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        return {};
    }
    directory.resize(separator);
    return directory;
}

std::wstring JoinPath(const std::wstring& directory,
                      const std::wstring& name) {
    return directory + L"\\" + name;
}

std::wstring Quote(const std::wstring& value) {
    return L"\"" + value + L"\"";
}

bool IsFile(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring WindowsError(DWORD code) {
    wchar_t* allocated = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&allocated), 0, nullptr);
    std::wstring message = length && allocated
                               ? std::wstring(allocated, length)
                               : L"Windows error " + std::to_wstring(code);
    if (allocated) {
        LocalFree(allocated);
    }
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' ||
            message.back() == L' ')) {
        message.pop_back();
    }
    return message;
}

bool RunAndWait(const std::wstring& executable,
                const std::wstring& arguments,
                const std::wstring& workingDirectory,
                DWORD creationFlags,
                DWORD& exitCode,
                std::wstring& error) {
    std::wstring commandLine = Quote(executable);
    if (!arguments.empty()) {
        commandLine += L" " + arguments;
    }
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr,
                        nullptr, FALSE,
                        creationFlags | CREATE_UNICODE_ENVIRONMENT, nullptr,
                        workingDirectory.c_str(), &startup, &process)) {
        error = WindowsError(GetLastError());
        return false;
    }

    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, INFINITE);
    if (wait != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.hProcess, &exitCode)) {
        error = WindowsError(GetLastError());
        CloseHandle(process.hProcess);
        return false;
    }
    CloseHandle(process.hProcess);
    return true;
}

void OpenDefaultAppsSettings() {
    HINSTANCE result = ShellExecuteW(
        nullptr, L"open",
        L"ms-settings:defaultapps?registeredAppUser=MoviePlayer",
        nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps",
                      nullptr, nullptr, SW_SHOWNORMAL);
    }
}

bool AcceptJapaneseModelTerms() {
    constexpr int kAcceptButton = 1001;
    const TASKDIALOG_BUTTON buttons[] = {
        {kAcceptButton, L"동의하고 설치 (Accept)"},
        {IDCANCEL, L"동의하지 않음"},
    };
    TASKDIALOGCONFIG config = {};
    config.cbSize = sizeof(config);
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    config.pszWindowTitle = L"일본어 전용 모델 라이선스 동의";
    config.pszMainIcon = TD_WARNING_ICON;
    config.pszMainInstruction =
        L"제3자 일본어→한국어 모델 약관을 확인해 주세요.";
    config.pszContent =
        L"제공자: Hunhee/argos-ko-ja\n"
        L"고정 리비전: 15a9f14d22beefcd1cb4d45abc73f293ec2b56a8\n"
        L"제공자가 명시한 라이선스: CC BY-NC 4.0\n\n"
        L"저작자 표시와 비상업 조건이 적용됩니다. 패키지 메타데이터는 "
        L"OPUS, Wiktionary/Wiktextract 및 Stanza를 출처로 표시합니다. "
        L"상업적 용도로 사용하지 말고, 배포 전 모델 카드와 모든 상위 "
        L"조건을 직접 확인해야 합니다.\n\n"
        L"모델 카드: https://huggingface.co/Hunhee/argos-ko-ja\n"
        L"라이선스: https://creativecommons.org/licenses/by-nc/4.0/\n"
        L"동봉 고지: licenses\\AI-RUNTIME-AND-MODELS.md\n\n"
        L"'동의하고 설치 (Accept)'를 누르면 위 조건을 확인하고 선택형 "
        L"모델 다운로드를 진행하는 것으로 처리됩니다.";
    config.cButtons = ARRAYSIZE(buttons);
    config.pButtons = buttons;
    config.nDefaultButton = IDCANCEL;

    int selected = IDCANCEL;
    if (SUCCEEDED(TaskDialogIndirect(&config, &selected, nullptr, nullptr))) {
        return selected == kAcceptButton;
    }
    return MessageBoxW(
               nullptr,
               L"제공자가 이 모델을 CC BY-NC 4.0으로 명시했습니다.\n"
               L"저작자 표시 및 비상업 조건과 동봉된 라이선스 고지를 "
               L"확인하고 동의합니까?",
               L"일본어 전용 모델 라이선스 동의",
               MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) == IDYES;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, wchar_t*, int) {
    const std::wstring directory = ApplicationDirectory();
    if (directory.empty()) {
        MessageBoxW(nullptr, L"setup.exe의 실행 폴더를 확인할 수 없습니다.",
                    L"MoviePlayer 포터블 설정", MB_OK | MB_ICONERROR);
        return 1;
    }

    const std::wstring player = JoinPath(directory, L"MoviePlayer.exe");
    const std::wstring modelScript =
        JoinPath(directory, L"scripts\\setup_whisper.ps1");
    if (!IsFile(player) || !IsFile(modelScript)) {
        MessageBoxW(
            nullptr,
            L"MoviePlayer.exe 또는 AI 모델 설치 스크립트가 없습니다.\n"
            L"ZIP의 일부 파일만 옮기지 말고 전체 폴더를 다시 압축 해제해 주세요.",
            L"MoviePlayer 포터블 설정", MB_OK | MB_ICONERROR);
        return 1;
    }

    const int confirmation = MessageBoxW(
        nullptr,
        L"현재 폴더를 기준으로 MoviePlayer 포터블 설정을 진행합니다.\n\n"
        L"• MP4, MKV, AVI, TS, M2TS, MTS 형식을 현재 사용자에게 등록\n"
        L"• Whisper 및 M2M100 AI 모델 약 2.0 GiB 다운로드 및 SHA-256 검증\n"
        L"• 일본어 전용 추가 모델은 기본값에서 제외되며 이후 선택 가능\n\n"
        L"인터넷 연결과 약 2.2 GiB 이상의 여유 공간이 필요합니다.",
        L"MoviePlayer 포터블 설정", MB_OKCANCEL | MB_ICONINFORMATION);
    if (confirmation != IDOK) {
        return 0;
    }

    DWORD associationExit = 1;
    std::wstring error;
    if (!RunAndWait(player, L"--register-file-associations", directory, 0,
                    associationExit, error) ||
        associationExit != 0) {
        const std::wstring message =
            L"동영상 형식 등록에 실패했습니다.\n\n" +
            (error.empty() ? L"MoviePlayer exit code " +
                                 std::to_wstring(associationExit)
                           : error);
        MessageBoxW(nullptr, message.c_str(), L"MoviePlayer 포터블 설정",
                    MB_OK | MB_ICONERROR);
        return 1;
    }

    std::array<wchar_t, MAX_PATH> windowsDirectory = {};
    const UINT windowsLength = GetWindowsDirectoryW(
        windowsDirectory.data(), static_cast<UINT>(windowsDirectory.size()));
    if (windowsLength == 0 || windowsLength >= windowsDirectory.size()) {
        MessageBoxW(nullptr, L"Windows PowerShell 경로를 확인할 수 없습니다.",
                    L"MoviePlayer 포터블 설정", MB_OK | MB_ICONERROR);
        return 1;
    }
    const std::wstring powershell =
        std::wstring(windowsDirectory.data(), windowsLength) +
        L"\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
    const std::wstring powershellArguments =
        L"-NoLogo -NoProfile -ExecutionPolicy Bypass -File " +
        Quote(modelScript);

    DWORD modelExit = 1;
    error.clear();
    const bool modelProcessStarted = RunAndWait(
        powershell, powershellArguments, directory, CREATE_NEW_CONSOLE,
        modelExit, error);

    if (!modelProcessStarted || modelExit != 0) {
        OpenDefaultAppsSettings();
        const std::wstring message =
            L"동영상 형식은 등록했지만 AI 모델 다운로드 또는 검증에 실패했습니다.\n"
            L"인터넷 연결과 디스크 공간을 확인한 뒤 setup.exe를 다시 실행해 주세요.\n\n" +
            (error.empty() ? L"PowerShell exit code " +
                                 std::to_wstring(modelExit)
                           : error);
        MessageBoxW(nullptr, message.c_str(), L"MoviePlayer 포터블 설정",
                    MB_OK | MB_ICONWARNING);
        return 1;
    }

    bool japaneseModelInstalled = false;
    const int installJapanese = MessageBoxW(
        nullptr,
        L"선택 사항인 일본어→한국어 전용 모델도 설치하시겠습니까?\n\n"
        L"기본 번역은 이미 설치된 M2M100으로 동작합니다. 이 추가 모델은 "
        L"제공자가 CC BY-NC 4.0(저작자 표시-비상업)으로 명시했으므로 "
        L"기본 설치에서 제외되어 있습니다.",
        L"MoviePlayer 선택 모델", MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION);
    if (installJapanese == IDYES && AcceptJapaneseModelTerms()) {
        const std::wstring japaneseScript = JoinPath(
            directory, L"scripts\\setup_japanese_translation_model.ps1");
        if (!IsFile(japaneseScript)) {
            MessageBoxW(nullptr,
                        L"일본어 전용 모델 설치 스크립트가 없습니다.",
                        L"MoviePlayer 포터블 설정", MB_OK | MB_ICONERROR);
            return 1;
        }
        const std::wstring japaneseArguments =
            L"-NoLogo -NoProfile -ExecutionPolicy Bypass -File " +
            Quote(japaneseScript) + L" -AcceptThirdPartyTerms";
        DWORD japaneseExit = 1;
        error.clear();
        if (!RunAndWait(powershell, japaneseArguments, directory,
                        CREATE_NEW_CONSOLE, japaneseExit, error) ||
            japaneseExit != 0) {
            OpenDefaultAppsSettings();
            const std::wstring message =
                L"기본 설정은 완료했지만 선택한 일본어 전용 모델 설치에 "
                L"실패했습니다. install_japanese_translation_model.cmd로 "
                L"다시 시도할 수 있습니다.\n\n" +
                (error.empty() ? L"PowerShell exit code " +
                                     std::to_wstring(japaneseExit)
                               : error);
            MessageBoxW(nullptr, message.c_str(), L"MoviePlayer 포터블 설정",
                        MB_OK | MB_ICONWARNING);
            return 1;
        }
        japaneseModelInstalled = true;
    }

    OpenDefaultAppsSettings();

    const std::wstring completed =
        L"포터블 설정이 완료되었습니다.\n" +
        std::wstring(japaneseModelInstalled
                         ? L"선택한 일본어 전용 모델도 설치·검증했습니다.\n"
                         : L"일본어 전용 모델은 설치하지 않았습니다.\n") +
        L"열린 Windows 기본 앱 화면에서 원하는 동영상 확장자를 "
        L"MoviePlayer로 선택해 주세요.";
    MessageBoxW(nullptr, completed.c_str(), L"MoviePlayer 포터블 설정",
                MB_OK | MB_ICONINFORMATION);
    return 0;
}
