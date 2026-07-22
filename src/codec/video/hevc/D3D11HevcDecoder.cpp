#include "codec/video/hevc/D3D11HevcDecoder.h"

#include <dxva.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <utility>

namespace movieplayer::codec::hevc {
namespace {

using Microsoft::WRL::ComPtr;

constexpr GUID kDxvaNoEncrypt = {
    0x1b81bed0, 0xa0c7, 0x11d3, {0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5}};

std::wstring HresultText(const wchar_t* operation, HRESULT result) {
    std::wostringstream out;
    out << operation << L" failed (HRESULT 0x" << std::hex << std::setw(8)
        << std::setfill(L'0') << static_cast<unsigned long>(result) << L")";
    return out.str();
}

bool SameGuid(REFGUID a, REFGUID b) {
    return InlineIsEqualGUID(a, b) != 0;
}

template <typename T>
bool CopyDecoderBuffer(ID3D11VideoContext* context, ID3D11VideoDecoder* decoder,
                       D3D11_VIDEO_DECODER_BUFFER_TYPE type, const T& value,
                       std::wstring& error) {
    UINT capacity = 0;
    void* destination = nullptr;
    HRESULT hr = context->GetDecoderBuffer(decoder, type, &capacity, &destination);
    if (FAILED(hr)) {
        error = HresultText(L"ID3D11VideoContext::GetDecoderBuffer", hr);
        return false;
    }
    if (!destination || capacity < sizeof(T)) {
        context->ReleaseDecoderBuffer(decoder, type);
        error = L"The hardware decoder returned an undersized parameter buffer";
        return false;
    }
    std::memcpy(destination, &value, sizeof(T));
    hr = context->ReleaseDecoderBuffer(decoder, type);
    if (FAILED(hr)) {
        error = HresultText(L"ID3D11VideoContext::ReleaseDecoderBuffer", hr);
        return false;
    }
    return true;
}

}  // namespace

struct D3D11HevcDecoder::Impl {
    struct Surface {
        ComPtr<ID3D11VideoDecoderOutputView> outputView;
        std::weak_ptr<VideoFrame> displayedFrame;
        int poc = 0;
        bool reference = false;
    };

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> immediateContext;
    ComPtr<ID3D11VideoDevice> videoDevice;
    ComPtr<ID3D11VideoContext> videoContext;
    ComPtr<ID3D11VideoDecoder> decoder;
    ComPtr<ID3D11Texture2D> surfacesTexture;
    std::vector<Surface> surfaces;
    D3D11_VIDEO_DECODER_DESC decoderDescription{};
    D3D11_VIDEO_DECODER_CONFIG decoderConfiguration{};
    TrackInfo track;
    HevcBitstreamParser parser;
    std::wstring description;
    std::wstring error;
    unsigned statusFeedback = 0;
    bool waitingForRandomAccessPoint = true;
    bool discardRaslPictures = false;

    bool Fail(const std::wstring& message) {
        error = message;
        return false;
    }

    void ReleaseDecoderResources() {
        surfaces.clear();
        surfacesTexture.Reset();
        decoder.Reset();
        decoderDescription = {};
        decoderConfiguration = {};
        statusFeedback = 0;
    }

    bool SelectConfiguration(const SequenceParameterSet& sps) {
        UINT profileCount = videoDevice->GetVideoDecoderProfileCount();
        bool profileFound = false;
        for (UINT i = 0; i < profileCount; ++i) {
            GUID profile = {};
            if (SUCCEEDED(videoDevice->GetVideoDecoderProfile(i, &profile)) &&
                SameGuid(profile, D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10)) {
                profileFound = true;
                break;
            }
        }
        if (!profileFound) {
            return Fail(L"The GPU does not expose the D3D11 HEVC Main 10 decoder profile");
        }
        BOOL formatSupported = FALSE;
        HRESULT hr = videoDevice->CheckVideoDecoderFormat(
            &D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10, DXGI_FORMAT_P010,
            &formatSupported);
        if (FAILED(hr) || !formatSupported) {
            return Fail(FAILED(hr)
                            ? HresultText(L"CheckVideoDecoderFormat(P010)", hr)
                            : L"The GPU HEVC Main 10 decoder does not output P010");
        }
        decoderDescription.Guid = D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10;
        decoderDescription.SampleWidth = sps.width;
        decoderDescription.SampleHeight = sps.height;
        decoderDescription.OutputFormat = DXGI_FORMAT_P010;

        UINT configurationCount = 0;
        hr = videoDevice->GetVideoDecoderConfigCount(&decoderDescription,
                                                      &configurationCount);
        if (FAILED(hr) || configurationCount == 0) {
            return Fail(FAILED(hr)
                            ? HresultText(L"GetVideoDecoderConfigCount", hr)
                            : L"The GPU returned no HEVC Main 10 decoder configuration");
        }
        bool found = false;
        std::wostringstream available;
        for (UINT i = 0; i < configurationCount; ++i) {
            D3D11_VIDEO_DECODER_CONFIG candidate = {};
            hr = videoDevice->GetVideoDecoderConfig(&decoderDescription, i,
                                                     &candidate);
            if (SUCCEEDED(hr)) {
                available << L" [raw=" << candidate.ConfigBitstreamRaw
                          << L", encrypted="
                          << (!SameGuid(candidate.guidConfigBitstreamEncryption,
                                       kDxvaNoEncrypt) ? L"yes" : L"no") << L"]";
            }
            if (FAILED(hr) || candidate.ConfigBitstreamRaw != 1 ||
                !SameGuid(candidate.guidConfigBitstreamEncryption, kDxvaNoEncrypt)) {
                continue;
            }
            decoderConfiguration = candidate;
            found = true;
            break;
        }
        if (!found) {
            return Fail(L"The GPU has no usable unencrypted HEVC VLD bitstream configuration:" +
                        available.str());
        }
        return true;
    }

    bool CreateDecoderResources(const SequenceParameterSet& sps) {
        ReleaseDecoderResources();
        if (!SelectConfiguration(sps)) return false;
        HRESULT hr = videoDevice->CreateVideoDecoder(
            &decoderDescription, &decoderConfiguration, &decoder);
        if (FAILED(hr)) return Fail(HresultText(L"CreateVideoDecoder(HEVC Main10)", hr));

        // The decode thread may be one frame ahead of the bounded presentation
        // queue. Keep those display-owned surfaces in addition to the codec DPB.
        const UINT surfaceCount = static_cast<UINT>(std::max(
            24U, std::min(32U, sps.maxDecPicBufferingMinus1 + 18U)));
        D3D11_TEXTURE2D_DESC textureDescription = {};
        textureDescription.Width = sps.width;
        textureDescription.Height = sps.height;
        textureDescription.MipLevels = 1;
        textureDescription.ArraySize = surfaceCount;
        textureDescription.Format = DXGI_FORMAT_P010;
        textureDescription.SampleDesc.Count = 1;
        textureDescription.Usage = D3D11_USAGE_DEFAULT;
        textureDescription.BindFlags = D3D11_BIND_DECODER;
        hr = device->CreateTexture2D(&textureDescription, nullptr, &surfacesTexture);
        if (FAILED(hr)) return Fail(HresultText(L"CreateTexture2D(P010 decoder surfaces)", hr));

        surfaces.resize(surfaceCount);
        for (UINT i = 0; i < surfaceCount; ++i) {
            D3D11_VIDEO_DECODER_OUTPUT_VIEW_DESC viewDescription = {};
            viewDescription.DecodeProfile = decoderDescription.Guid;
            viewDescription.ViewDimension = D3D11_VDOV_DIMENSION_TEXTURE2D;
            viewDescription.Texture2D.ArraySlice = i;
            hr = videoDevice->CreateVideoDecoderOutputView(
                surfacesTexture.Get(), &viewDescription, &surfaces[i].outputView);
            if (FAILED(hr)) {
                return Fail(HresultText(L"CreateVideoDecoderOutputView", hr));
            }
        }
        return true;
    }

    int FindReferenceSurface(int poc) const {
        for (std::size_t i = 0; i < surfaces.size(); ++i) {
            if (surfaces[i].reference && surfaces[i].poc == poc) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    int AcquireSurface() const {
        for (std::size_t i = 0; i < surfaces.size(); ++i) {
            if (!surfaces[i].reference && surfaces[i].displayedFrame.expired()) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    void MarkReferenceSet(const AccessUnit& unit) {
        if (unit.idr) {
            for (Surface& surface : surfaces) surface.reference = false;
            return;
        }
        const std::unordered_set<int> retained(unit.retainedReferencePocs.begin(),
                                               unit.retainedReferencePocs.end());
        for (Surface& surface : surfaces) {
            if (surface.reference && retained.find(surface.poc) == retained.end()) {
                surface.reference = false;
            }
        }
    }

    bool FillPictureParameters(const AccessUnit& unit, int currentSurface,
                               DXVA_PicParams_HEVC& picture) {
        const SequenceParameterSet& sps = *unit.sps;
        const PictureParameterSet& pps = *unit.pps;
        picture = {};
        const unsigned log2MinCbSize =
            sps.log2MinLumaCodingBlockSizeMinus3 + 3U;
        const unsigned minCbSize = 1U << log2MinCbSize;
        picture.PicWidthInMinCbsY = static_cast<USHORT>(
            (sps.width + minCbSize - 1U) / minCbSize);
        picture.PicHeightInMinCbsY = static_cast<USHORT>(
            (sps.height + minCbSize - 1U) / minCbSize);
        picture.chroma_format_idc = static_cast<USHORT>(sps.chromaFormatIdc);
        picture.separate_colour_plane_flag = sps.separateColourPlaneFlag;
        picture.bit_depth_luma_minus8 = static_cast<USHORT>(sps.bitDepthLumaMinus8);
        picture.bit_depth_chroma_minus8 = static_cast<USHORT>(sps.bitDepthChromaMinus8);
        picture.log2_max_pic_order_cnt_lsb_minus4 =
            static_cast<USHORT>(sps.log2MaxPicOrderCntLsbMinus4);
        picture.CurrPic.bPicEntry = static_cast<UCHAR>(currentSurface & 0x7f);
        picture.sps_max_dec_pic_buffering_minus1 =
            static_cast<UCHAR>(sps.maxDecPicBufferingMinus1);
        picture.log2_min_luma_coding_block_size_minus3 =
            static_cast<UCHAR>(sps.log2MinLumaCodingBlockSizeMinus3);
        picture.log2_diff_max_min_luma_coding_block_size =
            static_cast<UCHAR>(sps.log2DiffMaxMinLumaCodingBlockSize);
        picture.log2_min_transform_block_size_minus2 =
            static_cast<UCHAR>(sps.log2MinTransformBlockSizeMinus2);
        picture.log2_diff_max_min_transform_block_size =
            static_cast<UCHAR>(sps.log2DiffMaxMinTransformBlockSize);
        picture.max_transform_hierarchy_depth_inter =
            static_cast<UCHAR>(sps.maxTransformHierarchyDepthInter);
        picture.max_transform_hierarchy_depth_intra =
            static_cast<UCHAR>(sps.maxTransformHierarchyDepthIntra);
        picture.num_short_term_ref_pic_sets = static_cast<UCHAR>(
            sps.shortTermReferenceSets.size());
        picture.num_long_term_ref_pics_sps =
            static_cast<UCHAR>(sps.numLongTermRefPicsSps);
        picture.num_ref_idx_l0_default_active_minus1 =
            static_cast<UCHAR>(pps.numRefIdxL0DefaultActiveMinus1);
        picture.num_ref_idx_l1_default_active_minus1 =
            static_cast<UCHAR>(pps.numRefIdxL1DefaultActiveMinus1);
        picture.init_qp_minus26 = static_cast<CHAR>(pps.initQpMinus26);
        picture.ucNumDeltaPocsOfRefRpsIdx =
            static_cast<UCHAR>(unit.deltaPocsOfReferenceRps);
        picture.wNumBitsForShortTermRPSInSlice =
            static_cast<USHORT>(unit.shortTermRpsBitsInSlice);

        picture.scaling_list_enabled_flag = sps.scalingListEnabled;
        picture.amp_enabled_flag = sps.ampEnabled;
        picture.sample_adaptive_offset_enabled_flag = sps.sampleAdaptiveOffsetEnabled;
        picture.pcm_enabled_flag = sps.pcmEnabled;
        picture.pcm_sample_bit_depth_luma_minus1 =
            sps.pcmSampleBitDepthLumaMinus1;
        picture.pcm_sample_bit_depth_chroma_minus1 =
            sps.pcmSampleBitDepthChromaMinus1;
        picture.log2_min_pcm_luma_coding_block_size_minus3 =
            sps.log2MinPcmLumaCodingBlockSizeMinus3;
        picture.log2_diff_max_min_pcm_luma_coding_block_size =
            sps.log2DiffMaxMinPcmLumaCodingBlockSize;
        picture.pcm_loop_filter_disabled_flag = sps.pcmLoopFilterDisabled;
        picture.long_term_ref_pics_present_flag = sps.longTermRefPicsPresent;
        picture.sps_temporal_mvp_enabled_flag = sps.temporalMvpEnabled;
        picture.strong_intra_smoothing_enabled_flag = sps.strongIntraSmoothingEnabled;
        picture.dependent_slice_segments_enabled_flag = pps.dependentSliceSegmentsEnabled;
        picture.output_flag_present_flag = pps.outputFlagPresent;
        picture.num_extra_slice_header_bits = pps.numExtraSliceHeaderBits;
        picture.sign_data_hiding_enabled_flag = pps.signDataHidingEnabled;
        picture.cabac_init_present_flag = pps.cabacInitPresent;

        picture.constrained_intra_pred_flag = pps.constrainedIntraPred;
        picture.transform_skip_enabled_flag = pps.transformSkipEnabled;
        picture.cu_qp_delta_enabled_flag = pps.cuQpDeltaEnabled;
        picture.pps_slice_chroma_qp_offsets_present_flag =
            pps.sliceChromaQpOffsetsPresent;
        picture.weighted_pred_flag = pps.weightedPred;
        picture.weighted_bipred_flag = pps.weightedBiPred;
        picture.transquant_bypass_enabled_flag = pps.transquantBypassEnabled;
        picture.tiles_enabled_flag = pps.tilesEnabled;
        picture.entropy_coding_sync_enabled_flag = pps.entropyCodingSyncEnabled;
        picture.uniform_spacing_flag = pps.uniformSpacing;
        picture.loop_filter_across_tiles_enabled_flag =
            pps.loopFilterAcrossTilesEnabled;
        picture.pps_loop_filter_across_slices_enabled_flag =
            pps.loopFilterAcrossSlicesEnabled;
        picture.deblocking_filter_override_enabled_flag =
            pps.deblockingFilterOverrideEnabled;
        picture.pps_deblocking_filter_disabled_flag = pps.deblockingFilterDisabled;
        picture.lists_modification_present_flag = pps.listsModificationPresent;
        picture.slice_segment_header_extension_present_flag =
            pps.sliceSegmentHeaderExtensionPresent;
        picture.IrapPicFlag = unit.irap;
        picture.IdrPicFlag = unit.idr;
        picture.IntraPicFlag = unit.intra;
        picture.pps_cb_qp_offset = static_cast<CHAR>(pps.cbQpOffset);
        picture.pps_cr_qp_offset = static_cast<CHAR>(pps.crQpOffset);
        picture.num_tile_columns_minus1 =
            static_cast<UCHAR>(pps.numTileColumnsMinus1);
        picture.num_tile_rows_minus1 = static_cast<UCHAR>(pps.numTileRowsMinus1);
        picture.diff_cu_qp_delta_depth = static_cast<UCHAR>(pps.diffCuQpDeltaDepth);
        picture.pps_beta_offset_div2 = static_cast<CHAR>(pps.betaOffsetDiv2);
        picture.pps_tc_offset_div2 = static_cast<CHAR>(pps.tcOffsetDiv2);
        picture.log2_parallel_merge_level_minus2 =
            static_cast<UCHAR>(pps.log2ParallelMergeLevelMinus2);

        const unsigned ctbLog2 = log2MinCbSize +
                                 sps.log2DiffMaxMinLumaCodingBlockSize;
        const unsigned ctbSize = 1U << ctbLog2;
        const unsigned pictureWidthCtbs = (sps.width + ctbSize - 1U) / ctbSize;
        const unsigned pictureHeightCtbs = (sps.height + ctbSize - 1U) / ctbSize;
        if (pps.tilesEnabled && pps.uniformSpacing) {
            const unsigned columns = pps.numTileColumnsMinus1 + 1U;
            const unsigned rows = pps.numTileRowsMinus1 + 1U;
            for (unsigned i = 0; i + 1 < columns; ++i) {
                const unsigned end = (i + 1U) * pictureWidthCtbs / columns;
                const unsigned begin = i * pictureWidthCtbs / columns;
                picture.column_width_minus1[i] =
                    static_cast<USHORT>(end - begin - 1U);
            }
            for (unsigned i = 0; i + 1 < rows; ++i) {
                const unsigned end = (i + 1U) * pictureHeightCtbs / rows;
                const unsigned begin = i * pictureHeightCtbs / rows;
                picture.row_height_minus1[i] =
                    static_cast<USHORT>(end - begin - 1U);
            }
        } else if (pps.tilesEnabled) {
            for (unsigned i = 0; i < pps.numTileColumnsMinus1; ++i)
                picture.column_width_minus1[i] =
                    static_cast<USHORT>(pps.columnWidthMinus1[i]);
            for (unsigned i = 0; i < pps.numTileRowsMinus1; ++i)
                picture.row_height_minus1[i] =
                    static_cast<USHORT>(pps.rowHeightMinus1[i]);
        }
        picture.CurrPicOrderCntVal = unit.poc;
        std::memset(picture.RefPicList, 0xff, sizeof(picture.RefPicList));
        std::memset(picture.RefPicSetStCurrBefore, 0xff,
                    sizeof(picture.RefPicSetStCurrBefore));
        std::memset(picture.RefPicSetStCurrAfter, 0xff,
                    sizeof(picture.RefPicSetStCurrAfter));
        std::memset(picture.RefPicSetLtCurr, 0xff,
                    sizeof(picture.RefPicSetLtCurr));
        unsigned referenceIndex = 0;
        std::array<int, 15> referencePocs{};
        referencePocs.fill(std::numeric_limits<int>::min());
        for (std::size_t i = 0; i < surfaces.size() && referenceIndex < 15; ++i) {
            if (!surfaces[i].reference) continue;
            picture.RefPicList[referenceIndex].bPicEntry =
                static_cast<UCHAR>(i & 0x7fU);
            picture.PicOrderCntValList[referenceIndex] = surfaces[i].poc;
            referencePocs[referenceIndex] = surfaces[i].poc;
            ++referenceIndex;
        }
        int missingReferencePoc = std::numeric_limits<int>::min();
        auto mapSet = [&](const std::vector<int>& pocs, UCHAR* destination,
                          std::size_t capacity) {
            if (pocs.size() > capacity) return false;
            for (std::size_t i = 0; i < pocs.size(); ++i) {
                const auto match = std::find(referencePocs.begin(),
                                             referencePocs.end(), pocs[i]);
                if (match == referencePocs.end()) {
                    missingReferencePoc = pocs[i];
                    return false;
                }
                destination[i] = static_cast<UCHAR>(match - referencePocs.begin());
            }
            return true;
        };
        if (!mapSet(unit.beforeReferencePocs, picture.RefPicSetStCurrBefore, 8) ||
            !mapSet(unit.afterReferencePocs, picture.RefPicSetStCurrAfter, 8)) {
            std::wostringstream message;
            message << L"A required HEVC reference picture is not in the DXVA "
                       L"surface pool (NAL "
                    << unit.nalUnitType << L", POC " << unit.poc;
            if (missingReferencePoc != std::numeric_limits<int>::min()) {
                message << L", missing POC " << missingReferencePoc;
            }
            message << L")";
            return Fail(message.str());
        }
        picture.StatusReportFeedbackNumber = ++statusFeedback;
        return true;
    }

    static void FillQuantizationMatrix(const AccessUnit& unit,
                                       DXVA_Qmatrix_HEVC& matrix) {
        matrix = {};
        const ScalingLists& lists = unit.pps->hasScalingLists
                                        ? unit.pps->scalingLists
                                        : unit.sps->scalingLists;
        std::memcpy(matrix.ucScalingLists0, lists.size0.data(),
                    sizeof(matrix.ucScalingLists0));
        std::memcpy(matrix.ucScalingLists1, lists.size1.data(),
                    sizeof(matrix.ucScalingLists1));
        std::memcpy(matrix.ucScalingLists2, lists.size2.data(),
                    sizeof(matrix.ucScalingLists2));
        std::memcpy(matrix.ucScalingLists3, lists.size3.data(),
                    sizeof(matrix.ucScalingLists3));
        std::memcpy(matrix.ucScalingListDCCoefSizeID2, lists.dcSize2.data(),
                    sizeof(matrix.ucScalingListDCCoefSizeID2));
        std::memcpy(matrix.ucScalingListDCCoefSizeID3, lists.dcSize3.data(),
                    sizeof(matrix.ucScalingListDCCoefSizeID3));
    }

    bool SubmitPicture(const AccessUnit& unit, int surfaceIndex,
                       const DXVA_PicParams_HEVC& picture,
                       const DXVA_Qmatrix_HEVC& matrix) {
        // DecoderBeginFrame is allowed to report E_PENDING (or the equivalent
        // DXGI busy result) while the hardware is still retiring work from a
        // decoder that was just reset during a seek.  This is a transient
        // condition, not a corrupt frame.  In particular it is easy to hit
        // when a 4K Main 10 stream is seeked while its last displayed surface
        // is still owned by the renderer.
        constexpr auto kBeginFrameRetryWindow = std::chrono::milliseconds(500);
        const auto retryDeadline =
            std::chrono::steady_clock::now() + kBeginFrameRetryWindow;
        HRESULT hr = S_OK;
        do {
            hr = videoContext->DecoderBeginFrame(
                decoder.Get(), surfaces[surfaceIndex].outputView.Get(), 0, nullptr);
            if (hr != E_PENDING && hr != DXGI_ERROR_WAS_STILL_DRAWING) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < retryDeadline);
        if (FAILED(hr)) return Fail(HresultText(L"DecoderBeginFrame", hr));
        bool frameBegun = true;
        auto endFrame = [&]() {
            if (frameBegun) {
                const HRESULT endResult = videoContext->DecoderEndFrame(decoder.Get());
                frameBegun = false;
                return endResult;
            }
            return S_OK;
        };

        if (!CopyDecoderBuffer(videoContext.Get(), decoder.Get(),
                               D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS,
                               picture, error) ||
            !CopyDecoderBuffer(videoContext.Get(), decoder.Get(),
                               D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX,
                               matrix, error)) {
            endFrame();
            return false;
        }

        UINT bitstreamCapacity = 0;
        void* bitstreamDestination = nullptr;
        hr = videoContext->GetDecoderBuffer(
            decoder.Get(), D3D11_VIDEO_DECODER_BUFFER_BITSTREAM,
            &bitstreamCapacity, &bitstreamDestination);
        if (FAILED(hr)) {
            endFrame();
            return Fail(HresultText(L"GetDecoderBuffer(BITSTREAM)", hr));
        }
        const UINT alignedBitstreamSize = static_cast<UINT>(
            (unit.bitstream.size() + 127U) & ~std::size_t{127U});
        if (!bitstreamDestination || alignedBitstreamSize > bitstreamCapacity) {
            videoContext->ReleaseDecoderBuffer(
                decoder.Get(), D3D11_VIDEO_DECODER_BUFFER_BITSTREAM);
            endFrame();
            return Fail(L"The HEVC access unit exceeds the hardware bitstream buffer");
        }
        std::memcpy(bitstreamDestination, unit.bitstream.data(), unit.bitstream.size());
        std::memset(static_cast<std::uint8_t*>(bitstreamDestination) +
                        unit.bitstream.size(),
                    0, alignedBitstreamSize - unit.bitstream.size());
        hr = videoContext->ReleaseDecoderBuffer(
            decoder.Get(), D3D11_VIDEO_DECODER_BUFFER_BITSTREAM);
        if (FAILED(hr)) {
            endFrame();
            return Fail(HresultText(L"ReleaseDecoderBuffer(BITSTREAM)", hr));
        }

        std::vector<DXVA_Slice_HEVC_Short> slices(unit.slices.size());
        for (std::size_t i = 0; i < unit.slices.size(); ++i) {
            slices[i].BSNALunitDataLocation = unit.slices[i].offset;
            slices[i].SliceBytesInBuffer = unit.slices[i].size;
            slices[i].wBadSliceChopping = 0;
        }
        if (!slices.empty()) {
            slices.back().SliceBytesInBuffer +=
                alignedBitstreamSize - static_cast<UINT>(unit.bitstream.size());
        }
        UINT sliceCapacity = 0;
        void* sliceDestination = nullptr;
        hr = videoContext->GetDecoderBuffer(
            decoder.Get(), D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL,
            &sliceCapacity, &sliceDestination);
        const UINT sliceBytes = static_cast<UINT>(
            slices.size() * sizeof(DXVA_Slice_HEVC_Short));
        if (FAILED(hr) || !sliceDestination || sliceBytes > sliceCapacity) {
            if (SUCCEEDED(hr)) {
                videoContext->ReleaseDecoderBuffer(
                    decoder.Get(), D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL);
            }
            endFrame();
            return Fail(FAILED(hr)
                            ? HresultText(L"GetDecoderBuffer(SLICE_CONTROL)", hr)
                            : L"The HEVC slice table exceeds the hardware buffer");
        }
        std::memcpy(sliceDestination, slices.data(), sliceBytes);
        hr = videoContext->ReleaseDecoderBuffer(
            decoder.Get(), D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL);
        if (FAILED(hr)) {
            endFrame();
            return Fail(HresultText(L"ReleaseDecoderBuffer(SLICE_CONTROL)", hr));
        }

        std::array<D3D11_VIDEO_DECODER_BUFFER_DESC, 4> descriptions = {};
        descriptions[0].BufferType =
            D3D11_VIDEO_DECODER_BUFFER_PICTURE_PARAMETERS;
        descriptions[0].DataSize = static_cast<UINT>(sizeof(picture));
        descriptions[1].BufferType =
            D3D11_VIDEO_DECODER_BUFFER_INVERSE_QUANTIZATION_MATRIX;
        descriptions[1].DataSize = static_cast<UINT>(sizeof(matrix));
        descriptions[2].BufferType = D3D11_VIDEO_DECODER_BUFFER_SLICE_CONTROL;
        descriptions[2].DataSize = sliceBytes;
        descriptions[3].BufferType = D3D11_VIDEO_DECODER_BUFFER_BITSTREAM;
        descriptions[3].DataSize = alignedBitstreamSize;
        hr = videoContext->SubmitDecoderBuffers(
            decoder.Get(), static_cast<UINT>(descriptions.size()),
            descriptions.data());
        const HRESULT endResult = endFrame();
        if (FAILED(hr)) return Fail(HresultText(L"SubmitDecoderBuffers(HEVC)", hr));
        if (FAILED(endResult)) return Fail(HresultText(L"DecoderEndFrame", endResult));
        return true;
    }

    bool Initialize(ID3D11Device* sharedDevice, const TrackInfo& sourceTrack) {
        ReleaseDecoderResources();
        device.Reset();
        immediateContext.Reset();
        videoDevice.Reset();
        videoContext.Reset();
        error.clear();
        description.clear();
        waitingForRandomAccessPoint = true;
        discardRaslPictures = false;
        if (!sharedDevice || sourceTrack.codec != CodecId::Hevc) {
            return Fail(L"D3D11HevcDecoder received an invalid device or track");
        }
        device = sharedDevice;
        device->GetImmediateContext(&immediateContext);
        HRESULT hr = device.As(&videoDevice);
        if (FAILED(hr) || !videoDevice) {
            return Fail(L"The D3D11 device has no video decoding interface");
        }
        hr = immediateContext.As(&videoContext);
        if (FAILED(hr) || !videoContext) {
            return Fail(L"The D3D11 context has no video decoding interface");
        }
        track = sourceTrack;
        if (!parser.Initialize(track)) return Fail(parser.LastError());
        description = L"Native D3D11/DXVA HEVC Main 10 (P010)";
        return true;
    }

    bool Decode(const EncodedSample& sample,
                std::vector<std::shared_ptr<VideoFrame>>& output) {
        output.clear();
        AccessUnit unit;
        if (!parser.ParseAccessUnit(sample, unit)) return Fail(parser.LastError());

        // x265 normally emits open-GOP random-access points as CRA pictures.
        // RASL_N/R pictures can follow that CRA in decode order while having
        // an earlier presentation order and references that precede the CRA.
        // Those reference pictures deliberately do not exist after Reset(),
        // and HEVC random-access semantics allow the leading RASL pictures to
        // be discarded.  Do this only for the first IRAP after a reset; RASL
        // pictures encountered during ordinary sequential playback still use
        // the intact DPB and are decoded normally.
        if (waitingForRandomAccessPoint && unit.irap) {
            waitingForRandomAccessPoint = false;
            discardRaslPictures = !unit.idr;
        } else if (discardRaslPictures) {
            if (unit.nalUnitType == 8 || unit.nalUnitType == 9) {
                error.clear();
                return true;
            }
            discardRaslPictures = false;
        }
        if (!decoder && !CreateDecoderResources(*unit.sps)) return false;
        if (decoderDescription.SampleWidth != unit.sps->width ||
            decoderDescription.SampleHeight != unit.sps->height) {
            return Fail(L"Mid-stream HEVC resolution changes are not supported");
        }
        MarkReferenceSet(unit);
        const int surfaceIndex = AcquireSurface();
        if (surfaceIndex < 0) {
            return Fail(L"The HEVC decoder surface pool is exhausted");
        }
        DXVA_PicParams_HEVC picture = {};
        if (!FillPictureParameters(unit, surfaceIndex, picture)) return false;
        DXVA_Qmatrix_HEVC matrix = {};
        FillQuantizationMatrix(unit, matrix);
        if (!SubmitPicture(unit, surfaceIndex, picture, matrix)) return false;

        auto frame = std::make_shared<VideoFrame>();
        frame->texture = surfacesTexture;
        frame->arraySlice = static_cast<UINT>(surfaceIndex);
        frame->format = DXGI_FORMAT_P010;
        frame->width = track.width > 0 ? track.width : static_cast<int>(unit.sps->width);
        frame->height = track.height > 0 ? track.height : static_cast<int>(unit.sps->height);
        frame->sampleAspectRatio = unit.sps->sampleAspectRatio.IsValid()
                                       ? unit.sps->sampleAspectRatio
                                       : track.sampleAspectRatio;
        frame->color = unit.sps->color;
        if (frame->color.range == ColorRange::Unspecified)
            frame->color.range = ColorRange::Limited;
        if (frame->color.matrix == ColorMatrix::Unspecified)
            frame->color.matrix = frame->height > 576
                                      ? ColorMatrix::Bt709
                                      : ColorMatrix::Bt601;
        if (frame->color.primaries == ColorPrimaries::Unspecified)
            frame->color.primaries = ColorPrimaries::Bt709;
        if (frame->color.transfer == TransferCharacteristic::Unspecified)
            frame->color.transfer = TransferCharacteristic::Bt709;
        if (frame->color.chromaLocation == ChromaLocation::Unspecified)
            frame->color.chromaLocation = ChromaLocation::Left;
        frame->pts = sample.PtsSeconds();
        frame->duration = sample.DurationSeconds();
        surfaces[surfaceIndex].poc = unit.poc;
        surfaces[surfaceIndex].reference = unit.reference;
        surfaces[surfaceIndex].displayedFrame = frame;
        output.push_back(std::move(frame));
        error.clear();
        return true;
    }

    bool Reset() {
        ReleaseDecoderResources();
        parser.Reset();
        waitingForRandomAccessPoint = true;
        discardRaslPictures = false;
        error.clear();
        return true;
    }
};

D3D11HevcDecoder::D3D11HevcDecoder() : impl_(std::make_unique<Impl>()) {}
D3D11HevcDecoder::~D3D11HevcDecoder() = default;

bool D3D11HevcDecoder::Initialize(ID3D11Device* device, const TrackInfo& track) {
    return impl_->Initialize(device, track);
}
bool D3D11HevcDecoder::Decode(
    const EncodedSample& sample,
    std::vector<std::shared_ptr<VideoFrame>>& frames) {
    return impl_->Decode(sample, frames);
}
bool D3D11HevcDecoder::Flush(std::vector<std::shared_ptr<VideoFrame>>& frames) {
    frames.clear();
    return true;
}
bool D3D11HevcDecoder::Reset() { return impl_->Reset(); }
const std::wstring& D3D11HevcDecoder::Description() const noexcept {
    return impl_->description;
}
const std::wstring& D3D11HevcDecoder::LastError() const noexcept {
    return impl_->error;
}

}  // namespace movieplayer::codec::hevc
