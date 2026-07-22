#include "codec/audio/aac/AacLcDecoder.h"

#include "codec/audio/aac/AacHuffman.h"
#include "codec/core/BitReader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <random>
#include <utility>
#include <vector>

namespace movieplayer::codec::aac {
namespace {

constexpr double kPi = 3.14159265358979323846264338327950288;
constexpr unsigned kLongLength = 1024;
constexpr unsigned kShortLength = 128;
constexpr unsigned kMaximumScaleFactorBands = 64;

enum class WindowSequence : unsigned {
    OnlyLong = 0,
    LongStart = 1,
    EightShort = 2,
    LongStop = 3,
};

enum class ChannelRole {
    Unknown,
    FrontLeft,
    FrontRight,
    FrontCenter,
    SurroundLeft,
    SurroundRight,
    BackLeft,
    BackRight,
    Lfe,
};

struct IcsInfo {
    WindowSequence sequence = WindowSequence::OnlyLong;
    unsigned windowShape = 0;
    unsigned maxSfb = 0;
    std::vector<unsigned> groupLengths{1};

    bool IsShort() const noexcept {
        return sequence == WindowSequence::EightShort;
    }
};

struct TnsFilter {
    unsigned window = 0;
    unsigned startLine = 0;
    unsigned endLine = 0;
    bool reverse = false;
    std::vector<double> reflection;
};

struct Pulse {
    unsigned line = 0;
    unsigned amplitude = 0;
};

struct IcsChannel {
    IcsInfo info;
    int globalGain = 0;
    std::array<std::array<unsigned, kMaximumScaleFactorBands>, 8> codebooks{};
    std::array<std::array<int, kMaximumScaleFactorBands>, 8> scaleFactors{};
    std::array<double, kLongLength> spectrum{};
    std::vector<TnsFilter> tns;
    std::vector<Pulse> pulses;
};

struct DecodedChannel {
    ChannelRole role = ChannelRole::Unknown;
    std::array<float, kLongLength> pcm{};
};

constexpr std::array<unsigned, 50> kLongBands48k = {
    0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 48, 56, 64, 72, 80, 88,
    96, 108, 120, 132, 144, 160, 176, 196, 216, 240, 264, 292, 320, 352,
    384, 416, 448, 480, 512, 544, 576, 608, 640, 672, 704, 736, 768,
    800, 832, 864, 896, 928, 1024};

constexpr std::array<unsigned, 15> kShortBands48k = {
    0, 4, 8, 12, 16, 20, 28, 36, 44, 56, 68, 80, 96, 112, 128};

constexpr std::array<unsigned, 48> kLongBands24k = {
    0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 52, 60, 68, 76,
    84, 92, 100, 108, 116, 124, 136, 148, 160, 172, 188, 204, 220,
    240, 260, 284, 308, 336, 364, 396, 432, 468, 508, 552, 600, 652,
    704, 768, 832, 896, 960, 1024};

constexpr std::array<unsigned, 16> kShortBands24k = {
    0, 4, 8, 12, 16, 20, 24, 28, 36, 44, 52, 64, 76, 92, 108, 128};

const unsigned* BandOffsets(const IcsInfo& info, int sampleRate) {
    if (sampleRate == 24000)
        return info.IsShort() ? kShortBands24k.data() : kLongBands24k.data();
    return info.IsShort() ? kShortBands48k.data() : kLongBands48k.data();
}

unsigned BandCount(const IcsInfo& info, int sampleRate) {
    if (sampleRate == 24000) {
        return info.IsShort()
                   ? static_cast<unsigned>(kShortBands24k.size() - 1)
                   : static_cast<unsigned>(kLongBands24k.size() - 1);
    }
    return info.IsShort()
               ? static_cast<unsigned>(kShortBands48k.size() - 1)
               : static_cast<unsigned>(kLongBands48k.size() - 1);
}

bool ReadBits(BitReader& bits, unsigned count, unsigned& value) {
    std::uint32_t result = 0;
    if (!bits.ReadBits(count, result)) return false;
    value = result;
    return true;
}

double BesselI0(double value) {
    double sum = 1.0;
    double term = 1.0;
    const double quarter = value * value * 0.25;
    for (unsigned k = 1; k < 40; ++k) {
        term *= quarter / static_cast<double>(k * k);
        sum += term;
        if (term < sum * 1e-15) break;
    }
    return sum;
}

std::vector<double> MakeSineWindow(unsigned length) {
    std::vector<double> window(length);
    for (unsigned i = 0; i < length; ++i) {
        window[i] = std::sin(kPi * (static_cast<double>(i) + 0.5) /
                             (2.0 * length));
    }
    return window;
}

std::vector<double> MakeKbdWindow(unsigned length, double alpha) {
    std::vector<double> cumulative(length);
    double sum = 0.0;
    for (unsigned i = 0; i < length; ++i) {
        const double x = (2.0 * static_cast<double>(i) /
                          static_cast<double>(length - 1)) - 1.0;
        sum += BesselI0(kPi * alpha * std::sqrt(std::max(0.0, 1.0 - x * x)));
        cumulative[i] = sum;
    }
    for (double& value : cumulative) value = std::sqrt(value / sum);
    return cumulative;
}

const std::vector<double>& Window(unsigned shape, bool shortWindow) {
    static const std::vector<double> longSine = MakeSineWindow(kLongLength);
    static const std::vector<double> shortSine = MakeSineWindow(kShortLength);
    static const std::vector<double> longKbd = MakeKbdWindow(kLongLength, 4.0);
    static const std::vector<double> shortKbd = MakeKbdWindow(kShortLength, 6.0);
    if (shortWindow) return shape ? shortKbd : shortSine;
    return shape ? longKbd : longSine;
}

void Fft(std::vector<std::complex<double>>& data) {
    const std::size_t count = data.size();
    for (std::size_t i = 1, j = 0; i < count; ++i) {
        std::size_t bit = count >> 1U;
        for (; j & bit; bit >>= 1U) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(data[i], data[j]);
    }
    for (std::size_t length = 2; length <= count; length <<= 1U) {
        const double angle = -2.0 * kPi / static_cast<double>(length);
        const std::complex<double> root(std::cos(angle), std::sin(angle));
        for (std::size_t offset = 0; offset < count; offset += length) {
            std::complex<double> factor(1.0, 0.0);
            for (std::size_t i = 0; i < length / 2; ++i) {
                const auto even = data[offset + i];
                const auto odd = data[offset + i + length / 2] * factor;
                data[offset + i] = even + odd;
                data[offset + i + length / 2] = even - odd;
                factor *= root;
            }
        }
    }
}

std::vector<double> DctIv(const double* input, unsigned count) {
    const std::size_t fftSize = static_cast<std::size_t>(count) * 8U;
    std::vector<std::complex<double>> frequency(fftSize);
    for (unsigned i = 0; i < count; ++i) {
        const std::size_t position = static_cast<std::size_t>(2U * i + 1U);
        frequency[position] = input[i];
        frequency[fftSize - position] = input[i];
    }
    Fft(frequency);
    std::vector<double> output(count);
    for (unsigned i = 0; i < count; ++i) {
        output[i] = frequency[2U * i + 1U].real() * 0.5;
    }
    return output;
}

std::vector<double> Imdct(const double* input, unsigned count) {
    const auto transformed = DctIv(input, count);
    std::vector<double> output(static_cast<std::size_t>(count) * 2U);
    const double scale = 2.0 / static_cast<double>(count);
    const unsigned half = count / 2U;
    for (unsigned i = 0; i < half; ++i) {
        output[i] = transformed[half + i] * scale;
        output[half + i] = -transformed[count - 1U - i] * scale;
        output[count + i] = -transformed[half - 1U - i] * scale;
        output[count + half + i] = -transformed[i] * scale;
    }
    return output;
}

double Dequantize(int value, int scaleFactor) {
    if (value == 0) return 0.0;
    const double magnitude = std::pow(static_cast<double>(std::abs(value)), 4.0 / 3.0);
    const double scale = std::exp2((static_cast<double>(scaleFactor) - 100.0) * 0.25);
    return std::copysign(magnitude * scale, static_cast<double>(value));
}

}  // namespace

struct AacLcDecoder::Impl {
    struct ChannelState {
        std::array<double, kLongLength> overlap{};
        unsigned previousWindowShape = 0;
    };

    TrackInfo track;
    int sampleRate = 0;
    int channelConfiguration = 0;
    std::size_t expectedChannels = 0;
    std::array<ChannelState, 8> channelStates{};
    std::uint32_t noiseState = 0x6d2b79f5U;
    float downmixLimiterGain = 1.0F;
    std::wstring description;
    std::wstring error;

    bool Fail(const std::wstring& message) {
        error = message;
        return false;
    }

    bool ParseProgramConfigElement(BitReader& bits, bool configure) {
        unsigned tag = 0;
        unsigned objectType = 0;
        unsigned frequencyIndex = 0;
        unsigned frontCount = 0;
        unsigned sideCount = 0;
        unsigned backCount = 0;
        unsigned lfeCount = 0;
        unsigned associatedCount = 0;
        unsigned couplingCount = 0;
        if (!ReadBits(bits, 4, tag) || !ReadBits(bits, 2, objectType) ||
            !ReadBits(bits, 4, frequencyIndex) ||
            !ReadBits(bits, 4, frontCount) || !ReadBits(bits, 4, sideCount) ||
            !ReadBits(bits, 4, backCount) || !ReadBits(bits, 2, lfeCount) ||
            !ReadBits(bits, 3, associatedCount) ||
            !ReadBits(bits, 4, couplingCount)) {
            return Fail(L"Truncated AAC program configuration");
        }
        (void)tag;
        (void)frequencyIndex;
        bool present = false;
        unsigned ignored = 0;
        if (!bits.ReadBit(present)) return Fail(L"Truncated AAC mono mixdown flag");
        if (present && !ReadBits(bits, 4, ignored))
            return Fail(L"Truncated AAC mono mixdown element");
        if (!bits.ReadBit(present)) return Fail(L"Truncated AAC stereo mixdown flag");
        if (present && !ReadBits(bits, 4, ignored))
            return Fail(L"Truncated AAC stereo mixdown element");
        if (!bits.ReadBit(present)) return Fail(L"Truncated AAC matrix mixdown flag");
        if (present && !ReadBits(bits, 3, ignored))
            return Fail(L"Truncated AAC matrix mixdown data");

        std::size_t channelCount = 0;
        const auto readChannelElements = [&](unsigned count) -> bool {
            for (unsigned i = 0; i < count; ++i) {
                bool pair = false;
                unsigned elementTag = 0;
                if (!bits.ReadBit(pair) || !ReadBits(bits, 4, elementTag))
                    return false;
                channelCount += pair ? 2U : 1U;
            }
            return true;
        };
        if (!readChannelElements(frontCount) || !readChannelElements(sideCount) ||
            !readChannelElements(backCount)) {
            return Fail(L"Truncated AAC program channel elements");
        }
        channelCount += lfeCount;
        for (unsigned i = 0; i < lfeCount; ++i)
            if (!ReadBits(bits, 4, ignored))
                return Fail(L"Truncated AAC program LFE elements");
        for (unsigned i = 0; i < associatedCount; ++i)
            if (!ReadBits(bits, 4, ignored))
                return Fail(L"Truncated AAC associated data elements");
        for (unsigned i = 0; i < couplingCount; ++i) {
            bool independent = false;
            if (!bits.ReadBit(independent) || !ReadBits(bits, 4, ignored))
                return Fail(L"Truncated AAC coupling channel elements");
        }
        if (!bits.ByteAlign() || !ReadBits(bits, 8, ignored) ||
            !bits.SkipBits(static_cast<std::size_t>(ignored) * 8U)) {
            return Fail(L"Truncated AAC program configuration comment");
        }
        if (configure) {
            // The PCE object type stores Audio Object Type minus one.
            if (objectType != 1 || channelCount == 0 || channelCount > 8) {
                return Fail(L"Only AAC-LC program configurations up to 7.1 are supported");
            }
            expectedChannels = channelCount;
        }
        return true;
    }

    bool ParseAudioSpecificConfig(const std::vector<std::uint8_t>& config) {
        BitReader bits(config);
        unsigned objectType = 0;
        if (!ReadBits(bits, 5, objectType)) return Fail(L"Truncated AudioSpecificConfig");
        if (objectType == 31) {
            unsigned extension = 0;
            if (!ReadBits(bits, 6, extension)) return Fail(L"Truncated AAC object type");
            objectType = 32 + extension;
        }
        unsigned frequencyIndex = 0;
        if (!ReadBits(bits, 4, frequencyIndex)) return Fail(L"Truncated AAC frequency");
        static constexpr int frequencies[13] = {
            96000, 88200, 64000, 48000, 44100, 32000, 24000,
            22050, 16000, 12000, 11025, 8000, 7350};
        if (frequencyIndex == 15) {
            unsigned explicitFrequency = 0;
            if (!ReadBits(bits, 24, explicitFrequency))
                return Fail(L"Truncated explicit AAC frequency");
            sampleRate = static_cast<int>(explicitFrequency);
        } else if (frequencyIndex < std::size(frequencies)) {
            sampleRate = frequencies[frequencyIndex];
        } else {
            return Fail(L"Invalid AAC sampling frequency index");
        }
        unsigned channels = 0;
        if (!ReadBits(bits, 4, channels)) return Fail(L"Truncated AAC channel configuration");
        channelConfiguration = static_cast<int>(channels);
        bool frameLengthFlag = false;
        bool dependsOnCoreCoder = false;
        bool extensionFlag = false;
        if (!bits.ReadBit(frameLengthFlag) || !bits.ReadBit(dependsOnCoreCoder))
            return Fail(L"Truncated GASpecificConfig");
        if (dependsOnCoreCoder && !bits.SkipBits(14))
            return Fail(L"Truncated AAC core coder delay");
        if (!bits.ReadBit(extensionFlag)) return Fail(L"Truncated AAC extension flag");
        expectedChannels = channelConfiguration == 2
                               ? std::size_t{2}
                               : (channelConfiguration == 6 ? std::size_t{6}
                                                            : std::size_t{0});
        if (channelConfiguration == 0 && !ParseProgramConfigElement(bits, true))
            return false;
        if (objectType != 2 || frameLengthFlag || extensionFlag)
            return Fail(L"The AAC stream is not a supported AAC-LC frame configuration");
        if (sampleRate != 24000 && sampleRate != 44100 && sampleRate != 48000)
            return Fail(L"Only 24, 44.1, and 48 kHz AAC-LC are supported");
        if ((channelConfiguration != 0 && channelConfiguration != 2 &&
             channelConfiguration != 6) || expectedChannels == 0)
            return Fail(L"Only AAC stereo, 5.1, and PCE 7.1 channel layouts are supported");
        return true;
    }

    bool ParseIcsInfo(BitReader& bits, IcsInfo& info) {
        bool reserved = false;
        unsigned sequence = 0;
        if (!bits.ReadBit(reserved) || reserved || !ReadBits(bits, 2, sequence) ||
            !ReadBits(bits, 1, info.windowShape)) {
            return Fail(L"Invalid AAC ICS info");
        }
        info.sequence = static_cast<WindowSequence>(sequence);
        info.groupLengths.clear();
        if (info.IsShort()) {
            if (!ReadBits(bits, 4, info.maxSfb)) return Fail(L"Truncated short max_sfb");
            unsigned grouping = 0;
            if (!ReadBits(bits, 7, grouping)) return Fail(L"Truncated scale factor grouping");
            info.groupLengths.push_back(1);
            for (int bit = 6; bit >= 0; --bit) {
                if ((grouping >> bit) & 1U) {
                    ++info.groupLengths.back();
                } else {
                    info.groupLengths.push_back(1);
                }
            }
        } else {
            if (!ReadBits(bits, 6, info.maxSfb)) return Fail(L"Truncated long max_sfb");
            info.groupLengths.push_back(1);
            bool predictorDataPresent = false;
            if (!bits.ReadBit(predictorDataPresent)) return Fail(L"Truncated predictor flag");
            if (predictorDataPresent)
                return Fail(L"AAC Main prediction is not valid in the AAC-LC path");
        }
        if (info.maxSfb > BandCount(info, sampleRate))
            return Fail(L"AAC max_sfb exceeds the sampling-frequency table");
        return true;
    }

    bool ParseSections(BitReader& bits, IcsChannel& channel) {
        const unsigned lengthBits = channel.info.IsShort() ? 3U : 5U;
        const unsigned escape = (1U << lengthBits) - 1U;
        for (unsigned group = 0; group < channel.info.groupLengths.size(); ++group) {
            unsigned band = 0;
            while (band < channel.info.maxSfb) {
                unsigned codebook = 0;
                if (!ReadBits(bits, 4, codebook) || codebook == 12)
                    return Fail(L"Invalid AAC section codebook");
                unsigned length = 0;
                unsigned increment = 0;
                do {
                    if (!ReadBits(bits, lengthBits, increment))
                        return Fail(L"Truncated AAC section length");
                    if (length > channel.info.maxSfb - band ||
                        increment > channel.info.maxSfb - band - length)
                        return Fail(L"AAC section exceeds max_sfb");
                    length += increment;
                } while (increment == escape);
                if (length == 0) return Fail(L"Zero-length AAC section");
                for (unsigned i = 0; i < length; ++i)
                    channel.codebooks[group][band + i] = codebook;
                band += length;
            }
        }
        return true;
    }

    bool ParseScaleFactors(BitReader& bits, IcsChannel& channel) {
        int scaleFactor = channel.globalGain;
        int intensityPosition = 0;
        int noiseEnergy = channel.globalGain - 90;
        bool firstNoise = true;
        for (unsigned group = 0; group < channel.info.groupLengths.size(); ++group) {
            for (unsigned band = 0; band < channel.info.maxSfb; ++band) {
                const unsigned codebook = channel.codebooks[group][band];
                if (codebook == 0) continue;
                if (codebook == 13) {
                    int difference = 0;
                    if (firstNoise) {
                        unsigned raw = 0;
                        if (!ReadBits(bits, 9, raw)) return Fail(L"Truncated AAC PNS energy");
                        difference = static_cast<int>(raw) - 256;
                        firstNoise = false;
                    } else if (!AacHuffman::DecodeScaleFactor(bits, difference)) {
                        return Fail(L"Invalid AAC PNS Huffman value");
                    }
                    noiseEnergy += difference;
                    channel.scaleFactors[group][band] = noiseEnergy;
                } else if (codebook == 14 || codebook == 15) {
                    int difference = 0;
                    if (!AacHuffman::DecodeScaleFactor(bits, difference))
                        return Fail(L"Invalid AAC intensity Huffman value");
                    intensityPosition += difference;
                    channel.scaleFactors[group][band] = intensityPosition;
                } else {
                    int difference = 0;
                    if (!AacHuffman::DecodeScaleFactor(bits, difference))
                        return Fail(L"Invalid AAC scalefactor Huffman value");
                    scaleFactor += difference;
                    if (scaleFactor < 0 || scaleFactor > 255)
                        return Fail(L"AAC scalefactor is out of range");
                    channel.scaleFactors[group][band] = scaleFactor;
                }
            }
        }
        return true;
    }

    bool ParsePulseData(BitReader& bits, IcsChannel& channel) {
        bool present = false;
        if (!bits.ReadBit(present)) return Fail(L"Truncated AAC pulse flag");
        if (!present) return true;
        if (channel.info.IsShort()) return Fail(L"Pulse data is invalid in short AAC blocks");
        unsigned pulseCountMinus1 = 0, startSfb = 0;
        const unsigned* longBands = BandOffsets(channel.info, sampleRate);
        if (!ReadBits(bits, 2, pulseCountMinus1) || !ReadBits(bits, 6, startSfb) ||
            startSfb >= BandCount(channel.info, sampleRate))
            return Fail(L"Invalid AAC pulse header");
        unsigned line = longBands[startSfb];
        for (unsigned i = 0; i <= pulseCountMinus1; ++i) {
            unsigned offset = 0, amplitude = 0;
            if (!ReadBits(bits, 5, offset) || !ReadBits(bits, 4, amplitude))
                return Fail(L"Truncated AAC pulse data");
            line += offset;
            if (line >= kLongLength) return Fail(L"AAC pulse line is out of range");
            channel.pulses.push_back({line, amplitude});
        }
        return true;
    }

    bool ParseTnsData(BitReader& bits, IcsChannel& channel) {
        bool present = false;
        if (!bits.ReadBit(present)) return Fail(L"Truncated AAC TNS flag");
        if (!present) return true;
        const bool shortBlock = channel.info.IsShort();
        const unsigned windows = shortBlock ? 8U : 1U;
        const unsigned* offsets = BandOffsets(channel.info, sampleRate);
        for (unsigned window = 0; window < windows; ++window) {
            unsigned filterCount = 0;
            if (!ReadBits(bits, shortBlock ? 1U : 2U, filterCount))
                return Fail(L"Truncated AAC TNS filter count");
            unsigned coefficientResolution = 0;
            if (filterCount && !ReadBits(bits, 1, coefficientResolution))
                return Fail(L"Truncated AAC TNS coefficient resolution");
            unsigned topBand = channel.info.maxSfb;
            for (unsigned filter = 0; filter < filterCount; ++filter) {
                unsigned length = 0, order = 0;
                if (!ReadBits(bits, shortBlock ? 4U : 6U, length) ||
                    !ReadBits(bits, shortBlock ? 3U : 5U, order) || order > 20)
                    return Fail(L"Invalid AAC TNS filter");
                const unsigned bottomBand = length > topBand ? 0 : topBand - length;
                TnsFilter decoded;
                decoded.window = window;
                decoded.startLine = offsets[bottomBand];
                decoded.endLine = offsets[topBand];
                if (order) {
                    bool direction = false, compress = false;
                    if (!bits.ReadBit(direction) || !bits.ReadBit(compress))
                        return Fail(L"Truncated AAC TNS filter flags");
                    decoded.reverse = direction;
                    const unsigned resolutionBits =
                        coefficientResolution ? 4U : 3U;
                    const unsigned coefficientBits =
                        resolutionBits - (compress ? 1U : 0U);
                    const double positiveFactor =
                        (static_cast<double>(1U << (resolutionBits - 1U)) - 0.5) /
                        (kPi * 0.5);
                    const double negativeFactor =
                        (static_cast<double>(1U << (resolutionBits - 1U)) + 0.5) /
                        (kPi * 0.5);
                    decoded.reflection.reserve(order);
                    for (unsigned i = 0; i < order; ++i) {
                        unsigned raw = 0;
                        if (!ReadBits(bits, coefficientBits, raw))
                            return Fail(L"Truncated AAC TNS coefficients");
                        const int signBit = 1 << (coefficientBits - 1U);
                        const int signedValue = (raw & signBit)
                                                    ? static_cast<int>(raw) -
                                                          (1 << coefficientBits)
                                                    : static_cast<int>(raw);
                        decoded.reflection.push_back(std::sin(
                            static_cast<double>(signedValue) /
                            (signedValue >= 0 ? positiveFactor : negativeFactor)));
                    }
                }
                channel.tns.push_back(std::move(decoded));
                topBand = bottomBand;
            }
        }
        return true;
    }

    bool ParseSpectralData(BitReader& bits, IcsChannel& channel,
                           std::array<int, kLongLength>& quantized) {
        const unsigned* offsets = BandOffsets(channel.info, sampleRate);
        unsigned firstWindow = 0;
        for (unsigned group = 0; group < channel.info.groupLengths.size(); ++group) {
            const unsigned groupLength = channel.info.groupLengths[group];
            for (unsigned band = 0; band < channel.info.maxSfb; ++band) {
                const unsigned codebook = channel.codebooks[group][band];
                const unsigned bandStart = offsets[band];
                const unsigned bandEnd = offsets[band + 1U];
                if (codebook == 0 || codebook >= 13) continue;
                for (unsigned window = 0; window < groupLength; ++window) {
                    const unsigned base = channel.info.IsShort()
                                              ? (firstWindow + window) * kShortLength
                                              : 0;
                    unsigned line = bandStart;
                    while (line < bandEnd) {
                        std::array<int, 4> values{};
                        unsigned dimension = 0;
                        if (!AacHuffman::DecodeSpectral(bits, codebook, values,
                                                        dimension) ||
                            line + dimension > bandEnd) {
                            return Fail(L"Invalid AAC spectral Huffman data");
                        }
                        for (unsigned i = 0; i < dimension; ++i)
                            quantized[base + line + i] = values[i];
                        line += dimension;
                    }
                }
            }
            firstWindow += groupLength;
        }
        for (const Pulse& pulse : channel.pulses) {
            int& value = quantized[pulse.line];
            value += value >= 0 ? static_cast<int>(pulse.amplitude)
                                : -static_cast<int>(pulse.amplitude);
        }
        return true;
    }

    double NextNoise() {
        noiseState ^= noiseState << 13U;
        noiseState ^= noiseState >> 17U;
        noiseState ^= noiseState << 5U;
        return static_cast<double>(static_cast<std::int32_t>(noiseState)) /
               static_cast<double>(std::numeric_limits<std::int32_t>::max());
    }

    void ReconstructSpectrum(IcsChannel& channel,
                             const std::array<int, kLongLength>& quantized) {
        const unsigned* offsets = BandOffsets(channel.info, sampleRate);
        unsigned firstWindow = 0;
        for (unsigned group = 0; group < channel.info.groupLengths.size(); ++group) {
            const unsigned groupLength = channel.info.groupLengths[group];
            for (unsigned band = 0; band < channel.info.maxSfb; ++band) {
                const unsigned codebook = channel.codebooks[group][band];
                const unsigned bandStart = offsets[band];
                const unsigned bandEnd = offsets[band + 1U];
                for (unsigned window = 0; window < groupLength; ++window) {
                    const unsigned base = channel.info.IsShort()
                                              ? (firstWindow + window) * kShortLength
                                              : 0;
                    if (codebook == 13) {
                        double energy = 0.0;
                        for (unsigned line = bandStart; line < bandEnd; ++line) {
                            const double value = NextNoise();
                            channel.spectrum[base + line] = value;
                            energy += value * value;
                        }
                        const double target = std::exp2(
                            (channel.scaleFactors[group][band] - 100.0) * 0.25);
                        const double scale = energy > 0.0 ? target / std::sqrt(energy) : 0.0;
                        for (unsigned line = bandStart; line < bandEnd; ++line)
                            channel.spectrum[base + line] *= scale;
                    } else if (codebook > 0 && codebook < 12) {
                        for (unsigned line = bandStart; line < bandEnd; ++line) {
                            channel.spectrum[base + line] = Dequantize(
                                quantized[base + line],
                                channel.scaleFactors[group][band]);
                        }
                    }
                }
            }
            firstWindow += groupLength;
        }
    }

    bool ParseIndividualChannel(BitReader& bits, const IcsInfo* commonInfo,
                                IcsChannel& channel) {
        unsigned gain = 0;
        if (!ReadBits(bits, 8, gain)) return Fail(L"Truncated AAC global gain");
        channel.globalGain = static_cast<int>(gain);
        if (commonInfo) channel.info = *commonInfo;
        else if (!ParseIcsInfo(bits, channel.info)) return false;
        if (!ParseSections(bits, channel) || !ParseScaleFactors(bits, channel) ||
            !ParsePulseData(bits, channel) || !ParseTnsData(bits, channel))
            return false;
        bool gainControl = false;
        if (!bits.ReadBit(gainControl)) return Fail(L"Truncated AAC gain control flag");
        if (gainControl) return Fail(L"AAC gain control is not supported in AAC-LC");
        std::array<int, kLongLength> quantized{};
        if (!ParseSpectralData(bits, channel, quantized)) return false;
        ReconstructSpectrum(channel, quantized);
        return true;
    }

    void ApplyStereo(IcsChannel& left, IcsChannel& right,
                     const std::array<std::array<bool, kMaximumScaleFactorBands>, 8>&
                         msMask) {
        const unsigned* offsets = BandOffsets(left.info, sampleRate);
        unsigned firstWindow = 0;
        for (unsigned group = 0; group < left.info.groupLengths.size(); ++group) {
            const unsigned groupLength = left.info.groupLengths[group];
            for (unsigned band = 0; band < left.info.maxSfb; ++band) {
                const unsigned rightBook = right.codebooks[group][band];
                for (unsigned window = 0; window < groupLength; ++window) {
                    const unsigned base = left.info.IsShort()
                                              ? (firstWindow + window) * kShortLength
                                              : 0;
                    if (rightBook == 14 || rightBook == 15) {
                        double scale = std::exp2(
                            -0.25 * right.scaleFactors[group][band]);
                        if (rightBook == 14) scale = -scale;
                        if (msMask[group][band]) scale = -scale;
                        for (unsigned line = offsets[band];
                             line < offsets[band + 1U]; ++line)
                            right.spectrum[base + line] =
                                left.spectrum[base + line] * scale;
                    } else if (msMask[group][band] && rightBook != 13) {
                        for (unsigned line = offsets[band];
                             line < offsets[band + 1U]; ++line) {
                            const double middle = left.spectrum[base + line];
                            const double side = right.spectrum[base + line];
                            left.spectrum[base + line] = middle + side;
                            right.spectrum[base + line] = middle - side;
                        }
                    }
                }
            }
            firstWindow += groupLength;
        }
    }

    void ApplyTns(IcsChannel& channel) {
        for (const TnsFilter& filter : channel.tns) {
            if (filter.reflection.empty() || filter.endLine <= filter.startLine)
                continue;
            std::vector<double> lpc(filter.reflection.size() + 1, 0.0);
            lpc[0] = 1.0;
            for (std::size_t order = 0; order < filter.reflection.size(); ++order) {
                std::vector<double> previous = lpc;
                lpc[order + 1] = filter.reflection[order];
                for (std::size_t i = 1; i <= order; ++i)
                    lpc[i] = previous[i] +
                             filter.reflection[order] * previous[order + 1 - i];
            }
            const unsigned base = channel.info.IsShort()
                                      ? filter.window * kShortLength
                                      : 0;
            const int beginning = filter.reverse
                                      ? static_cast<int>(filter.endLine) - 1
                                      : static_cast<int>(filter.startLine);
            const int end = filter.reverse
                                ? static_cast<int>(filter.startLine) - 1
                                : static_cast<int>(filter.endLine);
            const int step = filter.reverse ? -1 : 1;
            std::vector<double> state(filter.reflection.size(), 0.0);
            for (int line = beginning; line != end; line += step) {
                double value = channel.spectrum[base + static_cast<unsigned>(line)];
                for (std::size_t i = 1; i < lpc.size(); ++i)
                    value -= lpc[i] * state[i - 1];
                for (std::size_t i = state.size(); i > 1; --i)
                    state[i - 1] = state[i - 2];
                if (!state.empty()) state[0] = value;
                channel.spectrum[base + static_cast<unsigned>(line)] = value;
            }
        }
    }

    std::array<float, kLongLength> Filterbank(unsigned channelIndex,
                                               const IcsChannel& channel) {
        std::array<double, kLongLength * 2> block{};
        ChannelState& state = channelStates[channelIndex];
        if (channel.info.IsShort()) {
            for (unsigned windowIndex = 0; windowIndex < 8; ++windowIndex) {
                const auto time = Imdct(channel.spectrum.data() +
                                            windowIndex * kShortLength,
                                        kShortLength);
                const auto& leftWindow = Window(
                    windowIndex == 0 ? state.previousWindowShape
                                     : channel.info.windowShape,
                    true);
                const auto& rightWindow = Window(channel.info.windowShape, true);
                const unsigned position = 448U + windowIndex * kShortLength;
                for (unsigned i = 0; i < kShortLength; ++i) {
                    block[position + i] += time[i] * leftWindow[i];
                    block[position + kShortLength + i] +=
                        time[kShortLength + i] *
                        rightWindow[kShortLength - 1U - i];
                }
            }
        } else {
            const auto time = Imdct(channel.spectrum.data(), kLongLength);
            const auto& previousLong = Window(state.previousWindowShape, false);
            const auto& currentLong = Window(channel.info.windowShape, false);
            if (channel.info.sequence == WindowSequence::OnlyLong) {
                for (unsigned i = 0; i < kLongLength; ++i) {
                    block[i] = time[i] * previousLong[i];
                    block[kLongLength + i] =
                        time[kLongLength + i] * currentLong[kLongLength - 1U - i];
                }
            } else if (channel.info.sequence == WindowSequence::LongStart) {
                const auto& shortWindow = Window(channel.info.windowShape, true);
                for (unsigned i = 0; i < kLongLength; ++i)
                    block[i] = time[i] * previousLong[i];
                for (unsigned i = 0; i < 448; ++i)
                    block[kLongLength + i] = time[kLongLength + i];
                for (unsigned i = 0; i < kShortLength; ++i)
                    block[kLongLength + 448U + i] =
                        time[kLongLength + 448U + i] *
                        shortWindow[kShortLength - 1U - i];
            } else if (channel.info.sequence == WindowSequence::LongStop) {
                const auto& shortWindow = Window(state.previousWindowShape, true);
                for (unsigned i = 0; i < kShortLength; ++i)
                    block[448U + i] = time[448U + i] * shortWindow[i];
                for (unsigned i = 576; i < kLongLength; ++i)
                    block[i] = time[i];
                for (unsigned i = 0; i < kLongLength; ++i)
                    block[kLongLength + i] =
                        time[kLongLength + i] * currentLong[kLongLength - 1U - i];
            }
        }
        std::array<float, kLongLength> output{};
        for (unsigned i = 0; i < kLongLength; ++i) {
            output[i] = static_cast<float>(state.overlap[i] + block[i]);
            state.overlap[i] = block[kLongLength + i];
        }
        state.previousWindowShape = channel.info.windowShape;
        return output;
    }

    bool ParseSingleChannel(BitReader& bits, unsigned channelIndex,
                            ChannelRole role, DecodedChannel& output) {
        unsigned tag = 0;
        if (!ReadBits(bits, 4, tag)) return Fail(L"Truncated AAC element tag");
        (void)tag;
        IcsChannel channel;
        if (!ParseIndividualChannel(bits, nullptr, channel)) return false;
        ApplyTns(channel);
        output.role = role;
        output.pcm = Filterbank(channelIndex, channel);
        return true;
    }

    bool ParseChannelPair(
        BitReader& bits, unsigned firstChannel, ChannelRole leftRole,
        ChannelRole rightRole, DecodedChannel& leftOutput,
        DecodedChannel& rightOutput) {
        unsigned tag = 0;
        bool commonWindow = false;
        if (!ReadBits(bits, 4, tag) || !bits.ReadBit(commonWindow))
            return Fail(L"Truncated AAC channel pair header");
        (void)tag;
        IcsInfo commonInfo;
        std::array<std::array<bool, kMaximumScaleFactorBands>, 8> msMask{};
        if (commonWindow) {
            if (!ParseIcsInfo(bits, commonInfo)) return false;
            unsigned maskPresent = 0;
            if (!ReadBits(bits, 2, maskPresent) || maskPresent == 3)
                return Fail(L"Invalid AAC MS mask mode");
            if (maskPresent == 2) {
                for (unsigned group = 0; group < commonInfo.groupLengths.size(); ++group)
                    for (unsigned band = 0; band < commonInfo.maxSfb; ++band)
                        msMask[group][band] = true;
            } else if (maskPresent == 1) {
                for (unsigned group = 0; group < commonInfo.groupLengths.size(); ++group)
                    for (unsigned band = 0; band < commonInfo.maxSfb; ++band)
                        if (!bits.ReadBit(msMask[group][band]))
                            return Fail(L"Truncated AAC MS mask");
            }
        }
        IcsChannel left, right;
        if (!ParseIndividualChannel(bits, commonWindow ? &commonInfo : nullptr, left) ||
            !ParseIndividualChannel(bits, commonWindow ? &commonInfo : nullptr, right))
            return false;
        // Without a common window the two individual_channel_stream elements
        // carry independent spectra; MS/intensity stereo signaling is absent.
        // Their window sequences are allowed to differ.
        if (commonWindow) ApplyStereo(left, right, msMask);
        ApplyTns(left);
        ApplyTns(right);
        leftOutput.role = leftRole;
        rightOutput.role = rightRole;
        leftOutput.pcm = Filterbank(firstChannel, left);
        rightOutput.pcm = Filterbank(firstChannel + 1U, right);
        return true;
    }

    bool SkipDataStreamElement(BitReader& bits) {
        unsigned tag = 0, count = 0;
        bool align = false;
        if (!ReadBits(bits, 4, tag) || !bits.ReadBit(align) ||
            !ReadBits(bits, 8, count)) return Fail(L"Truncated AAC data element");
        if (count == 255) {
            unsigned extension = 0;
            if (!ReadBits(bits, 8, extension)) return Fail(L"Truncated AAC data length");
            count += extension;
        }
        if (align && !bits.ByteAlign()) return Fail(L"Invalid AAC data alignment");
        return bits.SkipBits(static_cast<std::size_t>(count) * 8U) ||
               Fail(L"Truncated AAC data element payload");
    }

    bool SkipFillElement(BitReader& bits) {
        unsigned count = 0;
        if (!ReadBits(bits, 4, count)) return Fail(L"Truncated AAC fill element");
        if (count == 15) {
            unsigned extension = 0;
            if (!ReadBits(bits, 8, extension)) return Fail(L"Truncated AAC fill count");
            count += extension;
            if (count > 0) --count;
        }
        return bits.SkipBits(static_cast<std::size_t>(count) * 8U) ||
               Fail(L"Truncated AAC fill payload");
    }

    bool Decode(const EncodedSample& sample, AudioFrame& frame) {
        BitReader bits(sample.bytes);
        std::vector<DecodedChannel> decoded;
        decoded.reserve(expectedChannels);
        unsigned channelIndex = 0;
        unsigned pairIndex = 0;
        unsigned singleIndex = 0;
        for (;;) {
            unsigned element = 0;
            if (!ReadBits(bits, 3, element)) return Fail(L"Truncated AAC raw data block");
            if (element == 7) break;
            if (element == 0 || element == 3) {
                if (channelIndex >= channelStates.size()) return Fail(L"Too many AAC channels");
                DecodedChannel channel;
                ChannelRole role = element == 3
                                       ? ChannelRole::Lfe
                                       : (singleIndex++ == 0 ? ChannelRole::FrontCenter
                                                             : ChannelRole::Unknown);
                if (!ParseSingleChannel(bits, channelIndex++, role, channel)) return false;
                decoded.push_back(std::move(channel));
            } else if (element == 1) {
                if (channelIndex + 1 >= channelStates.size())
                    return Fail(L"Too many AAC channels");
                DecodedChannel left, right;
                const unsigned pairRole = pairIndex++;
                if (!ParseChannelPair(bits, channelIndex,
                                      pairRole == 0
                                          ? ChannelRole::FrontLeft
                                          : (pairRole == 1
                                                 ? ChannelRole::SurroundLeft
                                                 : ChannelRole::BackLeft),
                                      pairRole == 0
                                          ? ChannelRole::FrontRight
                                          : (pairRole == 1
                                                 ? ChannelRole::SurroundRight
                                                 : ChannelRole::BackRight),
                                      left, right)) return false;
                channelIndex += 2;
                decoded.push_back(std::move(left));
                decoded.push_back(std::move(right));
            } else if (element == 4) {
                if (!SkipDataStreamElement(bits)) return false;
            } else if (element == 5) {
                if (!ParseProgramConfigElement(bits, false)) return false;
            } else if (element == 6) {
                if (!SkipFillElement(bits)) return false;
            } else {
                return Fail(L"AAC coupling and PCE elements are not supported");
            }
        }
        if (decoded.size() != expectedChannels) {
            return Fail(L"The AAC frame channel count does not match its configuration");
        }
        const DecodedChannel* frontLeft = nullptr;
        const DecodedChannel* frontRight = nullptr;
        const DecodedChannel* center = nullptr;
        const DecodedChannel* surroundLeft = nullptr;
        const DecodedChannel* surroundRight = nullptr;
        const DecodedChannel* backLeft = nullptr;
        const DecodedChannel* backRight = nullptr;
        const DecodedChannel* lfe = nullptr;
        for (const auto& channel : decoded) {
            switch (channel.role) {
                case ChannelRole::FrontLeft: frontLeft = &channel; break;
                case ChannelRole::FrontRight: frontRight = &channel; break;
                case ChannelRole::FrontCenter: center = &channel; break;
                case ChannelRole::SurroundLeft: surroundLeft = &channel; break;
                case ChannelRole::SurroundRight: surroundRight = &channel; break;
                case ChannelRole::BackLeft: backLeft = &channel; break;
                case ChannelRole::BackRight: backRight = &channel; break;
                case ChannelRole::Lfe: lfe = &channel; break;
                default: break;
            }
        }
        if (!frontLeft || !frontRight) {
            return Fail(L"Unsupported AAC stereo channel element ordering");
        }
        frame = {};
        frame.sampleRate = sampleRate;
        frame.channels = 2;
        frame.channelMask = 3;
        frame.pts = sample.PtsSeconds();
        frame.samples.resize(kLongLength * 2U);
        // AAC spectral synthesis is expressed in signed 16-bit PCM units.
        if (channelConfiguration == 2) {
            constexpr float normalization = 1.0F / 32768.0F;
            for (unsigned i = 0; i < kLongLength; ++i) {
                frame.samples[2U * i] = std::clamp(
                    normalization * frontLeft->pcm[i], -1.0F, 1.0F);
                frame.samples[2U * i + 1U] = std::clamp(
                    normalization * frontRight->pcm[i], -1.0F, 1.0F);
            }
        } else if (expectedChannels == 6) {
            if (!center || !surroundLeft || !surroundRight || !lfe)
                return Fail(L"Unsupported AAC 5.1 element ordering");
            constexpr float centerGain = 0.70710678118F;
            constexpr float surroundGain = 0.70710678118F;
            constexpr float lfeGain = 0.5F;
            // Apply headroom for the 5.1-to-stereo sum.
            constexpr float normalization = 0.5F / 32768.0F;
            for (unsigned i = 0; i < kLongLength; ++i) {
                const float left = normalization *
                    (frontLeft->pcm[i] + centerGain * center->pcm[i] +
                     surroundGain * surroundLeft->pcm[i] + lfeGain * lfe->pcm[i]);
                const float right = normalization *
                    (frontRight->pcm[i] + centerGain * center->pcm[i] +
                     surroundGain * surroundRight->pcm[i] + lfeGain * lfe->pcm[i]);
                frame.samples[2U * i] = std::clamp(left, -1.0F, 1.0F);
                frame.samples[2U * i + 1U] = std::clamp(right, -1.0F, 1.0F);
            }
        } else {
            if (!center || !surroundLeft || !surroundRight || !backLeft ||
                !backRight || !lfe) {
                return Fail(L"Unsupported AAC 7.1 element ordering");
            }
            constexpr float centerGain = 0.70710678118F;
            constexpr float surroundGain = 0.5F;
            constexpr float backGain = 0.5F;
            constexpr float lfeGain = 0.35F;
            // The previous 0.4 normalization left typical 7.1 movie mixes
            // roughly 6 dB quieter than stereo tracks.  Use the available
            // headroom and apply a frame look-ahead limiter only when the
            // downmix would otherwise approach clipping.
            constexpr float normalization = 0.8F / 32768.0F;
            float peak = 0.0F;
            for (unsigned i = 0; i < kLongLength; ++i) {
                const float left = normalization *
                    (frontLeft->pcm[i] + centerGain * center->pcm[i] +
                     surroundGain * surroundLeft->pcm[i] +
                     backGain * backLeft->pcm[i] + lfeGain * lfe->pcm[i]);
                const float right = normalization *
                    (frontRight->pcm[i] + centerGain * center->pcm[i] +
                     surroundGain * surroundRight->pcm[i] +
                     backGain * backRight->pcm[i] + lfeGain * lfe->pcm[i]);
                frame.samples[2U * i] = left;
                frame.samples[2U * i + 1U] = right;
                peak = std::max(peak, std::max(std::abs(left), std::abs(right)));
            }

            constexpr float limiterCeiling = 0.98F;
            const float targetGain = peak > limiterCeiling
                                         ? limiterCeiling / peak
                                         : 1.0F;
            if (targetGain < downmixLimiterGain) {
                downmixLimiterGain = targetGain;
            } else {
                // Release over approximately one second to avoid audible
                // pumping at AAC frame boundaries.
                const float release = 1.0F - std::exp(
                    -static_cast<float>(kLongLength) /
                    static_cast<float>(sampleRate));
                downmixLimiterGain +=
                    (targetGain - downmixLimiterGain) * release;
            }
            for (float& value : frame.samples) {
                value = std::clamp(value * downmixLimiterGain, -1.0F, 1.0F);
            }
        }
        error.clear();
        return true;
    }

    void Reset() {
        for (auto& state : channelStates) state = {};
        noiseState = 0x6d2b79f5U;
        downmixLimiterGain = 1.0F;
        error.clear();
    }
};

AacLcDecoder::AacLcDecoder() : impl_(std::make_unique<Impl>()) {}
AacLcDecoder::~AacLcDecoder() = default;

bool AacLcDecoder::Initialize(const TrackInfo& track) {
    impl_->track = track;
    impl_->Reset();
    if (track.codec != CodecId::Aac || track.codecPrivate.empty())
        return impl_->Fail(L"AacLcDecoder received an invalid AAC track");
    if (!impl_->ParseAudioSpecificConfig(track.codecPrivate)) return false;
    impl_->description = L"Native C++ AAC-LC " +
                         std::to_wstring(impl_->sampleRate) + L" Hz ";
    if (impl_->expectedChannels == 2)
        impl_->description += L"stereo decoder";
    else if (impl_->expectedChannels == 6)
        impl_->description += L"5.1 decoder";
    else
        impl_->description += L"PCE 7.1 decoder";
    return true;
}

bool AacLcDecoder::Decode(const EncodedSample& sample, AudioFrame& frame) {
    return impl_->Decode(sample, frame);
}

void AacLcDecoder::Reset() { impl_->Reset(); }

const std::wstring& AacLcDecoder::Description() const noexcept {
    return impl_->description;
}

const std::wstring& AacLcDecoder::LastError() const noexcept {
    return impl_->error;
}

}  // namespace movieplayer::codec::aac
