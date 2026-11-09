#include "CTilePicker.h"

#include "../../Ext/CMapData/Body.h"
#include "../../Ext/CTileSetBrowserView/Body.h"
#include "../../Ext/CFinalSunApp/Body.h"
#include "../../Helpers/Translations.h"
#include "../../Helpers/FString.h"

#include <algorithm>
#include <cstdio>
#include <set>
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
        int Index = 0;        // index inside the tile set
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
        std::set<int> Selected;

        int CellW = 0;
        int CellH = 0;
        int Margin = 0;
        int LabelH = 0;
        int Cols = 1;
        int ScrollY = 0;
        int ContentH = 0;
        int LastClicked = -1;

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
        FString title = Translations::TranslateOrDefault("CTerrainGenerator.PickTilesTitle", "Pick tiles");
        char buffer[512];
        sprintf_s(buffer, "%s  (%d / %d)", title.c_str(),
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
            const bool selected = st.Selected.count(cell.Index) > 0;

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

            char label[32];
            sprintf_s(label, "%d", cell.Index);
            RECT rcLabel = { rcCell.left, rcCell.bottom - st.LabelH, rcCell.right, rcCell.bottom };
            SetTextColor(hMem, selected ? RGB(0, 50, 140) : GetSysColor(COLOR_WINDOWTEXT));
            DrawTextA(hMem, label, -1, &rcLabel, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
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
                st.Selected.insert(st.Cells[i].Index);
        }
        else
        {
            const int tileIndex = st.Cells[idx].Index;
            if (st.Selected.count(tileIndex))
                st.Selected.erase(tileIndex);
            else
                st.Selected.insert(tileIndex);
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
                    st.Selected.insert(cell.Index);
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

bool CTilePicker::Pick(HWND hOwner, int tileSet, std::vector<int>& selected)
{
    if (g_State)
        return false; // already open

    int start = 0;
    int count = 0;
    if (!GetTileRange(tileSet, start, count))
        return false;

    RegisterClasses();

    PickerState state;
    g_State = &state;

    state.Scale = CFinalSunAppExt::ProgramScaleFactor;
    if (state.Scale < 1.0f)
        state.Scale = 1.0f;

    for (int idx : selected)
    {
        if (idx >= 0 && idx < count)
            state.Selected.insert(idx);
    }

    // Capture every tile of the tile set as a bitmap.
    int maxW = 0;
    int maxH = 0;
    state.Cells.reserve(count);
    for (int i = 0; i < count; ++i)
    {
        Cell cell;
        cell.Index = i;
        cell.Bitmap = CaptureTile(start + i, cell.Width, cell.Height);
        maxW = std::max(maxW, cell.Width);
        maxH = std::max(maxH, cell.Height);
        state.Cells.push_back(cell);
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

    FString title = Translations::TranslateOrDefault("CTerrainGenerator.PickTilesTitle", "Pick tiles");
    state.hWnd = CreateWindowExA(WS_EX_DLGMODALFRAME, PICKER_CLASS, title.c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_CLIPCHILDREN,
        winX, winY, winW, winH, hOwner, nullptr, GetModuleHandleA(nullptr), nullptr);

    bool accepted = false;
    if (state.hWnd)
    {
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

        accepted = state.Accepted;
        if (accepted)
        {
            selected.assign(state.Selected.begin(), state.Selected.end());
        }
    }

    for (auto& cell : state.Cells)
    {
        if (cell.Bitmap)
            DeleteObject(cell.Bitmap);
    }
    g_State = nullptr;
    return accepted;
}
