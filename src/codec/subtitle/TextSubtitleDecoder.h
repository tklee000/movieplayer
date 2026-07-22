#pragma once

#include "codec/core/CodecTypes.h"

#include <string>

namespace movieplayer::codec::subtitle {

// Decodes Matroska UTF-8 and ASS event payloads into plain display text.
bool DecodeTextSample(const TrackInfo& track, const EncodedSample& sample,
                      std::wstring& text, std::wstring& error);

}  // namespace movieplayer::codec::subtitle
