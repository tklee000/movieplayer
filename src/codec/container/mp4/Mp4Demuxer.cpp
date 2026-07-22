#include "codec/container/mp4/Mp4Demuxer.h"

#include "codec/core/ByteReader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_set>
#include <utility>

namespace movieplayer::codec::mp4 {
namespace {

constexpr std::uint64_t kMaximumMetadataBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kMaximumSamples = 20U * 1000U * 1000U;
constexpr std::uint32_t kMaximumSampleBytes = 128U * 1024U * 1024U;

std::uint16_t ReadBe16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

std::uint32_t ReadBe32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

std::uint64_t ReadBe64(const std::uint8_t* p) {
    return (static_cast<std::uint64_t>(ReadBe32(p)) << 32) | ReadBe32(p + 4);
}

struct Box {
    std::string type;
    std::size_t start = 0;
    std::size_t size = 0;
    std::size_t header = 0;

    std::size_t Payload() const { return start + header; }
    std::size_t End() const { return start + size; }
};

bool NextBox(const std::vector<std::uint8_t>& bytes, std::size_t end,
             std::size_t& position, Box& box) {
    if (position > end || end - position < 8 || end > bytes.size()) {
        return false;
    }
    const std::uint8_t* p = bytes.data() + position;
    std::uint64_t size = ReadBe32(p);
    std::size_t header = 8;
    if (size == 1) {
        if (end - position < 16) {
            return false;
        }
        size = ReadBe64(p + 8);
        header = 16;
    } else if (size == 0) {
        size = end - position;
    }
    if (size < header || size > end - position ||
        size > std::numeric_limits<std::size_t>::max()) {
        return false;
    }
    box.type.assign(reinterpret_cast<const char*>(p + 4), 4);
    box.start = position;
    box.size = static_cast<std::size_t>(size);
    box.header = header;
    position += box.size;
    return true;
}

bool FindChild(const std::vector<std::uint8_t>& bytes, const Box& parent,
               const char* type, Box& result) {
    std::size_t position = parent.Payload();
    while (position < parent.End()) {
        Box child;
        if (!NextBox(bytes, parent.End(), position, child)) {
            return false;
        }
        if (child.type == type) {
            result = child;
            return true;
        }
    }
    return false;
}

std::vector<Box> Children(const std::vector<std::uint8_t>& bytes,
                          const Box& parent, std::size_t prefix = 0) {
    std::vector<Box> result;
    if (prefix > parent.size - parent.header) {
        return result;
    }
    std::size_t position = parent.Payload() + prefix;
    while (position < parent.End()) {
        Box child;
        if (!NextBox(bytes, parent.End(), position, child)) {
            result.clear();
            return result;
        }
        result.push_back(child);
    }
    return result;
}

std::string DecodeLanguage(std::uint16_t packed) {
    std::string result(3, 'u');
    for (int i = 0; i < 3; ++i) {
        const unsigned shift = static_cast<unsigned>((2 - i) * 5);
        const unsigned code = (packed >> shift) & 31U;
        result[static_cast<std::size_t>(i)] =
            code != 0 ? static_cast<char>('a' + code - 1U) : 'u';
    }
    return result;
}

bool ReadDescriptorLength(const std::uint8_t* bytes, std::size_t size,
                          std::size_t& position, std::size_t& value) {
    value = 0;
    for (int i = 0; i < 4; ++i) {
        if (position >= size) {
            return false;
        }
        const std::uint8_t b = bytes[position++];
        value = (value << 7U) | (b & 0x7fU);
        if ((b & 0x80U) == 0) {
            return true;
        }
    }
    return false;
}

bool FindAudioSpecificConfig(const std::uint8_t* bytes, std::size_t size,
                             std::vector<std::uint8_t>& config) {
    for (std::size_t i = 0; i + 3 <= size; ++i) {
        if (bytes[i] != 0x05) {
            continue;
        }
        std::size_t position = i + 1;
        std::size_t length = 0;
        if (!ReadDescriptorLength(bytes, size, position, length) || length < 2 ||
            length > 64 || position > size || length > size - position) {
            continue;
        }
        const unsigned audioObjectType = bytes[position] >> 3U;
        if (audioObjectType == 2 || audioObjectType == 5 || audioObjectType == 29) {
            config.assign(bytes + position, bytes + position + length);
            return true;
        }
    }
    return false;
}

ColorPrimaries MapPrimaries(std::uint16_t value) {
    if (value == 1) return ColorPrimaries::Bt709;
    if (value == 9) return ColorPrimaries::Bt2020;
    return ColorPrimaries::Unspecified;
}

TransferCharacteristic MapTransfer(std::uint16_t value) {
    if (value == 1 || value == 6) return TransferCharacteristic::Bt709;
    if (value == 16) return TransferCharacteristic::Pq;
    if (value == 18) return TransferCharacteristic::Hlg;
    return TransferCharacteristic::Unspecified;
}

ColorMatrix MapMatrix(std::uint16_t value) {
    if (value == 1) return ColorMatrix::Bt709;
    if (value == 5 || value == 6) return ColorMatrix::Bt601;
    if (value == 9) return ColorMatrix::Bt2020NonConstant;
    if (value == 10) return ColorMatrix::Bt2020Constant;
    return ColorMatrix::Unspecified;
}

}  // namespace

struct Mp4Demuxer::Impl {
    struct Sample {
        std::uint64_t offset = 0;
        std::uint32_t size = 0;
        std::int64_t dts = 0;
        std::int64_t pts = 0;
        std::uint32_t duration = 0;
        bool sync = false;
    };

    struct Track {
        TrackInfo info;
        std::vector<Sample> samples;
        std::size_t cursor = 0;
        bool enabled = true;
    };

    RandomAccessFile file;
    std::vector<Track> tracks;
    std::vector<TrackInfo> publicTracks;
    std::wstring error;
    double durationSeconds = 0.0;
    std::uint32_t movieTimeScale = 0;
    std::uint64_t movieDuration = 0;

    bool Fail(const std::wstring& message) {
        error = message;
        return false;
    }

    static bool ReadFullBox(const std::vector<std::uint8_t>& data, const Box& box,
                            std::uint8_t& version, std::uint32_t& flags,
                            ByteReader& payload) {
        if (box.size < box.header + 4) {
            return false;
        }
        ByteReader reader(data.data() + box.Payload(), box.size - box.header);
        std::uint32_t vf = 0;
        if (!reader.ReadU32(vf)) {
            return false;
        }
        version = static_cast<std::uint8_t>(vf >> 24U);
        flags = vf & 0x00ffffffU;
        payload = ByteReader(data.data() + box.Payload() + 4,
                             box.size - box.header - 4);
        return true;
    }

    bool ParseMovieHeader(const std::vector<std::uint8_t>& data, const Box& box) {
        std::uint8_t version = 0;
        std::uint32_t flags = 0;
        ByteReader reader;
        if (!ReadFullBox(data, box, version, flags, reader)) {
            return Fail(L"Invalid mvhd box");
        }
        (void)flags;
        if (version == 1) {
            std::uint64_t ignored = 0;
            if (!reader.ReadU64(ignored) || !reader.ReadU64(ignored) ||
                !reader.ReadU32(movieTimeScale) || !reader.ReadU64(movieDuration)) {
                return Fail(L"Truncated version 1 mvhd box");
            }
        } else if (version == 0) {
            std::uint32_t ignored = 0;
            std::uint32_t duration = 0;
            if (!reader.ReadU32(ignored) || !reader.ReadU32(ignored) ||
                !reader.ReadU32(movieTimeScale) || !reader.ReadU32(duration)) {
                return Fail(L"Truncated version 0 mvhd box");
            }
            movieDuration = duration;
        } else {
            return Fail(L"Unsupported mvhd version");
        }
        if (movieTimeScale == 0) {
            return Fail(L"The MP4 movie time scale is zero");
        }
        durationSeconds = static_cast<double>(movieDuration) / movieTimeScale;
        return true;
    }

    bool ParseTrackHeader(const std::vector<std::uint8_t>& data, const Box& box,
                          TrackInfo& info) {
        std::uint8_t version = 0;
        std::uint32_t flags = 0;
        ByteReader reader;
        if (!ReadFullBox(data, box, version, flags, reader)) {
            return Fail(L"Invalid tkhd box");
        }
        (void)flags;
        if (version == 1) {
            std::uint64_t ignored64 = 0;
            std::uint32_t ignored32 = 0;
            if (!reader.ReadU64(ignored64) || !reader.ReadU64(ignored64) ||
                !reader.ReadU32(info.trackId) || !reader.ReadU32(ignored32) ||
                !reader.ReadU64(ignored64)) {
                return Fail(L"Truncated version 1 tkhd box");
            }
        } else if (version == 0) {
            std::uint32_t ignored = 0;
            if (!reader.ReadU32(ignored) || !reader.ReadU32(ignored) ||
                !reader.ReadU32(info.trackId) || !reader.ReadU32(ignored) ||
                !reader.ReadU32(ignored)) {
                return Fail(L"Truncated version 0 tkhd box");
            }
        } else {
            return Fail(L"Unsupported tkhd version");
        }
        return true;
    }

    bool ParseMediaHeader(const std::vector<std::uint8_t>& data, const Box& box,
                          TrackInfo& info) {
        std::uint8_t version = 0;
        std::uint32_t flags = 0;
        ByteReader reader;
        if (!ReadFullBox(data, box, version, flags, reader)) {
            return Fail(L"Invalid mdhd box");
        }
        (void)flags;
        std::uint16_t language = 0;
        if (version == 1) {
            std::uint64_t ignored = 0;
            if (!reader.ReadU64(ignored) || !reader.ReadU64(ignored) ||
                !reader.ReadU32(info.timeScale) ||
                !reader.ReadU64(info.durationTicks) ||
                !reader.ReadU16(language)) {
                return Fail(L"Truncated version 1 mdhd box");
            }
        } else if (version == 0) {
            std::uint32_t ignored = 0;
            std::uint32_t duration = 0;
            if (!reader.ReadU32(ignored) || !reader.ReadU32(ignored) ||
                !reader.ReadU32(info.timeScale) || !reader.ReadU32(duration) ||
                !reader.ReadU16(language)) {
                return Fail(L"Truncated version 0 mdhd box");
            }
            info.durationTicks = duration;
        } else {
            return Fail(L"Unsupported mdhd version");
        }
        if (info.timeScale == 0) {
            return Fail(L"An MP4 track has a zero time scale");
        }
        info.language = DecodeLanguage(language);
        return true;
    }

    bool ParseHandler(const std::vector<std::uint8_t>& data, const Box& box,
                      TrackInfo& info) {
        if (box.size < box.header + 12) {
            return Fail(L"Truncated hdlr box");
        }
        const char* type = reinterpret_cast<const char*>(
            data.data() + box.Payload() + 8);
        const std::string handler(type, 4);
        if (handler == "vide") {
            info.type = TrackType::Video;
        } else if (handler == "soun") {
            info.type = TrackType::Audio;
        }
        return true;
    }

    void ParseVisualChild(const std::vector<std::uint8_t>& data, const Box& child,
                          TrackInfo& info) {
        const std::size_t payloadSize = child.size - child.header;
        const std::uint8_t* p = data.data() + child.Payload();
        if (child.type == "hvcC" || child.type == "avcC") {
            info.codecPrivate.assign(p, p + payloadSize);
        } else if (child.type == "pasp" && payloadSize >= 8) {
            info.sampleAspectRatio = {
                static_cast<std::int64_t>(ReadBe32(p)),
                static_cast<std::int64_t>(ReadBe32(p + 4))};
        } else if (child.type == "colr" && payloadSize >= 11 &&
                   std::string(reinterpret_cast<const char*>(p), 4) == "nclx") {
            info.color.primaries = MapPrimaries(ReadBe16(p + 4));
            info.color.transfer = MapTransfer(ReadBe16(p + 6));
            info.color.matrix = MapMatrix(ReadBe16(p + 8));
            info.color.range = (p[10] & 0x80U) != 0
                                   ? ColorRange::Full
                                   : ColorRange::Limited;
        }
    }

    bool ParseSampleDescription(const std::vector<std::uint8_t>& data,
                                const Box& stsd, TrackInfo& info) {
        if (stsd.size < stsd.header + 8) {
            return Fail(L"Truncated stsd box");
        }
        const std::uint8_t* p = data.data() + stsd.Payload();
        const std::uint32_t entryCount = ReadBe32(p + 4);
        if (entryCount == 0) {
            return Fail(L"An MP4 track has no sample description");
        }
        std::size_t position = stsd.Payload() + 8;
        Box entry;
        if (!NextBox(data, stsd.End(), position, entry)) {
            return Fail(L"Invalid MP4 sample entry");
        }
        info.sampleEntry = entry.type;
        const std::size_t payloadSize = entry.size - entry.header;
        const std::uint8_t* entryData = data.data() + entry.Payload();
        if (info.type == TrackType::Video) {
            if (payloadSize < 78) {
                return Fail(L"Truncated visual sample entry");
            }
            if (entry.type == "hvc1" || entry.type == "hev1") {
                info.codec = CodecId::Hevc;
            } else if (entry.type == "avc1" || entry.type == "avc3") {
                info.codec = CodecId::H264;
            } else {
                return Fail(L"Only H.264 avc1/avc3 and HEVC hvc1/hev1 MP4 video is supported");
            }
            info.width = ReadBe16(entryData + 24);
            info.height = ReadBe16(entryData + 26);
            info.sampleAspectRatio = {1, 1};
            const auto children = Children(data, entry, 78);
            if (children.empty()) {
                return Fail(L"The video sample entry has no codec configuration box");
            }
            for (const Box& child : children) {
                ParseVisualChild(data, child, info);
            }
            if (info.codecPrivate.empty()) {
                return Fail(info.codec == CodecId::H264
                                ? L"The H.264 sample entry is missing avcC data"
                                : L"The HEVC sample entry is missing hvcC data");
            }
        } else if (info.type == TrackType::Audio) {
            if (payloadSize < 28) {
                return Fail(L"Truncated audio sample entry");
            }
            if (entry.type != "mp4a") {
                return Fail(L"Only AAC mp4a audio is supported");
            }
            info.codec = CodecId::Aac;
            const std::uint16_t soundVersion = ReadBe16(entryData + 8);
            info.channels = ReadBe16(entryData + 16);
            info.bitsPerSample = ReadBe16(entryData + 18);
            info.sampleRate = static_cast<int>(ReadBe32(entryData + 24) >> 16U);
            const std::size_t childPrefix =
                soundVersion == 0 ? 28 : (soundVersion == 1 ? 44 : 64);
            const auto children = Children(data, entry, childPrefix);
            for (const Box& child : children) {
                if (child.type == "esds" && child.size >= child.header + 4) {
                    const std::uint8_t* esds = data.data() + child.Payload() + 4;
                    const std::size_t size = child.size - child.header - 4;
                    FindAudioSpecificConfig(esds, size, info.codecPrivate);
                }
            }
            if (info.codecPrivate.empty()) {
                return Fail(L"The AAC sample entry is missing AudioSpecificConfig");
            }
        }
        return true;
    }

    bool ParseTimeToSample(const std::vector<std::uint8_t>& data, const Box& box,
                           std::vector<std::uint32_t>& durations) {
        if (box.size < box.header + 8) {
            return Fail(L"Truncated stts box");
        }
        ByteReader reader(data.data() + box.Payload() + 4,
                          box.size - box.header - 4);
        std::uint32_t entryCount = 0;
        if (!reader.ReadU32(entryCount)) {
            return Fail(L"Invalid stts entry count");
        }
        std::uint64_t total = 0;
        struct Entry { std::uint32_t count; std::uint32_t delta; };
        std::vector<Entry> entries;
        entries.reserve(entryCount);
        for (std::uint32_t i = 0; i < entryCount; ++i) {
            Entry entry = {};
            if (!reader.ReadU32(entry.count) || !reader.ReadU32(entry.delta) ||
                entry.count == 0 || total + entry.count > kMaximumSamples) {
                return Fail(L"Invalid stts entry");
            }
            total += entry.count;
            entries.push_back(entry);
        }
        durations.reserve(static_cast<std::size_t>(total));
        for (const Entry& entry : entries) {
            durations.insert(durations.end(), entry.count, entry.delta);
        }
        return true;
    }

    bool ParseCompositionOffsets(const std::vector<std::uint8_t>& data,
                                 const Box* box, std::size_t sampleCount,
                                 std::vector<std::int64_t>& offsets) {
        offsets.assign(sampleCount, 0);
        if (!box) {
            return true;
        }
        if (box->size < box->header + 8) {
            return Fail(L"Truncated ctts box");
        }
        const std::uint8_t version = data[box->Payload()];
        ByteReader reader(data.data() + box->Payload() + 4,
                          box->size - box->header - 4);
        std::uint32_t entryCount = 0;
        if (!reader.ReadU32(entryCount)) {
            return Fail(L"Invalid ctts entry count");
        }
        std::size_t output = 0;
        for (std::uint32_t i = 0; i < entryCount; ++i) {
            std::uint32_t count = 0;
            std::uint32_t rawOffset = 0;
            if (!reader.ReadU32(count) || !reader.ReadU32(rawOffset) ||
                count > sampleCount - output) {
                return Fail(L"Invalid ctts entry");
            }
            const std::int64_t offset = version == 1
                                            ? static_cast<std::int32_t>(rawOffset)
                                            : static_cast<std::int64_t>(rawOffset);
            std::fill_n(offsets.begin() + static_cast<std::ptrdiff_t>(output),
                        count, offset);
            output += count;
        }
        return output == sampleCount || Fail(L"ctts does not cover every sample");
    }

    bool ParseSampleSizes(const std::vector<std::uint8_t>& data, const Box& box,
                          std::vector<std::uint32_t>& sizes) {
        if (box.size < box.header + 12) {
            return Fail(L"Truncated stsz box");
        }
        ByteReader reader(data.data() + box.Payload() + 4,
                          box.size - box.header - 4);
        std::uint32_t constantSize = 0;
        std::uint32_t count = 0;
        if (!reader.ReadU32(constantSize) || !reader.ReadU32(count) ||
            count > kMaximumSamples) {
            return Fail(L"Invalid stsz header");
        }
        sizes.resize(count, constantSize);
        if (constantSize == 0) {
            for (std::uint32_t& size : sizes) {
                if (!reader.ReadU32(size) || size > kMaximumSampleBytes) {
                    return Fail(L"Invalid MP4 sample size");
                }
            }
        } else if (constantSize > kMaximumSampleBytes) {
            return Fail(L"MP4 constant sample size is too large");
        }
        return true;
    }

    bool ParseChunkOffsets(const std::vector<std::uint8_t>& data, const Box& box,
                           std::vector<std::uint64_t>& offsets) {
        if (box.size < box.header + 8) {
            return Fail(L"Truncated chunk offset box");
        }
        ByteReader reader(data.data() + box.Payload() + 4,
                          box.size - box.header - 4);
        std::uint32_t count = 0;
        if (!reader.ReadU32(count) || count > kMaximumSamples) {
            return Fail(L"Invalid chunk offset count");
        }
        offsets.resize(count);
        for (std::uint64_t& offset : offsets) {
            if (box.type == "co64") {
                if (!reader.ReadU64(offset)) return Fail(L"Truncated co64 box");
            } else {
                std::uint32_t offset32 = 0;
                if (!reader.ReadU32(offset32)) return Fail(L"Truncated stco box");
                offset = offset32;
            }
        }
        return true;
    }

    struct SampleToChunk {
        std::uint32_t firstChunk = 0;
        std::uint32_t samplesPerChunk = 0;
        std::uint32_t description = 0;
    };

    bool ParseSampleToChunk(const std::vector<std::uint8_t>& data, const Box& box,
                            std::vector<SampleToChunk>& entries) {
        if (box.size < box.header + 8) {
            return Fail(L"Truncated stsc box");
        }
        ByteReader reader(data.data() + box.Payload() + 4,
                          box.size - box.header - 4);
        std::uint32_t count = 0;
        if (!reader.ReadU32(count) || count == 0 || count > kMaximumSamples) {
            return Fail(L"Invalid stsc entry count");
        }
        entries.resize(count);
        std::uint32_t previousChunk = 0;
        for (SampleToChunk& entry : entries) {
            if (!reader.ReadU32(entry.firstChunk) ||
                !reader.ReadU32(entry.samplesPerChunk) ||
                !reader.ReadU32(entry.description) ||
                entry.firstChunk <= previousChunk || entry.samplesPerChunk == 0 ||
                entry.description == 0) {
                return Fail(L"Invalid stsc entry");
            }
            previousChunk = entry.firstChunk;
        }
        return true;
    }

    bool ParseSyncSamples(const std::vector<std::uint8_t>& data, const Box* box,
                          std::size_t sampleCount,
                          std::unordered_set<std::uint32_t>& syncSamples) {
        if (!box) {
            for (std::uint32_t i = 1; i <= sampleCount; ++i) {
                syncSamples.insert(i);
            }
            return true;
        }
        if (box->size < box->header + 8) {
            return Fail(L"Truncated stss box");
        }
        ByteReader reader(data.data() + box->Payload() + 4,
                          box->size - box->header - 4);
        std::uint32_t count = 0;
        if (!reader.ReadU32(count) || count > sampleCount) {
            return Fail(L"Invalid stss entry count");
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t index = 0;
            if (!reader.ReadU32(index) || index == 0 || index > sampleCount) {
                return Fail(L"Invalid stss sample index");
            }
            syncSamples.insert(index);
        }
        return true;
    }

    bool BuildSamples(const std::vector<std::uint32_t>& sizes,
                      const std::vector<std::uint32_t>& durations,
                      const std::vector<std::int64_t>& compositionOffsets,
                      const std::vector<std::uint64_t>& chunkOffsets,
                      const std::vector<SampleToChunk>& sampleToChunk,
                      const std::unordered_set<std::uint32_t>& syncSamples,
                      Track& track) {
        if (sizes.size() != durations.size() ||
            sizes.size() != compositionOffsets.size()) {
            return Fail(L"The MP4 sample tables have inconsistent counts");
        }
        track.samples.resize(sizes.size());
        std::size_t sampleIndex = 0;
        for (std::size_t chunkIndex = 0; chunkIndex < chunkOffsets.size(); ++chunkIndex) {
            const std::uint32_t chunkNumber = static_cast<std::uint32_t>(chunkIndex + 1);
            auto next = std::upper_bound(
                sampleToChunk.begin(), sampleToChunk.end(), chunkNumber,
                [](std::uint32_t value, const SampleToChunk& entry) {
                    return value < entry.firstChunk;
                });
            if (next == sampleToChunk.begin()) {
                return Fail(L"stsc does not describe the first MP4 chunk");
            }
            const SampleToChunk& mapping = *std::prev(next);
            std::uint64_t offset = chunkOffsets[chunkIndex];
            for (std::uint32_t j = 0; j < mapping.samplesPerChunk; ++j) {
                if (sampleIndex >= sizes.size()) {
                    return Fail(L"stsc describes more samples than stsz");
                }
                const std::uint32_t size = sizes[sampleIndex];
                if (offset > file.Size() || size > file.Size() - offset) {
                    return Fail(L"An MP4 sample points outside the file");
                }
                Sample& sample = track.samples[sampleIndex];
                sample.offset = offset;
                sample.size = size;
                sample.duration = durations[sampleIndex];
                sample.sync = syncSamples.find(
                                  static_cast<std::uint32_t>(sampleIndex + 1)) !=
                              syncSamples.end();
                offset += size;
                ++sampleIndex;
            }
        }
        if (sampleIndex != sizes.size()) {
            return Fail(L"stsc describes fewer samples than stsz");
        }
        std::int64_t dts = 0;
        for (std::size_t i = 0; i < track.samples.size(); ++i) {
            track.samples[i].dts = dts;
            track.samples[i].pts = dts + compositionOffsets[i];
            if (durations[i] > static_cast<std::uint64_t>(
                                   std::numeric_limits<std::int64_t>::max() - dts)) {
                return Fail(L"MP4 track timestamps overflow 64 bits");
            }
            dts += durations[i];
        }
        track.info.sampleCount = track.samples.size();
        if (track.info.type == TrackType::Video && dts > 0 &&
            track.samples.size() > 1) {
            track.info.frameRate = {
                static_cast<std::int64_t>(track.samples.size()) * track.info.timeScale,
                dts};
        }
        return true;
    }

    bool ParseSampleTable(const std::vector<std::uint8_t>& data, const Box& stbl,
                          Track& track) {
        Box stsd, stts, stsz, stsc, stco;
        if (!FindChild(data, stbl, "stsd", stsd) ||
            !FindChild(data, stbl, "stts", stts) ||
            !FindChild(data, stbl, "stsz", stsz) ||
            !FindChild(data, stbl, "stsc", stsc)) {
            return Fail(L"An MP4 track is missing a required sample table");
        }
        if (!FindChild(data, stbl, "co64", stco) &&
            !FindChild(data, stbl, "stco", stco)) {
            return Fail(L"An MP4 track has no chunk offset table");
        }
        if (!ParseSampleDescription(data, stsd, track.info)) {
            return false;
        }

        std::vector<std::uint32_t> durations;
        std::vector<std::uint32_t> sizes;
        std::vector<std::int64_t> compositionOffsets;
        std::vector<std::uint64_t> chunkOffsets;
        std::vector<SampleToChunk> sampleToChunk;
        std::unordered_set<std::uint32_t> syncSamples;
        Box ctts, stss;
        const Box* cttsPointer = FindChild(data, stbl, "ctts", ctts) ? &ctts : nullptr;
        const Box* stssPointer = FindChild(data, stbl, "stss", stss) ? &stss : nullptr;
        if (!ParseTimeToSample(data, stts, durations) ||
            !ParseCompositionOffsets(data, cttsPointer, durations.size(),
                                     compositionOffsets) ||
            !ParseSampleSizes(data, stsz, sizes) ||
            !ParseChunkOffsets(data, stco, chunkOffsets) ||
            !ParseSampleToChunk(data, stsc, sampleToChunk) ||
            !ParseSyncSamples(data, stssPointer, sizes.size(), syncSamples)) {
            return false;
        }
        return BuildSamples(sizes, durations, compositionOffsets, chunkOffsets,
                            sampleToChunk, syncSamples, track);
    }

    bool ParseTrack(const std::vector<std::uint8_t>& data, const Box& trak) {
        Box tkhd, mdia, mdhd, hdlr, minf, stbl;
        if (!FindChild(data, trak, "tkhd", tkhd) ||
            !FindChild(data, trak, "mdia", mdia) ||
            !FindChild(data, mdia, "mdhd", mdhd) ||
            !FindChild(data, mdia, "hdlr", hdlr) ||
            !FindChild(data, mdia, "minf", minf) ||
            !FindChild(data, minf, "stbl", stbl)) {
            return Fail(L"A trak box is missing required children");
        }
        Track track;
        if (!ParseTrackHeader(data, tkhd, track.info) ||
            !ParseMediaHeader(data, mdhd, track.info) ||
            !ParseHandler(data, hdlr, track.info)) {
            return false;
        }
        if (track.info.type == TrackType::Unknown) {
            return true;
        }
        if (!ParseSampleTable(data, stbl, track)) {
            return false;
        }
        tracks.push_back(std::move(track));
        return true;
    }

    bool ParseMoov(std::uint64_t offset, std::uint64_t size,
                   std::uint64_t headerSize) {
        if (size < headerSize || size - headerSize > kMaximumMetadataBytes ||
            size > std::numeric_limits<std::size_t>::max()) {
            return Fail(L"The MP4 moov box is unreasonably large");
        }
        std::vector<std::uint8_t> data;
        if (!file.Read(offset, static_cast<std::size_t>(size), data, error)) {
            return false;
        }
        Box moov{"moov", 0, data.size(), static_cast<std::size_t>(headerSize)};
        Box mvhd;
        if (!FindChild(data, moov, "mvhd", mvhd) ||
            !ParseMovieHeader(data, mvhd)) {
            return false;
        }
        for (const Box& child : Children(data, moov)) {
            if (child.type == "trak" && !ParseTrack(data, child)) {
                return false;
            }
        }
        if (tracks.empty()) {
            return Fail(L"The MP4 file has no supported audio or video tracks");
        }
        bool hasVideo = false;
        for (const Track& track : tracks) {
            hasVideo = hasVideo || track.info.type == TrackType::Video;
        }
        if (!hasVideo) {
            return Fail(L"The MP4 file has no supported H.264 or HEVC video track");
        }
        publicTracks.clear();
        for (const Track& track : tracks) {
            publicTracks.push_back(track.info);
            durationSeconds = std::max(durationSeconds, track.info.DurationSeconds());
        }
        return true;
    }

    bool Open(const std::wstring& path) {
        Close();
        if (!file.Open(path, error)) {
            return false;
        }
        std::uint64_t position = 0;
        bool foundFtyp = false;
        bool foundMoov = false;
        while (position + 8 <= file.Size()) {
            std::array<std::uint8_t, 16> header = {};
            const std::size_t available = static_cast<std::size_t>(
                std::min<std::uint64_t>(header.size(), file.Size() - position));
            if (!file.Read(position, header.data(), available, error)) {
                return false;
            }
            std::uint64_t size = ReadBe32(header.data());
            std::uint64_t headerSize = 8;
            if (size == 1) {
                if (available < 16) return Fail(L"Truncated extended MP4 box header");
                size = ReadBe64(header.data() + 8);
                headerSize = 16;
            } else if (size == 0) {
                size = file.Size() - position;
            }
            if (size < headerSize || size > file.Size() - position) {
                return Fail(L"An MP4 top-level box has an invalid size");
            }
            const std::string type(reinterpret_cast<const char*>(header.data() + 4), 4);
            if (type == "ftyp") foundFtyp = true;
            if (type == "moof") return Fail(L"Fragmented MP4 is not supported yet");
            if (type == "moov") {
                if (!ParseMoov(position, size, headerSize)) {
                    return false;
                }
                foundMoov = true;
                break;
            }
            position += size;
        }
        if (!foundFtyp) return Fail(L"The file is not an ISO Base Media MP4 file");
        if (!foundMoov) return Fail(L"The MP4 file has no moov box");
        error.clear();
        return true;
    }

    void Close() {
        file.Close();
        tracks.clear();
        publicTracks.clear();
        durationSeconds = 0.0;
        movieTimeScale = 0;
        movieDuration = 0;
        error.clear();
    }

    bool ReadNext(EncodedSample& output, bool& endOfFile) {
        endOfFile = false;
        Track* selected = nullptr;
        for (Track& track : tracks) {
            if (!track.enabled) continue;
            if (track.cursor >= track.samples.size()) continue;
            if (!selected) {
                selected = &track;
                continue;
            }
            const Sample& a = track.samples[track.cursor];
            const Sample& b = selected->samples[selected->cursor];
            const long double aTime = static_cast<long double>(a.dts) /
                                      track.info.timeScale;
            const long double bTime = static_cast<long double>(b.dts) /
                                      selected->info.timeScale;
            if (aTime < bTime) selected = &track;
        }
        if (!selected) {
            endOfFile = true;
            output = {};
            return true;
        }
        const Sample& source = selected->samples[selected->cursor++];
        output = {};
        output.trackId = selected->info.trackId;
        output.type = selected->info.type;
        output.decodeTime = source.dts;
        output.presentationTime = source.pts;
        output.duration = source.duration;
        output.timeScale = selected->info.timeScale;
        output.sync = source.sync;
        return file.Read(source.offset, source.size, output.bytes, error);
    }

    bool SetTrackEnabled(std::uint32_t trackId, bool enabled) {
        const auto found = std::find_if(
            tracks.begin(), tracks.end(), [trackId](const Track& track) {
                return track.info.trackId == trackId;
            });
        if (found == tracks.end()) return Fail(L"The requested MP4 track does not exist");
        found->enabled = enabled;
        error.clear();
        return true;
    }

    bool Seek(double seconds, double& decodeStartSeconds) {
        if (!std::isfinite(seconds)) seconds = 0.0;
        seconds = std::max(0.0, std::min(durationSeconds, seconds));
        Track* video = nullptr;
        for (Track& track : tracks) {
            if (track.info.type == TrackType::Video) {
                video = &track;
                break;
            }
        }
        if (!video || video->samples.empty()) return Fail(L"No video samples to seek");
        const long double targetTicks =
            static_cast<long double>(seconds) * video->info.timeScale;
        auto it = std::upper_bound(
            video->samples.begin(), video->samples.end(), targetTicks,
            [](long double value, const Sample& sample) {
                return value < static_cast<long double>(sample.pts);
            });
        std::size_t index = it == video->samples.begin()
                                ? 0
                                : static_cast<std::size_t>(std::prev(it) -
                                                           video->samples.begin());
        while (index > 0 && !video->samples[index].sync) --index;
        if (!video->samples[index].sync) {
            auto firstSync = std::find_if(video->samples.begin(), video->samples.end(),
                                          [](const Sample& s) { return s.sync; });
            if (firstSync == video->samples.end()) {
                return Fail(L"The MP4 video track has no sync sample");
            }
            index = static_cast<std::size_t>(firstSync - video->samples.begin());
        }
        video->cursor = index;
        decodeStartSeconds = static_cast<double>(video->samples[index].dts) /
                             video->info.timeScale;
        for (Track& track : tracks) {
            if (&track == video) continue;
            const long double startTicks =
                static_cast<long double>(decodeStartSeconds) * track.info.timeScale;
            auto sample = std::lower_bound(
                track.samples.begin(), track.samples.end(), startTicks,
                [](const Sample& item, long double value) {
                    return static_cast<long double>(item.dts) < value;
                });
            track.cursor = static_cast<std::size_t>(sample - track.samples.begin());
        }
        error.clear();
        return true;
    }
};

Mp4Demuxer::Mp4Demuxer() : impl_(std::make_unique<Impl>()) {}
Mp4Demuxer::~Mp4Demuxer() = default;

bool Mp4Demuxer::Open(const std::wstring& path) { return impl_->Open(path); }
void Mp4Demuxer::Close() { impl_->Close(); }
const std::vector<TrackInfo>& Mp4Demuxer::Tracks() const noexcept {
    return impl_->publicTracks;
}
double Mp4Demuxer::DurationSeconds() const noexcept {
    return impl_->durationSeconds;
}
bool Mp4Demuxer::SetTrackEnabled(std::uint32_t trackId, bool enabled) {
    return impl_->SetTrackEnabled(trackId, enabled);
}
bool Mp4Demuxer::ReadNextSample(EncodedSample& sample, bool& endOfFile) {
    return impl_->ReadNext(sample, endOfFile);
}
bool Mp4Demuxer::Seek(double seconds, double& decodeStartSeconds) {
    return impl_->Seek(seconds, decodeStartSeconds);
}
const std::wstring& Mp4Demuxer::LastError() const noexcept { return impl_->error; }

}  // namespace movieplayer::codec::mp4
