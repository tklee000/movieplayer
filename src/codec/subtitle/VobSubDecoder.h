#pragma once

#include "codec/core/CodecTypes.h"

#include <string>

namespace movieplayer::codec::subtitle {

struct VobSubFrame {
    SubtitleBitmap bitmap;
    double startDelaySeconds = 0.0;
    double endDelaySeconds = 0.0;
    bool forced = false;
};

// Decodes one Matroska S_VOBSUB sample (an unpacked DVD SPU packet).
bool DecodeVobSubSample(const TrackInfo& track, const EncodedSample& sample,
                        VobSubFrame& frame, std::wstring& error);

}  // namespace movieplayer::codec::subtitle
