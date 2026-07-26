#include "codec/audio/ac3/Ac3Decoder.h"

#include "codec/core/BitReader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace movieplayer::codec::ac3 {
namespace {

constexpr double kPi = 3.14159265358979323846264338327950288;
constexpr unsigned kAudioBlocks = 6;
constexpr unsigned kBlockSamples = 256;
constexpr unsigned kFullBandwidthChannels = 5;
constexpr unsigned kMaximumChannels = 6;
constexpr unsigned kCoefficientCount = 256;
constexpr unsigned kCouplingSubbands = 18;
constexpr unsigned kBitAllocationBands = 50;
constexpr double kDitherScale = 0.7071067811865475244;
constexpr std::array<std::uint8_t, 2> kSyncWord = {0x0b, 0x77};

constexpr std::array<int, 3> kSampleRates = {48'000, 44'100, 32'000};
constexpr std::array<int, 19> kBitRates = {
    32, 40, 48, 56, 64, 80, 96, 112, 128, 160,
    192, 224, 256, 320, 384, 448, 512, 576, 640};
constexpr std::array<int, 19> kFrameWords44100 = {
    69, 87, 104, 121, 139, 174, 208, 243, 278, 348,
    417, 487, 557, 696, 835, 975, 1114, 1253, 1393};
constexpr std::array<unsigned, 8> kChannelCounts = {2, 1, 2, 3, 3, 4, 4, 5};
constexpr std::array<unsigned, 16> kMantissaBits = {
    0, 0, 0, 3, 0, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 16};
constexpr std::array<int, 4> kSlowDecay = {0x0f, 0x11, 0x13, 0x15};
constexpr std::array<int, 4> kFastDecay = {0x3f, 0x53, 0x67, 0x7b};
constexpr std::array<int, 4> kSlowGain = {0x540, 0x4d8, 0x478, 0x410};
constexpr std::array<int, 4> kDbPerBit = {0x000, 0x700, 0x900, 0xb00};
constexpr std::array<int, 8> kFloor = {
    0x2f0, 0x2b0, 0x270, 0x230, 0x1f0, 0x170, 0x0f0, -0x800};
constexpr std::array<int, 8> kFastGain = {
    0x080, 0x100, 0x180, 0x200, 0x280, 0x300, 0x380, 0x400};
constexpr std::array<unsigned, 51> kBandStart = {
    0,   1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,
    13,  14,  15,  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,
    26,  27,  28,  31,  34,  37,  40,  43,  46,  49,  55,  61,  67,
    73,  79,  85,  97,  109, 121, 133, 157, 181, 205, 229, 253};
constexpr std::array<std::uint8_t, 64> kBapTable = {
    0, 1, 1, 1, 1, 1, 2, 2, 3, 3, 3, 4, 4, 5, 5, 6,
    6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 8, 9, 9, 9, 9, 10,
    10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 13, 13, 14,
    14, 14, 14, 14, 14, 14, 14, 15, 15, 15, 15, 15, 15, 15, 15, 15};
constexpr std::array<std::uint8_t, 256> kLogAdd = {
    64, 63, 62, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52, 52, 51, 50,
    49, 48, 47, 47, 46, 45, 44, 44, 43, 42, 41, 41, 40, 39, 38, 38,
    37, 36, 36, 35, 35, 34, 33, 33, 32, 32, 31, 30, 30, 29, 29, 28,
    28, 27, 27, 26, 26, 25, 25, 24, 24, 23, 23, 22, 22, 21, 21, 21,
    20, 20, 19, 19, 19, 18, 18, 18, 17, 17, 17, 16, 16, 16, 15, 15,
    15, 14, 14, 14, 13, 13, 13, 13, 12, 12, 12, 12, 11, 11, 11, 11,
    10, 10, 10, 10, 10, 9, 9, 9, 9, 9, 8, 8, 8, 8, 8, 8,
    7, 7, 7, 7, 7, 7, 6, 6, 6, 6, 6, 6, 6, 6, 5, 5,
    5, 5, 5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
constexpr std::array<std::array<int, 50>, 3> kHearingThreshold = {{
    {1232, 1232, 1088, 1024, 992, 960, 944, 944, 928, 928,
     928, 928, 928, 912, 912, 912, 896, 896, 880, 880,
     864, 864, 848, 848, 832, 832, 816, 800, 784, 768,
     752, 752, 752, 752, 768, 784, 832, 912, 992, 1056,
     1120, 1168, 1184, 1120, 1088, 1088, 1312, 2048, 2112, 2112},
    {1264, 1264, 1120, 1040, 992, 976, 960, 944, 944, 928,
     928, 928, 928, 928, 912, 912, 912, 896, 896, 896,
     880, 880, 864, 864, 848, 848, 832, 832, 800, 784,
     768, 752, 752, 752, 752, 768, 800, 848, 912, 992,
     1056, 1104, 1184, 1168, 1120, 1088, 1152, 1584, 2112, 2112},
    {1408, 1408, 1200, 1104, 1056, 1008, 992, 976, 960, 944,
     944, 944, 928, 928, 928, 928, 928, 928, 928, 928,
     912, 912, 912, 912, 896, 896, 896, 880, 864, 848,
     832, 816, 800, 784, 768, 752, 752, 752, 768, 784,
     816, 848, 960, 1040, 1136, 1184, 1120, 1088, 1104, 1248}
}};

bool ReadBits(BitReader& bits, unsigned count, unsigned& value) {
    std::uint32_t result = 0;
    if (!bits.ReadBits(count, result)) return false;
    value = result;
    return true;
}

bool ReadFlag(BitReader& bits, bool& value) {
    return bits.ReadBit(value);
}

unsigned BandForBin(unsigned bin) {
    const auto found =
        std::upper_bound(kBandStart.begin(), kBandStart.end(), bin);
    return static_cast<unsigned>(
        std::max<std::ptrdiff_t>(0, (found - kBandStart.begin()) - 1));
}

int FrameSizeWords(unsigned fscod, unsigned frameSizeCode) {
    if (fscod >= kSampleRates.size() || frameSizeCode >= 38) return 0;
    const unsigned rateIndex = frameSizeCode >> 1U;
    if (fscod == 0) return kBitRates[rateIndex] * 2;
    if (fscod == 1)
        return kFrameWords44100[rateIndex] +
               static_cast<int>(frameSizeCode & 1U);
    return kBitRates[rateIndex] * 3;
}

std::uint16_t CrcRemainder(const std::uint8_t* data, std::size_t size) {
    std::uint16_t result = 0;
    for (std::size_t i = 0; i < size; ++i) {
        for (int bit = 7; bit >= 0; --bit) {
            const bool feedback =
                ((result >> 15U) & 1U) != 0;
            result = static_cast<std::uint16_t>(
                (result << 1U) | ((data[i] >> bit) & 1U));
            if (feedback) result ^= 0x8005U;
        }
    }
    return result;
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

std::array<double, kBlockSamples> MakeWindow() {
    std::array<double, kBlockSamples> cumulative{};
    double sum = 0.0;
    for (unsigned i = 0; i < kBlockSamples; ++i) {
        const double x =
            (2.0 * static_cast<double>(i) /
             static_cast<double>(kBlockSamples - 1U)) -
            1.0;
        sum += BesselI0(5.0 * kPi *
                        std::sqrt(std::max(0.0, 1.0 - x * x)));
        cumulative[i] = sum;
    }
    for (double& value : cumulative) value = std::sqrt(value / sum);
    return cumulative;
}

struct TransformTables {
    std::array<double, 128> longCos{};
    std::array<double, 128> longSin{};
    std::array<double, 64> shortCos{};
    std::array<double, 64> shortSin{};
    std::array<std::array<double, 128>, 128> ifftLongCos{};
    std::array<std::array<double, 128>, 128> ifftLongSin{};
    std::array<std::array<double, 64>, 64> ifftShortCos{};
    std::array<std::array<double, 64>, 64> ifftShortSin{};
    std::array<double, kBlockSamples> window = MakeWindow();

    TransformTables() {
        for (unsigned i = 0; i < 128; ++i) {
            const double angle =
                2.0 * kPi * static_cast<double>(8U * i + 1U) /
                (8.0 * 512.0);
            longCos[i] = -std::cos(angle);
            longSin[i] = -std::sin(angle);
        }
        for (unsigned i = 0; i < 64; ++i) {
            const double angle =
                2.0 * kPi * static_cast<double>(8U * i + 1U) /
                (4.0 * 512.0);
            shortCos[i] = -std::cos(angle);
            shortSin[i] = -std::sin(angle);
        }
        for (unsigned n = 0; n < 128; ++n) {
            for (unsigned k = 0; k < 128; ++k) {
                const double angle =
                    2.0 * kPi * static_cast<double>(k * n) / 128.0;
                ifftLongCos[n][k] = std::cos(angle);
                ifftLongSin[n][k] = std::sin(angle);
            }
        }
        for (unsigned n = 0; n < 64; ++n) {
            for (unsigned k = 0; k < 64; ++k) {
                const double angle =
                    2.0 * kPi * static_cast<double>(k * n) / 64.0;
                ifftShortCos[n][k] = std::cos(angle);
                ifftShortSin[n][k] = std::sin(angle);
            }
        }
    }
};

const TransformTables& Tables() {
    static const TransformTables tables;
    return tables;
}

std::array<double, kCoefficientCount> InverseTransform(
    const std::array<double, kCoefficientCount>& coefficients,
    bool shortBlocks,
    std::array<double, kBlockSamples>& overlap) {
    const TransformTables& tables = Tables();
    std::array<double, 512> windowed{};
    if (!shortBlocks) {
        std::array<std::complex<double>, 128> before{};
        std::array<std::complex<double>, 128> after{};
        for (unsigned k = 0; k < 128; ++k) {
            const double odd = coefficients[255U - 2U * k];
            const double even = coefficients[2U * k];
            const double c = tables.longCos[k];
            const double s = tables.longSin[k];
            before[k] = {odd * c - even * s, even * c + odd * s};
        }
        for (unsigned n = 0; n < 128; ++n) {
            double real = 0.0;
            double imaginary = 0.0;
            for (unsigned k = 0; k < 128; ++k) {
                const double c = tables.ifftLongCos[n][k];
                const double s = tables.ifftLongSin[n][k];
                real += before[k].real() * c - before[k].imag() * s;
                imaginary += before[k].real() * s + before[k].imag() * c;
            }
            const double c = tables.longCos[n];
            const double s = tables.longSin[n];
            after[n] = {real * c - imaginary * s,
                        imaginary * c + real * s};
        }
        for (unsigned n = 0; n < 64; ++n) {
            windowed[2U * n] =
                -after[64U + n].imag() * tables.window[2U * n];
            windowed[2U * n + 1U] =
                after[63U - n].real() * tables.window[2U * n + 1U];
            windowed[128U + 2U * n] =
                -after[n].real() * tables.window[128U + 2U * n];
            windowed[128U + 2U * n + 1U] =
                after[127U - n].imag() *
                tables.window[128U + 2U * n + 1U];
            windowed[256U + 2U * n] =
                -after[64U + n].real() * tables.window[255U - 2U * n];
            windowed[256U + 2U * n + 1U] =
                after[63U - n].imag() * tables.window[254U - 2U * n];
            windowed[384U + 2U * n] =
                after[n].imag() * tables.window[127U - 2U * n];
            windowed[384U + 2U * n + 1U] =
                -after[127U - n].real() *
                tables.window[126U - 2U * n];
        }
    } else {
        std::array<double, 128> first{};
        std::array<double, 128> second{};
        for (unsigned k = 0; k < 128; ++k) {
            first[k] = coefficients[2U * k];
            second[k] = coefficients[2U * k + 1U];
        }
        std::array<std::complex<double>, 64> beforeFirst{};
        std::array<std::complex<double>, 64> beforeSecond{};
        std::array<std::complex<double>, 64> afterFirst{};
        std::array<std::complex<double>, 64> afterSecond{};
        for (unsigned k = 0; k < 64; ++k) {
            const double c = tables.shortCos[k];
            const double s = tables.shortSin[k];
            const double firstOdd = first[127U - 2U * k];
            const double firstEven = first[2U * k];
            const double secondOdd = second[127U - 2U * k];
            const double secondEven = second[2U * k];
            beforeFirst[k] = {firstOdd * c - firstEven * s,
                              firstEven * c + firstOdd * s};
            beforeSecond[k] = {secondOdd * c - secondEven * s,
                               secondEven * c + secondOdd * s};
        }
        for (unsigned n = 0; n < 64; ++n) {
            std::complex<double> firstSum{};
            std::complex<double> secondSum{};
            for (unsigned k = 0; k < 64; ++k) {
                const std::complex<double> factor(
                    tables.ifftShortCos[n][k],
                    tables.ifftShortSin[n][k]);
                firstSum += beforeFirst[k] * factor;
                secondSum += beforeSecond[k] * factor;
            }
            const std::complex<double> twiddle(
                tables.shortCos[n], tables.shortSin[n]);
            afterFirst[n] = firstSum * twiddle;
            afterSecond[n] = secondSum * twiddle;
        }
        for (unsigned n = 0; n < 64; ++n) {
            windowed[2U * n] =
                -afterFirst[n].imag() * tables.window[2U * n];
            windowed[2U * n + 1U] =
                afterFirst[63U - n].real() * tables.window[2U * n + 1U];
            windowed[128U + 2U * n] =
                -afterFirst[n].real() * tables.window[128U + 2U * n];
            windowed[128U + 2U * n + 1U] =
                afterFirst[63U - n].imag() *
                tables.window[128U + 2U * n + 1U];
            windowed[256U + 2U * n] =
                -afterSecond[n].real() * tables.window[255U - 2U * n];
            windowed[256U + 2U * n + 1U] =
                afterSecond[63U - n].imag() *
                tables.window[254U - 2U * n];
            windowed[384U + 2U * n] =
                afterSecond[n].imag() * tables.window[127U - 2U * n];
            windowed[384U + 2U * n + 1U] =
                -afterSecond[63U - n].real() *
                tables.window[126U - 2U * n];
        }
    }
    std::array<double, kBlockSamples> output{};
    for (unsigned i = 0; i < kBlockSamples; ++i) {
        output[i] = 2.0 * (windowed[i] + overlap[i]);
        overlap[i] = windowed[kBlockSamples + i];
    }
    return output;
}

struct DeltaSegment {
    unsigned offset = 0;
    unsigned length = 0;
    unsigned value = 0;
};

struct DeltaAllocation {
    unsigned mode = 2;
    std::vector<DeltaSegment> segments;
};

struct ExponentSet {
    std::array<int, kCoefficientCount> exponents{};
    std::array<std::uint8_t, kCoefficientCount> bap{};
    unsigned start = 0;
    unsigned end = 0;
    unsigned fineSnrOffset = 0;
    unsigned fastGainCode = 0;
    DeltaAllocation delta;
};

struct FrameState {
    unsigned sampleRateCode = 0;
    unsigned frameSizeCode = 0;
    unsigned bitRate = 0;
    unsigned bitStreamId = 0;
    unsigned audioCodingMode = 0;
    unsigned fullBandwidthChannels = 0;
    bool lfeOn = false;
    double centerMix = 0.595;
    double surroundMix = 0.5;
    unsigned dialNorm = 31;

    bool couplingInUse = false;
    std::array<bool, kFullBandwidthChannels> channelInCoupling{};
    unsigned couplingBeginCode = 0;
    unsigned couplingEndCode = 0;
    unsigned couplingSubbandCount = 0;
    unsigned couplingBandCount = 0;
    std::array<unsigned, kCouplingSubbands> couplingBandForSubband{};
    std::array<std::array<double, kCouplingSubbands>,
               kFullBandwidthChannels>
        couplingCoordinates{};
    std::array<bool, kCouplingSubbands> phaseFlags{};
    bool phaseFlagsInUse = false;

    std::array<bool, 4> rematrixFlags{};
    unsigned rematrixBandCount = 0;
    std::array<ExponentSet, kFullBandwidthChannels> channels{};
    ExponentSet coupling;
    ExponentSet lfe;

    unsigned slowDecayCode = 0;
    unsigned fastDecayCode = 0;
    unsigned slowGainCode = 0;
    unsigned dbPerBitCode = 0;
    unsigned floorCode = 0;
    unsigned coarseSnrOffset = 0;
    unsigned couplingFastLeak = 0;
    unsigned couplingSlowLeak = 0;
    unsigned dynamicRange = 0;
    unsigned dynamicRange2 = 0;
};

bool DecodeExponents(BitReader& bits, unsigned strategy, unsigned absolute,
                     unsigned groupCount, unsigned outputStart,
                     unsigned outputCount,
                     std::array<int, kCoefficientCount>& output,
                     bool skipAbsolute) {
    if (strategy == 0 || strategy > 3) return false;
    const unsigned groupSize = 1U << (strategy - 1U);
    int previous = static_cast<int>(absolute);
    unsigned written = 0;
    if (!skipAbsolute && outputCount != 0) {
        output[outputStart] = previous;
        written = 1;
    }
    for (unsigned group = 0; group < groupCount; ++group) {
        unsigned packed = 0;
        if (!ReadBits(bits, 7, packed) || packed >= 125) return false;
        const std::array<int, 3> differences = {
            static_cast<int>(packed / 25U) - 2,
            static_cast<int>((packed % 25U) / 5U) - 2,
            static_cast<int>(packed % 5U) - 2};
        for (int difference : differences) {
            previous += difference;
            if (previous < 0 || previous > 24) return false;
            for (unsigned duplicate = 0; duplicate < groupSize; ++duplicate) {
                if (written < outputCount)
                    output[outputStart + written] = previous;
                ++written;
            }
        }
    }
    return written >= outputCount;
}

int LogAdd(int first, int second) {
    const int difference = first - second;
    const unsigned address = static_cast<unsigned>(std::min(
        255, std::abs(difference) >> 1));
    return (difference >= 0 ? first : second) + kLogAdd[address];
}

int LowCompensation(int value, int current, int next, unsigned band) {
    if (band < 7) {
        if (current + 256 == next)
            return 384;
        if (current > next) return std::max(0, value - 64);
    } else if (band < 20) {
        if (current + 256 == next)
            return 320;
        if (current > next) return std::max(0, value - 64);
    } else {
        return std::max(0, value - 128);
    }
    return value;
}

bool ComputeBitAllocation(const FrameState& frame, ExponentSet& set,
                          bool couplingChannel, bool forceZero) {
    set.bap.fill(0);
    if (set.end <= set.start || set.end > kCoefficientCount) return false;
    if (forceZero) return true;

    std::array<int, kCoefficientCount> psd{};
    std::array<int, kBitAllocationBands> bandPsd{};
    std::array<int, kBitAllocationBands> excitation{};
    std::array<int, kBitAllocationBands> mask{};
    for (unsigned bin = set.start; bin < set.end; ++bin)
        psd[bin] = 3072 - (set.exponents[bin] << 7);

    const unsigned firstBand = BandForBin(set.start);
    const unsigned endBand = BandForBin(set.end - 1U) + 1U;
    unsigned bin = set.start;
    for (unsigned band = firstBand; band < endBand; ++band) {
        const unsigned last =
            std::min<unsigned>(kBandStart[band + 1U], set.end);
        bandPsd[band] = psd[bin++];
        while (bin < last)
            bandPsd[band] = LogAdd(bandPsd[band], psd[bin++]);
    }

    const int slowDecay = kSlowDecay[frame.slowDecayCode];
    const int fastDecay = kFastDecay[frame.fastDecayCode];
    const int slowGain = kSlowGain[frame.slowGainCode];
    const int fastGain = kFastGain[set.fastGainCode];
    int fastLeak = 0;
    int slowLeak = 0;
    unsigned begin = firstBand;
    if (firstBand == 0) {
        int lowComp = 0;
        lowComp = LowCompensation(lowComp, bandPsd[0], bandPsd[1], 0);
        excitation[0] = bandPsd[0] - fastGain - lowComp;
        lowComp = LowCompensation(lowComp, bandPsd[1], bandPsd[2], 1);
        excitation[1] = bandPsd[1] - fastGain - lowComp;
        begin = 7;
        for (unsigned band = 2; band < 7; ++band) {
            const bool finalLfeBand = endBand == 7 && band == 6;
            if (!finalLfeBand)
                lowComp = LowCompensation(
                    lowComp, bandPsd[band], bandPsd[band + 1U], band);
            fastLeak = bandPsd[band] - fastGain;
            slowLeak = bandPsd[band] - slowGain;
            excitation[band] = fastLeak - lowComp;
            if (!finalLfeBand && bandPsd[band] <= bandPsd[band + 1U]) {
                begin = band + 1U;
                break;
            }
        }
        for (unsigned band = begin; band < std::min(endBand, 22U); ++band) {
            const bool finalLfeBand = endBand == 7 && band == 6;
            if (!finalLfeBand)
                lowComp = LowCompensation(
                    lowComp, bandPsd[band], bandPsd[band + 1U], band);
            fastLeak =
                std::max(fastLeak - fastDecay, bandPsd[band] - fastGain);
            slowLeak =
                std::max(slowLeak - slowDecay, bandPsd[band] - slowGain);
            excitation[band] = std::max(fastLeak - lowComp, slowLeak);
        }
        begin = 22;
    } else if (couplingChannel) {
        fastLeak = static_cast<int>((frame.couplingFastLeak << 8U) + 768U);
        slowLeak = static_cast<int>((frame.couplingSlowLeak << 8U) + 768U);
    }
    for (unsigned band = begin; band < endBand; ++band) {
        fastLeak =
            std::max(fastLeak - fastDecay, bandPsd[band] - fastGain);
        slowLeak =
            std::max(slowLeak - slowDecay, bandPsd[band] - slowGain);
        excitation[band] = std::max(fastLeak, slowLeak);
    }

    const int dbKnee = kDbPerBit[frame.dbPerBitCode];
    for (unsigned band = firstBand; band < endBand; ++band) {
        if (bandPsd[band] < dbKnee)
            excitation[band] += (dbKnee - bandPsd[band]) >> 2;
        mask[band] = std::max(
            excitation[band],
            kHearingThreshold[frame.sampleRateCode][band]);
    }

    if (set.delta.mode == 1) {
        unsigned relativeBand = 0;
        for (const DeltaSegment& segment : set.delta.segments) {
            relativeBand += segment.offset;
            const int delta = segment.value >= 4
                                  ? static_cast<int>(segment.value - 3U) << 7
                                  : (static_cast<int>(segment.value) - 4) << 7;
            for (unsigned i = 0; i < segment.length; ++i) {
                const unsigned target = firstBand + relativeBand++;
                if (target >= endBand) return false;
                mask[target] += delta;
            }
        }
    }

    const int snrOffset =
        ((static_cast<int>(frame.coarseSnrOffset) - 15) * 16 +
         static_cast<int>(set.fineSnrOffset)) *
        4;
    const int floor = kFloor[frame.floorCode];
    bin = set.start;
    for (unsigned band = firstBand; band < endBand; ++band) {
        const unsigned last =
            std::min<unsigned>(kBandStart[band + 1U], set.end);
        int adjusted = mask[band] - snrOffset - floor;
        if (adjusted < 0) adjusted = 0;
        adjusted &= 0x1fe0;
        adjusted += floor;
        while (bin < last) {
            const int address = std::max(
                0, std::min(63, (psd[bin] - adjusted) >> 5));
            set.bap[bin] = kBapTable[static_cast<unsigned>(address)];
            ++bin;
        }
    }
    return true;
}

struct GroupedMantissas {
    std::array<unsigned, 3> level3{};
    std::array<unsigned, 3> level5{};
    std::array<unsigned, 2> level11{};
    unsigned level3Position = 3;
    unsigned level5Position = 3;
    unsigned level11Position = 2;
};

bool DecodeMantissa(BitReader& bits, unsigned bap, int exponent, bool dither,
                    std::uint32_t& ditherState, GroupedMantissas& groups,
                    double& coefficient) {
    if (bap > 15 || exponent < 0 || exponent > 24) return false;
    const double exponentScale = std::ldexp(1.0, -exponent);
    if (bap == 0) {
        if (!dither) {
            coefficient = 0.0;
            return true;
        }
        ditherState = ditherState * 1664525U + 1013904223U;
        const auto random =
            static_cast<std::int16_t>(ditherState >> 16U);
        coefficient =
            (static_cast<double>(random) / 32768.0) *
            kDitherScale * exponentScale;
        return true;
    }

    unsigned code = 0;
    unsigned levels = 0;
    if (bap == 1) {
        levels = 3;
        if (groups.level3Position == groups.level3.size()) {
            unsigned packed = 0;
            if (!ReadBits(bits, 5, packed) || packed >= 27) return false;
            groups.level3 = {
                packed / 9U, (packed % 9U) / 3U, packed % 3U};
            groups.level3Position = 0;
        }
        code = groups.level3[groups.level3Position++];
    } else if (bap == 2) {
        levels = 5;
        if (groups.level5Position == groups.level5.size()) {
            unsigned packed = 0;
            if (!ReadBits(bits, 7, packed) || packed >= 125) return false;
            groups.level5 = {
                packed / 25U, (packed % 25U) / 5U, packed % 5U};
            groups.level5Position = 0;
        }
        code = groups.level5[groups.level5Position++];
    } else if (bap == 3) {
        levels = 7;
        if (!ReadBits(bits, 3, code) || code >= levels) return false;
    } else if (bap == 4) {
        levels = 11;
        if (groups.level11Position == groups.level11.size()) {
            unsigned packed = 0;
            if (!ReadBits(bits, 7, packed) || packed >= 121) return false;
            groups.level11 = {packed / 11U, packed % 11U};
            groups.level11Position = 0;
        }
        code = groups.level11[groups.level11Position++];
    } else if (bap == 5) {
        levels = 15;
        if (!ReadBits(bits, 4, code) || code >= levels) return false;
    } else {
        const unsigned bitCount = kMantissaBits[bap];
        if (!ReadBits(bits, bitCount, code)) return false;
        const std::uint32_t signBit = std::uint32_t{1} << (bitCount - 1U);
        const std::int32_t signedValue =
            (code & signBit) != 0
                ? static_cast<std::int32_t>(
                      code - (std::uint32_t{1} << bitCount))
                : static_cast<std::int32_t>(code);
        coefficient =
            static_cast<double>(signedValue) /
            static_cast<double>(signBit) * exponentScale;
        return true;
    }
    coefficient =
        (2.0 * static_cast<double>(code) -
         static_cast<double>(levels - 1U)) /
        static_cast<double>(levels) * exponentScale;
    return true;
}

bool ParseDeltaAllocation(BitReader& bits, DeltaAllocation& allocation) {
    allocation.segments.clear();
    unsigned encodedCount = 0;
    if (!ReadBits(bits, 3, encodedCount)) return false;
    allocation.segments.reserve(encodedCount + 1U);
    for (unsigned i = 0; i <= encodedCount; ++i) {
        DeltaSegment segment;
        if (!ReadBits(bits, 5, segment.offset) ||
            !ReadBits(bits, 4, segment.length) ||
            !ReadBits(bits, 3, segment.value)) {
            return false;
        }
        allocation.segments.push_back(segment);
    }
    allocation.mode = 1;
    return true;
}

double DynamicRangeGain(unsigned value) {
    int exponent = static_cast<int>((value >> 5U) & 7U);
    if (exponent >= 4) exponent -= 8;
    const double fraction =
        static_cast<double>(32U + (value & 31U)) / 64.0;
    return std::ldexp(fraction, exponent + 1);
}

}  // namespace

struct Ac3Decoder::Impl {
    TrackInfo track;
    std::array<std::array<double, kBlockSamples>, kMaximumChannels> overlap{};
    std::uint32_t ditherState = 0x12345678U;
    std::vector<std::uint8_t> pendingBytes;
    double pendingPts = 0.0;
    std::uint32_t averageBytesPerSecond = 0;
    bool pendingTimestamp = false;
    bool chunkedWaveInput = false;
    std::wstring description;
    std::wstring error;

    bool Fail(const std::wstring& message) {
        error = message;
        return false;
    }

    bool SkipOptional(BitReader& bits, unsigned flagBits,
                      unsigned payloadBits) {
        unsigned present = 0;
        return ReadBits(bits, flagBits, present) &&
               (!present || bits.SkipBits(payloadBits));
    }

    bool ParseHeader(BitReader& bits, const EncodedSample& sample,
                     FrameState& frame) {
        unsigned syncWord = 0;
        unsigned ignored = 0;
        if (!ReadBits(bits, 16, syncWord) || syncWord != 0x0b77 ||
            !ReadBits(bits, 16, ignored) ||
            !ReadBits(bits, 2, frame.sampleRateCode) ||
            !ReadBits(bits, 6, frame.frameSizeCode)) {
            return Fail(L"Invalid or truncated AC-3 synchronization header");
        }
        const int words =
            FrameSizeWords(frame.sampleRateCode, frame.frameSizeCode);
        if (words <= 0 ||
            sample.bytes.size() != static_cast<std::size_t>(words * 2)) {
            return Fail(L"The AC-3 packet size does not match its frame header");
        }
        if (CrcRemainder(sample.bytes.data() + 2U,
                         sample.bytes.size() - 2U) != 0) {
            return Fail(L"The AC-3 frame failed its CRC check");
        }
        frame.bitRate =
            static_cast<unsigned>(kBitRates[frame.frameSizeCode >> 1U]);
        if (!ReadBits(bits, 5, frame.bitStreamId) ||
            frame.bitStreamId > 8 ||
            !ReadBits(bits, 3, ignored) ||
            !ReadBits(bits, 3, frame.audioCodingMode)) {
            return Fail(L"Unsupported or truncated AC-3 bit-stream information");
        }
        frame.fullBandwidthChannels =
            kChannelCounts[frame.audioCodingMode];
        if ((frame.audioCodingMode & 1U) != 0 &&
            frame.audioCodingMode != 1) {
            unsigned centerMix = 0;
            if (!ReadBits(bits, 2, centerMix))
                return Fail(L"Truncated AC-3 center mix level");
            constexpr std::array<double, 4> values = {
                0.7071067811865476, 0.5946035575013605, 0.5,
                0.5946035575013605};
            frame.centerMix = values[centerMix];
        }
        if ((frame.audioCodingMode & 4U) != 0) {
            unsigned surroundMix = 0;
            if (!ReadBits(bits, 2, surroundMix))
                return Fail(L"Truncated AC-3 surround mix level");
            constexpr std::array<double, 4> values = {
                0.7071067811865476, 0.5, 0.0, 0.5};
            frame.surroundMix = values[surroundMix];
        }
        if (frame.audioCodingMode == 2 && !bits.SkipBits(2))
            return Fail(L"Truncated AC-3 surround mode");
        if (!ReadFlag(bits, frame.lfeOn) ||
            !ReadBits(bits, 5, frame.dialNorm) ||
            !SkipOptional(bits, 1, 8) ||
            !SkipOptional(bits, 1, 8) ||
            !SkipOptional(bits, 1, 7)) {
            return Fail(L"Truncated AC-3 program information");
        }
        if (frame.audioCodingMode == 0) {
            if (!bits.SkipBits(5) ||
                !SkipOptional(bits, 1, 8) ||
                !SkipOptional(bits, 1, 8) ||
                !SkipOptional(bits, 1, 7)) {
                return Fail(L"Truncated AC-3 dual-mono program information");
            }
        }
        if (!bits.SkipBits(2) ||
            !SkipOptional(bits, 1, 14) ||
            !SkipOptional(bits, 1, 14)) {
            return Fail(L"Truncated AC-3 time-code information");
        }
        unsigned additional = 0;
        if (!ReadBits(bits, 1, additional))
            return Fail(L"Truncated AC-3 additional information flag");
        if (additional) {
            unsigned length = 0;
            if (!ReadBits(bits, 6, length) ||
                !bits.SkipBits(static_cast<std::size_t>(length + 1U) * 8U)) {
                return Fail(L"Truncated AC-3 additional information");
            }
        }
        return true;
    }

    bool ParseCoupling(BitReader& bits, FrameState& frame, unsigned block) {
        bool strategyExists = false;
        if (!ReadFlag(bits, strategyExists))
            return Fail(L"Truncated AC-3 coupling strategy");
        if (strategyExists) {
            if (!ReadFlag(bits, frame.couplingInUse))
                return Fail(L"Truncated AC-3 coupling state");
            if (frame.couplingInUse) {
                unsigned coupledCount = 0;
                for (unsigned channel = 0;
                     channel < frame.fullBandwidthChannels; ++channel) {
                    if (!ReadFlag(bits, frame.channelInCoupling[channel]))
                        return Fail(L"Truncated AC-3 coupled-channel flags");
                    if (frame.channelInCoupling[channel]) ++coupledCount;
                }
                if (coupledCount < 2)
                    return Fail(L"Invalid AC-3 coupling configuration");
                if (frame.audioCodingMode == 2 &&
                    !ReadFlag(bits, frame.phaseFlagsInUse)) {
                    return Fail(L"Truncated AC-3 coupling phase mode");
                }
                if (!ReadBits(bits, 4, frame.couplingBeginCode) ||
                    !ReadBits(bits, 4, frame.couplingEndCode) ||
                    frame.couplingBeginCode >
                        frame.couplingEndCode + 2U) {
                    return Fail(L"Invalid AC-3 coupling frequency range");
                }
                frame.couplingSubbandCount =
                    3U + frame.couplingEndCode -
                    frame.couplingBeginCode;
                if (frame.couplingSubbandCount == 0 ||
                    frame.couplingSubbandCount > kCouplingSubbands) {
                    return Fail(L"Invalid AC-3 coupling sub-band count");
                }
                frame.couplingBandCount = 1;
                frame.couplingBandForSubband.fill(0);
                for (unsigned subband = 1;
                     subband < frame.couplingSubbandCount; ++subband) {
                    bool combined = false;
                    if (!ReadFlag(bits, combined))
                        return Fail(L"Truncated AC-3 coupling band structure");
                    if (!combined) ++frame.couplingBandCount;
                    frame.couplingBandForSubband[subband] =
                        frame.couplingBandCount - 1U;
                }
            } else {
                frame.channelInCoupling.fill(false);
                frame.couplingSubbandCount = 0;
                frame.couplingBandCount = 0;
            }
        } else if (block == 0) {
            return Fail(L"AC-3 block 0 reuses a missing coupling strategy");
        }

        if (!frame.couplingInUse) return true;
        std::array<bool, kFullBandwidthChannels> coordinatesExist{};
        for (unsigned channel = 0;
             channel < frame.fullBandwidthChannels; ++channel) {
            if (!frame.channelInCoupling[channel]) continue;
            if (!ReadFlag(bits, coordinatesExist[channel]))
                return Fail(L"Truncated AC-3 coupling coordinates");
            if (!coordinatesExist[channel]) {
                if (block == 0)
                    return Fail(L"AC-3 block 0 reuses missing coupling coordinates");
                continue;
            }
            unsigned master = 0;
            if (!ReadBits(bits, 2, master))
                return Fail(L"Truncated AC-3 master coupling coordinate");
            for (unsigned band = 0;
                 band < frame.couplingBandCount; ++band) {
                unsigned exponent = 0;
                unsigned mantissa = 0;
                if (!ReadBits(bits, 4, exponent) ||
                    !ReadBits(bits, 4, mantissa)) {
                    return Fail(L"Truncated AC-3 coupling coordinate");
                }
                const double base =
                    exponent == 15
                        ? static_cast<double>(mantissa) / 16.0
                        : static_cast<double>(mantissa + 16U) / 32.0;
                frame.couplingCoordinates[channel][band] =
                    std::ldexp(base, -static_cast<int>(
                                          exponent + 3U * master));
            }
        }
        if (frame.audioCodingMode == 2 && frame.phaseFlagsInUse &&
            (coordinatesExist[0] || coordinatesExist[1])) {
            for (unsigned band = 0;
                 band < frame.couplingBandCount; ++band) {
                if (!ReadFlag(bits, frame.phaseFlags[band]))
                    return Fail(L"Truncated AC-3 coupling phase flags");
            }
        }
        return true;
    }

    bool ParseRematrix(BitReader& bits, FrameState& frame, unsigned block) {
        if (frame.audioCodingMode != 2) return true;
        bool newStrategy = false;
        if (!ReadFlag(bits, newStrategy))
            return Fail(L"Truncated AC-3 rematrixing strategy");
        frame.rematrixBandCount =
            !frame.couplingInUse || frame.couplingBeginCode > 2
                ? 4U
                : (frame.couplingBeginCode > 0 ? 3U : 2U);
        if (!newStrategy) {
            if (block == 0)
                return Fail(L"AC-3 block 0 reuses missing rematrix flags");
            return true;
        }
        for (unsigned band = 0; band < frame.rematrixBandCount; ++band) {
            if (!ReadFlag(bits, frame.rematrixFlags[band]))
                return Fail(L"Truncated AC-3 rematrix flags");
        }
        return true;
    }

    bool ParseExponents(BitReader& bits, FrameState& frame, unsigned block) {
        unsigned couplingStrategy = 0;
        std::array<unsigned, kFullBandwidthChannels> strategies{};
        unsigned lfeStrategy = 0;
        if (frame.couplingInUse &&
            !ReadBits(bits, 2, couplingStrategy)) {
            return Fail(L"Truncated AC-3 coupling exponent strategy");
        }
        for (unsigned channel = 0;
             channel < frame.fullBandwidthChannels; ++channel) {
            if (!ReadBits(bits, 2, strategies[channel]))
                return Fail(L"Truncated AC-3 channel exponent strategy");
        }
        if (frame.lfeOn && !ReadBits(bits, 1, lfeStrategy))
            return Fail(L"Truncated AC-3 LFE exponent strategy");
        if (block == 0) {
            if ((frame.couplingInUse && couplingStrategy == 0) ||
                (frame.lfeOn && lfeStrategy == 0)) {
                return Fail(L"AC-3 block 0 reuses missing exponents");
            }
            for (unsigned channel = 0;
                 channel < frame.fullBandwidthChannels; ++channel) {
                if (strategies[channel] == 0)
                    return Fail(L"AC-3 block 0 reuses missing channel exponents");
            }
        }

        const unsigned couplingStart =
            37U + 12U * frame.couplingBeginCode;
        const unsigned couplingEnd =
            37U + 12U * (frame.couplingEndCode + 3U);
        if (frame.couplingInUse) {
            frame.coupling.start = couplingStart;
            frame.coupling.end = couplingEnd;
        }
        for (unsigned channel = 0;
             channel < frame.fullBandwidthChannels; ++channel) {
            ExponentSet& set = frame.channels[channel];
            set.start = 0;
            if (strategies[channel] == 0) continue;
            if (frame.channelInCoupling[channel]) {
                set.end = couplingStart;
            } else {
                unsigned bandwidth = 0;
                if (!ReadBits(bits, 6, bandwidth) || bandwidth > 60)
                    return Fail(L"Invalid AC-3 channel bandwidth code");
                set.end = 37U + 3U * (bandwidth + 12U);
            }
        }

        if (frame.couplingInUse && couplingStrategy != 0) {
            unsigned absolute = 0;
            if (!ReadBits(bits, 4, absolute))
                return Fail(L"Truncated AC-3 coupling absolute exponent");
            const unsigned groupSize = 1U << (couplingStrategy - 1U);
            const unsigned groupCount =
                (couplingEnd - couplingStart) / (3U * groupSize);
            if (!DecodeExponents(
                    bits, couplingStrategy, absolute << 1U, groupCount,
                    couplingStart, couplingEnd - couplingStart,
                    frame.coupling.exponents, true)) {
                return Fail(L"Invalid AC-3 coupling exponents");
            }
        }

        for (unsigned channel = 0;
             channel < frame.fullBandwidthChannels; ++channel) {
            if (strategies[channel] == 0) continue;
            ExponentSet& set = frame.channels[channel];
            unsigned absolute = 0;
            if (!ReadBits(bits, 4, absolute))
                return Fail(L"Truncated AC-3 absolute exponent");
            const unsigned groupSize = 1U << (strategies[channel] - 1U);
            unsigned groupCount = 0;
            if (strategies[channel] == 1)
                groupCount = (set.end - 1U) / 3U;
            else if (strategies[channel] == 2)
                groupCount = (set.end + 2U) / 6U;
            else
                groupCount = (set.end + 8U) / 12U;
            if (!DecodeExponents(
                    bits, strategies[channel], absolute, groupCount, 0,
                    set.end, set.exponents, false) ||
                !bits.SkipBits(2)) {
                return Fail(L"Invalid AC-3 channel exponents");
            }
        }
        if (frame.lfeOn && lfeStrategy != 0) {
            frame.lfe.start = 0;
            frame.lfe.end = 7;
            unsigned absolute = 0;
            if (!ReadBits(bits, 4, absolute) ||
                !DecodeExponents(bits, 1, absolute, 2, 0, 7,
                                 frame.lfe.exponents, false)) {
                return Fail(L"Invalid AC-3 LFE exponents");
            }
        }
        return true;
    }

    bool ParseBitAllocation(BitReader& bits, FrameState& frame,
                            unsigned block) {
        bool informationExists = false;
        if (!ReadFlag(bits, informationExists))
            return Fail(L"Truncated AC-3 bit allocation information");
        if (informationExists) {
            if (!ReadBits(bits, 2, frame.slowDecayCode) ||
                !ReadBits(bits, 2, frame.fastDecayCode) ||
                !ReadBits(bits, 2, frame.slowGainCode) ||
                !ReadBits(bits, 2, frame.dbPerBitCode) ||
                !ReadBits(bits, 3, frame.floorCode)) {
                return Fail(L"Truncated AC-3 bit allocation parameters");
            }
        } else if (block == 0) {
            return Fail(L"AC-3 block 0 reuses missing bit allocation parameters");
        }

        bool snrExists = false;
        if (!ReadFlag(bits, snrExists))
            return Fail(L"Truncated AC-3 SNR offset information");
        if (snrExists) {
            if (!ReadBits(bits, 6, frame.coarseSnrOffset))
                return Fail(L"Truncated AC-3 coarse SNR offset");
            if (frame.couplingInUse &&
                (!ReadBits(bits, 4, frame.coupling.fineSnrOffset) ||
                 !ReadBits(bits, 3, frame.coupling.fastGainCode))) {
                return Fail(L"Truncated AC-3 coupling SNR offset");
            }
            for (unsigned channel = 0;
                 channel < frame.fullBandwidthChannels; ++channel) {
                if (!ReadBits(bits, 4,
                              frame.channels[channel].fineSnrOffset) ||
                    !ReadBits(bits, 3,
                              frame.channels[channel].fastGainCode)) {
                    return Fail(L"Truncated AC-3 channel SNR offset");
                }
            }
            if (frame.lfeOn &&
                (!ReadBits(bits, 4, frame.lfe.fineSnrOffset) ||
                 !ReadBits(bits, 3, frame.lfe.fastGainCode))) {
                return Fail(L"Truncated AC-3 LFE SNR offset");
            }
        } else if (block == 0) {
            return Fail(L"AC-3 block 0 reuses missing SNR offsets");
        }

        if (frame.couplingInUse) {
            bool leakExists = false;
            if (!ReadFlag(bits, leakExists))
                return Fail(L"Truncated AC-3 coupling leak information");
            if (leakExists) {
                if (!ReadBits(bits, 3, frame.couplingFastLeak) ||
                    !ReadBits(bits, 3, frame.couplingSlowLeak)) {
                    return Fail(L"Truncated AC-3 coupling leak parameters");
                }
            } else if (block == 0) {
                return Fail(L"AC-3 block 0 reuses missing coupling leak parameters");
            }
        }

        bool deltaExists = false;
        if (!ReadFlag(bits, deltaExists))
            return Fail(L"Truncated AC-3 delta bit allocation information");
        if (!deltaExists) {
            if (block == 0) {
                frame.coupling.delta = {};
                frame.coupling.delta.mode = 2;
                for (unsigned channel = 0;
                     channel < frame.fullBandwidthChannels; ++channel) {
                    frame.channels[channel].delta = {};
                    frame.channels[channel].delta.mode = 2;
                }
            }
            return true;
        }

        unsigned couplingMode = frame.coupling.delta.mode;
        if (frame.couplingInUse &&
            !ReadBits(bits, 2, couplingMode)) {
            return Fail(L"Truncated AC-3 coupling delta allocation mode");
        }
        std::array<unsigned, kFullBandwidthChannels> channelModes{};
        for (unsigned channel = 0;
             channel < frame.fullBandwidthChannels; ++channel) {
            if (!ReadBits(bits, 2, channelModes[channel]) ||
                channelModes[channel] == 3) {
                return Fail(L"Invalid AC-3 channel delta allocation mode");
            }
        }
        if (frame.couplingInUse) {
            if (couplingMode == 3)
                return Fail(L"Invalid AC-3 coupling delta allocation mode");
            if (couplingMode == 1) {
                if (!ParseDeltaAllocation(bits, frame.coupling.delta))
                    return Fail(L"Truncated AC-3 coupling delta allocation");
            } else if (couplingMode == 2) {
                frame.coupling.delta = {};
                frame.coupling.delta.mode = 2;
            }
        }
        for (unsigned channel = 0;
             channel < frame.fullBandwidthChannels; ++channel) {
            if (channelModes[channel] == 1) {
                if (!ParseDeltaAllocation(
                        bits, frame.channels[channel].delta)) {
                    return Fail(L"Truncated AC-3 channel delta allocation");
                }
            } else if (channelModes[channel] == 2) {
                frame.channels[channel].delta = {};
                frame.channels[channel].delta.mode = 2;
            }
        }
        return true;
    }

    bool DecodeCoefficients(
        BitReader& bits, FrameState& frame,
        const std::array<bool, kFullBandwidthChannels>& ditherFlags,
        std::array<std::array<double, kCoefficientCount>,
                   kMaximumChannels>& coefficients) {
        bool allSnrZero = frame.coarseSnrOffset == 0;
        if (frame.couplingInUse)
            allSnrZero =
                allSnrZero && frame.coupling.fineSnrOffset == 0;
        for (unsigned channel = 0;
             channel < frame.fullBandwidthChannels; ++channel) {
            allSnrZero =
                allSnrZero && frame.channels[channel].fineSnrOffset == 0;
        }
        if (frame.lfeOn)
            allSnrZero = allSnrZero && frame.lfe.fineSnrOffset == 0;

        for (unsigned channel = 0;
             channel < frame.fullBandwidthChannels; ++channel) {
            if (!ComputeBitAllocation(
                    frame, frame.channels[channel], false, allSnrZero)) {
                return Fail(L"Invalid AC-3 channel bit allocation");
            }
        }
        if (frame.couplingInUse &&
            !ComputeBitAllocation(
                frame, frame.coupling, true, allSnrZero)) {
            return Fail(L"Invalid AC-3 coupling bit allocation");
        }
        if (frame.lfeOn &&
            !ComputeBitAllocation(frame, frame.lfe, false, allSnrZero)) {
            return Fail(L"Invalid AC-3 LFE bit allocation");
        }

        GroupedMantissas groups;
        std::array<double, kCoefficientCount> couplingCoefficients{};
        bool decodedCoupling = false;
        for (unsigned channel = 0;
             channel < frame.fullBandwidthChannels; ++channel) {
            ExponentSet& set = frame.channels[channel];
            for (unsigned bin = set.start; bin < set.end; ++bin) {
                if (!DecodeMantissa(
                        bits, set.bap[bin], set.exponents[bin],
                        ditherFlags[channel], ditherState, groups,
                        coefficients[channel][bin])) {
                    return Fail(L"Invalid or truncated AC-3 channel mantissas");
                }
            }
            if (frame.couplingInUse &&
                frame.channelInCoupling[channel] &&
                !decodedCoupling) {
                for (unsigned bin = frame.coupling.start;
                     bin < frame.coupling.end; ++bin) {
                    if (!DecodeMantissa(
                            bits, frame.coupling.bap[bin],
                            frame.coupling.exponents[bin], false,
                            ditherState, groups,
                            couplingCoefficients[bin])) {
                        return Fail(
                            L"Invalid or truncated AC-3 coupling mantissas");
                    }
                }
                decodedCoupling = true;
            }
        }
        if (frame.lfeOn) {
            for (unsigned bin = frame.lfe.start;
                 bin < frame.lfe.end; ++bin) {
                if (!DecodeMantissa(
                        bits, frame.lfe.bap[bin],
                        frame.lfe.exponents[bin], false,
                        ditherState, groups,
                        coefficients[frame.fullBandwidthChannels][bin])) {
                    return Fail(L"Invalid or truncated AC-3 LFE mantissas");
                }
            }
        }

        if (frame.couplingInUse) {
            for (unsigned channel = 0;
                 channel < frame.fullBandwidthChannels; ++channel) {
                if (!frame.channelInCoupling[channel]) continue;
                for (unsigned subband = 0;
                     subband < frame.couplingSubbandCount; ++subband) {
                    const unsigned band =
                        frame.couplingBandForSubband[subband];
                    double coordinate =
                        frame.couplingCoordinates[channel][band] * 8.0;
                    if (frame.audioCodingMode == 2 && channel == 1 &&
                        frame.phaseFlagsInUse &&
                        frame.phaseFlags[band]) {
                        coordinate = -coordinate;
                    }
                    const unsigned first =
                        frame.coupling.start + subband * 12U;
                    const unsigned last =
                        std::min(first + 12U, frame.coupling.end);
                    for (unsigned bin = first; bin < last; ++bin) {
                        if (frame.coupling.bap[bin] == 0 &&
                            ditherFlags[channel]) {
                            double dither = 0.0;
                            GroupedMantissas unused;
                            if (!DecodeMantissa(
                                    bits, 0,
                                    frame.coupling.exponents[bin], true,
                                    ditherState, unused, dither)) {
                                return false;
                            }
                            coefficients[channel][bin] =
                                dither * coordinate;
                        } else {
                            coefficients[channel][bin] =
                                couplingCoefficients[bin] * coordinate;
                        }
                    }
                }
            }
        }
        return true;
    }

    void ApplyRematrix(
        const FrameState& frame,
        std::array<std::array<double, kCoefficientCount>,
                   kMaximumChannels>& coefficients) {
        if (frame.audioCodingMode != 2) return;
        constexpr std::array<unsigned, 5> bounds = {13, 25, 37, 61, 253};
        const unsigned coupledEnd =
            frame.couplingInUse ? frame.coupling.start : 253U;
        for (unsigned band = 0; band < frame.rematrixBandCount; ++band) {
            if (!frame.rematrixFlags[band]) continue;
            const unsigned first = bounds[band];
            const unsigned last =
                std::min({bounds[band + 1U], coupledEnd,
                          frame.channels[0].end,
                          frame.channels[1].end});
            for (unsigned bin = first; bin < last; ++bin) {
                const double middle = coefficients[0][bin];
                const double side = coefficients[1][bin];
                coefficients[0][bin] = middle + side;
                coefficients[1][bin] = middle - side;
            }
        }
    }

    void Downmix(
        const FrameState& frame,
        const std::array<std::array<double, kBlockSamples>,
                         kMaximumChannels>& pcm,
        unsigned outputOffset, AudioFrame& output) {
        for (unsigned sample = 0; sample < kBlockSamples; ++sample) {
            double left = 0.0;
            double right = 0.0;
            const auto addLeft = [&](unsigned channel, double weight) {
                left += pcm[channel][sample] * weight;
            };
            const auto addRight = [&](unsigned channel, double weight) {
                right += pcm[channel][sample] * weight;
            };
            switch (frame.audioCodingMode) {
            case 0:
                addLeft(0, 1.0);
                addRight(1, 1.0);
                break;
            case 1:
                addLeft(0, 0.7071067811865476);
                addRight(0, 0.7071067811865476);
                break;
            case 2:
                addLeft(0, 1.0);
                addRight(1, 1.0);
                break;
            case 3:
                addLeft(0, 1.0);
                addLeft(1, frame.centerMix);
                addRight(2, 1.0);
                addRight(1, frame.centerMix);
                break;
            case 4:
                addLeft(0, 1.0);
                addLeft(2, frame.surroundMix * 0.7071067811865476);
                addRight(1, 1.0);
                addRight(2, frame.surroundMix * 0.7071067811865476);
                break;
            case 5:
                addLeft(0, 1.0);
                addLeft(1, frame.centerMix);
                addLeft(3, frame.surroundMix * 0.7071067811865476);
                addRight(2, 1.0);
                addRight(1, frame.centerMix);
                addRight(3, frame.surroundMix * 0.7071067811865476);
                break;
            case 6:
                addLeft(0, 1.0);
                addLeft(2, frame.surroundMix);
                addRight(1, 1.0);
                addRight(3, frame.surroundMix);
                break;
            case 7:
                addLeft(0, 1.0);
                addLeft(1, frame.centerMix);
                addLeft(3, frame.surroundMix);
                addRight(2, 1.0);
                addRight(1, frame.centerMix);
                addRight(4, frame.surroundMix);
                break;
            default:
                break;
            }
            const std::size_t destination =
                static_cast<std::size_t>(outputOffset + sample) * 2U;
            output.samples[destination] =
                static_cast<float>(left);
            output.samples[destination + 1U] =
                static_cast<float>(right);
        }
    }

    bool DecodeBlock(BitReader& bits, FrameState& frame, unsigned block,
                     AudioFrame& output) {
        std::array<bool, kFullBandwidthChannels> shortBlocks{};
        std::array<bool, kFullBandwidthChannels> ditherFlags{};
        for (unsigned channel = 0;
             channel < frame.fullBandwidthChannels; ++channel) {
            if (!ReadFlag(bits, shortBlocks[channel]))
                return Fail(L"Truncated AC-3 block-switch flags");
        }
        for (unsigned channel = 0;
             channel < frame.fullBandwidthChannels; ++channel) {
            if (!ReadFlag(bits, ditherFlags[channel]))
                return Fail(L"Truncated AC-3 dither flags");
        }
        bool dynamicRangeExists = false;
        if (!ReadFlag(bits, dynamicRangeExists))
            return Fail(L"Truncated AC-3 dynamic range flag");
        if (dynamicRangeExists &&
            !ReadBits(bits, 8, frame.dynamicRange)) {
            return Fail(L"Truncated AC-3 dynamic range value");
        }
        if (block == 0 && !dynamicRangeExists) frame.dynamicRange = 0;
        if (frame.audioCodingMode == 0) {
            bool secondExists = false;
            if (!ReadFlag(bits, secondExists))
                return Fail(L"Truncated AC-3 second dynamic range flag");
            if (secondExists &&
                !ReadBits(bits, 8, frame.dynamicRange2)) {
                return Fail(L"Truncated AC-3 second dynamic range value");
            }
            if (block == 0 && !secondExists) frame.dynamicRange2 = 0;
        }
        if (!ParseCoupling(bits, frame, block) ||
            !ParseRematrix(bits, frame, block) ||
            !ParseExponents(bits, frame, block) ||
            !ParseBitAllocation(bits, frame, block)) {
            return false;
        }
        bool skipExists = false;
        if (!ReadFlag(bits, skipExists))
            return Fail(L"Truncated AC-3 skip-field flag");
        if (skipExists) {
            unsigned bytes = 0;
            if (!ReadBits(bits, 9, bytes) ||
                !bits.SkipBits(static_cast<std::size_t>(bytes) * 8U)) {
                return Fail(L"Truncated AC-3 skip field");
            }
        }

        std::array<std::array<double, kCoefficientCount>,
                   kMaximumChannels>
            coefficients{};
        if (!DecodeCoefficients(bits, frame, ditherFlags, coefficients))
            return false;
        ApplyRematrix(frame, coefficients);

        const double primaryGain = DynamicRangeGain(frame.dynamicRange);
        for (unsigned channel = 0;
             channel < frame.fullBandwidthChannels; ++channel) {
            const double gain =
                frame.audioCodingMode == 0 && channel == 1
                    ? DynamicRangeGain(frame.dynamicRange2)
                    : primaryGain;
            for (double& value : coefficients[channel]) value *= gain;
        }
        if (frame.lfeOn) {
            for (double& value :
                 coefficients[frame.fullBandwidthChannels]) {
                value *= primaryGain;
            }
        }

        std::array<std::array<double, kBlockSamples>,
                   kMaximumChannels>
            pcm{};
        for (unsigned channel = 0;
             channel < frame.fullBandwidthChannels; ++channel) {
            pcm[channel] = InverseTransform(
                coefficients[channel], shortBlocks[channel],
                overlap[channel]);
        }
        if (frame.lfeOn) {
            pcm[frame.fullBandwidthChannels] = InverseTransform(
                coefficients[frame.fullBandwidthChannels], false,
                overlap[frame.fullBandwidthChannels]);
        }
        Downmix(frame, pcm, block * kBlockSamples, output);
        return true;
    }

    bool DecodeFrame(const EncodedSample& sample, AudioFrame& output) {
        if (track.trackId == 0)
            return Fail(L"The AC-3 decoder is not initialized");
        if (sample.trackId != track.trackId)
            return Fail(L"Ac3Decoder received a sample for the wrong track");
        if (sample.bytes.size() < 7)
            return Fail(L"The AC-3 packet is too small");

        BitReader bits(sample.bytes);
        FrameState frame;
        if (!ParseHeader(bits, sample, frame)) return false;
        if (track.sampleRate > 0 &&
            track.sampleRate != kSampleRates[frame.sampleRateCode]) {
            return Fail(L"The container and AC-3 sample rates do not match");
        }
        if (track.channels > 0 &&
            track.channels !=
                static_cast<int>(frame.fullBandwidthChannels +
                                 (frame.lfeOn ? 1U : 0U))) {
            return Fail(L"The container and AC-3 channel counts do not match");
        }

        output = {};
        output.sampleRate = kSampleRates[frame.sampleRateCode];
        output.channels = 2;
        output.channelMask = 0x3;
        output.pts = sample.PtsSeconds();
        output.samples.resize(
            static_cast<std::size_t>(kAudioBlocks) *
            kBlockSamples * 2U);
        for (unsigned block = 0; block < kAudioBlocks; ++block) {
            if (!DecodeBlock(bits, frame, block, output)) {
                output = {};
                return false;
            }
        }
        if (bits.BitPosition() + 17U > sample.bytes.size() * 8U) {
            output = {};
            return Fail(L"The AC-3 audio blocks overrun the frame payload");
        }
        error.clear();
        return true;
    }

    double SecondsForBytes(std::size_t bytes) const {
        return averageBytesPerSecond != 0
                   ? static_cast<double>(bytes) /
                         static_cast<double>(averageBytesPerSecond)
                   : 0.0;
    }

    bool DecodeChunk(const EncodedSample& sample, AudioFrame& output) {
        if (track.trackId == 0)
            return Fail(L"The AC-3 decoder is not initialized");
        if (sample.trackId != track.trackId)
            return Fail(L"Ac3Decoder received a sample for the wrong track");
        if (sample.bytes.empty())
            return Fail(L"The AC-3 packet is empty");

        if (!pendingTimestamp) {
            pendingPts = sample.PtsSeconds();
            pendingTimestamp = true;
        }
        pendingBytes.insert(pendingBytes.end(), sample.bytes.begin(),
                            sample.bytes.end());
        output = {};

        for (;;) {
            const auto sync = std::search(
                pendingBytes.begin(), pendingBytes.end(),
                kSyncWord.begin(), kSyncWord.end());
            if (sync == pendingBytes.end()) {
                // Preserve a trailing 0x0b because the 0x77 half of the sync
                // word can arrive in the next AVI chunk.
                const bool keepPrefix =
                    !pendingBytes.empty() && pendingBytes.back() == 0x0b;
                const std::size_t discarded =
                    pendingBytes.size() - (keepPrefix ? 1U : 0U);
                pendingPts += SecondsForBytes(discarded);
                if (keepPrefix) {
                    pendingBytes.erase(pendingBytes.begin(),
                                       pendingBytes.end() - 1);
                } else {
                    pendingBytes.clear();
                }
                error.clear();
                return true;
            }

            const std::size_t skipped =
                static_cast<std::size_t>(sync - pendingBytes.begin());
            if (skipped != 0) {
                pendingBytes.erase(pendingBytes.begin(), sync);
                pendingPts += SecondsForBytes(skipped);
            }
            if (pendingBytes.size() < 5U) {
                error.clear();
                return true;
            }

            const unsigned sampleRateCode = pendingBytes[4] >> 6U;
            const unsigned frameSizeCode = pendingBytes[4] & 0x3fU;
            const int frameWords =
                FrameSizeWords(sampleRateCode, frameSizeCode);
            if (frameWords <= 0) {
                pendingBytes.erase(pendingBytes.begin());
                pendingPts += SecondsForBytes(1);
                continue;
            }
            const std::size_t frameBytes =
                static_cast<std::size_t>(frameWords) * 2U;
            if (pendingBytes.size() < frameBytes) {
                error.clear();
                return true;
            }

            EncodedSample frameSample = sample;
            frameSample.bytes.assign(pendingBytes.begin(),
                                     pendingBytes.begin() + frameBytes);
            AudioFrame decoded;
            if (!DecodeFrame(frameSample, decoded)) {
                output = {};
                return false;
            }
            decoded.pts = pendingPts;
            if (output.samples.empty()) {
                output.pts = decoded.pts;
                output.sampleRate = decoded.sampleRate;
                output.channels = decoded.channels;
                output.channelMask = decoded.channelMask;
            }
            output.samples.insert(output.samples.end(),
                                  decoded.samples.begin(),
                                  decoded.samples.end());
            pendingBytes.erase(pendingBytes.begin(),
                               pendingBytes.begin() + frameBytes);
            pendingPts +=
                static_cast<double>(kAudioBlocks * kBlockSamples) /
                static_cast<double>(decoded.sampleRate);
        }
    }

    bool Decode(const EncodedSample& sample, AudioFrame& output) {
        return chunkedWaveInput ? DecodeChunk(sample, output)
                                : DecodeFrame(sample, output);
    }

    void Reset() {
        for (auto& channel : overlap) channel.fill(0.0);
        ditherState = 0x12345678U;
        pendingBytes.clear();
        pendingPts = 0.0;
        pendingTimestamp = false;
        error.clear();
    }
};

Ac3Decoder::Ac3Decoder() : impl_(std::make_unique<Impl>()) {}
Ac3Decoder::~Ac3Decoder() = default;

bool Ac3Decoder::Initialize(const TrackInfo& track) {
    impl_->Reset();
    impl_->track = {};
    impl_->chunkedWaveInput = false;
    impl_->averageBytesPerSecond = 0;
    impl_->description.clear();
    if (track.codec != CodecId::Ac3 ||
        (track.sampleRate != 0 &&
         track.sampleRate != 48'000 &&
         track.sampleRate != 44'100 &&
         track.sampleRate != 32'000) ||
        track.channels < 1 || track.channels > 6) {
        return impl_->Fail(L"Ac3Decoder received an invalid AC-3 track");
    }
    impl_->track = track;
    if (track.codecPrivate.size() >= 16U &&
        track.codecPrivate[0] == 0x00 &&
        track.codecPrivate[1] == 0x20) {
        impl_->chunkedWaveInput = true;
        impl_->averageBytesPerSecond =
            static_cast<std::uint32_t>(track.codecPrivate[8]) |
            (static_cast<std::uint32_t>(track.codecPrivate[9]) << 8U) |
            (static_cast<std::uint32_t>(track.codecPrivate[10]) << 16U) |
            (static_cast<std::uint32_t>(track.codecPrivate[11]) << 24U);
    }
    impl_->description =
        L"Native ATSC A/52 AC-3 " +
        std::to_wstring(track.sampleRate) + L" Hz " +
        std::to_wstring(track.channels) + L" ch -> stereo";
    impl_->error.clear();
    return true;
}

bool Ac3Decoder::Decode(const EncodedSample& sample, AudioFrame& frame) {
    return impl_->Decode(sample, frame);
}

void Ac3Decoder::Reset() { impl_->Reset(); }

const std::wstring& Ac3Decoder::Description() const noexcept {
    return impl_->description;
}

const std::wstring& Ac3Decoder::LastError() const noexcept {
    return impl_->error;
}

}  // namespace movieplayer::codec::ac3
