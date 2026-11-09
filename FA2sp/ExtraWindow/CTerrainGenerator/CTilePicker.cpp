#include "CTilePicker.h"

#include "../../FA2sp.h"
#include "../../Ext/CMapData/Body.h"
#include "../../Ext/CLoading/Body.h"
#include "../../Ext/CTileSetBrowserView/Body.h"
#include "../../Ext/CFinalSunApp/Body.h"
#include "../../Ext/CFinalSunDlg/Body.h"
#include "../../Helpers/Helper.h"
#include "../../Helpers/Translations.h"
#include "../../Helpers/FString.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

namespace
{
    constexpr const char* PICKER_CLASS = "FA2spTilePicker";
    constexpr const char* GRID_CLASS = "FA2spTilePickerGrid";

    enum : int
    {
        ID_SELECT_ALL = 9001,
        ID_CLEAR = 9002,
    };

    struct Cell
    {
        std::string Key;      // tile index inside the tile set, or the terrain object ID
        HBITMAP Bitmap = nullptr;
        int Width = 0;
        int Height = 0;
    };

    struct PickerState
    {
        HWND hWnd = nullptr;
        HWND hGrid = nullptr;
        HWND hOk = nullptr;
        HWND hCancel = nullptr;
        HWND hAll = nullptr;
        HWND hNone = nullptr;

        std::vector<Cell> Cells;
        std::set<std::string> Selected;

        int CellW = 0;
        int CellH = 0;
        int Margin = 0;
        int LabelH = 0;
        int Cols = 1;
        int ScrollY = 0;
        int ContentH = 0;
        int LastClicked = -1;

        std::string Title;
        float Scale = 1.0f;
        bool Done = false;
        bool Accepted = false;
    };

    PickerState* g_State = nullptr;

    int S(int value)
    {
        const float scale = g_State ? g_State->Scale : 1.0f;
        return static_cast<int>(value * scale + 0.5f);
    }

    bool GetTileRange(int tileSet, int& start, int& count)
    {
        start = 0;
        count = 0;
        if (tileSet < 0)
            return false;

        if (tileSet < 10000)
        {
            if (!CMapDataExt::TileData)
                return false;
            if (tileSet + 1 >= static_cast<int>(CMapDataExt::TileSet_starts.size()))
                return false;
            start = CMapDataExt::TileSet_starts[tileSet];
            count = CMapDataExt::TileSet_starts[tileSet + 1] - start;
        }
        else
        {
            auto it = CMapDataExt::CustomTiles.find(tileSet);
            if (it == CMapDataExt::CustomTiles.end())
                return false;
            start = CMapDataExt::GetCustomTileIndex(tileSet, 0);
            count = static_cast<int>(it->second.size());
        }
        return count > 0;
    }

    // Copies the rendered tile into a plain GDI bitmap so it can be drawn at any time.
    HBITMAP CaptureTile(int tileIndex, int& width, int& height)
    {
        width = 0;
        height = 0;

        LPDIRECTDRAWSURFACE7 pSurface = CTileSetBrowserViewExt::RenderTile(tileIndex);
        if (!pSurface)
            return nullptr;

        HBITMAP hBitmap = nullptr;
        DDSURFACEDESC2 desc = {};
        desc.dwSize = sizeof(desc);
        if (SUCCEEDED(pSurface->GetSurfaceDesc(&desc)) && desc.dwWidth > 0 && desc.dwHeight > 0)
        {
            const int w = static_cast<int>(desc.dwWidth);
            const int h = static_cast<int>(desc.dwHeight);
            HDC hSurfaceDC = nullptr;
            if (SUCCEEDED(pSurface->GetDC(&hSurfaceDC)) && hSurfaceDC)
            {
                HDC hScreen = GetDC(nullptr);
                HDC hMem = CreateCompatibleDC(hScreen);
                hBitmap = CreateCompatibleBitmap(hScreen, w, h);
                if (hBitmap)
                {
                    HGDIOBJ hOld = SelectObject(hMem, hBitmap);
                    BitBlt(hMem, 0, 0, w, h, hSurfaceDC, 0, 0, SRCCOPY);
                    SelectObject(hMem, hOld);
                    width = w;
                    height = h;
                }
                DeleteDC(hMem);
                ReleaseDC(nullptr, hScreen);
                pSurface->ReleaseDC(hSurfaceDC);
            }
        }
        pSurface->Release();
        return hBitmap;
    }

    void UpdateTitle()
    {
        if (!g_State || !g_State->hWnd)
            return;
        char buffer[512];
        sprintf_s(buffer, "%s  (%d / %d)", g_State->Title.c_str(),
            static_cast<int>(g_State->Selected.size()),
            static_cast<int>(g_State->Cells.size()));
        SetWindowTextA(g_State->hWnd, buffer);
    }

    void UpdateScrollBar()
    {
        auto& st = *g_State;
        RECT rc;
        GetClientRect(st.hGrid, &rc);

        const int usable = std::max(1, static_cast<int>(rc.right) - st.Margin * 2);
        st.Cols = std::max(1, usable / st.CellW);
        const int rows = (static_cast<int>(st.Cells.size()) + st.Cols - 1) / st.Cols;
        st.ContentH = rows * st.CellH + st.Margin * 2;

        const int maxScroll = std::max(0, st.ContentH - static_cast<int>(rc.bottom));
        st.ScrollY = std::clamp(st.ScrollY, 0, maxScroll);

        SCROLLINFO si = {};
        si.cbSize = sizeof(si);
        si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        si.nMin = 0;
        si.nMax = std::max(0, st.ContentH - 1);
        si.nPage = static_cast<UINT>(std::max(1, static_cast<int>(rc.bottom)));
        si.nPos = st.ScrollY;
        SetScrollInfo(st.hGrid, SB_VERT, &si, TRUE);
    }

    void ScrollTo(int newY)
    {
        auto& st = *g_State;
        RECT rc;
        GetClientRect(st.hGrid, &rc);
        const int maxScroll = std::max(0, st.ContentH - static_cast<int>(rc.bottom));
        newY = std::clamp(newY, 0, maxScroll);
        if (newY == st.ScrollY)
            return;
        st.ScrollY = newY;
        SetScrollPos(st.hGrid, SB_VERT, newY, TRUE);
        InvalidateRect(st.hGrid, nullptr, FALSE);
    }

    void PaintGrid(HWND hWnd)
    {
        auto& st = *g_State;
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        RECT rc;
        GetClientRect(hWnd, &rc);
        const int cw = std::max(1, static_cast<int>(rc.right));
        const int ch = std::max(1, static_cast<int>(rc.bottom));

        HDC hMem = CreateCompatibleDC(hdc);
        HBITMAP hBuffer = CreateCompatibleBitmap(hdc, cw, ch);
        HGDIOBJ hOldBuffer = SelectObject(hMem, hBuffer);

        FillRect(hMem, &rc, reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_WINDOW + 1)));

        HGDIOBJ hOldFont = SelectObject(hMem, GetStockObject(DEFAULT_GUI_FONT));
        SetBkMode(hMem, TRANSPARENT);
        SetStretchBltMode(hMem, HALFTONE);
        SetBrushOrgEx(hMem, 0, 0, nullptr);

        HBRUSH hSelectedFill = CreateSolidBrush(RGB(196, 220, 255));
        HBRUSH hSelectedFrame = CreateSolidBrush(RGB(0, 90, 200));
        HBRUSH hNormalFrame = CreateSolidBrush(RGB(210, 210, 210));
        HDC hBitmapDC = CreateCompatibleDC(hdc);

        const int count = static_cast<int>(st.Cells.size());
        for (int i = 0; i < count; ++i)
        {
            const int row = i / st.Cols;
            const int col = i % st.Cols;
            const int x = st.Margin + col * st.CellW;
            const int y = st.Margin + row * st.CellH - st.ScrollY;
            if (y + st.CellH < 0 || y > ch)
                continue;

            const Cell& cell = st.Cells[i];
            const bool selected = st.Selected.count(cell.Key) > 0;

            RECT rcCell = { x, y, x + st.CellW - S(2), y + st.CellH - S(2) };
            if (selected)
                FillRect(hMem, &rcCell, hSelectedFill);
            FrameRect(hMem, &rcCell, selected ? hSelectedFrame : hNormalFrame);
            if (selected)
            {
                RECT inner = rcCell;
                InflateRect(&inner, -1, -1);
                FrameRect(hMem, &inner, hSelectedFrame);
            }

            if (cell.Bitmap && cell.Width > 0 && cell.Height > 0)
            {
                const int pad = S(3);
                const int availW = (rcCell.right - rcCell.left) - pad * 2;
                const int availH = (rcCell.bottom - rcCell.top) - st.LabelH - pad * 2;
                double scale = 1.0;
                if (cell.Width > availW)
                    scale = std::min(scale, static_cast<double>(availW) / cell.Width);
                if (cell.Height > availH)
                    scale = std::min(scale, static_cast<double>(availH) / cell.Height);
                const int dw = std::max(1, static_cast<int>(cell.Width * scale));
                const int dh = std::max(1, static_cast<int>(cell.Height * scale));
                const int dx = rcCell.left + pad + (availW - dw) / 2;
                const int dy = rcCell.top + pad + (availH - dh) / 2;

                HGDIOBJ hOldBitmap = SelectObject(hBitmapDC, cell.Bitmap);
                if (dw == cell.Width && dh == cell.Height)
                    BitBlt(hMem, dx, dy, dw, dh, hBitmapDC, 0, 0, SRCCOPY);
                else
                    StretchBlt(hMem, dx, dy, dw, dh, hBitmapDC, 0, 0, cell.Width, cell.Height, SRCCOPY);
                SelectObject(hBitmapDC, hOldBitmap);
            }

            RECT rcLabel = { rcCell.left, rcCell.bottom - st.LabelH, rcCell.right, rcCell.bottom };
            SetTextColor(hMem, selected ? RGB(0, 50, 140) : GetSysColor(COLOR_WINDOWTEXT));
            DrawTextA(hMem, cell.Key.c_str(), -1, &rcLabel,
                DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS);
        }

        DeleteDC(hBitmapDC);
        DeleteObject(hSelectedFill);
        DeleteObject(hSelectedFrame);
        DeleteObject(hNormalFrame);
        SelectObject(hMem, hOldFont);

        BitBlt(hdc, 0, 0, cw, ch, hMem, 0, 0, SRCCOPY);

        SelectObject(hMem, hOldBuffer);
        DeleteObject(hBuffer);
        DeleteDC(hMem);
        EndPaint(hWnd, &ps);
    }

    int HitTest(int px, int py)
    {
        auto& st = *g_State;
        const int x = px - st.Margin;
        const int y = py + st.ScrollY - st.Margin;
        if (x < 0 || y < 0)
            return -1;
        const int col = x / st.CellW;
        const int row = y / st.CellH;
        if (col >= st.Cols)
            return -1;
        const int idx = row * st.Cols + col;
        if (idx < 0 || idx >= static_cast<int>(st.Cells.size()))
            return -1;
        return idx;
    }

    void OnGridClick(int px, int py, bool shift)
    {
        auto& st = *g_State;
        const int idx = HitTest(px, py);
        if (idx < 0)
            return;

        if (shift && st.LastClicked >= 0 && st.LastClicked < static_cast<int>(st.Cells.size()))
        {
            const int from = std::min(st.LastClicked, idx);
            const int to = std::max(st.LastClicked, idx);
            for (int i = from; i <= to; ++i)
                st.Selected.insert(st.Cells[i].Key);
        }
        else
        {
            const std::string& key = st.Cells[idx].Key;
            if (st.Selected.count(key))
                st.Selected.erase(key);
            else
                st.Selected.insert(key);
        }
        st.LastClicked = idx;
        InvalidateRect(st.hGrid, nullptr, FALSE);
        UpdateTitle();
    }

    LRESULT CALLBACK GridProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (!g_State)
            return DefWindowProcA(hWnd, msg, wParam, lParam);

        switch (msg)
        {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            PaintGrid(hWnd);
            return 0;
        case WM_SIZE:
            if (g_State->hGrid) // the grid handle is not stored yet while the window is being created
            {
                UpdateScrollBar();
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            return 0;
        case WM_VSCROLL:
        {
            auto& st = *g_State;
            RECT rc;
            GetClientRect(hWnd, &rc);
            int pos = st.ScrollY;
            switch (LOWORD(wParam))
            {
            case SB_LINEUP: pos -= st.CellH / 2; break;
            case SB_LINEDOWN: pos += st.CellH / 2; break;
            case SB_PAGEUP: pos -= rc.bottom; break;
            case SB_PAGEDOWN: pos += rc.bottom; break;
            case SB_TOP: pos = 0; break;
            case SB_BOTTOM: pos = st.ContentH; break;
            case SB_THUMBTRACK:
            case SB_THUMBPOSITION:
            {
                SCROLLINFO si = {};
                si.cbSize = sizeof(si);
                si.fMask = SIF_TRACKPOS;
                GetScrollInfo(hWnd, SB_VERT, &si);
                pos = si.nTrackPos;
                break;
            }
            default: break;
            }
            ScrollTo(pos);
            return 0;
        }
        case WM_MOUSEWHEEL:
        {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            ScrollTo(g_State->ScrollY - delta * g_State->CellH / WHEEL_DELTA / 2);
            return 0;
        }
        case WM_LBUTTONDOWN:
            SetFocus(hWnd);
            OnGridClick(static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)),
                (wParam & MK_SHIFT) != 0);
            return 0;
        default:
            break;
        }
        return DefWindowProcA(hWnd, msg, wParam, lParam);
    }

    void LayoutChildren()
    {
        auto& st = *g_State;
        RECT rc;
        GetClientRect(st.hWnd, &rc);

        const int gap = S(8);
        const int btnH = S(26);
        const int barH = btnH + gap * 2;
        const int btnW = S(84);

        SetWindowPos(st.hGrid, nullptr, 0, 0, rc.right, std::max(1, static_cast<int>(rc.bottom) - barH),
            SWP_NOZORDER);

        const int y = rc.bottom - barH + gap;
        SetWindowPos(st.hAll, nullptr, gap, y, btnW, btnH, SWP_NOZORDER);
        SetWindowPos(st.hNone, nullptr, gap * 2 + btnW, y, btnW, btnH, SWP_NOZORDER);
        SetWindowPos(st.hCancel, nullptr, rc.right - gap - btnW, y, btnW, btnH, SWP_NOZORDER);
        SetWindowPos(st.hOk, nullptr, rc.right - gap * 2 - btnW * 2, y, btnW, btnH, SWP_NOZORDER);
    }

    HWND MakeButton(HWND hParent, const char* text, int id, bool isDefault)
    {
        HWND hButton = CreateWindowExA(0, "BUTTON", text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | (isDefault ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
            0, 0, 10, 10, hParent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            GetModuleHandleA(nullptr), nullptr);
        SendMessageA(hButton, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        return hButton;
    }

    LRESULT CALLBACK PickerProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (!g_State)
            return DefWindowProcA(hWnd, msg, wParam, lParam);

        auto& st = *g_State;
        switch (msg)
        {
        case WM_SIZE:
            if (st.hGrid)
                LayoutChildren();
            return 0;
        case WM_COMMAND:
        {
            const int id = LOWORD(wParam);
            if (HIWORD(wParam) != BN_CLICKED)
                break;
            switch (id)
            {
            case IDOK:
                st.Accepted = true;
                st.Done = true;
                return 0;
            case IDCANCEL:
                st.Done = true;
                return 0;
            case ID_SELECT_ALL:
                for (const auto& cell : st.Cells)
                    st.Selected.insert(cell.Key);
                InvalidateRect(st.hGrid, nullptr, FALSE);
                UpdateTitle();
                return 0;
            case ID_CLEAR:
                st.Selected.clear();
                InvalidateRect(st.hGrid, nullptr, FALSE);
                UpdateTitle();
                return 0;
            default:
                break;
            }
            break;
        }
        case WM_CLOSE:
            st.Done = true;
            return 0;
        default:
            break;
        }
        return DefWindowProcA(hWnd, msg, wParam, lParam);
    }

    void RegisterClasses()
    {
        static bool registered = false;
        if (registered)
            return;
        registered = true;

        HINSTANCE hInstance = GetModuleHandleA(nullptr);

        WNDCLASSEXA wc = {};
        wc.cbSize = sizeof(wc);
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);

        wc.lpfnWndProc = PickerProc;
        wc.lpszClassName = PICKER_CLASS;
        wc.hbrBackground = reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_BTNFACE + 1));
        RegisterClassExA(&wc);

        wc.lpfnWndProc = GridProc;
        wc.lpszClassName = GRID_CLASS;
        wc.hbrBackground = nullptr;
        RegisterClassExA(&wc);
    }
}

namespace
{
    // Shows the window for a state whose Cells (and Selected) are already filled in.
    // Returns true if the user pressed OK; state.Selected then holds the choice.
    bool RunPicker(HWND hOwner, PickerState& state)
    {
        int maxW = 0;
        int maxH = 0;
        for (const auto& cell : state.Cells)
        {
            maxW = std::max(maxW, cell.Width);
            maxH = std::max(maxH, cell.Height);
        }

        state.Margin = S(6);
        state.LabelH = S(16);
        state.CellW = std::clamp(maxW, S(60), S(170)) + S(10);
        state.CellH = std::clamp(maxH, S(30), S(130)) + S(10) + state.LabelH;

        // Place the window over the owner (or the screen) and keep it inside the work area.
        RECT work = {};
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
        RECT ownerRect = work;
        if (hOwner)
            GetWindowRect(hOwner, &ownerRect);
        const int winW = std::min(S(820), static_cast<int>(work.right - work.left));
        const int winH = std::min(S(600), static_cast<int>(work.bottom - work.top));
        int winX = (ownerRect.left + ownerRect.right - winW) / 2;
        int winY = (ownerRect.top + ownerRect.bottom - winH) / 2;
        winX = std::clamp(winX, static_cast<int>(work.left), std::max(static_cast<int>(work.left), static_cast<int>(work.right) - winW));
        winY = std::clamp(winY, static_cast<int>(work.top), std::max(static_cast<int>(work.top), static_cast<int>(work.bottom) - winH));

        state.hWnd = CreateWindowExA(WS_EX_DLGMODALFRAME, PICKER_CLASS, state.Title.c_str(),
            WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN,
            winX, winY, winW, winH, hOwner, nullptr, GetModuleHandleA(nullptr), nullptr);
        if (!state.hWnd)
            return false;

        FString textOk = Translations::TranslateOrDefault("CTerrainGenerator.PickTilesOK", "OK");
        FString textCancel = Translations::TranslateOrDefault("CTerrainGenerator.PickTilesCancel", "Cancel");
        FString textAll = Translations::TranslateOrDefault("CTerrainGenerator.PickTilesAll", "Select all");
        FString textNone = Translations::TranslateOrDefault("CTerrainGenerator.PickTilesNone", "Clear");

        state.hGrid = CreateWindowExA(WS_EX_CLIENTEDGE, GRID_CLASS, "",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPSIBLINGS,
            0, 0, 10, 10, state.hWnd, nullptr, GetModuleHandleA(nullptr), nullptr);
        state.hAll = MakeButton(state.hWnd, textAll.c_str(), ID_SELECT_ALL, false);
        state.hNone = MakeButton(state.hWnd, textNone.c_str(), ID_CLEAR, false);
        state.hCancel = MakeButton(state.hWnd, textCancel.c_str(), IDCANCEL, false);
        state.hOk = MakeButton(state.hWnd, textOk.c_str(), IDOK, true);

        LayoutChildren();
        UpdateScrollBar();
        UpdateTitle();

        if (hOwner)
            EnableWindow(hOwner, FALSE);
        ShowWindow(state.hWnd, SW_SHOW);
        UpdateWindow(state.hWnd);
        SetFocus(state.hGrid);

        MSG msg = {};
        while (!state.Done && GetMessageA(&msg, nullptr, 0, 0) > 0)
        {
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE
                && (msg.hwnd == state.hWnd || IsChild(state.hWnd, msg.hwnd)))
            {
                state.Done = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (msg.message == WM_QUIT)
            PostQuitMessage(static_cast<int>(msg.wParam));

        if (hOwner)
            EnableWindow(hOwner, TRUE);
        DestroyWindow(state.hWnd);
        if (hOwner)
            SetForegroundWindow(hOwner);

        return state.Accepted;
    }

    void ReleaseCells(PickerState& state)
    {
        for (auto& cell : state.Cells)
        {
            if (cell.Bitmap)
                DeleteObject(cell.Bitmap);
        }
        state.Cells.clear();
    }

    PickerState MakeState()
    {
        PickerState state;
        state.Scale = CFinalSunAppExt::ProgramScaleFactor;
        if (state.Scale < 1.0f)
            state.Scale = 1.0f;
        return state;
    }

    // Draws a palette based object image (tree, rock...) on a light background and
    // returns it as a 32 bit bitmap cropped to the visible pixels.
    HBITMAP CaptureObject(const FString& id, int& width, int& height)
    {
        width = 0;
        height = 0;

        TempValueHolder<bool> keepShadowOff(ExtConfigs::InGameDisplay_Shadow, false);
        TempValueHolder<bool> objectViewMode(CLoadingExt::IsLoadingObjectView, true);

        if (!CLoadingExt::IsObjectPreviewLoaded(id) && !CLoadingExt::IsObjectLoaded(id))
            CLoadingExt::GetExtension()->LoadObjects(id);

        FString imageName = CLoadingExt::GetImageName(id, 0);
        ImageDataClassSafe* pd = CLoadingExt::GetImageDataFromMap(imageName);
        if (!pd || !pd->pImageBuffer || !pd->pPixelValidRanges || !pd->pPalette
            || pd->FullWidth <= 0 || pd->FullHeight <= 0)
            return nullptr;

        const int fullW = pd->FullWidth;
        const int fullH = pd->FullHeight;

        // Bounding box of the visible pixels.
        int minX = fullW, maxX = -1, minY = fullH, maxY = -1;
        for (int y = 0; y < fullH; ++y)
        {
            const int left = pd->pPixelValidRanges[y].First;
            const int right = pd->pPixelValidRanges[y].Last;
            if (left > right)
                continue;
            minX = std::min(minX, left);
            maxX = std::max(maxX, right);
            minY = std::min(minY, y);
            maxY = std::max(maxY, y);
        }
        if (maxX < minX || maxY < minY)
            return nullptr;

        const int w = maxX - minX + 1;
        const int h = maxY - minY + 1;

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* pBits = nullptr;
        HDC hScreen = GetDC(nullptr);
        HBITMAP hBitmap = CreateDIBSection(hScreen, &bmi, DIB_RGB_COLORS, &pBits, nullptr, 0);
        ReleaseDC(nullptr, hScreen);
        if (!hBitmap || !pBits)
        {
            if (hBitmap)
                DeleteObject(hBitmap);
            return nullptr;
        }

        constexpr BYTE BG_R = 205, BG_G = 210, BG_B = 205;
        BYTE* dst = static_cast<BYTE*>(pBits);
        const BYTE* src = pd->pImageBuffer.get();
        const BYTE* opacity = pd->pOpacity ? pd->pOpacity.get() : nullptr;

        for (int y = 0; y < h; ++y)
        {
            const int sy = minY + y;
            const int left = pd->pPixelValidRanges[sy].First;
            const int right = pd->pPixelValidRanges[sy].Last;
            BYTE* row = dst + static_cast<size_t>(y) * w * 4;
            for (int x = 0; x < w; ++x)
            {
                const int sx = minX + x;
                BYTE r = BG_R, g = BG_G, b = BG_B;
                if (sx >= left && sx <= right)
                {
                    const BYTE index = src[sy * fullW + sx];
                    if (index != 0)
                    {
                        const int alpha = opacity ? opacity[sy * fullW + sx] : 255;
                        const auto& c = pd->pPalette->Data[index];
                        r = static_cast<BYTE>((c.R * alpha + BG_R * (255 - alpha)) / 255);
                        g = static_cast<BYTE>((c.G * alpha + BG_G * (255 - alpha)) / 255);
                        b = static_cast<BYTE>((c.B * alpha + BG_B * (255 - alpha)) / 255);
                    }
                }
                row[x * 4 + 0] = b;
                row[x * 4 + 1] = g;
                row[x * 4 + 2] = r;
                row[x * 4 + 3] = 255;
            }
        }

        width = w;
        height = h;
        return hBitmap;
    }
}

bool CTilePicker::Pick(HWND hOwner, int tileSet, std::vector<int>& selected)
{
    if (g_State)
        return false; // already open

    int start = 0;
    int count = 0;
    if (!GetTileRange(tileSet, start, count))
        return false;

    RegisterClasses();

    PickerState state = MakeState();
    g_State = &state;
    state.Title = Translations::TranslateOrDefault("CTerrainGenerator.PickTilesTitle", "Pick tiles").c_str();

    for (int idx : selected)
    {
        if (idx >= 0 && idx < count)
            state.Selected.insert(std::to_string(idx));
    }

    state.Cells.reserve(count);
    for (int i = 0; i < count; ++i)
    {
        Cell cell;
        cell.Key = std::to_string(i);
        cell.Bitmap = CaptureTile(start + i, cell.Width, cell.Height);
        state.Cells.push_back(cell);
    }

    bool accepted = RunPicker(hOwner, state);
    if (accepted)
    {
        selected.clear();
        for (const auto& cell : state.Cells) // cells are in ascending index order
        {
            if (state.Selected.count(cell.Key))
                selected.push_back(std::atoi(cell.Key.c_str()));
        }
    }

    ReleaseCells(state);
    g_State = nullptr;
    return accepted;
}

bool CTilePicker::PickTerrain(HWND hOwner, std::vector<std::string>& selected)
{
    if (g_State)
        return false; // already open

    RegisterClasses();

    PickerState state = MakeState();
    g_State = &state;
    state.Title = Translations::TranslateOrDefault("CTerrainGenerator.PickTerrainTitle", "Pick terrain objects").c_str();

    for (auto& [_, obj] : Variables::RulesMap.GetSection("TerrainTypes"))
    {
        const char* name = obj;
        if (!name || !*name || CViewObjectsExt::IsIgnored(name))
            continue;

        Cell cell;
        cell.Key = name;
        cell.Bitmap = CaptureObject(FString(name), cell.Width, cell.Height);
        if (!cell.Bitmap)
            continue; // not available (e.g. other theater), nothing to show
        state.Cells.push_back(cell);
    }

    // Remember typed IDs that have no thumbnail, they must survive the round trip.
    std::set<std::string> shown;
    for (const auto& cell : state.Cells)
        shown.insert(cell.Key);
    std::vector<std::string> kept;
    for (const auto& id : selected)
    {
        if (shown.count(id))
            state.Selected.insert(id);
        else
            kept.push_back(id);
    }

    bool accepted = false;
    if (!state.Cells.empty())
        accepted = RunPicker(hOwner, state);

    if (accepted)
    {
        selected = kept;
        for (const auto& cell : state.Cells)
        {
            if (state.Selected.count(cell.Key))
                selected.push_back(cell.Key);
        }
    }

    ReleaseCells(state);
    g_State = nullptr;
    return accepted;
}
