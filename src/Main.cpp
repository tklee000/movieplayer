#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "D3DRenderer.h"
#include "FileAssociations.h"
#include "Localization.h"
#include "MediaFileTools.h"
#include "PlayerEngine.h"
#include "resource.h"
#include "SubtitleTrack.h"
#include "Utilities.h"
#include "Version.h"
#include "WhisperSubtitleJob.h"

extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

namespace {

constexpr wchar_t kMainClassName[] = L"MoviePlayer.MainWindow";
constexpr wchar_t kVideoClassName[] = L"MoviePlayer.VideoSurface";
constexpr wchar_t kControlPanelClassName[] = L"MoviePlayer.ControlPanel";
constexpr UINT_PTR kPlaybackTimer = 1;
constexpr UINT kOpenInitialFile = WM_APP + 1;
constexpr int kSeekRange = 10000;
constexpr UINT kAudioTrackCommandBase = 30000;
constexpr UINT kSubtitleTrackCommandBase = 31000;
constexpr std::size_t kMaximumTrackMenuItems = 512;
constexpr COLORREF kColorWindow = RGB(7, 8, 10);
constexpr COLORREF kColorPanel = RGB(18, 18, 21);
constexpr COLORREF kColorPanelTop = RGB(34, 35, 41);
constexpr COLORREF kColorButton = RGB(30, 31, 36);
constexpr COLORREF kColorButtonDown = RGB(43, 44, 52);
constexpr COLORREF kColorButtonBorder = RGB(64, 65, 74);
constexpr COLORREF kColorText = RGB(239, 240, 244);
constexpr COLORREF kColorTextMuted = RGB(146, 150, 160);
constexpr COLORREF kColorTrack = RGB(70, 72, 80);
constexpr COLORREF kColorAccent = RGB(255, 118, 49);
constexpr COLORREF kColorAccentAlt = RGB(180, 76, 216);
constexpr COLORREF kColorMenu = RGB(17, 18, 21);
constexpr COLORREF kColorMenuHot = RGB(42, 43, 49);
constexpr COLORREF kColorMenuLine = RGB(57, 59, 66);
constexpr COLORREF kColorMenuBorder = RGB(76, 78, 84);
constexpr int kControlPanelHeight = 62;
constexpr int kFullscreenControlHotZone = 86;
constexpr DWORD kDwmUseImmersiveDarkMode = 20;
constexpr DWORD kDwmUseImmersiveDarkModeBefore20H1 = 19;
constexpr DWORD kDwmBorderColor = 34;
constexpr DWORD kDwmCaptionColor = 35;
constexpr DWORD kDwmTextColor = 36;

struct DarkMenuItem {
    std::wstring text;
    bool separator = false;
    bool menuBarItem = false;
    bool hasSubmenu = false;
};

struct MenuState {
    bool muted = false;
    bool loop = false;
    bool autoPlayNext = false;
    bool alwaysOnTop = false;
    bool rtxVideoUpscaling = false;
    bool subtitleOff = false;
    bool whisperBusy = false;
    bool engineOpen = false;
    std::uint32_t selectedAudioTrackId = 0;
    std::uint32_t selectedSubtitleTrackId = 0;
    bool externalSubtitle = false;
    int speedId = 0;
    int languageId = 0;
};

enum CommandId : int {
    ID_FILE_OPEN = 1001,
    ID_FILE_CLOSE,
    ID_FILE_ASSOCIATIONS,
    ID_FILE_EXIT,
    ID_PLAY_PAUSE,
    ID_STOP,
    ID_SEEK_BACK,
    ID_SEEK_FORWARD,
    ID_STEP_FRAME,
    ID_MUTE,
    ID_VOLUME_UP,
    ID_VOLUME_DOWN,
    ID_FULLSCREEN,
    ID_EXIT_FULLSCREEN,
    ID_ALWAYS_ON_TOP,
    ID_LOOP,
    ID_AUTO_PLAY_NEXT,
    ID_SUBTITLE_OPEN,
    ID_SUBTITLE_AUTO,
    ID_SUBTITLE_WHISPER_GENERATE,
    ID_SUBTITLE_WHISPER_CANCEL,
    ID_SUBTITLE_OFF,
    ID_ABOUT,
    ID_SPEED_050,
    ID_SPEED_075,
    ID_SPEED_100,
    ID_SPEED_125,
    ID_SPEED_150,
    ID_SPEED_200,
    ID_RTX_VIDEO_UPSCALING,
    ID_LANGUAGE_EN,
    ID_LANGUAGE_JA,
    ID_LANGUAGE_KO,
    ID_LANGUAGE_FR,
    ID_LANGUAGE_ZH_CN,
    ID_LANGUAGE_ZH_TW,
    ID_LANGUAGE_ES,
    ID_LANGUAGE_PT,
    ID_LANGUAGE_HI,
    ID_LANGUAGE_ID,
    ID_LANGUAGE_AR,
    ID_CONTROL_OPEN = 2001,
    ID_CONTROL_PLAY,
    ID_CONTROL_STOP,
    ID_CONTROL_MUTE,
    ID_CONTROL_SPEED,
    ID_CONTROL_FULLSCREEN,
    ID_CONTROL_SEEK,
    ID_CONTROL_VOLUME,
    ID_CONTROL_TIME,
    ID_CONTROL_STATUS,
};

int ScaleForDpi(int value, UINT dpi) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

void FillRectColor(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void DrawRoundRect(HDC dc, const RECT& rect, int radius,
                   COLORREF fill, COLORREF border, int borderWidth = 1) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, borderWidth, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    DeleteObject(brush);
}

void DrawLine(HDC dc, int x1, int y1, int x2, int y2,
              COLORREF color, int width) {
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, x1, y1, nullptr);
    LineTo(dc, x2, y2);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void DrawFilledPolygon(HDC dc, const POINT* points, int count, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    Polygon(dc, points, count);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    DeleteObject(brush);
}

bool ContainsRightToLeftScript(const std::wstring& text) {
    return std::any_of(text.begin(), text.end(), [](wchar_t character) {
        return (character >= 0x0590 && character <= 0x08FF) ||
               (character >= 0xFB1D && character <= 0xFDFF) ||
               (character >= 0xFE70 && character <= 0xFEFF);
    });
}

bool SameMenuState(const MenuState& left, const MenuState& right) {
    return left.muted == right.muted &&
           left.loop == right.loop &&
           left.autoPlayNext == right.autoPlayNext &&
           left.alwaysOnTop == right.alwaysOnTop &&
           left.rtxVideoUpscaling == right.rtxVideoUpscaling &&
           left.subtitleOff == right.subtitleOff &&
           left.whisperBusy == right.whisperBusy &&
           left.engineOpen == right.engineOpen &&
           left.selectedAudioTrackId == right.selectedAudioTrackId &&
           left.selectedSubtitleTrackId == right.selectedSubtitleTrackId &&
           left.externalSubtitle == right.externalSubtitle &&
           left.speedId == right.speedId &&
           left.languageId == right.languageId;
}

bool SetWindowTextIfChanged(HWND hwnd, const std::wstring& text) {
    if (!hwnd) {
        return false;
    }
    const int length = GetWindowTextLengthW(hwnd);
    if (length == static_cast<int>(text.size())) {
        std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1);
        if (GetWindowTextW(hwnd, buffer.data(), length + 1) > 0 &&
            text == buffer.data()) {
            return false;
        }
        if (length == 0 && text.empty()) {
            return false;
        }
    }
    SetWindowTextW(hwnd, text.c_str());
    return true;
}

bool IsKoreanWindowsUiLanguage() {
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_KOREAN ||
           PRIMARYLANGID(GetSystemDefaultUILanguage()) == LANG_KOREAN;
}

bool PreferMsGothicForJapaneseUi() {
    return IsKoreanWindowsUiLanguage() &&
           _wcsicmp(Localization::CurrentLanguage().c_str(), L"ja") == 0;
}

void ApplyDarkWindowTheme(HWND hwnd) {
    BOOL enabled = TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd, kDwmUseImmersiveDarkMode,
                                     &enabled, sizeof(enabled)))) {
        DwmSetWindowAttribute(hwnd, kDwmUseImmersiveDarkModeBefore20H1,
                              &enabled, sizeof(enabled));
    }
    COLORREF caption = RGB(18, 18, 21);
    COLORREF text = kColorText;
    DwmSetWindowAttribute(hwnd, kDwmCaptionColor, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, kDwmTextColor, &text, sizeof(text));
    SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr);
}

void EnableDarkMenuFrames() {
    HMODULE theme = GetModuleHandleW(L"uxtheme.dll");
    if (!theme) {
        return;
    }
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();
    auto setPreferredAppMode = reinterpret_cast<SetPreferredAppModeFn>(
        GetProcAddress(theme, MAKEINTRESOURCEA(135)));
    auto flushMenuThemes = reinterpret_cast<FlushMenuThemesFn>(
        GetProcAddress(theme, MAKEINTRESOURCEA(136)));
    if (setPreferredAppMode) {
        setPreferredAppMode(2);  // ForceDark
    }
    if (flushMenuThemes) {
        flushMenuThemes();
    }
}

void DrawPopupMenuBorder(HWND hwnd) {
    HDC dc = GetWindowDC(hwnd);
    if (!dc) {
        return;
    }
    RECT window = {};
    GetWindowRect(hwnd, &window);
    OffsetRect(&window, -window.left, -window.top);
    HBRUSH brush = CreateSolidBrush(kColorMenuBorder);
    FrameRect(dc, &window, brush);
    DeleteObject(brush);
    ReleaseDC(hwnd, dc);
}

void ClipPopupMenuFrame(HWND hwnd) {
    RECT window = {};
    if (!GetWindowRect(hwnd, &window)) {
        return;
    }
    HRGN region = CreateRectRgn(0, 0, window.right - window.left,
                                window.bottom - window.top);
    if (!region) {
        return;
    }
    if (!SetWindowRgn(hwnd, region, TRUE)) {
        DeleteObject(region);
    }
}

LRESULT CALLBACK PopupMenuSubclassProc(HWND hwnd, UINT message, WPARAM wParam,
                                       LPARAM lParam, UINT_PTR subclassId,
                                       DWORD_PTR refData) {
    UNREFERENCED_PARAMETER(refData);
    if (message == WM_NCPAINT || message == WM_NCACTIVATE) {
        const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        DrawPopupMenuBorder(hwnd);
        return result;
    }
    if (message == WM_PAINT) {
        const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        DrawPopupMenuBorder(hwnd);
        return result;
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, PopupMenuSubclassProc, subclassId);
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

void CALLBACK MenuWindowEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                                  LONG objectId, LONG, DWORD, DWORD) {
    if (event != EVENT_OBJECT_SHOW || objectId != OBJID_WINDOW || !hwnd) {
        return;
    }
    wchar_t className[32] = {};
    if (!GetClassNameW(hwnd, className, static_cast<int>(std::size(className))) ||
        wcscmp(className, L"#32768") != 0) {
        return;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId != GetCurrentProcessId()) {
        return;
    }
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd, GWL_STYLE, style & ~WS_BORDER);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                      exStyle & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE |
                                  WS_EX_CLIENTEDGE | WS_EX_STATICEDGE));
    DWMNCRENDERINGPOLICY renderingPolicy = DWMNCRP_DISABLED;
    DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY,
                          &renderingPolicy, sizeof(renderingPolicy));
    BOOL allowNcPaint = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_ALLOW_NCPAINT,
                          &allowNcPaint, sizeof(allowNcPaint));
    COLORREF border = kColorMenuBorder;
    DwmSetWindowAttribute(hwnd, kDwmBorderColor, &border, sizeof(border));
    SetWindowSubclass(hwnd, PopupMenuSubclassProc, 1, 0);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);
    ClipPopupMenuFrame(hwnd);
    RedrawWindow(hwnd, nullptr, nullptr,
                 RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW);
    DrawPopupMenuBorder(hwnd);
}

struct SliderGeometry {
    RECT client = {};
    RECT track = {};
    RECT knob = {};
    UINT dpi = 96;
    int minPos = 0;
    int maxPos = 0;
    int pos = 0;
    int thickness = 1;
};

SliderGeometry CalculateSliderGeometry(HWND hwnd) {
    SliderGeometry geometry;
    GetClientRect(hwnd, &geometry.client);
    HWND parent = GetParent(hwnd);
    geometry.dpi = parent ? GetDpiForWindow(parent) : 96;
    const bool seek = GetDlgCtrlID(hwnd) == ID_CONTROL_SEEK;
    const int thumbSize = seek ? ScaleForDpi(10, geometry.dpi)
                               : ScaleForDpi(9, geometry.dpi);
    geometry.thickness = ScaleForDpi(3, geometry.dpi);
    const int left = thumbSize / 2;
    const int right = std::max(
        left + 1, static_cast<int>(geometry.client.right) - thumbSize / 2);
    const int centerY = (geometry.client.top + geometry.client.bottom) / 2;
    geometry.track = {left, centerY - geometry.thickness / 2,
                      right, centerY + geometry.thickness / 2};
    geometry.minPos = static_cast<int>(
        SendMessageW(hwnd, TBM_GETRANGEMIN, 0, 0));
    geometry.maxPos = static_cast<int>(
        SendMessageW(hwnd, TBM_GETRANGEMAX, 0, 0));
    geometry.pos = static_cast<int>(SendMessageW(hwnd, TBM_GETPOS, 0, 0));
    const int span = std::max(1, geometry.maxPos - geometry.minPos);
    const int knobX = geometry.track.left + MulDiv(
        geometry.track.right - geometry.track.left,
        geometry.pos - geometry.minPos, span);
    geometry.knob = {knobX - thumbSize / 2, centerY - thumbSize / 2,
                     knobX + thumbSize / 2, centerY + thumbSize / 2};
    return geometry;
}

void DrawSliderControl(HWND hwnd, HDC dc) {
    const SliderGeometry geometry = CalculateSliderGeometry(hwnd);
    FillRectColor(dc, geometry.client, kColorPanel);
    DrawRoundRect(dc, geometry.track, geometry.thickness,
                  kColorTrack, kColorTrack);

    const int fillRight = (geometry.knob.left + geometry.knob.right) / 2;
    const bool seek = GetDlgCtrlID(hwnd) == ID_CONTROL_SEEK;
    if (fillRight > geometry.track.left) {
        RECT filled = geometry.track;
        filled.right = std::max<LONG>(filled.left + geometry.thickness, fillRight);
        const COLORREF accent = seek ? kColorAccent : kColorAccentAlt;
        DrawRoundRect(dc, filled, geometry.thickness, accent, accent);
    }

    HBRUSH brush = CreateSolidBrush(kColorText);
    HPEN pen = CreatePen(PS_SOLID, ScaleForDpi(2, geometry.dpi),
                         seek ? kColorAccent : kColorAccentAlt);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    Ellipse(dc, geometry.knob.left, geometry.knob.top,
            geometry.knob.right, geometry.knob.bottom);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    DeleteObject(brush);
}

void PaintSliderControl(HWND hwnd, HDC targetDc) {
    RECT client = {};
    GetClientRect(hwnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        return;
    }

    HDC bufferDc = CreateCompatibleDC(targetDc);
    HBITMAP bufferBitmap = CreateCompatibleBitmap(targetDc, width, height);
    if (!bufferDc || !bufferBitmap) {
        if (bufferBitmap) {
            DeleteObject(bufferBitmap);
        }
        if (bufferDc) {
            DeleteDC(bufferDc);
        }
        DrawSliderControl(hwnd, targetDc);
        return;
    }

    HGDIOBJ oldBitmap = SelectObject(bufferDc, bufferBitmap);
    DrawSliderControl(hwnd, bufferDc);
    BitBlt(targetDc, 0, 0, width, height, bufferDc, 0, 0, SRCCOPY);
    SelectObject(bufferDc, oldBitmap);
    DeleteObject(bufferBitmap);
    DeleteDC(bufferDc);
}

void RedrawSliderNow(HWND hwnd) {
    RedrawWindow(hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOCHILDREN);
}

LRESULT CALLBACK SliderSubclassProc(HWND hwnd, UINT message, WPARAM wParam,
                                    LPARAM lParam, UINT_PTR subclassId,
                                    DWORD_PTR refData) {
    UNREFERENCED_PARAMETER(refData);
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(hwnd, &paint);
        PaintSliderControl(hwnd, dc);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_PRINTCLIENT:
        PaintSliderControl(hwnd, reinterpret_cast<HDC>(wParam));
        return 0;
    case WM_LBUTTONDOWN: {
        const SliderGeometry geometry = CalculateSliderGeometry(hwnd);
        POINT point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        RECT thumbHitArea = geometry.knob;
        const int hitPadding = ScaleForDpi(2, geometry.dpi);
        InflateRect(&thumbHitArea, hitPadding, hitPadding);
        if (!PtInRect(&thumbHitArea, point)) {
            const int trackLeft = static_cast<int>(geometry.track.left);
            const int trackRight = static_cast<int>(geometry.track.right);
            const int x = std::max(trackLeft, std::min(trackRight,
                                                       static_cast<int>(point.x)));
            const int span = std::max(1, geometry.maxPos - geometry.minPos);
            const int position = geometry.minPos + MulDiv(
                x - trackLeft, span, std::max(1, trackRight - trackLeft));
            SetFocus(hwnd);
            SendMessageW(hwnd, TBM_SETPOS, TRUE, position);
            RedrawSliderNow(hwnd);
            SendMessageW(GetParent(hwnd), WM_HSCROLL,
                         MAKEWPARAM(TB_THUMBPOSITION, position),
                         reinterpret_cast<LPARAM>(hwnd));
            return 0;
        }
        const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        RedrawSliderNow(hwnd);
        return result;
    }
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED: {
        const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        RedrawSliderNow(hwnd);
        return result;
    }
    case WM_MOUSEMOVE: {
        const bool dragging = (wParam & MK_LBUTTON) != 0 || GetCapture() == hwnd;
        const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        if (dragging) {
            RedrawSliderNow(hwnd);
        }
        return result;
    }
    case WM_THEMECHANGED:
        InvalidateRect(hwnd, nullptr, FALSE);
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, SliderSubclassProc, subclassId);
        break;
    default:
        break;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

void PaintControlPanel(HWND hwnd, HDC dc) {
    RECT client = {};
    GetClientRect(hwnd, &client);
    FillRectColor(dc, client, kColorPanel);
    const UINT dpi = GetDpiForWindow(GetParent(hwnd) ? GetParent(hwnd) : hwnd);
    RECT topLine = {client.left, client.top, client.right,
                    client.top + ScaleForDpi(1, dpi)};
    FillRectColor(dc, topLine, kColorPanelTop);
}

LRESULT CALLBACK ControlPanelWindowProc(HWND hwnd, UINT message,
                                        WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(hwnd, &paint);
        PaintControlPanel(hwnd, dc);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_PRINTCLIENT:
        PaintControlPanel(hwnd, reinterpret_cast<HDC>(wParam));
        return 0;
    default:
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

std::wstring LowerExtension(const std::wstring& path) {
    const wchar_t* extension = PathFindExtensionW(path.c_str());
    std::wstring result = extension ? extension : L"";
    std::transform(result.begin(), result.end(), result.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return result;
}

class MainWindow {
public:
    bool Create(HINSTANCE instance, const std::wstring& initialPath,
                const std::wstring& initialSubtitle) {
        instance_ = instance;
        initialPath_ = initialPath;
        initialSubtitle_ = initialSubtitle;
        menuBrush_ = CreateSolidBrush(kColorMenu);
        if (!RegisterWindowClasses()) {
            return false;
        }
        menu_ = CreateApplicationMenu();
        hwnd_ = CreateWindowExW(
            0, kMainClassName, L"MoviePlayer",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, 1280, 760,
            nullptr, menu_, instance_, this);
        if (!hwnd_) {
            return false;
        }
        ApplyDarkWindowTheme(hwnd_);
        DragAcceptFiles(hwnd_, TRUE);
        return true;
    }

    int Run(int showCommand) {
        ShowWindow(hwnd_, showCommand);
        UpdateWindow(hwnd_);
        if (!initialPath_.empty()) {
            PostMessageW(hwnd_, kOpenInitialFile, 0, 0);
        }

        const std::array<ACCEL, 13> accelerators = {{
            {FVIRTKEY | FCONTROL, 'O', ID_FILE_OPEN},
            {FVIRTKEY, VK_SPACE, ID_PLAY_PAUSE},
            {FVIRTKEY, VK_LEFT, ID_SEEK_BACK},
            {FVIRTKEY, VK_RIGHT, ID_SEEK_FORWARD},
            {FVIRTKEY, VK_UP, ID_VOLUME_UP},
            {FVIRTKEY, VK_DOWN, ID_VOLUME_DOWN},
            {FVIRTKEY, 'M', ID_MUTE},
            {FVIRTKEY, 'F', ID_FULLSCREEN},
            {FVIRTKEY, VK_F11, ID_FULLSCREEN},
            {FVIRTKEY, VK_RETURN, ID_FULLSCREEN},
            {FVIRTKEY | FALT, VK_RETURN, ID_FULLSCREEN},
            {FVIRTKEY, VK_ESCAPE, ID_EXIT_FULLSCREEN},
            {FVIRTKEY, 'L', ID_LOOP},
        }};
        HACCEL acceleratorTable = CreateAcceleratorTableW(
            const_cast<LPACCEL>(accelerators.data()),
            static_cast<int>(accelerators.size()));

        MSG message = {};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!TranslateAcceleratorW(hwnd_, acceleratorTable, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        DestroyAcceleratorTable(acceleratorTable);
        return static_cast<int>(message.wParam);
    }

private:
    static LRESULT CALLBACK MainWindowProc(HWND hwnd, UINT message,
                                           WPARAM wParam, LPARAM lParam) {
        MainWindow* self = reinterpret_cast<MainWindow*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<MainWindow*>(create->lpCreateParams);
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
        }
        return self ? self->HandleMessage(message, wParam, lParam)
                    : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    static LRESULT CALLBACK VideoWindowProc(HWND hwnd, UINT message,
                                            WPARAM wParam, LPARAM lParam) {
        MainWindow* self = reinterpret_cast<MainWindow*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<MainWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) {
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
        switch (message) {
        case WM_LBUTTONDBLCLK:
            self->ToggleFullscreen();
            return 0;
        case WM_MBUTTONDOWN:
            self->TogglePlayback();
            return 0;
        case WM_RBUTTONUP:
            self->ShowVideoContextMenu(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_MOUSEMOVE:
            self->UpdateFullscreenControlsFromCursor(false);
            return DefWindowProcW(hwnd, message, wParam, lParam);
        case WM_DROPFILES:
            self->HandleDrop(reinterpret_cast<HDROP>(wParam));
            return 0;
        case WM_ERASEBKGND:
            return 1;
        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
    }

    bool RegisterWindowClasses() {
        WNDCLASSEXW mainClass = {};
        mainClass.cbSize = sizeof(mainClass);
        mainClass.style = CS_HREDRAW | CS_VREDRAW;
        mainClass.lpfnWndProc = MainWindowProc;
        mainClass.hInstance = instance_;
        mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        mainClass.hIcon = static_cast<HICON>(LoadImageW(
            instance_, MAKEINTRESOURCEW(IDI_MOVIEPLAYER), IMAGE_ICON,
            0, 0, LR_DEFAULTSIZE));
        mainClass.hIconSm = static_cast<HICON>(LoadImageW(
            instance_, MAKEINTRESOURCEW(IDI_MOVIEPLAYER), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
        mainClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        mainClass.lpszClassName = kMainClassName;
        if (!RegisterClassExW(&mainClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        WNDCLASSEXW videoClass = {};
        videoClass.cbSize = sizeof(videoClass);
        videoClass.style = CS_DBLCLKS | CS_OWNDC;
        videoClass.lpfnWndProc = VideoWindowProc;
        videoClass.hInstance = instance_;
        videoClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        videoClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        videoClass.lpszClassName = kVideoClassName;
        if (!RegisterClassExW(&videoClass) &&
            GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        WNDCLASSEXW panelClass = {};
        panelClass.cbSize = sizeof(panelClass);
        panelClass.lpfnWndProc = ControlPanelWindowProc;
        panelClass.hInstance = instance_;
        panelClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        panelClass.lpszClassName = kControlPanelClassName;
        return RegisterClassExW(&panelClass) ||
               GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    void ApplyDarkMenuStyle(HMENU menu, bool menuBar) {
        if (!menu) {
            return;
        }
        MENUINFO menuInfo = {sizeof(menuInfo)};
        menuInfo.fMask = MIM_BACKGROUND;
        menuInfo.hbrBack = menuBrush_;
        SetMenuInfo(menu, &menuInfo);

        const int count = GetMenuItemCount(menu);
        for (int index = 0; index < count; ++index) {
            MENUITEMINFOW itemInfo = {sizeof(itemInfo)};
            itemInfo.fMask = MIIM_FTYPE | MIIM_SUBMENU;
            if (!GetMenuItemInfoW(menu, index, TRUE, &itemInfo)) {
                continue;
            }

            const int textLength = GetMenuStringW(menu, index, nullptr, 0,
                                                  MF_BYPOSITION);
            std::vector<wchar_t> textBuffer(static_cast<size_t>(textLength) + 1, L'\0');
            if (textLength > 0) {
                GetMenuStringW(menu, index, textBuffer.data(), textLength + 1,
                               MF_BYPOSITION);
            }

            if (itemInfo.hSubMenu) {
                ApplyDarkMenuStyle(itemInfo.hSubMenu, false);
            }

            auto darkItem = std::make_unique<DarkMenuItem>();
            darkItem->text = textBuffer.data();
            darkItem->separator = (itemInfo.fType & MFT_SEPARATOR) != 0;
            darkItem->menuBarItem = menuBar;
            darkItem->hasSubmenu = itemInfo.hSubMenu != nullptr;
            DarkMenuItem* storedItem = darkItem.get();
            darkMenuItems_.push_back(std::move(darkItem));

            MENUITEMINFOW updateInfo = {sizeof(updateInfo)};
            updateInfo.fMask = MIIM_FTYPE | MIIM_DATA;
            updateInfo.fType = MFT_OWNERDRAW | (itemInfo.fType & MFT_RADIOCHECK);
            updateInfo.dwItemData = reinterpret_cast<ULONG_PTR>(storedItem);
            SetMenuItemInfoW(menu, index, TRUE, &updateInfo);
        }
    }

    bool MeasureDarkMenuItem(MEASUREITEMSTRUCT* measure) {
        if (!measure || measure->CtlType != ODT_MENU || !measure->itemData) {
            return false;
        }
        auto* item = reinterpret_cast<DarkMenuItem*>(measure->itemData);
        const UINT dpi = hwnd_ ? GetDpiForWindow(hwnd_) : 96;
        if (item->separator) {
            measure->itemWidth = ScaleForDpi(80, dpi);
            measure->itemHeight = ScaleForDpi(8, dpi);
            return true;
        }

        std::wstring label = item->text;
        std::wstring shortcut;
        const size_t tab = label.find(L'\t');
        if (tab != std::wstring::npos) {
            shortcut = label.substr(tab + 1);
            label.resize(tab);
        }

        HDC dc = GetDC(hwnd_);
        HFONT drawingFont = menuFont_
                                ? menuFont_
                                : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ oldFont = SelectObject(dc, drawingFont);
        RECT labelBounds = {};
        RECT shortcutBounds = {};
        DrawTextW(dc, label.c_str(), -1, &labelBounds,
                  DT_CALCRECT | DT_SINGLELINE);
        if (!shortcut.empty()) {
            DrawTextW(dc, shortcut.c_str(), -1, &shortcutBounds,
                      DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        }
        SelectObject(dc, oldFont);
        ReleaseDC(hwnd_, dc);

        if (item->menuBarItem) {
            measure->itemWidth = static_cast<UINT>(labelBounds.right +
                                                   ScaleForDpi(20, dpi));
            measure->itemHeight = ScaleForDpi(24, dpi);
        } else {
            const int shortcutGap = shortcut.empty() ? 0 : ScaleForDpi(24, dpi);
            measure->itemWidth = static_cast<UINT>(
                labelBounds.right + shortcutBounds.right + shortcutGap +
                ScaleForDpi(58, dpi));
            measure->itemHeight = ScaleForDpi(26, dpi);
        }
        return true;
    }

    bool DrawDarkMenuItem(DRAWITEMSTRUCT* draw) {
        if (!draw || draw->CtlType != ODT_MENU || !draw->itemData) {
            return false;
        }
        auto* item = reinterpret_cast<DarkMenuItem*>(draw->itemData);
        const UINT dpi = hwnd_ ? GetDpiForWindow(hwnd_) : 96;
        RECT rect = draw->rcItem;
        const bool selected = (draw->itemState & (ODS_SELECTED | ODS_HOTLIGHT)) != 0;
        const bool disabled = (draw->itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
        FillRectColor(draw->hDC, rect, selected ? kColorMenuHot : kColorMenu);

        if (item->separator) {
            const int y = (rect.top + rect.bottom) / 2;
            DrawLine(draw->hDC, rect.left + ScaleForDpi(30, dpi), y,
                     rect.right - ScaleForDpi(10, dpi), y,
                     kColorMenuLine, 1);
            return true;
        }

        HFONT drawingFont = menuFont_
                                ? menuFont_
                                : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ oldFont = SelectObject(draw->hDC, drawingFont);
        SetBkMode(draw->hDC, TRANSPARENT);
        SetTextColor(draw->hDC, disabled ? RGB(102, 105, 113) : kColorText);
        UINT textFlags = DT_SINGLELINE | DT_VCENTER;
        if (draw->itemState & ODS_NOACCEL) {
            textFlags |= DT_HIDEPREFIX;
        }

        std::wstring label = item->text;
        std::wstring shortcut;
        const size_t tab = label.find(L'\t');
        if (tab != std::wstring::npos) {
            shortcut = label.substr(tab + 1);
            label.resize(tab);
        }

        if (item->menuBarItem) {
            RECT textRect = rect;
            InflateRect(&textRect, -ScaleForDpi(10, dpi), 0);
            const UINT direction = ContainsRightToLeftScript(label)
                                       ? DT_RTLREADING
                                       : 0;
            DrawTextW(draw->hDC, label.c_str(), -1, &textRect,
                      textFlags | DT_CENTER | direction);
        } else {
            const int checkWidth = ScaleForDpi(28, dpi);
            const int arrowWidth = ScaleForDpi(22, dpi);
            RECT labelRect = {rect.left + checkWidth, rect.top,
                              rect.right - arrowWidth, rect.bottom};
            const bool rightToLeft = ContainsRightToLeftScript(label);
            DrawTextW(draw->hDC, label.c_str(), -1, &labelRect,
                      textFlags | (rightToLeft ? DT_RIGHT | DT_RTLREADING
                                               : DT_LEFT));
            if (!shortcut.empty()) {
                RECT shortcutRect = labelRect;
                shortcutRect.right -= ScaleForDpi(8, dpi);
                DrawTextW(draw->hDC, shortcut.c_str(), -1, &shortcutRect,
                          DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX |
                              (rightToLeft ? DT_LEFT : DT_RIGHT));
            }

            const COLORREF glyphColor = disabled ? RGB(102, 105, 113) : kColorText;
            if (draw->itemState & ODS_CHECKED) {
                const int cx = rect.left + ScaleForDpi(14, dpi);
                const int cy = (rect.top + rect.bottom) / 2;
                DrawLine(draw->hDC, cx - ScaleForDpi(5, dpi), cy,
                         cx - ScaleForDpi(1, dpi), cy + ScaleForDpi(4, dpi),
                         kColorAccent, ScaleForDpi(2, dpi));
                DrawLine(draw->hDC, cx - ScaleForDpi(1, dpi), cy + ScaleForDpi(4, dpi),
                         cx + ScaleForDpi(6, dpi), cy - ScaleForDpi(5, dpi),
                         kColorAccent, ScaleForDpi(2, dpi));
            }
            if (item->hasSubmenu) {
                const int cx = rect.right - ScaleForDpi(11, dpi);
                const int cy = (rect.top + rect.bottom) / 2;
                DrawLine(draw->hDC, cx - ScaleForDpi(2, dpi), cy - ScaleForDpi(4, dpi),
                         cx + ScaleForDpi(2, dpi), cy, glyphColor, 1);
                DrawLine(draw->hDC, cx + ScaleForDpi(2, dpi), cy,
                         cx - ScaleForDpi(2, dpi), cy + ScaleForDpi(4, dpi),
                         glyphColor, 1);
            }
        }
        SelectObject(draw->hDC, oldFont);
        return true;
    }

    std::wstring FormatAudioTrackLabel(
        const movieplayer::codec::TrackInfo& track, std::size_t index) const {
        std::wstring label = std::to_wstring(index + 1) + L". ";
        const std::wstring name = Utf8ToWide(track.name);
        const std::wstring codec = track.codec == movieplayer::codec::CodecId::Mp3
                                       ? L"MP3"
                                       : L"AAC-LC";
        label += name.empty() ? codec : name + L" — " + codec;
        const std::wstring language = Utf8ToWide(track.language);
        if (!language.empty() && language != L"und") {
            label += L" [" + language + L"]";
        }
        if (track.sampleRate > 0) {
            label += L", " + std::to_wstring(track.sampleRate) + L" Hz";
        }
        if (track.channels > 0) {
            label += L", " + std::to_wstring(track.channels) + L" ch";
        }
        if (track.defaultTrack) {
            label += L" [" + Localization::Text("menu.track.default") + L"]";
        }
        return label;
    }

    std::wstring FormatSubtitleTrackLabel(
        const movieplayer::codec::TrackInfo& track, std::size_t index) const {
        std::wstring label = std::to_wstring(index + 1) + L". ";
        const std::wstring name = Utf8ToWide(track.name);
        const std::wstring codec =
            track.codec == movieplayer::codec::CodecId::Ass
                ? L"ASS"
                : track.codec == movieplayer::codec::CodecId::VobSub
                      ? L"VobSub"
                      : L"SRT/UTF-8";
        label += name.empty() ? codec : name + L" — " + codec;
        const std::wstring language = Utf8ToWide(track.language);
        if (!language.empty() && language != L"und") {
            label += L" [" + language + L"]";
        }
        if (track.defaultTrack) {
            label += L" [" + Localization::Text("menu.track.default") + L"]";
        }
        return label;
    }

    static void ClearMenuItems(HMENU menu) {
        if (!menu) return;
        while (GetMenuItemCount(menu) > 0) {
            DeleteMenu(menu, 0, MF_BYPOSITION);
        }
    }

    void PopulateTrackMenus(bool applyDarkStyle) {
        if (!audioTrackMenu_ || !subtitleTrackMenu_) return;
        ClearMenuItems(audioTrackMenu_);
        ClearMenuItems(subtitleTrackMenu_);

        const auto audioTracks = engine_.AudioTracks();
        const std::size_t audioCount =
            (std::min)(audioTracks.size(), kMaximumTrackMenuItems);
        if (audioCount == 0) {
            const std::wstring none = Localization::Text("menu.track.none");
            AppendMenuW(audioTrackMenu_, MF_STRING | MF_GRAYED, 0, none.c_str());
        } else {
            for (std::size_t i = 0; i < audioCount; ++i) {
                const std::wstring label = FormatAudioTrackLabel(audioTracks[i], i);
                AppendMenuW(audioTrackMenu_, MF_STRING,
                            kAudioTrackCommandBase + static_cast<UINT>(i),
                            label.c_str());
            }
        }

        const auto subtitleTracks = engine_.EmbeddedSubtitleTracks();
        const std::size_t subtitleCount =
            (std::min)(subtitleTracks.size(), kMaximumTrackMenuItems);
        if (subtitleCount == 0) {
            const std::wstring none = Localization::Text("menu.track.none");
            AppendMenuW(subtitleTrackMenu_, MF_STRING | MF_GRAYED, 0, none.c_str());
        } else {
            for (std::size_t i = 0; i < subtitleCount; ++i) {
                const std::wstring label =
                    FormatSubtitleTrackLabel(subtitleTracks[i], i);
                AppendMenuW(subtitleTrackMenu_, MF_STRING,
                            kSubtitleTrackCommandBase + static_cast<UINT>(i),
                            label.c_str());
            }
        }

        if (applyDarkStyle) {
            ApplyDarkMenuStyle(audioTrackMenu_, false);
            ApplyDarkMenuStyle(subtitleTrackMenu_, false);
        }
        menuStateValid_ = false;
    }

    void RebuildTrackMenus() {
        PopulateTrackMenus(true);
        if (hwnd_) DrawMenuBar(hwnd_);
    }

    HMENU CreateApplicationMenu() {
        const auto append = [](HMENU target, UINT flags, UINT_PTR id,
                               const char* key) {
            const std::wstring text = Localization::Text(key);
            AppendMenuW(target, flags, id, text.c_str());
        };
        HMENU root = CreateMenu();
        HMENU file = CreatePopupMenu();
        append(file, MF_STRING, ID_FILE_OPEN, "menu.file.open");
        append(file, MF_STRING, ID_FILE_CLOSE, "menu.file.close");
        AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
        append(file, MF_STRING, ID_FILE_ASSOCIATIONS,
               "menu.file.associations");
        AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
        append(file, MF_STRING, ID_FILE_EXIT, "menu.file.exit");
        append(root, MF_POPUP, reinterpret_cast<UINT_PTR>(file), "menu.file");

        HMENU playback = CreatePopupMenu();
        append(playback, MF_STRING, ID_PLAY_PAUSE, "menu.playback.play_pause");
        append(playback, MF_STRING, ID_STOP, "menu.playback.stop");
        AppendMenuW(playback, MF_SEPARATOR, 0, nullptr);
        append(playback, MF_STRING, ID_SEEK_BACK, "menu.playback.seek_back");
        append(playback, MF_STRING, ID_SEEK_FORWARD, "menu.playback.seek_forward");
        append(playback, MF_STRING, ID_STEP_FRAME, "menu.playback.next_frame");

        HMENU speedMenu = CreatePopupMenu();
        AppendMenuW(speedMenu, MF_STRING, ID_SPEED_050, L"0.50×");
        AppendMenuW(speedMenu, MF_STRING, ID_SPEED_075, L"0.75×");
        AppendMenuW(speedMenu, MF_STRING, ID_SPEED_100, L"1.00×");
        AppendMenuW(speedMenu, MF_STRING, ID_SPEED_125, L"1.25×");
        AppendMenuW(speedMenu, MF_STRING, ID_SPEED_150, L"1.50×");
        AppendMenuW(speedMenu, MF_STRING, ID_SPEED_200, L"2.00×");
        append(playback, MF_POPUP, reinterpret_cast<UINT_PTR>(speedMenu),
               "menu.playback.speed");
        AppendMenuW(playback, MF_SEPARATOR, 0, nullptr);
        append(playback, MF_STRING, ID_LOOP, "menu.playback.loop");
        append(playback, MF_STRING, ID_AUTO_PLAY_NEXT,
               "menu.playback.auto_next");
        append(root, MF_POPUP, reinterpret_cast<UINT_PTR>(playback),
               "menu.playback");

        HMENU audio = CreatePopupMenu();
        append(audio, MF_STRING, ID_MUTE, "menu.audio.mute");
        append(audio, MF_STRING, ID_VOLUME_UP, "menu.audio.volume_up");
        append(audio, MF_STRING, ID_VOLUME_DOWN, "menu.audio.volume_down");
        AppendMenuW(audio, MF_SEPARATOR, 0, nullptr);
        audioTrackMenu_ = CreatePopupMenu();
        append(audio, MF_POPUP, reinterpret_cast<UINT_PTR>(audioTrackMenu_),
               "menu.audio.tracks");
        append(root, MF_POPUP, reinterpret_cast<UINT_PTR>(audio), "menu.audio");

        HMENU view = CreatePopupMenu();
        append(view, MF_STRING, ID_FULLSCREEN, "menu.view.fullscreen");
        append(view, MF_STRING, ID_ALWAYS_ON_TOP, "menu.view.always_on_top");
        AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
        append(view, MF_STRING, ID_RTX_VIDEO_UPSCALING,
               "menu.view.rtx_video_upscaling");
        append(root, MF_POPUP, reinterpret_cast<UINT_PTR>(view), "menu.view");

        HMENU subtitle = CreatePopupMenu();
        append(subtitle, MF_STRING, ID_SUBTITLE_OPEN, "menu.subtitle.open");
        append(subtitle, MF_STRING, ID_SUBTITLE_AUTO, "menu.subtitle.auto_find");
        AppendMenuW(subtitle, MF_SEPARATOR, 0, nullptr);
        append(subtitle, MF_STRING, ID_SUBTITLE_WHISPER_GENERATE,
               "menu.subtitle.whisper_auto");
        append(subtitle, MF_STRING | MF_GRAYED, ID_SUBTITLE_WHISPER_CANCEL,
               "menu.subtitle.whisper_cancel");
        AppendMenuW(subtitle, MF_SEPARATOR, 0, nullptr);
        subtitleTrackMenu_ = CreatePopupMenu();
        append(subtitle, MF_POPUP,
               reinterpret_cast<UINT_PTR>(subtitleTrackMenu_),
               "menu.subtitle.tracks");
        AppendMenuW(subtitle, MF_SEPARATOR, 0, nullptr);
        append(subtitle, MF_STRING, ID_SUBTITLE_OFF, "menu.subtitle.off");
        append(root, MF_POPUP, reinterpret_cast<UINT_PTR>(subtitle),
               "menu.subtitle");

        HMENU language = CreatePopupMenu();
        append(language, MF_STRING, ID_LANGUAGE_EN, "language.english");
        append(language, MF_STRING, ID_LANGUAGE_JA, "language.japanese");
        append(language, MF_STRING, ID_LANGUAGE_KO, "language.korean");
        append(language, MF_STRING, ID_LANGUAGE_FR, "language.french");
        append(language, MF_STRING, ID_LANGUAGE_ZH_CN,
               "language.chinese_simplified");
        append(language, MF_STRING, ID_LANGUAGE_ZH_TW,
               "language.chinese_traditional");
        append(language, MF_STRING, ID_LANGUAGE_ES, "language.spanish");
        append(language, MF_STRING, ID_LANGUAGE_PT, "language.portuguese");
        append(language, MF_STRING, ID_LANGUAGE_HI, "language.hindi");
        append(language, MF_STRING, ID_LANGUAGE_ID, "language.indonesian");
        append(language, MF_STRING, ID_LANGUAGE_AR, "language.arabic");
        append(root, MF_POPUP, reinterpret_cast<UINT_PTR>(language),
               "menu.language");

        HMENU help = CreatePopupMenu();
        append(help, MF_STRING, ID_ABOUT, "menu.help.about");
        append(root, MF_POPUP, reinterpret_cast<UINT_PTR>(help), "menu.help");
        PopulateTrackMenus(false);
        ApplyDarkMenuStyle(root, true);
        return root;
    }

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE:
            return OnCreate() ? 0 : -1;
        case WM_SIZE:
            LayoutControls();
            InvalidateRect(hwnd_, nullptr, FALSE);
            renderer_.Resize();
            if (lastFrame_ && lastFrame_->texture) {
                renderer_.RenderFrame(*lastFrame_);
            } else {
                renderer_.Clear();
            }
            return 0;
        case WM_DPICHANGED: {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            RecreateFont();
            LayoutControls();
            DrawMenuBar(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            const UINT dpi = GetDpiForWindow(hwnd_);
            info->ptMinTrackSize.x = ScaleForDpi(680, dpi);
            info->ptMinTrackSize.y = ScaleForDpi(420, dpi);
            return 0;
        }
        case WM_COMMAND:
            ExecuteCommand(LOWORD(wParam));
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_RETURN) {
                ToggleFullscreen();
                return 0;
            }
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        case WM_MOUSEMOVE:
            UpdateFullscreenControlsFromCursor(false);
            return 0;
        case WM_MEASUREITEM:
            return MeasureDarkMenuItem(reinterpret_cast<MEASUREITEMSTRUCT*>(lParam))
                       ? TRUE
                       : DefWindowProcW(hwnd_, message, wParam, lParam);
        case WM_NOTIFY:
            return HandleNotify(reinterpret_cast<NMHDR*>(lParam));
        case WM_DRAWITEM: {
            auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (DrawDarkMenuItem(item) || DrawControlButton(item)) {
                return TRUE;
            }
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            HWND control = reinterpret_cast<HWND>(lParam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, control == timeLabel_ ? kColorText : kColorTextMuted);
            return reinterpret_cast<LRESULT>(panelBrush_);
        }
        case WM_CTLCOLORBTN:
            return reinterpret_cast<LRESULT>(panelBrush_);
        case WM_HSCROLL:
            HandleTrackbar(reinterpret_cast<HWND>(lParam), LOWORD(wParam));
            return 0;
        case WM_NCPAINT: {
            const LRESULT result = DefWindowProcW(hwnd_, message, wParam, lParam);
            DrawMainMenuBorder();
            return result;
        }
        case WM_NCACTIVATE: {
            const LRESULT result = DefWindowProcW(hwnd_, message, wParam, lParam);
            DrawMainMenuBorder();
            return result;
        }
        case WM_PAINT:
            OnPaint();
            return 0;
        case WM_TIMER:
            if (wParam == kPlaybackTimer) {
                PlaybackTick();
            }
            return 0;
        case WM_DROPFILES:
            HandleDrop(reinterpret_cast<HDROP>(wParam));
            return 0;
        case kOpenInitialFile:
            OpenPath(initialPath_);
            if (engine_.IsOpen() && !initialSubtitle_.empty()) {
                LoadSubtitle(initialSubtitle_, true);
            }
            initialPath_.clear();
            initialSubtitle_.clear();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_CLOSE:
            if (fullscreen_) {
                ToggleFullscreen();
            }
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd_, kPlaybackTimer);
            whisperJob_.Cancel();
            if (whisperInstallerProcess_) {
                CloseHandle(whisperInstallerProcess_);
                whisperInstallerProcess_ = nullptr;
            }
            engine_.Close();
            if (font_) {
                DeleteObject(font_);
                font_ = nullptr;
            }
            if (menuFont_) {
                DeleteObject(menuFont_);
                menuFont_ = nullptr;
            }
            if (panelBrush_) {
                DeleteObject(panelBrush_);
                panelBrush_ = nullptr;
            }
            if (windowBrush_) {
                DeleteObject(windowBrush_);
                windowBrush_ = nullptr;
            }
            if (menuBrush_) {
                DeleteObject(menuBrush_);
                menuBrush_ = nullptr;
            }
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    bool OnCreate() {
        if (!windowBrush_) {
            windowBrush_ = CreateSolidBrush(kColorWindow);
        }
        if (!panelBrush_) {
            panelBrush_ = CreateSolidBrush(kColorPanel);
        }
        ApplyDarkWindowTheme(hwnd_);

        videoHwnd_ = CreateWindowExW(
            WS_EX_NOPARENTNOTIFY, kVideoClassName, nullptr,
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            0, 0, 100, 100, hwnd_, nullptr, instance_, this);
        if (!videoHwnd_) {
            return false;
        }
        DragAcceptFiles(videoHwnd_, TRUE);

        controlPanelHwnd_ = CreateWindowExW(
            0, kControlPanelClassName, nullptr,
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
            0, 0, 100, 100, hwnd_, nullptr, instance_, nullptr);
        if (!controlPanelHwnd_) {
            return false;
        }

        openButton_ = CreateControl(L"BUTTON", Localization::Text("button.open").c_str(),
                                    ID_CONTROL_OPEN, BS_OWNERDRAW);
        playButton_ = CreateControl(L"BUTTON", Localization::Text("button.play").c_str(),
                                    ID_CONTROL_PLAY, BS_OWNERDRAW);
        stopButton_ = CreateControl(L"BUTTON", Localization::Text("button.stop").c_str(),
                                    ID_CONTROL_STOP, BS_OWNERDRAW);
        muteButton_ = CreateControl(L"BUTTON", Localization::Text("button.mute").c_str(),
                                    ID_CONTROL_MUTE, BS_OWNERDRAW);
        speedButton_ = CreateControl(L"BUTTON", L"1.00×", ID_CONTROL_SPEED, BS_OWNERDRAW);
        fullscreenButton_ = CreateControl(L"BUTTON",
                                          Localization::Text("button.fullscreen").c_str(),
                                          ID_CONTROL_FULLSCREEN,
                                          BS_OWNERDRAW);
        timeLabel_ = CreateControl(L"STATIC", L"00:00 / 00:00", ID_CONTROL_TIME,
                                   SS_CENTER | SS_CENTERIMAGE);
        statusLabel_ = CreateControl(L"STATIC",
                                     Localization::Text("status.idle").c_str(),
                                     ID_CONTROL_STATUS,
                                     SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS);

        seekBar_ = CreateWindowExW(
            0, TRACKBAR_CLASSW, nullptr,
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
            0, 0, 100, 20, hwnd_, reinterpret_cast<HMENU>(ID_CONTROL_SEEK),
            instance_, nullptr);
        volumeBar_ = CreateWindowExW(
            0, TRACKBAR_CLASSW, nullptr,
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_NOTICKS,
            0, 0, 100, 20, hwnd_, reinterpret_cast<HMENU>(ID_CONTROL_VOLUME),
            instance_, nullptr);
        SendMessageW(seekBar_, TBM_SETRANGE, TRUE, MAKELPARAM(0, kSeekRange));
        SendMessageW(volumeBar_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageW(volumeBar_, TBM_SETPOS, TRUE, 80);
        SetWindowTheme(seekBar_, L"", nullptr);
        SetWindowTheme(volumeBar_, L"", nullptr);
        SetWindowSubclass(seekBar_, SliderSubclassProc, 1, 0);
        SetWindowSubclass(volumeBar_, SliderSubclassProc, 2, 0);
        engine_.SetVolume(0.8f);

        RecreateFont();
        RefreshLocalizedUi();
        if (!renderer_.Initialize(videoHwnd_)) {
            const std::wstring message = Localization::Format(
                "error.renderer_init", {{L"error", renderer_.LastError()}});
            MessageBoxW(hwnd_, message.c_str(),
                        L"MoviePlayer", MB_OK | MB_ICONERROR);
            return false;
        }
        renderer_.Clear();
        SetTimer(hwnd_, kPlaybackTimer, 10, nullptr);
        LayoutControls();
        UpdateMenuChecks();
        return true;
    }

    HWND CreateControl(const wchar_t* className, const wchar_t* text,
                       int id, DWORD controlStyle) {
        return CreateWindowExW(
            0, className, text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | controlStyle,
            0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            instance_, nullptr);
    }

    void RecreateFont() {
        if (font_) {
            DeleteObject(font_);
        }
        if (menuFont_) {
            DeleteObject(menuFont_);
        }
        const UINT dpi = hwnd_ ? GetDpiForWindow(hwnd_) : 96;
        const bool preferMsGothic = PreferMsGothicForJapaneseUi();
        const wchar_t* controlFontFamily =
            _wcsicmp(Localization::CurrentLanguage().c_str(), L"hi") == 0
                ? L"Nirmala UI"
                : L"Segoe UI";
        const wchar_t* menuFontFamily =
            preferMsGothic ? L"MS Gothic" : controlFontFamily;
        renderer_.SetSubtitleFontFamily(preferMsGothic ? L"MS Gothic" : L"");
        font_ = CreateFontW(-ScaleForDpi(12, dpi), 0, 0, 0, FW_NORMAL,
                            FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                            controlFontFamily);
        menuFont_ = CreateFontW(-ScaleForDpi(12, dpi), 0, 0, 0, FW_NORMAL,
                                FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                menuFontFamily);
        const std::array<HWND, 10> controls = {
            openButton_, playButton_, stopButton_, muteButton_, speedButton_,
            fullscreenButton_, timeLabel_, statusLabel_, seekBar_, volumeBar_};
        for (HWND control : controls) {
            if (control) {
                SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            }
        }
    }

    void BringControlsToTop() {
        const std::array<HWND, 10> controls = {
            openButton_, playButton_, stopButton_, muteButton_, speedButton_,
            fullscreenButton_, timeLabel_, statusLabel_, seekBar_, volumeBar_};
        for (HWND control : controls) {
            if (control) {
                SetWindowPos(control, HWND_TOP, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }
    }

    void LayoutControls() {
        if (!videoHwnd_) {
            return;
        }
        RECT client = {};
        GetClientRect(hwnd_, &client);
        const int width = client.right;
        const int height = client.bottom;
        const UINT dpi = GetDpiForWindow(hwnd_);
        const bool controlsVisible = !fullscreen_ || fullscreenControlsVisible_;
        const int panelHeight =
            controlsVisible ? ScaleForDpi(kControlPanelHeight, dpi) : 0;
        const int margin = ScaleForDpi(14, dpi);

        const int videoHeight = fullscreen_ ? height : height - panelHeight;
        SetWindowPos(videoHwnd_, HWND_BOTTOM, 0, 0, width,
                     std::max(1, videoHeight), SWP_NOACTIVATE);
        if (!controlsVisible) {
            SetControlVisibility(false);
            return;
        }

        const int panelTop = height - panelHeight;
        SetWindowPos(controlPanelHwnd_, HWND_TOP, 0, panelTop, width,
                     panelHeight, SWP_NOACTIVATE | SWP_SHOWWINDOW);

        MoveWindow(seekBar_, margin, panelTop + ScaleForDpi(4, dpi),
                    std::max(1, width - margin * 2), ScaleForDpi(15, dpi), TRUE);

        const int rowY = panelTop + ScaleForDpi(22, dpi);
        const int rowHeight = ScaleForDpi(32, dpi);
        const int gap = ScaleForDpi(6, dpi);
        const int openWidth = ScaleForDpi(34, dpi);
        const int stopWidth = ScaleForDpi(34, dpi);
        const int playWidth = ScaleForDpi(42, dpi);
        const int muteWidth = ScaleForDpi(34, dpi);
        const int speedWidth = ScaleForDpi(56, dpi);
        const int fullscreenWidth = ScaleForDpi(34, dpi);
        const int groupWidth = openWidth + stopWidth + playWidth + muteWidth +
                               speedWidth + fullscreenWidth + gap * 5;
        const int groupX = std::max(margin, (width - groupWidth) / 2);

        const int timeWidth = ScaleForDpi(132, dpi);
        const int statusX = margin + timeWidth + ScaleForDpi(8, dpi);
        const int statusRight = std::max(statusX + 1, groupX - gap * 2);
        MoveWindow(timeLabel_, margin, rowY, timeWidth, rowHeight, TRUE);
        MoveWindow(statusLabel_, statusX, rowY,
                   std::max(1, statusRight - statusX), rowHeight, TRUE);

        int x = groupX;
        MoveWindow(openButton_, x, rowY, openWidth, rowHeight, TRUE);
        x += openWidth + gap;
        MoveWindow(stopButton_, x, rowY, stopWidth, rowHeight, TRUE);
        x += stopWidth + gap;
        MoveWindow(playButton_, x, rowY - ScaleForDpi(3, dpi),
                   playWidth, rowHeight + ScaleForDpi(6, dpi), TRUE);
        x += playWidth + gap;
        MoveWindow(muteButton_, x, rowY, muteWidth, rowHeight, TRUE);
        x += muteWidth + gap;
        MoveWindow(speedButton_, x, rowY, speedWidth, rowHeight, TRUE);
        x += speedWidth + gap;
        MoveWindow(fullscreenButton_, x, rowY, fullscreenWidth, rowHeight, TRUE);

        const int volumeWidth = ScaleForDpi(96, dpi);
        const int volumeX = std::max(x + fullscreenWidth + gap,
                                     width - margin - volumeWidth);
        MoveWindow(volumeBar_, volumeX, rowY + ScaleForDpi(8, dpi),
                   std::max(1, width - volumeX - margin),
                   ScaleForDpi(16, dpi), TRUE);
        BringControlsToTop();
        SetControlVisibility(true);
        InvalidateRect(controlPanelHwnd_, nullptr, FALSE);
    }

    void SetControlVisibility(bool visible) {
        const int command = visible ? SW_SHOW : SW_HIDE;
        const std::array<HWND, 11> controls = {
            controlPanelHwnd_, openButton_, playButton_, stopButton_, muteButton_,
            speedButton_, fullscreenButton_, timeLabel_, statusLabel_, seekBar_,
            volumeBar_};
        for (HWND control : controls) {
            if (control && (IsWindowVisible(control) != FALSE) != visible) {
                ShowWindow(control, command);
            }
        }
    }

    void OnPaint() {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(hwnd_, &paint);
        RECT client = {};
        GetClientRect(hwnd_, &client);
        FillRectColor(dc, client, kColorWindow);
        if (!fullscreen_) {
            const UINT dpi = GetDpiForWindow(hwnd_);
            const int panelHeight = ScaleForDpi(kControlPanelHeight, dpi);
            RECT panel = {0, std::max<LONG>(0, client.bottom - panelHeight),
                          client.right, client.bottom};
            FillRectColor(dc, panel, kColorPanel);
            RECT topLine = {panel.left, panel.top, panel.right,
                            panel.top + ScaleForDpi(1, dpi)};
            FillRectColor(dc, topLine, kColorPanelTop);
        }
        EndPaint(hwnd_, &paint);
    }

    void DrawMainMenuBorder() {
        if (!hwnd_ || fullscreen_ || !GetMenu(hwnd_)) {
            return;
        }
        MENUBARINFO menuBar = {sizeof(menuBar)};
        RECT window = {};
        if (!GetMenuBarInfo(hwnd_, OBJID_MENU, 0, &menuBar) ||
            !GetWindowRect(hwnd_, &window)) {
            return;
        }
        HDC dc = GetWindowDC(hwnd_);
        if (!dc) {
            return;
        }
        const int left = menuBar.rcBar.left - window.left;
        const int right = menuBar.rcBar.right - window.left;
        const int y = menuBar.rcBar.bottom - window.top;
        DrawLine(dc, left, y, right, y, kColorMenuBorder, 1);
        ReleaseDC(hwnd_, dc);
    }

    bool DrawControlButton(DRAWITEMSTRUCT* item) {
        if (!item || item->CtlType != ODT_BUTTON) {
            return false;
        }

        const std::array<UINT, 6> buttonIds = {
            ID_CONTROL_OPEN, ID_CONTROL_STOP, ID_CONTROL_PLAY,
            ID_CONTROL_MUTE, ID_CONTROL_SPEED, ID_CONTROL_FULLSCREEN};
        if (std::find(buttonIds.begin(), buttonIds.end(), item->CtlID) ==
            buttonIds.end()) {
            return false;
        }

        const UINT dpi = GetDpiForWindow(hwnd_);
        RECT rect = item->rcItem;
        FillRectColor(item->hDC, rect, kColorPanel);
        const bool pressed = (item->itemState & ODS_SELECTED) != 0;
        const bool disabled = (item->itemState & ODS_DISABLED) != 0;
        const bool primary = item->CtlID == ID_CONTROL_PLAY;
        const COLORREF fill = pressed ? kColorButtonDown : kColorButton;
        const COLORREF iconColor = disabled ? RGB(92, 94, 102) : kColorText;

        if (primary) {
            const int size = std::min(rect.right - rect.left, rect.bottom - rect.top) -
                             ScaleForDpi(3, dpi);
            RECT circle = {
                rect.left + ((rect.right - rect.left) - size) / 2,
                rect.top + ((rect.bottom - rect.top) - size) / 2,
                rect.left + ((rect.right - rect.left) + size) / 2,
                rect.top + ((rect.bottom - rect.top) + size) / 2,
            };
            HBRUSH brush = CreateSolidBrush(fill);
            HPEN pen = CreatePen(PS_SOLID, ScaleForDpi(2, dpi), kColorAccent);
            HGDIOBJ oldBrush = SelectObject(item->hDC, brush);
            HGDIOBJ oldPen = SelectObject(item->hDC, pen);
            Ellipse(item->hDC, circle.left, circle.top, circle.right, circle.bottom);
            SelectObject(item->hDC, oldBrush);
            SelectObject(item->hDC, oldPen);
            DeleteObject(pen);
            DeleteObject(brush);
        } else {
            RECT surface = rect;
            InflateRect(&surface, -ScaleForDpi(1, dpi), -ScaleForDpi(1, dpi));
            DrawRoundRect(item->hDC, surface, ScaleForDpi(6, dpi), fill,
                          kColorButtonBorder);
        }

        SetBkMode(item->hDC, TRANSPARENT);
        switch (item->CtlID) {
        case ID_CONTROL_OPEN:
            DrawOpenGlyph(item->hDC, rect, iconColor, dpi);
            break;
        case ID_CONTROL_STOP:
            DrawStopGlyph(item->hDC, rect, iconColor, dpi);
            break;
        case ID_CONTROL_PLAY: {
            if (engine_.IsOpen() && !engine_.IsPaused() && !engine_.IsEnded()) {
                DrawPauseGlyph(item->hDC, rect, iconColor, dpi);
            } else {
                DrawPlayGlyph(item->hDC, rect, iconColor, dpi);
            }
            break;
        }
        case ID_CONTROL_MUTE: {
            DrawSpeakerGlyph(item->hDC, rect, iconColor, engine_.IsMuted(), dpi);
            break;
        }
        case ID_CONTROL_SPEED:
            DrawSpeedLabel(item->hDC, item->hwndItem, rect, dpi);
            break;
        case ID_CONTROL_FULLSCREEN:
            DrawFullscreenGlyph(item->hDC, rect, iconColor, dpi);
            break;
        default:
            break;
        }
        return true;
    }

    void DrawOpenGlyph(HDC dc, const RECT& rect, COLORREF color, UINT dpi) {
        const int cx = (rect.left + rect.right) / 2;
        const int cy = (rect.top + rect.bottom) / 2;
        const int w = ScaleForDpi(18, dpi);
        const int h = ScaleForDpi(12, dpi);
        POINT folder[] = {
            {cx - w / 2, cy - h / 2 + ScaleForDpi(3, dpi)},
            {cx - ScaleForDpi(5, dpi), cy - h / 2 + ScaleForDpi(3, dpi)},
            {cx - ScaleForDpi(2, dpi), cy - h / 2},
            {cx + ScaleForDpi(6, dpi), cy - h / 2},
            {cx + ScaleForDpi(8, dpi), cy - h / 2 + ScaleForDpi(3, dpi)},
            {cx + w / 2, cy - h / 2 + ScaleForDpi(3, dpi)},
            {cx + w / 2 - ScaleForDpi(2, dpi), cy + h / 2},
            {cx - w / 2, cy + h / 2},
        };
        DrawFilledPolygon(dc, folder, static_cast<int>(std::size(folder)), color);
    }

    void DrawPlayGlyph(HDC dc, const RECT& rect, COLORREF color, UINT dpi) {
        const int cx = (rect.left + rect.right) / 2 + ScaleForDpi(2, dpi);
        const int cy = (rect.top + rect.bottom) / 2;
        const int size = ScaleForDpi(16, dpi);
        POINT triangle[] = {
            {cx - size / 3, cy - size / 2},
            {cx - size / 3, cy + size / 2},
            {cx + size / 2, cy},
        };
        DrawFilledPolygon(dc, triangle, static_cast<int>(std::size(triangle)), color);
    }

    void DrawPauseGlyph(HDC dc, const RECT& rect, COLORREF color, UINT dpi) {
        const int cx = (rect.left + rect.right) / 2;
        const int cy = (rect.top + rect.bottom) / 2;
        const int barWidth = ScaleForDpi(4, dpi);
        const int barHeight = ScaleForDpi(17, dpi);
        const int gap = ScaleForDpi(4, dpi);
        RECT left = {cx - gap - barWidth, cy - barHeight / 2,
                     cx - gap, cy + barHeight / 2};
        RECT right = {cx + gap, cy - barHeight / 2,
                      cx + gap + barWidth, cy + barHeight / 2};
        FillRectColor(dc, left, color);
        FillRectColor(dc, right, color);
    }

    void DrawStopGlyph(HDC dc, const RECT& rect, COLORREF color, UINT dpi) {
        const int size = ScaleForDpi(11, dpi);
        const int cx = (rect.left + rect.right) / 2;
        const int cy = (rect.top + rect.bottom) / 2;
        RECT square = {cx - size / 2, cy - size / 2,
                       cx + size / 2, cy + size / 2};
        FillRectColor(dc, square, color);
    }

    void DrawSpeakerGlyph(HDC dc, const RECT& rect, COLORREF color,
                          bool muted, UINT dpi) {
        const int cx = (rect.left + rect.right) / 2;
        const int cy = (rect.top + rect.bottom) / 2;
        POINT speaker[] = {
            {cx - ScaleForDpi(10, dpi), cy - ScaleForDpi(4, dpi)},
            {cx - ScaleForDpi(5, dpi), cy - ScaleForDpi(4, dpi)},
            {cx + ScaleForDpi(1, dpi), cy - ScaleForDpi(9, dpi)},
            {cx + ScaleForDpi(1, dpi), cy + ScaleForDpi(9, dpi)},
            {cx - ScaleForDpi(5, dpi), cy + ScaleForDpi(4, dpi)},
            {cx - ScaleForDpi(10, dpi), cy + ScaleForDpi(4, dpi)},
        };
        DrawFilledPolygon(dc, speaker, static_cast<int>(std::size(speaker)), color);
        if (muted) {
            DrawLine(dc, cx + ScaleForDpi(6, dpi), cy - ScaleForDpi(8, dpi),
                     cx + ScaleForDpi(15, dpi), cy + ScaleForDpi(8, dpi),
                     kColorAccent, ScaleForDpi(2, dpi));
            DrawLine(dc, cx + ScaleForDpi(15, dpi), cy - ScaleForDpi(8, dpi),
                     cx + ScaleForDpi(6, dpi), cy + ScaleForDpi(8, dpi),
                     kColorAccent, ScaleForDpi(2, dpi));
            return;
        }
        DrawLine(dc, cx + ScaleForDpi(6, dpi), cy - ScaleForDpi(6, dpi),
                 cx + ScaleForDpi(11, dpi), cy - ScaleForDpi(2, dpi),
                 color, ScaleForDpi(2, dpi));
        DrawLine(dc, cx + ScaleForDpi(11, dpi), cy - ScaleForDpi(2, dpi),
                 cx + ScaleForDpi(11, dpi), cy + ScaleForDpi(2, dpi),
                 color, ScaleForDpi(2, dpi));
        DrawLine(dc, cx + ScaleForDpi(11, dpi), cy + ScaleForDpi(2, dpi),
                 cx + ScaleForDpi(6, dpi), cy + ScaleForDpi(6, dpi),
                 color, ScaleForDpi(2, dpi));
    }

    void DrawSpeedLabel(HDC dc, HWND hwnd, const RECT& rect, UINT dpi) {
        wchar_t text[16] = {};
        GetWindowTextW(hwnd, text, static_cast<int>(std::size(text)));
        HFONT previousFont = reinterpret_cast<HFONT>(SelectObject(dc, font_));
        SetTextColor(dc, kColorText);
        SetBkMode(dc, TRANSPARENT);
        RECT textRect = rect;
        InflateRect(&textRect, -ScaleForDpi(6, dpi), 0);
        DrawTextW(dc, text, -1, &textRect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, previousFont);
    }

    void DrawFullscreenGlyph(HDC dc, const RECT& rect, COLORREF color, UINT dpi) {
        const int cx = (rect.left + rect.right) / 2;
        const int cy = (rect.top + rect.bottom) / 2;
        const int half = ScaleForDpi(9, dpi);
        const int corner = ScaleForDpi(6, dpi);
        const int left = cx - half;
        const int right = cx + half;
        const int top = cy - half;
        const int bottom = cy + half;
        const int width = ScaleForDpi(2, dpi);
        DrawLine(dc, left, top + corner, left, top, color, width);
        DrawLine(dc, left, top, left + corner, top, color, width);
        DrawLine(dc, right - corner, top, right, top, color, width);
        DrawLine(dc, right, top, right, top + corner, color, width);
        DrawLine(dc, left, bottom - corner, left, bottom, color, width);
        DrawLine(dc, left, bottom, left + corner, bottom, color, width);
        DrawLine(dc, right - corner, bottom, right, bottom, color, width);
        DrawLine(dc, right, bottom - corner, right, bottom, color, width);
    }

    LRESULT HandleNotify(NMHDR* header) {
        if (header && header->code == NM_CUSTOMDRAW &&
            (header->hwndFrom == seekBar_ || header->hwndFrom == volumeBar_)) {
            return DrawTrackbar(reinterpret_cast<NMCUSTOMDRAW*>(header));
        }
        return 0;
    }

    LRESULT DrawTrackbar(NMCUSTOMDRAW* draw) {
        HWND source = draw->hdr.hwndFrom;
        const UINT dpi = GetDpiForWindow(hwnd_);
        if (draw->dwDrawStage == CDDS_PREPAINT) {
            RECT bounds = {};
            GetClientRect(source, &bounds);
            FillRectColor(draw->hdc, bounds, kColorPanel);
            return CDRF_NOTIFYITEMDRAW;
        }
        if (draw->dwDrawStage != CDDS_ITEMPREPAINT) {
            return CDRF_DODEFAULT;
        }
        if (draw->dwItemSpec == TBCD_TICS) {
            return CDRF_SKIPDEFAULT;
        }
        if (draw->dwItemSpec == TBCD_CHANNEL) {
            RECT channel = {};
            SendMessageW(source, TBM_GETCHANNELRECT, 0,
                         reinterpret_cast<LPARAM>(&channel));
            const int centerY = (channel.top + channel.bottom) / 2;
            const int thickness = ScaleForDpi(3, dpi);
            RECT track = {channel.left, centerY - thickness / 2,
                          channel.right, centerY + thickness / 2};
            DrawRoundRect(draw->hdc, track, thickness, kColorTrack, kColorTrack);

            const int minPos = static_cast<int>(SendMessageW(source, TBM_GETRANGEMIN, 0, 0));
            const int maxPos = static_cast<int>(SendMessageW(source, TBM_GETRANGEMAX, 0, 0));
            const int pos = static_cast<int>(SendMessageW(source, TBM_GETPOS, 0, 0));
            const int span = std::max(1, maxPos - minPos);
            const int fillRight = track.left +
                MulDiv(track.right - track.left, pos - minPos, span);
            if (fillRight > track.left) {
                RECT filled = track;
                filled.right = std::max<LONG>(filled.left + thickness, fillRight);
                DrawRoundRect(draw->hdc, filled, thickness,
                              source == seekBar_ ? kColorAccent : kColorAccentAlt,
                              source == seekBar_ ? kColorAccent : kColorAccentAlt);
            }
            return CDRF_SKIPDEFAULT;
        }
        if (draw->dwItemSpec == TBCD_THUMB) {
            RECT thumb = {};
            SendMessageW(source, TBM_GETTHUMBRECT, 0,
                         reinterpret_cast<LPARAM>(&thumb));
            const int size = source == seekBar_ ? ScaleForDpi(10, dpi)
                                                : ScaleForDpi(9, dpi);
            const int cx = (thumb.left + thumb.right) / 2;
            const int cy = (thumb.top + thumb.bottom) / 2;
            RECT knob = {cx - size / 2, cy - size / 2,
                         cx + size / 2, cy + size / 2};
            HBRUSH brush = CreateSolidBrush(kColorText);
            HPEN pen = CreatePen(PS_SOLID, ScaleForDpi(2, dpi),
                                 source == seekBar_ ? kColorAccent : kColorAccentAlt);
            HGDIOBJ oldBrush = SelectObject(draw->hdc, brush);
            HGDIOBJ oldPen = SelectObject(draw->hdc, pen);
            Ellipse(draw->hdc, knob.left, knob.top, knob.right, knob.bottom);
            SelectObject(draw->hdc, oldBrush);
            SelectObject(draw->hdc, oldPen);
            DeleteObject(pen);
            DeleteObject(brush);
            return CDRF_SKIPDEFAULT;
        }
        return CDRF_DODEFAULT;
    }

    int CurrentLanguageCommand() const {
        const std::wstring code = Localization::CurrentLanguage();
        if (_wcsicmp(code.c_str(), L"ja") == 0) return ID_LANGUAGE_JA;
        if (_wcsicmp(code.c_str(), L"ko") == 0) return ID_LANGUAGE_KO;
        if (_wcsicmp(code.c_str(), L"fr") == 0) return ID_LANGUAGE_FR;
        if (_wcsicmp(code.c_str(), L"zh-CN") == 0) return ID_LANGUAGE_ZH_CN;
        if (_wcsicmp(code.c_str(), L"zh-TW") == 0) return ID_LANGUAGE_ZH_TW;
        if (_wcsicmp(code.c_str(), L"es") == 0) return ID_LANGUAGE_ES;
        if (_wcsicmp(code.c_str(), L"pt") == 0) return ID_LANGUAGE_PT;
        if (_wcsicmp(code.c_str(), L"hi") == 0) return ID_LANGUAGE_HI;
        if (_wcsicmp(code.c_str(), L"id") == 0) return ID_LANGUAGE_ID;
        if (_wcsicmp(code.c_str(), L"ar") == 0) return ID_LANGUAGE_AR;
        return ID_LANGUAGE_EN;
    }

    void RefreshLocalizedUi() {
        LONG_PTR statusStyle = GetWindowLongPtrW(statusLabel_, GWL_EXSTYLE);
        if (_wcsicmp(Localization::CurrentLanguage().c_str(), L"ar") == 0) {
            statusStyle |= WS_EX_RTLREADING | WS_EX_RIGHT;
        } else {
            statusStyle &= ~(static_cast<LONG_PTR>(WS_EX_RTLREADING) |
                             static_cast<LONG_PTR>(WS_EX_RIGHT));
        }
        SetWindowLongPtrW(statusLabel_, GWL_EXSTYLE, statusStyle);
        SetWindowTextW(openButton_, Localization::Text("button.open").c_str());
        SetWindowTextW(stopButton_, Localization::Text("button.stop").c_str());
        SetWindowTextW(fullscreenButton_,
                       Localization::Text("button.fullscreen").c_str());
        SetWindowTextW(playButton_, Localization::Text(
            engine_.IsOpen() && !engine_.IsPaused() ? "button.pause" : "button.play").c_str());
        SetWindowTextW(muteButton_, Localization::Text(
            engine_.IsMuted() ? "button.unmute" : "button.mute").c_str());
        if (engine_.IsOpen()) {
            UpdateControls(true);
        } else {
            SetWindowTextW(statusLabel_, Localization::Text("status.idle").c_str());
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void SetUiLanguage(const wchar_t* code) {
        if (!code || _wcsicmp(Localization::CurrentLanguage().c_str(), code) == 0) {
            return;
        }
        std::wstring error;
        if (!Localization::SetLanguage(code, error)) {
            const std::wstring message = Localization::Format(
                "error.language_load", {{L"error", error}});
            MessageBoxW(hwnd_, message.c_str(),
                        Localization::Text("title.language_error").c_str(),
                        MB_OK | MB_ICONERROR);
            return;
        }

        RecreateFont();

        HMENU oldMenu = menu_;
        if (!fullscreen_) {
            SetMenu(hwnd_, nullptr);
        }
        menu_ = nullptr;
        if (oldMenu) {
            DestroyMenu(oldMenu);
        }
        darkMenuItems_.clear();
        menu_ = CreateApplicationMenu();
        menuStateValid_ = false;
        if (!fullscreen_) {
            SetMenu(hwnd_, menu_);
        }
        if (engine_.IsOpen() && !currentPath_.empty()) {
            TryAutoLoadSubtitle(currentPath_, false);
        }
        RefreshLocalizedUi();
        UpdateMenuChecks();
        RedrawLastFrame();
    }

    void ExecuteCommand(int command) {
        if (command >= static_cast<int>(kAudioTrackCommandBase) &&
            command < static_cast<int>(kAudioTrackCommandBase +
                                       kMaximumTrackMenuItems)) {
            const auto tracks = engine_.AudioTracks();
            const std::size_t index = static_cast<std::size_t>(
                command - static_cast<int>(kAudioTrackCommandBase));
            if (index < tracks.size()) {
                if (!engine_.SelectAudioTrack(tracks[index].trackId)) {
                    MessageBoxW(hwnd_, engine_.LastError().c_str(),
                                Localization::Text("title.playback_error").c_str(),
                                MB_OK | MB_ICONERROR);
                } else {
                    endedHandled_ = false;
                }
            }
            menuStateValid_ = false;
            UpdateControls(true);
            UpdateMenuChecks();
            return;
        }
        if (command >= static_cast<int>(kSubtitleTrackCommandBase) &&
            command < static_cast<int>(kSubtitleTrackCommandBase +
                                       kMaximumTrackMenuItems)) {
            const auto tracks = engine_.EmbeddedSubtitleTracks();
            const std::size_t index = static_cast<std::size_t>(
                command - static_cast<int>(kSubtitleTrackCommandBase));
            if (index < tracks.size()) {
                if (!engine_.SelectEmbeddedSubtitleTrack(tracks[index].trackId)) {
                    MessageBoxW(hwnd_, engine_.LastError().c_str(),
                                Localization::Text("title.playback_error").c_str(),
                                MB_OK | MB_ICONERROR);
                } else {
                    subtitleTrack_.Clear();
                    subtitleEnabled_ = true;
                    displayedSubtitle_.clear();
                    displayedSubtitleBitmap_.reset();
                    UpdateSubtitleText(true);
                    endedHandled_ = false;
                }
            }
            menuStateValid_ = false;
            UpdateControls(true);
            UpdateMenuChecks();
            return;
        }

        switch (command) {
        case ID_FILE_OPEN:
        case ID_CONTROL_OPEN:
            ShowOpenDialog();
            break;
        case ID_FILE_CLOSE:
            CloseMedia();
            break;
        case ID_FILE_ASSOCIATIONS: {
            std::wstring error;
            if (!RegisterVideoFileAssociations(error) ||
                !OpenVideoDefaultAppsSettings(hwnd_, error)) {
                MessageBoxW(hwnd_, error.c_str(),
                            Localization::Text("title.file_association_error").c_str(),
                            MB_OK | MB_ICONERROR);
            }
            break;
        }
        case ID_FILE_EXIT:
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            break;
        case ID_PLAY_PAUSE:
        case ID_CONTROL_PLAY:
            TogglePlayback();
            break;
        case ID_STOP:
        case ID_CONTROL_STOP:
            if (engine_.IsOpen()) {
                endedHandled_ = false;
                engine_.Stop();
            }
            break;
        case ID_SEEK_BACK:
            SeekRelative(-10.0);
            break;
        case ID_SEEK_FORWARD:
            SeekRelative(10.0);
            break;
        case ID_STEP_FRAME:
            engine_.StepFrame();
            break;
        case ID_MUTE:
        case ID_CONTROL_MUTE:
            engine_.SetMuted(!engine_.IsMuted());
            break;
        case ID_VOLUME_UP:
            ChangeVolume(5);
            break;
        case ID_VOLUME_DOWN:
            ChangeVolume(-5);
            break;
        case ID_FULLSCREEN:
        case ID_EXIT_FULLSCREEN:
        case ID_CONTROL_FULLSCREEN:
            if (command != ID_EXIT_FULLSCREEN || fullscreen_) {
                ToggleFullscreen();
            }
            break;
        case ID_ALWAYS_ON_TOP:
            alwaysOnTop_ = !alwaysOnTop_;
            SetWindowPos(hwnd_, alwaysOnTop_ ? HWND_TOPMOST : HWND_NOTOPMOST,
                         0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            break;
        case ID_RTX_VIDEO_UPSCALING: {
            const bool enable = !renderer_.IsRtxVideoUpscalingEnabled();
            if (!renderer_.SetRtxVideoUpscalingEnabled(enable)) {
                const std::wstring message = Localization::Format(
                    "message.rtx_video_unavailable",
                    {{L"error", renderer_.RtxVideoUpscalingStatus()}});
                MessageBoxW(hwnd_, message.c_str(),
                            Localization::Text("title.rtx_video_upscaling").c_str(),
                            MB_OK | MB_ICONINFORMATION);
            } else {
                RedrawLastFrame();
            }
            break;
        }
        case ID_LOOP:
            loop_ = !loop_;
            break;
        case ID_AUTO_PLAY_NEXT:
            autoPlayNext_ = !autoPlayNext_;
            break;
        case ID_SUBTITLE_OPEN:
            ShowSubtitleDialog();
            break;
        case ID_SUBTITLE_AUTO:
            if (!engine_.IsOpen()) {
                MessageBoxW(hwnd_, Localization::Text("message.open_video_first").c_str(),
                            Localization::Text("title.subtitles").c_str(),
                            MB_OK | MB_ICONINFORMATION);
            } else if (!TryAutoLoadSubtitle(currentPath_, true)) {
                if (engine_.HasEmbeddedSubtitles()) {
                    subtitleTrack_.Clear();
                    subtitleEnabled_ = true;
                    UpdateSubtitleText(true);
                } else {
                    MessageBoxW(
                        hwnd_,
                        Localization::Text("message.subtitle_not_found").c_str(),
                        Localization::Text("title.subtitles").c_str(),
                        MB_OK | MB_ICONINFORMATION);
                }
            }
            break;
        case ID_SUBTITLE_WHISPER_GENERATE:
            StartWhisperSubtitleGeneration();
            break;
        case ID_SUBTITLE_WHISPER_CANCEL:
            if (whisperJob_.IsRunning()) {
                whisperCancelledByUser_ = true;
                whisperJob_.Cancel();
                whisperStatusKey_ = "status.whisper_canceling";
            }
            break;
        case ID_SUBTITLE_OFF:
            subtitleEnabled_ = false;
            displayedSubtitle_.clear();
            displayedSubtitleBitmap_.reset();
            renderer_.SetSubtitleText(L"");
            RedrawLastFrame();
            break;
        case ID_CONTROL_SPEED:
            CycleSpeed();
            break;
        case ID_SPEED_050:
            SetSpeed(0.50f);
            break;
        case ID_SPEED_075:
            SetSpeed(0.75f);
            break;
        case ID_SPEED_100:
            SetSpeed(1.00f);
            break;
        case ID_SPEED_125:
            SetSpeed(1.25f);
            break;
        case ID_SPEED_150:
            SetSpeed(1.50f);
            break;
        case ID_SPEED_200:
            SetSpeed(2.00f);
            break;
        case ID_LANGUAGE_EN:
            SetUiLanguage(L"en");
            break;
        case ID_LANGUAGE_JA:
            SetUiLanguage(L"ja");
            break;
        case ID_LANGUAGE_KO:
            SetUiLanguage(L"ko");
            break;
        case ID_LANGUAGE_FR:
            SetUiLanguage(L"fr");
            break;
        case ID_LANGUAGE_ZH_CN:
            SetUiLanguage(L"zh-CN");
            break;
        case ID_LANGUAGE_ZH_TW:
            SetUiLanguage(L"zh-TW");
            break;
        case ID_LANGUAGE_ES:
            SetUiLanguage(L"es");
            break;
        case ID_LANGUAGE_PT:
            SetUiLanguage(L"pt");
            break;
        case ID_LANGUAGE_HI:
            SetUiLanguage(L"hi");
            break;
        case ID_LANGUAGE_ID:
            SetUiLanguage(L"id");
            break;
        case ID_LANGUAGE_AR:
            SetUiLanguage(L"ar");
            break;
        case ID_ABOUT:
            MessageBoxW(hwnd_, Localization::Format(
                            "message.about",
                            {{L"version", MOVIEPLAYER_DISPLAY_VERSION}}).c_str(),
                        Localization::Text("title.about").c_str(),
                        MB_OK | MB_ICONINFORMATION);
            break;
        default:
            break;
        }
        UpdateControls(true);
        UpdateMenuChecks();
    }

    void ShowOpenDialog() {
        std::vector<wchar_t> path(32768, L'\0');
        const std::wstring filter =
            Localization::Text("dialog.video_files") + L'\0' +
            L"*.mp4;*.mkv;*.avi" +
            L'\0' + Localization::Text("dialog.all_files") + L'\0' + L"*.*" +
            L'\0' + L'\0';
        OPENFILENAMEW dialog = {};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = hwnd_;
        dialog.lpstrFilter = filter.c_str();
        dialog.lpstrFile = path.data();
        dialog.nMaxFile = static_cast<DWORD>(path.size());
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                       OFN_EXPLORER | OFN_HIDEREADONLY;
        if (GetOpenFileNameW(&dialog)) {
            OpenPath(path.data());
        }
    }

    void ShowSubtitleDialog() {
        if (!engine_.IsOpen()) {
            MessageBoxW(hwnd_, Localization::Text("message.open_video_first").c_str(),
                        Localization::Text("title.subtitles").c_str(),
                        MB_OK | MB_ICONINFORMATION);
            return;
        }
        std::vector<wchar_t> path(32768, L'\0');
        const std::wstring filter =
            Localization::Text("dialog.subtitle_files") + L'\0' +
            L"*.srt;*.ass;*.ssa;*.smi;*.sami;*.vtt" + L'\0' +
            Localization::Text("dialog.all_files") + L'\0' + L"*.*" + L'\0' + L'\0';
        OPENFILENAMEW dialog = {};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = hwnd_;
        dialog.lpstrFilter = filter.c_str();
        dialog.lpstrFile = path.data();
        dialog.nMaxFile = static_cast<DWORD>(path.size());
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                       OFN_EXPLORER | OFN_HIDEREADONLY;
        if (GetOpenFileNameW(&dialog)) {
            LoadSubtitle(path.data(), true);
        }
    }

    bool LoadSubtitle(const std::wstring& path, bool showError) {
        if (!engine_.IsOpen()) {
            if (showError) {
                MessageBoxW(hwnd_, Localization::Text("message.open_video_first").c_str(),
                            Localization::Text("title.subtitles").c_str(),
                            MB_OK | MB_ICONINFORMATION);
            }
            return false;
        }
        std::wstring error;
        SubtitleTrack loaded;
        if (!loaded.Load(path, error)) {
            if (showError) {
                MessageBoxW(hwnd_, error.c_str(),
                            Localization::Text("title.subtitle_error").c_str(),
                            MB_OK | MB_ICONERROR);
            }
            return false;
        }
        subtitleTrack_ = std::move(loaded);
        subtitleEnabled_ = true;
        displayedSubtitle_.clear();
        displayedSubtitleBitmap_.reset();
        UpdateSubtitleText(true);
        UpdateControls(true);
        return true;
    }

    bool TryAutoLoadSubtitle(const std::wstring& videoPath, bool showParseError) {
        if (videoPath.empty()) {
            return false;
        }
        const wchar_t* extension = PathFindExtensionW(videoPath.c_str());
        const size_t baseLength = extension
                                      ? static_cast<size_t>(extension - videoPath.c_str())
                                      : videoPath.size();
        const std::wstring base = videoPath.substr(0, baseLength);
        const std::wstring language = Localization::CurrentLanguage();
        std::vector<std::wstring> aliases = {language};
        if (_wcsicmp(language.c_str(), L"en") == 0) {
            aliases.push_back(L"eng");
        } else if (_wcsicmp(language.c_str(), L"ja") == 0) {
            aliases.push_back(L"jpn");
        } else if (_wcsicmp(language.c_str(), L"ko") == 0) {
            aliases.insert(aliases.end(), {L"kor", L"korean"});
        } else if (_wcsicmp(language.c_str(), L"fr") == 0) {
            aliases.insert(aliases.end(), {L"fra", L"fre"});
        } else if (_wcsicmp(language.c_str(), L"zh-CN") == 0) {
            aliases.insert(aliases.end(), {L"zh", L"zh-Hans", L"chs"});
        } else if (_wcsicmp(language.c_str(), L"zh-TW") == 0) {
            aliases.insert(aliases.end(), {L"zh-Hant", L"cht"});
        } else if (_wcsicmp(language.c_str(), L"es") == 0) {
            aliases.push_back(L"spa");
        } else if (_wcsicmp(language.c_str(), L"pt") == 0) {
            aliases.insert(aliases.end(), {L"por", L"pt-BR", L"pt-PT"});
        } else if (_wcsicmp(language.c_str(), L"hi") == 0) {
            aliases.push_back(L"hin");
        } else if (_wcsicmp(language.c_str(), L"id") == 0) {
            aliases.push_back(L"ind");
        } else if (_wcsicmp(language.c_str(), L"ar") == 0) {
            aliases.push_back(L"ara");
        }

        std::vector<std::wstring> suffixes;
        suffixes.push_back(L"." + language + L".whisper.srt");
        for (const std::wstring& alias : aliases) {
            suffixes.push_back(L"." + alias + L".srt");
        }
        for (const std::wstring& alias : aliases) {
            suffixes.push_back(L"." + alias + L".ass");
            suffixes.push_back(L"." + alias + L".smi");
            suffixes.push_back(L"." + alias + L".vtt");
        }
        suffixes.insert(suffixes.end(), {
            L".srt", L".ass", L".ssa", L".smi", L".vtt",
        });
        for (const std::wstring& suffix : suffixes) {
            const std::wstring candidate = base + suffix;
            const DWORD attributes = GetFileAttributesW(candidate.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES &&
                !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                return LoadSubtitle(candidate, showParseError);
            }
        }
        return false;
    }

    std::wstring WhisperOutputPath(const std::wstring& videoPath,
                                   const std::wstring& targetLanguage) const {
        const wchar_t* extension = PathFindExtensionW(videoPath.c_str());
        const size_t baseLength = extension
                                      ? static_cast<size_t>(extension - videoPath.c_str())
                                      : videoPath.size();
        return videoPath.substr(0, baseLength) + L"." + targetLanguage +
               L".whisper.srt";
    }

    bool EnsureWhisperRuntime(bool resumeRegeneration,
                              const std::wstring& targetLanguage) {
        const WhisperSubtitleJob::RuntimeInfo runtime =
            WhisperSubtitleJob::InspectRuntime();
        if (runtime.ready) {
            return true;
        }
        if (!runtime.error.empty()) {
            MessageBoxW(hwnd_, runtime.error.c_str(),
                        Localization::Text("title.ai_engine").c_str(),
                        MB_OK | MB_ICONERROR);
            return false;
        }
        if (whisperInstallerProcess_) {
            MessageBoxW(hwnd_, Localization::Text("message.ai_installing").c_str(),
                        Localization::Text("title.ai_engine").c_str(),
                        MB_OK | MB_ICONINFORMATION);
            return false;
        }
        const DWORD installerAttributes = GetFileAttributesW(
            runtime.installerPath.c_str());
        if (installerAttributes == INVALID_FILE_ATTRIBUTES ||
            (installerAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            const std::wstring message = Localization::Format(
                "error.ai_installer_missing", {{L"path", runtime.installerPath}});
            MessageBoxW(hwnd_, message.c_str(),
                        Localization::Text("title.ai_engine").c_str(),
                        MB_OK | MB_ICONERROR);
            return false;
        }

        const std::wstring missing = runtime.missingComponent.empty()
                                         ? Localization::Text("component.ai_runtime_or_model")
                                         : runtime.missingComponent;
        const std::wstring prompt = Localization::Format(
            "message.ai_first_install", {{L"missing", missing}});
        if (MessageBoxW(hwnd_, prompt.c_str(),
                        Localization::Text("title.ai_first_install").c_str(),
                        MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return false;
        }

        SHELLEXECUTEINFOW execute = {};
        execute.cbSize = sizeof(execute);
        execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        execute.hwnd = hwnd_;
        execute.lpVerb = L"open";
        execute.lpFile = runtime.installerPath.c_str();
        execute.lpParameters = L"--from-app";
        execute.lpDirectory = runtime.rootPath.c_str();
        execute.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&execute) || !execute.hProcess) {
            const std::wstring message = Localization::Format(
                "error.ai_installer_launch",
                {{L"error", FormatHResult(GetLastError())}});
            MessageBoxW(hwnd_, message.c_str(),
                        Localization::Text("title.ai_engine").c_str(),
                        MB_OK | MB_ICONERROR);
            return false;
        }

        whisperInstallerProcess_ = execute.hProcess;
        whisperInstallerSourcePath_ = currentPath_;
        whisperInstallerTargetLanguage_ = targetLanguage;
        whisperInstallerResumeRegeneration_ = resumeRegeneration;
        whisperStatusKey_ = "status.ai_installing";
        UpdateControls(true);
        UpdateMenuChecks();
        return false;
    }

    void StartWhisperSubtitleGeneration(
        bool resumeRegeneration = false,
        std::wstring targetLanguage = {}) {
        if (!engine_.IsOpen() || currentPath_.empty()) {
            MessageBoxW(hwnd_, Localization::Text("message.open_video_first").c_str(),
                        Localization::Text("title.whisper_subtitle").c_str(),
                        MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (whisperJob_.IsRunning()) {
            MessageBoxW(hwnd_, Localization::Text("message.whisper_already_running").c_str(),
                        Localization::Text("title.whisper_subtitle").c_str(),
                        MB_OK | MB_ICONINFORMATION);
            return;
        }

        if (targetLanguage.empty()) {
            targetLanguage = Localization::CurrentLanguage();
        }

        const std::wstring output = WhisperOutputPath(currentPath_, targetLanguage);
        const DWORD attributes = GetFileAttributesW(output.c_str());
        const bool outputExists =
            attributes != INVALID_FILE_ATTRIBUTES &&
            !(attributes & FILE_ATTRIBUTE_DIRECTORY);
        if (outputExists && !resumeRegeneration) {
            const std::wstring regenerateDescription =
                Localization::Text("message.whisper_regenerate_auto");
            const std::wstring existingPrompt = Localization::Format(
                "message.whisper_existing", {{L"regenerate", regenerateDescription}});
            const int choice = MessageBoxW(
                hwnd_, existingPrompt.c_str(),
                Localization::Text("title.whisper_subtitle").c_str(),
                MB_YESNOCANCEL | MB_ICONQUESTION);
            if (choice == IDCANCEL) {
                return;
            }
            if (choice == IDYES) {
                LoadSubtitle(output, true);
                return;
            }
        }

        if (!EnsureWhisperRuntime(outputExists, targetLanguage)) {
            return;
        }

        if (!outputExists) {
            const std::wstring modeDescription =
                Localization::Text("message.whisper_mode_auto");
            const std::wstring prompt = Localization::Format(
                "message.whisper_generate", {{L"mode", modeDescription}});
            if (MessageBoxW(hwnd_, prompt.c_str(),
                            Localization::Text("title.whisper_subtitle").c_str(),
                            MB_YESNO | MB_ICONQUESTION) != IDYES) {
                return;
            }
        }

        std::wstring error;
        whisperSourcePath_ = currentPath_;
        whisperTargetLanguage_ = targetLanguage;
        whisperCancelledByUser_ = false;
        if (!whisperJob_.Start(currentPath_, output, engine_.Duration(),
                               targetLanguage, error)) {
            whisperSourcePath_.clear();
            whisperTargetLanguage_.clear();
            whisperStatusKey_.clear();
            MessageBoxW(hwnd_, error.c_str(),
                        Localization::Text("title.whisper_run_error").c_str(),
                        MB_OK | MB_ICONERROR);
            return;
        }
        whisperProgress_ = 0.0;
        whisperStatusKey_ = "status.whisper_preparing_auto";
    }

    void PollWhisperRuntimeInstaller() {
        if (!whisperInstallerProcess_) {
            return;
        }
        const DWORD wait = WaitForSingleObject(whisperInstallerProcess_, 0);
        if (wait == WAIT_TIMEOUT) {
            return;
        }

        DWORD exitCode = ERROR_GEN_FAILURE;
        if (wait == WAIT_OBJECT_0) {
            GetExitCodeProcess(whisperInstallerProcess_, &exitCode);
        }
        CloseHandle(whisperInstallerProcess_);
        whisperInstallerProcess_ = nullptr;

        const std::wstring sourcePath = whisperInstallerSourcePath_;
        const std::wstring targetLanguage = whisperInstallerTargetLanguage_;
        const bool resumeRegeneration = whisperInstallerResumeRegeneration_;
        whisperInstallerSourcePath_.clear();
        whisperInstallerTargetLanguage_.clear();
        whisperInstallerResumeRegeneration_ = false;

        const WhisperSubtitleJob::RuntimeInfo runtime =
            WhisperSubtitleJob::InspectRuntime();
        if (exitCode != ERROR_SUCCESS || !runtime.ready) {
            whisperStatusKey_ = "status.ai_install_failed";
            std::wstring message = Localization::Text("error.ai_install_incomplete");
            if (!runtime.error.empty()) {
                message += L"\n\n" + runtime.error;
            } else if (!runtime.missingComponent.empty()) {
                message += L"\n\n" + Localization::Format(
                    "message.missing_component", {{L"component", runtime.missingComponent}});
            }
            MessageBoxW(hwnd_, message.c_str(),
                        Localization::Text("title.ai_install_error").c_str(),
                        MB_OK | MB_ICONERROR);
            UpdateControls(true);
            UpdateMenuChecks();
            return;
        }

        whisperStatusKey_ = "status.ai_install_complete";
        UpdateControls(true);
        UpdateMenuChecks();
        if (engine_.IsOpen() && !sourcePath.empty() &&
            _wcsicmp(currentPath_.c_str(), sourcePath.c_str()) == 0) {
            StartWhisperSubtitleGeneration(
                resumeRegeneration, targetLanguage);
        }
    }

    void PollWhisperSubtitleJob() {
        WhisperSubtitleJob::Update update;
        if (!whisperJob_.Poll(update)) {
            return;
        }
        whisperProgress_ = update.progress;
        if (!update.finished) {
            whisperStatusKey_ = "status.whisper_progress";
            return;
        }

        if (!update.error.empty()) {
            whisperStatusKey_ = whisperCancelledByUser_
                                    ? "status.whisper_canceled"
                                    : "status.whisper_failed";
            if (!whisperCancelledByUser_) {
                MessageBoxW(hwnd_, update.error.c_str(),
                            Localization::Text("title.whisper_generation_error").c_str(),
                            MB_OK | MB_ICONERROR);
            }
        } else if (!update.output.empty() &&
                   _wcsicmp(currentPath_.c_str(), whisperSourcePath_.c_str()) == 0) {
            if (LoadSubtitle(update.output, true)) {
                whisperStatusKey_ = "status.whisper_complete";
            }
        }
        whisperSourcePath_.clear();
        whisperTargetLanguage_.clear();
        whisperCancelledByUser_ = false;
        UpdateControls(true);
        UpdateMenuChecks();
    }

    void CancelWhisperForMediaChange() {
        if (whisperJob_.IsRunning()) {
            whisperJob_.Cancel();
        }
        whisperSourcePath_.clear();
        whisperTargetLanguage_.clear();
        whisperStatusKey_.clear();
        whisperCancelledByUser_ = false;
        whisperProgress_ = 0.0;
    }

    void RedrawLastFrame() {
        if (lastFrame_ && lastFrame_->texture) {
            renderer_.RenderFrame(*lastFrame_);
        }
    }

    void UpdateSubtitleText(bool redraw) {
        std::wstring text;
        std::shared_ptr<const movieplayer::codec::SubtitleBitmap> bitmap;
        if (subtitleEnabled_) {
            if (!subtitleTrack_.Empty())
                text = subtitleTrack_.TextAt(engine_.CurrentPosition());
            else if (engine_.HasEmbeddedSubtitles()) {
                text = engine_.EmbeddedSubtitleText();
                bitmap = engine_.EmbeddedSubtitleBitmap();
            }
        }
        if (text == displayedSubtitle_ &&
            bitmap == displayedSubtitleBitmap_) {
            return;
        }
        displayedSubtitle_ = std::move(text);
        displayedSubtitleBitmap_ = std::move(bitmap);
        if (displayedSubtitleBitmap_)
            renderer_.SetSubtitleBitmap(displayedSubtitleBitmap_);
        else
            renderer_.SetSubtitleText(displayedSubtitle_);
        if (redraw) {
            RedrawLastFrame();
        }
    }

    bool OpenPath(const std::wstring& path) {
        if (path.empty()) {
            return false;
        }
        CancelWhisperForMediaChange();
        SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        SetWindowTextW(statusLabel_, Localization::Text("status.opening").c_str());
        UpdateWindow(hwnd_);

        subtitleTrack_.Clear();
        subtitleEnabled_ = false;
        displayedSubtitle_.clear();
        displayedSubtitleBitmap_.reset();
        renderer_.SetSubtitleText(L"");
        lastFrame_.reset();
        renderer_.Clear();
        endedHandled_ = false;
        currentPath_ = path;
        const bool opened = engine_.Open(path, renderer_.Device());
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        RebuildTrackMenus();
        if (!opened) {
            currentPath_.clear();
            SetWindowTextW(statusLabel_, Localization::Text("status.open_failed").c_str());
            MessageBoxW(hwnd_, engine_.LastError().c_str(),
                        Localization::Text("title.playback_error").c_str(),
                        MB_OK | MB_ICONERROR);
            SetWindowTextW(hwnd_, L"MoviePlayer");
            return false;
        }

        const wchar_t* fileName = PathFindFileNameW(path.c_str());
        std::wstring title = fileName ? fileName : path;
        title += L" - MoviePlayer";
        SetWindowTextW(hwnd_, title.c_str());
        if (initialSubtitle_.empty()) {
            const bool externalLoaded = TryAutoLoadSubtitle(path, false);
            if (!externalLoaded && engine_.HasEmbeddedSubtitles()) {
                subtitleEnabled_ = true;
            }
        }
        UpdateControls(true);
        return true;
    }

    void CloseMedia() {
        CancelWhisperForMediaChange();
        engine_.Close();
        RebuildTrackMenus();
        subtitleTrack_.Clear();
        subtitleEnabled_ = false;
        displayedSubtitle_.clear();
        displayedSubtitleBitmap_.reset();
        renderer_.SetSubtitleText(L"");
        lastFrame_.reset();
        currentPath_.clear();
        renderer_.Clear();
        SetWindowTextW(hwnd_, L"MoviePlayer");
        SetWindowTextW(statusLabel_, Localization::Text("status.idle").c_str());
        SendMessageW(seekBar_, TBM_SETPOS, TRUE, 0);
        SetWindowTextW(timeLabel_, L"00:00 / 00:00");
        SetWindowTextW(playButton_, Localization::Text("button.play").c_str());
        SetWindowTextW(muteButton_, Localization::Text(
            engine_.IsMuted() ? "button.unmute" : "button.mute").c_str());
        InvalidateRect(hwnd_, nullptr, FALSE);
        InvalidateRect(playButton_, nullptr, FALSE);
        InvalidateRect(muteButton_, nullptr, FALSE);
        InvalidateRect(seekBar_, nullptr, FALSE);
    }

    void HandleDrop(HDROP drop) {
        wchar_t path[32768] = {};
        if (DragQueryFileW(drop, 0, path, static_cast<UINT>(std::size(path))) > 0) {
            const std::wstring extension = LowerExtension(path);
            if (extension == L".srt" || extension == L".ass" ||
                extension == L".ssa" || extension == L".smi" ||
                extension == L".sami" || extension == L".vtt") {
                LoadSubtitle(path, true);
            } else {
                OpenPath(path);
            }
        }
        DragFinish(drop);
    }

    void TogglePlayback() {
        if (!engine_.IsOpen()) {
            ShowOpenDialog();
            return;
        }
        if (engine_.IsEnded()) {
            endedHandled_ = false;
            engine_.Seek(0.0);
            engine_.Play();
        } else {
            engine_.TogglePause();
        }
    }

    void SeekRelative(double delta) {
        if (engine_.IsOpen()) {
            endedHandled_ = false;
            engine_.SeekRelative(delta);
        }
    }

    void ChangeVolume(int delta) {
        int current = static_cast<int>(SendMessageW(volumeBar_, TBM_GETPOS, 0, 0));
        current = std::max(0, std::min(100, current + delta));
        SendMessageW(volumeBar_, TBM_SETPOS, TRUE, current);
        engine_.SetVolume(static_cast<float>(current) / 100.0f);
        InvalidateRect(volumeBar_, nullptr, FALSE);
        if (current > 0 && engine_.IsMuted()) {
            engine_.SetMuted(false);
        }
    }

    void HandleTrackbar(HWND source, UINT notification) {
        if (source == volumeBar_) {
            const int value = static_cast<int>(SendMessageW(volumeBar_, TBM_GETPOS, 0, 0));
            engine_.SetVolume(static_cast<float>(value) / 100.0f);
            InvalidateRect(volumeBar_, nullptr, FALSE);
            return;
        }
        if (source != seekBar_ || !engine_.IsOpen()) {
            return;
        }

        const int position = static_cast<int>(SendMessageW(seekBar_, TBM_GETPOS, 0, 0));
        const double target = engine_.Duration() * position / kSeekRange;
        if (notification == TB_THUMBTRACK) {
            trackingSeek_ = true;
            const std::wstring text = FormatMediaTime(target) + L" / " +
                                      FormatMediaTime(engine_.Duration());
            SetWindowTextW(timeLabel_, text.c_str());
        } else if (notification == TB_THUMBPOSITION || notification == TB_ENDTRACK) {
            if (trackingSeek_ || notification == TB_THUMBPOSITION) {
                CommitSeekFromSlider(target);
            }
        } else if (notification == TB_LINEUP || notification == TB_LINEDOWN ||
                   notification == TB_PAGEUP || notification == TB_PAGEDOWN ||
                   notification == TB_TOP || notification == TB_BOTTOM) {
            CommitSeekFromSlider(target);
        }
    }

    void CommitSeekFromSlider(double target) {
        trackingSeek_ = false;
        endedHandled_ = false;
        displayedSubtitle_.clear();
        displayedSubtitleBitmap_.reset();
        renderer_.SetSubtitleText(L"");
        if (!engine_.Seek(target)) {
            const std::wstring error = engine_.LastError();
            const std::wstring message = error.empty()
                                             ? Localization::Text("error.seek_failed")
                                             : error;
            MessageBoxW(hwnd_, message.c_str(),
                        Localization::Text("title.seek_error").c_str(),
                        MB_OK | MB_ICONERROR);
            UpdateControls(true);
            return;
        }
        // A scrollbar seek is an explicit request to continue from the new
        // point, even if playback happened to be paused before dragging.
        engine_.Play();
        UpdateSubtitleText(false);
        UpdateControls(true);
    }

    void SetSpeed(float speed) {
        engine_.SetSpeed(speed);
        wchar_t text[16] = {};
        swprintf_s(text, L"%.2f×", speed);
        SetWindowTextW(speedButton_, text);
        InvalidateRect(speedButton_, nullptr, FALSE);
    }

    void CycleSpeed() {
        constexpr std::array<float, 6> speeds = {0.50f, 0.75f, 1.00f,
                                                 1.25f, 1.50f, 2.00f};
        const float current = engine_.Speed();
        auto it = std::find_if(speeds.begin(), speeds.end(), [current](float value) {
            return value > current + 0.01f;
        });
        SetSpeed(it == speeds.end() ? speeds.front() : *it);
    }

    void SetFullscreenControlsVisible(bool visible, bool force = false) {
        if (!fullscreen_) {
            return;
        }
        if (!force && fullscreenControlsVisible_ == visible) {
            return;
        }
        fullscreenControlsVisible_ = visible;
        LayoutControls();
    }

    void UpdateFullscreenControlsFromCursor(bool force) {
        if (!fullscreen_ || !hwnd_) {
            return;
        }

        POINT point = {};
        if (!GetCursorPos(&point)) {
            return;
        }
        ScreenToClient(hwnd_, &point);

        RECT client = {};
        GetClientRect(hwnd_, &client);
        const UINT dpi = GetDpiForWindow(hwnd_);
        const int hotZone = ScaleForDpi(kFullscreenControlHotZone, dpi);
        const bool inside = point.x >= client.left && point.x < client.right &&
                            point.y >= client.top && point.y < client.bottom;
        const bool show = inside && point.y >= client.bottom - hotZone;
        SetFullscreenControlsVisible(show, force);
    }

    void ToggleFullscreen() {
        if (!fullscreen_) {
            savedPlacement_.length = sizeof(savedPlacement_);
            GetWindowPlacement(hwnd_, &savedPlacement_);
            savedStyle_ = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
            SetMenu(hwnd_, nullptr);
            SetControlVisibility(false);
            SetWindowLongPtrW(hwnd_, GWL_STYLE,
                              WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN);
            MONITORINFO monitor = {sizeof(monitor)};
            GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor);
            fullscreen_ = true;
            fullscreenControlsVisible_ = false;
            SetWindowPos(hwnd_, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                         monitor.rcMonitor.right - monitor.rcMonitor.left,
                         monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                         SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        } else {
            fullscreen_ = false;
            fullscreenControlsVisible_ = false;
            SetWindowLongPtrW(hwnd_, GWL_STYLE, savedStyle_);
            SetMenu(hwnd_, menu_);
            SetWindowPlacement(hwnd_, &savedPlacement_);
            SetWindowPos(hwnd_, alwaysOnTop_ ? HWND_TOPMOST : HWND_NOTOPMOST,
                         0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
            SetControlVisibility(true);
        }
        renderer_.SetFullscreenSubtitleScale(fullscreen_);
        LayoutControls();
        renderer_.Resize();
        if (lastFrame_ && lastFrame_->texture) {
            renderer_.RenderFrame(*lastFrame_);
        } else {
            renderer_.Clear();
        }
        menuStateValid_ = false;
        UpdateMenuChecks();
    }

    void ShowVideoContextMenu(int x, int y) {
        POINT point = {x, y};
        ClientToScreen(videoHwnd_, &point);
        HMENU popup = CreatePopupMenu();
        const std::wstring playText = Localization::Text(
            engine_.IsPaused() ? "button.play" : "button.pause");
        const std::wstring stopText = Localization::Text("button.stop");
        AppendMenuW(popup, MF_STRING, ID_PLAY_PAUSE, playText.c_str());
        AppendMenuW(popup, MF_STRING, ID_STOP, stopText.c_str());
        AppendMenuW(popup, MF_SEPARATOR, 0, nullptr);
        const std::wstring fullscreenText = Localization::Text(
            fullscreen_ ? "context.exit_fullscreen" : "button.fullscreen");
        AppendMenuW(popup, MF_STRING, ID_FULLSCREEN, fullscreenText.c_str());
        const size_t firstContextItem = darkMenuItems_.size();
        ApplyDarkMenuStyle(popup, false);
        const int command = TrackPopupMenu(popup, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                           point.x, point.y, 0, hwnd_, nullptr);
        DestroyMenu(popup);
        darkMenuItems_.resize(firstContextItem);
        if (command) {
            ExecuteCommand(command);
        }
    }

    void PlaybackTick() {
        PollWhisperSubtitleJob();
        PollWhisperRuntimeInstaller();
        UpdateFullscreenControlsFromCursor(false);
        if (!engine_.IsOpen()) {
            return;
        }
        UpdateSubtitleText(false);
        if (auto frame = engine_.AcquireVideoFrame()) {
            lastFrame_ = std::move(frame);
            if (!renderer_.RenderFrame(*lastFrame_)) {
                SetWindowTextW(statusLabel_, renderer_.LastError().c_str());
            }
        }

        const ULONGLONG now = GetTickCount64();
        if (now - lastUiUpdate_ >= 100) {
            lastUiUpdate_ = now;
            UpdateControls(false);
        }

        const std::wstring asyncError = engine_.TakeAsyncError();
        if (!asyncError.empty()) {
            MessageBoxW(hwnd_, asyncError.c_str(),
                        Localization::Text("title.playback_error").c_str(),
                        MB_OK | MB_ICONERROR);
        }

        if (engine_.IsEnded() && !endedHandled_) {
            endedHandled_ = true;
            if (loop_) {
                endedHandled_ = false;
                engine_.Seek(0.0);
                engine_.Play();
            } else {
                const std::wstring nextPath = autoPlayNext_
                                                  ? FindNextEpisodePath(currentPath_)
                                                  : std::wstring();
                if (nextPath.empty() || !OpenPath(nextPath)) {
                    if (engine_.IsOpen()) {
                        engine_.Pause();
                        UpdateControls(true);
                    }
                }
            }
        }
    }

    std::wstring CurrentWhisperStatus() const {
        if (whisperStatusKey_.empty()) {
            return {};
        }
        if (whisperStatusKey_ == "status.whisper_progress") {
            wchar_t progress[32] = {};
            swprintf_s(progress, L"%.0f", whisperProgress_);
            return Localization::Format(whisperStatusKey_.c_str(),
                                        {{L"progress", progress}});
        }
        return Localization::Text(whisperStatusKey_.c_str());
    }

    void UpdateControls(bool force) {
        if (!engine_.IsOpen()) {
            return;
        }
        if (!force && trackingSeek_) {
            return;
        }
        const double duration = engine_.Duration();
        const double position = engine_.CurrentPosition();
        if (!trackingSeek_ && duration > 0.0) {
            const int slider = static_cast<int>(std::max(0.0, std::min(
                static_cast<double>(kSeekRange), position / duration * kSeekRange)));
            const int currentSlider =
                static_cast<int>(SendMessageW(seekBar_, TBM_GETPOS, 0, 0));
            if (force || currentSlider != slider) {
                SendMessageW(seekBar_, TBM_SETPOS, TRUE, slider);
                InvalidateRect(seekBar_, nullptr, FALSE);
            }
        }
        const std::wstring time = FormatMediaTime(position) + L" / " +
                                  FormatMediaTime(duration);
        const bool timeChanged = SetWindowTextIfChanged(timeLabel_, time);
        if (force && !timeChanged) {
            InvalidateRect(timeLabel_, nullptr, FALSE);
        }

        const bool playingGlyph =
            engine_.IsOpen() && !engine_.IsPaused() && !engine_.IsEnded();
        const bool muted = engine_.IsMuted();
        const bool playTextChanged = SetWindowTextIfChanged(
            playButton_,
            Localization::Text(engine_.IsPaused() ? "button.play" : "button.pause"));
        const bool muteTextChanged = SetWindowTextIfChanged(
            muteButton_,
            Localization::Text(muted ? "button.unmute" : "button.mute"));
        if (force || !controlStateValid_ || playTextChanged ||
            lastPlayingGlyph_ != playingGlyph) {
            InvalidateRect(playButton_, nullptr, FALSE);
        }
        if (force || !controlStateValid_ || muteTextChanged ||
            lastMutedGlyph_ != muted) {
            InvalidateRect(muteButton_, nullptr, FALSE);
        }

        std::wstring status = engine_.MediaDescription();
        const std::wstring decoder = engine_.DecoderDescription();
        if (!decoder.empty()) {
            status += L"  ·  " + decoder;
        }
        if (renderer_.IsRtxVideoUpscalingEnabled()) {
            status += L"  ·  RTX VSR";
        }
        if (subtitleEnabled_ &&
            (!subtitleTrack_.Empty() || engine_.HasEmbeddedSubtitles())) {
            status += L"  ·  " + Localization::Text("status.subtitle") + L": ";
            if (!subtitleTrack_.Empty()) {
                const wchar_t* subtitleName =
                    PathFindFileNameW(subtitleTrack_.FilePath().c_str());
                status += subtitleName ? subtitleName : subtitleTrack_.FilePath();
            } else {
                status += engine_.EmbeddedSubtitleDescription();
            }
            if (!displayedSubtitle_.empty() || displayedSubtitleBitmap_) {
                status += L" (" + Localization::Text("status.subtitle_showing") + L")";
            }
        }
        const std::wstring whisperStatus = CurrentWhisperStatus();
        if (!whisperStatus.empty()) {
            status += L"  ·  " + whisperStatus;
        }
        const bool statusChanged = SetWindowTextIfChanged(statusLabel_, status);
        if (force && !statusChanged) {
            InvalidateRect(statusLabel_, nullptr, FALSE);
        }
        lastPlayingGlyph_ = playingGlyph;
        lastMutedGlyph_ = muted;
        controlStateValid_ = true;
        UpdateMenuChecks();
    }

    void UpdateMenuChecks() {
        if (!menu_) {
            return;
        }
        MenuState state;
        state.muted = engine_.IsMuted();
        state.loop = loop_;
        state.autoPlayNext = autoPlayNext_;
        state.alwaysOnTop = alwaysOnTop_;
        state.rtxVideoUpscaling = renderer_.IsRtxVideoUpscalingEnabled();
        state.subtitleOff = !subtitleEnabled_;
        state.whisperBusy =
            whisperJob_.IsRunning() || whisperInstallerProcess_ != nullptr;
        state.engineOpen = engine_.IsOpen();
        state.selectedAudioTrackId = engine_.SelectedAudioTrackId();
        state.externalSubtitle = !subtitleTrack_.Empty();
        state.selectedSubtitleTrackId =
            subtitleEnabled_ && !state.externalSubtitle
                ? engine_.SelectedEmbeddedSubtitleTrackId()
                : 0;

        const float speed = engine_.Speed();
        if (speed < 0.625f) state.speedId = ID_SPEED_050;
        else if (speed < 0.875f) state.speedId = ID_SPEED_075;
        else if (speed < 1.125f) state.speedId = ID_SPEED_100;
        else if (speed < 1.375f) state.speedId = ID_SPEED_125;
        else if (speed < 1.75f) state.speedId = ID_SPEED_150;
        else state.speedId = ID_SPEED_200;
        state.languageId = CurrentLanguageCommand();

        if (menuStateValid_ && SameMenuState(menuState_, state)) {
            return;
        }

        CheckMenuItem(menu_, ID_MUTE,
                      MF_BYCOMMAND | (state.muted ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(menu_, ID_LOOP,
                      MF_BYCOMMAND | (state.loop ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(menu_, ID_AUTO_PLAY_NEXT,
                      MF_BYCOMMAND | (state.autoPlayNext ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(menu_, ID_ALWAYS_ON_TOP,
                      MF_BYCOMMAND | (state.alwaysOnTop ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(menu_, ID_RTX_VIDEO_UPSCALING,
                      MF_BYCOMMAND | (state.rtxVideoUpscaling
                                          ? MF_CHECKED
                                          : MF_UNCHECKED));
        CheckMenuItem(menu_, ID_SUBTITLE_OFF,
                      MF_BYCOMMAND | (state.subtitleOff ? MF_CHECKED : MF_UNCHECKED));
        EnableMenuItem(menu_, ID_SUBTITLE_WHISPER_GENERATE,
                       MF_BYCOMMAND | (state.whisperBusy || !state.engineOpen
                                            ? MF_GRAYED
                                            : MF_ENABLED));
        EnableMenuItem(menu_, ID_SUBTITLE_WHISPER_CANCEL,
                       MF_BYCOMMAND | (whisperJob_.IsRunning()
                                            ? MF_ENABLED
                                            : MF_GRAYED));

        const auto audioTracks = engine_.AudioTracks();
        const std::size_t audioCount =
            (std::min)(audioTracks.size(), kMaximumTrackMenuItems);
        for (std::size_t i = 0; i < audioCount; ++i) {
            CheckMenuItem(audioTrackMenu_,
                          kAudioTrackCommandBase + static_cast<UINT>(i),
                          MF_BYCOMMAND |
                              (audioTracks[i].trackId == state.selectedAudioTrackId
                                   ? MF_CHECKED
                                   : MF_UNCHECKED));
        }
        const auto subtitleTracks = engine_.EmbeddedSubtitleTracks();
        const std::size_t subtitleCount =
            (std::min)(subtitleTracks.size(), kMaximumTrackMenuItems);
        for (std::size_t i = 0; i < subtitleCount; ++i) {
            CheckMenuItem(subtitleTrackMenu_,
                          kSubtitleTrackCommandBase + static_cast<UINT>(i),
                          MF_BYCOMMAND |
                              (subtitleTracks[i].trackId ==
                                       state.selectedSubtitleTrackId
                                   ? MF_CHECKED
                                   : MF_UNCHECKED));
        }

        CheckMenuRadioItem(menu_, ID_SPEED_050, ID_SPEED_200,
                           state.speedId, MF_BYCOMMAND);
        CheckMenuRadioItem(menu_, ID_LANGUAGE_EN, ID_LANGUAGE_AR,
                           state.languageId, MF_BYCOMMAND);
        menuState_ = state;
        menuStateValid_ = true;
        DrawMenuBar(hwnd_);
        DrawMainMenuBorder();
    }

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND videoHwnd_ = nullptr;
    HWND controlPanelHwnd_ = nullptr;
    HWND openButton_ = nullptr;
    HWND playButton_ = nullptr;
    HWND stopButton_ = nullptr;
    HWND muteButton_ = nullptr;
    HWND speedButton_ = nullptr;
    HWND fullscreenButton_ = nullptr;
    HWND seekBar_ = nullptr;
    HWND volumeBar_ = nullptr;
    HWND timeLabel_ = nullptr;
    HWND statusLabel_ = nullptr;
    HFONT font_ = nullptr;
    HFONT menuFont_ = nullptr;
    HBRUSH windowBrush_ = nullptr;
    HBRUSH panelBrush_ = nullptr;
    HBRUSH menuBrush_ = nullptr;
    HMENU menu_ = nullptr;
    HMENU audioTrackMenu_ = nullptr;
    HMENU subtitleTrackMenu_ = nullptr;
    std::vector<std::unique_ptr<DarkMenuItem>> darkMenuItems_;

    PlayerEngine engine_;
    D3DRenderer renderer_;
    std::shared_ptr<DecodedVideoFrame> lastFrame_;
    std::wstring initialPath_;
    std::wstring initialSubtitle_;
    std::wstring currentPath_;
    SubtitleTrack subtitleTrack_;
    std::wstring displayedSubtitle_;
    std::shared_ptr<const movieplayer::codec::SubtitleBitmap>
        displayedSubtitleBitmap_;
    WhisperSubtitleJob whisperJob_;
    HANDLE whisperInstallerProcess_ = nullptr;
    std::wstring whisperSourcePath_;
    std::wstring whisperTargetLanguage_;
    std::wstring whisperInstallerSourcePath_;
    std::wstring whisperInstallerTargetLanguage_;
    std::string whisperStatusKey_;
    double whisperProgress_ = 0.0;

    bool trackingSeek_ = false;
    bool fullscreen_ = false;
    bool fullscreenControlsVisible_ = false;
    bool alwaysOnTop_ = false;
    bool loop_ = false;
    bool autoPlayNext_ = true;
    bool subtitleEnabled_ = false;
    bool whisperCancelledByUser_ = false;
    bool whisperInstallerResumeRegeneration_ = false;
    bool endedHandled_ = false;
    bool controlStateValid_ = false;
    bool lastPlayingGlyph_ = false;
    bool lastMutedGlyph_ = false;
    bool menuStateValid_ = false;
    MenuState menuState_;
    DWORD savedStyle_ = 0;
    WINDOWPLACEMENT savedPlacement_ = {sizeof(WINDOWPLACEMENT)};
    ULONGLONG lastUiUpdate_ = 0;
};

void EnableBestDpiAwareness() {
    using SetDpiContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    auto setContext = reinterpret_cast<SetDpiContextFn>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (setContext) {
        setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    } else {
        SetProcessDPIAware();
    }
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, wchar_t*, int showCommand) {
    EnableBestDpiAwareness();
    EnableDarkMenuFrames();
    std::wstring localizationError;
    if (!Localization::Initialize(localizationError)) {
        MessageBoxW(nullptr, localizationError.c_str(),
                    L"MoviePlayer language resources", MB_OK | MB_ICONWARNING);
    }
    INITCOMMONCONTROLSEX commonControls = {sizeof(commonControls), ICC_BAR_CLASSES};
    InitCommonControlsEx(&commonControls);
    HWINEVENTHOOK menuEventHook = SetWinEventHook(
        EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, MenuWindowEventProc,
        GetCurrentProcessId(), GetCurrentThreadId(), WINEVENT_OUTOFCONTEXT);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    std::wstring initialPath;
    std::wstring initialSubtitle;
    const bool registerAssociations =
        arguments && argumentCount > 1 &&
        _wcsicmp(arguments[1], L"--register-file-associations") == 0;
    if (arguments && argumentCount > 1 && !registerAssociations) {
        initialPath = arguments[1];
    }
    if (arguments && argumentCount > 2 && !registerAssociations) {
        initialSubtitle = arguments[2];
    }
    if (arguments) {
        LocalFree(arguments);
    }

    if (registerAssociations) {
        std::wstring error;
        const bool registered = RegisterVideoFileAssociations(error);
        if (!registered) {
            MessageBoxW(nullptr, error.c_str(),
                        Localization::Text("title.file_association_error").c_str(),
                        MB_OK | MB_ICONERROR);
        }
        if (menuEventHook) {
            UnhookWinEvent(menuEventHook);
        }
        CoUninitialize();
        return registered ? 0 : 1;
    }

    MainWindow window;
    if (!window.Create(instance, initialPath, initialSubtitle)) {
        MessageBoxW(nullptr, Localization::Text("error.window_create").c_str(),
                    L"MoviePlayer", MB_OK | MB_ICONERROR);
        if (menuEventHook) {
            UnhookWinEvent(menuEventHook);
        }
        CoUninitialize();
        return 1;
    }
    const int result = window.Run(showCommand);
    if (menuEventHook) {
        UnhookWinEvent(menuEventHook);
    }
    CoUninitialize();
    return result;
}
