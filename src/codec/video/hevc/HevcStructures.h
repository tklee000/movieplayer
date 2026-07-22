#pragma once

#include "codec/core/CodecTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace movieplayer::codec::hevc {

struct ScalingLists {
    std::array<std::array<std::uint8_t, 16>, 6> size0{};
    std::array<std::array<std::uint8_t, 64>, 6> size1{};
    std::array<std::array<std::uint8_t, 64>, 6> size2{};
    std::array<std::array<std::uint8_t, 64>, 2> size3{};
    std::array<std::uint8_t, 6> dcSize2{};
    std::array<std::uint8_t, 2> dcSize3{};

    ScalingLists() {
        for (auto& matrix : size0) matrix.fill(16);
        for (auto& matrix : size1) matrix.fill(16);
        for (auto& matrix : size2) matrix.fill(16);
        for (auto& matrix : size3) matrix.fill(16);
        dcSize2.fill(16);
        dcSize3.fill(16);
    }
};

struct ShortTermReference {
    int deltaPoc = 0;
    bool used = false;
};

struct ShortTermReferenceSet {
    std::vector<ShortTermReference> negative;
    std::vector<ShortTermReference> positive;

    std::size_t DeltaPocCount() const noexcept {
        return negative.size() + positive.size();
    }
};

struct SequenceParameterSet {
    unsigned id = 0;
    unsigned vpsId = 0;
    unsigned maxSubLayersMinus1 = 0;
    unsigned chromaFormatIdc = 1;
    bool separateColourPlaneFlag = false;
    unsigned width = 0;
    unsigned height = 0;
    unsigned bitDepthLumaMinus8 = 0;
    unsigned bitDepthChromaMinus8 = 0;
    unsigned log2MaxPicOrderCntLsbMinus4 = 0;
    unsigned maxDecPicBufferingMinus1 = 0;
    unsigned maxNumReorderPics = 0;
    unsigned log2MinLumaCodingBlockSizeMinus3 = 0;
    unsigned log2DiffMaxMinLumaCodingBlockSize = 0;
    unsigned log2MinTransformBlockSizeMinus2 = 0;
    unsigned log2DiffMaxMinTransformBlockSize = 0;
    unsigned maxTransformHierarchyDepthInter = 0;
    unsigned maxTransformHierarchyDepthIntra = 0;
    bool scalingListEnabled = false;
    bool ampEnabled = false;
    bool sampleAdaptiveOffsetEnabled = false;
    bool pcmEnabled = false;
    unsigned pcmSampleBitDepthLumaMinus1 = 0;
    unsigned pcmSampleBitDepthChromaMinus1 = 0;
    unsigned log2MinPcmLumaCodingBlockSizeMinus3 = 0;
    unsigned log2DiffMaxMinPcmLumaCodingBlockSize = 0;
    bool pcmLoopFilterDisabled = false;
    bool longTermRefPicsPresent = false;
    unsigned numLongTermRefPicsSps = 0;
    bool temporalMvpEnabled = false;
    bool strongIntraSmoothingEnabled = false;
    std::vector<ShortTermReferenceSet> shortTermReferenceSets;
    ScalingLists scalingLists;
    Rational sampleAspectRatio{1, 1};
    ColorDescription color;
};

struct PictureParameterSet {
    unsigned id = 0;
    unsigned spsId = 0;
    bool dependentSliceSegmentsEnabled = false;
    bool outputFlagPresent = false;
    unsigned numExtraSliceHeaderBits = 0;
    bool signDataHidingEnabled = false;
    bool cabacInitPresent = false;
    unsigned numRefIdxL0DefaultActiveMinus1 = 0;
    unsigned numRefIdxL1DefaultActiveMinus1 = 0;
    int initQpMinus26 = 0;
    bool constrainedIntraPred = false;
    bool transformSkipEnabled = false;
    bool cuQpDeltaEnabled = false;
    unsigned diffCuQpDeltaDepth = 0;
    int cbQpOffset = 0;
    int crQpOffset = 0;
    bool sliceChromaQpOffsetsPresent = false;
    bool weightedPred = false;
    bool weightedBiPred = false;
    bool transquantBypassEnabled = false;
    bool tilesEnabled = false;
    bool entropyCodingSyncEnabled = false;
    bool uniformSpacing = true;
    unsigned numTileColumnsMinus1 = 0;
    unsigned numTileRowsMinus1 = 0;
    std::array<unsigned, 20> columnWidthMinus1{};
    std::array<unsigned, 22> rowHeightMinus1{};
    bool loopFilterAcrossTilesEnabled = true;
    bool loopFilterAcrossSlicesEnabled = false;
    bool deblockingFilterOverrideEnabled = false;
    bool deblockingFilterDisabled = false;
    int betaOffsetDiv2 = 0;
    int tcOffsetDiv2 = 0;
    bool listsModificationPresent = false;
    unsigned log2ParallelMergeLevelMinus2 = 0;
    bool sliceSegmentHeaderExtensionPresent = false;
    bool hasScalingLists = false;
    ScalingLists scalingLists;
};

struct SliceLocation {
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
};

struct AccessUnit {
    std::vector<std::uint8_t> bitstream;
    std::vector<SliceLocation> slices;
    const SequenceParameterSet* sps = nullptr;
    const PictureParameterSet* pps = nullptr;
    ShortTermReferenceSet shortTermReferences;
    std::vector<int> retainedReferencePocs;
    std::vector<int> beforeReferencePocs;
    std::vector<int> afterReferencePocs;
    int poc = 0;
    int nalUnitType = -1;
    int temporalId = 0;
    unsigned shortTermRpsBitsInSlice = 0;
    unsigned deltaPocsOfReferenceRps = 0;
    bool irap = false;
    bool idr = false;
    bool intra = false;
    bool reference = false;
};

}  // namespace movieplayer::codec::hevc
