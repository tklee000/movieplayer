#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace movieplayer::codec {

struct Rational {
    std::int64_t numerator = 0;
    std::int64_t denominator = 1;

    constexpr bool IsValid() const noexcept {
        return numerator > 0 && denominator > 0;
    }

    double ToDouble(double fallback = 0.0) const noexcept {
        return denominator != 0
                   ? static_cast<double>(numerator) /
                         static_cast<double>(denominator)
                   : fallback;
    }
};

enum class TrackType {
    Unknown,
    Video,
    Audio,
    Subtitle,
};

enum class CodecId {
    Unknown,
    H264,
    Hevc,
    Mpeg4Part2,
    Aac,
    Mp3,
    SubRip,
    Ass,
    VobSub,
};

enum class ColorRange {
    Unspecified,
    Limited,
    Full,
};

enum class ColorMatrix {
    Unspecified,
    Bt601,
    Bt709,
    Bt2020NonConstant,
    Bt2020Constant,
};

enum class ColorPrimaries {
    Unspecified,
    Bt709,
    Bt2020,
};

enum class TransferCharacteristic {
    Unspecified,
    Bt709,
    Pq,
    Hlg,
};

enum class ChromaLocation {
    Unspecified,
    Left,
    TopLeft,
};

struct ColorDescription {
    ColorRange range = ColorRange::Unspecified;
    ColorMatrix matrix = ColorMatrix::Unspecified;
    ColorPrimaries primaries = ColorPrimaries::Unspecified;
    TransferCharacteristic transfer = TransferCharacteristic::Unspecified;
    ChromaLocation chromaLocation = ChromaLocation::Unspecified;
};

struct TrackInfo {
    std::uint32_t trackId = 0;
    TrackType type = TrackType::Unknown;
    CodecId codec = CodecId::Unknown;
    std::string sampleEntry;
    std::string language;
    std::string name;
    bool defaultTrack = true;
    std::uint32_t timeScale = 0;
    std::uint64_t durationTicks = 0;
    std::uint64_t sampleCount = 0;
    std::vector<std::uint8_t> codecPrivate;

    int width = 0;
    int height = 0;
    Rational sampleAspectRatio{1, 1};
    Rational frameRate{};
    ColorDescription color;

    int sampleRate = 0;
    int channels = 0;
    int bitsPerSample = 0;

    double DurationSeconds() const noexcept {
        return timeScale != 0
                   ? static_cast<double>(durationTicks) /
                         static_cast<double>(timeScale)
                   : 0.0;
    }
};

struct EncodedSample {
    std::uint32_t trackId = 0;
    TrackType type = TrackType::Unknown;
    std::vector<std::uint8_t> bytes;
    std::int64_t decodeTime = 0;
    std::int64_t presentationTime = 0;
    std::uint32_t duration = 0;
    std::uint32_t timeScale = 1;
    bool sync = false;

    double DtsSeconds() const noexcept {
        return static_cast<double>(decodeTime) / static_cast<double>(timeScale);
    }

    double PtsSeconds() const noexcept {
        return static_cast<double>(presentationTime) /
               static_cast<double>(timeScale);
    }

    double DurationSeconds() const noexcept {
        return static_cast<double>(duration) / static_cast<double>(timeScale);
    }
};

struct VideoFrame {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    UINT arraySlice = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    int width = 0;
    int height = 0;
    Rational sampleAspectRatio{1, 1};
    ColorDescription color;
    bool interlaced = false;
    bool topFieldFirst = false;
    double pts = 0.0;
    double duration = 0.0;
};

struct AudioFrame {
    std::vector<float> samples;
    int sampleRate = 0;
    int channels = 0;
    std::uint64_t channelMask = 0;
    double pts = 0.0;
};

// Premultiplied BGRA subtitle pixels positioned in the subtitle stream's
// original canvas. Bitmap subtitle formats (for example DVD VobSub) use this
// instead of converting their artwork to text.
struct SubtitleBitmap {
    int canvasWidth = 0;
    int canvasHeight = 0;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> bgra;
};

constexpr double kInvalidTimestamp = -std::numeric_limits<double>::infinity();

}  // namespace movieplayer::codec
