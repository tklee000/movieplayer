#include "codec/video/hevc/HevcBitstreamParser.h"

#include "codec/core/BitReader.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

namespace movieplayer::codec::hevc {
namespace {

bool Flag(BitReader& bits, bool& value) {
    return bits.ReadBit(value);
}

bool Ue(BitReader& bits, unsigned& value, unsigned maximum = 1U << 24U) {
    std::uint32_t result = 0;
    if (!bits.ReadUnsignedExpGolomb(result) || result > maximum) {
        return false;
    }
    value = result;
    return true;
}

bool Se(BitReader& bits, int& value, int minimum = -(1 << 20),
        int maximum = 1 << 20) {
    std::int32_t result = 0;
    if (!bits.ReadSignedExpGolomb(result) || result < minimum || result > maximum) {
        return false;
    }
    value = result;
    return true;
}

unsigned CeilLog2(unsigned value) {
    unsigned bits = 0;
    if (value > 0) --value;
    while (value != 0) {
        ++bits;
        value >>= 1U;
    }
    return bits;
}

bool SkipProfileTierLevel(BitReader& bits, unsigned maxSubLayersMinus1) {
    // general_profile_space through general_level_idc
    if (!bits.SkipBits(96)) return false;
    bool profilePresent[8] = {};
    bool levelPresent[8] = {};
    for (unsigned i = 0; i < maxSubLayersMinus1; ++i) {
        if (!Flag(bits, profilePresent[i]) || !Flag(bits, levelPresent[i])) {
            return false;
        }
    }
    if (maxSubLayersMinus1 > 0) {
        if (!bits.SkipBits((8U - maxSubLayersMinus1) * 2U)) return false;
    }
    for (unsigned i = 0; i < maxSubLayersMinus1; ++i) {
        if (profilePresent[i] && !bits.SkipBits(88)) return false;
        if (levelPresent[i] && !bits.SkipBits(8)) return false;
    }
    return true;
}

std::array<std::uint8_t, 64>* Matrix64(ScalingLists& lists, unsigned sizeId,
                                      unsigned matrixId) {
    if (sizeId == 1 && matrixId < 6) return &lists.size1[matrixId];
    if (sizeId == 2 && matrixId < 6) return &lists.size2[matrixId];
    if (sizeId == 3 && (matrixId == 0 || matrixId == 3)) {
        return &lists.size3[matrixId / 3];
    }
    return nullptr;
}

bool ParseScalingList(BitReader& bits, ScalingLists& lists) {
    for (unsigned sizeId = 0; sizeId < 4; ++sizeId) {
        const unsigned step = sizeId == 3 ? 3 : 1;
        for (unsigned matrixId = 0; matrixId < 6; matrixId += step) {
            bool predictionMode = false;
            if (!Flag(bits, predictionMode)) return false;
            if (!predictionMode) {
                unsigned delta = 0;
                if (!Ue(bits, delta, matrixId)) return false;
                if (delta != 0) {
                    const unsigned sourceId = matrixId - delta;
                    if (sizeId == 0) {
                        lists.size0[matrixId] = lists.size0[sourceId];
                    } else {
                        auto* destination = Matrix64(lists, sizeId, matrixId);
                        auto* source = Matrix64(lists, sizeId, sourceId);
                        if (!destination || !source) return false;
                        *destination = *source;
                        if (sizeId == 2) {
                            lists.dcSize2[matrixId] = lists.dcSize2[sourceId];
                        } else if (sizeId == 3) {
                            lists.dcSize3[matrixId / 3] =
                                lists.dcSize3[sourceId / 3];
                        }
                    }
                }
                continue;
            }
            const unsigned coefficientCount =
                std::min(64U, 1U << (4U + (sizeId << 1U)));
            int nextCoefficient = 8;
            if (sizeId > 1) {
                int dcMinus8 = 0;
                if (!Se(bits, dcMinus8, -7, 247)) return false;
                nextCoefficient = dcMinus8 + 8;
                if (sizeId == 2) {
                    lists.dcSize2[matrixId] =
                        static_cast<std::uint8_t>(nextCoefficient);
                } else {
                    lists.dcSize3[matrixId / 3] =
                        static_cast<std::uint8_t>(nextCoefficient);
                }
            }
            for (unsigned i = 0; i < coefficientCount; ++i) {
                int delta = 0;
                if (!Se(bits, delta, -128, 127)) return false;
                nextCoefficient = (nextCoefficient + delta + 256) & 255;
                if (sizeId == 0) {
                    lists.size0[matrixId][i] =
                        static_cast<std::uint8_t>(nextCoefficient);
                } else {
                    auto* matrix = Matrix64(lists, sizeId, matrixId);
                    if (!matrix) return false;
                    (*matrix)[i] = static_cast<std::uint8_t>(nextCoefficient);
                }
            }
        }
    }
    return true;
}

bool ParseShortTermReferenceSet(BitReader& bits, unsigned index,
                                unsigned setCount,
                                const std::vector<ShortTermReferenceSet>& previous,
                                ShortTermReferenceSet& result,
                                unsigned* deltaPocsOfReference = nullptr) {
    bool predicted = false;
    if (index != 0 && !Flag(bits, predicted)) return false;
    if (!predicted) {
        unsigned negativeCount = 0;
        unsigned positiveCount = 0;
        if (!Ue(bits, negativeCount, 16) || !Ue(bits, positiveCount, 16) ||
            negativeCount + positiveCount > 16) {
            return false;
        }
        int delta = 0;
        for (unsigned i = 0; i < negativeCount; ++i) {
            unsigned minus1 = 0;
            bool used = false;
            if (!Ue(bits, minus1, 32767) || !Flag(bits, used)) return false;
            delta -= static_cast<int>(minus1 + 1);
            result.negative.push_back({delta, used});
        }
        delta = 0;
        for (unsigned i = 0; i < positiveCount; ++i) {
            unsigned minus1 = 0;
            bool used = false;
            if (!Ue(bits, minus1, 32767) || !Flag(bits, used)) return false;
            delta += static_cast<int>(minus1 + 1);
            result.positive.push_back({delta, used});
        }
        if (deltaPocsOfReference) *deltaPocsOfReference = 0;
        return true;
    }

    unsigned deltaIndexMinus1 = 0;
    if (index == setCount && !Ue(bits, deltaIndexMinus1, index - 1)) return false;
    if (deltaIndexMinus1 + 1 > index) return false;
    const unsigned referenceIndex = index - (deltaIndexMinus1 + 1);
    if (referenceIndex >= previous.size()) return false;
    const ShortTermReferenceSet& reference = previous[referenceIndex];
    const unsigned referenceCount =
        static_cast<unsigned>(reference.DeltaPocCount());
    if (deltaPocsOfReference) *deltaPocsOfReference = referenceCount;
    bool deltaSign = false;
    unsigned absoluteDeltaMinus1 = 0;
    if (!Flag(bits, deltaSign) || !Ue(bits, absoluteDeltaMinus1, 32767)) {
        return false;
    }
    const int deltaRps = (deltaSign ? -1 : 1) *
                         static_cast<int>(absoluteDeltaMinus1 + 1);
    std::vector<bool> used(referenceCount + 1, false);
    std::vector<bool> useDelta(referenceCount + 1, true);
    for (unsigned j = 0; j <= referenceCount; ++j) {
        bool usedFlag = false;
        if (!Flag(bits, usedFlag)) return false;
        used[j] = usedFlag;
        if (!usedFlag) {
            bool useDeltaFlag = false;
            if (!Flag(bits, useDeltaFlag)) return false;
            useDelta[j] = useDeltaFlag;
        }
    }

    const unsigned negativeCount = static_cast<unsigned>(reference.negative.size());
    const unsigned positiveCount = static_cast<unsigned>(reference.positive.size());
    for (int j = static_cast<int>(positiveCount) - 1; j >= 0; --j) {
        const int deltaPoc = reference.positive[static_cast<std::size_t>(j)].deltaPoc +
                             deltaRps;
        const unsigned flagIndex = negativeCount + static_cast<unsigned>(j);
        if (deltaPoc < 0 && useDelta[flagIndex]) {
            result.negative.push_back({deltaPoc, used[flagIndex]});
        }
    }
    if (deltaRps < 0 && useDelta[referenceCount]) {
        result.negative.push_back({deltaRps, used[referenceCount]});
    }
    for (unsigned j = 0; j < negativeCount; ++j) {
        const int deltaPoc = reference.negative[j].deltaPoc + deltaRps;
        if (deltaPoc < 0 && useDelta[j]) {
            result.negative.push_back({deltaPoc, used[j]});
        }
    }
    for (int j = static_cast<int>(negativeCount) - 1; j >= 0; --j) {
        const int deltaPoc = reference.negative[static_cast<std::size_t>(j)].deltaPoc +
                             deltaRps;
        if (deltaPoc > 0 && useDelta[static_cast<std::size_t>(j)]) {
            result.positive.push_back(
                {deltaPoc, used[static_cast<std::size_t>(j)]});
        }
    }
    if (deltaRps > 0 && useDelta[referenceCount]) {
        result.positive.push_back({deltaRps, used[referenceCount]});
    }
    for (unsigned j = 0; j < positiveCount; ++j) {
        const int deltaPoc = reference.positive[j].deltaPoc + deltaRps;
        const unsigned flagIndex = negativeCount + j;
        if (deltaPoc > 0 && useDelta[flagIndex]) {
            result.positive.push_back({deltaPoc, used[flagIndex]});
        }
    }
    return result.DeltaPocCount() <= 16;
}

void ParseVuiPrefix(BitReader& bits, SequenceParameterSet& sps) {
    bool present = false;
    if (!Flag(bits, present)) return;
    if (present) {
        std::uint32_t aspectIdc = 0;
        if (!bits.ReadBits(8, aspectIdc)) return;
        static constexpr Rational kAspectRatios[] = {
            {0, 1}, {1, 1}, {12, 11}, {10, 11}, {16, 11}, {40, 33},
            {24, 11}, {20, 11}, {32, 11}, {80, 33}, {18, 11}, {15, 11},
            {64, 33}, {160, 99}, {4, 3}, {3, 2}, {2, 1}};
        if (aspectIdc == 255) {
            std::uint32_t width = 0, height = 0;
            if (!bits.ReadBits(16, width) || !bits.ReadBits(16, height)) return;
            if (width != 0 && height != 0) sps.sampleAspectRatio = {width, height};
        } else if (aspectIdc < std::size(kAspectRatios) && aspectIdc != 0) {
            sps.sampleAspectRatio = kAspectRatios[aspectIdc];
        }
    }
    if (!Flag(bits, present)) return;
    if (present && !bits.SkipBits(1)) return;
    if (!Flag(bits, present)) return;
    if (present) {
        if (!bits.SkipBits(3)) return;
        bool fullRange = false;
        bool colorDescription = false;
        if (!Flag(bits, fullRange) || !Flag(bits, colorDescription)) return;
        sps.color.range = fullRange ? ColorRange::Full : ColorRange::Limited;
        if (colorDescription) {
            std::uint32_t primaries = 0, transfer = 0, matrix = 0;
            if (!bits.ReadBits(8, primaries) || !bits.ReadBits(8, transfer) ||
                !bits.ReadBits(8, matrix)) return;
            if (primaries == 1) sps.color.primaries = ColorPrimaries::Bt709;
            if (primaries == 9) sps.color.primaries = ColorPrimaries::Bt2020;
            if (transfer == 1 || transfer == 6)
                sps.color.transfer = TransferCharacteristic::Bt709;
            if (transfer == 16) sps.color.transfer = TransferCharacteristic::Pq;
            if (transfer == 18) sps.color.transfer = TransferCharacteristic::Hlg;
            if (matrix == 1) sps.color.matrix = ColorMatrix::Bt709;
            if (matrix == 5 || matrix == 6) sps.color.matrix = ColorMatrix::Bt601;
            if (matrix == 9) sps.color.matrix = ColorMatrix::Bt2020NonConstant;
            if (matrix == 10) sps.color.matrix = ColorMatrix::Bt2020Constant;
        }
    }
    if (!Flag(bits, present)) return;
    if (present) {
        unsigned top = 0, bottom = 0;
        if (!Ue(bits, top, 5) || !Ue(bits, bottom, 5)) return;
        if (top == 0 && bottom == 0) sps.color.chromaLocation = ChromaLocation::Left;
        if (top == 2 && bottom == 2)
            sps.color.chromaLocation = ChromaLocation::TopLeft;
    }
}

bool IsIrap(int nalType) { return nalType >= 16 && nalType <= 23; }
bool IsIdr(int nalType) { return nalType == 19 || nalType == 20; }
bool IsBla(int nalType) { return nalType >= 16 && nalType <= 18; }
bool IsReferenceNal(int nalType) {
    if (nalType >= 16 && nalType <= 23) return true;
    return nalType >= 0 && nalType <= 9 && (nalType & 1) != 0;
}

}  // namespace

bool HevcBitstreamParser::Fail(const std::wstring& message) {
    error_ = message;
    return false;
}

void HevcBitstreamParser::Reset() {
    previousPocLsb_ = 0;
    previousPocMsb_ = 0;
    havePreviousPoc_ = false;
    error_.clear();
}

bool HevcBitstreamParser::Initialize(const TrackInfo& track) {
    hasSps_.fill(false);
    hasPps_.fill(false);
    Reset();
    if (track.codec != CodecId::Hevc || track.codecPrivate.size() < 23 ||
        track.codecPrivate[0] != 1) {
        return Fail(L"Invalid HEVCDecoderConfigurationRecord");
    }
    const unsigned profileIdc = track.codecPrivate[1] & 31U;
    const unsigned chromaFormat = track.codecPrivate[16] & 3U;
    const unsigned lumaDepth = 8U + (track.codecPrivate[17] & 7U);
    const unsigned chromaDepth = 8U + (track.codecPrivate[18] & 7U);
    if (profileIdc != 2 || chromaFormat != 1 || lumaDepth != 10 ||
        chromaDepth != 10) {
        return Fail(L"Only HEVC Main 10 4:2:0 is supported");
    }
    nalLengthSize_ = 1U + (track.codecPrivate[21] & 3U);
    if (nalLengthSize_ < 1 || nalLengthSize_ > 4) {
        return Fail(L"Invalid HEVC NAL length size");
    }
    std::size_t position = 23;
    const unsigned arrayCount = track.codecPrivate[22];
    for (unsigned array = 0; array < arrayCount; ++array) {
        if (position + 3 > track.codecPrivate.size()) {
            return Fail(L"Truncated hvcC NAL array");
        }
        const int nalType = track.codecPrivate[position] & 0x3fU;
        const unsigned count =
            (static_cast<unsigned>(track.codecPrivate[position + 1]) << 8U) |
            track.codecPrivate[position + 2];
        position += 3;
        for (unsigned i = 0; i < count; ++i) {
            if (position + 2 > track.codecPrivate.size()) {
                return Fail(L"Truncated hvcC NAL length");
            }
            const std::size_t length =
                (static_cast<std::size_t>(track.codecPrivate[position]) << 8U) |
                track.codecPrivate[position + 1];
            position += 2;
            if (length < 2 || length > track.codecPrivate.size() - position ||
                !ParseNalUnit(track.codecPrivate.data() + position, length,
                              nalType, nullptr)) {
                return Fail(error_.empty() ? L"Invalid hvcC parameter set" : error_);
            }
            position += length;
        }
    }
    if (std::none_of(hasSps_.begin(), hasSps_.end(), [](bool v) { return v; }) ||
        std::none_of(hasPps_.begin(), hasPps_.end(), [](bool v) { return v; })) {
        return Fail(L"hvcC does not contain both SPS and PPS parameter sets");
    }
    error_.clear();
    return true;
}

bool HevcBitstreamParser::ParseNalUnit(const std::uint8_t* data, std::size_t size,
                                       int nalUnitType, AccessUnit* accessUnit) {
    if (!data || size < 2 || (data[0] & 0x80U) != 0 || (data[1] & 7U) == 0) {
        return Fail(L"Invalid HEVC NAL header");
    }
    const int headerType = (data[0] >> 1U) & 0x3fU;
    if (headerType != nalUnitType) nalUnitType = headerType;
    if (nalUnitType == 32) return ParseVps(data + 2, size - 2);
    if (nalUnitType == 33) return ParseSps(data + 2, size - 2);
    if (nalUnitType == 34) return ParsePps(data + 2, size - 2);
    if (nalUnitType <= 31 && accessUnit && !accessUnit->sps) {
        const int temporalId = static_cast<int>((data[1] & 7U) - 1U);
        return ParseFirstSlice(data + 2, size - 2, nalUnitType, temporalId,
                               *accessUnit);
    }
    return true;
}

bool HevcBitstreamParser::ParseVps(const std::uint8_t* data, std::size_t size) {
    const auto rbsp = RemoveEmulationPreventionBytes(data, size);
    BitReader bits(rbsp);
    std::uint32_t id = 0;
    if (!bits.ReadBits(4, id) || id > 15) return Fail(L"Invalid HEVC VPS");
    return true;
}

bool HevcBitstreamParser::ParseSps(const std::uint8_t* data, std::size_t size) {
    const auto rbsp = RemoveEmulationPreventionBytes(data, size);
    BitReader bits(rbsp);
    SequenceParameterSet sps;
    std::uint32_t value = 0;
    if (!bits.ReadBits(4, value)) return Fail(L"Truncated SPS vps id");
    sps.vpsId = value;
    if (!bits.ReadBits(3, value)) return Fail(L"Truncated SPS sub-layer count");
    sps.maxSubLayersMinus1 = value;
    bool ignoredFlag = false;
    if (sps.maxSubLayersMinus1 > 6 || !Flag(bits, ignoredFlag) ||
        !SkipProfileTierLevel(bits, sps.maxSubLayersMinus1) ||
        !Ue(bits, sps.id, 15) || !Ue(bits, sps.chromaFormatIdc, 3)) {
        return Fail(L"Invalid SPS profile or identifiers");
    }
    if (sps.chromaFormatIdc == 3 && !Flag(bits, sps.separateColourPlaneFlag)) {
        return Fail(L"Truncated SPS separate colour plane flag");
    }
    if (!Ue(bits, sps.width, 32768) || !Ue(bits, sps.height, 32768) ||
        sps.width == 0 || sps.height == 0) {
        return Fail(L"Invalid SPS dimensions");
    }
    bool conformanceWindow = false;
    if (!Flag(bits, conformanceWindow)) return Fail(L"Truncated SPS conformance flag");
    if (conformanceWindow) {
        unsigned crop = 0;
        for (int i = 0; i < 4; ++i) {
            if (!Ue(bits, crop, 32768)) return Fail(L"Invalid SPS conformance window");
        }
    }
    if (!Ue(bits, sps.bitDepthLumaMinus8, 6) ||
        !Ue(bits, sps.bitDepthChromaMinus8, 6) ||
        !Ue(bits, sps.log2MaxPicOrderCntLsbMinus4, 12)) {
        return Fail(L"Invalid SPS bit depth or POC range");
    }
    bool orderingInfoPresent = false;
    if (!Flag(bits, orderingInfoPresent)) return Fail(L"Truncated SPS ordering flag");
    const unsigned firstLayer = orderingInfoPresent ? 0 : sps.maxSubLayersMinus1;
    for (unsigned i = firstLayer; i <= sps.maxSubLayersMinus1; ++i) {
        unsigned buffering = 0, reorder = 0, latency = 0;
        if (!Ue(bits, buffering, 15) || !Ue(bits, reorder, 15) ||
            !Ue(bits, latency, 1U << 24U)) {
            return Fail(L"Invalid SPS sub-layer ordering info");
        }
        if (i == sps.maxSubLayersMinus1) {
            sps.maxDecPicBufferingMinus1 = buffering;
            sps.maxNumReorderPics = reorder;
        }
    }
    if (!Ue(bits, sps.log2MinLumaCodingBlockSizeMinus3, 3) ||
        !Ue(bits, sps.log2DiffMaxMinLumaCodingBlockSize, 6) ||
        !Ue(bits, sps.log2MinTransformBlockSizeMinus2, 3) ||
        !Ue(bits, sps.log2DiffMaxMinTransformBlockSize, 3) ||
        !Ue(bits, sps.maxTransformHierarchyDepthInter, 4) ||
        !Ue(bits, sps.maxTransformHierarchyDepthIntra, 4) ||
        !Flag(bits, sps.scalingListEnabled)) {
        return Fail(L"Invalid SPS coding block parameters");
    }
    if (sps.scalingListEnabled) {
        bool present = false;
        if (!Flag(bits, present) || (present && !ParseScalingList(bits, sps.scalingLists))) {
            return Fail(L"Invalid SPS scaling list");
        }
    }
    if (!Flag(bits, sps.ampEnabled) ||
        !Flag(bits, sps.sampleAdaptiveOffsetEnabled) ||
        !Flag(bits, sps.pcmEnabled)) {
        return Fail(L"Truncated SPS coding tool flags");
    }
    if (sps.pcmEnabled) {
        if (!bits.ReadBits(4, value)) return Fail(L"Invalid SPS PCM luma depth");
        sps.pcmSampleBitDepthLumaMinus1 = value;
        if (!bits.ReadBits(4, value)) return Fail(L"Invalid SPS PCM chroma depth");
        sps.pcmSampleBitDepthChromaMinus1 = value;
        if (!Ue(bits, sps.log2MinPcmLumaCodingBlockSizeMinus3, 2) ||
            !Ue(bits, sps.log2DiffMaxMinPcmLumaCodingBlockSize, 4) ||
            !Flag(bits, sps.pcmLoopFilterDisabled)) {
            return Fail(L"Invalid SPS PCM parameters");
        }
    }
    unsigned rpsCount = 0;
    if (!Ue(bits, rpsCount, 64)) return Fail(L"Invalid SPS short-term RPS count");
    sps.shortTermReferenceSets.reserve(rpsCount);
    for (unsigned i = 0; i < rpsCount; ++i) {
        ShortTermReferenceSet set;
        if (!ParseShortTermReferenceSet(bits, i, rpsCount,
                                        sps.shortTermReferenceSets, set)) {
            return Fail(L"Invalid SPS short-term reference picture set");
        }
        sps.shortTermReferenceSets.push_back(std::move(set));
    }
    if (!Flag(bits, sps.longTermRefPicsPresent)) {
        return Fail(L"Truncated SPS long-term reference flag");
    }
    if (sps.longTermRefPicsPresent) {
        if (!Ue(bits, sps.numLongTermRefPicsSps, 16)) {
            return Fail(L"Invalid SPS long-term reference count");
        }
        const unsigned pocBits = sps.log2MaxPicOrderCntLsbMinus4 + 4;
        for (unsigned i = 0; i < sps.numLongTermRefPicsSps; ++i) {
            if (!bits.SkipBits(pocBits + 1U)) {
                return Fail(L"Truncated SPS long-term reference");
            }
        }
    }
    if (!Flag(bits, sps.temporalMvpEnabled) ||
        !Flag(bits, sps.strongIntraSmoothingEnabled)) {
        return Fail(L"Truncated SPS final coding flags");
    }
    bool vuiPresent = false;
    if (!Flag(bits, vuiPresent)) return Fail(L"Truncated SPS VUI flag");
    if (vuiPresent) ParseVuiPrefix(bits, sps);
    if (sps.chromaFormatIdc != 1 || sps.bitDepthLumaMinus8 != 2 ||
        sps.bitDepthChromaMinus8 != 2 || sps.separateColourPlaneFlag) {
        return Fail(L"The stream is not HEVC Main 10 4:2:0");
    }
    sps_[sps.id] = std::move(sps);
    hasSps_[sps.id] = true;
    return true;
}

bool HevcBitstreamParser::ParsePps(const std::uint8_t* data, std::size_t size) {
    const auto rbsp = RemoveEmulationPreventionBytes(data, size);
    BitReader bits(rbsp);
    PictureParameterSet pps;
    if (!Ue(bits, pps.id, 63) || !Ue(bits, pps.spsId, 15) ||
        !hasSps_[pps.spsId] ||
        !Flag(bits, pps.dependentSliceSegmentsEnabled) ||
        !Flag(bits, pps.outputFlagPresent)) {
        return Fail(L"Invalid PPS identifiers or flags");
    }
    std::uint32_t threeBits = 0;
    if (!bits.ReadBits(3, threeBits)) return Fail(L"Truncated PPS header bits");
    pps.numExtraSliceHeaderBits = threeBits;
    if (!Flag(bits, pps.signDataHidingEnabled) ||
        !Flag(bits, pps.cabacInitPresent) ||
        !Ue(bits, pps.numRefIdxL0DefaultActiveMinus1, 14) ||
        !Ue(bits, pps.numRefIdxL1DefaultActiveMinus1, 14) ||
        !Se(bits, pps.initQpMinus26, -26, 25) ||
        !Flag(bits, pps.constrainedIntraPred) ||
        !Flag(bits, pps.transformSkipEnabled) ||
        !Flag(bits, pps.cuQpDeltaEnabled)) {
        return Fail(L"Invalid PPS prediction parameters");
    }
    if (pps.cuQpDeltaEnabled && !Ue(bits, pps.diffCuQpDeltaDepth, 4)) {
        return Fail(L"Invalid PPS CU QP delta depth");
    }
    if (!Se(bits, pps.cbQpOffset, -12, 12) ||
        !Se(bits, pps.crQpOffset, -12, 12) ||
        !Flag(bits, pps.sliceChromaQpOffsetsPresent) ||
        !Flag(bits, pps.weightedPred) || !Flag(bits, pps.weightedBiPred) ||
        !Flag(bits, pps.transquantBypassEnabled) ||
        !Flag(bits, pps.tilesEnabled) ||
        !Flag(bits, pps.entropyCodingSyncEnabled)) {
        return Fail(L"Invalid PPS coding flags");
    }
    if (pps.tilesEnabled) {
        if (!Ue(bits, pps.numTileColumnsMinus1, 19) ||
            !Ue(bits, pps.numTileRowsMinus1, 21) ||
            !Flag(bits, pps.uniformSpacing)) {
            return Fail(L"Invalid PPS tile grid");
        }
        if (!pps.uniformSpacing) {
            for (unsigned i = 0; i < pps.numTileColumnsMinus1; ++i) {
                if (!Ue(bits, pps.columnWidthMinus1[i], 65535)) {
                    return Fail(L"Invalid PPS tile column width");
                }
            }
            for (unsigned i = 0; i < pps.numTileRowsMinus1; ++i) {
                if (!Ue(bits, pps.rowHeightMinus1[i], 65535)) {
                    return Fail(L"Invalid PPS tile row height");
                }
            }
        }
        if (!Flag(bits, pps.loopFilterAcrossTilesEnabled)) {
            return Fail(L"Truncated PPS tile loop filter flag");
        }
    }
    if (!Flag(bits, pps.loopFilterAcrossSlicesEnabled)) {
        return Fail(L"Truncated PPS slice loop filter flag");
    }
    bool deblockingControlPresent = false;
    if (!Flag(bits, deblockingControlPresent)) {
        return Fail(L"Truncated PPS deblocking control flag");
    }
    if (deblockingControlPresent) {
        if (!Flag(bits, pps.deblockingFilterOverrideEnabled) ||
            !Flag(bits, pps.deblockingFilterDisabled)) {
            return Fail(L"Invalid PPS deblocking flags");
        }
        if (!pps.deblockingFilterDisabled &&
            (!Se(bits, pps.betaOffsetDiv2, -6, 6) ||
             !Se(bits, pps.tcOffsetDiv2, -6, 6))) {
            return Fail(L"Invalid PPS deblocking offsets");
        }
    }
    if (!Flag(bits, pps.hasScalingLists)) {
        return Fail(L"Truncated PPS scaling list flag");
    }
    pps.scalingLists = sps_[pps.spsId].scalingLists;
    if (pps.hasScalingLists && !ParseScalingList(bits, pps.scalingLists)) {
        return Fail(L"Invalid PPS scaling list");
    }
    if (!Flag(bits, pps.listsModificationPresent) ||
        !Ue(bits, pps.log2ParallelMergeLevelMinus2, 4) ||
        !Flag(bits, pps.sliceSegmentHeaderExtensionPresent)) {
        return Fail(L"Invalid PPS final syntax");
    }
    bool extensionPresent = false;
    if (!Flag(bits, extensionPresent)) return Fail(L"Truncated PPS extension flag");
    if (extensionPresent) {
        return Fail(L"HEVC PPS extensions are not supported by the Main 10 path");
    }
    pps_[pps.id] = std::move(pps);
    hasPps_[pps.id] = true;
    return true;
}

bool HevcBitstreamParser::ParseFirstSlice(const std::uint8_t* data,
                                          std::size_t size, int nalUnitType,
                                          int temporalId,
                                          AccessUnit& accessUnit) {
    const auto rbsp = RemoveEmulationPreventionBytes(data, size);
    BitReader bits(rbsp);
    bool firstSlice = false;
    if (!Flag(bits, firstSlice) || !firstSlice) {
        return Fail(L"An HEVC access unit does not begin with its first slice");
    }
    const bool irap = IsIrap(nalUnitType);
    bool ignored = false;
    if (irap && !Flag(bits, ignored)) return Fail(L"Truncated IRAP slice header");
    unsigned ppsId = 0;
    if (!Ue(bits, ppsId, 63) || !hasPps_[ppsId]) {
        return Fail(L"The slice refers to an unknown PPS");
    }
    const PictureParameterSet& pps = pps_[ppsId];
    const SequenceParameterSet& sps = sps_[pps.spsId];
    for (unsigned i = 0; i < pps.numExtraSliceHeaderBits; ++i) {
        if (!Flag(bits, ignored)) return Fail(L"Truncated extra slice header bits");
    }
    unsigned sliceType = 0;
    if (!Ue(bits, sliceType, 2)) return Fail(L"Invalid HEVC slice type");
    if (pps.outputFlagPresent && !Flag(bits, ignored)) {
        return Fail(L"Truncated slice output flag");
    }
    if (sps.separateColourPlaneFlag && !bits.SkipBits(2)) {
        return Fail(L"Truncated slice colour plane id");
    }

    ShortTermReferenceSet rps;
    unsigned rpsBits = 0;
    unsigned referenceDeltaPocs = 0;
    int poc = 0;
    if (!IsIdr(nalUnitType)) {
        const unsigned pocBitCount = sps.log2MaxPicOrderCntLsbMinus4 + 4;
        std::uint32_t pocLsbValue = 0;
        if (!bits.ReadBits(pocBitCount, pocLsbValue)) {
            return Fail(L"Truncated slice picture order count");
        }
        const int pocLsb = static_cast<int>(pocLsbValue);
        const int maximumPocLsb = 1 << pocBitCount;
        int pocMsb = 0;
        if (IsBla(nalUnitType) || !havePreviousPoc_) {
            pocMsb = 0;
        } else if (pocLsb < previousPocLsb_ &&
                   previousPocLsb_ - pocLsb >= maximumPocLsb / 2) {
            pocMsb = previousPocMsb_ + maximumPocLsb;
        } else if (pocLsb > previousPocLsb_ &&
                   pocLsb - previousPocLsb_ > maximumPocLsb / 2) {
            pocMsb = previousPocMsb_ - maximumPocLsb;
        } else {
            pocMsb = previousPocMsb_;
        }
        poc = pocMsb + pocLsb;
        bool useSpsRps = false;
        if (!Flag(bits, useSpsRps)) return Fail(L"Truncated slice RPS flag");
        if (!useSpsRps) {
            const std::size_t beginning = bits.BitPosition();
            if (!ParseShortTermReferenceSet(
                    bits, static_cast<unsigned>(sps.shortTermReferenceSets.size()),
                    static_cast<unsigned>(sps.shortTermReferenceSets.size()),
                    sps.shortTermReferenceSets, rps, &referenceDeltaPocs)) {
                return Fail(L"Invalid slice short-term reference picture set");
            }
            rpsBits = static_cast<unsigned>(bits.BitPosition() - beginning);
        } else if (!sps.shortTermReferenceSets.empty()) {
            unsigned index = 0;
            if (sps.shortTermReferenceSets.size() > 1) {
                std::uint32_t raw = 0;
                const unsigned indexBits = CeilLog2(
                    static_cast<unsigned>(sps.shortTermReferenceSets.size()));
                if (!bits.ReadBits(indexBits, raw) ||
                    raw >= sps.shortTermReferenceSets.size()) {
                    return Fail(L"Invalid slice SPS RPS index");
                }
                index = raw;
            }
            rps = sps.shortTermReferenceSets[index];
        }
        if (sps.longTermRefPicsPresent) {
            return Fail(L"Long-term HEVC references are not needed by this decoder profile");
        }
        if (temporalId == 0 && nalUnitType != 6 && nalUnitType != 7 &&
            nalUnitType != 8 && nalUnitType != 9) {
            previousPocLsb_ = pocLsb;
            previousPocMsb_ = pocMsb;
            havePreviousPoc_ = true;
        }
    } else {
        previousPocLsb_ = 0;
        previousPocMsb_ = 0;
        havePreviousPoc_ = true;
    }

    accessUnit.sps = &sps;
    accessUnit.pps = &pps;
    accessUnit.shortTermReferences = rps;
    accessUnit.poc = poc;
    accessUnit.nalUnitType = nalUnitType;
    accessUnit.temporalId = temporalId;
    accessUnit.shortTermRpsBitsInSlice = rpsBits;
    accessUnit.deltaPocsOfReferenceRps = referenceDeltaPocs;
    accessUnit.irap = irap;
    accessUnit.idr = IsIdr(nalUnitType);
    accessUnit.intra = sliceType == 2;
    accessUnit.reference = IsReferenceNal(nalUnitType);
    for (const ShortTermReference& ref : rps.negative) {
        const int referencePoc = poc + ref.deltaPoc;
        accessUnit.retainedReferencePocs.push_back(referencePoc);
        if (ref.used) accessUnit.beforeReferencePocs.push_back(referencePoc);
    }
    for (const ShortTermReference& ref : rps.positive) {
        const int referencePoc = poc + ref.deltaPoc;
        accessUnit.retainedReferencePocs.push_back(referencePoc);
        if (ref.used) accessUnit.afterReferencePocs.push_back(referencePoc);
    }
    return true;
}

bool HevcBitstreamParser::ParseAccessUnit(const EncodedSample& sample,
                                          AccessUnit& accessUnit) {
    accessUnit = {};
    if (nalLengthSize_ == 0) return Fail(L"The HEVC parser is not initialized");
    std::size_t position = 0;
    while (position < sample.bytes.size()) {
        if (nalLengthSize_ > sample.bytes.size() - position) {
            return Fail(L"Truncated length-prefixed HEVC NAL unit");
        }
        std::size_t length = 0;
        for (unsigned i = 0; i < nalLengthSize_; ++i) {
            length = (length << 8U) | sample.bytes[position++];
        }
        if (length < 2 || length > sample.bytes.size() - position) {
            return Fail(L"Invalid length-prefixed HEVC NAL unit");
        }
        const std::uint8_t* nal = sample.bytes.data() + position;
        const int nalType = (nal[0] >> 1U) & 0x3fU;
        if (!ParseNalUnit(nal, length, nalType, &accessUnit)) return false;
        if (nalType <= 31) {
            if (accessUnit.bitstream.size() >
                std::numeric_limits<std::uint32_t>::max() - length - 3) {
                return Fail(L"HEVC access unit is too large");
            }
            const std::uint32_t offset =
                static_cast<std::uint32_t>(accessUnit.bitstream.size());
            accessUnit.bitstream.push_back(0);
            accessUnit.bitstream.push_back(0);
            accessUnit.bitstream.push_back(1);
            accessUnit.bitstream.insert(accessUnit.bitstream.end(), nal, nal + length);
            accessUnit.slices.push_back(
                {offset, static_cast<std::uint32_t>(length + 3)});
        }
        position += length;
    }
    if (!accessUnit.sps || !accessUnit.pps || accessUnit.slices.empty()) {
        return Fail(L"The HEVC sample contains no decodable picture");
    }
    error_.clear();
    return true;
}

}  // namespace movieplayer::codec::hevc
