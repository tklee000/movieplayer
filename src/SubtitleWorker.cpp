#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <ctranslate2/models/model.h>
#include <ctranslate2/models/sequence_to_sequence.h>
#include <ctranslate2/utils.h>
#include <sentencepiece_processor.h>
#include <whisper.h>

#include "codec/audio/aac/AacLcDecoder.h"
#include "codec/container/mp4/Mp4Demuxer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("text is too long to encode as UTF-8");
    }
    const int length = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        throw std::runtime_error("failed to encode text as UTF-8");
    }
    std::string result(static_cast<size_t>(length), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), length, nullptr,
            nullptr) != length) {
        throw std::runtime_error("failed to encode text as UTF-8");
    }
    return result;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("UTF-8 text is too long to decode");
    }
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        throw std::runtime_error("model returned invalid UTF-8 text");
    }
    std::wstring result(static_cast<size_t>(length), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), length) != length) {
        throw std::runtime_error("model returned invalid UTF-8 text");
    }
    return result;
}

std::wstring CleanStatusValue(std::wstring value) {
    for (wchar_t& character : value) {
        if (character == L'\r' || character == L'\n') {
            character = L' ';
        }
    }
    return value;
}

void EnsureParentDirectory(const fs::path& path) {
    const fs::path parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code error;
        fs::create_directories(parent, error);
        if (error && !fs::is_directory(parent)) {
            throw std::runtime_error(
                "failed to create directory: " + WideToUtf8(parent.wstring()));
        }
    }
}

void ReplaceFile(const fs::path& temporary, const fs::path& destination) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (MoveFileExW(temporary.c_str(), destination.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return;
        }
        if (attempt + 1 < 20) {
            Sleep(50);
        }
    }
    throw std::runtime_error(
        "failed to replace output file: " + WideToUtf8(destination.wstring()));
}

void WriteUtf8Atomically(const fs::path& path, const std::string& content) {
    EnsureParentDirectory(path);
    fs::path temporary = path;
    temporary += L".part." + std::to_wstring(GetCurrentProcessId());
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error(
                "failed to create temporary file: " +
                WideToUtf8(temporary.wstring()));
        }
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        output.flush();
        if (!output) {
            throw std::runtime_error(
                "failed to write temporary file: " +
                WideToUtf8(temporary.wstring()));
        }
    }
    ReplaceFile(temporary, path);
}

class RunLog final {
public:
    explicit RunLog(fs::path path) : path_(std::move(path)) {
        EnsureParentDirectory(path_);
    }

    void Write(const std::string& message) const {
        std::ofstream output(path_, std::ios::binary | std::ios::app);
        if (!output) {
            return;
        }
        SYSTEMTIME time = {};
        GetLocalTime(&time);
        output << std::setfill('0') << '[' << std::setw(2) << time.wHour << ':'
               << std::setw(2) << time.wMinute << ':' << std::setw(2)
               << time.wSecond << "] " << message << "\r\n";
    }

private:
    fs::path path_;
};

class StatusWriter final {
public:
    explicit StatusWriter(fs::path path) : path_(std::move(path)) {}

    void Update(double progress,
                const std::wstring& message,
                const std::wstring& output = {},
                const std::wstring& error = {},
                bool finished = false) {
        std::wostringstream wide;
        wide.imbue(std::locale::classic());
        wide << L"progress=" << std::fixed << std::setprecision(1)
             << std::clamp(progress, 0.0, 100.0) << L"\nmessage="
             << CleanStatusValue(message) << L"\n";
        if (!output.empty()) {
            wide << L"output=" << CleanStatusValue(output) << L"\n";
        }
        if (!error.empty()) {
            wide << L"error=" << CleanStatusValue(error) << L"\n";
        }
        wide << L"finished=" << (finished ? L"true" : L"false") << L"\n";
        WriteUtf8Atomically(path_, WideToUtf8(wide.str()));
    }

private:
    fs::path path_;
};

struct Arguments {
    fs::path root;
    fs::path input;
    fs::path output;
    fs::path status;
    fs::path log;
    std::wstring targetLanguage;
    std::wstring workId;
};

Arguments ParseArguments(int argc, wchar_t* argv[]) {
    std::unordered_map<std::wstring, std::wstring> values;
    for (int index = 1; index < argc; ++index) {
        const std::wstring key = argv[index];
        if (key.rfind(L"--", 0) != 0 || index + 1 >= argc) {
            throw std::runtime_error("invalid subtitle worker command line");
        }
        values[key] = argv[++index];
    }
    const auto require = [&values](const wchar_t* key) -> std::wstring {
        const auto found = values.find(key);
        if (found == values.end() || found->second.empty()) {
            throw std::runtime_error(
                "missing required option: " + WideToUtf8(key));
        }
        return found->second;
    };

    Arguments result;
    result.root = fs::absolute(require(L"--root"));
    result.input = fs::absolute(require(L"--input"));
    result.output = fs::absolute(require(L"--output"));
    result.status = fs::absolute(require(L"--status"));
    result.log = fs::absolute(require(L"--log"));
    result.targetLanguage = require(L"--target-language");
    result.workId = require(L"--work-id");
    if (!std::all_of(result.workId.begin(), result.workId.end(), [](wchar_t value) {
            return (value >= L'a' && value <= L'z') ||
                   (value >= L'A' && value <= L'Z') ||
                   (value >= L'0' && value <= L'9') || value == L'-' ||
                   value == L'_' || value == L'.';
        })) {
        throw std::runtime_error("invalid internal work identifier");
    }
    return result;
}

class Fir48kTo16k final {
public:
    Fir48kTo16k() {
        constexpr double kPi = 3.14159265358979323846;
        constexpr double kCutoff = 7600.0 / 48000.0;
        constexpr int kCenter = static_cast<int>(kTaps / 2);
        double sum = 0.0;
        for (std::size_t i = 0; i < kTaps; ++i) {
            const int distance = static_cast<int>(i) - kCenter;
            const double sinc = distance == 0
                                    ? 2.0 * kCutoff
                                    : std::sin(2.0 * kPi * kCutoff * distance) /
                                          (kPi * distance);
            const double window = 0.5 - 0.5 *
                std::cos(2.0 * kPi * static_cast<double>(i) /
                         static_cast<double>(kTaps - 1));
            coefficients_[i] = static_cast<float>(sinc * window);
            sum += coefficients_[i];
        }
        for (float& coefficient : coefficients_) {
            coefficient = static_cast<float>(coefficient / sum);
        }
    }

    void Push(float value, std::vector<float>& output) {
        history_[cursor_] = value;
        cursor_ = (cursor_ + 1) % kTaps;
        if (phase_ == 0) {
            float filtered = 0.0F;
            for (std::size_t i = 0; i < kTaps; ++i) {
                filtered += coefficients_[i] * history_[(cursor_ + i) % kTaps];
            }
            output.push_back(std::clamp(filtered, -1.0F, 1.0F));
        }
        phase_ = (phase_ + 1) % 3;
    }

private:
    static constexpr std::size_t kTaps = 63;
    std::array<float, kTaps> coefficients_{};
    std::array<float, kTaps> history_{};
    std::size_t cursor_ = 0;
    unsigned phase_ = 0;
};

std::vector<float> DecodeAacToWhisperPcm(const fs::path& path,
                                         StatusWriter& status,
                                         const RunLog& log) {
    using namespace movieplayer::codec;
    mp4::Mp4Demuxer demuxer;
    if (!demuxer.Open(path.wstring())) {
        throw std::runtime_error("MP4 parser: " + WideToUtf8(demuxer.LastError()));
    }

    const TrackInfo* audioTrack = nullptr;
    for (const TrackInfo& track : demuxer.Tracks()) {
        if (!audioTrack && track.codec == CodecId::Aac) audioTrack = &track;
    }
    if (!audioTrack) {
        throw std::runtime_error("the MP4 file has no supported AAC audio track");
    }
    for (const TrackInfo& track : demuxer.Tracks()) {
        if (!demuxer.SetTrackEnabled(track.trackId,
                                     track.trackId == audioTrack->trackId)) {
            throw std::runtime_error("MP4 track selection failed");
        }
    }

    aac::AacLcDecoder decoder;
    if (!decoder.Initialize(*audioTrack)) {
        throw std::runtime_error("AAC decoder: " + WideToUtf8(decoder.LastError()));
    }
    if (audioTrack->sampleRate != 48000) {
        throw std::runtime_error(
            "subtitle speech recognition currently requires 48 kHz AAC audio");
    }

    std::vector<float> result;
    const double duration = demuxer.DurationSeconds();
    if (duration > 0.0 && duration < 24.0 * 60.0 * 60.0) {
        result.reserve(static_cast<std::size_t>(duration * 16000.0) + 16000U);
    }
    Fir48kTo16k resampler;
    std::uint64_t decodedFrames = 0;
    for (;;) {
        EncodedSample sample;
        bool endOfFile = false;
        if (!demuxer.ReadNextSample(sample, endOfFile)) {
            throw std::runtime_error("MP4 read: " + WideToUtf8(demuxer.LastError()));
        }
        if (endOfFile) break;
        AudioFrame frame;
        if (!decoder.Decode(sample, frame)) {
            throw std::runtime_error("AAC decode: " + WideToUtf8(decoder.LastError()));
        }
        if (frame.sampleRate != 48000 || frame.channels != 2 ||
            (frame.samples.size() & 1U) != 0) {
            throw std::runtime_error("AAC decoder returned an invalid PCM frame");
        }
        for (std::size_t i = 0; i < frame.samples.size(); i += 2) {
            resampler.Push((frame.samples[i] + frame.samples[i + 1]) * 0.5F,
                           result);
        }
        if ((++decodedFrames & 255U) == 0U && duration > 0.0) {
            const double progress = 2.0 +
                std::clamp(sample.PtsSeconds() / duration, 0.0, 1.0) * 6.0;
            status.Update(progress, L"Decoding AAC audio to 16 kHz mono PCM");
        }
    }
    if (result.empty()) throw std::runtime_error("the AAC audio track is empty");
    log.Write("Decoded AAC with the built-in decoder: " +
              std::to_string(decodedFrames) + " frames, " +
              std::to_string(result.size()) + " mono samples");
    return result;
}

std::string TrimText(std::string text) {
    const auto whitespace = [](unsigned char value) {
        return value == ' ' || value == '\t' || value == '\r' || value == '\n';
    };
    while (!text.empty() && whitespace(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    while (!text.empty() && whitespace(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    return text;
}

struct Cue {
    double start = 0.0;
    double end = 0.0;
    std::string text;
};

struct WhisperProgress {
    StatusWriter* status = nullptr;
};

void WhisperProgressCallback(whisper_context*, whisper_state*, int progress,
                             void* userData) {
    auto* state = static_cast<WhisperProgress*>(userData);
    if (state && state->status) {
        state->status->Update(
            8.0 + static_cast<double>(progress) * 0.58,
            L"Recognizing speech with whisper.cpp");
    }
}

std::pair<std::vector<Cue>, std::string> Transcribe(
    const fs::path& modelPath,
    const std::vector<float>& samples,
    StatusWriter& status,
    const RunLog& log) {
    whisper_context_params contextParams = whisper_context_default_params();
    contextParams.use_gpu = false;
    std::unique_ptr<whisper_context, decltype(&whisper_free)> context(
        whisper_init_from_file_with_params(
            WideToUtf8(modelPath.wstring()).c_str(), contextParams),
        &whisper_free);
    if (!context) {
        throw std::runtime_error("whisper.cpp could not load its GGML model");
    }
    log.Write("Loaded whisper.cpp model on CPU: " +
              WideToUtf8(modelPath.wstring()));

    whisper_full_params params =
        whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    const unsigned int hardwareThreads = std::thread::hardware_concurrency();
    params.n_threads = static_cast<int>(
        std::max(1U, std::min(8U, hardwareThreads == 0 ? 4U : hardwareThreads)));
    params.translate = false;
    params.no_context = false;
    params.no_timestamps = false;
    params.single_segment = false;
    params.print_special = false;
    params.print_progress = false;
    params.print_realtime = false;
    params.print_timestamps = false;
    params.language = "auto";
    // "auto" detects the language and continues transcription.  The separate
    // detect_language flag intentionally exits after detection only.
    params.detect_language = false;
    params.greedy.best_of = 2;
    WhisperProgress progress{&status};
    params.progress_callback = &WhisperProgressCallback;
    params.progress_callback_user_data = &progress;

    if (samples.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("audio is too long for one whisper.cpp pass");
    }
    if (whisper_full(context.get(), params, samples.data(),
                     static_cast<int>(samples.size())) != 0) {
        throw std::runtime_error("whisper.cpp transcription failed");
    }

    const int languageId = whisper_full_lang_id(context.get());
    const char* languageName = whisper_lang_str(languageId);
    const std::string language = languageName ? languageName : "und";
    std::vector<Cue> cues;
    const int count = whisper_full_n_segments(context.get());
    cues.reserve(static_cast<size_t>(std::max(0, count)));
    for (int index = 0; index < count; ++index) {
        const char* segmentText =
            whisper_full_get_segment_text(context.get(), index);
        std::string text = TrimText(segmentText ? segmentText : "");
        if (text.empty()) {
            continue;
        }
        const double start = static_cast<double>(
                                 whisper_full_get_segment_t0(context.get(), index)) /
                             100.0;
        const double end = static_cast<double>(
                               whisper_full_get_segment_t1(context.get(), index)) /
                           100.0;
        cues.push_back({start, std::max(start + 0.15, end), std::move(text)});
    }
    if (cues.empty()) {
        throw std::runtime_error("whisper.cpp found no speech in the media");
    }
    log.Write("whisper.cpp detected language=" + language +
              " segments=" + std::to_string(cues.size()));
    return {std::move(cues), language};
}

std::string BaseLanguage(std::string language) {
    std::transform(language.begin(), language.end(), language.begin(),
                   [](unsigned char value) {
                       if (value == '_') return '-';
                       return static_cast<char>(std::tolower(value));
                   });
    if (language.rfind("zh", 0) == 0) {
        return "zh";
    }
    const size_t separator = language.find('-');
    return language.substr(0, separator);
}

std::string ValidateTargetLanguage(const std::wstring& value) {
    const std::string requested = WideToUtf8(value);
    const std::string normalized = BaseLanguage(requested);
    static const std::vector<std::string> supported = {
        "en", "ja", "ko", "fr", "zh", "es", "pt", "hi", "id", "ar",
    };
    if (std::find(supported.begin(), supported.end(), normalized) ==
        supported.end()) {
        throw std::runtime_error("unsupported subtitle target language: " +
                                 requested);
    }
    if (normalized == "zh") {
        std::string lower = requested;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char value) {
                           return static_cast<char>(std::tolower(value));
                       });
        return lower == "zh-tw" || lower == "zh_tw" ? "zh-TW" : "zh-CN";
    }
    return normalized;
}

std::vector<std::string> Translate(
    const std::vector<Cue>& cues,
    const fs::path& modelDirectory,
    const std::string& sourceLanguage,
    const std::string& targetLanguage,
    StatusWriter& status,
    const RunLog& log) {
    const fs::path sentencepiecePath =
        modelDirectory / L"sentencepiece.bpe.model";
    sentencepiece::SentencePieceProcessor tokenizer;
    const auto tokenizerStatus =
        tokenizer.Load(WideToUtf8(sentencepiecePath.wstring()));
    if (!tokenizerStatus.ok()) {
        throw std::runtime_error(
            "failed to load the M2M100 SentencePiece tokenizer: " +
            tokenizerStatus.ToString());
    }

    const std::string sourceTag = "__" + BaseLanguage(sourceLanguage) + "__";
    const std::string targetTag = "__" + BaseLanguage(targetLanguage) + "__";
    log.Write("Loading native CTranslate2 M2M100 " + sourceTag + " -> " +
              targetTag + " on CPU");
    ctranslate2::set_num_threads(0);
    const auto model = ctranslate2::models::Model::load(
        WideToUtf8(modelDirectory.wstring()), ctranslate2::Device::CPU, 0,
        ctranslate2::ComputeType::INT8);
    auto translator = model->as_sequence_to_sequence();
    if (!translator) {
        throw std::runtime_error("M2M100 is not a sequence-to-sequence model");
    }

    ctranslate2::TranslationOptions options;
    options.beam_size = 4;
    options.max_decoding_length = 192;
    options.repetition_penalty = 1.1F;
    options.no_repeat_ngram_size = 3;
    options.disable_unk = true;

    std::vector<std::string> translated;
    translated.reserve(cues.size());
    constexpr size_t batchSize = 8;
    for (size_t begin = 0; begin < cues.size(); begin += batchSize) {
        const size_t end = std::min(cues.size(), begin + batchSize);
        std::vector<std::vector<std::string>> sourceTokens;
        std::vector<std::vector<std::string>> targetPrefixes;
        sourceTokens.reserve(end - begin);
        targetPrefixes.reserve(end - begin);
        for (size_t index = begin; index < end; ++index) {
            std::vector<std::string> pieces;
            const auto encodeStatus = tokenizer.Encode(cues[index].text, &pieces);
            if (!encodeStatus.ok()) {
                throw std::runtime_error("M2M100 tokenization failed: " +
                                         encodeStatus.ToString());
            }
            pieces.insert(pieces.begin(), sourceTag);
            pieces.emplace_back("</s>");
            sourceTokens.emplace_back(std::move(pieces));
            targetPrefixes.push_back({targetTag});
        }
        const auto results = translator->translate(
            sourceTokens, targetPrefixes, options);
        if (results.size() != sourceTokens.size()) {
            throw std::runtime_error("CTranslate2 returned an incomplete batch");
        }
        for (size_t index = 0; index < results.size(); ++index) {
            std::vector<std::string> pieces;
            for (const auto& token : results[index].output()) {
                if (token != targetTag && token != "</s>" && token != "<s>") {
                    pieces.push_back(token);
                }
            }
            std::string text;
            const auto decodeStatus = tokenizer.Decode(pieces, &text);
            if (!decodeStatus.ok()) {
                throw std::runtime_error("M2M100 detokenization failed: " +
                                         decodeStatus.ToString());
            }
            text = TrimText(text);
            translated.push_back(
                text.empty() ? cues[begin + index].text : std::move(text));
        }
        const double fraction =
            static_cast<double>(translated.size()) / cues.size();
        status.Update(68.0 + fraction * 27.0,
                      L"Translating subtitles with native M2M100");
    }
    return translated;
}

bool JapaneseNativeModelReady(const fs::path& modelDirectory) {
    for (const wchar_t* file : {L"model.bin", L"config.json", L"source.spm",
                                L"target.spm"}) {
        if (!fs::is_regular_file(modelDirectory / file) ||
            fs::file_size(modelDirectory / file) == 0) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> TranslateJapaneseNative(
    const std::vector<Cue>& cues,
    const fs::path& modelDirectory,
    StatusWriter& status,
    const RunLog& log) {
    sentencepiece::SentencePieceProcessor sourceTokenizer;
    sentencepiece::SentencePieceProcessor targetTokenizer;
    auto tokenizerStatus = sourceTokenizer.Load(
        WideToUtf8((modelDirectory / L"source.spm").wstring()));
    if (!tokenizerStatus.ok()) {
        throw std::runtime_error(
            "failed to load optional Japanese source tokenizer: " +
            tokenizerStatus.ToString());
    }
    tokenizerStatus = targetTokenizer.Load(
        WideToUtf8((modelDirectory / L"target.spm").wstring()));
    if (!tokenizerStatus.ok()) {
        throw std::runtime_error(
            "failed to load optional Korean target tokenizer: " +
            tokenizerStatus.ToString());
    }

    log.Write("Loading manually supplied native Japanese-to-Korean model on CPU");
    status.Update(68.0,
                  L"Loading optional native Japanese-to-Korean model");
    ctranslate2::set_num_threads(0);
    const auto model = ctranslate2::models::Model::load(
        WideToUtf8(modelDirectory.wstring()), ctranslate2::Device::CPU, 0,
        ctranslate2::ComputeType::INT8);
    auto translator = model->as_sequence_to_sequence();
    if (!translator) {
        throw std::runtime_error(
            "optional Japanese model is not sequence-to-sequence");
    }

    ctranslate2::TranslationOptions options;
    options.beam_size = 4;
    options.max_decoding_length = 192;
    options.repetition_penalty = 1.1F;
    options.no_repeat_ngram_size = 3;
    options.disable_unk = true;

    std::vector<std::string> translated;
    translated.reserve(cues.size());
    constexpr size_t batchSize = 8;
    for (size_t begin = 0; begin < cues.size(); begin += batchSize) {
        const size_t end = std::min(cues.size(), begin + batchSize);
        std::vector<std::vector<std::string>> sourceTokens;
        sourceTokens.reserve(end - begin);
        for (size_t index = begin; index < end; ++index) {
            std::vector<std::string> pieces;
            const auto encodeStatus =
                sourceTokenizer.Encode(cues[index].text, &pieces);
            if (!encodeStatus.ok()) {
                throw std::runtime_error(
                    "optional Japanese tokenization failed: " +
                    encodeStatus.ToString());
            }
            sourceTokens.emplace_back(std::move(pieces));
        }
        const auto results = translator->translate(sourceTokens, {}, options);
        if (results.size() != sourceTokens.size()) {
            throw std::runtime_error(
                "optional Japanese model returned an incomplete batch");
        }
        for (size_t index = 0; index < results.size(); ++index) {
            std::vector<std::string> pieces;
            for (const auto& token : results[index].output()) {
                if (token != "</s>" && token != "<s>") {
                    pieces.push_back(token);
                }
            }
            std::string text;
            const auto decodeStatus = targetTokenizer.Decode(pieces, &text);
            if (!decodeStatus.ok()) {
                throw std::runtime_error(
                    "optional Korean detokenization failed: " +
                    decodeStatus.ToString());
            }
            text = TrimText(text);
            translated.push_back(
                text.empty() ? cues[begin + index].text : std::move(text));
        }
        const double fraction =
            static_cast<double>(translated.size()) / cues.size();
        status.Update(68.0 + fraction * 27.0,
                      L"Translating with optional Japanese-to-Korean model");
    }
    return translated;
}

std::string ToTraditionalChinese(const std::string& text) {
    const std::wstring source = Utf8ToWide(text);
    if (source.empty()) {
        return text;
    }
    const int required = LCMapStringEx(
        L"zh-TW", LCMAP_TRADITIONAL_CHINESE, source.data(),
        static_cast<int>(source.size()), nullptr, 0, nullptr, nullptr, 0);
    if (required <= 0) {
        throw std::runtime_error("Traditional Chinese conversion failed");
    }
    std::wstring output(static_cast<size_t>(required), L'\0');
    if (LCMapStringEx(
            L"zh-TW", LCMAP_TRADITIONAL_CHINESE, source.data(),
            static_cast<int>(source.size()), output.data(), required, nullptr,
            nullptr, 0) != required) {
        throw std::runtime_error("Traditional Chinese conversion failed");
    }
    return WideToUtf8(output);
}

std::string WrapTwoLines(const std::string& text, size_t width) {
    std::wstring value = Utf8ToWide(text);
    for (wchar_t& character : value) {
        if (character == L'\r' || character == L'\n' || character == L'\t') {
            character = L' ';
        }
    }
    if (value.size() <= width) {
        return WideToUtf8(value);
    }
    const size_t midpoint = value.size() / 2;
    size_t split = std::wstring::npos;
    size_t distance = value.size();
    for (size_t index = 0; index < value.size(); ++index) {
        if (value[index] == L' ') {
            const size_t current = index > midpoint ? index - midpoint
                                                     : midpoint - index;
            if (current < distance) {
                distance = current;
                split = index;
            }
        }
    }
    if (split == std::wstring::npos) {
        split = std::min(width, (value.size() + 1) / 2);
    }
    std::wstring first = value.substr(0, split);
    std::wstring second = value.substr(
        split + (split < value.size() && value[split] == L' ' ? 1 : 0));
    while (!first.empty() && first.back() == L' ') first.pop_back();
    while (!second.empty() && second.front() == L' ') second.erase(second.begin());
    return WideToUtf8(first + (second.empty() ? L"" : L"\n" + second));
}

std::string FormatSrtTime(double seconds) {
    const auto milliseconds = static_cast<std::uint64_t>(
        std::max(0.0, std::round(seconds * 1000.0)));
    const std::uint64_t hours = milliseconds / 3600000ULL;
    const std::uint64_t minutes = (milliseconds / 60000ULL) % 60ULL;
    const std::uint64_t wholeSeconds = (milliseconds / 1000ULL) % 60ULL;
    const std::uint64_t remainder = milliseconds % 1000ULL;
    std::ostringstream output;
    output << std::setfill('0') << std::setw(2) << hours << ':' << std::setw(2)
           << minutes << ':' << std::setw(2) << wholeSeconds << ','
           << std::setw(3) << remainder;
    return output.str();
}

size_t DefaultLineWidth(const std::string& language) {
    const std::string base = BaseLanguage(language);
    if (base == "ja" || base == "ko" || base == "zh") return 22;
    if (base == "hi" || base == "ar") return 36;
    return 42;
}

void WriteSrt(const fs::path& output,
              const std::vector<Cue>& cues,
              const std::vector<std::string>& texts,
              const std::string& language) {
    if (cues.size() != texts.size()) {
        throw std::runtime_error("subtitle cue/text count mismatch");
    }
    std::ostringstream content;
    for (size_t index = 0; index < cues.size(); ++index) {
        content << index + 1 << "\r\n" << FormatSrtTime(cues[index].start)
                << " --> " << FormatSrtTime(cues[index].end) << "\r\n"
                << WrapTwoLines(texts[index], DefaultLineWidth(language))
                << "\r\n\r\n";
    }
    WriteUtf8Atomically(output, content.str());
}

bool SamePath(const fs::path& left, const fs::path& right) {
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) ==
           CSTR_EQUAL;
}

fs::path SourceSubtitlePath(const fs::path& input,
                            const std::string& sourceLanguage) {
    const std::string safe = BaseLanguage(sourceLanguage) == "zh"
                                 ? "zh-CN"
                                 : BaseLanguage(sourceLanguage);
    fs::path name = input.stem();
    name += L"." + Utf8ToWide(safe) + L".whisper.srt";
    return input.parent_path() / name;
}

int Run(const Arguments& arguments, StatusWriter& status, const RunLog& log) {
    const fs::path whisperRoot =
        arguments.root / L"third_party" / L"whisper";
    const fs::path whisperModel =
        whisperRoot / L"models" / L"ggml-large-v3-turbo.bin";
    const fs::path translationModel =
        whisperRoot / L"models" / L"translation-m2m100";
    const fs::path japaneseNativeModel =
        whisperRoot / L"models" / L"translation-ja-ko-native";
    if (!fs::is_regular_file(arguments.input)) {
        throw std::runtime_error("input media file was not found");
    }
    if (!fs::is_regular_file(whisperModel)) {
        throw std::runtime_error("whisper.cpp GGML model is missing");
    }
    for (const wchar_t* file : {L"model.bin", L"config.json",
                                L"sentencepiece.bpe.model",
                                L"shared_vocabulary.json"}) {
        if (!fs::is_regular_file(translationModel / file)) {
            throw std::runtime_error("M2M100 translation model is incomplete");
        }
    }
    const std::string targetLanguage =
        ValidateTargetLanguage(arguments.targetLanguage);
    status.Update(2.0, L"Decoding AAC audio to 16 kHz mono PCM");
    const std::vector<float> samples =
        DecodeAacToWhisperPcm(arguments.input, status, log);
    status.Update(8.0, L"Loading whisper.cpp speech recognition model");
    auto transcription =
        Transcribe(whisperModel, samples, status, log);
    std::vector<Cue>& cues = transcription.first;
    const std::string& sourceLanguage = transcription.second;

    const fs::path sourceOutput =
        SourceSubtitlePath(arguments.input, sourceLanguage);
    std::vector<std::string> sourceTexts;
    sourceTexts.reserve(cues.size());
    for (const auto& cue : cues) sourceTexts.push_back(cue.text);
    status.Update(67.0, L"Preserving the original-language transcript");
    if (!SamePath(sourceOutput, arguments.output)) {
        WriteSrt(sourceOutput, cues, sourceTexts, sourceLanguage);
    }

    std::vector<std::string> targetTexts;
    if (BaseLanguage(sourceLanguage) == BaseLanguage(targetLanguage)) {
        targetTexts = sourceTexts;
        status.Update(95.0,
                      L"Detected language matches the selected subtitle language");
    } else if (BaseLanguage(sourceLanguage) == "ja" &&
               BaseLanguage(targetLanguage) == "ko" &&
               JapaneseNativeModelReady(japaneseNativeModel)) {
        try {
            targetTexts = TranslateJapaneseNative(
                cues, japaneseNativeModel, status, log);
        } catch (const std::exception& error) {
            log.Write(std::string("Optional Japanese model failed; falling back to M2M100: ") +
                      error.what());
            status.Update(68.0,
                          L"Optional Japanese model failed; using M2M100");
            targetTexts = Translate(cues, translationModel, sourceLanguage,
                                    targetLanguage, status, log);
        }
    } else {
        targetTexts = Translate(cues, translationModel, sourceLanguage,
                                targetLanguage, status, log);
    }
    if (targetLanguage == "zh-TW") {
        for (std::string& text : targetTexts) {
            text = ToTraditionalChinese(text);
        }
    }
    status.Update(97.0, L"Writing UTF-8 SRT subtitle file");
    WriteSrt(arguments.output, cues, targetTexts, targetLanguage);
    log.Write("Completed native subtitle generation: " +
              WideToUtf8(arguments.output.wstring()));
    status.Update(100.0, L"Automatic subtitle generation completed",
                  arguments.output.wstring(), {}, true);
    return ERROR_SUCCESS;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    std::unique_ptr<StatusWriter> status;
    std::unique_ptr<RunLog> log;
    try {
        const Arguments arguments = ParseArguments(argc, argv);
        status = std::make_unique<StatusWriter>(arguments.status);
        log = std::make_unique<RunLog>(arguments.log);
        status->Update(1.0, L"Validating native AI subtitle runtime");
        return Run(arguments, *status, *log);
    } catch (const std::exception& error) {
        if (log) {
            log->Write(std::string("FAILED: ") + error.what());
        }
        if (status) {
            try {
                const std::wstring message = Utf8ToWide(error.what());
                status->Update(0.0, L"Automatic subtitle generation failed", {},
                               message, true);
            } catch (...) {
            }
        }
        return ERROR_GEN_FAILURE;
    } catch (...) {
        if (log) {
            log->Write("FAILED: unknown native subtitle worker exception");
        }
        if (status) {
            try {
                status->Update(0.0, L"Automatic subtitle generation failed", {},
                               L"Unknown native subtitle worker error", true);
            } catch (...) {
            }
        }
        return ERROR_GEN_FAILURE;
    }
}
