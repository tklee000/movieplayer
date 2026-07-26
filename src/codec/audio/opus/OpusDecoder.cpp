#include "codec/audio/opus/OpusDecoder.h"

#include <opus.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace movieplayer::codec::opus {
namespace {

constexpr int kOpusSampleRate = 48'000;
constexpr int kMaximumPacketSamples = 5'760;
constexpr std::size_t kOpusHeadSize = 19;
constexpr char kOpusHeadSignature[] = "OpusHead";

std::uint16_t ReadLittleEndian16(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>(
        bytes[0] | (static_cast<std::uint16_t>(bytes[1]) << 8U));
}

std::wstring OpusErrorText(const wchar_t* operation, int code) {
    std::wstring message(operation);
    message += L" failed: ";
    const char* description = opus_strerror(code);
    if (description) {
        while (*description) {
            message.push_back(static_cast<unsigned char>(*description));
            ++description;
        }
    } else {
        message += L"unknown libopus error";
    }
    return message;
}

std::wstring OpusVersionText() {
    const char* version = opus_get_version_string();
    std::wstring result;
    if (version) {
        while (*version) {
            result.push_back(static_cast<unsigned char>(*version));
            ++version;
        }
    }
    return result.empty() ? L"libopus" : result;
}

}  // namespace

struct OpusDecoder::Impl {
    ::OpusDecoder* decoder = nullptr;
    TrackInfo track;
    int sourceChannels = 0;
    std::uint32_t preSkipRemaining = 0;
    std::array<float, 2> softClipMemory{};
    std::wstring description;
    std::wstring error;

    ~Impl() { Shutdown(); }

    bool Fail(const std::wstring& message) {
        error = message;
        return false;
    }

    void Shutdown() {
        if (decoder) {
            opus_decoder_destroy(decoder);
            decoder = nullptr;
        }
        sourceChannels = 0;
        preSkipRemaining = 0;
        softClipMemory = {};
    }

    bool Initialize(const TrackInfo& sourceTrack) {
        Shutdown();
        error.clear();
        description.clear();
        if (sourceTrack.codec != CodecId::Opus ||
            sourceTrack.codecPrivate.size() < kOpusHeadSize ||
            std::memcmp(sourceTrack.codecPrivate.data(), kOpusHeadSignature,
                        sizeof(kOpusHeadSignature) - 1U) != 0) {
            return Fail(L"OpusDecoder received an invalid Matroska Opus track");
        }
        const auto* header = sourceTrack.codecPrivate.data();
        const unsigned version = header[8];
        if ((version & 0xf0U) != 0) {
            return Fail(L"The OpusHead version is not supported");
        }
        sourceChannels = header[9];
        if (sourceChannels < 1 || sourceChannels > 2) {
            return Fail(L"Only mono and stereo Opus tracks are supported");
        }
        const unsigned mappingFamily = header[18];
        if (mappingFamily != 0) {
            return Fail(L"Only Opus channel mapping family 0 is supported");
        }
        if (sourceTrack.channels > 0 &&
            sourceTrack.channels != sourceChannels) {
            return Fail(L"The Matroska and OpusHead channel counts do not match");
        }

        int result = OPUS_OK;
        decoder = opus_decoder_create(kOpusSampleRate, sourceChannels, &result);
        if (!decoder || result != OPUS_OK) {
            if (decoder) {
                opus_decoder_destroy(decoder);
                decoder = nullptr;
            }
            return Fail(OpusErrorText(L"opus_decoder_create", result));
        }
        const auto outputGain =
            static_cast<std::int16_t>(ReadLittleEndian16(header + 16));
        result = opus_decoder_ctl(decoder, OPUS_SET_GAIN(outputGain));
        if (result != OPUS_OK) {
            Shutdown();
            return Fail(OpusErrorText(L"OPUS_SET_GAIN", result));
        }

        track = sourceTrack;
        track.sampleRate = kOpusSampleRate;
        track.channels = sourceChannels;
        preSkipRemaining = ReadLittleEndian16(header + 10);
        description = OpusVersionText() + L" Opus " +
                      std::to_wstring(kOpusSampleRate) + L" Hz " +
                      (sourceChannels == 1 ? L"mono-to-stereo decoder"
                                           : L"stereo decoder");
        error.clear();
        return true;
    }

    bool Decode(const EncodedSample& sample, AudioFrame& frame) {
        frame = {};
        if (!decoder) return Fail(L"The Opus decoder is not initialized");
        if (sample.bytes.empty() ||
            sample.bytes.size() >
                static_cast<std::size_t>((std::numeric_limits<opus_int32>::max)())) {
            return Fail(L"The Opus packet is empty or too large");
        }

        std::vector<float> decoded(
            static_cast<std::size_t>(kMaximumPacketSamples * sourceChannels));
        const int decodedSamples = opus_decode_float(
            decoder, sample.bytes.data(),
            static_cast<opus_int32>(sample.bytes.size()), decoded.data(),
            kMaximumPacketSamples, 0);
        if (decodedSamples < 0) {
            return Fail(OpusErrorText(L"opus_decode_float", decodedSamples));
        }
        opus_pcm_soft_clip(decoded.data(), decodedSamples, sourceChannels,
                           softClipMemory.data());

        const std::uint32_t skipped = std::min<std::uint32_t>(
            preSkipRemaining, static_cast<std::uint32_t>(decodedSamples));
        preSkipRemaining -= skipped;
        const int outputSamples = decodedSamples - static_cast<int>(skipped);
        if (outputSamples == 0) {
            error.clear();
            return true;
        }

        frame.sampleRate = kOpusSampleRate;
        frame.channels = 2;
        frame.channelMask = 3;
        frame.pts = sample.PtsSeconds() +
                    static_cast<double>(skipped) / kOpusSampleRate;
        frame.samples.resize(static_cast<std::size_t>(outputSamples) * 2U);
        const float* source =
            decoded.data() + static_cast<std::size_t>(skipped * sourceChannels);
        if (sourceChannels == 2) {
            std::copy_n(source, frame.samples.size(), frame.samples.begin());
        } else {
            for (int i = 0; i < outputSamples; ++i) {
                frame.samples[2U * static_cast<std::size_t>(i)] = source[i];
                frame.samples[2U * static_cast<std::size_t>(i) + 1U] = source[i];
            }
        }
        error.clear();
        return true;
    }

    void Reset() {
        if (decoder) {
            const int result = opus_decoder_ctl(decoder, OPUS_RESET_STATE);
            if (result != OPUS_OK) {
                error = OpusErrorText(L"OPUS_RESET_STATE", result);
                return;
            }
        }
        // OpusHead pre-skip applies only at the beginning of the stream, not
        // every time Matroska seeking resets the decoder state.
        preSkipRemaining = 0;
        softClipMemory = {};
        error.clear();
    }
};

OpusDecoder::OpusDecoder() : impl_(std::make_unique<Impl>()) {}
OpusDecoder::~OpusDecoder() = default;

bool OpusDecoder::Initialize(const TrackInfo& track) {
    return impl_->Initialize(track);
}

bool OpusDecoder::Decode(const EncodedSample& sample, AudioFrame& frame) {
    return impl_->Decode(sample, frame);
}

void OpusDecoder::Reset() { impl_->Reset(); }

const std::wstring& OpusDecoder::Description() const noexcept {
    return impl_->description;
}

const std::wstring& OpusDecoder::LastError() const noexcept {
    return impl_->error;
}

}  // namespace movieplayer::codec::opus
