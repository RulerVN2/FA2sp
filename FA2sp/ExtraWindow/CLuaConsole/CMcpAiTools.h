#pragma once

#include <functional>
#include <string>
#include <json.hpp>

// Extra MCP tools that give an AI agent better "eyes" on the editor:
//  - TileAtlas: a labelled contact sheet (PNG) of the tiles in one tile set plus
//    machine readable facts about each tile.
//  - ValidateMap: a list of concrete problems and some composition statistics.
//
// Both must run on the editor thread (they are dispatched like the other MCP handlers).
class CMcpAiTools
{
public:
    // Turns a game-side (ANSI) string into the encoding the MCP client expects.
    using Encoder = std::function<std::string(const std::string&)>;

    // tileSet < 0 : list all tile sets (index, name, tile_count).
    // tileSet >= 0: render page `page` (0-based) of that tile set into a PNG.
    static nlohmann::json TileAtlas(int tileSet, int page, const Encoder& encode);

    // Scans the open map. At most maxIssues individual issues are listed,
    // the per-type counts always cover everything.
    static nlohmann::json ValidateMap(int maxIssues, const Encoder& encode);
};
