#include "RtxVideoVsr.h"

#include <d3d10.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <iomanip>
#include <sstream>

#define NGX_ENABLE_DEPRECATED_GET_PARAMETERS
#include <nvsdk_ngx_helpers_vsr.h>

using Microsoft::WRL::ComPtr;

namespace {

const wchar_t* NgxResultName(NVSDK_NGX_Result result)
{
    switch (result) {
    case NVSDK_NGX_Result_Success:
        return L"Success";
    case NVSDK_NGX_Result_FAIL_FeatureNotSupported:
        return L"FeatureNotSupported";
    case NVSDK_NGX_Result_FAIL_PlatformError:
        return L"PlatformError";
    case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists:
        return L"FeatureAlreadyExists";
    case NVSDK_NGX_Result_FAIL_FeatureNotFound:
        return L"FeatureNotFound";
    case NVSDK_NGX_Result_FAIL_InvalidParameter:
        return L"InvalidParameter";
    case NVSDK_NGX_Result_FAIL_ScratchBufferTooSmall:
        return L"ScratchBufferTooSmall";
    case NVSDK_NGX_Result_FAIL_NotInitialized:
        return L"NotInitialized";
    case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat:
        return L"UnsupportedInputFormat";
    case NVSDK_NGX_Result_FAIL_RWFlagMissing:
        return L"RWFlagMissing";
    case NVSDK_NGX_Result_FAIL_MissingInput:
        return L"MissingInput";
    case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature:
        return L"UnableToInitializeFeature";
    case NVSDK_NGX_Result_FAIL_OutOfDate:
        return L"OutOfDate";
    case NVSDK_NGX_Result_FAIL_OutOfGPUMemory:
        return L"OutOfGPUMemory";
    case NVSDK_NGX_Result_FAIL_UnsupportedFormat:
        return L"UnsupportedFormat";
    case NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath:
        return L"UnableToWriteToAppDataPath";
    case NVSDK_NGX_Result_FAIL_UnsupportedParameter:
        return L"UnsupportedParameter";
    case NVSDK_NGX_Result_FAIL_Denied:
        return L"Denied";
    case NVSDK_NGX_Result_FAIL_NotImplemented:
        return L"NotImplemented";
    default:
        return L"Unknown";
    }
}

std::wstring NgxResultText(const wchar_t* operation, NVSDK_NGX_Result result)
{
    std::wostringstream stream;
    stream << operation << L" failed: " << NgxResultName(result) << L" (0x"
           << std::uppercase << std::hex << std::setw(8) << std::setfill(L'0')
           << static_cast<unsigned int>(result) << L")";
    return stream.str();
}

std::wstring ApplicationDataPath()
{
    PWSTR localAppData = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE,
                                       nullptr, &localAppData)) &&
        localAppData && *localAppData) {
        path = localAppData;
        path += L"\\MoviePlayer\\NVIDIA";
    }
    CoTaskMemFree(localAppData);

    if (!path.empty()) {
        const int result = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
        if (result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS ||
            result == ERROR_FILE_EXISTS) {
            return path;
        }
    }

    // The current directory is the SDK sample's documented fallback and is
    // normally writable for local builds.
    return L".";
}

class MultiThreadScope final {
public:
    explicit MultiThreadScope(ID3D10Multithread* multithread)
        : multithread_(multithread)
    {
        if (multithread_) {
            multithread_->Enter();
        }
    }

    ~MultiThreadScope()
    {
        if (multithread_) {
            multithread_->Leave();
        }
    }

    MultiThreadScope(const MultiThreadScope&) = delete;
    MultiThreadScope& operator=(const MultiThreadScope&) = delete;

private:
    ID3D10Multithread* multithread_ = nullptr;
};

bool IsVsrFormat(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_R8G8B8A8_UNORM ||
           format == DXGI_FORMAT_B8G8R8A8_UNORM ||
           format == DXGI_FORMAT_R10G10B10A2_UNORM;
}

bool RectIsInside(const RECT& rect, UINT width, UINT height)
{
    return rect.left >= 0 && rect.top >= 0 &&
           rect.right > rect.left && rect.bottom > rect.top &&
           rect.right <= static_cast<LONG>(width) &&
           rect.bottom <= static_cast<LONG>(height);
}

} // namespace

struct RtxVideoVsr::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D10Multithread> multithread;
    NVSDK_NGX_Parameter* parameters = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    bool parametersOwned = false;
    bool ngxInitialized = false;
    bool available = false;
    std::wstring status = L"RTX Video Super Resolution has not been initialized.";
};

RtxVideoVsr::RtxVideoVsr()
    : impl_(std::make_unique<Impl>())
{
}

RtxVideoVsr::~RtxVideoVsr()
{
    Shutdown();
}

bool RtxVideoVsr::Initialize(ID3D11Device* device)
{
    Shutdown();
    if (!device) {
        impl_->status = L"RTX Video VSR received a null D3D11 device.";
        return false;
    }

    impl_->device = device;
    device->GetImmediateContext(&impl_->context);
    if (!impl_->context || FAILED(impl_->context.As(&impl_->multithread)) ||
        !impl_->multithread) {
        impl_->status = L"RTX Video VSR requires ID3D10Multithread support.";
        Shutdown();
        return false;
    }
    impl_->multithread->SetMultithreadProtected(TRUE);

    const std::wstring dataPath = ApplicationDataPath();
    NVSDK_NGX_Result result = NVSDK_NGX_D3D11_Init(
        0, dataPath.c_str(), impl_->device.Get());
    if (NVSDK_NGX_FAILED(result)) {
        impl_->status = NgxResultText(L"NVSDK_NGX_D3D11_Init", result);
        Shutdown();
        return false;
    }
    impl_->ngxInitialized = true;

    result = NVSDK_NGX_D3D11_GetCapabilityParameters(&impl_->parameters);
    if (result == NVSDK_NGX_Result_FAIL_OutOfDate) {
        result = NVSDK_NGX_D3D11_GetParameters(&impl_->parameters);
        impl_->parametersOwned = false;
    } else if (NVSDK_NGX_SUCCEED(result)) {
        impl_->parametersOwned = true;
    }
    if (NVSDK_NGX_FAILED(result) || !impl_->parameters) {
        impl_->status = NgxResultText(
            L"NVSDK_NGX_D3D11_GetCapabilityParameters", result);
        Shutdown();
        return false;
    }

    int vsrAvailable = 0;
    result = impl_->parameters->Get(NVSDK_NGX_Parameter_VSR_Available,
                                    &vsrAvailable);
    if (NVSDK_NGX_FAILED(result) || !vsrAvailable) {
        impl_->status = NVSDK_NGX_FAILED(result)
                            ? NgxResultText(L"Querying VSR availability", result)
                            : L"RTX Video VSR is not supported by this GPU, driver, or feature DLL.";
        Shutdown();
        return false;
    }

    NVSDK_NGX_Feature_Create_Params createParams = {};
    {
        MultiThreadScope lock(impl_->multithread.Get());
        result = NGX_D3D11_CREATE_VSR_EXT(
            impl_->context.Get(), &impl_->feature, impl_->parameters, &createParams);
    }
    if (NVSDK_NGX_FAILED(result) || !impl_->feature) {
        impl_->status = NgxResultText(L"Creating the RTX Video VSR feature", result);
        Shutdown();
        return false;
    }

    impl_->available = true;
    impl_->status = L"RTX Video Super Resolution is available.";
    return true;
}

void RtxVideoVsr::Shutdown()
{
    impl_->available = false;
    if (impl_->feature) {
        NVSDK_NGX_D3D11_ReleaseFeature(impl_->feature);
        impl_->feature = nullptr;
    }
    if (impl_->parameters && impl_->parametersOwned) {
        NVSDK_NGX_D3D11_DestroyParameters(impl_->parameters);
    }
    impl_->parameters = nullptr;
    impl_->parametersOwned = false;
    if (impl_->ngxInitialized) {
        NVSDK_NGX_D3D11_Shutdown1(impl_->device.Get());
        impl_->ngxInitialized = false;
    }
    impl_->multithread.Reset();
    impl_->context.Reset();
    impl_->device.Reset();
}

bool RtxVideoVsr::Evaluate(ID3D11Texture2D* output, const RECT& outputRect,
                           ID3D11Texture2D* input, const RECT& inputRect)
{
    if (!impl_->available || !impl_->feature || !impl_->parameters) {
        impl_->status = L"RTX Video VSR is not available.";
        return false;
    }
    if (!input || !output) {
        impl_->status = L"RTX Video VSR received a null input or output texture.";
        return false;
    }

    D3D11_TEXTURE2D_DESC inputDesc = {};
    D3D11_TEXTURE2D_DESC outputDesc = {};
    input->GetDesc(&inputDesc);
    output->GetDesc(&outputDesc);
    if (!IsVsrFormat(inputDesc.Format) || !IsVsrFormat(outputDesc.Format)) {
        impl_->status = L"RTX Video VSR requires an RGBA8, BGRA8, or RGB10A2 texture.";
        return false;
    }
    if (!(outputDesc.BindFlags & D3D11_BIND_UNORDERED_ACCESS)) {
        impl_->status = L"RTX Video VSR output texture is missing unordered-access support.";
        return false;
    }
    if (!RectIsInside(inputRect, inputDesc.Width, inputDesc.Height) ||
        !RectIsInside(outputRect, outputDesc.Width, outputDesc.Height)) {
        impl_->status = L"RTX Video VSR received an invalid input or output rectangle.";
        return false;
    }

    NVSDK_NGX_D3D11_VSR_Eval_Params params = {};
    params.pInput = input;
    params.pOutput = output;
    params.InputSubrectBase.X = static_cast<unsigned int>(inputRect.left);
    params.InputSubrectBase.Y = static_cast<unsigned int>(inputRect.top);
    params.InputSubrectSize.Width =
        static_cast<unsigned int>(inputRect.right - inputRect.left);
    params.InputSubrectSize.Height =
        static_cast<unsigned int>(inputRect.bottom - inputRect.top);
    params.OutputSubrectBase.X = static_cast<unsigned int>(outputRect.left);
    params.OutputSubrectBase.Y = static_cast<unsigned int>(outputRect.top);
    params.OutputSubrectSize.Width =
        static_cast<unsigned int>(outputRect.right - outputRect.left);
    params.OutputSubrectSize.Height =
        static_cast<unsigned int>(outputRect.bottom - outputRect.top);
    params.QualityLevel = NVSDK_NGX_VSR_Quality_Ultra;

    NVSDK_NGX_Result result;
    {
        MultiThreadScope lock(impl_->multithread.Get());
        result = NGX_D3D11_EVALUATE_VSR_EXT(
            impl_->context.Get(), impl_->feature, impl_->parameters, &params);
    }
    if (NVSDK_NGX_FAILED(result)) {
        impl_->status = NgxResultText(L"Evaluating RTX Video VSR", result);
        return false;
    }

    impl_->status = L"RTX Video Super Resolution is active.";
    return true;
}

bool RtxVideoVsr::IsAvailable() const noexcept
{
    return impl_->available;
}

const std::wstring& RtxVideoVsr::Status() const noexcept
{
    return impl_->status;
}
