#pragma once

#include <FA2PP.h>

// Custom tiles that are saved inside the map itself ([FA2spCustomTiles] section).
// They all live in one custom tile set, MapTileSet, which shows up in the tile browser.
class CustomTileStore
{
public:
    static constexpr int MapTileSet = 19000;

    // Rebuilds CMapDataExt::CustomTiles[MapTileSet] from the open map.
    // Returns true if the map holds at least one custom tile.
    static bool LoadFromMap();

    // Makes sure the tile set combo box of the tile browser lists the map's custom tile set.
    static void EnsureComboEntry(HWND hTileCombo);

    // Takes the cells currently on the clipboard (Ctrl+C), stores them as a new custom tile
    // in the map and selects it in the tile browser.
    static void SaveClipboardAsTile();
};
