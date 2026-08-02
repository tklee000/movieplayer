#include <windows.h>
#include <shellapi.h>
#include <mfapi.h>
#include <mfplay.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cwchar>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT_PTR kPositionTimer = 1;

ComPtr<IMFPMediaPlayer> g_player;
double g_startSeconds = 0.0;

class PlayerCallback final : public IMFPMediaPlayerCallback {
public:
    explicit PlayerCallback(HWND window) : window_(window) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,
                                             void** object) override {
        if (!object) return E_POINTER;
        if (iid == IID_IUnknown || iid == __uuidof(IMFPMediaPlayerCallback)) {
            *object = static_cast<IMFPMediaPlayerCallback*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }

    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* event) override {
        if (!event) return;
        if (FAILED(event->hrEvent)) {
            wchar_t title[128] = {};
            swprintf_s(title, L"Media Foundation reference error: 0x%08X",
                       static_cast<unsigned>(event->hrEvent));
            SetWindowTextW(window_, title);
            return;
        }
        switch (event->eEventType) {
        case MFP_EVENT_TYPE_MEDIAITEM_CREATED: {
            const auto* created =
                reinterpret_cast<const MFP_MEDIAITEM_CREATED_EVENT*>(event);
            event->pMediaPlayer->SetMediaItem(created->pMediaItem);
            break;
        }
        case MFP_EVENT_TYPE_MEDIAITEM_SET: {
            PROPVARIANT position;
            PropVariantInit(&position);
            position.vt = VT_I8;
            position.hVal.QuadPart = static_cast<LONGLONG>(
                g_startSeconds * 10'000'000.0);
            event->pMediaPlayer->SetPosition(MFP_POSITIONTYPE_100NS,
                                             &position);
            PropVariantClear(&position);
            break;
        }
        case MFP_EVENT_TYPE_POSITION_SET:
            event->pMediaPlayer->Play();
            break;
        case MFP_EVENT_TYPE_PLAY:
            SetTimer(window_, kPositionTimer, 250, nullptr);
            break;
        case MFP_EVENT_TYPE_PLAYBACK_ENDED:
            KillTimer(window_, kPositionTimer);
            break;
        default:
            break;
        }
    }

private:
    std::atomic<ULONG> references_{1};
    HWND window_ = nullptr;
};

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam,
                                 LPARAM lParam) {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (g_player) g_player->UpdateVideo();
        return 0;
    case WM_TIMER:
        if (wParam == kPositionTimer && g_player) {
            PROPVARIANT position;
            PropVariantInit(&position);
            if (SUCCEEDED(g_player->GetPosition(MFP_POSITIONTYPE_100NS,
                                                &position)) &&
                position.vt == VT_I8) {
                const double seconds =
                    static_cast<double>(position.hVal.QuadPart) / 10'000'000.0;
                const unsigned minutes =
                    static_cast<unsigned>(seconds / 60.0);
                const double remainder = seconds - minutes * 60.0;
                wchar_t title[160] = {};
                swprintf_s(title,
                           L"Media Foundation reference - %02u:%05.2f "
                           L"(independent A/V pipeline)",
                           minutes, remainder);
                SetWindowTextW(window, title);
            }
            PropVariantClear(&position);
        }
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) PostMessageW(window, WM_CLOSE, 0, 0);
        return 0;
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kPositionTimer);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(),
                                             &argumentCount);
    if (!arguments || argumentCount < 2) {
        MessageBoxW(nullptr,
                    L"Usage: MfReferencePlayer <media-path> [start-seconds]",
                    L"Media Foundation reference player", MB_OK | MB_ICONERROR);
        if (arguments) LocalFree(arguments);
        return 1;
    }
    const std::wstring mediaPath = arguments[1];
    if (argumentCount >= 3) {
        wchar_t* end = nullptr;
        errno = 0;
        const double parsed = std::wcstod(arguments[2], &end);
        if (errno != 0 || end == arguments[2] || *end != L'\0' ||
            parsed < 0.0) {
            MessageBoxW(nullptr, L"The start time must be a non-negative number.",
                        L"Media Foundation reference player",
                        MB_OK | MB_ICONERROR);
            LocalFree(arguments);
            return 1;
        }
        g_startSeconds = parsed;
    }
    LocalFree(arguments);

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) {
        CoUninitialize();
        return 2;
    }

    WNDCLASSW windowClass = {};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.lpszClassName = L"MoviePlayerMfReferenceWindow";
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (!RegisterClassW(&windowClass)) {
        MFShutdown();
        CoUninitialize();
        return 3;
    }

    HWND window = CreateWindowExW(
        0, windowClass.lpszClassName,
        L"Media Foundation reference - opening media",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 2560, 1440, nullptr,
        nullptr, instance, nullptr);
    if (!window) {
        MFShutdown();
        CoUninitialize();
        return 4;
    }
    ShowWindow(window, SW_MAXIMIZE);
    UpdateWindow(window);

    PlayerCallback* callback = new PlayerCallback(window);
    HRESULT hr = MFPCreateMediaPlayer(nullptr, FALSE, MFP_OPTION_NONE, callback,
                                      window, &g_player);
    if (SUCCEEDED(hr)) {
        hr = g_player->CreateMediaItemFromURL(mediaPath.c_str(), FALSE, 0,
                                              nullptr);
    }
    if (FAILED(hr)) {
        wchar_t title[128] = {};
        swprintf_s(title, L"Media Foundation reference error: 0x%08X",
                   static_cast<unsigned>(hr));
        SetWindowTextW(window, title);
    }

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (g_player) {
        g_player->Shutdown();
        g_player.Reset();
    }
    callback->Release();
    MFShutdown();
    CoUninitialize();
    return 0;
}
