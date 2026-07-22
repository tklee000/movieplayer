#pragma once

#include "codec/video/hevc/HevcStructures.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace movieplayer::codec::hevc {

class HevcBitstreamParser {
public:
    bool Initialize(const TrackInfo& track);
    bool ParseAccessUnit(const EncodedSample& sample, AccessUnit& accessUnit);
    void Reset();

    unsigned NalLengthSize() const noexcept { return nalLengthSize_; }
    const std::wstring& LastError() const noexcept { return error_; }

private:
    bool ParseNalUnit(const std::uint8_t* data, std::size_t size,
                      int nalUnitType, AccessUnit* accessUnit);
    bool ParseVps(const std::uint8_t* data, std::size_t size);
    bool ParseSps(const std::uint8_t* data, std::size_t size);
    bool ParsePps(const std::uint8_t* data, std::size_t size);
    bool ParseFirstSlice(const std::uint8_t* data, std::size_t size,
                         int nalUnitType, int temporalId, AccessUnit& accessUnit);
    bool Fail(const std::wstring& message);

    unsigned nalLengthSize_ = 0;
    std::array<SequenceParameterSet, 16> sps_{};
    std::array<bool, 16> hasSps_{};
    std::array<PictureParameterSet, 64> pps_{};
    std::array<bool, 64> hasPps_{};
    int previousPocLsb_ = 0;
    int previousPocMsb_ = 0;
    bool havePreviousPoc_ = false;
    std::wstring error_;
};

}  // namespace movieplayer::codec::hevc
