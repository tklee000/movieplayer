#include "codec/subtitle/VobSubDecoder.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace movieplayer::codec::subtitle {
namespace {

std::uint16_t Read16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[0]) << 8U) | data[1]);
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool ParseUnsigned(const std::string& text, std::size_t& position,
                   unsigned& value) {
    while (position < text.size() &&
           std::isspace(static_cast<unsigned char>(text[position]))) {
        ++position;
    }
    if (position >= text.size() ||
        !std::isdigit(static_cast<unsigned char>(text[position]))) {
        return false;
    }
    value = 0;
    while (position < text.size() &&
           std::isdigit(static_cast<unsigned char>(text[position]))) {
        const unsigned digit = static_cast<unsigned>(text[position] - '0');
        if (value > ((std::numeric_limits<unsigned>::max)() - digit) / 10U)
            return false;
        value = value * 10U + digit;
        ++position;
    }
    return true;
}

bool ParseHexColour(const std::string& text, std::size_t& position,
                    std::uint32_t& colour) {
    while (position < text.size() &&
           (std::isspace(static_cast<unsigned char>(text[position])) ||
            text[position] == ',')) {
        ++position;
    }
    colour = 0;
    unsigned digits = 0;
    while (position < text.size() && digits < 6) {
        const unsigned char c = static_cast<unsigned char>(text[position]);
        unsigned value = 0;
        if (c >= '0' && c <= '9') value = c - '0';
        else if (c >= 'a' && c <= 'f') value = c - 'a' + 10U;
        else if (c >= 'A' && c <= 'F') value = c - 'A' + 10U;
        else break;
        colour = (colour << 4U) | value;
        ++position;
        ++digits;
    }
    return digits == 6;
}

bool ParseCodecPrivate(const TrackInfo& track, int& canvasWidth,
                       int& canvasHeight,
                       std::array<std::uint32_t, 16>& palette) {
    const std::string text(track.codecPrivate.begin(), track.codecPrivate.end());
    const std::string lowered = Lower(text);
    const std::size_t sizeKey = lowered.find("size:");
    if (sizeKey == std::string::npos) return false;
    std::size_t position = sizeKey + 5;
    unsigned width = 0, height = 0;
    if (!ParseUnsigned(text, position, width)) return false;
    while (position < text.size() &&
           (std::isspace(static_cast<unsigned char>(text[position])) ||
            text[position] == 'x' || text[position] == 'X')) {
        ++position;
    }
    if (!ParseUnsigned(text, position, height) || width == 0 || height == 0 ||
        width > 4096 || height > 4096) {
        return false;
    }

    const std::size_t paletteKey = lowered.find("palette:");
    if (paletteKey == std::string::npos) return false;
    position = paletteKey + 8;
    for (std::uint32_t& colour : palette) {
        if (!ParseHexColour(text, position, colour)) return false;
    }
    canvasWidth = static_cast<int>(width);
    canvasHeight = static_cast<int>(height);
    return true;
}

class Nibbles {
public:
    Nibbles(const std::uint8_t* data, std::size_t limit, std::size_t position)
        : data_(data), limit_(limit), position_(position) {}

    bool Read(unsigned& value) {
        if (position_ >= limit_) return false;
        value = high_ ? data_[position_] >> 4U : data_[position_] & 0x0fU;
        if (!high_) ++position_;
        high_ = !high_;
        return true;
    }

    void AlignByte() {
        if (!high_) {
            ++position_;
            high_ = true;
        }
    }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t limit_ = 0;
    std::size_t position_ = 0;
    bool high_ = true;
};

bool ReadRun(Nibbles& input, unsigned& run, unsigned& colour) {
    unsigned code = 0, nibble = 0;
    if (!input.Read(code)) return false;
    if (code < 0x04U) {
        if (!input.Read(nibble)) return false;
        code = (code << 4U) | nibble;
        if (code < 0x10U) {
            if (!input.Read(nibble)) return false;
            code = (code << 4U) | nibble;
            if (code < 0x40U) {
                if (!input.Read(nibble)) return false;
                code = (code << 4U) | nibble;
            }
        }
    }
    run = code >> 2U;
    colour = code & 3U;
    return true;
}

}  // namespace

bool DecodeVobSubSample(const TrackInfo& track, const EncodedSample& sample,
                        VobSubFrame& frame, std::wstring& error) {
    frame = {};
    std::array<std::uint32_t, 16> palette{};
    int canvasWidth = 0, canvasHeight = 0;
    if (track.codec != CodecId::VobSub ||
        !ParseCodecPrivate(track, canvasWidth, canvasHeight, palette)) {
        error = L"The VobSub palette or canvas metadata is invalid";
        return false;
    }
    if (sample.bytes.size() < 8) {
        error = L"The VobSub SPU packet is truncated";
        return false;
    }
    const std::uint8_t* packet = sample.bytes.data();
    const std::size_t declaredSize = Read16(packet);
    const std::size_t packetSize =
        declaredSize == 0 ? sample.bytes.size() : declaredSize;
    const std::size_t controlOffset = Read16(packet + 2);
    if (packetSize > sample.bytes.size() || controlOffset < 4 ||
        controlOffset >= packetSize) {
        error = L"The VobSub SPU packet header is invalid";
        return false;
    }

    std::array<unsigned, 4> colourMap{0, 1, 2, 3};
    std::array<unsigned, 4> alpha{0, 15, 15, 15};
    std::array<std::size_t, 2> fieldOffset{};
    int x1 = -1, x2 = -1, y1 = -1, y2 = -1;
    bool haveOffsets = false;
    bool haveStart = false;
    bool haveStop = false;
    std::size_t sequence = controlOffset;
    std::vector<std::size_t> visited;
    for (unsigned sequenceCount = 0; sequenceCount < 64; ++sequenceCount) {
        if (sequence + 4 > packetSize ||
            std::find(visited.begin(), visited.end(), sequence) != visited.end()) {
            break;
        }
        visited.push_back(sequence);
        const unsigned date = Read16(packet + sequence);
        const std::size_t nextSequence = Read16(packet + sequence + 2);
        std::size_t position = sequence + 4;
        bool ended = false;
        while (position < packetSize && !ended) {
            const std::uint8_t command = packet[position++];
            switch (command) {
            case 0x00:
                frame.forced = true;
                [[fallthrough]];
            case 0x01:
                if (!haveStart) {
                    frame.startDelaySeconds = date * (1024.0 / 90000.0);
                    haveStart = true;
                }
                break;
            case 0x02:
                if (!haveStop) {
                    frame.endDelaySeconds = date * (1024.0 / 90000.0);
                    haveStop = true;
                }
                break;
            case 0x03:
                if (position + 2 > packetSize) {
                    error = L"The VobSub palette command is truncated";
                    return false;
                }
                colourMap[3] = packet[position] >> 4U;
                colourMap[2] = packet[position] & 0x0fU;
                colourMap[1] = packet[position + 1] >> 4U;
                colourMap[0] = packet[position + 1] & 0x0fU;
                position += 2;
                break;
            case 0x04:
                if (position + 2 > packetSize) {
                    error = L"The VobSub alpha command is truncated";
                    return false;
                }
                alpha[3] = packet[position] >> 4U;
                alpha[2] = packet[position] & 0x0fU;
                alpha[1] = packet[position + 1] >> 4U;
                alpha[0] = packet[position + 1] & 0x0fU;
                position += 2;
                break;
            case 0x05:
                if (position + 6 > packetSize) {
                    error = L"The VobSub coordinate command is truncated";
                    return false;
                }
                x1 = (packet[position] << 4U) | (packet[position + 1] >> 4U);
                x2 = ((packet[position + 1] & 0x0fU) << 8U) |
                     packet[position + 2];
                y1 = (packet[position + 3] << 4U) |
                     (packet[position + 4] >> 4U);
                y2 = ((packet[position + 4] & 0x0fU) << 8U) |
                     packet[position + 5];
                position += 6;
                break;
            case 0x06:
                if (position + 4 > packetSize) {
                    error = L"The VobSub pixel-offset command is truncated";
                    return false;
                }
                fieldOffset[0] = Read16(packet + position);
                fieldOffset[1] = Read16(packet + position + 2);
                haveOffsets = true;
                position += 4;
                break;
            case 0x07: {
                if (position + 2 > packetSize) {
                    error = L"The VobSub colour-change command is truncated";
                    return false;
                }
                const std::size_t commandSize = Read16(packet + position);
                if (commandSize < 2 || commandSize > packetSize - position) {
                    error = L"The VobSub colour-change command is invalid";
                    return false;
                }
                position += commandSize;
                break;
            }
            case 0xff:
                ended = true;
                break;
            default:
                error = L"The VobSub control sequence contains an unknown command";
                return false;
            }
        }
        if (nextSequence == sequence) break;
        if (nextSequence < controlOffset || nextSequence >= packetSize) {
            error = L"The VobSub control-sequence link is invalid";
            return false;
        }
        sequence = nextSequence;
    }

    if (x1 < 0 || y1 < 0 || x2 < x1 || y2 < y1 ||
        x2 >= canvasWidth || y2 >= canvasHeight || !haveOffsets ||
        fieldOffset[0] < 4 || fieldOffset[1] < 4 ||
        fieldOffset[0] >= controlOffset || fieldOffset[1] >= controlOffset) {
        error = L"The VobSub bitmap bounds or pixel offsets are invalid";
        return false;
    }
    const int width = x2 - x1 + 1;
    const int height = y2 - y1 + 1;
    const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
    if (pixelCount > 4096U * 4096U) {
        error = L"The VobSub bitmap is too large";
        return false;
    }

    std::array<Nibbles, 2> fields{
        Nibbles(packet, controlOffset, fieldOffset[0]),
        Nibbles(packet, controlOffset, fieldOffset[1]),
    };
    std::vector<std::uint8_t> colourIndices(pixelCount, 0);
    for (int y = 0; y < height; ++y) {
        Nibbles& input = fields[static_cast<unsigned>(y) & 1U];
        int x = 0;
        while (x < width) {
            unsigned run = 0, colour = 0;
            if (!ReadRun(input, run, colour)) {
                error = L"The VobSub RLE bitmap is truncated";
                return false;
            }
            if (run == 0) run = static_cast<unsigned>(width - x);
            run = (std::min)(run, static_cast<unsigned>(width - x));
            std::fill_n(colourIndices.begin() +
                            static_cast<std::size_t>(y) * width + x,
                        run, static_cast<std::uint8_t>(colour));
            x += static_cast<int>(run);
        }
        input.AlignByte();
    }

    frame.bitmap.canvasWidth = canvasWidth;
    frame.bitmap.canvasHeight = canvasHeight;
    frame.bitmap.x = x1;
    frame.bitmap.y = y1;
    frame.bitmap.width = width;
    frame.bitmap.height = height;
    frame.bitmap.bgra.resize(pixelCount * 4U);
    for (std::size_t i = 0; i < pixelCount; ++i) {
        const unsigned pixelCode = colourIndices[i];
        const std::uint32_t rgb = palette[colourMap[pixelCode] & 0x0fU];
        const unsigned opacity = (alpha[pixelCode] & 0x0fU) * 17U;
        const unsigned red = (rgb >> 16U) & 0xffU;
        const unsigned green = (rgb >> 8U) & 0xffU;
        const unsigned blue = rgb & 0xffU;
        frame.bitmap.bgra[i * 4U] =
            static_cast<std::uint8_t>((blue * opacity + 127U) / 255U);
        frame.bitmap.bgra[i * 4U + 1U] =
            static_cast<std::uint8_t>((green * opacity + 127U) / 255U);
        frame.bitmap.bgra[i * 4U + 2U] =
            static_cast<std::uint8_t>((red * opacity + 127U) / 255U);
        frame.bitmap.bgra[i * 4U + 3U] = static_cast<std::uint8_t>(opacity);
    }
    if (!haveStart) frame.startDelaySeconds = 0.0;
    if (!haveStop || frame.endDelaySeconds <= frame.startDelaySeconds)
        frame.endDelaySeconds = 0.0;
    error.clear();
    return true;
}

}  // namespace movieplayer::codec::subtitle
