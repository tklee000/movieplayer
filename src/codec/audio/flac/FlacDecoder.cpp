#include "codec/audio/flac/FlacDecoder.h"

#include "codec/core/BitReader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace movieplayer::codec::flac {
namespace {

constexpr std::size_t kStreamInfoBytes = 34;
constexpr std::size_t kMaximumFrameBytes = 32U * 1024U * 1024U;
constexpr unsigned kMaximumChannels = 8;
constexpr unsigned kMaximumBlockSize = 65'535;

struct StreamInfo {
    unsigned minimumBlockSize = 0;
    unsigned maximumBlockSize = 0;
    unsigned minimumFrameSize = 0;
    unsigned maximumFrameSize = 0;
    unsigned sampleRate = 0;
    unsigned channels = 0;
    unsigned bitsPerSample = 0;
    std::uint64_t totalSamples = 0;
};

struct FrameHeader {
    bool variableBlockSize = false;
    unsigned blockSize = 0;
    unsigned sampleRate = 0;
    unsigned channels = 0;
    unsigned channelAssignment = 0;
    unsigned bitsPerSample = 0;
    std::uint64_t codedNumber = 0;
};

std::uint64_t ReadBigEndian(const std::uint8_t* data, std::size_t size) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size; ++i)
        value = (value << 8U) | data[i];
    return value;
}

std::uint8_t Crc8(const std::uint8_t* data, std::size_t size) {
    std::uint8_t crc = 0;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = static_cast<std::uint8_t>(
                (crc & 0x80U) != 0
                    ? static_cast<unsigned>(crc << 1U) ^ 0x07U
                    : static_cast<unsigned>(crc << 1U));
        }
    }
    return crc;
}

std::uint16_t Crc16(const std::uint8_t* data, std::size_t size) {
    std::uint16_t crc = 0;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<std::uint16_t>(data[i]) << 8U;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = static_cast<std::uint16_t>(
                (crc & 0x8000U) != 0
                    ? static_cast<unsigned>(crc << 1U) ^ 0x8005U
                    : static_cast<unsigned>(crc << 1U));
        }
    }
    return crc;
}

bool ReadUnsigned(BitReader& bits, unsigned count, std::uint64_t& value) {
    return bits.ReadBits64(count, value);
}

bool ReadUnsigned(BitReader& bits, unsigned count, unsigned& value) {
    std::uint32_t result = 0;
    if (!bits.ReadBits(count, result)) return false;
    value = result;
    return true;
}

bool ReadSigned(BitReader& bits, unsigned count, std::int64_t& value) {
    if (count == 0) {
        value = 0;
        return true;
    }
    if (count > 62) return false;
    std::uint64_t raw = 0;
    if (!ReadUnsigned(bits, count, raw)) return false;
    const std::uint64_t sign = std::uint64_t{1} << (count - 1U);
    value = static_cast<std::int64_t>(raw);
    if ((raw & sign) != 0)
        value -= std::int64_t{1} << count;
    return true;
}

bool ReadUnary(BitReader& bits, std::uint64_t limit,
               std::uint64_t& zeroCount) {
    zeroCount = 0;
    bool value = false;
    while (bits.ReadBit(value)) {
        if (value) return true;
        if (zeroCount == limit) return false;
        ++zeroCount;
    }
    return false;
}

bool FitsSignedBits(std::int64_t value, unsigned bits) {
    if (bits == 0 || bits > 63) return false;
    const std::int64_t minimum =
        -(std::int64_t{1} << (bits - 1U));
    const std::int64_t maximum =
        (std::int64_t{1} << (bits - 1U)) - 1;
    return value >= minimum && value <= maximum;
}

bool AddWithoutOverflow(std::int64_t first, std::int64_t second,
                        std::int64_t& result) {
    if ((second > 0 &&
         first > (std::numeric_limits<std::int64_t>::max)() - second) ||
        (second < 0 &&
         first < (std::numeric_limits<std::int64_t>::min)() - second)) {
        return false;
    }
    result = first + second;
    return true;
}

bool ShiftLeftWithoutOverflow(std::int64_t value, unsigned shift,
                              std::int64_t& result) {
    if (shift == 0) {
        result = value;
        return true;
    }
    if (shift >= 63) return false;
    const std::int64_t minimum =
        (std::numeric_limits<std::int64_t>::min)() /
        (std::int64_t{1} << shift);
    const std::int64_t maximum =
        (std::numeric_limits<std::int64_t>::max)() /
        (std::int64_t{1} << shift);
    if (value < minimum || value > maximum) return false;
    result = value * (std::int64_t{1} << shift);
    return true;
}

bool ParseStreamInfoBytes(const std::uint8_t* data, std::size_t size,
                          StreamInfo& info) {
    if (!data || size != kStreamInfoBytes) return false;
    info = {};
    info.minimumBlockSize =
        static_cast<unsigned>(ReadBigEndian(data, 2));
    info.maximumBlockSize =
        static_cast<unsigned>(ReadBigEndian(data + 2, 2));
    info.minimumFrameSize =
        static_cast<unsigned>(ReadBigEndian(data + 4, 3));
    info.maximumFrameSize =
        static_cast<unsigned>(ReadBigEndian(data + 7, 3));
    const std::uint64_t packed = ReadBigEndian(data + 10, 8);
    info.sampleRate = static_cast<unsigned>((packed >> 44U) & 0xfffffU);
    info.channels = static_cast<unsigned>((packed >> 41U) & 7U) + 1U;
    info.bitsPerSample =
        static_cast<unsigned>((packed >> 36U) & 31U) + 1U;
    info.totalSamples = packed & 0xfffffffffULL;
    return info.minimumBlockSize >= 16 &&
           info.maximumBlockSize >= info.minimumBlockSize &&
           info.maximumBlockSize <= kMaximumBlockSize &&
           info.sampleRate != 0 &&
           info.channels >= 1 && info.channels <= kMaximumChannels &&
           info.bitsPerSample >= 4 && info.bitsPerSample <= 32 &&
           (info.minimumFrameSize == 0 ||
            info.maximumFrameSize == 0 ||
            info.minimumFrameSize <= info.maximumFrameSize);
}

bool ParseCodecPrivate(const std::vector<std::uint8_t>& data,
                       StreamInfo& info) {
    if (data.size() == kStreamInfoBytes)
        return ParseStreamInfoBytes(data.data(), data.size(), info);
    if (data.size() < 4 ||
        data[0] != 'f' || data[1] != 'L' ||
        data[2] != 'a' || data[3] != 'C') {
        return false;
    }
    std::size_t position = 4;
    bool first = true;
    bool foundStreamInfo = false;
    bool foundLast = false;
    while (position + 4U <= data.size()) {
        const std::uint8_t type = data[position] & 0x7fU;
        const bool last = (data[position] & 0x80U) != 0;
        const std::size_t payloadSize =
            static_cast<std::size_t>(ReadBigEndian(
                data.data() + position + 1U, 3));
        position += 4U;
        if (payloadSize > data.size() - position ||
            (first && type != 0)) {
            return false;
        }
        if (type == 0) {
            if (foundStreamInfo ||
                !ParseStreamInfoBytes(data.data() + position,
                                      payloadSize, info)) {
                return false;
            }
            foundStreamInfo = true;
        }
        position += payloadSize;
        first = false;
        if (last) {
            foundLast = true;
            break;
        }
    }
    return foundStreamInfo && foundLast && position == data.size();
}

bool ReadCodedNumber(BitReader& bits, std::uint64_t& value) {
    unsigned first = 0;
    if (!ReadUnsigned(bits, 8, first)) return false;
    if ((first & 0x80U) == 0) {
        value = first;
        return true;
    }

    unsigned length = 0;
    std::uint8_t mask = 0x80U;
    while ((first & mask) != 0 && length < 8) {
        ++length;
        mask >>= 1U;
    }
    if (length < 2 || length > 7 ||
        (first & mask) != 0) {
        return false;
    }
    value = first & (mask - 1U);
    for (unsigned i = 1; i < length; ++i) {
        unsigned byte = 0;
        if (!ReadUnsigned(bits, 8, byte) ||
            (byte & 0xc0U) != 0x80U) {
            return false;
        }
        value = (value << 6U) | (byte & 0x3fU);
    }
    constexpr std::array<std::uint64_t, 8> minimum = {
        0, 0, 0x80, 0x800, 0x10000,
        0x200000, 0x4000000, 0x80000000ULL};
    return value >= minimum[length] && value <= 0xfffffffffULL;
}

unsigned DecodeBlockSize(unsigned code) {
    if (code == 1) return 192;
    if (code >= 2 && code <= 5)
        return 576U << (code - 2U);
    if (code >= 8 && code <= 15)
        return 256U << (code - 8U);
    return 0;
}

unsigned DecodeSampleRate(unsigned code) {
    constexpr std::array<unsigned, 16> rates = {
        0, 88'200, 176'400, 192'000,
        8'000, 16'000, 22'050, 24'000,
        32'000, 44'100, 48'000, 96'000,
        0, 0, 0, 0};
    return rates[code];
}

unsigned DecodeBitsPerSample(unsigned code,
                             unsigned streamBitsPerSample) {
    constexpr std::array<unsigned, 8> depths = {
        0, 8, 12, 0, 16, 20, 24, 32};
    return code == 0 ? streamBitsPerSample : depths[code];
}

bool ParseFrameHeader(BitReader& bits,
                      const std::vector<std::uint8_t>& frame,
                      const StreamInfo& streamInfo,
                      FrameHeader& header) {
    unsigned sync = 0;
    bool variable = false;
    unsigned blockCode = 0;
    unsigned rateCode = 0;
    unsigned channelAssignment = 0;
    unsigned depthCode = 0;
    bool reserved = false;
    if (!ReadUnsigned(bits, 15, sync) || sync != 0x7ffcU ||
        !bits.ReadBit(variable) ||
        !ReadUnsigned(bits, 4, blockCode) || blockCode == 0 ||
        !ReadUnsigned(bits, 4, rateCode) || rateCode == 15 ||
        !ReadUnsigned(bits, 4, channelAssignment) ||
        channelAssignment > 10 ||
        !ReadUnsigned(bits, 3, depthCode) || depthCode == 3 ||
        !bits.ReadBit(reserved) || reserved ||
        !ReadCodedNumber(bits, header.codedNumber)) {
        return false;
    }

    unsigned blockSize = DecodeBlockSize(blockCode);
    if (blockCode == 6) {
        unsigned value = 0;
        if (!ReadUnsigned(bits, 8, value)) return false;
        blockSize = value + 1U;
    } else if (blockCode == 7) {
        unsigned value = 0;
        if (!ReadUnsigned(bits, 16, value) || value == 65'535U)
            return false;
        blockSize = value + 1U;
    }

    unsigned sampleRate = DecodeSampleRate(rateCode);
    if (rateCode == 0) {
        sampleRate = streamInfo.sampleRate;
    } else if (rateCode == 12) {
        unsigned value = 0;
        if (!ReadUnsigned(bits, 8, value)) return false;
        sampleRate = value * 1000U;
    } else if (rateCode == 13) {
        if (!ReadUnsigned(bits, 16, sampleRate)) return false;
    } else if (rateCode == 14) {
        unsigned value = 0;
        if (!ReadUnsigned(bits, 16, value)) return false;
        sampleRate = value * 10U;
    }

    if ((bits.BitPosition() & 7U) != 0) return false;
    const std::size_t headerBytes = bits.BitPosition() / 8U;
    unsigned storedCrc = 0;
    if (!ReadUnsigned(bits, 8, storedCrc) ||
        headerBytes + 1U > frame.size() ||
        Crc8(frame.data(), headerBytes + 1U) != 0) {
        return false;
    }

    const unsigned channels =
        channelAssignment <= 7 ? channelAssignment + 1U : 2U;
    const unsigned bitsPerSample =
        DecodeBitsPerSample(depthCode, streamInfo.bitsPerSample);
    if (blockSize == 0 || blockSize > streamInfo.maximumBlockSize ||
        sampleRate == 0 || sampleRate != streamInfo.sampleRate ||
        channels != streamInfo.channels ||
        bitsPerSample == 0 ||
        bitsPerSample != streamInfo.bitsPerSample) {
        return false;
    }
    header.variableBlockSize = variable;
    header.blockSize = blockSize;
    header.sampleRate = sampleRate;
    header.channels = channels;
    header.channelAssignment = channelAssignment;
    header.bitsPerSample = bitsPerSample;
    return true;
}

bool DecodeResidual(BitReader& bits, unsigned blockSize,
                    unsigned predictorOrder,
                    std::vector<std::int64_t>& samples) {
    unsigned method = 0;
    unsigned partitionOrder = 0;
    if (!ReadUnsigned(bits, 2, method) || method > 1 ||
        !ReadUnsigned(bits, 4, partitionOrder)) {
        return false;
    }
    const unsigned partitions = 1U << partitionOrder;
    if ((blockSize & (partitions - 1U)) != 0) return false;
    const unsigned partitionSamples = blockSize >> partitionOrder;
    if (partitionSamples <= predictorOrder) return false;

    const unsigned parameterBits = method == 0 ? 4U : 5U;
    const unsigned escape = (1U << parameterBits) - 1U;
    std::size_t output = predictorOrder;
    for (unsigned partition = 0; partition < partitions; ++partition) {
        unsigned parameter = 0;
        if (!ReadUnsigned(bits, parameterBits, parameter)) return false;
        const unsigned count =
            partitionSamples -
            (partition == 0 ? predictorOrder : 0U);
        if (parameter == escape) {
            unsigned rawBits = 0;
            if (!ReadUnsigned(bits, 5, rawBits) || rawBits > 32)
                return false;
            for (unsigned i = 0; i < count; ++i) {
                std::int64_t value = 0;
                if (!ReadSigned(bits, rawBits, value) ||
                    output >= samples.size()) {
                    return false;
                }
                samples[output++] = value;
            }
        } else {
            const std::uint64_t maximumQuotient =
                0xfffffffeULL >> parameter;
            for (unsigned i = 0; i < count; ++i) {
                std::uint64_t quotient = 0;
                std::uint64_t remainder = 0;
                if (!ReadUnary(bits, maximumQuotient, quotient) ||
                    !ReadUnsigned(bits, parameter, remainder) ||
                    output >= samples.size()) {
                    return false;
                }
                const std::uint64_t folded =
                    (quotient << parameter) | remainder;
                samples[output++] =
                    (folded & 1U) != 0
                        ? -static_cast<std::int64_t>(
                              (folded >> 1U) + 1U)
                        : static_cast<std::int64_t>(folded >> 1U);
            }
        }
    }
    return output == samples.size();
}

bool RestoreFixed(unsigned order, unsigned bitsPerSample,
                  std::vector<std::int64_t>& samples) {
    for (std::size_t i = order; i < samples.size(); ++i) {
        std::int64_t prediction = 0;
        switch (order) {
        case 0:
            break;
        case 1:
            prediction = samples[i - 1U];
            break;
        case 2:
            prediction =
                2 * samples[i - 1U] - samples[i - 2U];
            break;
        case 3:
            prediction =
                3 * samples[i - 1U] -
                3 * samples[i - 2U] +
                samples[i - 3U];
            break;
        case 4:
            prediction =
                4 * samples[i - 1U] -
                6 * samples[i - 2U] +
                4 * samples[i - 3U] -
                samples[i - 4U];
            break;
        default:
            return false;
        }
        std::int64_t restored = 0;
        if (!AddWithoutOverflow(prediction, samples[i], restored) ||
            !FitsSignedBits(restored, bitsPerSample)) {
            return false;
        }
        samples[i] = restored;
    }
    return true;
}

bool RestoreLpc(const std::array<std::int64_t, 32>& coefficients,
                unsigned order, int shift, unsigned bitsPerSample,
                std::vector<std::int64_t>& samples) {
    for (std::size_t i = order; i < samples.size(); ++i) {
        std::int64_t sum = 0;
        for (unsigned coefficient = 0;
             coefficient < order; ++coefficient) {
            const std::int64_t product =
                coefficients[coefficient] *
                samples[i - coefficient - 1U];
            if (!AddWithoutOverflow(sum, product, sum)) return false;
        }
        std::int64_t prediction = 0;
        if (shift >= 0) {
            prediction = sum >> static_cast<unsigned>(shift);
        } else if (!ShiftLeftWithoutOverflow(
                       sum, static_cast<unsigned>(-shift), prediction)) {
            return false;
        }
        std::int64_t restored = 0;
        if (!AddWithoutOverflow(prediction, samples[i], restored) ||
            !FitsSignedBits(restored, bitsPerSample)) {
            return false;
        }
        samples[i] = restored;
    }
    return true;
}

bool DecodeSubframe(BitReader& bits, unsigned blockSize,
                    unsigned bitsPerSample,
                    std::vector<std::int64_t>& samples) {
    bool padding = false;
    unsigned type = 0;
    bool hasWastedBits = false;
    if (!bits.ReadBit(padding) || padding ||
        !ReadUnsigned(bits, 6, type) ||
        !bits.ReadBit(hasWastedBits)) {
        return false;
    }
    unsigned wastedBits = 0;
    if (hasWastedBits) {
        std::uint64_t zeros = 0;
        if (!ReadUnary(bits, bitsPerSample, zeros) ||
            zeros >= bitsPerSample) {
            return false;
        }
        wastedBits = static_cast<unsigned>(zeros) + 1U;
    }
    if (wastedBits >= bitsPerSample) return false;
    const unsigned codedBits = bitsPerSample - wastedBits;
    samples.assign(blockSize, 0);

    if (type == 0) {
        std::int64_t value = 0;
        if (!ReadSigned(bits, codedBits, value)) return false;
        std::fill(samples.begin(), samples.end(), value);
    } else if (type == 1) {
        for (std::int64_t& value : samples) {
            if (!ReadSigned(bits, codedBits, value)) return false;
        }
    } else if (type >= 8 && type <= 12) {
        const unsigned order = type - 8U;
        if (order > blockSize) return false;
        for (unsigned i = 0; i < order; ++i) {
            if (!ReadSigned(bits, codedBits, samples[i])) return false;
        }
        if (!DecodeResidual(bits, blockSize, order, samples) ||
            !RestoreFixed(order, codedBits, samples)) {
            return false;
        }
    } else if (type >= 32) {
        const unsigned order = type - 31U;
        if (order > 32 || order > blockSize) return false;
        for (unsigned i = 0; i < order; ++i) {
            if (!ReadSigned(bits, codedBits, samples[i])) return false;
        }
        unsigned encodedPrecision = 0;
        std::int64_t shiftValue = 0;
        if (!ReadUnsigned(bits, 4, encodedPrecision) ||
            encodedPrecision == 15 ||
            !ReadSigned(bits, 5, shiftValue)) {
            return false;
        }
        const unsigned precision = encodedPrecision + 1U;
        std::array<std::int64_t, 32> coefficients{};
        for (unsigned i = 0; i < order; ++i) {
            if (!ReadSigned(bits, precision, coefficients[i]))
                return false;
        }
        if (!DecodeResidual(bits, blockSize, order, samples) ||
            !RestoreLpc(coefficients, order,
                        static_cast<int>(shiftValue),
                        codedBits, samples)) {
            return false;
        }
    } else {
        return false;
    }

    if (wastedBits != 0) {
        for (std::int64_t& value : samples) {
            std::int64_t shifted = 0;
            if (!ShiftLeftWithoutOverflow(value, wastedBits, shifted) ||
                !FitsSignedBits(shifted, bitsPerSample)) {
                return false;
            }
            value = shifted;
        }
    }
    return true;
}

bool RestoreStereo(unsigned assignment,
                   std::vector<std::vector<std::int64_t>>& channels,
                   unsigned bitsPerSample) {
    if (assignment < 8 || assignment > 10 ||
        channels.size() != 2 ||
        channels[0].size() != channels[1].size()) {
        return assignment <= 7;
    }
    for (std::size_t i = 0; i < channels[0].size(); ++i) {
        const std::int64_t first = channels[0][i];
        const std::int64_t second = channels[1][i];
        std::int64_t left = 0;
        std::int64_t right = 0;
        if (assignment == 8) {
            left = first;
            if (!AddWithoutOverflow(first, -second, right))
                return false;
        } else if (assignment == 9) {
            right = second;
            if (!AddWithoutOverflow(first, second, left))
                return false;
        } else {
            std::int64_t doubledMid = 0;
            if (!ShiftLeftWithoutOverflow(first, 1, doubledMid))
                return false;
            if ((second & 1) != 0 &&
                !AddWithoutOverflow(doubledMid, 1, doubledMid)) {
                return false;
            }
            std::int64_t leftSum = 0;
            std::int64_t rightSum = 0;
            if (!AddWithoutOverflow(doubledMid, second, leftSum) ||
                !AddWithoutOverflow(doubledMid, -second, rightSum)) {
                return false;
            }
            left = leftSum >> 1;
            right = rightSum >> 1;
        }
        if (!FitsSignedBits(left, bitsPerSample) ||
            !FitsSignedBits(right, bitsPerSample)) {
            return false;
        }
        channels[0][i] = left;
        channels[1][i] = right;
    }
    return true;
}

void DownmixToStereo(
    const std::vector<std::vector<std::int64_t>>& channels,
    unsigned bitsPerSample, AudioFrame& output) {
    const std::size_t blockSize = channels.front().size();
    output.samples.resize(blockSize * 2U);
    const double scale = std::ldexp(1.0, 1 - static_cast<int>(bitsPerSample));
    for (std::size_t sample = 0; sample < blockSize; ++sample) {
        double left = 0.0;
        double right = 0.0;
        const auto addLeft = [&](unsigned channel, double weight) {
            if (channel < channels.size())
                left += static_cast<double>(channels[channel][sample]) *
                        weight;
        };
        const auto addRight = [&](unsigned channel, double weight) {
            if (channel < channels.size())
                right += static_cast<double>(channels[channel][sample]) *
                         weight;
        };
        switch (channels.size()) {
        case 1:
            addLeft(0, 1.0);
            addRight(0, 1.0);
            break;
        case 2:
            addLeft(0, 1.0);
            addRight(1, 1.0);
            break;
        case 3:
            addLeft(0, 1.0);
            addRight(1, 1.0);
            addLeft(2, 0.7071067811865476);
            addRight(2, 0.7071067811865476);
            break;
        case 4:
            addLeft(0, 1.0);
            addRight(1, 1.0);
            addLeft(2, 0.7071067811865476);
            addRight(3, 0.7071067811865476);
            break;
        case 5:
            addLeft(0, 1.0);
            addRight(1, 1.0);
            addLeft(2, 0.7071067811865476);
            addRight(2, 0.7071067811865476);
            addLeft(3, 0.7071067811865476);
            addRight(4, 0.7071067811865476);
            break;
        case 6:
            addLeft(0, 1.0);
            addRight(1, 1.0);
            addLeft(2, 0.7071067811865476);
            addRight(2, 0.7071067811865476);
            addLeft(4, 0.7071067811865476);
            addRight(5, 0.7071067811865476);
            break;
        case 7:
            addLeft(0, 1.0);
            addRight(1, 1.0);
            addLeft(2, 0.7071067811865476);
            addRight(2, 0.7071067811865476);
            addLeft(4, 0.5);
            addRight(4, 0.5);
            addLeft(5, 0.7071067811865476);
            addRight(6, 0.7071067811865476);
            break;
        default:
            addLeft(0, 1.0);
            addRight(1, 1.0);
            addLeft(2, 0.7071067811865476);
            addRight(2, 0.7071067811865476);
            addLeft(4, 0.5);
            addRight(5, 0.5);
            addLeft(6, 0.7071067811865476);
            addRight(7, 0.7071067811865476);
            break;
        }
        output.samples[sample * 2U] =
            static_cast<float>(left * scale);
        output.samples[sample * 2U + 1U] =
            static_cast<float>(right * scale);
    }
}

}  // namespace

struct FlacDecoder::Impl {
    TrackInfo track;
    StreamInfo streamInfo;
    std::wstring description;
    std::wstring error;

    bool Fail(const std::wstring& message) {
        error = message;
        return false;
    }

    bool Initialize(const TrackInfo& candidate) {
        track = {};
        streamInfo = {};
        description.clear();
        error.clear();
        if (candidate.trackId == 0 ||
            candidate.codec != CodecId::Flac ||
            !ParseCodecPrivate(candidate.codecPrivate, streamInfo)) {
            return Fail(L"FlacDecoder received invalid FLAC STREAMINFO metadata");
        }
        if ((candidate.sampleRate > 0 &&
             candidate.sampleRate !=
                 static_cast<int>(streamInfo.sampleRate)) ||
            (candidate.channels > 0 &&
             candidate.channels !=
                 static_cast<int>(streamInfo.channels)) ||
            (candidate.bitsPerSample > 0 &&
             candidate.bitsPerSample !=
                 static_cast<int>(streamInfo.bitsPerSample))) {
            return Fail(
                L"The Matroska and FLAC STREAMINFO properties do not match");
        }
        track = candidate;
        description =
            L"Native IETF RFC 9639 FLAC " +
            std::to_wstring(streamInfo.sampleRate) + L" Hz " +
            std::to_wstring(streamInfo.bitsPerSample) + L"-bit " +
            std::to_wstring(streamInfo.channels) + L" ch -> stereo";
        return true;
    }

    bool Decode(const EncodedSample& sample, AudioFrame& output) {
        output = {};
        if (track.trackId == 0)
            return Fail(L"The FLAC decoder is not initialized");
        if (sample.trackId != track.trackId)
            return Fail(L"FlacDecoder received a sample for the wrong track");
        if (sample.bytes.size() < 8 ||
            sample.bytes.size() > kMaximumFrameBytes ||
            (streamInfo.maximumFrameSize != 0 &&
             sample.bytes.size() > streamInfo.maximumFrameSize)) {
            return Fail(L"The FLAC frame size is invalid");
        }
        if (Crc16(sample.bytes.data(), sample.bytes.size()) != 0)
            return Fail(L"The FLAC frame failed its CRC-16 check");

        BitReader bits(sample.bytes);
        FrameHeader header;
        if (!ParseFrameHeader(bits, sample.bytes, streamInfo, header))
            return Fail(L"The FLAC frame header is invalid");

        std::vector<std::vector<std::int64_t>> channels(header.channels);
        for (unsigned channel = 0; channel < header.channels; ++channel) {
            unsigned channelBits = header.bitsPerSample;
            if ((header.channelAssignment == 8 && channel == 1) ||
                (header.channelAssignment == 9 && channel == 0) ||
                (header.channelAssignment == 10 && channel == 1)) {
                ++channelBits;
            }
            if (!DecodeSubframe(bits, header.blockSize, channelBits,
                                channels[channel])) {
                return Fail(L"The FLAC subframe is invalid or truncated");
            }
        }

        while ((bits.BitPosition() & 7U) != 0) {
            bool padding = false;
            if (!bits.ReadBit(padding) || padding) {
                return Fail(L"The FLAC frame has invalid alignment padding");
            }
        }
        if (bits.BitPosition() / 8U + 2U != sample.bytes.size()) {
            return Fail(L"The FLAC subframes do not fill the frame payload");
        }
        if (!bits.SkipBits(16) ||
            !RestoreStereo(header.channelAssignment, channels,
                           header.bitsPerSample)) {
            return Fail(L"The FLAC channel decorrelation data is invalid");
        }

        output.sampleRate = static_cast<int>(header.sampleRate);
        output.channels = 2;
        output.channelMask = 0x3;
        output.pts = sample.PtsSeconds();
        DownmixToStereo(channels, header.bitsPerSample, output);
        error.clear();
        return true;
    }

    void Reset() { error.clear(); }
};

FlacDecoder::FlacDecoder() : impl_(std::make_unique<Impl>()) {}
FlacDecoder::~FlacDecoder() = default;

bool FlacDecoder::Initialize(const TrackInfo& track) {
    return impl_->Initialize(track);
}

bool FlacDecoder::Decode(const EncodedSample& sample, AudioFrame& frame) {
    return impl_->Decode(sample, frame);
}

void FlacDecoder::Reset() { impl_->Reset(); }

const std::wstring& FlacDecoder::Description() const noexcept {
    return impl_->description;
}

const std::wstring& FlacDecoder::LastError() const noexcept {
    return impl_->error;
}

}  // namespace movieplayer::codec::flac
