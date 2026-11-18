## MCP tools: tile_atlas and validate_map

### `tile_atlas` — see the tiles instead of guessing from numbers
- No arguments: lists every tile set (`tileset`, `name`, `tile_count`).
- `tileset = N`: renders a PNG contact sheet of that tile set (8 columns, 64 tiles per page; use `page` for more) and returns the image path. Each picture has its tile-set index written under it; that index is the tile number used by the Lua API (`cell.tile = start_of_set + index`; relative indexes are also used by `get_tileset_info` and the terrain generator).
- Each tile also comes with: `cells_x` / `cells_y` (size in cells), `land` (clear, road, water, rock = impassable cliff, rough, ...), `height_min` / `height_max`, `has_ramp`.
- Typical use: call it for the tile sets you plan to use (cliffs, roads, ground), look at the PNG, pick tiles by their look and size, then place them.

- `tileset = N` plus `tile = T`: shows that single tile enlarged with the **sub cell number** written on every cell, and returns `sub_cells` with `cell_dx` / `cell_dy` (where the cell lands relative to sub 0, in the get_cell x/y convention), land type, height and ramp. Use it before placing cliffs and other multi-cell tiles, so you know which sub index goes where.

### `observe` at a given spot
`observe` also accepts `x` and `y` (same convention as `get_cell` and its own output). The view is centred on that cell for the screenshot and object list, then put back where it was. Use it to inspect any part of the map without asking the user to scroll.

### `validate_map` — find mistakes after drawing
Returns concrete issues with coordinates (same x / y convention as `observe` and `get_cell`):
- `invalid_tile` (error): a cell uses a tile index that does not exist in this theater.
- `unit_on_cliff` (error): infantry or a vehicle stands on an impassable cliff cell.
- `building_on_cliff`, `tree_on_water` (warning).
- `height_step` (warning, heuristic): neighbouring cells differ in height by 2 or more without a ramp or cliff tile between them. Intended terraces can also be flagged.
- `bare_areas`: 12x12 regions of open land with no trees, buildings, overlays or smudges (hints only).
Also returns composition statistics: tile-set usage in percent, decorated-cell percentage, counts of terrain objects, structures, overlays and smudges.
Workflow: draw -> `validate_map` -> fix the listed problems -> run again -> `observe` to look at the result.
