#pragma once

#include "codec/core/BitReader.h"

#include <array>
#include <cstdint>
#include <string>

namespace movieplayer::codec::aac {

class AacHuffman {
public:
    static bool DecodeScaleFactor(BitReader& bits, int& difference);
    static bool DecodeSpectral(BitReader& bits, unsigned codebook,
                               std::array<int, 4>& values, unsigned& dimension);
};

}  // namespace movieplayer::codec::aac
