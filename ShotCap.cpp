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
        << "  -base X,Y             (with -inspect) subtract (X,Y) from emitted coords —\n"
        << "                        emits cell-relative dx,dy values as a comment-marked\n"
        << "                        line for OCR digit-table data. Pair with -inspect.\n"
        << "  -check \"<triplets>\"   Batch verify N pixels: each triplet is\n"
        << "                        dx,dy,0xRRGGBB (space-separated), sampled at\n"
        << "                        (base+dx, base+dy) of the image loaded via -inspect.\n"
        << "                        Logs got/want/diff/MATCH-MISS per row, exit 0 if all\n"
        << "                        within -tol (default 20 per channel). No window opens.\n"
        << "  -tol N                Per-channel tolerance for -check (default 20).\n"
        << "  -grid WxH             Dump a WxH ASCII shape map (. dark / o mid / W light)\n"
        << "                        starting at -base. Lets you see digit shape at a glance.\n"
        << "                        Pair with -inspect <file> and -base X,Y.\n"
        << "  -resize WxH           Preprocess: scale -inspect <input> to WxH and save as\n"
        << "                        -f <output>. Use to normalize a screenshot to a fixed\n"
        << "                        reference resolution before OCR (e.g. 1280x720).\n"
        << "  -crop X,Y,W,H         Preprocess: extract a sub-rect of -inspect <input> and\n"
        << "                        save as -f <output>. Crop runs BEFORE -resize.\n"
        << "  -ocr_test \"<asserts>\"  Run the built-in 3-pixel OCR algorithm at each (x,y)\n"
        << "                        in a space-separated list of \"x,y=expected_digit\" pairs\n"
        << "                        (use -1 for \"no match expected\"). Prints per-row\n"
        << "                        PASS/FAIL. Exit 0 if all pass, 1 otherwise. Pair with\n"
        << "                        -inspect <image> and -tol N (default 20).\n"
        << "  -ocr_scan \"X1,Y1,X2,Y2\" Auto-detect: walk every (x,y) in the rectangle,\n"
        << "                        run the OCR walker. On match, print \"(x,y) digit=N\"\n"
        << "                        and advance x by the matched entry's width to avoid\n"
        << "                        dup-detection. Pair with -inspect <image>. Use this\n"
        << "                        to discover digit base coords without manual hovering.\n"
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
    int baseX = 0, baseY = 0;   // -base X,Y: subtract from emitted coords
    bool baseSet = false;       // controls emit format
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
    // Clamp panX/panY so the image never scrolls past its own edges
    // — without this, holding Right at high zoom keeps growing panX
    // until the visible viewport sits beyond imgW and shows blank.
    int scaledW = g_insp.imgW * g_insp.zoom;
    int scaledH = g_insp.imgH * g_insp.zoom;
    int maxPanX = scaledW - clientW;
    int maxPanY = scaledH - clientH;
    if (maxPanX < 0) maxPanX = 0;
    if (maxPanY < 0) maxPanY = 0;
    if (g_insp.panX > maxPanX) g_insp.panX = maxPanX;
    if (g_insp.panY > maxPanY) g_insp.panY = maxPanY;
    if (g_insp.panX < 0) g_insp.panX = 0;
    if (g_insp.panY < 0) g_insp.panY = 0;
}

static void inspEmitReady(HWND hWnd)
{
    if (g_insp.samples.size() < 3) return;
    char buf[512];
    int bx = g_insp.baseX, by = g_insp.baseY;
    int x1 = g_insp.samples[0].x - bx, y1 = g_insp.samples[0].y - by;
    int x2 = g_insp.samples[1].x - bx, y2 = g_insp.samples[1].y - by;
    int x3 = g_insp.samples[2].x - bx, y3 = g_insp.samples[2].y - by;
    if (g_insp.baseSet) {
        // Table-entry format: cell-relative offsets, comment-marked
        // for direct paste into an OCR digit-table data section.
        sprintf_s(buf, sizeof(buf),
            "; pix3 base=(%d,%d) dx1=0x%X dy1=0x%X c1=0x%06X  dx2=0x%X dy2=0x%X c2=0x%06X  dx3=0x%X dy3=0x%X c3=0x%06X\r\n",
            bx, by,
            x1, y1, (unsigned)g_insp.samples[0].color,
            x2, y2, (unsigned)g_insp.samples[1].color,
            x3, y3, (unsigned)g_insp.samples[2].color);
    } else {
        sprintf_s(buf, sizeof(buf),
            "0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 0x%08X 10 pix3eq\r\n",
            x1, y1, (unsigned)g_insp.samples[0].color,
            x2, y2, (unsigned)g_insp.samples[1].color,
            x3, y3, (unsigned)g_insp.samples[2].color);
    }
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
        char statText[320];
        if (g_insp.baseSet) {
            sprintf_s(statText, sizeof(statText),
                "x=0x%04X y=0x%04X (dx=0x%X dy=0x%X) base=(%d,%d) color=0x%06X  zoom=%dx  samples=%zu/3",
                g_insp.cursorX, g_insp.cursorY,
                g_insp.cursorX - g_insp.baseX, g_insp.cursorY - g_insp.baseY,
                g_insp.baseX, g_insp.baseY,
                (unsigned)col, g_insp.zoom, g_insp.samples.size());
        } else {
            sprintf_s(statText, sizeof(statText),
                "x=0x%04X y=0x%04X color=0x%06X  zoom=%dx  samples=%zu/3   "
                "(mouse/arrows move, click/Space=record, +/-=zoom, R=reset, ESC=exit)",
                g_insp.cursorX, g_insp.cursorY, (unsigned)col, g_insp.zoom, g_insp.samples.size());
        }
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

// -check batch verifier — reads N (dx,dy,expected_color) triplets from
// `spec`, samples each at (baseX+dx, baseY+dy) in the loaded image, and
// logs got/want/diff/MATCH-MISS per row. Returns 0 if every row matches
// within `tol` per channel, 1 otherwise. No window opens — pure CLI.
//
// Spec format: space-separated triplets, each "dx,dy,0xCCC" (commas
// inside, spaces between triplets). E.g.:
//    "1,7,0xA09E80 3,6,0xD8D4AC 7,7,0x979579"
static int runCheckMode(const std::wstring& filePath, const std::string& spec, int tol)
{
    g_insp.image = Gdiplus::Bitmap::FromFile(filePath.c_str());
    if (!g_insp.image || g_insp.image->GetLastStatus() != Gdiplus::Ok) {
        std::wcerr << L"[ERROR] Could not load image: " << filePath << std::endl;
        return 1;
    }
    g_insp.imgW = g_insp.image->GetWidth();
    g_insp.imgH = g_insp.image->GetHeight();

    printf("[CHECK] base=(%d,%d) tol=%d\n", g_insp.baseX, g_insp.baseY, tol);
    int rows = 0, matches = 0;
    std::istringstream iss(spec);
    std::string token;
    while (iss >> token) {
        int dx = 0, dy = 0;
        unsigned want = 0;
        if (sscanf_s(token.c_str(), "%d,%d,0x%x", &dx, &dy, &want) != 3 &&
            sscanf_s(token.c_str(), "%d,%d,%x", &dx, &dy, &want) != 3) {
            fprintf(stderr, "[ERROR] bad triplet: %s (want dx,dy,0xRRGGBB)\n", token.c_str());
            return 1;
        }
        int x = g_insp.baseX + dx;
        int y = g_insp.baseY + dy;
        COLORREF got = inspReadPixel(x, y);
        // COLORREF is 0x00BBGGRR; want is given as 0xRRGGBB so flip the
        // expected so we can diff in matched byte order.
        unsigned want_bgr = ((want & 0xFF) << 16) | (want & 0xFF00) | ((want >> 16) & 0xFF);
        int dR = abs((int)((got >> 0) & 0xFF) - (int)((want_bgr >> 0) & 0xFF));
        int dG = abs((int)((got >> 8) & 0xFF) - (int)((want_bgr >> 8) & 0xFF));
        int dB = abs((int)((got >> 16) & 0xFF) - (int)((want_bgr >> 16) & 0xFF));
        bool match = (dR <= tol && dG <= tol && dB <= tol);
        printf("  (%d,%d) @ (%d,%d) got=0x%06X want=0x%06X diff=(%d,%d,%d) %s\n",
            dx, dy, x, y, (unsigned)got, want, dR, dG, dB,
            match ? "MATCH" : "MISS");
        rows++;
        if (match) matches++;
    }
    printf("[CHECK] %d/%d matched\n", matches, rows);
    delete g_insp.image;
    g_insp.image = nullptr;
    return (matches == rows) ? 0 : 1;
}

// -grid WxH ASCII shape dump — for each (dx,dy) in [0,W) x [0,H), reads
// the image pixel at (base+dx, base+dy) and prints one of:
//   . dark    (luma < 64)
//   o mid     (64-191)
//   W light   (>= 192)
// Top row is dy=0.  Letter labels run x then a numeric row label.  Cuts
// the table-building loop dramatically: I can SEE the digit shape and
// pick discriminating offsets visually instead of grinding 50 hex scans.
static int runGridMode(const std::wstring& filePath, int gridW, int gridH)
{
    g_insp.image = Gdiplus::Bitmap::FromFile(filePath.c_str());
    if (!g_insp.image || g_insp.image->GetLastStatus() != Gdiplus::Ok) {
        std::wcerr << L"[ERROR] Could not load image: " << filePath << std::endl;
        return 1;
    }
    g_insp.imgW = g_insp.image->GetWidth();
    g_insp.imgH = g_insp.image->GetHeight();
    printf("[GRID] base=(%d,%d) size=%dx%d  (. dark / o mid / W light)\n",
        g_insp.baseX, g_insp.baseY, gridW, gridH);
    // x-axis header (mod-10 digits)
    printf("       ");
    for (int x = 0; x < gridW; ++x) printf("%d", x % 10);
    printf("\n");
    for (int dy = 0; dy < gridH; ++dy) {
        printf(" %3d : ", dy);
        for (int dx = 0; dx < gridW; ++dx) {
            COLORREF c = inspReadPixel(g_insp.baseX + dx, g_insp.baseY + dy);
            int r = (c >> 0) & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
            int luma = (r * 30 + g * 59 + b * 11) / 100;
            char ch = (luma < 64) ? '.' : (luma >= 192) ? 'W' : 'o';
            putchar(ch);
        }
        printf("\n");
    }
    delete g_insp.image;
    g_insp.image = nullptr;
    return 0;
}

// Preprocess: load `inFile`, optionally resize to (rw,rh) and/or crop
// (cx,cy,cw,ch), save to `outFile` as PNG.  Used to normalize captured
// screenshots to a fixed reference resolution before OCR — fingerprint
// tables built at the reference resolution then apply universally.
//
// resize is applied AFTER crop (so you crop the region of interest at
// native resolution, then upscale/downscale to the table reference).
static int runPreprocessMode(const std::wstring& inFile,
    const std::wstring& outFile,
    int rw, int rh, int cx, int cy, int cw, int ch)
{
    Gdiplus::Bitmap src(inFile.c_str());
    if (src.GetLastStatus() != Gdiplus::Ok) {
        std::wcerr << L"[ERROR] Could not load image: " << inFile << std::endl;
        return 1;
    }
    int srcW = src.GetWidth(), srcH = src.GetHeight();

    // ---- crop stage ----
    Gdiplus::Bitmap* cropped = nullptr;
    if (cw > 0 && ch > 0) {
        if (cx < 0 || cy < 0 || cx + cw > srcW || cy + ch > srcH) {
            std::cerr << "[ERROR] -crop rect out of bounds (src=" << srcW << "x" << srcH << ")\n";
            return 1;
        }
        Gdiplus::Rect r(cx, cy, cw, ch);
        cropped = src.Clone(r, src.GetPixelFormat());
        printf("[PREPROCESS] cropped (%d,%d,%dx%d) from %dx%d\n", cx, cy, cw, ch, srcW, srcH);
    } else {
        cropped = src.Clone(0, 0, srcW, srcH, src.GetPixelFormat());
    }
    int cropW = cropped->GetWidth(), cropH = cropped->GetHeight();

    // ---- resize stage ----
    Gdiplus::Bitmap* final = cropped;
    if (rw > 0 && rh > 0 && (rw != cropW || rh != cropH)) {
        Gdiplus::Bitmap* resized = new Gdiplus::Bitmap(rw, rh, cropped->GetPixelFormat());
        Gdiplus::Graphics g(resized);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.DrawImage(cropped, 0, 0, rw, rh);
        delete cropped;
        final = resized;
        printf("[PREPROCESS] resized %dx%d -> %dx%d\n", cropW, cropH, rw, rh);
    }

    CLSID encoderClsid;
    if (GetEncoderClsid(L"image/png", &encoderClsid) < 0) {
        std::cerr << "[ERROR] PNG encoder not found.\n";
        delete final;
        return 1;
    }
    Gdiplus::Status st = final->Save(outFile.c_str(), &encoderClsid, NULL);
    delete final;
    if (st != Gdiplus::Ok) {
        std::wcerr << L"[ERROR] Failed to save: " << outFile << std::endl;
        return 1;
    }
    std::wcout << L"[PREPROCESS] wrote " << outFile << std::endl;
    return 0;
}

// --- OCR test harness ------------------------------------------------
// Built-in 3-pixel-fingerprint table.  Each entry: digit value, 3
// (dx,dy,expected_color_RRGGBB), width.  Walks the table on each call,
// returning the first digit whose 3 fingerprint pixels all fall within
// -tol per-channel of expected.  This is the C++ prototype of the
// algorithm we'll port to color_iw.asm once we've validated it here.
//
// Current table (calibrated against tests/coc1.png, "2 942 057" gold
// counter at top-right): digits 2, 9, 4.
struct OcrFP {
    int digit;
    int dx[3], dy[3];
    unsigned c[3];   // 0xRRGGBB
    int width;
};
static const OcrFP g_digit_table[] = {
    // Calibrated colors measured directly from tests/coc1.png — anti-
    // aliased mid-grays for "dark" pixels rather than pure 0x000000.
    // With tight tol the table discriminates without false positives.
    // '2' at base (1680,22)
    { 2, {1,8,1}, {14,23,26}, {0xFFFFFF, 0x19191A, 0xFFFFFF}, 18 },
    // '9' at base (1698,22)
    { 9, {6,8,12}, {17,17,14}, {0x1C1D1D, 0xFEFEFE, 0x383B3A}, 18 },
    // '4' at base (1716,22)
    { 4, {3,12,2}, {23,26,19}, {0xFFFFFF, 0xFCFCFC, 0x303131}, 18 },
    // '0' at base (1748,22) — left wall, hollow center, right wall
    { 0, {0,5,12}, {21,21,21}, {0xFFFFFF, 0x2F2F30, 0xFFFFFF}, 18 },
    // '5' at base (1762,22) — top stroke, dark gap after vertical, body left
    { 5, {4,10,4}, {14,17,25}, {0xFFFFFF, 0x181818, 0xB0B1B1}, 18 },
    // '7' at base (1775,22) — no upper-left body, diagonal mid, dark lower-left
    { 7, {1,5,2}, {18,22,26}, {0x2F3030, 0xEAEAEA, 0x2E2F2F}, 18 },
};
static const int g_digit_table_count =
    sizeof(g_digit_table) / sizeof(g_digit_table[0]);

// Returns the digit at (bx,by) per the table, or -1 if no fingerprint
// matches.  pix3 = 3-pixel COLORREF compare against want with abs-diff
// <= tol per channel.
//
// Jitter retry: each digit is tried at x, x-1, x+1 before moving to
// the next digit.  Mirrors the original sequential scanner's pattern;
// absorbs sub-pixel drift from JPEG compression / anti-alias variation
// without expanding the table.
static int ocrDigitAt(int bx, int by, int tol)
{
    static const int kJitter[3] = { 0, -1, +1 };
    for (int i = 0; i < g_digit_table_count; ++i) {
        const OcrFP& fp = g_digit_table[i];
        for (int j = 0; j < 3; ++j) {
            int xj = bx + kJitter[j];
            bool all_ok = true;
            for (int p = 0; p < 3; ++p) {
                COLORREF got = inspReadPixel(xj + fp.dx[p], by + fp.dy[p]);
                unsigned want = fp.c[p];
                // got is 0x00BBGGRR; want is 0xRRGGBB.  Flip want for diff.
                unsigned want_bgr = ((want & 0xFF) << 16) | (want & 0xFF00) | ((want >> 16) & 0xFF);
                int dR = abs((int)((got >> 0) & 0xFF) - (int)((want_bgr >> 0) & 0xFF));
                int dG = abs((int)((got >> 8) & 0xFF) - (int)((want_bgr >> 8) & 0xFF));
                int dB = abs((int)((got >> 16) & 0xFF) - (int)((want_bgr >> 16) & 0xFF));
                if (dR > tol || dG > tol || dB > tol) { all_ok = false; break; }
            }
            if (all_ok) return fp.digit;
        }
    }
    return -1;
}

// Helper: like ocrDigitAt but also returns the width of the matched
// entry via *outWidth, so the scanner can advance by it cleanly.
static int ocrDigitAtW(int bx, int by, int tol, int* outWidth)
{
    static const int kJitter[3] = { 0, -1, +1 };
    for (int i = 0; i < g_digit_table_count; ++i) {
        const OcrFP& fp = g_digit_table[i];
        for (int j = 0; j < 3; ++j) {
            int xj = bx + kJitter[j];
            bool all_ok = true;
            for (int p = 0; p < 3; ++p) {
                COLORREF got = inspReadPixel(xj + fp.dx[p], by + fp.dy[p]);
                unsigned want = fp.c[p];
                unsigned want_bgr = ((want & 0xFF) << 16) | (want & 0xFF00) | ((want >> 16) & 0xFF);
                int dR = abs((int)((got >> 0) & 0xFF) - (int)((want_bgr >> 0) & 0xFF));
                int dG = abs((int)((got >> 8) & 0xFF) - (int)((want_bgr >> 8) & 0xFF));
                int dB = abs((int)((got >> 16) & 0xFF) - (int)((want_bgr >> 16) & 0xFF));
                if (dR > tol || dG > tol || dB > tol) { all_ok = false; break; }
            }
            if (all_ok) {
                if (outWidth) *outWidth = fp.width;
                return fp.digit;
            }
        }
    }
    if (outWidth) *outWidth = 1;   // miss: advance by 1
    return -1;
}

// -ocr_scan "X1,Y1,X2,Y2"
// Walks every (x, y) in the rectangle [X1..X2) x [Y1..Y2), running the
// OCR walker at each position.  On a match, prints "(x,y) digit=N" and
// advances x by the matched entry's width (avoids the same digit being
// detected at jitter-adjacent positions).  On a miss, advances x by 1.
// Output is a list of detected digits with their image positions — use
// this to auto-discover digit base coordinates without manual hovering.
static int runOcrScanMode(const std::wstring& filePath, const std::string& spec, int tol)
{
    g_insp.image = Gdiplus::Bitmap::FromFile(filePath.c_str());
    if (!g_insp.image || g_insp.image->GetLastStatus() != Gdiplus::Ok) {
        std::wcerr << L"[ERROR] Could not load image: " << filePath << std::endl;
        return 1;
    }
    g_insp.imgW = g_insp.image->GetWidth();
    g_insp.imgH = g_insp.image->GetHeight();
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    if (sscanf_s(spec.c_str(), "%d,%d,%d,%d", &x1, &y1, &x2, &y2) != 4) {
        std::cerr << "[ERROR] -ocr_scan expects X1,Y1,X2,Y2 (e.g. -ocr_scan 1600,15,2000,40)\n";
        return 1;
    }
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > g_insp.imgW) x2 = g_insp.imgW;
    if (y2 > g_insp.imgH) y2 = g_insp.imgH;
    printf("[OCR_SCAN] rect=(%d,%d)-(%d,%d) tol=%d table=%d\n",
        x1, y1, x2, y2, tol, g_digit_table_count);
    int hits = 0;
    for (int y = y1; y < y2; ++y) {
        int x = x1;
        while (x < x2) {
            int width = 1;
            int d = ocrDigitAtW(x, y, tol, &width);
            if (d >= 0) {
                printf("  (%d,%d) digit=%d width=%d\n", x, y, d, width);
                hits++;
                x += width;
            } else {
                x++;
            }
        }
    }
    printf("[OCR_SCAN] %d hit(s)\n", hits);
    delete g_insp.image;
    g_insp.image = nullptr;
    return (hits > 0) ? 0 : 1;
}

// -ocr_test "x1,y1=d1 x2,y2=d2 ..."
// Walks each assertion, runs ocrDigitAt at (x,y), compares to expected
// digit (use -1 for "no match expected").  Prints per-row PASS/FAIL.
// Exit 0 if all pass, 1 if any fail.
static int runOcrTestMode(const std::wstring& filePath, const std::string& spec, int tol)
{
    g_insp.image = Gdiplus::Bitmap::FromFile(filePath.c_str());
    if (!g_insp.image || g_insp.image->GetLastStatus() != Gdiplus::Ok) {
        std::wcerr << L"[ERROR] Could not load image: " << filePath << std::endl;
        return 1;
    }
    g_insp.imgW = g_insp.image->GetWidth();
    g_insp.imgH = g_insp.image->GetHeight();
    printf("[OCR_TEST] tol=%d table=%d digits\n", tol, g_digit_table_count);
    int rows = 0, passes = 0;
    std::istringstream iss(spec);
    std::string token;
    while (iss >> token) {
        int x = 0, y = 0, expected = 0;
        if (sscanf_s(token.c_str(), "%d,%d=%d", &x, &y, &expected) != 3) {
            fprintf(stderr, "[ERROR] bad assertion: %s (want x,y=digit_or_-1)\n", token.c_str());
            return 1;
        }
        int got = ocrDigitAt(x, y, tol);
        bool ok = (got == expected);
        printf("  (%d,%d) got=%-2d expected=%-2d %s\n", x, y, got, expected,
            ok ? "PASS" : "FAIL");
        rows++;
        if (ok) passes++;
    }
    printf("[OCR_TEST] %d/%d passed\n", passes, rows);
    delete g_insp.image;
    g_insp.image = nullptr;
    return (passes == rows) ? 0 : 1;
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
    // Force keyboard focus to the inspect window — without this, when
    // launched from a cmd shell with VS or another foreground app
    // active, the inspect window opens unfocused and arrow keys go to
    // whatever still owns focus.  Mouse click would eventually fix it,
    // but blind keyboard nav is the symptom.
    SetForegroundWindow(hWnd);
    SetFocus(hWnd);

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
    std::string  checkSpec;             // -check <triplets>
    int          checkTol = 15;         // -tol N (default 15 — calibrated table tol)
    int          gridW = 0, gridH = 0;  // -grid WxH (0 = off)
    int          resizeW = 0, resizeH = 0;  // -resize WxH (0 = off)
    int          cropX = 0, cropY = 0, cropW = 0, cropH = 0;  // -crop X,Y,W,H
    std::string  ocrTestSpec;            // -ocr_test "<assertions>"
    std::string  ocrScanSpec;            // -ocr_scan "X1,Y1,X2,Y2"

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
        else if (arg == "-check" && i + 1 < argc)
        {
            checkSpec = argv[i + 1];
            i++;
        }
        else if (arg == "-grid" && i + 1 < argc)
        {
            if (sscanf_s(argv[i + 1], "%dx%d", &gridW, &gridH) != 2 ||
                gridW <= 0 || gridH <= 0) {
                std::cerr << "[ERROR] -grid expects WxH (e.g. -grid 24x20)\n";
                return -1;
            }
            i++;
        }
        else if (arg == "-resize" && i + 1 < argc)
        {
            if (sscanf_s(argv[i + 1], "%dx%d", &resizeW, &resizeH) != 2 ||
                resizeW <= 0 || resizeH <= 0) {
                std::cerr << "[ERROR] -resize expects WxH (e.g. -resize 1280x720)\n";
                return -1;
            }
            i++;
        }
        else if (arg == "-crop" && i + 1 < argc)
        {
            if (sscanf_s(argv[i + 1], "%d,%d,%d,%d", &cropX, &cropY, &cropW, &cropH) != 4 ||
                cropW <= 0 || cropH <= 0) {
                std::cerr << "[ERROR] -crop expects X,Y,W,H (e.g. -crop 100,50,400,300)\n";
                return -1;
            }
            i++;
        }
        else if (arg == "-ocr_test" && i + 1 < argc)
        {
            ocrTestSpec = argv[i + 1];
            i++;
        }
        else if (arg == "-ocr_scan" && i + 1 < argc)
        {
            ocrScanSpec = argv[i + 1];
            i++;
        }
        else if (arg == "-tol" && i + 1 < argc)
        {
            checkTol = std::atoi(argv[i + 1]);
            i++;
        }
        else if (arg == "-base" && i + 1 < argc)
        {
            // -base X,Y — inspect mode subtracts (X,Y) from emitted coords
            // so the dx,dy values are cell-relative. Lets the same digit
            // fingerprint apply at any screen position.
            int bx = 0, by = 0;
            if (sscanf_s(argv[i + 1], "%d,%d", &bx, &by) == 2) {
                g_insp.baseX = bx;
                g_insp.baseY = by;
                g_insp.baseSet = true;
            } else {
                std::cerr << "[ERROR] -base expects X,Y (e.g. -base 100,200)\n";
                return -1;
            }
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

    // -inspect short-circuits the capture pipeline.  -check on its own
    // also short-circuits (batch verify, no window).  -check needs a
    // file too — reuse -inspect's path arg.
    if (!checkSpec.empty())
    {
        if (inspectFile.empty()) {
            std::cerr << "[ERROR] -check requires -inspect <file> to know which image to read.\n";
            GdiplusShutdown(gdiplusToken);
            return 1;
        }
        int rc = runCheckMode(inspectFile, checkSpec, checkTol);
        GdiplusShutdown(gdiplusToken);
        return rc;
    }
    if (gridW > 0 && gridH > 0)
    {
        if (inspectFile.empty()) {
            std::cerr << "[ERROR] -grid requires -inspect <file> to know which image to read.\n";
            GdiplusShutdown(gdiplusToken);
            return 1;
        }
        int rc = runGridMode(inspectFile, gridW, gridH);
        GdiplusShutdown(gdiplusToken);
        return rc;
    }
    if (!ocrTestSpec.empty())
    {
        if (inspectFile.empty()) {
            std::cerr << "[ERROR] -ocr_test requires -inspect <file> to know which image to read.\n";
            GdiplusShutdown(gdiplusToken);
            return 1;
        }
        int rc = runOcrTestMode(inspectFile, ocrTestSpec, checkTol);
        GdiplusShutdown(gdiplusToken);
        return rc;
    }
    if (!ocrScanSpec.empty())
    {
        if (inspectFile.empty()) {
            std::cerr << "[ERROR] -ocr_scan requires -inspect <file> to know which image to read.\n";
            GdiplusShutdown(gdiplusToken);
            return 1;
        }
        int rc = runOcrScanMode(inspectFile, ocrScanSpec, checkTol);
        GdiplusShutdown(gdiplusToken);
        return rc;
    }
    // Preprocess: -resize and/or -crop applied to an -inspect <input>, written to -f <output>.
    if (!inspectFile.empty() && (resizeW > 0 || cropW > 0))
    {
        std::wstring outPath = outputFile;
        if (!outputDir.empty()) outPath = outputDir + L"\\" + outputFile;
        int rc = runPreprocessMode(inspectFile, outPath,
            resizeW, resizeH, cropX, cropY, cropW, cropH);
        GdiplusShutdown(gdiplusToken);
        return rc;
    }
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