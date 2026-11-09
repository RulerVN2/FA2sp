#include "CustomTileStore.h"

#include <CFinalSunDlg.h>
#include "../CMapData/Body.h"
#include "../CFinalSunApp/Body.h"
#include "../../Miscs/CopyPaste.h"
#include "../../Miscs/Hooks.INI.h"
#include "../../Helpers/Translations.h"
#include "../../Helpers/FString.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <vector>

namespace
{
    constexpr const char* SECTION = "FA2spCustomTiles";
    constexpr int MAX_SIDE = 64;

    FString MakeKey(int tile, const char* suffix)
    {
        FString key;
        key.Format("%d.%s", tile, suffix);
        return key;
    }

    FString MakeRowKey(int tile, int row)
    {
        FString key;
        key.Format("%d.R%d", tile, row);
        return key;
    }

    void ShowMessage(const char* text)
    {
        HWND hMain = CFinalSunApp::Instance->m_pMainWnd ? CFinalSunApp::Instance->m_pMainWnd->m_hWnd : nullptr;
        MessageBoxA(hMain, text, "FA2sp", MB_OK | MB_ICONINFORMATION);
    }
}

bool CustomTileStore::LoadFromMap()
{
    CMapDataExt::CustomTiles.erase(MapTileSet);

    auto& ini = CINIExt::CurrentDocument();
    const int count = ini.GetInteger(SECTION, "Count", 0);
    if (count <= 0 || !CMapDataExt::TileData)
        return false;

    std::vector<CustomTile> pieces;
    for (int i = 0; i < count; ++i)
    {
        auto size = FString::SplitString(ini.GetString(SECTION, MakeKey(i, "Size").c_str()));
        if (size.size() < 2)
            continue;

        const int width = std::atoi(size[0].c_str());
        const int height = std::atoi(size[1].c_str());
        if (width <= 0 || height <= 0 || width > MAX_SIDE || height > MAX_SIDE)
            continue;

        auto& piece = pieces.emplace_back();
        piece.Initialize(width, height);

        for (int row = 0; row < height; ++row)
        {
            auto cells = FString::SplitString(ini.GetString(SECTION, MakeRowKey(i, row).c_str()));
            for (int col = 0; col < width && col < static_cast<int>(cells.size()); ++col)
            {
                auto parts = FString::SplitString(cells[col], ":");
                if (parts.size() < 3)
                    continue; // "-" means an empty cell

                const int tile = std::atoi(parts[0].c_str());
                const int sub = std::atoi(parts[1].c_str());
                const int cellHeight = std::atoi(parts[2].c_str());
                if (tile < 0 || tile >= CMapDataExt::TileDataCount || sub < 0)
                    continue;

                piece.TileBlockDatas[row * width + col].SetTileBlock(tile, sub, cellHeight);
            }
        }
    }

    if (pieces.empty())
        return false;

    CMapDataExt::CustomTiles[MapTileSet] = std::move(pieces);
    Translations::CustomTileSetNames[MapTileSet] =
        Translations::TranslateOrDefault("CustomTile.MapSetName", "Map custom tiles");
    return true;
}

void CustomTileStore::EnsureComboEntry(HWND hTileCombo)
{
    if (!hTileCombo)
        return;

    const int count = static_cast<int>(SendMessage(hTileCombo, CB_GETCOUNT, 0, 0));
    for (int i = 0; i < count; ++i)
    {
        if (SendMessage(hTileCombo, CB_GETITEMDATA, i, 0) == MapTileSet)
            return;
    }

    FString name = Translations::CustomTileSetNames[MapTileSet];
    FString text;
    text.Format("%d (%s)", MapTileSet, name.c_str());
    const int idx = static_cast<int>(SendMessage(hTileCombo, CB_INSERTSTRING, count,
        reinterpret_cast<LPARAM>(text.c_str())));
    if (idx >= 0)
        SendMessage(hTileCombo, CB_SETITEMDATA, idx, MapTileSet);
}

void CustomTileStore::SaveClipboardAsTile()
{
    if (!CMapDataExt::TileData)
        return;

    struct Item
    {
        int X;
        int Y;
        int Tile;
        int Sub;
        int Height;
    };
    std::vector<Item> items;

    HWND hMain = CFinalSunApp::Instance->m_pMainWnd ? CFinalSunApp::Instance->m_pMainWnd->m_hWnd : nullptr;
    if (OpenClipboard(hMain))
    {
        HANDLE hData = GetClipboardData(CFinalSunApp::Instance->ClipboardFormat);
        const char* ptr = hData ? static_cast<const char*>(GlobalLock(hData)) : nullptr;
        if (ptr)
        {
            const size_t bytes = GlobalSize(hData);
            // Same layout CopyPaste::Copy writes: 16 byte header, then the cells.
            if (bytes >= 16 && reinterpret_cast<const int*>(ptr)[0] == 0)
            {
                const size_t length = reinterpret_cast<const size_t*>(ptr)[1];
                if (length <= (bytes - 16) / sizeof(MyClipboardData))
                {
                    const auto* cells = reinterpret_cast<const MyClipboardData*>(ptr + 16);
                    for (size_t i = 0; i < length; ++i)
                    {
                        const auto& c = cells[i];
                        // Tile 0 is plain clear ground, leave it out so the piece does not
                        // flatten whatever it is pasted over.
                        if (c.TileIndex == 0 || c.TileIndex >= CMapDataExt::TileDataCount)
                            continue;
                        items.push_back({ c.X, c.Y, c.TileIndex, c.TileSubIndex, c.Height });
                    }
                }
            }
            GlobalUnlock(hData);
        }
        CloseClipboard();
    }

    if (items.empty())
    {
        FString text = Translations::TranslateOrDefault("CustomTile.NothingToSave",
            "Nothing to save. Select an area of tiles and press Ctrl+C first.");
        ShowMessage(text.c_str());
        return;
    }

    int minX = INT_MAX, maxX = INT_MIN, minY = INT_MAX, maxY = INT_MIN, minHeight = INT_MAX;
    for (const auto& it : items)
    {
        minX = std::min(minX, it.X);
        maxX = std::max(maxX, it.X);
        minY = std::min(minY, it.Y);
        maxY = std::max(maxY, it.Y);
        minHeight = std::min(minHeight, it.Height);
    }

    // Same orientation the tile placement uses: rows follow map X, columns follow map Y.
    const int rows = maxX - minX + 1;
    const int cols = maxY - minY + 1;
    if (rows > MAX_SIDE || cols > MAX_SIDE)
    {
        FString text = Translations::TranslateOrDefault("CustomTile.TooLarge",
            "The selection is too large for a custom tile (64 x 64 cells at most).");
        ShowMessage(text.c_str());
        return;
    }

    std::vector<const Item*> grid(static_cast<size_t>(rows) * cols, nullptr);
    for (const auto& it : items)
        grid[static_cast<size_t>(it.X - minX) * cols + (it.Y - minY)] = &it;

    auto& ini = CINIExt::CurrentDocument();
    const int newIndex = ini.GetInteger(SECTION, "Count", 0);

    FString sizeText;
    sizeText.Format("%d,%d", cols, rows);
    ini.WriteString(SECTION, MakeKey(newIndex, "Size").c_str(), sizeText.c_str());

    for (int row = 0; row < rows; ++row)
    {
        FString line;
        for (int col = 0; col < cols; ++col)
        {
            if (col > 0)
                line += ",";
            const Item* it = grid[static_cast<size_t>(row) * cols + col];
            if (!it)
            {
                line += "-";
                continue;
            }
            FString token;
            token.Format("%d:%d:%d", it->Tile, it->Sub, it->Height - minHeight);
            line += token;
        }
        ini.WriteString(SECTION, MakeRowKey(newIndex, row).c_str(), line.c_str());
    }

    FString countText;
    countText.Format("%d", newIndex + 1);
    ini.WriteString(SECTION, "Count", countText.c_str());

    LoadFromMap();

    // Show the new piece in the tile browser and make it the current brush.
    auto* pFrame = CFinalSunDlg::Instance->MyViewFrame.pTileSetBrowserFrame;
    HWND hCombo = GetDlgItem(pFrame->DialogBar.GetSafeHwnd(), 1366);
    EnsureComboEntry(hCombo);
    if (hCombo)
    {
        const int count = static_cast<int>(SendMessage(hCombo, CB_GETCOUNT, 0, 0));
        for (int i = 0; i < count; ++i)
        {
            if (SendMessage(hCombo, CB_GETITEMDATA, i, 0) == MapTileSet)
            {
                SendMessage(hCombo, CB_SETCURSEL, i, 0);
                break;
            }
        }
    }
    pFrame->View.SelectTileSet(MapTileSet, false);
    CIsoView::CurrentCommand->Command = 10;
    CIsoView::CurrentCommand->Type = CMapDataExt::GetCustomTileIndex(MapTileSet, newIndex);
    pFrame->View.RedrawWindow(nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
}
