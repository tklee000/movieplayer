#include "NvidiaFrameInterpolator.h"

#include <d3d11_4.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>

#ifndef MOVIEPLAYER_HAS_NVIDIA_FRUC
#define MOVIEPLAYER_HAS_NVIDIA_FRUC 0
#endif

#if MOVIEPLAYER_HAS_NVIDIA_FRUC
#include <NvOFFRUC.h>
#endif

using Microsoft::WRL::ComPtr;

namespace {

constexpr DXGI_FORMAT kFrucFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

std::wstring StatusText(const wchar_t* operation, int status)
{
    std::wostringstream stream;
    stream << operation << L" failed (NvOFFRUC status " << status << L").";
    return stream.str();
}

std::wstring ApplicationDirectory()
{
    std::array<wchar_t, 32768> path = {};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                            static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) {
        return {};
    }
    std::wstring result(path.data(), length);
    const size_t separator = result.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        return {};
    }
    result.resize(separator);
    return result;
}

}  // namespace

struct NvidiaFrameInterpolator::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11Device5> device5;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D11Fence> fence;
    UINT64 fenceValue = 0;

    HMODULE module = nullptr;
    bool available = false;
    std::wstring status =
        L"NVIDIA 2x frame interpolation has not been initialized.";

    UINT width = 0;
    UINT height = 0;
    std::array<ComPtr<ID3D11Texture2D>, 2> inputs;
    std::array<ComPtr<ID3D11RenderTargetView>, 2> inputTargets;
    std::array<ComPtr<ID3D11ShaderResourceView>, 2> inputViews;
    ComPtr<ID3D11Texture2D> output;
    ComPtr<ID3D11ShaderResourceView> outputView;
    unsigned nextInput = 0;
    unsigned currentInput = 0;
    bool hasPrevious = false;

#if MOVIEPLAYER_HAS_NVIDIA_FRUC
    PtrToFuncNvOFFRUCCreate create = nullptr;
    PtrToFuncNvOFFRUCRegisterResource registerResource = nullptr;
    PtrToFuncNvOFFRUCUnregisterResource unregisterResource = nullptr;
    PtrToFuncNvOFFRUCProcess process = nullptr;
    PtrToFuncNvOFFRUCDestroy destroy = nullptr;
    NvOFFRUCHandle handle = nullptr;
    bool resourcesRegistered = false;

    template <typename T>
    bool LoadFunction(const char* name, T& destination)
    {
        destination = reinterpret_cast<T>(GetProcAddress(module, name));
        if (!destination) {
            status = L"NvOFFRUC.dll does not export ";
            while (*name) {
                status.push_back(static_cast<unsigned char>(*name++));
            }
            status += L".";
            return false;
        }
        return true;
    }

    void ReleaseSession()
    {
        if (handle && resourcesRegistered && unregisterResource) {
            NvOFFRUC_UNREGISTER_RESOURCE_PARAM params = {};
            params.pArrResource[0] = inputs[0].Get();
            params.pArrResource[1] = inputs[1].Get();
            params.pArrResource[2] = output.Get();
            params.uiCount = 3;
            unregisterResource(handle, &params);
        }
        resourcesRegistered = false;
        if (handle && destroy) {
            destroy(handle);
        }
        handle = nullptr;
        outputView.Reset();
        output.Reset();
        for (auto& view : inputViews) view.Reset();
        for (auto& target : inputTargets) target.Reset();
        for (auto& input : inputs) input.Reset();
        width = height = 0;
        nextInput = currentInput = 0;
        hasPrevious = false;
    }
#else
    void ReleaseSession()
    {
        outputView.Reset();
        output.Reset();
        for (auto& view : inputViews) view.Reset();
        for (auto& target : inputTargets) target.Reset();
        for (auto& input : inputs) input.Reset();
        width = height = 0;
        nextInput = currentInput = 0;
        hasPrevious = false;
    }
#endif
};

NvidiaFrameInterpolator::NvidiaFrameInterpolator()
    : impl_(std::make_unique<Impl>())
{
}

NvidiaFrameInterpolator::~NvidiaFrameInterpolator()
{
    Shutdown();
}

bool NvidiaFrameInterpolator::Initialize(ID3D11Device* device)
{
    Shutdown();
    if (!device) {
        impl_->status = L"NVIDIA FRUC received a null D3D11 device.";
        return false;
    }

#if !MOVIEPLAYER_HAS_NVIDIA_FRUC
    impl_->status =
        L"NVIDIA Optical Flow SDK 5.0 is not installed. Run "
        L"scripts/setup_nvidia_optical_flow_sdk.ps1 with the SDK archive.";
    return false;
#else
    impl_->device = device;
    device->GetImmediateContext(&impl_->context);
    if (!impl_->context ||
        FAILED(device->QueryInterface(IID_PPV_ARGS(&impl_->device5))) ||
        FAILED(impl_->context.As(&impl_->context4))) {
        impl_->status =
            L"NVIDIA FRUC requires the Windows 10 D3D11 fence interfaces.";
        Shutdown();
        return false;
    }
    HRESULT hr = impl_->device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED,
                                              IID_PPV_ARGS(&impl_->fence));
    if (FAILED(hr)) {
        impl_->status = L"Creating the NVIDIA FRUC synchronization fence failed.";
        Shutdown();
        return false;
    }

    const std::wstring directory = ApplicationDirectory();
    if (directory.empty()) {
        impl_->status = L"The MoviePlayer application directory is unavailable.";
        Shutdown();
        return false;
    }
    const std::wstring dllPath = directory + L"\\NvOFFRUC.dll";
    impl_->module = LoadLibraryExW(
        dllPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!impl_->module) {
        impl_->status = L"NvOFFRUC.dll was not found beside MoviePlayer.exe.";
        Shutdown();
        return false;
    }
    if (!impl_->LoadFunction("NvOFFRUCCreate", impl_->create) ||
        !impl_->LoadFunction("NvOFFRUCRegisterResource", impl_->registerResource) ||
        !impl_->LoadFunction("NvOFFRUCUnregisterResource", impl_->unregisterResource) ||
        !impl_->LoadFunction("NvOFFRUCProcess", impl_->process) ||
        !impl_->LoadFunction("NvOFFRUCDestroy", impl_->destroy)) {
        Shutdown();
        return false;
    }

    impl_->available = true;
    impl_->status = L"NVIDIA 2x frame interpolation is available.";
    return true;
#endif
}

void NvidiaFrameInterpolator::Shutdown()
{
    impl_->ReleaseSession();
#if MOVIEPLAYER_HAS_NVIDIA_FRUC
    impl_->create = nullptr;
    impl_->registerResource = nullptr;
    impl_->unregisterResource = nullptr;
    impl_->process = nullptr;
    impl_->destroy = nullptr;
#endif
    if (impl_->module) {
        FreeLibrary(impl_->module);
        impl_->module = nullptr;
    }
    impl_->fence.Reset();
    impl_->context4.Reset();
    impl_->context.Reset();
    impl_->device5.Reset();
    impl_->device.Reset();
    impl_->fenceValue = 0;
    impl_->available = false;
}

bool NvidiaFrameInterpolator::Prepare(UINT width, UINT height)
{
#if !MOVIEPLAYER_HAS_NVIDIA_FRUC
    (void)width;
    (void)height;
    return false;
#else
    if (!impl_->available || !impl_->device || !width || !height) {
        impl_->status = L"NVIDIA FRUC is unavailable or received invalid dimensions.";
        return false;
    }
    if (impl_->handle && impl_->width == width && impl_->height == height) {
        return true;
    }

    impl_->ReleaseSession();
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = kFrucFormat;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED |
                     D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    for (size_t i = 0; i < impl_->inputs.size(); ++i) {
        HRESULT hr = impl_->device->CreateTexture2D(&desc, nullptr,
                                                     &impl_->inputs[i]);
        if (FAILED(hr) ||
            FAILED(impl_->device->CreateRenderTargetView(
                impl_->inputs[i].Get(), nullptr, &impl_->inputTargets[i])) ||
            FAILED(impl_->device->CreateShaderResourceView(
                impl_->inputs[i].Get(), nullptr, &impl_->inputViews[i]))) {
            impl_->status = L"Creating NVIDIA FRUC input textures failed.";
            impl_->ReleaseSession();
            return false;
        }
    }
    if (FAILED(impl_->device->CreateTexture2D(&desc, nullptr, &impl_->output)) ||
        FAILED(impl_->device->CreateShaderResourceView(
            impl_->output.Get(), nullptr, &impl_->outputView))) {
        impl_->status = L"Creating the NVIDIA FRUC output texture failed.";
        impl_->ReleaseSession();
        return false;
    }

    NvOFFRUC_CREATE_PARAM createParams = {};
    createParams.pDevice = impl_->device.Get();
    createParams.uiWidth = width;
    createParams.uiHeight = height;
    createParams.eResourceType = DirectX11Resource;
    createParams.eSurfaceFormat = ARGBSurface;
    createParams.eCUDAResourceType = CudaResourceCuDevicePtr;
    int status = static_cast<int>(impl_->create(&createParams, &impl_->handle));
    if (status != 0 || !impl_->handle) {
        impl_->status = StatusText(L"NvOFFRUCCreate", status);
        impl_->ReleaseSession();
        return false;
    }

    NvOFFRUC_REGISTER_RESOURCE_PARAM registerParams = {};
    registerParams.pArrResource[0] = impl_->inputs[0].Get();
    registerParams.pArrResource[1] = impl_->inputs[1].Get();
    registerParams.pArrResource[2] = impl_->output.Get();
    registerParams.uiCount = 3;
    registerParams.pD3D11FenceObj = impl_->fence.Get();
    status = static_cast<int>(
        impl_->registerResource(impl_->handle, &registerParams));
    if (status != 0) {
        impl_->status = StatusText(L"NvOFFRUCRegisterResource", status);
        impl_->ReleaseSession();
        return false;
    }
    impl_->resourcesRegistered = true;
    impl_->width = width;
    impl_->height = height;
    impl_->status = L"NVIDIA 2x frame interpolation is ready.";
    return true;
#endif
}

ID3D11Texture2D* NvidiaFrameInterpolator::InputTexture() const noexcept
{
    return impl_->inputs[impl_->nextInput].Get();
}

ID3D11RenderTargetView* NvidiaFrameInterpolator::InputRenderTarget() const noexcept
{
    return impl_->inputTargets[impl_->nextInput].Get();
}

bool NvidiaFrameInterpolator::Process(double inputTimestamp,
                                      double outputTimestamp,
                                      bool& hasHistory)
{
    hasHistory = false;
#if !MOVIEPLAYER_HAS_NVIDIA_FRUC
    (void)inputTimestamp;
    (void)outputTimestamp;
    return false;
#else
    if (!impl_->handle || !impl_->process ||
        !std::isfinite(inputTimestamp) || !std::isfinite(outputTimestamp)) {
        impl_->status = L"NVIDIA FRUC received invalid process parameters.";
        return false;
    }

    hasHistory = impl_->hasPrevious;
    impl_->currentInput = impl_->nextInput;
    const UINT64 inputReady = ++impl_->fenceValue;
    const UINT64 outputReady = ++impl_->fenceValue;
    if (FAILED(impl_->context4->Signal(impl_->fence.Get(), inputReady))) {
        impl_->status = L"Signaling the NVIDIA FRUC input fence failed.";
        return false;
    }

    NvOFFRUC_PROCESS_IN_PARAMS input = {};
    input.stFrameDataInput.pFrame = impl_->inputs[impl_->currentInput].Get();
    input.stFrameDataInput.nTimeStamp = inputTimestamp;
    input.uSyncWait.FenceWaitValue.uiFenceValueToWaitOn = inputReady;

    bool repeated = false;
    NvOFFRUC_PROCESS_OUT_PARAMS output = {};
    output.stFrameDataOutput.pFrame = impl_->output.Get();
    output.stFrameDataOutput.nTimeStamp = outputTimestamp;
    output.stFrameDataOutput.bHasFrameRepetitionOccurred = &repeated;
    output.uSyncSignal.FenceSignalValue.uiFenceValueToSignalOn = outputReady;

    const int status = static_cast<int>(
        impl_->process(impl_->handle, &input, &output));
    if (status != 0) {
        impl_->status = StatusText(L"NvOFFRUCProcess", status);
        return false;
    }
    if (FAILED(impl_->context4->Wait(impl_->fence.Get(), outputReady))) {
        impl_->status = L"Waiting for the NVIDIA FRUC output fence failed.";
        return false;
    }

    impl_->hasPrevious = true;
    impl_->nextInput = (impl_->currentInput + 1U) % 2U;
    impl_->status = repeated
                        ? L"NVIDIA frame interpolation repeated the previous frame."
                        : L"NVIDIA 2x frame interpolation is active.";
    return true;
#endif
}

ID3D11Texture2D* NvidiaFrameInterpolator::CurrentTexture() const noexcept
{
    return impl_->inputs[impl_->currentInput].Get();
}

ID3D11ShaderResourceView* NvidiaFrameInterpolator::CurrentView() const noexcept
{
    return impl_->inputViews[impl_->currentInput].Get();
}

ID3D11Texture2D* NvidiaFrameInterpolator::InterpolatedTexture() const noexcept
{
    return impl_->output.Get();
}

ID3D11ShaderResourceView* NvidiaFrameInterpolator::InterpolatedView() const noexcept
{
    return impl_->outputView.Get();
}

void NvidiaFrameInterpolator::ResetHistory()
{
    if (!impl_->width || !impl_->height) {
        impl_->hasPrevious = false;
        return;
    }
    const UINT width = impl_->width;
    const UINT height = impl_->height;
    impl_->ReleaseSession();
    Prepare(width, height);
}

bool NvidiaFrameInterpolator::IsAvailable() const noexcept
{
    return impl_->available;
}

const std::wstring& NvidiaFrameInterpolator::Status() const noexcept
{
    return impl_->status;
}
