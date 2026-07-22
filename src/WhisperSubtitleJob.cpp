#include "WhisperSubtitleJob.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <locale>
#include <mutex>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "Localization.h"

namespace {

constexpr DWORD kCancelExitCode = ERROR_CANCELLED;
constexpr DWORD kCancelWaitMilliseconds = 5000;
constexpr size_t kMaximumStatusBytes = 1024U * 1024U;
constexpr size_t kMaximumLogTailBytes = 32U * 1024U;

class UniqueHandle final {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE value) noexcept : value_(value) {}
    ~UniqueHandle() { Reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : value_(other.Release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            Reset(other.Release());
        }
        return *this;
    }

    HANDLE Get() const noexcept { return value_; }
    explicit operator bool() const noexcept {
        return value_ && value_ != INVALID_HANDLE_VALUE;
    }

    HANDLE Release() noexcept {
        HANDLE result = value_;
        value_ = nullptr;
        return result;
    }

    void Reset(HANDLE value = nullptr) noexcept {
        if (*this) {
            CloseHandle(value_);
        }
        value_ = value;
    }

private:
    HANDLE value_ = nullptr;
};

std::wstring WindowsErrorMessage(DWORD errorCode) {
    wchar_t* allocated = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageW(
        flags, nullptr, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&allocated), 0, nullptr);
    std::wstring result;
    if (length && allocated) {
        result.assign(allocated, length);
    } else {
        std::wostringstream stream;
        stream << L"Windows error " << errorCode;
        result = stream.str();
    }
    if (allocated) {
        LocalFree(allocated);
    }
    while (!result.empty() &&
           (result.back() == L'\r' || result.back() == L'\n' ||
            result.back() == L' ' || result.back() == L'\t')) {
        result.pop_back();
    }
    return result;
}

std::wstring GetModulePath(std::wstring& error) {
    std::vector<wchar_t> buffer(1024, L'\0');
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            error = Localization::Format(
                "error.executable_path_simple",
                {{L"error", WindowsErrorMessage(GetLastError())}});
            return {};
        }
        if (length < buffer.size() - 1) {
            return std::wstring(buffer.data(), length);
        }
        if (buffer.size() >= 32768) {
            error = Localization::Text("error.executable_path_too_long");
            return {};
        }
        buffer.resize(std::min<size_t>(buffer.size() * 2, 32768), L'\0');
    }
}

std::wstring MakeAbsolutePath(const std::wstring& path, std::wstring& error) {
    if (path.empty()) {
        error = Localization::Text("error.empty_file_path");
        return {};
    }

    std::vector<wchar_t> buffer(1024, L'\0');
    for (;;) {
        const DWORD required = GetFullPathNameW(
            path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
        if (required == 0) {
            error = Localization::Format(
                "error.absolute_path",
                {{L"error", WindowsErrorMessage(GetLastError())}});
            return {};
        }
        if (required < buffer.size()) {
            return std::wstring(buffer.data(), required);
        }
        if (required >= 32768) {
            error = Localization::Text("error.file_path_too_long");
            return {};
        }
        buffer.resize(static_cast<size_t>(required) + 1, L'\0');
    }
}

std::wstring ParentPath(const std::wstring& path) {
    if (path.empty()) {
        return {};
    }
    size_t end = path.size();
    while (end > 0 && (path[end - 1] == L'\\' || path[end - 1] == L'/')) {
        --end;
    }
    const size_t separator = path.find_last_of(L"\\/", end == 0 ? 0 : end - 1);
    if (separator == std::wstring::npos) {
        return {};
    }
    if (separator == 2 && path.size() >= 3 && path[1] == L':') {
        return path.substr(0, 3);
    }
    return path.substr(0, separator);
}

std::wstring JoinPath(const std::wstring& left, const std::wstring& right) {
    if (left.empty()) {
        return right;
    }
    if (right.empty()) {
        return left;
    }
    if (left.back() == L'\\' || left.back() == L'/') {
        return left + right;
    }
    return left + L"\\" + right;
}

std::wstring ExtendedPath(const std::wstring& absolutePath) {
    if (absolutePath.rfind(L"\\\\?\\", 0) == 0) {
        return absolutePath;
    }
    if (absolutePath.rfind(L"\\\\", 0) == 0) {
        return L"\\\\?\\UNC\\" + absolutePath.substr(2);
    }
    if (absolutePath.size() >= 3 && absolutePath[1] == L':' &&
        (absolutePath[2] == L'\\' || absolutePath[2] == L'/')) {
        return L"\\\\?\\" + absolutePath;
    }
    return absolutePath;
}

bool IsRegularFile(const std::wstring& absolutePath) {
    const DWORD attributes = GetFileAttributesW(ExtendedPath(absolutePath).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool HasExpectedFileSize(const std::wstring& absolutePath,
                         std::uintmax_t expectedSize) {
    std::error_code error;
    const std::filesystem::path path(ExtendedPath(absolutePath));
    if (!std::filesystem::is_regular_file(path, error) || error) {
        return false;
    }
    return std::filesystem::file_size(path, error) == expectedSize && !error;
}

bool EnsureDirectory(const std::wstring& absolutePath, std::wstring& error) {
    std::error_code fileError;
    const std::filesystem::path path(ExtendedPath(absolutePath));
    if (std::filesystem::is_directory(path, fileError)) {
        return true;
    }
    fileError.clear();
    if (std::filesystem::create_directories(path, fileError) ||
        std::filesystem::is_directory(path, fileError)) {
        return true;
    }
    error = Localization::Format(
        "error.directory_create", {{L"path", absolutePath}});
    if (fileError) {
        const std::string narrowMessage = fileError.message();
        error += L" (" + std::wstring(narrowMessage.begin(),
                                        narrowMessage.end()) + L")";
    }
    return false;
}

std::wstring QuoteCommandLineArgument(const std::wstring& argument) {
    // CommandLineToArgvW-compatible quoting.  In particular, backslashes that
    // precede a quote (or the closing quote) must be doubled.
    std::wstring quoted;
    quoted.reserve(argument.size() + 2);
    quoted.push_back(L'"');
    size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring BuildCommandLine(const std::vector<std::wstring>& arguments) {
    std::wstring commandLine;
    for (const std::wstring& argument : arguments) {
        if (!commandLine.empty()) {
            commandLine.push_back(L' ');
        }
        commandLine += QuoteCommandLineArgument(argument);
    }
    return commandLine;
}

bool DecodeUtf8(const std::string& bytes, std::wstring& result) {
    size_t offset = 0;
    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        offset = 3;
    }
    if (offset == bytes.size()) {
        result.clear();
        return true;
    }
    const size_t remaining = bytes.size() - offset;
    if (remaining > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + offset,
        static_cast<int>(remaining), nullptr, 0);
    if (length <= 0) {
        return false;
    }
    result.resize(static_cast<size_t>(length));
    return MultiByteToWideChar(
               CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + offset,
               static_cast<int>(remaining), result.data(), length) == length;
}

bool ReadSmallFile(const std::wstring& path,
                   size_t maximumBytes,
                   std::string& bytes) {
    UniqueHandle file(CreateFileW(
        ExtendedPath(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file) {
        return false;
    }

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file.Get(), &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > maximumBytes) {
        return false;
    }
    bytes.assign(static_cast<size_t>(size.QuadPart), '\0');
    size_t offset = 0;
    while (offset < bytes.size()) {
        DWORD read = 0;
        const DWORD request = static_cast<DWORD>(std::min<size_t>(
            bytes.size() - offset, std::numeric_limits<DWORD>::max()));
        if (!ReadFile(file.Get(), &bytes[offset], request, &read, nullptr)) {
            return false;
        }
        if (read == 0) {
            return false;
        }
        offset += read;
    }
    return true;
}

std::wstring LowerAscii(std::wstring value) {
    for (wchar_t& character : value) {
        if (character >= L'A' && character <= L'Z') {
            character = static_cast<wchar_t>(character - L'A' + L'a');
        }
    }
    return value;
}

std::wstring TrimAsciiWhitespace(const std::wstring& value) {
    size_t begin = 0;
    while (begin < value.size() &&
           (value[begin] == L' ' || value[begin] == L'\t' ||
            value[begin] == L'\r' || value[begin] == L'\n')) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin &&
           (value[end - 1] == L' ' || value[end - 1] == L'\t' ||
            value[end - 1] == L'\r' || value[end - 1] == L'\n')) {
        --end;
    }
    return value.substr(begin, end - begin);
}

bool ParseBoolean(const std::wstring& value) {
    const std::wstring normalized = LowerAscii(TrimAsciiWhitespace(value));
    return normalized == L"1" || normalized == L"true" ||
           normalized == L"yes" || normalized == L"on";
}

bool ParseProgress(const std::wstring& value, double& progress) {
    const std::wstring normalized = TrimAsciiWhitespace(value);
    if (normalized.empty()) {
        return false;
    }
    wchar_t* end = nullptr;
    errno = 0;
    const double parsed = std::wcstod(normalized.c_str(), &end);
    if (end == normalized.c_str() || errno == ERANGE || !std::isfinite(parsed)) {
        return false;
    }
    while (*end == L' ' || *end == L'\t') {
        ++end;
    }
    if (*end == L'%') {
        ++end;
    }
    while (*end == L' ' || *end == L'\t') {
        ++end;
    }
    if (*end != L'\0') {
        return false;
    }
    progress = std::max(0.0, std::min(100.0, parsed));
    return true;
}

bool ParseStatus(const std::wstring& content,
                 const WhisperSubtitleJob::Update& previous,
                 WhisperSubtitleJob::Update& parsed) {
    parsed = previous;
    bool recognized = false;
    std::wistringstream stream(content);
    std::wstring line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == L'\r') {
            line.pop_back();
        }
        const size_t equals = line.find(L'=');
        if (equals == std::wstring::npos) {
            continue;
        }
        const std::wstring key = LowerAscii(
            TrimAsciiWhitespace(line.substr(0, equals)));
        const std::wstring value = line.substr(equals + 1);
        if (key == L"progress") {
            double progress = 0.0;
            if (ParseProgress(value, progress)) {
                parsed.progress = progress;
                recognized = true;
            }
        } else if (key == L"message") {
            parsed.message = value;
            recognized = true;
        } else if (key == L"output" || key == L"output_path") {
            parsed.output = value;
            recognized = true;
        } else if (key == L"error" || key == L"error_message") {
            parsed.error = value;
            recognized = true;
        } else if (key == L"finished" || key == L"done") {
            parsed.finished = ParseBoolean(value);
            recognized = true;
        }
    }
    return recognized;
}

std::wstring ReadUtf8LogTail(const std::wstring& path) {
    UniqueHandle file(CreateFileW(
        ExtendedPath(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file) {
        return {};
    }
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file.Get(), &size) || size.QuadPart <= 0) {
        return {};
    }
    const LONGLONG count = std::min<LONGLONG>(
        size.QuadPart, static_cast<LONGLONG>(kMaximumLogTailBytes));
    LARGE_INTEGER offset = {};
    offset.QuadPart = size.QuadPart - count;
    if (!SetFilePointerEx(file.Get(), offset, nullptr, FILE_BEGIN)) {
        return {};
    }
    std::string bytes(static_cast<size_t>(count), '\0');
    DWORD read = 0;
    if (!ReadFile(file.Get(), bytes.data(), static_cast<DWORD>(bytes.size()),
                  &read, nullptr)) {
        return {};
    }
    bytes.resize(read);
    // The first code point can be partial when only the tail was read.  Skip
    // UTF-8 continuation bytes until a valid code point boundary is reached.
    size_t start = 0;
    while (start < bytes.size() &&
           (static_cast<unsigned char>(bytes[start]) & 0xC0U) == 0x80U) {
        ++start;
    }
    bytes.erase(0, start);
    std::wstring result;
    if (!DecodeUtf8(bytes, result)) {
        return {};
    }
    return TrimAsciiWhitespace(result);
}

std::wstring MakeJobStem() {
    static std::atomic<unsigned long> counter{0};
    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    stream << L"whisper-" << GetCurrentProcessId() << L'-'
           << GetTickCount64() << L'-' << counter.fetch_add(1);
    return stream.str();
}

}  // namespace

struct WhisperSubtitleJob::Impl {
    mutable std::mutex mutex;
    UniqueHandle process;
    UniqueHandle primaryThread;
    UniqueHandle job;
    bool running = false;
    bool cancellationUpdatePending = false;
    std::wstring requestedOutput;
    std::wstring statusPath;
    std::wstring logPath;
    std::string lastStatusBytes;
    Update lastUpdate;

    void CloseHandlesUnlocked() {
        primaryThread.Reset();
        process.Reset();
        job.Reset();
        running = false;
    }

    void CancelUnlocked(bool reportCancellation) {
        if (process) {
            const DWORD waitResult = WaitForSingleObject(process.Get(), 0);
            if (waitResult == WAIT_TIMEOUT) {
                if (job) {
                    TerminateJobObject(job.Get(), kCancelExitCode);
                } else {
                    TerminateProcess(process.Get(), kCancelExitCode);
                }
                if (WaitForSingleObject(process.Get(), kCancelWaitMilliseconds) ==
                    WAIT_TIMEOUT) {
                    TerminateProcess(process.Get(), kCancelExitCode);
                    WaitForSingleObject(process.Get(), 1000);
                }
            }
        }
        CloseHandlesUnlocked();
        if (reportCancellation) {
            lastUpdate.finished = true;
            lastUpdate.error = Localization::Text("error.whisper_canceled");
            lastUpdate.message = Localization::Text("status.canceled");
            cancellationUpdatePending = true;
        }
    }

    bool ReadStatusUnlocked(Update& parsed) {
        if (statusPath.empty()) {
            return false;
        }
        std::string bytes;
        if (!ReadSmallFile(statusPath, kMaximumStatusBytes, bytes) ||
            bytes == lastStatusBytes) {
            return false;
        }
        std::wstring content;
        if (!DecodeUtf8(bytes, content)) {
            // The worker may currently be replacing a non-atomic status file.
            // Leave the previous bytes untouched so a later Poll retries it.
            return false;
        }
        Update next;
        if (!ParseStatus(content, lastUpdate, next)) {
            return false;
        }
        lastStatusBytes = std::move(bytes);
        lastUpdate = std::move(next);
        parsed = lastUpdate;
        return true;
    }
};

WhisperSubtitleJob::WhisperSubtitleJob()
    : impl_(std::make_unique<Impl>()) {}

WhisperSubtitleJob::~WhisperSubtitleJob() {
    Cancel();
}

WhisperSubtitleJob::RuntimeInfo WhisperSubtitleJob::InspectRuntime() {
    RuntimeInfo info;
    std::wstring pathError;
    const std::wstring modulePath = GetModulePath(pathError);
    if (modulePath.empty()) {
        info.error = pathError;
        return info;
    }

    const std::wstring moduleDirectory = ParentPath(modulePath);
    const std::wstring nativeWorker = JoinPath(
        moduleDirectory, L"MoviePlayerSubtitleWorker.exe");
    std::wstring candidate = moduleDirectory;
    for (int depth = 0; depth < 6 && !candidate.empty(); ++depth) {
        if (IsRegularFile(JoinPath(candidate, L"install_ai_models.cmd"))) {
            info.rootPath = candidate;
            break;
        }
        const std::wstring parent = ParentPath(candidate);
        if (parent == candidate) {
            break;
        }
        candidate = parent;
    }

    if (info.rootPath.empty()) {
        info.error = Localization::Text("error.whisper_worker_directory");
        return info;
    }

    info.installerPath = JoinPath(info.rootPath, L"install_ai_models.cmd");
    const std::wstring whisperRoot = JoinPath(
        info.rootPath, L"third_party\\whisper");
    const std::vector<std::pair<std::wstring, std::wstring>> requiredFiles = {
        {nativeWorker, Localization::Text("component.whisper_worker")},
        {JoinPath(whisperRoot, L"models\\ggml-large-v3-turbo.bin"),
         Localization::Text("component.whisper_model")},
        {JoinPath(whisperRoot,
                  L"models\\translation-m2m100\\model.bin"),
         Localization::Text("component.m2m100_model")},
        {JoinPath(whisperRoot,
                  L"models\\translation-m2m100\\config.json"),
         Localization::Text("component.m2m100_config")},
        {JoinPath(whisperRoot,
                  L"models\\translation-m2m100\\sentencepiece.bpe.model"),
         Localization::Text("component.m2m100_tokenizer")},
        {JoinPath(whisperRoot,
                   L"models\\translation-m2m100\\shared_vocabulary.json"),
          Localization::Text("component.m2m100_vocabulary")},
    };
    for (const auto& required : requiredFiles) {
        if (!IsRegularFile(required.first)) {
            info.missingComponent = required.second;
            return info;
        }
    }

    const std::vector<std::tuple<std::wstring, std::uintmax_t, std::wstring>>
        sizedModels = {
            {JoinPath(whisperRoot, L"models\\ggml-large-v3-turbo.bin"),
             1624555275ULL, Localization::Text("component.whisper_model")},
            {JoinPath(whisperRoot,
                       L"models\\translation-m2m100\\model.bin"),
              490667752ULL, Localization::Text("component.m2m100_model")},
        };
    for (const auto& model : sizedModels) {
        if (!HasExpectedFileSize(std::get<0>(model), std::get<1>(model))) {
            info.missingComponent = Localization::Format(
                "component.incomplete_file", {{L"component", std::get<2>(model)}});
            return info;
        }
    }

    info.ready = true;
    return info;
}

bool WhisperSubtitleJob::Start(const std::wstring& videoPath,
                               const std::wstring& outputPath,
                               double totalDuration,
                               const std::wstring& targetLanguage,
                               std::wstring& error) {
    Cancel();
    error.clear();

    if (!std::isfinite(totalDuration) || totalDuration < 0.0) {
        error = Localization::Text("error.invalid_duration");
        return false;
    }

    const RuntimeInfo runtime = InspectRuntime();
    if (!runtime.ready) {
        error = !runtime.error.empty()
                    ? runtime.error
                    : Localization::Format(
                          "error.ai_runtime_incomplete",
                          {{L"component", runtime.missingComponent}});
        return false;
    }
    const std::wstring projectRoot = runtime.rootPath;
    std::wstring pathError;

    std::wstring moduleError;
    const std::wstring modulePath = GetModulePath(moduleError);
    if (modulePath.empty()) {
        error = moduleError;
        return false;
    }
    const std::wstring workerPath = JoinPath(
        ParentPath(modulePath), L"MoviePlayerSubtitleWorker.exe");
    const std::wstring runtimePath = JoinPath(
        projectRoot, L"third_party\\whisper\\runtime");

    if (!IsRegularFile(workerPath)) {
        error = Localization::Format(
            "error.whisper_worker_missing", {{L"path", workerPath}});
        return false;
    }

    const std::wstring absoluteVideo = MakeAbsolutePath(videoPath, pathError);
    if (absoluteVideo.empty()) {
        error = pathError;
        return false;
    }
    if (!IsRegularFile(absoluteVideo)) {
        error = Localization::Format(
            "error.whisper_video_missing", {{L"path", absoluteVideo}});
        return false;
    }
    const std::wstring absoluteOutput = MakeAbsolutePath(outputPath, pathError);
    if (absoluteOutput.empty()) {
        error = pathError;
        return false;
    }

    if (!EnsureDirectory(runtimePath, error)) {
        return false;
    }
    const std::wstring outputDirectory = ParentPath(absoluteOutput);
    if (!outputDirectory.empty() && !EnsureDirectory(outputDirectory, error)) {
        return false;
    }

    const std::wstring stem = MakeJobStem();
    const std::wstring statusPath = JoinPath(runtimePath, stem + L".status");
    const std::wstring logPath = JoinPath(runtimePath, stem + L".log");
    const std::vector<std::wstring> arguments = {
        workerPath,
        L"--root", projectRoot,
        L"--input", absoluteVideo,
        L"--output", absoluteOutput,
        L"--status", statusPath,
        L"--log", logPath,
        L"--target-language", targetLanguage,
        L"--work-id", stem,
    };
    std::wstring commandLine = BuildCommandLine(arguments);
    if (commandLine.size() >= 32767) {
        error = Localization::Text("error.whisper_command_too_long");
        return false;
    }

    UniqueHandle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) {
        error = Localization::Format(
            "error.whisper_job_create",
            {{L"error", WindowsErrorMessage(GetLastError())}});
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInformation = {};
    jobInformation.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.Get(), JobObjectExtendedLimitInformation,
                                 &jobInformation, sizeof(jobInformation))) {
        error = Localization::Format(
            "error.whisper_job_setup",
            {{L"error", WindowsErrorMessage(GetLastError())}});
        return false;
    }

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION processInformation = {};
    const DWORD creationFlags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
                                CREATE_SUSPENDED;
    if (!CreateProcessW(
            workerPath.c_str(), commandLine.data(), nullptr, nullptr, FALSE,
            creationFlags, nullptr, projectRoot.c_str(), &startup,
            &processInformation)) {
        error = Localization::Format(
            "error.whisper_worker_launch",
            {{L"error", WindowsErrorMessage(GetLastError())}});
        return false;
    }

    UniqueHandle process(processInformation.hProcess);
    UniqueHandle primaryThread(processInformation.hThread);
    if (!AssignProcessToJobObject(job.Get(), process.Get())) {
        const DWORD assignError = GetLastError();
        TerminateProcess(process.Get(), assignError);
        WaitForSingleObject(process.Get(), 1000);
        error = Localization::Format(
            "error.whisper_job_assign",
            {{L"error", WindowsErrorMessage(assignError)}});
        return false;
    }
    if (ResumeThread(primaryThread.Get()) == static_cast<DWORD>(-1)) {
        const DWORD resumeError = GetLastError();
        TerminateJobObject(job.Get(), resumeError);
        WaitForSingleObject(process.Get(), 1000);
        error = Localization::Format(
            "error.whisper_worker_start",
            {{L"error", WindowsErrorMessage(resumeError)}});
        return false;
    }

    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->process = std::move(process);
    impl_->primaryThread = std::move(primaryThread);
    impl_->job = std::move(job);
    impl_->running = true;
    impl_->cancellationUpdatePending = false;
    impl_->requestedOutput = absoluteOutput;
    impl_->statusPath = statusPath;
    impl_->logPath = logPath;
    impl_->lastStatusBytes.clear();
    impl_->lastUpdate = {};
    impl_->lastUpdate.message = Localization::Text("status.whisper_worker_starting");
    return true;
}

bool WhisperSubtitleJob::Poll(Update& update) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    bool changed = false;

    if (impl_->cancellationUpdatePending) {
        impl_->cancellationUpdatePending = false;
        update = impl_->lastUpdate;
        return true;
    }

    Update parsed;
    if (impl_->ReadStatusUnlocked(parsed)) {
        update = parsed;
        changed = true;
    }

    if (!impl_->process) {
        return changed;
    }

    const DWORD waitResult = WaitForSingleObject(impl_->process.Get(), 0);
    if (waitResult == WAIT_TIMEOUT) {
        return changed;
    }
    if (waitResult != WAIT_OBJECT_0) {
        impl_->lastUpdate.finished = true;
        impl_->lastUpdate.error = Localization::Format(
            "error.whisper_worker_status",
            {{L"error", WindowsErrorMessage(GetLastError())}});
        impl_->lastUpdate.message = Localization::Text("status.whisper_failed");
        impl_->CloseHandlesUnlocked();
        update = impl_->lastUpdate;
        return true;
    }

    // The process has exited, so any final atomic status-file replacement is
    // now complete.  Read once more before synthesizing terminal state.
    if (impl_->ReadStatusUnlocked(parsed)) {
        update = parsed;
        changed = true;
    }

    DWORD exitCode = ERROR_GEN_FAILURE;
    if (!GetExitCodeProcess(impl_->process.Get(), &exitCode)) {
        exitCode = ERROR_GEN_FAILURE;
    }
    impl_->lastUpdate.finished = true;
    if (exitCode == ERROR_SUCCESS) {
        impl_->lastUpdate.progress = 100.0;
        if (impl_->lastUpdate.output.empty() &&
            IsRegularFile(impl_->requestedOutput)) {
            impl_->lastUpdate.output = impl_->requestedOutput;
        }
        if (impl_->lastUpdate.message.empty()) {
            impl_->lastUpdate.message = Localization::Text("status.whisper_complete");
        }
    } else {
        if (impl_->lastUpdate.error.empty()) {
            impl_->lastUpdate.error = ReadUtf8LogTail(impl_->logPath);
        }
        if (impl_->lastUpdate.error.empty()) {
            impl_->lastUpdate.error = Localization::Format(
                "error.whisper_exit_code",
                {{L"code", std::to_wstring(exitCode)}});
        }
        impl_->lastUpdate.message = Localization::Text("status.whisper_failed");
    }
    impl_->CloseHandlesUnlocked();
    update = impl_->lastUpdate;
    return true;
}

bool WhisperSubtitleJob::IsRunning() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->running || !impl_->process) {
        return false;
    }
    return WaitForSingleObject(impl_->process.Get(), 0) == WAIT_TIMEOUT;
}

void WhisperSubtitleJob::Cancel() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const bool wasRunning = impl_->running && impl_->process &&
                            WaitForSingleObject(impl_->process.Get(), 0) ==
                                WAIT_TIMEOUT;
    impl_->CancelUnlocked(wasRunning);
}
