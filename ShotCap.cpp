// Define _WIN32_WINNT if not already defined (set to Windows 10)
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <windowsx.h>  // For GET_X_LPARAM, GET_Y_LPARAM macros
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

// Fallback: if SetProcessDpiAwarenessContext is not defined, use SetProcessDPIAware.
#ifndef SetProcessDpiAwarenessContext
#define SetProcessDpiAwarenessContext(x) SetProcessDPIAware()
#endif

#include <gdiplus.h>
#include <shellscalingapi.h>  // For DPI functions
#include <iostream>
#include <sstream>
#include <vector>
#include <string>
#include <iomanip>
#include <ctime>
#include <algorithm>
#include <chrono>
#include <thread>

#pragma comment (lib, "gdiplus.lib")
#pragma comment (lib, "Shcore.lib")  // For DPI functions

using namespace Gdiplus;

//---------------------------------------------------------------------
// Global variable to store the selected rectangle (interactive mode)
static RECT g_selRect = { 0, 0, 0, 0 };

//---------------------------------------------------------------------
// Window Procedure for the interactive selection overlay.
LRESULT CALLBACK SelectionWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    static POINT startPoint = { 0, 0 };
    switch (message)
    {
    case WM_LBUTTONDOWN:
        SetCapture(hWnd);
        startPoint.x = GET_X_LPARAM(lParam);
        startPoint.y = GET_Y_LPARAM(lParam);
        g_selRect.left = startPoint.x;
        g_selRect.top = startPoint.y;
        g_selRect.right = startPoint.x;
        g_selRect.bottom = startPoint.y;
        InvalidateRect(hWnd, NULL, TRUE);
        break;
    case WM_MOUSEMOVE:
        if (wParam & MK_LBUTTON)
        {
            POINT currentPoint;
            currentPoint.x = GET_X_LPARAM(lParam);
            currentPoint.y = GET_Y_LPARAM(lParam);
            g_selRect.left = min(startPoint.x, currentPoint.x);
            g_selRect.top = min(startPoint.y, currentPoint.y);
            g_selRect.right = max(startPoint.x, currentPoint.x);
            g_selRect.bottom = max(startPoint.y, currentPoint.y);
            InvalidateRect(hWnd, NULL, TRUE);
        }
        break;
    case WM_LBUTTONUP:
        ReleaseCapture();
        PostQuitMessage(0);
        break;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        // Draw semi-transparent black overlay.
        HBRUSH hOverlay = CreateSolidBrush(RGB(0, 0, 0));
        SetBkMode(hdc, TRANSPARENT);
        FillRect(hdc, &ps.rcPaint, hOverlay);
        DeleteObject(hOverlay);
        // Draw red selection rectangle.
        HPEN hPen = CreatePen(PS_SOLID, 2, RGB(255, 0, 0));
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
        Rectangle(hdc, g_selRect.left, g_selRect.top, g_selRect.right, g_selRect.bottom);
        SelectObject(hdc, hOldPen);
        DeleteObject(hPen);
        EndPaint(hWnd, &ps);
    }
    break;
    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

//---------------------------------------------------------------------
// Create a full-screen overlay for interactive selection and return the selected RECT.
RECT GetSelectionRect(HINSTANCE hInstance)
{
    const wchar_t CLASS_NAME[] = L"SelectionWindowClass";
    WNDCLASS wc = { };
    wc.lpfnWndProc = SelectionWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_CROSS); // Use crosshair cursor
    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(
        WS_EX_TOPMOST | WS_EX_LAYERED,
        CLASS_NAME, L"Select Area", WS_POPUP,
        0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
        NULL, NULL, hInstance, NULL
    );
    if (!hwnd)
    {
        std::cerr << "Failed to create selection window." << std::endl;
        RECT r = { 0, 0, 0, 0 };
        return r;
    }
    // Set window opacity to 50%.
    SetLayeredWindowAttributes(hwnd, 0, (BYTE)(255 * 0.5), LWA_ALPHA);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    DestroyWindow(hwnd);
    return g_selRect;
}

//---------------------------------------------------------------------
// Helper: Convert an HBITMAP to a DIB stored in global memory (for clipboard).
HGLOBAL CreateDIBFromHBITMAP(HBITMAP hBitmap)
{
    BITMAP bm;
    if (!GetObject(hBitmap, sizeof(bm), &bm))
        return NULL;
    BITMAPINFOHEADER bi;
    ZeroMemory(&bi, sizeof(bi));
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = bm.bmWidth;
    bi.biHeight = bm.bmHeight;
    bi.biPlanes = 1;
    bi.biBitCount = bm.bmBitsPixel;
    bi.biCompression = BI_RGB;
    int lineBytes = ((bm.bmWidth * bm.bmBitsPixel + 31) & ~31) / 8;
    DWORD dwSize = lineBytes * bm.bmHeight;
    DWORD dwMemSize = sizeof(BITMAPINFOHEADER) + dwSize;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, dwMemSize);
    if (!hMem)
        return NULL;
    LPVOID pMem = GlobalLock(hMem);
    if (!pMem)
    {
        GlobalFree(hMem);
        return NULL;
    }
    memcpy(pMem, &bi, sizeof(BITMAPINFOHEADER));
    LPVOID pBits = (LPBYTE)pMem + sizeof(BITMAPINFOHEADER);
    HDC hDC = GetDC(NULL);
    if (!hDC)
    {
        GlobalUnlock(hMem);
        GlobalFree(hMem);
        return NULL;
    }
    BITMAPINFO* pbi = (BITMAPINFO*)pMem;
    if (!GetDIBits(hDC, hBitmap, 0, bm.bmHeight, pBits, pbi, DIB_RGB_COLORS))
    {
        ReleaseDC(NULL, hDC);
        GlobalUnlock(hMem);
        GlobalFree(hMem);
        return NULL;
    }
    ReleaseDC(NULL, hDC);
    GlobalUnlock(hMem);
    return hMem;
}

//---------------------------------------------------------------------
// Helper: Retrieve the CLSID of an image encoder (e.g., PNG, JPEG, BMP).
int GetEncoderClsid(const WCHAR* format, CLSID* pClsid)
{
    UINT num = 0, size = 0;
    if (GetImageEncodersSize(&num, &size) != Ok || size == 0)
        return -1;

    // Allocate the correct number of bytes.
    ImageCodecInfo* pImageCodecInfo = reinterpret_cast<ImageCodecInfo*>(malloc(size));
    if (!pImageCodecInfo)
        return -1;

    if (GetImageEncoders(num, size, pImageCodecInfo) != Ok) {
        free(pImageCodecInfo);
        return -1;
    }

    int retVal = -1;
    for (UINT j = 0; j < num; ++j)
    {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0)
        {
            *pClsid = pImageCodecInfo[j].Clsid;
            retVal = static_cast<int>(j);
            break;
        }
    }
    free(pImageCodecInfo);
    return retVal;
}


//---------------------------------------------------------------------
// Print usage instructions.
void printUsage()
{
    std::cout << "Usage: ShotCap.exe [options]\n"
        << "Options:\n"
        << "  -f <filename>         Output file name (default: screenshot.png)\n"
        << "  -dir <directory>      Output directory (default: current directory)\n"
        << "  -d <delay>            Delay in seconds before capturing (default: 0)\n"
        << "  -r <x,y,w,h>          Capture region (default: full screen)\n"
        << "  -select               Interactively select a region with the mouse\n"
        << "  -format <format>      Image format: png, jpg, bmp (default: png)\n"
        << "  -quality <0-100>      JPEG quality (only for -format jpg, default: 90)\n"
        << "  -w <window_title>     Capture a specific window by its title\n"
        << "  -active               Capture the active (foreground) window\n"
        << "  -m <monitor_index>    Capture a specific monitor (0-based index)\n"
        << "  -clipboard            Copy captured image to clipboard\n"
        << "  -show                 Open the captured image after saving\n"
        << "  -p                    Include the mouse pointer in the screenshot\n"
        << "  -timestamp            Annotate screenshot with current date/time\n"
        << "  -repeat <i> <n>       Repeat capture every i seconds for n times\n"
        << "  -listmonitors         List available monitors and exit\n"
        << "  -listwindows [N]      List titled top-level windows (titles usable with\n"
        << "                        -w) and exit. Optional N caps output to first N\n"
        << "                        entries from top of z-order.\n"
        << "  -listallwindows       Debug: list ALL top-level windows (visible+hidden,\n"
        << "                        titled+untitled, with class + rect) and exit\n"
        << "  -inspect <file>       Open <file> in pixel-inspector window. Click 3 pixels\n"
        << "                        (or Space at crosshair) to emit a pix3eq fingerprint line\n"
        << "                        to stdout AND clipboard. Mouse/arrows move crosshair,\n"
        << "                        +/- zoom, R resets samples, ESC exits.\n"
        << "  -vl                   Enable verbose logging\n"
        << "  -v, --version         Show current shotcap version\n"
        << "  -h, --help            Display this help message\n";
}

//---------------------------------------------------------------------
// --- -inspect mode: pixel inspector for building pix3eq fingerprints
//---------------------------------------------------------------------
// Opens a captured image in a window with a crosshair you can move via
// mouse or arrows; click/Space records a (x, y, color) sample; after 3
// samples it emits a pix3eq-ready script line to stdout AND clipboard.

struct InspectSample {
    int x, y;
    COLORREF color;
};

struct InspectState {
    Gdiplus::Bitmap* image = nullptr;
    int imgW = 0, imgH = 0;
    int zoom = 8;
    int cursorX = 0, cursorY = 0;
    int panX = 0, panY = 0;
    std::vector<InspectSample> samples;
};
static InspectState g_insp;
static const int INSP_STATUS_H = 30;

static COLORREF inspReadPixel(int x, int y)
{
    if (!g_insp.image || x < 0 || y < 0 || x >= g_insp.imgW || y >= g_insp.imgH)
        return 0;
    Gdiplus::Color c;
    if (g_insp.image->GetPixel(x, y, &c) != Gdiplus::Ok) return 0;
    return RGB(c.GetR(), c.GetG(), c.GetB());
}

static void inspAutoPan(HWND hWnd)
{
    RECT rc; GetClientRect(hWnd, &rc);
    int clientW = rc.right, clientH = rc.bottom - INSP_STATUS_H;
    int cx = g_insp.cursorX * g_insp.zoom + g_insp.zoom / 2;
    int cy = g_insp.cursorY * g_insp.zoom + g_insp.zoom / 2;
    const int M = 50;
    if (cx - g_insp.panX < M) g_insp.panX = cx - M;
    if (cx - g_insp.panX > clientW - M) g_insp.panX = cx - clientW + M;
    if (cy - g_insp.panY < M) g_insp.panY = cy - M;
    if (cy - g_insp.panY > clientH - M) g_insp.panY = cy - clientH + M;
    if (g_insp.panX < 0) g_insp.panX = 0;
    if (g_insp.panY < 0) g_insp.panY = 0;
}

static void inspEmitReady(HWND hWnd)
{
    if (g_insp.samples.size() < 3) return;
    char buf[512];
    sprintf_s(buf, sizeof(buf),
        "0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 10 pix3eq\r\n",
        g_insp.samples[0].x, g_insp.samples[0].y, (unsigned)g_insp.samples[0].color,
        g_insp.samples[1].x, g_insp.samples[1].y, (unsigned)g_insp.samples[1].color,
        g_insp.samples[2].x, g_insp.samples[2].y, (unsigned)g_insp.samples[2].color);
    std::cout << buf;
    if (OpenClipboard(hWnd)) {
        EmptyClipboard();
        size_t len = strlen(buf);
        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, len + 1);
        if (hg) {
            char* p = (char*)GlobalLock(hg);
            memcpy(p, buf, len + 1);
            GlobalUnlock(hg);
            SetClipboardData(CF_TEXT, hg);
        }
        CloseClipboard();
    }
}

static LRESULT CALLBACK InspectWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc; GetClientRect(hWnd, &rc);

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBM = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HGDIOBJ oldBM = SelectObject(memDC, memBM);
        FillRect(memDC, &rc, (HBRUSH)(COLOR_BTNFACE + 1));

        if (g_insp.image) {
            Gdiplus::Graphics g(memDC);
            g.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            int scaledW = g_insp.imgW * g_insp.zoom;
            int scaledH = g_insp.imgH * g_insp.zoom;
            g.DrawImage(g_insp.image, -g_insp.panX, INSP_STATUS_H - g_insp.panY, scaledW, scaledH);

            HPEN penM = CreatePen(PS_SOLID, 1, RGB(255, 0, 255));
            HGDIOBJ oldPen = SelectObject(memDC, penM);
            HGDIOBJ oldBr = SelectObject(memDC, GetStockObject(NULL_BRUSH));
            for (auto& s : g_insp.samples) {
                int sx = s.x * g_insp.zoom + g_insp.zoom / 2 - g_insp.panX;
                int sy = INSP_STATUS_H + s.y * g_insp.zoom + g_insp.zoom / 2 - g_insp.panY;
                Rectangle(memDC, sx - 8, sy - 8, sx + 8, sy + 8);
            }
            HPEN penY = CreatePen(PS_SOLID, 1, RGB(255, 255, 0));
            SelectObject(memDC, penY);
            int cx = g_insp.cursorX * g_insp.zoom + g_insp.zoom / 2 - g_insp.panX;
            int cy = INSP_STATUS_H + g_insp.cursorY * g_insp.zoom + g_insp.zoom / 2 - g_insp.panY;
            MoveToEx(memDC, cx - 20, cy, NULL); LineTo(memDC, cx + 20, cy);
            MoveToEx(memDC, cx, cy - 20, NULL); LineTo(memDC, cx, cy + 20);
            SelectObject(memDC, oldPen); SelectObject(memDC, oldBr);
            DeleteObject(penM); DeleteObject(penY);
        }

        RECT sr = { 0, 0, rc.right, INSP_STATUS_H };
        FillRect(memDC, &sr, (HBRUSH)GetStockObject(WHITE_BRUSH));
        COLORREF col = inspReadPixel(g_insp.cursorX, g_insp.cursorY);
        char statText[256];
        sprintf_s(statText, sizeof(statText),
            "x=0x%04X y=0x%04X color=0x%06X  zoom=%dx  samples=%zu/3   "
            "(mouse/arrows move, click/Space=record, +/-=zoom, R=reset, ESC=exit)",
            g_insp.cursorX, g_insp.cursorY, (unsigned)col, g_insp.zoom, g_insp.samples.size());
        SetBkMode(memDC, TRANSPARENT);
        TextOutA(memDC, 10, 8, statText, (int)strlen(statText));

        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBM);
        DeleteObject(memBM);
        DeleteDC(memDC);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
        int ix = (mx + g_insp.panX) / g_insp.zoom;
        int iy = (my - INSP_STATUS_H + g_insp.panY) / g_insp.zoom;
        if (ix >= 0 && iy >= 0 && ix < g_insp.imgW && iy < g_insp.imgH) {
            g_insp.cursorX = ix; g_insp.cursorY = iy;
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (g_insp.samples.size() < 3) {
            COLORREF c = inspReadPixel(g_insp.cursorX, g_insp.cursorY);
            g_insp.samples.push_back({ g_insp.cursorX, g_insp.cursorY, c });
            if (g_insp.samples.size() == 3) inspEmitReady(hWnd);
            InvalidateRect(hWnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        int step = (GetKeyState(VK_SHIFT) & 0x8000) ? 10 : 1;
        bool moved = false;
        switch (wp) {
        case VK_LEFT:  g_insp.cursorX -= step; moved = true; break;
        case VK_RIGHT: g_insp.cursorX += step; moved = true; break;
        case VK_UP:    g_insp.cursorY -= step; moved = true; break;
        case VK_DOWN:  g_insp.cursorY += step; moved = true; break;
        case VK_SPACE:
            if (g_insp.samples.size() < 3) {
                COLORREF c = inspReadPixel(g_insp.cursorX, g_insp.cursorY);
                g_insp.samples.push_back({ g_insp.cursorX, g_insp.cursorY, c });
                if (g_insp.samples.size() == 3) inspEmitReady(hWnd);
            }
            break;
        case 'R': g_insp.samples.clear(); break;
        case VK_OEM_PLUS: case VK_ADD:
            if (g_insp.zoom < 32) g_insp.zoom *= 2; break;
        case VK_OEM_MINUS: case VK_SUBTRACT:
            if (g_insp.zoom > 1) g_insp.zoom /= 2; break;
        case VK_ESCAPE: DestroyWindow(hWnd); return 0;
        }
        if (g_insp.cursorX < 0) g_insp.cursorX = 0;
        if (g_insp.cursorY < 0) g_insp.cursorY = 0;
        if (g_insp.cursorX >= g_insp.imgW) g_insp.cursorX = g_insp.imgW - 1;
        if (g_insp.cursorY >= g_insp.imgH) g_insp.cursorY = g_insp.imgH - 1;
        if (moved) inspAutoPan(hWnd);
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;
    }
    case WM_CLOSE:   DestroyWindow(hWnd); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hWnd, msg, wp, lp);
}

static int runInspectMode(const std::wstring& filePath)
{
    g_insp.image = Gdiplus::Bitmap::FromFile(filePath.c_str());
    if (!g_insp.image || g_insp.image->GetLastStatus() != Gdiplus::Ok) {
        std::wcerr << L"[ERROR] Could not load image: " << filePath << std::endl;
        return 1;
    }
    g_insp.imgW = g_insp.image->GetWidth();
    g_insp.imgH = g_insp.image->GetHeight();

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = InspectWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"ShotCapInspect";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassExW(&wc);

    int wndW = (std::min)(1600, g_insp.imgW * g_insp.zoom + 16);
    int wndH = (std::min)(1000, g_insp.imgH * g_insp.zoom + INSP_STATUS_H + 39);

    HWND hWnd = CreateWindowExW(0, L"ShotCapInspect", L"ShotCap Inspect — pix3eq fingerprint builder",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, wndW, wndH,
        NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hWnd, SW_SHOWNORMAL);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg); DispatchMessage(&msg);
    }
    delete g_insp.image;
    g_insp.image = nullptr;
    return 0;
}

//---------------------------------------------------------------------
// Utility: Split a string by a delimiter.
std::vector<std::string> split(const std::string& s, char delimiter)
{
    std::vector<std::string> tokens;
    std::istringstream tokenStream(s);
    std::string token;
    while (std::getline(tokenStream, token, delimiter))
        tokens.push_back(token);
    return tokens;
}

//---------------------------------------------------------------------
// Structure to store monitor information.
struct MonitorInfo {
    RECT rect;
};

// Callback for enumerating monitors.
BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC, LPRECT lprcMonitor, LPARAM dwData)
{
    std::vector<MonitorInfo>* monitors = reinterpret_cast<std::vector<MonitorInfo>*>(dwData);
    MonitorInfo mi;
    mi.rect = *lprcMonitor;
    monitors->push_back(mi);
    return TRUE;
}

//---------------------------------------------------------------------
// Callback for -listwindows: titled top-level windows. We deliberately
// DON'T filter on IsWindowVisible — UWP/WinUI/Edge/Chrome often flag
// their visible HWND as hidden (cloaked by DWM) so IsWindowVisible
// returns 0 even though the window is on screen. A non-empty title is
// the better usability filter for picking a -w argument.
//
// Supports optional count cap via -listwindows N: stops enumeration
// after writing N entries.  EnumWindows walks top-of-z-order first,
// so the cap gives you the foreground/most-recent windows.
struct ListWinCtx { std::wstringstream* ss; int limit; int count; };

BOOL CALLBACK EnumWindowsProc(HWND hWnd, LPARAM lParam)
{
    ListWinCtx* ctx = reinterpret_cast<ListWinCtx*>(lParam);
    if (ctx->limit > 0 && ctx->count >= ctx->limit) return FALSE;
    wchar_t title[256] = { 0 };
    GetWindowTextW(hWnd, title, 256);
    if (wcslen(title) == 0) return TRUE;
    *ctx->ss << L"Handle: " << hWnd << L" | Title: " << title << std::endl;
    ctx->count++;
    return TRUE;
}

//---------------------------------------------------------------------
// Callback for -listallwindows: every top-level HWND returned by
// EnumWindows, including hidden + untitled.  Adds class + rect columns
// for debug.  WinSpy/Window-Detective in lib_cf22_h/ahk give a richer
// tree view if you need more.
BOOL CALLBACK EnumAllWindowsProc(HWND hWnd, LPARAM lParam)
{
    wchar_t title[256] = { 0 };
    wchar_t cls[128] = { 0 };
    GetWindowTextW(hWnd, title, 256);
    GetClassNameW(hWnd, cls, 128);
    RECT r = { 0 };
    GetWindowRect(hWnd, &r);
    wchar_t vis = IsWindowVisible(hWnd) ? L'V' : L'H';

    std::wstringstream* ss = reinterpret_cast<std::wstringstream*>(lParam);
    *ss << L"  [0x" << std::hex << std::setw(8) << std::setfill(L'0')
        << (uintptr_t)hWnd << std::dec << std::setfill(L' ')
        << L"] " << vis
        << L" cls=\"" << cls << L"\""
        << L" title=\"" << title << L"\""
        << L" rect=(" << r.left << L"," << r.top
        << L"," << (r.right - r.left) << L"x" << (r.bottom - r.top) << L")"
        << std::endl;
    return TRUE;
}

//---------------------------------------------------------------------
// Annotate the captured image with a timestamp (or custom text) at bottom-right.
void AnnotateImage(Bitmap* bmp, const std::wstring& text, bool verbose)
{
    Graphics graphics(bmp);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(TextRenderingHintAntiAlias);

    Font font(L"Arial", 20);
    SolidBrush brush(Color(255, 255, 255, 255)); // White
    SolidBrush shadowBrush(Color(128, 0, 0, 0));   // Black shadow

    RectF layoutRect(0, 0, static_cast<REAL>(bmp->GetWidth()), static_cast<REAL>(bmp->GetHeight()));
    StringFormat format;
    format.SetAlignment(StringAlignmentFar);
    format.SetLineAlignment(StringAlignmentFar);
    RectF bound;
    graphics.MeasureString(text.c_str(), -1, &font, layoutRect, &format, &bound);

    float margin = 10.0f;
    PointF position(static_cast<REAL>(bmp->GetWidth()) - bound.Width - margin,
        static_cast<REAL>(bmp->GetHeight()) - bound.Height - margin);

    graphics.DrawString(text.c_str(), -1, &font, PointF(position.X + 2, position.Y + 2), &format, &shadowBrush);
    graphics.DrawString(text.c_str(), -1, &font, position, &format, &brush);

    if (verbose)
        std::wcout << L"[INFO] Timestamp annotation applied: " << text << std::endl;
}

//---------------------------------------------------------------------
// Main function.
int main(int argc, char* argv[])
{
    // --- DPI Awareness Setup ---
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        SetProcessDPIAware();
    // --------------------------------

    // Default parameters.
    std::wstring outputFile = L"screenshot.png";
    std::wstring outputDir = L""; // Current directory if empty.
    double delaySeconds = 0.0;
    bool regionSpecified = false;
    int regionX = 0, regionY = 0, regionW = 0, regionH = 0;
    std::wstring imageFormat = L"png";
    std::wstring windowTitle = L"";
    int monitorIndex = -1;
    bool copyToClipboard = false;
    bool showAfterCapture = false;
    bool capturePointer = false;
    bool annotateTimestamp = false;
    bool captureActiveWindow = false;
    bool repeatEnabled = false;
    double repeatInterval = 0.0;
    int repeatCount = 0;
    int jpegQuality = 90;
    bool verbose = false;
    bool listMonitors = false;
    bool listWindows = false;
    int  listWindowsLimit = 0;          // 0 = unlimited
    bool listAllWindows = false;
    bool interactiveSelect = false;
    std::wstring inspectFile;

    // Parse command-line arguments.
    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help")
        {
            printUsage();
            return 0;
        }
        else if (arg == "--version" || arg == "-v")
        {
            std::wcout << L"ShotCap version 1.3\n";
            return 0;
        }
        else if (arg == "-f" && i + 1 < argc)
        {
            int len = MultiByteToWideChar(CP_UTF8, 0, argv[i + 1], -1, NULL, 0);
            wchar_t* buffer = new wchar_t[len];
            MultiByteToWideChar(CP_UTF8, 0, argv[i + 1], -1, buffer, len);
            outputFile = buffer;
            delete[] buffer;
            i++;
        }
        else if (arg == "-dir" && i + 1 < argc)
        {
            int len = MultiByteToWideChar(CP_UTF8, 0, argv[i + 1], -1, NULL, 0);
            wchar_t* buffer = new wchar_t[len];
            MultiByteToWideChar(CP_UTF8, 0, argv[i + 1], -1, buffer, len);
            outputDir = buffer;
            delete[] buffer;
            i++;
        }
        else if (arg == "-d" && i + 1 < argc)
        {
            delaySeconds = std::stod(argv[i + 1]);
            i++;
        }
        else if (arg == "-r" && i + 1 < argc)
        {
            auto parts = split(argv[i + 1], ',');
            if (parts.size() == 4)
            {
                regionX = std::stoi(parts[0]);
                regionY = std::stoi(parts[1]);
                regionW = std::stoi(parts[2]);
                regionH = std::stoi(parts[3]);
                regionSpecified = true;
            }
            else
            {
                std::cerr << "Invalid region specification. Expected format: x,y,w,h\n";
                return -1;
            }
            i++;
        }
        else if (arg == "-select")
        {
            interactiveSelect = true;
        }
        else if (arg == "-format" && i + 1 < argc)
        {
            std::string fmt = argv[i + 1];
            std::transform(fmt.begin(), fmt.end(), fmt.begin(), ::tolower);
            if (fmt == "png" || fmt == "jpg" || fmt == "bmp")
            {
                int len = MultiByteToWideChar(CP_UTF8, 0, fmt.c_str(), -1, NULL, 0);
                wchar_t* buffer = new wchar_t[len];
                MultiByteToWideChar(CP_UTF8, 0, fmt.c_str(), -1, buffer, len);
                imageFormat = buffer;
                delete[] buffer;
            }
            else
            {
                std::cerr << "Unsupported image format. Supported formats: png, jpg, bmp\n";
                return -1;
            }
            i++;
        }
        else if (arg == "-quality" && i + 1 < argc)
        {
            jpegQuality = std::atoi(argv[i + 1]);
            if (jpegQuality < 0 || jpegQuality > 100)
            {
                std::cerr << "Quality must be between 0 and 100.\n";
                return -1;
            }
            i++;
        }
        else if (arg == "-w" && i + 1 < argc)
        {
            int len = MultiByteToWideChar(CP_UTF8, 0, argv[i + 1], -1, NULL, 0);
            wchar_t* buffer = new wchar_t[len];
            MultiByteToWideChar(CP_UTF8, 0, argv[i + 1], -1, buffer, len);
            windowTitle = buffer;
            delete[] buffer;
            i++;
        }
        else if (arg == "-active")
        {
            captureActiveWindow = true;
        }
        else if (arg == "-m" && i + 1 < argc)
        {
            monitorIndex = std::atoi(argv[i + 1]);
            i++;
        }
        else if (arg == "-clipboard")
        {
            copyToClipboard = true;
        }
        else if (arg == "-show")
        {
            showAfterCapture = true;
        }
        else if (arg == "-inspect" && i + 1 < argc)
        {
            int len = MultiByteToWideChar(CP_UTF8, 0, argv[i + 1], -1, NULL, 0);
            wchar_t* buffer = new wchar_t[len];
            MultiByteToWideChar(CP_UTF8, 0, argv[i + 1], -1, buffer, len);
            inspectFile = buffer;
            delete[] buffer;
            i++;
        }
        else if (arg == "-p")
        {
            capturePointer = true;
        }
        else if (arg == "-timestamp")
        {
            annotateTimestamp = true;
        }
        else if (arg == "-repeat" && i + 2 < argc)
        {
            repeatInterval = std::stod(argv[i + 1]);
            repeatCount = std::atoi(argv[i + 2]);
            repeatEnabled = true;
            i += 2;
        }
        else if (arg == "-listmonitors")
        {
            listMonitors = true;
        }
        else if (arg == "-listwindows")
        {
            listWindows = true;
            // Optional positive integer cap: -listwindows 10
            if (i + 1 < argc) {
                char* end = nullptr;
                long n = std::strtol(argv[i + 1], &end, 10);
                if (end != argv[i + 1] && *end == '\0' && n > 0) {
                    listWindowsLimit = (int)n;
                    ++i;
                }
            }
        }
        else if (arg == "-listallwindows")
        {
            listAllWindows = true;
        }
        else if (arg == "-vl")
        {
            verbose = true;
        }
        else
        {
            std::cerr << "Unknown argument: " << arg << "\n";
            printUsage();
            return -1;
        }
    }

    // List monitors if requested.
    if (listMonitors)
    {
        std::vector<MonitorInfo> monitors;
        if (EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, (LPARAM)&monitors))
        {
            std::wcout << L"Monitors available:\n";
            for (size_t i = 0; i < monitors.size(); i++)
            {
                RECT r = monitors[i].rect;
                std::wcout << L"  [" << i << L"] (" << r.left << L"," << r.top << L") - ("
                    << r.right << L"," << r.bottom << L")\n";
            }
        }
        else
        {
            std::wcerr << L"Failed to enumerate monitors.\n";
        }
        return 0;
    }

    // List windows if requested.
    if (listWindows)
    {
        std::wstringstream ss;
        ListWinCtx ctx { &ss, listWindowsLimit, 0 };
        EnumWindows(EnumWindowsProc, (LPARAM)&ctx);
        std::wcout << L"Visible windows";
        if (listWindowsLimit > 0)
            std::wcout << L" (top " << ctx.count << L" of cap " << listWindowsLimit << L")";
        std::wcout << L":\n" << ss.str();
        return 0;
    }
    if (listAllWindows)
    {
        std::wstringstream ss;
        EnumWindows(EnumAllWindowsProc, (LPARAM)&ss);
        std::wcout << L"All top-level windows (V=visible, H=hidden):\n" << ss.str();
        return 0;
    }

    // If interactive selection is enabled, override region settings.
    if (interactiveSelect)
    {
        if (verbose)
            std::wcout << L"[INFO] Entering interactive selection mode...\n";
        HINSTANCE hInstance = GetModuleHandle(NULL);
        RECT selRect = GetSelectionRect(hInstance);
        regionX = selRect.left;
        regionY = selRect.top;
        regionW = selRect.right - selRect.left;
        regionH = selRect.bottom - selRect.top;
        regionSpecified = true;
        if (verbose)
        {
            std::wcout << L"[INFO] Selected region: ("
                << regionX << L"," << regionY << L","
                << regionW << L"," << regionH << L")\n";
        }
    }

    if (verbose)
        std::wcout << L"[INFO] Starting ShotCap...\n";

    if (delaySeconds > 0)
    {
        if (verbose)
            std::wcout << L"[INFO] Waiting for " << delaySeconds << L" seconds before capturing...\n";
        Sleep(static_cast<DWORD>(delaySeconds * 1000));
    }

    // Initialize GDI+.
    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    if (GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL) != Ok)
    {
        std::cerr << "Failed to initialize GDI+." << std::endl;
        return -1;
    }

    // -inspect short-circuits the capture pipeline.
    if (!inspectFile.empty())
    {
        int rc = runInspectMode(inspectFile);
        GdiplusShutdown(gdiplusToken);
        return rc;
    }

    // Lambda: Capture and save a screenshot using current settings.
    auto captureAndSave = [&](const std::wstring& fileName) -> bool
        {
            HDC hSourceDC = nullptr;
            RECT captureRect = { 0, 0, 0, 0 };

            // For active window or specified window, use PrintWindow.
            if (captureActiveWindow || !windowTitle.empty())
            {
                HWND hWnd = captureActiveWindow ? GetForegroundWindow() : FindWindowW(NULL, windowTitle.c_str());
                if (!hWnd)
                {
                    std::wcerr << L"Window not found." << std::endl;
                    return false;
                }
                if (verbose)
                    std::wcout << L"[INFO] Capturing window." << std::endl;
                if (!GetWindowRect(hWnd, &captureRect))
                {
                    std::cerr << "Failed to get window rect." << std::endl;
                    return false;
                }
                hSourceDC = GetDC(hWnd);
                if (!hSourceDC)
                {
                    std::cerr << "Failed to get window DC." << std::endl;
                    return false;
                }
            }
            else if (monitorIndex != -1)
            {
                std::vector<MonitorInfo> monitors;
                if (!EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, (LPARAM)&monitors))
                {
                    std::cerr << "Failed to enumerate monitors." << std::endl;
                    return false;
                }
                if (monitorIndex < 0 || monitorIndex >= static_cast<int>(monitors.size()))
                {
                    std::cerr << "Invalid monitor index." << std::endl;
                    return false;
                }
                captureRect = monitors[monitorIndex].rect;
                HWND hDesktopWnd = GetDesktopWindow();
                hSourceDC = GetDC(hDesktopWnd);
                if (!hSourceDC)
                {
                    std::cerr << "Failed to get desktop DC." << std::endl;
                    return false;
                }
            }
            else if (regionSpecified)
            {
                captureRect.left = regionX;
                captureRect.top = regionY;
                captureRect.right = regionX + regionW;
                captureRect.bottom = regionY + regionH;
                HWND hDesktopWnd = GetDesktopWindow();
                hSourceDC = GetDC(hDesktopWnd);
                if (!hSourceDC)
                {
                    std::cerr << "Failed to get desktop DC." << std::endl;
                    return false;
                }
            }
            else
            {
                HWND hDesktopWnd = GetDesktopWindow();
                hSourceDC = GetDC(hDesktopWnd);
                if (!hSourceDC)
                {
                    std::cerr << "Failed to get desktop DC." << std::endl;
                    return false;
                }
                captureRect.left = 0;
                captureRect.top = 0;
                captureRect.right = GetSystemMetrics(SM_CXSCREEN);
                captureRect.bottom = GetSystemMetrics(SM_CYSCREEN);
            }

            int capW = captureRect.right - captureRect.left;
            int capH = captureRect.bottom - captureRect.top;
            if (verbose)
                std::wcout << L"[INFO] Capture dimensions: " << capW << L"x" << capH << std::endl;

            HDC hCaptureDC = CreateCompatibleDC(hSourceDC);
            if (!hCaptureDC)
            {
                std::cerr << "Failed to create compatible DC." << std::endl;
                if (hSourceDC)
                    ReleaseDC(NULL, hSourceDC);
                return false;
            }
            HBITMAP hCaptureBitmap = CreateCompatibleBitmap(hSourceDC, capW, capH);
            if (!hCaptureBitmap)
            {
                std::cerr << "Failed to create compatible bitmap." << std::endl;
                DeleteDC(hCaptureDC);
                if (hSourceDC)
                    ReleaseDC(NULL, hSourceDC);
                return false;
            }
            HGDIOBJ hOld = SelectObject(hCaptureDC, hCaptureBitmap);
            if (!hOld)
            {
                std::cerr << "Failed to select bitmap into DC." << std::endl;
                DeleteObject(hCaptureBitmap);
                DeleteDC(hCaptureDC);
                if (hSourceDC)
                    ReleaseDC(NULL, hSourceDC);
                return false;
            }

            // For window capture, try using PrintWindow.
            if (captureActiveWindow || !windowTitle.empty())
            {
                HWND hWnd = captureActiveWindow ? GetForegroundWindow() : FindWindowW(NULL, windowTitle.c_str());
                BOOL printResult = PrintWindow(hWnd, hCaptureDC, PW_RENDERFULLCONTENT);
                if (!printResult)
                {
                    if (verbose)
                        std::wcout << L"[INFO] PW_RENDERFULLCONTENT failed, trying PW_CLIENTONLY...\n";
                    printResult = PrintWindow(hWnd, hCaptureDC, PW_CLIENTONLY);
                }
                if (!printResult)
                {
                    if (verbose)
                        std::wcout << L"[INFO] PrintWindow failed; falling back to BitBlt capture...\n";
                    // Fallback: capture full desktop and crop.
                    SelectObject(hCaptureDC, hOld);
                    DeleteObject(hCaptureBitmap);
                    DeleteDC(hCaptureDC);
                    ReleaseDC(NULL, hSourceDC);
                    HWND hDesktopWnd = GetDesktopWindow();
                    hSourceDC = GetDC(hDesktopWnd);
                    captureRect.left = 0;
                    captureRect.top = 0;
                    captureRect.right = GetSystemMetrics(SM_CXSCREEN);
                    captureRect.bottom = GetSystemMetrics(SM_CYSCREEN);
                    capW = captureRect.right - captureRect.left;
                    capH = captureRect.bottom - captureRect.top;
                    hCaptureDC = CreateCompatibleDC(hSourceDC);
                    hCaptureBitmap = CreateCompatibleBitmap(hSourceDC, capW, capH);
                    hOld = SelectObject(hCaptureDC, hCaptureBitmap);
                    if (!BitBlt(hCaptureDC, 0, 0, capW, capH,
                        hSourceDC, captureRect.left, captureRect.top, SRCCOPY | CAPTUREBLT))
                    {
                        std::cerr << "Fallback BitBlt failed." << std::endl;
                        SelectObject(hCaptureDC, hOld);
                        DeleteObject(hCaptureBitmap);
                        DeleteDC(hCaptureDC);
                        ReleaseDC(NULL, hSourceDC);
                        return false;
                    }
                }
            }
            else
            {
                if (!BitBlt(hCaptureDC, 0, 0, capW, capH,
                    hSourceDC, captureRect.left, captureRect.top, SRCCOPY | CAPTUREBLT))
                {
                    std::cerr << "BitBlt failed." << std::endl;
                    SelectObject(hCaptureDC, hOld);
                    DeleteObject(hCaptureBitmap);
                    DeleteDC(hCaptureDC);
                    ReleaseDC(NULL, hSourceDC);
                    return false;
                }
            }

            if (capturePointer)
            {
                CURSORINFO ci = { 0 };
                ci.cbSize = sizeof(ci);
                if (GetCursorInfo(&ci) && (ci.flags == CURSOR_SHOWING))
                {
                    int iconX = ci.ptScreenPos.x - captureRect.left;
                    int iconY = ci.ptScreenPos.y - captureRect.top;
                    DrawIconEx(hCaptureDC, iconX, iconY, ci.hCursor, 0, 0, 0, NULL, DI_NORMAL);
                    if (verbose)
                        std::wcout << L"[INFO] Mouse pointer drawn.\n";
                }
            }

            if (copyToClipboard)
            {
                if (verbose)
                    std::wcout << L"[INFO] Copying image to clipboard...\n";
                HGLOBAL hDib = CreateDIBFromHBITMAP(hCaptureBitmap);
                if (!hDib)
                {
                    std::cerr << "Failed to create DIB for clipboard." << std::endl;
                }
                else
                {
                    if (OpenClipboard(NULL))
                    {
                        EmptyClipboard();
                        SetClipboardData(CF_DIB, hDib);
                        CloseClipboard();
                        if (verbose)
                            std::wcout << L"[INFO] Image copied to clipboard successfully.\n";
                    }
                    else
                    {
                        std::cerr << "Failed to open clipboard." << std::endl;
                        GlobalFree(hDib);
                    }
                }
            }

            Bitmap* bmp = new Bitmap(hCaptureBitmap, NULL);
            if (!bmp)
            {
                std::cerr << "Failed to create GDI+ Bitmap." << std::endl;
                SelectObject(hCaptureDC, hOld);
                DeleteObject(hCaptureBitmap);
                DeleteDC(hCaptureDC);
                ReleaseDC(NULL, hSourceDC);
                return false;
            }

            if (annotateTimestamp)
            {
                std::time_t t = std::time(nullptr);
                struct tm tmTime;
                localtime_s(&tmTime, &t);
                std::wstringstream ts;
                ts << std::put_time(&tmTime, L"%Y-%m-%d %H:%M:%S");
                AnnotateImage(bmp, ts.str(), verbose);
            }

            const WCHAR* mimeType = L"image/png";
            if (imageFormat == L"jpg")
                mimeType = L"image/jpeg";
            else if (imageFormat == L"bmp")
                mimeType = L"image/bmp";

            CLSID encoderClsid;
            if (GetEncoderClsid(mimeType, &encoderClsid) < 0)
            {
                std::cerr << "Image encoder not found for specified format." << std::endl;
                delete bmp;
                SelectObject(hCaptureDC, hOld);
                DeleteObject(hCaptureBitmap);
                DeleteDC(hCaptureDC);
                ReleaseDC(NULL, hSourceDC);
                return false;
            }

            EncoderParameters encoderParams;
            ULONG qualityParam = jpegQuality;
            if (imageFormat == L"jpg")
            {
                encoderParams.Count = 1;
                encoderParams.Parameter[0].Guid = EncoderQuality;
                encoderParams.Parameter[0].Type = EncoderParameterValueTypeLong;
                encoderParams.Parameter[0].NumberOfValues = 1;
                encoderParams.Parameter[0].Value = &qualityParam;
            }
            else
            {
                encoderParams.Count = 0;
            }

            Status stat;
            if (imageFormat == L"jpg")
                stat = bmp->Save(fileName.c_str(), &encoderClsid, &encoderParams);
            else
                stat = bmp->Save(fileName.c_str(), &encoderClsid, NULL);

            if (stat != Ok)
                std::wcerr << L"Failed to save screenshot (" << fileName << L"). Status code: " << stat << std::endl;
            else
                std::wcout << L"Screenshot saved as " << fileName << std::endl;

            if (showAfterCapture)
            {
                if (verbose)
                    std::wcout << L"[INFO] Opening image...\n";
                ShellExecuteW(NULL, L"open", fileName.c_str(), NULL, NULL, SW_SHOWNORMAL);
            }

            delete bmp;
            SelectObject(hCaptureDC, hOld);
            DeleteObject(hCaptureBitmap);
            DeleteDC(hCaptureDC);
            ReleaseDC(NULL, hSourceDC);
            return true;
        };

        if (repeatEnabled && repeatCount > 0)
        {
            std::wstring baseName = outputFile;
            std::wstring extension = L"";
            size_t pos = outputFile.find_last_of(L'.');
            if (pos != std::wstring::npos)
            {
                baseName = outputFile.substr(0, pos);
                extension = outputFile.substr(pos);
            }
            else
            {
                if (imageFormat == L"jpg")
                    extension = L".jpg";
                else if (imageFormat == L"bmp")
                    extension = L".bmp";
                else
                    extension = L".png";
            }
            auto nextFrameTime = std::chrono::steady_clock::now();
            for (int i = 0; i < repeatCount; i++)
            {
                std::wstringstream ss;
                ss << baseName << L"_" << std::setfill(L'0') << std::setw(3) << i + 1 << extension;
                std::wstring fileName = outputDir.empty() ? ss.str() : (outputDir + L"\\" + ss.str());
                if (!captureAndSave(fileName))
                {
                    std::wcerr << L"[ERROR] Capture iteration " << i + 1 << L" failed.\n";
                }
                nextFrameTime += std::chrono::milliseconds(static_cast<int>(repeatInterval * 1000));
                std::this_thread::sleep_until(nextFrameTime);
            }
        }
        else
        {
            std::wstring fileName = outputDir.empty() ? outputFile : (outputDir + L"\\" + outputFile);
            captureAndSave(fileName);
        }

    GdiplusShutdown(gdiplusToken);
    if (verbose)
        std::wcout << L"[INFO] Done.\n";
    return 0;
}