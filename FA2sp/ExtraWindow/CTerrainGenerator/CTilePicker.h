#pragma once

#include <FA2PP.h>
#include <vector>

// A small modal window that shows a thumbnail grid of every tile of one tile set,
// so the user can click the tiles they want instead of typing their indexes.
class CTilePicker
{
public:
    // tileSet  : tile set number (>= 10000 means a custom tile set).
    // selected : on entry the indexes (relative to the tile set) that start out selected,
    //            on exit (only when true is returned) the indexes the user chose, ascending.
    // Returns true if the user confirmed with OK, false on cancel or when the tile set
    // could not be shown.
    static bool Pick(HWND hOwner, int tileSet, std::vector<int>& selected);
};
