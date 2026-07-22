#include "codec/container/avi/AviDemuxer.h"

#include "codec/core/RandomAccessFile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace movieplayer::codec::avi {
namespace {

constexpr std::uint32_t FourCc(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24U);
}

std::uint16_t ReadU16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(data[0]) |
           (static_cast<std::uint16_t>(data[1]) << 8U);
}

std::uint32_t ReadU32(const std::uint8_t* data) {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8U) |
           (static_cast<std::uint32_t>(data[2]) << 16U) |
           (static_cast<std::uint32_t>(data[3]) << 24U);
}

std::string FourCcText(std::uint32_t value) {
    std::string result(4, '\0');
    for (unsigned i = 0; i < 4; ++i)
        result[i] = static_cast<char>((value >> (8U * i)) & 0xffU);
    return result;
}

bool IsMpeg4Part2(std::uint32_t value) {
    std::string text = FourCcText(value);
    std::transform(text.begin(), text.end(), text.begin(),
                   [](char c) { return static_cast<char>(
                       std::toupper(static_cast<unsigned char>(c))); });
    return text == "XVID" || text == "DX50" || text == "DIVX" ||
           text == "MP4V" || text == "FMP4";
}

int StreamNumber(std::uint32_t chunkId) {
    const char first = static_cast<char>(chunkId & 0xffU);
    const char second = static_cast<char>((chunkId >> 8U) & 0xffU);
    if (first < '0' || first > '9' || second < '0' || second > '9') return -1;
    return (first - '0') * 10 + (second - '0');
}

}  // namespace

struct AviDemuxer::Impl {
    struct Sample {
        std::uint64_t dataOffset = 0;
        std::uint32_t size = 0;
        std::int64_t timestamp = 0;
        std::uint32_t duration = 0;
        std::size_t globalIndex = 0;
        bool sync = false;
    };

    struct Track {
        TrackInfo info;
        std::uint32_t streamIndex = 0;
        std::uint32_t scale = 0;
        std::uint32_t rate = 0;
        std::uint32_t start = 0;
        std::uint32_t declaredLength = 0;
        std::uint32_t sampleSize = 0;
        bool enabled = false;
        std::vector<Sample> samples;
        std::int64_t nextTimestamp = 0;
    };

    struct GlobalSample {
        std::size_t trackIndex = 0;
        std::size_t sampleIndex = 0;
    };

    RandomAccessFile file;
    std::vector<Track> tracks;
    std::vector<TrackInfo> publicTracks;
    std::vector<GlobalSample> samples;
    std::wstring error;
    std::uint64_t moviTypeOffset = 0;
    std::uint64_t indexOffset = 0;
    std::uint32_t indexSize = 0;
    std::size_t readIndex = 0;
    double durationSeconds = 0.0;

    bool Fail(const std::wstring& message) {
        error = message;
        return false;
    }

    bool ReadHeader(std::uint64_t offset, std::uint32_t& id,
                    std::uint32_t& size) {
        std::array<std::uint8_t, 8> bytes{};
        if (!file.Read(offset, bytes.data(), bytes.size(), error)) return false;
        id = ReadU32(bytes.data());
        size = ReadU32(bytes.data() + 4);
        return true;
    }

    bool ParseStreamList(const std::uint8_t* data, std::size_t size,
                         std::uint32_t streamIndex) {
        std::vector<std::uint8_t> header;
        std::vector<std::uint8_t> format;
        std::size_t position = 0;
        while (position + 8 <= size) {
            const std::uint32_t id = ReadU32(data + position);
            const std::uint32_t chunkSize = ReadU32(data + position + 4);
            const std::size_t payload = position + 8;
            if (chunkSize > size - payload) return Fail(L"Truncated AVI stream list");
            if (id == FourCc('s', 't', 'r', 'h'))
                header.assign(data + payload, data + payload + chunkSize);
            else if (id == FourCc('s', 't', 'r', 'f'))
                format.assign(data + payload, data + payload + chunkSize);
            position = payload + chunkSize + (chunkSize & 1U);
        }
        if (header.size() < 48 || format.empty()) return true;

        Track track;
        track.streamIndex = streamIndex;
        track.info.trackId = streamIndex + 1U;
        const std::uint32_t type = ReadU32(header.data());
        const std::uint32_t handler = ReadU32(header.data() + 4);
        track.scale = ReadU32(header.data() + 20);
        track.rate = ReadU32(header.data() + 24);
        track.start = ReadU32(header.data() + 28);
        track.declaredLength = ReadU32(header.data() + 32);
        track.sampleSize = ReadU32(header.data() + 44);
        if (track.scale == 0 || track.rate == 0) return true;
        track.info.timeScale = track.rate;
        track.info.durationTicks =
            static_cast<std::uint64_t>(track.declaredLength) * track.scale;

        if (type == FourCc('v', 'i', 'd', 's') && format.size() >= 40) {
            const std::uint32_t headerSize = ReadU32(format.data());
            const std::int32_t width = static_cast<std::int32_t>(ReadU32(format.data() + 4));
            const std::int32_t height = static_cast<std::int32_t>(ReadU32(format.data() + 8));
            const std::uint32_t compression = ReadU32(format.data() + 16);
            const std::uint32_t codec = handler != 0 ? handler : compression;
            if (!IsMpeg4Part2(codec) && !IsMpeg4Part2(compression)) return true;
            track.info.type = TrackType::Video;
            track.info.codec = CodecId::Mpeg4Part2;
            track.info.sampleEntry = FourCcText(codec);
            track.info.width = std::abs(width);
            track.info.height = std::abs(height);
            track.info.frameRate = {track.rate, track.scale};
            track.info.color.range = ColorRange::Limited;
            track.info.color.matrix = ColorMatrix::Bt601;
            track.info.color.primaries = ColorPrimaries::Bt709;
            track.info.color.transfer = TransferCharacteristic::Bt709;
            if (headerSize >= 40 && headerSize <= format.size()) {
                track.info.codecPrivate.assign(format.begin() + headerSize,
                                               format.end());
            }
        } else if (type == FourCc('a', 'u', 'd', 's') && format.size() >= 16) {
            const std::uint16_t formatTag = ReadU16(format.data());
            if (formatTag != 0x0055) return true;
            track.info.type = TrackType::Audio;
            track.info.codec = CodecId::Mp3;
            track.info.sampleEntry = "MP3 ";
            track.info.channels = ReadU16(format.data() + 2);
            track.info.sampleRate = static_cast<int>(ReadU32(format.data() + 4));
            track.info.bitsPerSample = ReadU16(format.data() + 14);
            track.info.codecPrivate = std::move(format);
        } else {
            return true;
        }
        tracks.push_back(std::move(track));
        return true;
    }

    bool ParseHeaderList(const std::vector<std::uint8_t>& bytes) {
        std::size_t position = 0;
        std::uint32_t streamIndex = 0;
        while (position + 8 <= bytes.size()) {
            const std::uint32_t id = ReadU32(bytes.data() + position);
            const std::uint32_t size = ReadU32(bytes.data() + position + 4);
            const std::size_t payload = position + 8;
            if (size > bytes.size() - payload) return Fail(L"Truncated AVI header list");
            if (id == FourCc('L', 'I', 'S', 'T') && size >= 4 &&
                ReadU32(bytes.data() + payload) == FourCc('s', 't', 'r', 'l')) {
                if (!ParseStreamList(bytes.data() + payload + 4, size - 4,
                                     streamIndex++)) {
                    return false;
                }
            }
            position = payload + size + (size & 1U);
        }
        return true;
    }

    Track* FindTrackByStream(int streamNumber) {
        const auto found = std::find_if(
            tracks.begin(), tracks.end(), [streamNumber](const Track& track) {
                return track.streamIndex == static_cast<std::uint32_t>(streamNumber);
            });
        return found == tracks.end() ? nullptr : &*found;
    }

    bool ResolveIndexBase(std::uint32_t chunkId, std::uint32_t offset,
                          std::uint64_t& base) {
        const std::uint64_t candidates[] = {moviTypeOffset, 0,
                                            moviTypeOffset + 4U};
        for (const std::uint64_t candidate : candidates) {
            if (offset > file.Size() - std::min(candidate, file.Size()) ||
                candidate + offset > file.Size() - 8) {
                continue;
            }
            std::uint32_t actual = 0, ignored = 0;
            if (ReadHeader(candidate + offset, actual, ignored) && actual == chunkId) {
                base = candidate;
                return true;
            }
        }
        return Fail(L"The AVI idx1 offsets do not point into the movi list");
    }

    std::uint32_t SampleDuration(Track& track, std::uint32_t byteSize) const {
        if (track.sampleSize != 0) {
            const std::uint64_t units = std::max<std::uint64_t>(
                1, static_cast<std::uint64_t>(byteSize) / track.sampleSize);
            return static_cast<std::uint32_t>(std::min<std::uint64_t>(
                static_cast<std::uint64_t>(track.scale) * units,
                (std::numeric_limits<std::uint32_t>::max)()));
        }
        return track.scale;
    }

    bool ParseIndex() {
        if (indexOffset == 0 || indexSize < 16 || (indexSize % 16U) != 0)
            return Fail(L"This AVI file has no usable classic idx1 index");
        std::vector<std::uint8_t> index;
        if (!file.Read(indexOffset, indexSize, index, error)) return false;
        const std::size_t count = index.size() / 16U;
        samples.reserve(count);
        std::uint64_t offsetBase = 0;
        bool baseResolved = false;
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint8_t* entry = index.data() + i * 16U;
            const std::uint32_t chunkId = ReadU32(entry);
            const std::uint32_t flags = ReadU32(entry + 4);
            const std::uint32_t relativeOffset = ReadU32(entry + 8);
            const std::uint32_t byteSize = ReadU32(entry + 12);
            Track* track = FindTrackByStream(StreamNumber(chunkId));
            if (!track || byteSize == 0) continue;
            if (!baseResolved) {
                if (!ResolveIndexBase(chunkId, relativeOffset, offsetBase)) return false;
                baseResolved = true;
            }
            const std::uint64_t headerOffset = offsetBase + relativeOffset;
            if (headerOffset > file.Size() - 8 || byteSize > file.Size() - headerOffset - 8)
                return Fail(L"An AVI idx1 sample points outside the file");
            const std::uint32_t duration = SampleDuration(*track, byteSize);
            Sample sample;
            sample.dataOffset = headerOffset + 8U;
            sample.size = byteSize;
            sample.timestamp = track->nextTimestamp +
                               static_cast<std::int64_t>(track->start) * track->scale;
            sample.duration = duration;
            sample.globalIndex = samples.size();
            sample.sync = track->info.type == TrackType::Audio ||
                          (flags & 0x10U) != 0;
            track->nextTimestamp += duration;
            const std::size_t trackIndex = static_cast<std::size_t>(track - tracks.data());
            const std::size_t sampleIndex = track->samples.size();
            track->samples.push_back(sample);
            samples.push_back({trackIndex, sampleIndex});
        }
        if (!baseResolved || samples.empty()) return Fail(L"The AVI idx1 index is empty");
        return true;
    }

    bool Open(const std::wstring& path) {
        Close();
        if (!file.Open(path, error)) return false;
        std::array<std::uint8_t, 12> riff{};
        if (!file.Read(0, riff.data(), riff.size(), error) ||
            ReadU32(riff.data()) != FourCc('R', 'I', 'F', 'F') ||
            ReadU32(riff.data() + 8) != FourCc('A', 'V', 'I', ' ')) {
            return Fail(L"The file is not a RIFF AVI container");
        }
        const std::uint64_t declaredEnd =
            std::min<std::uint64_t>(file.Size(), ReadU32(riff.data() + 4) + 8ULL);
        std::uint64_t position = 12;
        while (position + 8 <= declaredEnd) {
            std::uint32_t id = 0, size = 0;
            if (!ReadHeader(position, id, size)) return false;
            const std::uint64_t payload = position + 8U;
            if (size > declaredEnd - payload) return Fail(L"Truncated top-level AVI chunk");
            if (id == FourCc('L', 'I', 'S', 'T') && size >= 4) {
                std::uint32_t listType = 0;
                if (!file.Read(payload, &listType, sizeof(listType), error)) return false;
                if (listType == FourCc('h', 'd', 'r', 'l')) {
                    std::vector<std::uint8_t> header;
                    if (!file.Read(payload + 4U, size - 4U, header, error) ||
                        !ParseHeaderList(header)) {
                        return false;
                    }
                } else if (listType == FourCc('m', 'o', 'v', 'i')) {
                    moviTypeOffset = payload;
                }
            } else if (id == FourCc('i', 'd', 'x', '1')) {
                indexOffset = payload;
                indexSize = size;
            }
            position = payload + size + (size & 1U);
        }
        if (tracks.empty() || moviTypeOffset == 0)
            return Fail(L"The AVI file has no supported Xvid/DX50 or MP3 streams");
        if (!ParseIndex()) return false;

        bool selectedVideo = false;
        bool selectedAudio = false;
        publicTracks.clear();
        durationSeconds = 0.0;
        for (Track& track : tracks) {
            track.info.sampleCount = track.samples.size();
            if (!track.samples.empty()) {
                const Sample& last = track.samples.back();
                track.info.durationTicks = static_cast<std::uint64_t>(
                    std::max<std::int64_t>(0, last.timestamp)) + last.duration;
            }
            if (!selectedVideo && track.info.type == TrackType::Video) {
                track.enabled = true;
                selectedVideo = true;
            } else if (!selectedAudio && track.info.type == TrackType::Audio) {
                track.enabled = true;
                selectedAudio = true;
            }
            durationSeconds = std::max(durationSeconds, track.info.DurationSeconds());
            publicTracks.push_back(track.info);
        }
        if (!selectedVideo) return Fail(L"The AVI file has no supported video stream");
        readIndex = 0;
        error.clear();
        return true;
    }

    void Close() {
        file.Close();
        tracks.clear();
        publicTracks.clear();
        samples.clear();
        error.clear();
        moviTypeOffset = indexOffset = 0;
        indexSize = 0;
        readIndex = 0;
        durationSeconds = 0.0;
    }

    bool SetTrackEnabled(std::uint32_t trackId, bool enabled) {
        const auto found = std::find_if(tracks.begin(), tracks.end(),
                                        [trackId](const Track& track) {
                                            return track.info.trackId == trackId;
                                        });
        if (found == tracks.end()) return Fail(L"The requested AVI track does not exist");
        found->enabled = enabled;
        error.clear();
        return true;
    }

    bool ReadNext(EncodedSample& output, bool& endOfFile) {
        endOfFile = false;
        while (readIndex < samples.size()) {
            const GlobalSample reference = samples[readIndex++];
            Track& track = tracks[reference.trackIndex];
            if (!track.enabled) continue;
            const Sample& source = track.samples[reference.sampleIndex];
            output = {};
            output.trackId = track.info.trackId;
            output.type = track.info.type;
            output.decodeTime = source.timestamp;
            output.presentationTime = source.timestamp;
            output.duration = source.duration;
            output.timeScale = track.rate;
            output.sync = source.sync;
            if (!file.Read(source.dataOffset, source.size, output.bytes, error)) return false;
            error.clear();
            return true;
        }
        output = {};
        endOfFile = true;
        return true;
    }

    bool Seek(double seconds, double& decodeStartSeconds) {
        if (!std::isfinite(seconds)) seconds = 0.0;
        seconds = std::max(0.0, std::min(durationSeconds, seconds));
        const Track* video = nullptr;
        for (const Track& track : tracks) {
            if (track.enabled && track.info.type == TrackType::Video) {
                video = &track;
                break;
            }
        }
        if (!video || video->samples.empty()) return Fail(L"No indexed AVI video samples to seek");
        const std::int64_t target = static_cast<std::int64_t>(
            std::llround(seconds * video->rate));
        auto found = std::upper_bound(
            video->samples.begin(), video->samples.end(), target,
            [](std::int64_t value, const Sample& sample) {
                return value < sample.timestamp;
            });
        if (found != video->samples.begin()) --found;
        while (found != video->samples.begin() && !found->sync) --found;
        if (!found->sync) {
            const auto nextKey = std::find_if(found, video->samples.end(),
                                              [](const Sample& sample) { return sample.sync; });
            if (nextKey == video->samples.end()) return Fail(L"The AVI index has no keyframe");
            found = nextKey;
        }
        readIndex = found->globalIndex;
        decodeStartSeconds = static_cast<double>(found->timestamp) / video->rate;
        error.clear();
        return true;
    }
};

AviDemuxer::AviDemuxer() : impl_(std::make_unique<Impl>()) {}
AviDemuxer::~AviDemuxer() = default;
bool AviDemuxer::Open(const std::wstring& path) { return impl_->Open(path); }
void AviDemuxer::Close() { impl_->Close(); }
const std::vector<TrackInfo>& AviDemuxer::Tracks() const noexcept {
    return impl_->publicTracks;
}
double AviDemuxer::DurationSeconds() const noexcept { return impl_->durationSeconds; }
bool AviDemuxer::SetTrackEnabled(std::uint32_t trackId, bool enabled) {
    return impl_->SetTrackEnabled(trackId, enabled);
}
bool AviDemuxer::ReadNextSample(EncodedSample& sample, bool& endOfFile) {
    return impl_->ReadNext(sample, endOfFile);
}
bool AviDemuxer::Seek(double seconds, double& decodeStartSeconds) {
    return impl_->Seek(seconds, decodeStartSeconds);
}
const std::wstring& AviDemuxer::LastError() const noexcept { return impl_->error; }

}  // namespace movieplayer::codec::avi
