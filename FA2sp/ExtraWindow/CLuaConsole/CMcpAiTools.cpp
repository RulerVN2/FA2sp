#include "CMcpAiTools.h"

#include <CMapData.h>
#include <CINI.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <vector>

#include "../../Ext/CMapData/Body.h"
#include "../../Ext/CIsoView/Body.h"
#include "../../Ext/CTileSetBrowserView/Body.h"
#include "../../Helpers/FString.h"
#include "../../Helpers/STDHelpers.h"
#include "../CTerrainGenerator/CTilePicker.h"

using json = nlohmann::json;

namespace
{
    // Land types as the game numbers them for tile cells.
    constexpr int LAND_WATER = 2;
    constexpr int LAND_ROCK = 3;

    const char* LandName(int type)
    {
        static const char* names[] = {
            "clear", "road", "water", "rock", "wall", "tiberium",
            "beach", "rough", "ice", "railroad", "tunnel", "weeds"
        };
        if (type >= 0 && type < static_cast<int>(sizeof(names) / sizeof(names[0])))
            return names[type];
        return "unknown";
    }

    std::string TileSetName(int tileSet)
    {
        if (!CINI::CurrentTheater)
            return "";
        FString section;
        section.Format("TileSet%04d", tileSet);
        if (!CINI::CurrentTheater->SectionExists(section))
            return "";
        const char* name = CINI::CurrentTheater->GetString(section, "SetName");
        return name ? name : "";
    }

    bool SavePng(HBITMAP hBitmap, const std::wstring& path)
    {
        CIsoViewExt::InitGdiplus();

        Gdiplus::Bitmap bitmap(hBitmap, nullptr);
        UINT num = 0, size = 0;
        Gdiplus::GetImageEncodersSize(&num, &size);
        if (size == 0)
            return false;
        auto* codecs = static_cast<Gdiplus::ImageCodecInfo*>(malloc(size));
        if (!codecs)
            return false;
        Gdiplus::GetImageEncoders(num, size, codecs);
        CLSID png = {};
        bool found = false;
        for (UINT i = 0; i < num; ++i)
        {
            if (wcscmp(codecs[i].MimeType, L"image/png") == 0)
            {
                png = codecs[i].Clsid;
                found = true;
                break;
            }
        }
        free(codecs);
        if (!found)
            return false;
        return bitmap.Save(path.c_str(), &png, nullptr) == Gdiplus::Ok;
    }

    std::wstring TempFilePath(const wchar_t* fileName)
    {
        wchar_t tempPath[MAX_PATH];
        if (GetTempPathW(MAX_PATH, tempPath) == 0)
            return L"";
        std::wstring dir = std::wstring(tempPath) + L"FinalAlert2";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\" + fileName;
    }

    const CTileBlockClass* BlockOfCell(const CellData* cell)
    {
        if (!cell || !CMapDataExt::TileData)
            return nullptr;
        int tile = cell->TileIndex;
        if (tile == 0xFFFF)
            tile = 0;
        if (tile < 0 || tile >= CMapDataExt::TileDataCount)
            return nullptr;
        const CTileTypeClass& type = CMapDataExt::TileData[tile];
        const int sub = cell->TileSubIndex;
        if (sub < 0 || sub >= static_cast<int>(type.TileBlockCount) || !type.TileBlockDatas)
            return nullptr;
        return &type.TileBlockDatas[sub];
    }
}

namespace
{
    // One tile, enlarged, with the number of every sub cell written on it.
    json RenderTileDetail(int tileSet, int start, int count, int rel, const CMcpAiTools::Encoder& encode)
    {
        json result;
        if (rel < 0 || rel >= count)
        {
            result["error"] = "Tile index out of range for this tile set.";
            result["tile_count"] = count;
            return result;
        }
        const int abs = start + rel;
        if (tileSet >= 10000)
        {
            result["error"] = "Sub cell numbering is only available for the normal tile sets.";
            return result;
        }
        if (abs < 0 || abs >= CMapDataExt::TileDataCount)
        {
            result["error"] = "Tile index out of range.";
            return result;
        }

        int tw = 0, th = 0;
        HBITMAP hTile = CTilePicker::CaptureTileBitmap(abs, tw, th);
        if (!hTile || tw <= 0 || th <= 0)
        {
            result["error"] = "Could not render this tile.";
            return result;
        }

        const CTileTypeClass& type = CMapDataExt::TileData[abs];
        const int addedW = CTileSetBrowserViewExt::GetAddedWidth(abs);
        const int addedH = CTileSetBrowserViewExt::GetAddedHeight(abs);

        const double scale = std::max(1.0, std::min({ 4.0, 640.0 / tw, 480.0 / th }));
        const int pad = 6;
        const int width = static_cast<int>(tw * scale) + pad * 2;
        const int height = static_cast<int>(th * scale) + pad * 2;

        HDC hScreen = GetDC(nullptr);
        HDC hMem = CreateCompatibleDC(hScreen);
        HBITMAP hSheet = CreateCompatibleBitmap(hScreen, width, height);
        HGDIOBJ hOldSheet = SelectObject(hMem, hSheet);
        HBRUSH hBack = CreateSolidBrush(RGB(48, 52, 60));
        RECT all = { 0, 0, width, height };
        FillRect(hMem, &all, hBack);
        DeleteObject(hBack);

        HDC hTileDC = CreateCompatibleDC(hScreen);
        HGDIOBJ hOldTile = SelectObject(hTileDC, hTile);
        SetStretchBltMode(hMem, COLORONCOLOR);
        StretchBlt(hMem, pad, pad, static_cast<int>(tw * scale), static_cast<int>(th * scale),
            hTileDC, 0, 0, tw, th, SRCCOPY);
        SelectObject(hTileDC, hOldTile);
        DeleteDC(hTileDC);
        DeleteObject(hTile);

        HFONT hFont = CreateFontA(16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, "Arial");
        HGDIOBJ hOldFont = SelectObject(hMem, hFont);
        SetBkMode(hMem, OPAQUE);
        SetBkColor(hMem, RGB(20, 20, 24));
        SetTextColor(hMem, RGB(255, 235, 120));
        SetTextAlign(hMem, TA_CENTER | TA_TOP);

        json cells = json::array();
        int p = 0;
        for (int i = 0; i < type.Height; ++i)
        {
            for (int e = 0; e < type.Width; ++e)
            {
                if (p >= static_cast<int>(type.TileBlockCount) || !type.TileBlockDatas)
                    break;
                const CTileBlockClass& block = type.TileBlockDatas[p];
                if (block.HasValidImage)
                {
                    // Centre of this cell's diamond inside the rendered tile (same layout RenderTile uses).
                    const double cx = e * 30 - i * 30 + 30 - type.Bounds.left + addedW;
                    const double cy = e * 15 + i * 15 + 15 - block.Height * 15 - type.Bounds.top + addedH;
                    char label[16];
                    sprintf_s(label, "%d", p);
                    TextOutA(hMem, pad + static_cast<int>(cx * scale), pad + static_cast<int>(cy * scale) - 8,
                        label, static_cast<int>(strlen(label)));
                }
                cells.push_back({
                    {"sub_index", p},
                    {"cell_dx", e},
                    {"cell_dy", i},
                    {"filled", block.HasValidImage != FALSE},
                    {"land", LandName(static_cast<int>(block.TerrainType))},
                    {"height", static_cast<int>(block.Height)},
                    {"ramp", block.RampType != 0}
                });
                ++p;
            }
        }

        SelectObject(hMem, hOldFont);
        DeleteObject(hFont);
        SelectObject(hMem, hOldSheet);

        wchar_t fileName[96];
        swprintf_s(fileName, L"tiledetail_%d_%d.png", tileSet, rel);
        const std::wstring path = TempFilePath(fileName);
        const bool saved = !path.empty() && SavePng(hSheet, path);
        DeleteDC(hMem);
        DeleteObject(hSheet);
        ReleaseDC(nullptr, hScreen);

        if (!saved)
        {
            result["error"] = "Could not write the image.";
            return result;
        }

        result["tileset"] = tileSet;
        result["tileset_name"] = encode(TileSetName(tileSet));
        result["tile"] = rel;
        result["absolute_index"] = abs;
        result["image"] = encode(STDHelpers::WStringToString(path));
        result["image_size"] = { {"width", width}, {"height", height} };
        result["cells_x"] = static_cast<int>(type.Width);
        result["cells_y"] = static_cast<int>(type.Height);
        result["sub_cells"] = cells;
        result["layout"] = "Each yellow number in the picture is the sub_index of the cell it sits on. "
            "cell_dx / cell_dy give where that cell lands when the tile is placed with its first "
            "cell (sub_index 0) at (x, y): the cell goes to (x + cell_dx, y + cell_dy) in the same x/y "
            "convention as get_cell and observe. Cells with filled=false are empty. "
            "Height is the cell's height offset (0 = lowest cell of the tile).";
        return result;
    }
}

json CMcpAiTools::TileAtlas(int tileSet, int page, int tile, const Encoder& encode)
{
    json result;
    if (!CMapDataExt::TileData || CMapDataExt::TileSet_starts.size() < 2)
    {
        result["error"] = "No map is loaded, so there is no tile data yet.";
        return result;
    }

    if (tileSet < 0)
    {
        json sets = json::array();
        const int setCount = static_cast<int>(CMapDataExt::TileSet_starts.size()) - 1;
        for (int i = 0; i < setCount; ++i)
        {
            sets.push_back({
                {"tileset", i},
                {"name", encode(TileSetName(i))},
                {"tile_count", CMapDataExt::TileSet_starts[i + 1] - CMapDataExt::TileSet_starts[i]}
            });
        }
        for (const auto& [set, tiles] : CMapDataExt::CustomTiles)
        {
            sets.push_back({
                {"tileset", set},
                {"name", "custom"},
                {"tile_count", static_cast<int>(tiles.size())}
            });
        }
        result["tilesets"] = sets;
        result["hint"] = "Call tile_atlas again with a 'tileset' number to get a picture of its tiles.";
        return result;
    }

    int start = 0, count = 0;
    if (!CTilePicker::GetTileSetRange(tileSet, start, count))
    {
        result["error"] = "Unknown or empty tile set.";
        return result;
    }

    if (tile >= 0)
        return RenderTileDetail(tileSet, start, count, tile, encode);

    constexpr int COLUMNS = 8;
    constexpr int ROWS = 8;
    constexpr int PER_PAGE = COLUMNS * ROWS;
    constexpr int CELL_W = 128;
    constexpr int CELL_H = 128;
    constexpr int LABEL_H = 16;

    const int pageCount = (count + PER_PAGE - 1) / PER_PAGE;
    if (page < 0)
        page = 0;
    if (page >= pageCount)
    {
        result["error"] = "Page out of range.";
        result["page_count"] = pageCount;
        return result;
    }

    const int first = page * PER_PAGE;
    const int last = std::min(count, first + PER_PAGE);
    const int shown = last - first;
    const int rows = (shown + COLUMNS - 1) / COLUMNS;
    const int width = COLUMNS * CELL_W;
    const int height = rows * CELL_H;

    HDC hScreen = GetDC(nullptr);
    HDC hMem = CreateCompatibleDC(hScreen);
    HBITMAP hSheet = CreateCompatibleBitmap(hScreen, width, height);
    HGDIOBJ hOldSheet = SelectObject(hMem, hSheet);

    HBRUSH hBack = CreateSolidBrush(RGB(48, 52, 60));
    RECT all = { 0, 0, width, height };
    FillRect(hMem, &all, hBack);
    DeleteObject(hBack);

    HFONT hFont = CreateFontA(13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, "Arial");
    HGDIOBJ hOldFont = SelectObject(hMem, hFont);
    SetBkMode(hMem, TRANSPARENT);
    SetTextColor(hMem, RGB(255, 235, 120));

    HDC hTileDC = CreateCompatibleDC(hScreen);
    json tiles = json::array();

    for (int n = 0; n < shown; ++n)
    {
        const int rel = first + n;
        const int abs = start + rel;
        const int col = n % COLUMNS;
        const int row = n / COLUMNS;
        const int ox = col * CELL_W;
        const int oy = row * CELL_H;

        // frame of this cell
        HBRUSH hFrame = CreateSolidBrush(RGB(80, 86, 98));
        RECT frame = { ox, oy, ox + CELL_W, oy + CELL_H };
        FrameRect(hMem, &frame, hFrame);
        DeleteObject(hFrame);

        int tw = 0, th = 0;
        HBITMAP hTile = CTilePicker::CaptureTileBitmap(abs, tw, th);
        if (hTile && tw > 0 && th > 0)
        {
            const int boxW = CELL_W - 4;
            const int boxH = CELL_H - LABEL_H - 4;
            double scale = std::min(1.0, std::min(double(boxW) / tw, double(boxH) / th));
            const int dw = std::max(1, int(tw * scale));
            const int dh = std::max(1, int(th * scale));
            HGDIOBJ hOldTile = SelectObject(hTileDC, hTile);
            SetStretchBltMode(hMem, HALFTONE);
            StretchBlt(hMem, ox + (CELL_W - dw) / 2, oy + 2 + (boxH - dh) / 2, dw, dh,
                hTileDC, 0, 0, tw, th, SRCCOPY);
            SelectObject(hTileDC, hOldTile);
            DeleteObject(hTile);
        }

        char label[32];
        sprintf_s(label, "%d", rel);
        RECT labelRect = { ox + 3, oy + CELL_H - LABEL_H, ox + CELL_W - 3, oy + CELL_H - 1 };
        DrawTextA(hMem, label, -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        json entry = {
            {"index", rel},
            {"absolute_index", abs},
            {"atlas_column", col},
            {"atlas_row", row}
        };

        if (tileSet < 10000 && abs >= 0 && abs < CMapDataExt::TileDataCount)
        {
            const CTileTypeClass& type = CMapDataExt::TileData[abs];
            entry["cells_x"] = static_cast<int>(type.Width);
            entry["cells_y"] = static_cast<int>(type.Height);

            std::set<int> lands, ramps;
            int minH = 99, maxH = -1, valid = 0;
            for (int k = 0; k < static_cast<int>(type.TileBlockCount) && type.TileBlockDatas; ++k)
            {
                const CTileBlockClass& block = type.TileBlockDatas[k];
                if (!block.HasValidImage)
                    continue;
                ++valid;
                lands.insert(static_cast<int>(block.TerrainType));
                if (block.RampType != 0)
                    ramps.insert(static_cast<int>(block.RampType));
                minH = std::min(minH, static_cast<int>(block.Height));
                maxH = std::max(maxH, static_cast<int>(block.Height));
            }
            entry["filled_cells"] = valid;
            if (valid > 0)
            {
                json landNames = json::array();
                for (int l : lands)
                    landNames.push_back(LandName(l));
                entry["land"] = landNames;
                entry["height_min"] = minH;
                entry["height_max"] = maxH;
                entry["has_ramp"] = !ramps.empty();
            }
        }
        else if (tileSet >= 10000)
        {
            const CustomTile* custom = CMapDataExt::GetCustomTile(abs);
            if (custom)
            {
                entry["cells_x"] = custom->Width;
                entry["cells_y"] = custom->Height;
            }
        }
        tiles.push_back(entry);
    }

    DeleteDC(hTileDC);
    SelectObject(hMem, hOldFont);
    DeleteObject(hFont);
    SelectObject(hMem, hOldSheet);

    wchar_t fileName[64];
    swprintf_s(fileName, L"tileatlas_%d_p%d.png", tileSet, page);
    const std::wstring path = TempFilePath(fileName);
    const bool saved = !path.empty() && SavePng(hSheet, path);

    DeleteDC(hMem);
    DeleteObject(hSheet);
    ReleaseDC(nullptr, hScreen);

    if (!saved)
    {
        result["error"] = "Could not write the atlas image.";
        return result;
    }

    result["tileset"] = tileSet;
    result["tileset_name"] = encode(TileSetName(tileSet));
    result["image"] = encode(STDHelpers::WStringToString(path));
    result["image_size"] = { {"width", width}, {"height", height} };
    result["page"] = page;
    result["page_count"] = pageCount;
    result["tile_count"] = count;
    result["layout"] = "Grid of " + std::to_string(COLUMNS) + " columns; each tile is drawn "
        "(scaled down if large) in its own box with its tile-set index written at the bottom left. "
        "Use that index as the tile number (relative to the tile set).";
    result["land_types"] = "Per-tile 'land' lists the ground kinds found in its cells "
        "(clear, road, water, rock = impassable cliff, ...).";
    result["tiles"] = tiles;
    return result;
}

json CMcpAiTools::ValidateMap(int maxIssues, const Encoder& encode)
{
    json result;
    auto pMap = CMapDataExt::GetExtension();
    if (!pMap || !CMapDataExt::TileData || pMap->CellDataCount <= 0)
    {
        result["error"] = "No map is loaded.";
        return result;
    }
    if (maxIssues <= 0)
        maxIssues = 100;

    json issues = json::array();
    std::map<std::string, int> counts;

    // Coordinates are reported the way the Lua API and 'observe' use them:
    // x = column used by get_cell's first argument, y = the other one.
    auto report = [&](const char* type, const char* severity, int coordX, int coordY, const std::string& text)
        {
            ++counts[type];
            if (static_cast<int>(issues.size()) >= maxIssues)
                return;
            issues.push_back({
                {"type", type}, {"severity", severity},
                {"x", coordY}, {"y", coordX}, {"message", text}
            });
        };

    std::map<int, int> tileSetCells;
    int cellsInMap = 0;
    int decorated = 0;
    int terrainObjects = 0, structures = 0, overlays = 0, smudges = 0;

    // for the bare-area search
    const int BLOCK = 12;
    struct BlockInfo { int cells = 0; int decor = 0; int water = 0; int rock = 0; };
    std::map<std::pair<int, int>, BlockInfo> blocks;

    for (int pos = 0; pos < pMap->CellDataCount; ++pos)
    {
        const int cx = pMap->GetXFromCoordIndex(pos);
        const int cy = pMap->GetYFromCoordIndex(pos);
        if (!pMap->IsCoordInMap(cx, cy))
            continue;

        ++cellsInMap;
        const CellData* cell = pMap->GetCellAt(pos);
        if (!cell)
            continue;

        int tile = cell->TileIndex;
        if (tile == 0xFFFF)
            tile = 0;

        if (tile < 0 || tile >= CMapDataExt::TileDataCount)
        {
            report("invalid_tile", "error", cx, cy,
                "Cell uses tile index " + std::to_string(tile) + " which does not exist in this theater.");
        }
        else
        {
            ++tileSetCells[CMapDataExt::TileData[tile].TileSet];
        }

        const CTileBlockClass* block = BlockOfCell(cell);
        const int land = block ? static_cast<int>(block->TerrainType) : 0;
        const bool hasRamp = block && block->RampType != 0;

        const bool hasTerrain = cell->Terrain > -1;
        const bool hasStructure = cell->Structure > -1;
        const bool hasOverlay = cell->Overlay != 0xFFFF;
        const bool hasSmudge = cell->Smudge > -1;
        const bool hasInfantry = cell->Infantry[0] > -1 || cell->Infantry[1] > -1 || cell->Infantry[2] > -1;
        const bool hasUnit = cell->Unit > -1;

        terrainObjects += hasTerrain;
        structures += hasStructure;
        overlays += hasOverlay;
        smudges += hasSmudge;
        const bool decor = hasTerrain || hasStructure || hasOverlay || hasSmudge;
        decorated += decor;

        if (land == LAND_ROCK && (hasInfantry || hasUnit))
            report("unit_on_cliff", "error", cx, cy, "A ground unit stands on an impassable cliff cell.");
        if (land == LAND_ROCK && hasStructure)
            report("building_on_cliff", "warning", cx, cy, "A building covers a cliff cell.");
        if (land == LAND_WATER && hasTerrain)
            report("tree_on_water", "warning", cx, cy, "A terrain object (tree/rock) sits on water.");

        // Height jump to the next cell in each direction (each pair is looked at once).
        // Cells that belong to the same tile, ramps and cliff cells are expected to jump.
        static const int dirs[2][2] = { {1, 0}, {0, 1} };
        for (const auto& d : dirs)
        {
            const int nx = cx + d[0];
            const int ny = cy + d[1];
            if (!pMap->IsCoordInMap(nx, ny))
                continue;
            const CellData* next = pMap->GetCellAt(pMap->GetCoordIndex(nx, ny));
            if (!next)
                continue;
            const int dh = std::abs(static_cast<int>(cell->Height) - static_cast<int>(next->Height));
            if (dh < 2)
                continue;
            if (cell->TileIndex == next->TileIndex)
                continue;
            const CTileBlockClass* nextBlock = BlockOfCell(next);
            const int nextLand = nextBlock ? static_cast<int>(nextBlock->TerrainType) : 0;
            if (land == LAND_ROCK || nextLand == LAND_ROCK)
                continue;
            if (hasRamp || (nextBlock && nextBlock->RampType != 0))
                continue;
            report("height_step", "warning", cx, cy,
                "Height changes by " + std::to_string(dh) + " between neighbouring cells without a ramp or cliff tile.");
        }

        auto& info = blocks[{ cx / BLOCK, cy / BLOCK }];
        ++info.cells;
        info.decor += decor ? 1 : 0;
        info.water += land == LAND_WATER ? 1 : 0;
        info.rock += land == LAND_ROCK ? 1 : 0;
    }

    // Large open land areas without anything on them look unfinished.
    json bare = json::array();
    int bareCount = 0;
    for (const auto& [key, info] : blocks)
    {
        if (info.cells < BLOCK * BLOCK / 2)
            continue;
        const int land = info.cells - info.water - info.rock;
        if (land < info.cells * 3 / 4)
            continue;
        if (info.decor > 0)
            continue;
        ++bareCount;
        if (bare.size() < 20)
        {
            bare.push_back({
                {"x_from", key.second * BLOCK}, {"x_to", key.second * BLOCK + BLOCK - 1},
                {"y_from", key.first * BLOCK}, {"y_to", key.first * BLOCK + BLOCK - 1}
            });
        }
    }
    if (bareCount > 0)
        counts["bare_area"] = bareCount;

    json usage = json::array();
    std::vector<std::pair<int, int>> sorted;
    for (const auto& [set, n] : tileSetCells)
        sorted.push_back({ n, set });
    std::sort(sorted.rbegin(), sorted.rend());
    for (size_t i = 0; i < sorted.size() && i < 12; ++i)
    {
        usage.push_back({
            {"tileset", sorted[i].second},
            {"name", encode(TileSetName(sorted[i].second))},
            {"cells", sorted[i].first},
            {"percent", std::round(sorted[i].first * 1000.0 / std::max(1, cellsInMap)) / 10.0}
        });
    }

    json countJson = json::object();
    for (const auto& [type, n] : counts)
        countJson[type] = n;

    result["map_size"] = { {"width", pMap->Size.Width}, {"height", pMap->Size.Height} };
    result["cells_in_map"] = cellsInMap;
    result["issue_counts"] = countJson;
    result["issues_listed"] = static_cast<int>(issues.size());
    result["issues"] = issues;
    result["bare_areas"] = bare;
    result["bare_area_note"] = "Each entry is a " + std::to_string(BLOCK) + "x" + std::to_string(BLOCK) +
        " region of mostly open land with no trees, buildings, overlays or smudges. "
        "These are only hints - empty space is sometimes intended.";
    result["composition"] = {
        {"decorated_cell_percent", std::round(decorated * 1000.0 / std::max(1, cellsInMap)) / 10.0},
        {"terrain_objects", terrainObjects},
        {"structure_cells", structures},
        {"overlay_cells", overlays},
        {"smudge_cells", smudges},
        {"tileset_usage", usage}
    };
    result["severity_note"] = "error = almost certainly a mistake; warning = worth a look "
        "(height_step is a heuristic and can also flag intended terraces).";
    return result;
}
