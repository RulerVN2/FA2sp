#pragma once

#include <FA2PP.h>
#include <string>
#include <vector>

// Small modal windows that show a thumbnail grid so the user can click the items
// they want instead of typing indexes or names.
class CTilePicker
{
public:
    // Thumbnails of every tile of one tile set.
    // tileSet  : tile set number (>= 10000 means a custom tile set).
    // selected : on entry the indexes (relative to the tile set) that start out selected,
    //            on exit (only when true is returned) the indexes the user chose, ascending.
    // Returns true if the user confirmed with OK, false on cancel or when the tile set
    // could not be shown.
    static bool Pick(HWND hOwner, int tileSet, std::vector<int>& selected);

    // Thumbnails of every terrain object (trees, rocks, ...) listed in [TerrainTypes].
    // selected : on entry the IDs that start out selected, on exit (only when true is
    //            returned) the chosen IDs. IDs that were typed but have no thumbnail
    //            (not available in this theater) are kept in front of the list.
    static bool PickTerrain(HWND hOwner, std::vector<std::string>& selected);
};
