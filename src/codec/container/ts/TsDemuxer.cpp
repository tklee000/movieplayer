#include "codec/container/ts/TsDemuxer.h"

#include "codec/core/RandomAccessFile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace movieplayer::codec::ts {
namespace {

constexpr std::uint64_t kPtsModulus = std::uint64_t{1} << 33U;
constexpr std::uint32_t kTimeScale = 90'000;
constexpr std::uint64_t kMetadataScanBytes = 128ULL * 1024ULL * 1024ULL;

struct PacketView {
    std::uint16_t pid = 0;
    bool start = false;
    const std::uint8_t* payload = nullptr;
    std::size_t payloadSize = 0;
};

bool ParsePacket(const std::uint8_t* packet, PacketView& view) {
    if (packet[0] != 0x47 || (packet[1] & 0x80U) != 0) return false;
    view = {};
    view.start = (packet[1] & 0x40U) != 0;
    view.pid = static_cast<std::uint16_t>(
        ((packet[1] & 0x1fU) << 8U) | packet[2]);
    const unsigned adaptation = (packet[3] >> 4U) & 3U;
    if (adaptation == 0 || adaptation == 2) return true;
    std::size_t position = 4;
    if (adaptation == 3) {
        const std::size_t length = packet[position++];
        if (length > 183 || position + length > 188) return false;
        position += length;
    }
    if (position < 188) {
        view.payload = packet + position;
        view.payloadSize = 188 - position;
    }
    return true;
}

std::uint64_t ReadPts(const std::uint8_t* bytes) {
    return ((static_cast<std::uint64_t>(bytes[0] & 0x0eU)) << 29U) |
           (static_cast<std::uint64_t>(bytes[1]) << 22U) |
           (static_cast<std::uint64_t>(bytes[2] & 0xfeU) << 14U) |
           (static_cast<std::uint64_t>(bytes[3]) << 7U) |
           (bytes[4] >> 1U);
}

bool ParsePes(const std::vector<std::uint8_t>& pes, std::size_t& payload,
              std::uint64_t& pts, bool& hasPts) {
    payload = 0;
    pts = 0;
    hasPts = false;
    if (pes.size() < 9 || pes[0] != 0 || pes[1] != 0 || pes[2] != 1)
        return false;
    const std::size_t header = 9U + pes[8];
    if (header > pes.size()) return false;
    if ((pes[7] & 0x80U) != 0 && pes[8] >= 5) {
        pts = ReadPts(pes.data() + 9);
        hasPts = true;
    }
    payload = header;
    return true;
}

bool ContainsH264Idr(const std::uint8_t* bytes, std::size_t size) {
    for (std::size_t i = 0; i + 4 < size; ++i) {
        if (bytes[i] != 0 || bytes[i + 1] != 0) continue;
        if (bytes[i + 2] == 1 && (bytes[i + 3] & 0x1fU) == 5U)
            return true;
        if (bytes[i + 2] == 0 && bytes[i + 3] == 1 &&
            (bytes[i + 4] & 0x1fU) == 5U)
            return true;
    }
    return false;
}

bool ContainsMpeg2IFrame(const std::uint8_t* bytes, std::size_t size) {
    for (std::size_t i = 0; i + 6 <= size; ++i) {
        if (bytes[i] == 0 && bytes[i + 1] == 0 &&
            bytes[i + 2] == 1 && bytes[i + 3] == 0) {
            const unsigned pictureCodingType = (bytes[i + 5] >> 3U) & 7U;
            if (pictureCodingType == 1U) return true;
        }
    }
    return false;
}

bool FindMpeg2Sequence(const std::uint8_t* bytes, std::size_t size,
                       int& width, int& height, Rational& frameRate) {
    static constexpr Rational kFrameRates[] = {
        {},
        {24'000, 1001},
        {24, 1},
        {25, 1},
        {30'000, 1001},
        {30, 1},
        {50, 1},
        {60'000, 1001},
        {60, 1},
    };
    for (std::size_t i = 0; i + 8 <= size; ++i) {
        if (bytes[i] != 0 || bytes[i + 1] != 0 ||
            bytes[i + 2] != 1 || bytes[i + 3] != 0xb3) {
            continue;
        }
        const int parsedWidth =
            (static_cast<int>(bytes[i + 4]) << 4U) |
            (bytes[i + 5] >> 4U);
        const int parsedHeight =
            ((bytes[i + 5] & 0x0fU) << 8U) | bytes[i + 6];
        const unsigned frameRateCode = bytes[i + 7] & 0x0fU;
        if (parsedWidth <= 0 || parsedHeight <= 0 ||
            frameRateCode >= std::size(kFrameRates) ||
            !kFrameRates[frameRateCode].IsValid()) {
            return false;
        }
        width = parsedWidth;
        height = parsedHeight;
        frameRate = kFrameRates[frameRateCode];
        return true;
    }
    return false;
}

struct AdtsFrame {
    std::size_t payload = 0;
    std::size_t payloadSize = 0;
    int sampleRate = 0;
    int channels = 0;
    unsigned frequencyIndex = 0;
    unsigned objectType = 0;
};

bool ParseAdtsFrame(const std::uint8_t* bytes, std::size_t size,
                    AdtsFrame& frame) {
    static constexpr int kSampleRates[] = {
        96'000, 88'200, 64'000, 48'000, 44'100, 32'000, 24'000,
        22'050, 16'000, 12'000, 11'025, 8'000, 7'350,
    };
    frame = {};
    if (size < 7 || bytes[0] != 0xff || (bytes[1] & 0xf6U) != 0xf0U)
        return false;
    const bool protectionAbsent = (bytes[1] & 1U) != 0;
    const unsigned frequencyIndex = (bytes[2] >> 2U) & 0x0fU;
    const unsigned channelConfiguration =
        ((bytes[2] & 1U) << 2U) | (bytes[3] >> 6U);
    const std::size_t frameLength =
        ((static_cast<std::size_t>(bytes[3]) & 3U) << 11U) |
        (static_cast<std::size_t>(bytes[4]) << 3U) |
        (bytes[5] >> 5U);
    const std::size_t headerSize = protectionAbsent ? 7U : 9U;
    if (frequencyIndex >= std::size(kSampleRates) ||
        channelConfiguration == 0 || channelConfiguration > 7 ||
        (bytes[6] & 3U) != 0 || frameLength < headerSize ||
        frameLength > size) {
        return false;
    }
    frame.payload = headerSize;
    frame.payloadSize = frameLength - headerSize;
    frame.sampleRate = kSampleRates[frequencyIndex];
    frame.channels = static_cast<int>(channelConfiguration);
    frame.frequencyIndex = frequencyIndex;
    frame.objectType = ((bytes[2] >> 6U) & 3U) + 1U;
    return true;
}

class RbspReader {
public:
    RbspReader(const std::uint8_t* bytes, std::size_t size) {
        data_.reserve(size);
        unsigned zeros = 0;
        for (std::size_t i = 0; i < size; ++i) {
            if (zeros >= 2 && bytes[i] == 3) {
                zeros = 0;
                continue;
            }
            data_.push_back(bytes[i]);
            zeros = bytes[i] == 0 ? zeros + 1U : 0U;
        }
    }

    bool Bit(unsigned& value) {
        if (bit_ >= data_.size() * 8U) return false;
        value = (data_[bit_ / 8U] >> (7U - bit_ % 8U)) & 1U;
        ++bit_;
        return true;
    }
    bool Bits(unsigned count, std::uint32_t& value) {
        value = 0;
        for (unsigned i = 0; i < count; ++i) {
            unsigned bit = 0;
            if (!Bit(bit)) return false;
            value = (value << 1U) | bit;
        }
        return true;
    }
    bool Ue(std::uint32_t& value) {
        unsigned zeros = 0, bit = 0;
        while (zeros < 31) {
            if (!Bit(bit)) return false;
            if (bit != 0) break;
            ++zeros;
        }
        std::uint32_t suffix = 0;
        if (zeros != 0 && !Bits(zeros, suffix)) return false;
        value = ((std::uint32_t{1} << zeros) - 1U) + suffix;
        return true;
    }
    bool Se(std::int32_t& value) {
        std::uint32_t code = 0;
        if (!Ue(code)) return false;
        value = (code & 1U) != 0
                    ? static_cast<std::int32_t>((code + 1U) / 2U)
                    : -static_cast<std::int32_t>(code / 2U);
        return true;
    }

private:
    std::vector<std::uint8_t> data_;
    std::size_t bit_ = 0;
};

bool SkipScalingList(RbspReader& bits, unsigned size) {
    int last = 8;
    int next = 8;
    for (unsigned i = 0; i < size; ++i) {
        if (next != 0) {
            std::int32_t delta = 0;
            if (!bits.Se(delta)) return false;
            next = (last + delta + 256) & 255;
        }
        last = next == 0 ? last : next;
    }
    return true;
}

bool ParseH264Sps(const std::uint8_t* bytes, std::size_t size,
                  int& width, int& height) {
    if (size < 4 || (bytes[0] & 0x1fU) != 7U) return false;
    RbspReader bits(bytes + 1, size - 1);
    std::uint32_t profile = 0, ignored = 0, spsId = 0;
    if (!bits.Bits(8, profile) || !bits.Bits(8, ignored) ||
        !bits.Bits(8, ignored) || !bits.Ue(spsId))
        return false;
    std::uint32_t chroma = 1;
    if (profile == 100 || profile == 110 || profile == 122 ||
        profile == 244 || profile == 44 || profile == 83 ||
        profile == 86 || profile == 118 || profile == 128 ||
        profile == 138 || profile == 139 || profile == 134 ||
        profile == 135) {
        if (!bits.Ue(chroma) || chroma > 3) return false;
        if (chroma == 3) {
            unsigned separate = 0;
            if (!bits.Bit(separate)) return false;
            if (separate) chroma = 0;
        }
        if (!bits.Ue(ignored) || !bits.Ue(ignored)) return false;
        unsigned transformBypass = 0, scaling = 0;
        if (!bits.Bit(transformBypass) || !bits.Bit(scaling)) return false;
        if (scaling) {
            for (unsigned i = 0; i < (chroma == 3 ? 12U : 8U); ++i) {
                unsigned present = 0;
                if (!bits.Bit(present)) return false;
                if (present && !SkipScalingList(bits, i < 6 ? 16 : 64))
                    return false;
            }
        }
    }
    std::uint32_t pocType = 0;
    if (!bits.Ue(ignored) || !bits.Ue(pocType)) return false;
    if (pocType == 0) {
        if (!bits.Ue(ignored)) return false;
    } else if (pocType == 1) {
        unsigned flag = 0;
        std::int32_t signedValue = 0;
        std::uint32_t count = 0;
        if (!bits.Bit(flag) || !bits.Se(signedValue) ||
            !bits.Se(signedValue) || !bits.Ue(count) || count > 256)
            return false;
        for (std::uint32_t i = 0; i < count; ++i)
            if (!bits.Se(signedValue)) return false;
    }
    unsigned gaps = 0, frameOnly = 0, direct = 0, crop = 0;
    std::uint32_t mapWidth = 0, mapHeight = 0;
    if (!bits.Ue(ignored) || !bits.Bit(gaps) || !bits.Ue(mapWidth) ||
        !bits.Ue(mapHeight) || !bits.Bit(frameOnly))
        return false;
    if (!frameOnly) {
        unsigned adaptive = 0;
        if (!bits.Bit(adaptive)) return false;
    }
    if (!bits.Bit(direct) || !bits.Bit(crop)) return false;
    std::uint32_t left = 0, right = 0, top = 0, bottom = 0;
    if (crop && (!bits.Ue(left) || !bits.Ue(right) || !bits.Ue(top) ||
                 !bits.Ue(bottom)))
        return false;
    const unsigned subWidth = chroma == 0 || chroma == 3 ? 1U : 2U;
    const unsigned subHeight = chroma == 1 ? 2U : 1U;
    const unsigned cropX = subWidth;
    const unsigned cropY = subHeight * (2U - frameOnly);
    const std::uint64_t codedWidth =
        static_cast<std::uint64_t>(mapWidth + 1U) * 16U;
    const std::uint64_t codedHeight =
        static_cast<std::uint64_t>(mapHeight + 1U) * 16U *
        (2U - frameOnly);
    if (codedWidth <= static_cast<std::uint64_t>(left + right) * cropX ||
        codedHeight <= static_cast<std::uint64_t>(top + bottom) * cropY)
        return false;
    width = static_cast<int>(
        codedWidth - static_cast<std::uint64_t>(left + right) * cropX);
    height = static_cast<int>(
        codedHeight - static_cast<std::uint64_t>(top + bottom) * cropY);
    return width > 0 && height > 0;
}

bool FindH264Dimensions(const std::uint8_t* bytes, std::size_t size,
                        int& width, int& height) {
    for (std::size_t i = 0; i + 5 < size; ++i) {
        std::size_t header = 0;
        if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1)
            header = i + 3;
        else if (bytes[i] == 0 && bytes[i + 1] == 0 &&
                 bytes[i + 2] == 0 && bytes[i + 3] == 1)
            header = i + 4;
        if (header == 0 || (bytes[header] & 0x1fU) != 7U) continue;
        std::size_t end = header + 1;
        while (end + 3 < size &&
               !(bytes[end] == 0 && bytes[end + 1] == 0 &&
                 (bytes[end + 2] == 1 ||
                  (bytes[end + 2] == 0 && bytes[end + 3] == 1))))
            ++end;
        return ParseH264Sps(bytes + header, end - header, width, height);
    }
    return false;
}

}  // namespace

struct TsDemuxer::Impl {
    struct Stream {
        TrackInfo info;
        std::uint16_t pid = 0;
        std::uint8_t streamType = 0;
        bool enabled = false;
        bool hasNextPts = false;
        std::uint64_t nextPts = 0;
        std::vector<std::uint8_t> pes;
    };

    RandomAccessFile file;
    std::vector<Stream> streams;
    std::vector<TrackInfo> publicTracks;
    std::deque<EncodedSample> pending;
    std::wstring error;
    std::uint16_t pmtPid = 0x1fff;
    std::uint64_t readPosition = 0;
    std::uint64_t firstPts = 0;
    std::uint64_t lastPts = 0;
    std::uint64_t ptsOrigin = 0;
    std::size_t packetSize = 188;
    std::size_t syncOffset = 0;
    double durationSeconds = 0.0;
    bool haveFirstPts = false;
    bool haveLastPts = false;
    bool havePtsOrigin = false;
    bool waitingForKey = true;
    bool eofFlushed = false;

    bool Fail(const std::wstring& value) {
        error = value;
        return false;
    }

    Stream* ByPid(std::uint16_t pid) {
        const auto found = std::find_if(
            streams.begin(), streams.end(),
            [pid](const Stream& stream) { return stream.pid == pid; });
        return found == streams.end() ? nullptr : &*found;
    }

    bool ParsePat(const PacketView& packet) {
        if (!packet.start || !packet.payload || packet.payloadSize < 9)
            return true;
        const std::size_t pointer = packet.payload[0];
        if (pointer + 9 > packet.payloadSize) return true;
        const std::uint8_t* section = packet.payload + 1U + pointer;
        const std::size_t available = packet.payloadSize - 1U - pointer;
        if (section[0] != 0 || available < 8) return true;
        const std::size_t length =
            ((section[1] & 0x0fU) << 8U) | section[2];
        if (length + 3U > available || length < 9) return true;
        for (std::size_t position = 8; position + 4 <= length - 1;
             position += 4) {
            const std::uint16_t program =
                static_cast<std::uint16_t>((section[position] << 8U) |
                                           section[position + 1]);
            if (program != 0) {
                pmtPid = static_cast<std::uint16_t>(
                    ((section[position + 2] & 0x1fU) << 8U) |
                    section[position + 3]);
                break;
            }
        }
        return true;
    }

    bool ParsePmt(const PacketView& packet) {
        if (!packet.start || !packet.payload || packet.payloadSize < 13)
            return true;
        const std::size_t pointer = packet.payload[0];
        if (pointer + 13 > packet.payloadSize) return true;
        const std::uint8_t* section = packet.payload + 1U + pointer;
        const std::size_t available = packet.payloadSize - 1U - pointer;
        if (section[0] != 2 || available < 12) return true;
        const std::size_t length =
            ((section[1] & 0x0fU) << 8U) | section[2];
        if (length + 3U > available || length < 13) return true;
        std::size_t position =
            12U + (((section[10] & 0x0fU) << 8U) | section[11]);
        const std::size_t end = length - 1U;
        while (position + 5 <= end) {
            const std::uint8_t type = section[position];
            const std::uint16_t pid = static_cast<std::uint16_t>(
                ((section[position + 1] & 0x1fU) << 8U) |
                section[position + 2]);
            const std::size_t infoLength =
                ((section[position + 3] & 0x0fU) << 8U) |
                section[position + 4];
            if (position + 5U + infoLength > end) break;
            CodecId codec = CodecId::Unknown;
            TrackType trackType = TrackType::Unknown;
            if (type == 0x1b) {
                codec = CodecId::H264;
                trackType = TrackType::Video;
            } else if (type == 0x02) {
                codec = CodecId::Mpeg2Video;
                trackType = TrackType::Video;
            } else if (type == 0x0f) {
                codec = CodecId::Aac;
                trackType = TrackType::Audio;
            } else if (type == 0x81) {
                codec = CodecId::Ac3;
                trackType = TrackType::Audio;
            } else if (type == 0x87) {
                codec = CodecId::Eac3;
                trackType = TrackType::Audio;
            } else if (type == 0x82 || type == 0x85 || type == 0x86 ||
                       type == 0x8a) {
                codec = CodecId::Dts;
                trackType = TrackType::Audio;
            } else if (type == 0x06) {
                for (std::size_t d = position + 5;
                     d + 2 <= position + 5U + infoLength;) {
                    const std::size_t descriptorLength = section[d + 1];
                    if (d + 2U + descriptorLength >
                        position + 5U + infoLength)
                        break;
                    if (section[d] == 0x6a) {
                        codec = CodecId::Ac3;
                        trackType = TrackType::Audio;
                    } else if (section[d] == 0x7a) {
                        codec = CodecId::Eac3;
                        trackType = TrackType::Audio;
                    } else if (section[d] == 0x05 &&
                               descriptorLength >= 3) {
                        const std::uint8_t* name = section + d + 2;
                        if (name[0] == 'A' && name[1] == 'C' &&
                            name[2] == '-') {
                            codec = CodecId::Ac3;
                            trackType = TrackType::Audio;
                        } else if (name[0] == 'E' && name[1] == 'A' &&
                                   name[2] == 'C') {
                            codec = CodecId::Eac3;
                            trackType = TrackType::Audio;
                        } else if ((name[0] == 'D' && name[1] == 'T' &&
                                    name[2] == 'S') ||
                                   (name[0] == 'B' && name[1] == 'S' &&
                                    name[2] == 'S')) {
                            codec = CodecId::Dts;
                            trackType = TrackType::Audio;
                        }
                    }
                    d += 2U + descriptorLength;
                }
            }
            if (codec != CodecId::Unknown && !ByPid(pid)) {
                Stream stream;
                stream.pid = pid;
                stream.streamType = type;
                stream.info.trackId =
                    static_cast<std::uint32_t>(streams.size() + 1U);
                stream.info.type = trackType;
                stream.info.codec = codec;
                stream.info.sampleEntry =
                    codec == CodecId::H264
                        ? "H264"
                        : (codec == CodecId::Mpeg2Video
                               ? "MPEG2"
                               : (codec == CodecId::Dts
                                      ? "DTS "
                                      : (codec == CodecId::Ac3
                                             ? "AC-3"
                                             : (codec == CodecId::Eac3
                                                    ? "EAC3"
                                                    : "AAC "))));
                stream.info.timeScale = kTimeScale;
                stream.info.frameRate = {24'000, 1001};
                if (trackType == TrackType::Audio) {
                    stream.info.sampleRate = 48'000;
                    stream.info.channels = codec == CodecId::Dts ? 6 : 2;
                }
                stream.enabled =
                    std::none_of(streams.begin(), streams.end(),
                                 [trackType](const Stream& existing) {
                                     return existing.info.type == trackType;
                                 });
                streams.push_back(std::move(stream));
            }
            position += 5U + infoLength;
        }
        return true;
    }

    bool Finalize(Stream& stream, bool discovery = false) {
        if (stream.pes.empty()) return true;
        std::size_t payload = 0;
        std::uint64_t pts = 0;
        bool hasPts = false;
        if (!ParsePes(stream.pes, payload, pts, hasPts) ||
            payload >= stream.pes.size()) {
            stream.pes.clear();
            return true;
        }
        const std::uint8_t* bytes = stream.pes.data() + payload;
        const std::size_t size = stream.pes.size() - payload;
        if (stream.info.type == TrackType::Video &&
            stream.info.codec == CodecId::H264 &&
            (stream.info.width == 0 || stream.info.height == 0)) {
            FindH264Dimensions(bytes, size, stream.info.width,
                               stream.info.height);
        }
        if (stream.info.codec == CodecId::Mpeg2Video &&
            (stream.info.width == 0 || stream.info.height == 0)) {
            FindMpeg2Sequence(bytes, size, stream.info.width,
                              stream.info.height, stream.info.frameRate);
        }
        if (stream.info.codec == CodecId::Aac &&
            stream.info.codecPrivate.empty()) {
            for (std::size_t position = 0; position + 7U <= size;
                 ++position) {
                AdtsFrame frame;
                if (!ParseAdtsFrame(bytes + position, size - position, frame))
                    continue;
                stream.info.sampleRate = frame.sampleRate;
                stream.info.channels = frame.channels;
                stream.info.codecPrivate = {
                    static_cast<std::uint8_t>(
                        (frame.objectType << 3U) |
                        (frame.frequencyIndex >> 1U)),
                    static_cast<std::uint8_t>(
                        ((frame.frequencyIndex & 1U) << 7U) |
                        (static_cast<unsigned>(frame.channels) << 3U)),
                };
                break;
            }
        }
        if (hasPts) {
            if (!haveFirstPts) {
                firstPts = pts;
                haveFirstPts = true;
            }
            lastPts = pts;
            haveLastPts = true;
        }
        if (!discovery && stream.enabled) {
            const std::uint64_t normalizedPts =
                hasPts
                    ? (!havePtsOrigin || pts >= ptsOrigin
                           ? pts - (havePtsOrigin ? ptsOrigin : 0)
                           : pts + kPtsModulus - ptsOrigin)
                    : (stream.hasNextPts ? stream.nextPts : 0);
            const auto appendSample =
                [&](const std::uint8_t* sampleBytes,
                    std::size_t sampleSize, std::uint64_t samplePts,
                    std::uint32_t duration, bool sync) {
                    EncodedSample sample;
                    sample.trackId = stream.info.trackId;
                    sample.type = stream.info.type;
                    sample.timeScale = kTimeScale;
                    sample.decodeTime =
                        static_cast<std::int64_t>(samplePts);
                    sample.presentationTime =
                        static_cast<std::int64_t>(samplePts);
                    sample.duration = duration;
                    sample.sync = sync;
                    sample.bytes.assign(sampleBytes,
                                        sampleBytes + sampleSize);
                    pending.push_back(std::move(sample));
                    stream.nextPts = samplePts + duration;
                    stream.hasNextPts = true;
                };
            if (stream.info.codec == CodecId::Aac) {
                std::size_t position = 0;
                std::uint64_t framePts = normalizedPts;
                while (position + 7U <= size) {
                    AdtsFrame frame;
                    if (!ParseAdtsFrame(bytes + position, size - position,
                                        frame)) {
                        ++position;
                        continue;
                    }
                    const std::uint32_t duration =
                        static_cast<std::uint32_t>(std::llround(
                            1024.0 * kTimeScale / frame.sampleRate));
                    appendSample(bytes + position + frame.payload,
                                 frame.payloadSize, framePts, duration, true);
                    framePts += duration;
                    position += frame.payload + frame.payloadSize;
                }
            } else {
                const std::uint32_t videoDuration =
                    stream.info.frameRate.IsValid()
                        ? static_cast<std::uint32_t>(std::max<long long>(
                              1, std::llround(
                                     static_cast<long double>(kTimeScale) *
                                     stream.info.frameRate.denominator /
                                     stream.info.frameRate.numerator)))
                        : 3754U;
                const std::uint32_t duration =
                    stream.info.type == TrackType::Video
                        ? videoDuration
                        : 2880U;
                const bool sync =
                    stream.info.type == TrackType::Audio ||
                    (stream.info.codec == CodecId::H264 &&
                     ContainsH264Idr(bytes, size)) ||
                    (stream.info.codec == CodecId::Mpeg2Video &&
                     ContainsMpeg2IFrame(bytes, size));
                appendSample(bytes, size, normalizedPts, duration, sync);
            }
        }
        stream.pes.clear();
        return true;
    }

    bool Consume(const std::uint8_t* bytes, bool discovery) {
        PacketView packet;
        if (!ParsePacket(bytes, packet)) return true;
        if (packet.pid == 0) ParsePat(packet);
        if (packet.pid == pmtPid) ParsePmt(packet);
        Stream* stream = ByPid(packet.pid);
        if (!stream || !packet.payload || packet.payloadSize == 0) return true;
        if (packet.start) {
            if (!Finalize(*stream, discovery)) return false;
            stream->pes.clear();
        }
        constexpr std::size_t kMaximumPes = 16U * 1024U * 1024U;
        if (stream->pes.size() + packet.payloadSize > kMaximumPes) {
            stream->pes.clear();
            return true;
        }
        stream->pes.insert(stream->pes.end(), packet.payload,
                           packet.payload + packet.payloadSize);
        return true;
    }

    bool ReadPacketAt(std::uint64_t offset,
                      std::array<std::uint8_t, 188>& packet) {
        if (offset > file.Size() ||
            packetSize > file.Size() - offset ||
            syncOffset + packet.size() > packetSize) {
            return Fail(L"An MPEG-TS packet points outside the file");
        }
        return file.Read(offset + syncOffset, packet.data(), packet.size(),
                         error);
    }

    bool DetectPacketLayout() {
        static constexpr std::array<std::pair<std::size_t, std::size_t>, 3>
            layouts = {{{188, 0}, {192, 4}, {204, 0}}};
        std::array<std::uint8_t, 3U * 204U + 4U> signature{};
        const std::size_t available = static_cast<std::size_t>(
            std::min<std::uint64_t>(signature.size(), file.Size()));
        if (!file.Read(0, signature.data(), available, error)) return false;
        for (const auto& layout : layouts) {
            const std::size_t stride = layout.first;
            const std::size_t sync = layout.second;
            if (sync + 2U * stride >= available) continue;
            if (signature[sync] == 0x47 &&
                signature[sync + stride] == 0x47 &&
                signature[sync + 2U * stride] == 0x47) {
                packetSize = stride;
                syncOffset = sync;
                error.clear();
                return true;
            }
        }
        return Fail(
            L"The file is not a supported 188/192/204-byte MPEG transport "
            L"stream");
    }

    bool Discover(const std::wstring& path) {
        std::array<std::uint8_t, 188> packet{};
        const std::uint64_t end =
            std::min<std::uint64_t>(file.Size(), kMetadataScanBytes);
        for (std::uint64_t position = 0; position + packetSize <= end;
             position += packetSize) {
            if (!ReadPacketAt(position, packet) ||
                !Consume(packet.data(), true))
                return false;
            const auto video = std::find_if(
                streams.begin(), streams.end(), [](const Stream& stream) {
                    return stream.info.type == TrackType::Video &&
                           stream.info.width > 0 &&
                           stream.info.height > 0;
                });
            if (video != streams.end() && haveFirstPts &&
                position >= 4U * 1024U * 1024U)
                break;
        }
        for (Stream& stream : streams) {
            Finalize(stream, true);
            stream.pes.clear();
        }
        if (streams.empty())
            return Fail(L"The MPEG-TS program has no supported streams");
        const auto video = std::find_if(
            streams.begin(), streams.end(), [](const Stream& stream) {
                return stream.info.type == TrackType::Video;
            });
        if (video == streams.end())
            return Fail(L"The MPEG-TS program has no supported video");
        if (video->info.width == 0 || video->info.height == 0) {
            if (video->info.codec == CodecId::H264)
                return Fail(L"Could not find an H.264 SPS in the MPEG-TS");
            return Fail(
                L"Could not find an MPEG-2 sequence header in the MPEG-TS");
        }

        const std::uint64_t tailBegin =
            file.Size() > kMetadataScanBytes
                ? file.Size() - kMetadataScanBytes
                : 0;
        const std::uint64_t aligned =
            tailBegin +
            ((packetSize - tailBegin % packetSize) % packetSize);
        std::uint64_t tailPts = 0;
        bool haveTailPts = false;
        for (std::uint64_t position = aligned;
             position + packetSize <= file.Size(); position += packetSize) {
            if (!ReadPacketAt(position, packet)) return false;
            PacketView view;
            if (!ParsePacket(packet.data(), view) || !view.start ||
                view.pid != video->pid || !view.payload ||
                view.payloadSize < 14)
                continue;
            std::vector<std::uint8_t> header(view.payload,
                                             view.payload + view.payloadSize);
            std::size_t payload = 0;
            std::uint64_t pts = 0;
            bool hasPts = false;
            if (ParsePes(header, payload, pts, hasPts) && hasPts) {
                tailPts = pts;
                haveTailPts = true;
            }
        }
        if (haveFirstPts && haveTailPts) {
            const std::uint64_t ticks =
                tailPts >= firstPts ? tailPts - firstPts
                                    : tailPts + kPtsModulus - firstPts;
            durationSeconds =
                static_cast<double>(ticks) / kTimeScale;
        }
        if (durationSeconds <= 0.0)
            durationSeconds = 1.0;
        ptsOrigin = firstPts;
        havePtsOrigin = haveFirstPts;
        publicTracks.clear();
        for (Stream& stream : streams) {
            stream.info.sourcePath = path;
            stream.info.durationTicks = static_cast<std::uint64_t>(
                std::llround(durationSeconds * kTimeScale));
            if (stream.info.codec == CodecId::Aac &&
                stream.info.codecPrivate.empty()) {
                return Fail(
                    L"Could not find a complete AAC ADTS frame in the MPEG-TS");
            }
            publicTracks.push_back(stream.info);
        }
        readPosition = 0;
        firstPts = 0;
        lastPts = 0;
        haveFirstPts = false;
        haveLastPts = false;
        pending.clear();
        waitingForKey = true;
        eofFlushed = false;
        error.clear();
        return true;
    }

    bool Open(const std::wstring& path) {
        Close();
        if (!file.Open(path, error) || file.Size() < 188 * 3U)
            return Fail(L"The MPEG-TS file is too small");
        if (!DetectPacketLayout()) return false;
        return Discover(path);
    }

    void Close() {
        file.Close();
        streams.clear();
        publicTracks.clear();
        pending.clear();
        error.clear();
        pmtPid = 0x1fff;
        readPosition = firstPts = lastPts = ptsOrigin = 0;
        packetSize = 188;
        syncOffset = 0;
        durationSeconds = 0.0;
        haveFirstPts = haveLastPts = havePtsOrigin = false;
        waitingForKey = true;
        eofFlushed = false;
    }

    bool ReadNext(EncodedSample& sample, bool& endOfFile) {
        endOfFile = false;
        std::array<std::uint8_t, 188> packet{};
        for (;;) {
            while (!pending.empty()) {
                sample = std::move(pending.front());
                pending.pop_front();
                if (waitingForKey) {
                    if (sample.type != TrackType::Video || !sample.sync)
                        continue;
                    waitingForKey = false;
                }
                error.clear();
                return true;
            }
            if (readPosition + packetSize <= file.Size()) {
                if (!ReadPacketAt(readPosition, packet)) return false;
                readPosition += packetSize;
                if (!Consume(packet.data(), false)) return false;
                continue;
            }
            if (!eofFlushed) {
                for (Stream& stream : streams)
                    if (!Finalize(stream, false)) return false;
                eofFlushed = true;
                continue;
            }
            sample = {};
            endOfFile = true;
            error.clear();
            return true;
        }
    }

    bool SetTrack(std::uint32_t id, bool enabled) {
        const auto found = std::find_if(
            streams.begin(), streams.end(),
            [id](const Stream& stream) { return stream.info.trackId == id; });
        if (found == streams.end())
            return Fail(L"The MPEG-TS track does not exist");
        found->enabled = enabled;
        error.clear();
        return true;
    }

    bool Seek(double seconds, double& decodeStart) {
        if (!std::isfinite(seconds)) seconds = 0.0;
        seconds = std::max(0.0, std::min(durationSeconds, seconds));
        const long double fraction =
            durationSeconds > 0.0 ? seconds / durationSeconds : 0.0;
        std::uint64_t approximate = static_cast<std::uint64_t>(
            fraction * static_cast<long double>(file.Size()));
        approximate -= approximate % packetSize;
        readPosition = std::min<std::uint64_t>(
            approximate, file.Size() - file.Size() % packetSize);
        for (Stream& stream : streams) {
            stream.pes.clear();
            stream.hasNextPts = false;
            stream.nextPts = 0;
        }
        pending.clear();
        waitingForKey = true;
        eofFlushed = false;
        decodeStart = seconds;
        error.clear();
        return true;
    }
};

TsDemuxer::TsDemuxer() : impl_(std::make_unique<Impl>()) {}
TsDemuxer::~TsDemuxer() = default;
bool TsDemuxer::Open(const std::wstring& path) { return impl_->Open(path); }
void TsDemuxer::Close() { impl_->Close(); }
const std::vector<TrackInfo>& TsDemuxer::Tracks() const noexcept {
    return impl_->publicTracks;
}
double TsDemuxer::DurationSeconds() const noexcept {
    return impl_->durationSeconds;
}
bool TsDemuxer::SetTrackEnabled(std::uint32_t id, bool enabled) {
    return impl_->SetTrack(id, enabled);
}
bool TsDemuxer::ReadNextSample(EncodedSample& sample, bool& endOfFile) {
    return impl_->ReadNext(sample, endOfFile);
}
bool TsDemuxer::Seek(double seconds, double& decodeStart) {
    return impl_->Seek(seconds, decodeStart);
}
const std::wstring& TsDemuxer::LastError() const noexcept {
    return impl_->error;
}

}  // namespace movieplayer::codec::ts
