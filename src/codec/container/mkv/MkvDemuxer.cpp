#include "codec/container/mkv/MkvDemuxer.h"

#include "codec/core/RandomAccessFile.h"
#include "codec/core/ZlibInflater.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace movieplayer::codec::mkv {
namespace {

constexpr std::uint64_t kEbml = 0x1a45dfa3;
constexpr std::uint64_t kSegment = 0x18538067;
constexpr std::uint64_t kSeekHead = 0x114d9b74;
constexpr std::uint64_t kInfo = 0x1549a966;
constexpr std::uint64_t kTracks = 0x1654ae6b;
constexpr std::uint64_t kCluster = 0x1f43b675;
constexpr std::uint64_t kCues = 0x1c53bb6b;
constexpr std::uint64_t kMaximumMetadataBytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kTimeScale = 1'000'000;

std::uint64_t ReadBigEndian(const std::uint8_t* data, std::size_t size) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < size; ++i) value = (value << 8U) | data[i];
    return value;
}

double ReadFloat(const std::uint8_t* data, std::size_t size) {
    if (size == 4) {
        const std::uint32_t bits = static_cast<std::uint32_t>(
            ReadBigEndian(data, size));
        float value = 0.0F;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    if (size == 8) {
        const std::uint64_t bits = ReadBigEndian(data, size);
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    return 0.0;
}

bool ReadVint(const std::uint8_t* data, std::size_t size, std::size_t& position,
              bool identifier, std::uint64_t& value, std::size_t& length,
              bool& unknown) {
    if (position >= size) return false;
    const std::uint8_t first = data[position];
    std::uint8_t mask = 0x80;
    length = 1;
    while (length <= 8 && (first & mask) == 0) {
        mask >>= 1U;
        ++length;
    }
    if (length > 8 || length > size - position) return false;
    value = identifier ? first : static_cast<std::uint8_t>(first & (mask - 1U));
    for (std::size_t i = 1; i < length; ++i) {
        value = (value << 8U) | data[position + i];
    }
    unknown = !identifier && value == ((std::uint64_t{1} << (7U * length)) - 1U);
    position += length;
    return true;
}

struct MemoryElement {
    std::uint64_t id = 0;
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

bool NextElement(const std::uint8_t* data, std::size_t size,
                 std::size_t& position, MemoryElement& element) {
    std::uint64_t id = 0;
    std::uint64_t payloadSize = 0;
    std::size_t length = 0;
    bool unknown = false;
    if (!ReadVint(data, size, position, true, id, length, unknown) ||
        !ReadVint(data, size, position, false, payloadSize, length, unknown) ||
        unknown || payloadSize > size - position) {
        return false;
    }
    element.id = id;
    element.data = data + position;
    element.size = static_cast<std::size_t>(payloadSize);
    position += element.size;
    return true;
}

std::string ReadString(const MemoryElement& element) {
    return std::string(reinterpret_cast<const char*>(element.data), element.size);
}

struct FileElement {
    std::uint64_t id = 0;
    std::uint64_t offset = 0;
    std::uint64_t dataOffset = 0;
    std::uint64_t size = 0;
    std::uint64_t end = 0;
    bool unknownSize = false;
};

}  // namespace

struct MkvDemuxer::Impl {
    struct Track {
        TrackInfo info;
        std::uint64_t number = 0;
        std::uint64_t defaultDurationNs = 0;
        bool defaultTrack = true;
        bool enabled = false;
        bool zlibCompressed = false;
        bool unsupportedCompression = false;
        std::vector<std::uint8_t> strippedHeader;
    };

    struct Cue {
        std::int64_t timeUs = 0;
        std::uint64_t clusterOffset = 0;
    };

    RandomAccessFile file;
    std::vector<Track> tracks;
    std::vector<TrackInfo> publicTracks;
    std::vector<Cue> cues;
    std::deque<EncodedSample> pending;
    std::wstring error;
    std::uint64_t segmentDataOffset = 0;
    std::uint64_t segmentEnd = 0;
    std::uint64_t firstClusterOffset = 0;
    std::uint64_t readPosition = 0;
    std::uint64_t timecodeScaleNs = 1'000'000;
    std::uint64_t cuesOffset = 0;
    std::uint64_t videoTrackNumber = 0;
    double durationSeconds = 0.0;

    bool Fail(const std::wstring& message) {
        error = message;
        return false;
    }

    bool ReadElementHeader(std::uint64_t offset, FileElement& element) {
        if (offset >= file.Size()) return false;
        std::uint8_t bytes[16] = {};
        const std::size_t available = static_cast<std::size_t>(
            std::min<std::uint64_t>(sizeof(bytes), file.Size() - offset));
        if (!file.Read(offset, bytes, available, error)) return false;
        std::size_t position = 0;
        std::size_t length = 0;
        bool ignored = false;
        bool unknown = false;
        std::uint64_t id = 0;
        std::uint64_t size = 0;
        if (!ReadVint(bytes, available, position, true, id, length, ignored) ||
            !ReadVint(bytes, available, position, false, size, length, unknown)) {
            return Fail(L"Invalid Matroska element header");
        }
        element = {};
        element.id = id;
        element.offset = offset;
        element.dataOffset = offset + position;
        element.size = size;
        element.unknownSize = unknown;
        if (unknown) {
            element.end = file.Size();
        } else if (size > file.Size() - element.dataOffset) {
            return Fail(L"A Matroska element extends past the end of the file");
        } else {
            element.end = element.dataOffset + size;
        }
        return true;
    }

    bool ReadPayload(const FileElement& element,
                     std::vector<std::uint8_t>& payload) {
        if (element.unknownSize || element.size > kMaximumMetadataBytes ||
            element.size > (std::numeric_limits<std::size_t>::max)()) {
            return Fail(L"A Matroska metadata element is too large");
        }
        return file.Read(element.dataOffset, static_cast<std::size_t>(element.size),
                         payload, error);
    }

    std::int64_t TimecodeToUs(std::int64_t timecode) const {
        const long double value = static_cast<long double>(timecode) *
                                  static_cast<long double>(timecodeScaleNs) /
                                  1000.0L;
        const long double limited = std::max(
            static_cast<long double>((std::numeric_limits<std::int64_t>::min)()),
            std::min(static_cast<long double>(
                         (std::numeric_limits<std::int64_t>::max)()),
                     value));
        return static_cast<std::int64_t>(std::llround(limited));
    }

    Track* FindTrack(std::uint64_t number) {
        const auto found = std::find_if(
            tracks.begin(), tracks.end(), [number](const Track& track) {
                return track.number == number;
            });
        return found == tracks.end() ? nullptr : &*found;
    }

    void ParseColour(const MemoryElement& colour, ColorDescription& output) {
        std::size_t position = 0;
        MemoryElement child;
        while (NextElement(colour.data, colour.size, position, child)) {
            const std::uint64_t value = ReadBigEndian(child.data, child.size);
            if (child.id == 0x55b9) {
                if (value == 1) output.range = ColorRange::Limited;
                if (value == 2) output.range = ColorRange::Full;
            } else if (child.id == 0x55b1) {
                if (value == 1) output.matrix = ColorMatrix::Bt709;
                if (value == 9) output.matrix = ColorMatrix::Bt2020NonConstant;
                if (value == 10) output.matrix = ColorMatrix::Bt2020Constant;
            } else if (child.id == 0x55bb) {
                if (value == 1) output.primaries = ColorPrimaries::Bt709;
                if (value == 9) output.primaries = ColorPrimaries::Bt2020;
            } else if (child.id == 0x55ba) {
                if (value == 1 || value == 6)
                    output.transfer = TransferCharacteristic::Bt709;
                if (value == 16) output.transfer = TransferCharacteristic::Pq;
                if (value == 18) output.transfer = TransferCharacteristic::Hlg;
            } else if (child.id == 0x55b7 && value == 1) {
                output.chromaLocation = ChromaLocation::Left;
            }
        }
    }

    void ParseVideo(const MemoryElement& video, Track& track) {
        std::uint64_t displayWidth = 0;
        std::uint64_t displayHeight = 0;
        std::size_t position = 0;
        MemoryElement child;
        while (NextElement(video.data, video.size, position, child)) {
            const std::uint64_t value = ReadBigEndian(child.data, child.size);
            if (child.id == 0xb0) track.info.width = static_cast<int>(value);
            else if (child.id == 0xba) track.info.height = static_cast<int>(value);
            else if (child.id == 0x54b0) displayWidth = value;
            else if (child.id == 0x54ba) displayHeight = value;
            else if (child.id == 0x55b0) ParseColour(child, track.info.color);
        }
        if (displayWidth != 0 && displayHeight != 0 && track.info.width > 0 &&
            track.info.height > 0) {
            std::int64_t numerator = static_cast<std::int64_t>(displayWidth) *
                                     track.info.height;
            std::int64_t denominator = static_cast<std::int64_t>(displayHeight) *
                                       track.info.width;
            const std::int64_t divisor = std::gcd(numerator, denominator);
            track.info.sampleAspectRatio = {numerator / divisor,
                                            denominator / divisor};
        }
    }

    void ParseAudio(const MemoryElement& audio, Track& track) {
        std::size_t position = 0;
        MemoryElement child;
        while (NextElement(audio.data, audio.size, position, child)) {
            if (child.id == 0xb5) {
                track.info.sampleRate = static_cast<int>(
                    std::lround(ReadFloat(child.data, child.size)));
            } else if (child.id == 0x9f) {
                track.info.channels = static_cast<int>(
                    ReadBigEndian(child.data, child.size));
            } else if (child.id == 0x6264) {
                track.info.bitsPerSample = static_cast<int>(
                    ReadBigEndian(child.data, child.size));
            }
        }
    }

    void ParseContentEncodings(const MemoryElement& encodings, Track& track) {
        std::size_t outer = 0;
        MemoryElement encoding;
        while (NextElement(encodings.data, encodings.size, outer, encoding)) {
            if (encoding.id != 0x6240) continue;  // ContentEncoding
            std::uint64_t scope = 1;
            std::uint64_t type = 0;
            std::uint64_t algorithm = 0;
            bool hasCompression = false;
            std::vector<std::uint8_t> settings;
            std::size_t inner = 0;
            MemoryElement child;
            while (NextElement(encoding.data, encoding.size, inner, child)) {
                if (child.id == 0x5032) {
                    scope = ReadBigEndian(child.data, child.size);
                } else if (child.id == 0x5033) {
                    type = ReadBigEndian(child.data, child.size);
                } else if (child.id == 0x5034) {  // ContentCompression
                    hasCompression = true;
                    std::size_t nested = 0;
                    MemoryElement compression;
                    while (NextElement(child.data, child.size, nested,
                                       compression)) {
                        if (compression.id == 0x4254)
                            algorithm = ReadBigEndian(compression.data,
                                                      compression.size);
                        else if (compression.id == 0x4255)
                            settings.assign(compression.data,
                                            compression.data + compression.size);
                    }
                }
            }
            // Type 0 is compression and scope bit 0 applies it to every frame.
            // An empty ContentCompression element means the Matroska default:
            // algorithm 0 (zlib), as used by MKVToolNix for VobSub streams.
            if (!hasCompression || type != 0 || (scope & 1U) == 0) continue;
            if (algorithm == 0) {
                track.zlibCompressed = true;
            } else if (algorithm == 3) {
                track.strippedHeader = std::move(settings);
            } else {
                track.unsupportedCompression = true;
            }
        }
    }

    bool ParseTrackEntry(const MemoryElement& entry) {
        Track track;
        std::uint64_t trackType = 0;
        std::string codecId;
        std::size_t position = 0;
        MemoryElement child;
        while (NextElement(entry.data, entry.size, position, child)) {
            if (child.id == 0xd7) track.number = ReadBigEndian(child.data, child.size);
            else if (child.id == 0x83) trackType = ReadBigEndian(child.data, child.size);
            else if (child.id == 0x86) codecId = ReadString(child);
            else if (child.id == 0x63a2)
                track.info.codecPrivate.assign(child.data, child.data + child.size);
            else if (child.id == 0x23e383)
                track.defaultDurationNs = ReadBigEndian(child.data, child.size);
            else if (child.id == 0x88)
                track.defaultTrack = ReadBigEndian(child.data, child.size) != 0;
            else if (child.id == 0x22b59c) track.info.language = ReadString(child);
            else if (child.id == 0x536e) track.info.name = ReadString(child);
            else if (child.id == 0xe0) ParseVideo(child, track);
            else if (child.id == 0xe1) ParseAudio(child, track);
            else if (child.id == 0x6d80) ParseContentEncodings(child, track);
        }
        if (track.number == 0 || track.number >
                                     (std::numeric_limits<std::uint32_t>::max)()) {
            return true;
        }
        track.info.trackId = static_cast<std::uint32_t>(track.number);
        track.info.timeScale = static_cast<std::uint32_t>(kTimeScale);
        track.info.durationTicks = static_cast<std::uint64_t>(
            std::max(0.0, durationSeconds) * kTimeScale);
        track.info.sampleEntry = codecId;
        track.info.defaultTrack = track.defaultTrack;
        if (trackType == 1) {
            track.info.type = TrackType::Video;
            if (codecId == "V_MPEGH/ISO/HEVC") track.info.codec = CodecId::Hevc;
            else if (codecId == "V_MPEG4/ISO/AVC") track.info.codec = CodecId::H264;
            if (track.defaultDurationNs != 0) {
                const std::int64_t divisor = std::gcd<std::int64_t>(
                    1'000'000'000, static_cast<std::int64_t>(track.defaultDurationNs));
                track.info.frameRate = {
                    1'000'000'000 / divisor,
                    static_cast<std::int64_t>(track.defaultDurationNs) / divisor};
            }
        } else if (trackType == 2) {
            track.info.type = TrackType::Audio;
            if (codecId.rfind("A_AAC", 0) == 0) track.info.codec = CodecId::Aac;
        } else if (trackType == 17) {
            track.info.type = TrackType::Subtitle;
            if (codecId == "S_TEXT/UTF8" || codecId == "S_TEXT/ASCII")
                track.info.codec = CodecId::SubRip;
            else if (codecId == "S_TEXT/ASS" || codecId == "S_TEXT/SSA")
                track.info.codec = CodecId::Ass;
            else if (codecId == "S_VOBSUB")
                track.info.codec = CodecId::VobSub;
        } else {
            return true;
        }
        if (track.info.codec != CodecId::Unknown &&
            !track.unsupportedCompression) {
            tracks.push_back(std::move(track));
        }
        return true;
    }

    bool ParseTracks(const std::vector<std::uint8_t>& payload) {
        std::size_t position = 0;
        MemoryElement element;
        while (NextElement(payload.data(), payload.size(), position, element)) {
            if (element.id == 0xae && !ParseTrackEntry(element)) return false;
        }
        return true;
    }

    bool ParseInfo(const std::vector<std::uint8_t>& payload) {
        std::size_t position = 0;
        MemoryElement element;
        double durationTicks = 0.0;
        while (NextElement(payload.data(), payload.size(), position, element)) {
            if (element.id == 0x2ad7b1) {
                timecodeScaleNs = ReadBigEndian(element.data, element.size);
            } else if (element.id == 0x4489) {
                durationTicks = ReadFloat(element.data, element.size);
            }
        }
        if (timecodeScaleNs == 0 || !std::isfinite(durationTicks) ||
            durationTicks < 0.0) {
            return Fail(L"Invalid Matroska timing information");
        }
        durationSeconds = durationTicks * static_cast<double>(timecodeScaleNs) /
                          1'000'000'000.0;
        return true;
    }

    void ParseSeekHead(const std::vector<std::uint8_t>& payload) {
        std::size_t position = 0;
        MemoryElement seek;
        while (NextElement(payload.data(), payload.size(), position, seek)) {
            if (seek.id != 0x4dbb) continue;
            std::uint64_t targetId = 0;
            std::uint64_t targetPosition = 0;
            std::size_t inner = 0;
            MemoryElement child;
            while (NextElement(seek.data, seek.size, inner, child)) {
                if (child.id == 0x53ab)
                    targetId = ReadBigEndian(child.data, child.size);
                else if (child.id == 0x53ac)
                    targetPosition = ReadBigEndian(child.data, child.size);
            }
            if (targetId == kCues && targetPosition <= file.Size() - segmentDataOffset)
                cuesOffset = segmentDataOffset + targetPosition;
        }
    }

    bool ParseCues() {
        if (cuesOffset == 0) return true;
        FileElement cuesElement;
        if (!ReadElementHeader(cuesOffset, cuesElement)) return false;
        if (cuesElement.id != kCues) return Fail(L"The Matroska Cue index is invalid");
        std::vector<std::uint8_t> payload;
        if (!ReadPayload(cuesElement, payload)) return false;
        std::size_t position = 0;
        MemoryElement cuePoint;
        while (NextElement(payload.data(), payload.size(), position, cuePoint)) {
            if (cuePoint.id != 0xbb) continue;
            std::uint64_t cueTime = 0;
            std::size_t inner = 0;
            MemoryElement child;
            std::vector<MemoryElement> positions;
            while (NextElement(cuePoint.data, cuePoint.size, inner, child)) {
                if (child.id == 0xb3) cueTime = ReadBigEndian(child.data, child.size);
                else if (child.id == 0xb7) positions.push_back(child);
            }
            for (const MemoryElement& trackPosition : positions) {
                std::uint64_t cueTrack = 0;
                std::uint64_t clusterPosition = 0;
                std::size_t nested = 0;
                while (NextElement(trackPosition.data, trackPosition.size, nested,
                                   child)) {
                    if (child.id == 0xf7)
                        cueTrack = ReadBigEndian(child.data, child.size);
                    else if (child.id == 0xf1)
                        clusterPosition = ReadBigEndian(child.data, child.size);
                }
                if (cueTrack == videoTrackNumber &&
                    clusterPosition <= file.Size() - segmentDataOffset) {
                    cues.push_back({TimecodeToUs(static_cast<std::int64_t>(cueTime)),
                                    segmentDataOffset + clusterPosition});
                    break;
                }
            }
        }
        std::sort(cues.begin(), cues.end(), [](const Cue& a, const Cue& b) {
            return a.timeUs < b.timeUs;
        });
        return true;
    }

    bool SplitLaces(const std::uint8_t* data, std::size_t size,
                    unsigned lacing, std::vector<std::pair<std::size_t,
                                                           std::size_t>>& frames) {
        frames.clear();
        if (lacing == 0) {
            frames.push_back({0, size});
            return true;
        }
        if (size == 0) return Fail(L"A laced Matroska block is empty");
        const std::size_t frameCount = static_cast<std::size_t>(data[0]) + 1U;
        std::size_t position = 1;
        std::vector<std::int64_t> sizes(frameCount, 0);
        if (lacing == 1) {
            for (std::size_t i = 0; i + 1 < frameCount; ++i) {
                std::size_t value = 0;
                for (;;) {
                    if (position >= size) return Fail(L"Truncated Xiph lacing");
                    const std::uint8_t part = data[position++];
                    value += part;
                    if (part != 255) break;
                }
                sizes[i] = static_cast<std::int64_t>(value);
            }
        } else if (lacing == 2) {
            const std::size_t remaining = size - position;
            if (remaining % frameCount != 0)
                return Fail(L"Invalid fixed-size Matroska lacing");
            std::fill(sizes.begin(), sizes.end(),
                      static_cast<std::int64_t>(remaining / frameCount));
        } else if (lacing == 3) {
            std::uint64_t firstSize = 0;
            std::size_t length = 0;
            bool unknown = false;
            if (!ReadVint(data, size, position, false, firstSize, length, unknown) ||
                unknown) {
                return Fail(L"Invalid EBML lacing size");
            }
            sizes[0] = static_cast<std::int64_t>(firstSize);
            for (std::size_t i = 1; i + 1 < frameCount; ++i) {
                std::uint64_t encoded = 0;
                if (!ReadVint(data, size, position, false, encoded, length, unknown) ||
                    unknown) {
                    return Fail(L"Invalid EBML lacing delta");
                }
                const std::uint64_t bias =
                    (std::uint64_t{1} << (7U * length - 1U)) - 1U;
                const std::int64_t delta = static_cast<std::int64_t>(encoded) -
                                           static_cast<std::int64_t>(bias);
                sizes[i] = sizes[i - 1] + delta;
                if (sizes[i] < 0) return Fail(L"Negative EBML lace size");
            }
        } else {
            return Fail(L"Unsupported Matroska lacing mode");
        }
        std::uint64_t used = 0;
        for (std::size_t i = 0; i + 1 < frameCount; ++i) {
            used += static_cast<std::uint64_t>(sizes[i]);
        }
        if (used > size - position) return Fail(L"Matroska lace sizes are truncated");
        if (lacing != 2) sizes.back() = static_cast<std::int64_t>(size - position - used);
        for (std::int64_t frameSize : sizes) {
            if (frameSize < 0 || static_cast<std::uint64_t>(frameSize) >
                                     size - position) {
                return Fail(L"Invalid Matroska lace frame size");
            }
            frames.push_back({position, static_cast<std::size_t>(frameSize)});
            position += static_cast<std::size_t>(frameSize);
        }
        return position == size || Fail(L"Matroska lacing did not consume the block");
    }

    bool ParseBlock(const std::uint8_t* data, std::size_t size,
                    std::int64_t clusterTimecode, bool simpleBlock,
                    bool blockGroupSync, std::uint64_t blockDurationTicks) {
        std::size_t position = 0;
        std::uint64_t trackNumber = 0;
        std::size_t length = 0;
        bool unknown = false;
        if (!ReadVint(data, size, position, false, trackNumber, length, unknown) ||
            unknown || position + 3 > size) {
            return Fail(L"Truncated Matroska block header");
        }
        const std::int16_t relative = static_cast<std::int16_t>(
            (static_cast<std::uint16_t>(data[position]) << 8U) |
            data[position + 1]);
        position += 2;
        const std::uint8_t flags = data[position++];
        Track* track = FindTrack(trackNumber);
        if (!track || !track->enabled) return true;
        const unsigned lacing = (flags & 0x06U) >> 1U;
        std::vector<std::pair<std::size_t, std::size_t>> frames;
        if (!SplitLaces(data + position, size - position, lacing, frames)) return false;
        std::uint64_t durationUs = 0;
        if (blockDurationTicks != 0) {
            durationUs = static_cast<std::uint64_t>(std::max<std::int64_t>(
                1, TimecodeToUs(static_cast<std::int64_t>(blockDurationTicks)))) /
                         frames.size();
        } else if (track->defaultDurationNs != 0) {
            durationUs = std::max<std::uint64_t>(1, track->defaultDurationNs / 1000U);
        }
        const std::int64_t basePts = TimecodeToUs(clusterTimecode + relative);
        for (std::size_t i = 0; i < frames.size(); ++i) {
            EncodedSample sample;
            sample.trackId = track->info.trackId;
            sample.type = track->info.type;
            sample.timeScale = static_cast<std::uint32_t>(kTimeScale);
            sample.presentationTime = basePts +
                                      static_cast<std::int64_t>(i * durationUs);
            sample.decodeTime = sample.presentationTime;
            sample.duration = static_cast<std::uint32_t>(std::min<std::uint64_t>(
                durationUs, (std::numeric_limits<std::uint32_t>::max)()));
            sample.sync = track->info.type == TrackType::Audio ||
                          track->info.type == TrackType::Subtitle ||
                          (simpleBlock ? (flags & 0x80U) != 0 : blockGroupSync);
            const std::size_t frameOffset = position + frames[i].first;
            sample.bytes.assign(data + frameOffset,
                                data + frameOffset + frames[i].second);
            if (track->zlibCompressed) {
                std::vector<std::uint8_t> inflated;
                std::wstring inflateError;
                if (!InflateZlib(sample.bytes.data(), sample.bytes.size(),
                                 inflated, inflateError)) {
                    return Fail(L"Could not decompress a Matroska track frame: " +
                                inflateError);
                }
                sample.bytes = std::move(inflated);
            } else if (!track->strippedHeader.empty()) {
                sample.bytes.insert(sample.bytes.begin(),
                                    track->strippedHeader.begin(),
                                    track->strippedHeader.end());
            }
            pending.push_back(std::move(sample));
        }
        return true;
    }

    bool ParseCluster(const FileElement& cluster) {
        std::vector<std::uint8_t> payload;
        if (!ReadPayload(cluster, payload)) return false;
        std::int64_t clusterTimecode = 0;
        std::size_t position = 0;
        MemoryElement element;
        while (NextElement(payload.data(), payload.size(), position, element)) {
            if (element.id == 0xe7) {
                clusterTimecode = static_cast<std::int64_t>(
                    ReadBigEndian(element.data, element.size));
                break;
            }
        }
        position = 0;
        while (NextElement(payload.data(), payload.size(), position, element)) {
            if (element.id == 0xa3) {
                if (!ParseBlock(element.data, element.size, clusterTimecode, true,
                                false, 0)) {
                    return false;
                }
            } else if (element.id == 0xa0) {
                const MemoryElement* block = nullptr;
                MemoryElement blockStorage;
                bool hasReference = false;
                std::uint64_t duration = 0;
                std::size_t inner = 0;
                MemoryElement child;
                while (NextElement(element.data, element.size, inner, child)) {
                    if (child.id == 0xa1) {
                        blockStorage = child;
                        block = &blockStorage;
                    } else if (child.id == 0xfb) {
                        hasReference = true;
                    } else if (child.id == 0x9b) {
                        duration = ReadBigEndian(child.data, child.size);
                    }
                }
                if (block && !ParseBlock(block->data, block->size, clusterTimecode,
                                         false, !hasReference, duration)) {
                    return false;
                }
            }
        }
        return true;
    }

    bool FillPending() {
        while (pending.empty() && readPosition < segmentEnd) {
            FileElement element;
            if (!ReadElementHeader(readPosition, element)) return false;
            if (element.unknownSize) return Fail(L"Unknown-size Matroska clusters are not supported");
            readPosition = element.end;
            if (element.id == kCluster && !ParseCluster(element)) return false;
        }
        return true;
    }

    bool Open(const std::wstring& path) {
        Close();
        if (!file.Open(path, error)) return false;
        FileElement element;
        if (!ReadElementHeader(0, element) || element.id != kEbml) {
            return Fail(L"The file does not begin with an EBML header");
        }
        std::uint64_t position = element.end;
        bool foundSegment = false;
        while (position < file.Size()) {
            if (!ReadElementHeader(position, element)) return false;
            if (element.id == kSegment) {
                segmentDataOffset = element.dataOffset;
                segmentEnd = element.unknownSize ? file.Size() : element.end;
                foundSegment = true;
                break;
            }
            if (element.unknownSize) break;
            position = element.end;
        }
        if (!foundSegment) return Fail(L"The Matroska file has no Segment element");

        bool foundInfo = false;
        bool foundTracks = false;
        position = segmentDataOffset;
        while (position < segmentEnd) {
            if (!ReadElementHeader(position, element)) return false;
            if (element.id == kCluster) {
                firstClusterOffset = element.offset;
                break;
            }
            if (element.unknownSize) return Fail(L"Unknown-size Matroska metadata is not supported");
            if (element.id == kSeekHead || element.id == kInfo ||
                element.id == kTracks) {
                std::vector<std::uint8_t> payload;
                if (!ReadPayload(element, payload)) return false;
                if (element.id == kSeekHead) ParseSeekHead(payload);
                else if (element.id == kInfo) foundInfo = ParseInfo(payload);
                else if (element.id == kTracks) foundTracks = ParseTracks(payload);
                if ((element.id == kInfo && !foundInfo) ||
                    (element.id == kTracks && !foundTracks)) {
                    return false;
                }
            }
            position = element.end;
        }
        if (!foundInfo || !foundTracks || firstClusterOffset == 0)
            return Fail(L"The Matroska file is missing Info, Tracks, or Cluster data");

        Track* selectedVideo = nullptr;
        Track* selectedAudio = nullptr;
        Track* selectedSubtitle = nullptr;
        for (Track& track : tracks) {
            track.info.durationTicks = static_cast<std::uint64_t>(
                durationSeconds * kTimeScale);
            if (!selectedVideo && track.info.type == TrackType::Video)
                selectedVideo = &track;
            if (!selectedAudio && track.info.type == TrackType::Audio &&
                track.defaultTrack)
                selectedAudio = &track;
            if (!selectedSubtitle && track.info.type == TrackType::Subtitle &&
                track.defaultTrack)
                selectedSubtitle = &track;
        }
        if (!selectedAudio) {
            const auto found = std::find_if(tracks.begin(), tracks.end(),
                                            [](const Track& track) {
                                                return track.info.type == TrackType::Audio;
                                            });
            if (found != tracks.end()) selectedAudio = &*found;
        }
        if (!selectedSubtitle) {
            const auto found = std::find_if(tracks.begin(), tracks.end(),
                                            [](const Track& track) {
                                                return track.info.type ==
                                                       TrackType::Subtitle;
                                            });
            if (found != tracks.end()) selectedSubtitle = &*found;
        }
        if (!selectedVideo) return Fail(L"The Matroska file has no supported video track");
        selectedVideo->enabled = true;
        videoTrackNumber = selectedVideo->number;
        if (selectedAudio) selectedAudio->enabled = true;
        if (selectedSubtitle) selectedSubtitle->enabled = true;
        publicTracks.clear();
        for (const Track& track : tracks) publicTracks.push_back(track.info);
        if (!ParseCues()) return false;
        readPosition = firstClusterOffset;
        error.clear();
        return true;
    }

    void Close() {
        file.Close();
        tracks.clear();
        publicTracks.clear();
        cues.clear();
        pending.clear();
        error.clear();
        segmentDataOffset = segmentEnd = firstClusterOffset = readPosition = 0;
        timecodeScaleNs = 1'000'000;
        cuesOffset = videoTrackNumber = 0;
        durationSeconds = 0.0;
    }

    bool SetTrackEnabled(std::uint32_t trackId, bool enabled) {
        Track* track = FindTrack(trackId);
        if (!track) return Fail(L"The requested Matroska track does not exist");
        track->enabled = enabled;
        pending.clear();
        error.clear();
        return true;
    }

    bool ReadNext(EncodedSample& sample, bool& endOfFile) {
        endOfFile = false;
        if (!FillPending()) return false;
        if (pending.empty()) {
            sample = {};
            endOfFile = true;
            return true;
        }
        sample = std::move(pending.front());
        pending.pop_front();
        error.clear();
        return true;
    }

    bool Seek(double seconds, double& decodeStartSeconds) {
        if (!std::isfinite(seconds)) seconds = 0.0;
        seconds = std::max(0.0, std::min(durationSeconds, seconds));
        readPosition = firstClusterOffset;
        decodeStartSeconds = 0.0;
        if (!cues.empty()) {
            const std::int64_t targetUs = static_cast<std::int64_t>(
                std::llround(seconds * kTimeScale));
            const auto found = std::upper_bound(
                cues.begin(), cues.end(), targetUs,
                [](std::int64_t value, const Cue& cue) { return value < cue.timeUs; });
            const Cue& selected = found == cues.begin() ? cues.front() : *std::prev(found);
            readPosition = selected.clusterOffset;
            decodeStartSeconds = static_cast<double>(selected.timeUs) / kTimeScale;
        }
        pending.clear();
        error.clear();
        return true;
    }
};

MkvDemuxer::MkvDemuxer() : impl_(std::make_unique<Impl>()) {}
MkvDemuxer::~MkvDemuxer() = default;
bool MkvDemuxer::Open(const std::wstring& path) { return impl_->Open(path); }
void MkvDemuxer::Close() { impl_->Close(); }
const std::vector<TrackInfo>& MkvDemuxer::Tracks() const noexcept {
    return impl_->publicTracks;
}
double MkvDemuxer::DurationSeconds() const noexcept {
    return impl_->durationSeconds;
}
bool MkvDemuxer::SetTrackEnabled(std::uint32_t trackId, bool enabled) {
    return impl_->SetTrackEnabled(trackId, enabled);
}
bool MkvDemuxer::ReadNextSample(EncodedSample& sample, bool& endOfFile) {
    return impl_->ReadNext(sample, endOfFile);
}
bool MkvDemuxer::Seek(double seconds, double& decodeStartSeconds) {
    return impl_->Seek(seconds, decodeStartSeconds);
}
const std::wstring& MkvDemuxer::LastError() const noexcept { return impl_->error; }

}  // namespace movieplayer::codec::mkv
