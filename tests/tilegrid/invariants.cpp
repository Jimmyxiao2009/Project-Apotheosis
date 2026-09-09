// Apotheosis: invariants I1 - I17 of TILING-REWRITE-PLAN.md section 4, as
// functions the harness calls after every pass and every composite. They are
// the specification the model is held to; the scenarios only supply the inputs.

#include "check.h"

#include <cmath>
#include <map>
#include <set>

namespace tilegrid {

std::string describe(const IntRect& rect)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%d,%d %dx%d", rect.x, rect.y, rect.width, rect.height);
    return std::string(buffer);
}

std::string describe(CellIndex cell)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "(%d,%d)", cell.column, cell.row);
    return std::string(buffer);
}

namespace {

// Keyed by store id: every model numbers its textures and jobs from 1, so two
// harnesses alive at the same time (replay.cpp keeps one per store) would
// otherwise collide in these ledgers.
std::set<std::pair<unsigned, TextureId>> g_releasedTextures;
std::set<std::pair<unsigned, JobId>> g_cancelledJobs;

bool sameScale(float a, float b)
{
    return std::fabs(a - b) < 1e-4f;
}

std::optional<GridInfo> gridOf(const TileGridModel& model, GridId id)
{
    auto primary = model.primaryGrid();
    if (primary && primary->id == id)
        return primary;
    auto backdrop = model.backdropGrid();
    if (backdrop && backdrop->id == id)
        return backdrop;
    return std::nullopt;
}

} // namespace

void resetReleaseLedger(unsigned storeId)
{
    for (auto it = g_releasedTextures.begin(); it != g_releasedTextures.end();)
        it = it->first == storeId ? g_releasedTextures.erase(it) : std::next(it);
    for (auto it = g_cancelledJobs.begin(); it != g_cancelledJobs.end();)
        it = it->first == storeId ? g_cancelledJobs.erase(it) : std::next(it);
}

void checkInvariants(const TileGridModel& model, const char* where)
{
    const std::string prefix = std::string(where) + ": ";
    const auto primary = model.primaryGrid();
    const auto backdrop = model.backdropGrid();
    const std::vector<TileInfo> tiles = model.tiles();
    const std::vector<JobInfo> jobs = model.jobs();

    // I1 lattice: every tile's rect is exactly its cell rect clipped to B, and
    // no two tiles share an index (the grid is a map keyed by index, so a
    // duplicate would be a second grid - which is what v1 kept producing).
    std::set<std::pair<GridId, std::pair<int, int>>> seen;
    for (const TileInfo& tile : tiles) {
        const auto grid = gridOf(model, tile.grid);
        CHECK(grid.has_value());
        if (!grid)
            continue;
        const IntRect expected = model.cellRect(tile.cell, grid->bounds);
        CHECK(tile.rect.isSameGeometry(expected));
        CHECK(!tile.rect.isEmpty());
        const auto key = std::make_pair(tile.grid, std::make_pair(tile.cell.column, tile.cell.row));
        CHECK(seen.insert(key).second);
    }

    // I2 budget.
    if (primary)
        CHECK_LE(primary->tileCount, primary->budget);

    // I3 visible first: if the budget can hold the visible cells, every cell of
    // V has a tile.
    if (primary && model.lastVisible()) {
        const std::vector<CellIndex> visibleCells = model.cellsOf(*model.lastVisible(), primary->bounds);
        if (visibleCells.size() <= primary->budget) {
            std::set<std::pair<int, int>> present;
            for (const TileInfo& tile : tiles) {
                if (tile.grid == primary->id)
                    present.insert(std::make_pair(tile.cell.column, tile.cell.row));
            }
            for (CellIndex cell : visibleCells)
                CHECK(present.count(std::make_pair(cell.column, cell.row)) > 0);
        }
    }

    // I4 ready is true: a Ready tile's texture holds pixels rasterised for this
    // cell at this grid's scale. "For this cell" is paintedRect (R6): the rect
    // the pixels were rasterised for, which after a partial-cell resize is not
    // the current cell rect. The texture is exactly that big - it is never
    // stretched over a rect it was not rasterised for. There is no such thing
    // as a foreign texture in v2.
    const int edge = model.config().tileEdge;
    for (const TileInfo& tile : tiles) {
        if (tile.state != TileState::Ready)
            continue;
        const auto grid = gridOf(model, tile.grid);
        CHECK(tile.texture != invalidTextureId);
        CHECK(!tile.paintedRect.isEmpty());
        CHECK(tile.textureSize == tile.paintedRect.size());
        // paintedRect belongs to THIS cell of the lattice: same origin, never
        // bigger than one cell.
        const IntRect lattice(tile.cell.column * edge, tile.cell.row * edge, edge, edge);
        CHECK(tile.paintedRect.location() == lattice.location());
        CHECK(lattice.contains(tile.paintedRect));
        if (grid)
            CHECK(sameScale(tile.textureScale, grid->scale));
    }

    // I8 one backdrop: at most one grid is in a backdrop state, and it is never
    // a dropped one that is still around.
    if (backdrop) {
        CHECK(backdrop->state == GridState::PersistentBackdrop || backdrop->state == GridState::TransientBackdrop);
        CHECK(primary.has_value());
        if (primary)
            CHECK(primary->state == GridState::Primary);
    }

    // I7 backdrop bound: a transient backdrop never outlives N composites.
    if (backdrop && backdrop->state == GridState::TransientBackdrop)
        CHECK_LE(backdrop->age, model.config().backdropMaxComposites);

    // I15 blurry not white, structural half: the persistent backdrop holds at
    // most P tiles and does not exist at scale 1.
    if (backdrop && backdrop->state == GridState::PersistentBackdrop) {
        CHECK_LE(backdrop->tileCount, model.config().persistentBackdropBudget);
        CHECK(!sameScale(model.scale(), 1.0f));
    }

    // I12 one job: at most one replay job per tile at any time.
    std::map<std::pair<GridId, std::pair<int, int>>, int> jobsPerTile;
    for (const JobInfo& job : jobs) {
        if (job.state == JobState::Cancelled)
            continue;
        ++jobsPerTile[std::make_pair(job.grid, std::make_pair(job.cell.column, job.cell.row))];
    }
    for (const auto& entry : jobsPerTile)
        CHECK_LE(entry.second, 1);

    // I9 no timeouts, priority half: a visible cell's job is never queued
    // behind a non-visible one.
    if (primary && model.lastVisible()) {
        unsigned worstVisible = 0;
        bool haveVisible = false;
        unsigned bestNonVisible = 0xffffffffu;
        for (const JobInfo& job : jobs) {
            if (job.state != JobState::Queued)
                continue;
            const auto grid = gridOf(model, job.grid);
            if (!grid)
                continue;
            const IntRect rect = model.cellRect(job.cell, grid->bounds);
            const bool visible = grid->id == primary->id && rect.intersects(*model.lastVisible());
            if (visible) {
                haveVisible = true;
                worstVisible = std::max(worstVisible, job.priority);
            } else
                bestNonVisible = std::min(bestNonVisible, job.priority);
        }
        if (haveVisible && bestNonVisible != 0xffffffffu)
            CHECK(worstVisible < bestNonVisible);
    }

    // I17 phases: the model is idle between calls.
    CHECK(model.phase() == PhaseState::Idle);

    (void)prefix;
}

void checkI18NothingToShow(const TileGridModel&);

void checkPassOutput(const TileGridModel& model, const PassOutput& out, const char* where)
{
    (void)where;

    // I13 removal: a texture is released exactly once, a job cancelled exactly
    // once. Both ledgers live for the lifetime of one harness, keyed by store.
    const unsigned store = model.storeId();
    for (TextureId id : out.released) {
        CHECK(id != invalidTextureId);
        CHECK(g_releasedTextures.insert(std::make_pair(store, id)).second);
    }
    for (JobId id : out.cancels)
        CHECK(g_cancelledJobs.insert(std::make_pair(store, id)).second);

    // I12, second half: a pass creates at most one job per tile, and every
    // request carries a fresh job id.
    std::set<std::pair<GridId, std::pair<int, int>>> requested;
    std::set<JobId> ids;
    for (const PaintRequest& request : out.paints) {
        CHECK(request.job != invalidJobId);
        CHECK(ids.insert(request.job).second);
        CHECK(requested.insert(std::make_pair(request.grid, std::make_pair(request.cell.column, request.cell.row))).second);
        CHECK(!request.rect.isEmpty());
        // The requested rect never leaves its own cell (I6's precondition).
        const auto grid = gridOf(model, request.grid);
        CHECK(grid.has_value());
        if (grid)
            CHECK(model.cellRect(request.cell, grid->bounds).contains(request.rect));
    }

    // I9: the paint list is in priority order and the persistent backdrop is
    // last.
    unsigned previous = 0;
    bool sawBackdrop = false;
    const auto primary = model.primaryGrid();
    for (const PaintRequest& request : out.paints) {
        CHECK(request.priority >= previous);
        previous = request.priority;
        const bool isPrimary = primary && request.grid == primary->id;
        if (!isPrimary)
            sawBackdrop = true;
        else
            CHECK(!sawBackdrop);
    }

    checkI18NothingToShow(model);
}

// I18 - nothing to show, nothing to raster (0.1.9.34).
//
// If the visible rect is known and does not meet the layer bounds, a composite
// of this store can draw nothing at all: R8 walks cells(V n B), and cellsOf()
// yields no cell outside B. So neither of the two grids a pass reconciles - the
// primary and a *persistent* backdrop - may hold a tile, and the model may not
// ask for another pass.
//
// A transient backdrop is exempt: it is frozen by R7 and ends on composites
// (R9), so it legitimately outlives the moment its layer leaves the screen.
//
// On the device this was 23 stores whose backdrop rastered a tile that no
// composite ever uploaded (an off-screen layer is never painted), each holding
// its raster buffer and forcing a pass on every tick for the rest of the
// session. See deviceRound3_offscreenLayerNeverRasters().
void checkI18NothingToShow(const TileGridModel& model)
{
    const auto visible = model.lastVisible();
    if (!visible)
        return;
    const IntSize bounds = model.bounds();
    if (!visible->intersection(IntRect(0, 0, bounds.width, bounds.height)).isEmpty())
        return;

    const auto backdrop = model.backdropGrid();
    const bool transientAlive = backdrop && backdrop->state == GridState::TransientBackdrop;

    for (const TileInfo& tile : model.tiles()) {
        if (tile.gridState == GridState::TransientBackdrop)
            continue;
        ++::check::checks;
        ::check::fail(__FILE__, __LINE__, "I18: a store that shows nothing holds a tile",
            describe(tile.cell) + " grid " + std::to_string(tile.grid)
            + " state " + std::to_string(static_cast<int>(tile.state))
            + " V " + describe(*visible) + " B " + std::to_string(bounds.width) + "x"
            + std::to_string(bounds.height));
        break;
    }
    if (!transientAlive)
        CHECK(!model.wantsPass());
}

void checkDrawList(const TileGridModel& model, const DrawList& draw, const IntRect& visible, const char* where)
{
    (void)where;
    const auto primary = model.primaryGrid();
    const auto backdrop = model.backdropGrid();
    if (!primary)
        return;

    // I5 draws once: every visible cell is covered by exactly one of
    // {primary Ready tile, backdrop clip, nothing}.
    std::map<std::pair<int, int>, int> drawnBy;
    for (const TileDraw& tile : draw.tiles) {
        CHECK_EQ(tile.grid, primary->id);
        ++drawnBy[std::make_pair(tile.cell.column, tile.cell.row)];
    }
    for (const BackdropClip& clip : draw.backdrop)
        ++drawnBy[std::make_pair(clip.cell.column, clip.cell.row)];

    // The draw list covers the visible cells the model has DECIDED to have -
    // R2 gives a tile only to a desired cell, so a visible cell outside the
    // desired set can never become Ready and reporting it as a hole reports the
    // budget as a failure (device round 1: a store with an unknown V then held
    // the driver in a permanent extra-composite loop). I3 keeps this honest:
    // while the budget covers V, every cell of V is desired.
    std::set<std::pair<int, int>> desiredCells;
    for (const TileInfo& info : model.tiles()) {
        if (info.grid == primary->id && info.desired)
            desiredCells.insert(std::make_pair(info.cell.column, info.cell.row));
    }
    std::vector<CellIndex> visibleCells;
    for (CellIndex cell : model.cellsOf(visible, primary->bounds)) {
        if (desiredCells.count(std::make_pair(cell.column, cell.row)))
            visibleCells.push_back(cell);
    }
    CHECK_EQ(draw.visibleCells, static_cast<unsigned>(visibleCells.size()));
    unsigned uncovered = 0;
    for (CellIndex cell : visibleCells) {
        const auto it = drawnBy.find(std::make_pair(cell.column, cell.row));
        const int count = it == drawnBy.end() ? 0 : it->second;
        CHECK_LE(count, 1);
        if (!count)
            ++uncovered;
    }
    // "nothing" only when there is no backdrop.
    if (backdrop)
        CHECK_EQ(uncovered, 0u);

    // I6 no foreign pixels: every drawn texture belongs to the cell and the
    // scale it is drawn for, and it is drawn at exactly the rect it was
    // rasterised for (paintedRect, R6) - never stretched onto another rect.
    std::map<std::pair<int, int>, IntRect> paintedOf;
    for (const TileInfo& info : model.tiles()) {
        if (info.grid == primary->id)
            paintedOf[std::make_pair(info.cell.column, info.cell.row)] = info.paintedRect;
    }
    for (const TileDraw& tile : draw.tiles) {
        CHECK(tile.texture != invalidTextureId);
        const auto painted = paintedOf.find(std::make_pair(tile.cell.column, tile.cell.row));
        CHECK(painted != paintedOf.end());
        if (painted != paintedOf.end())
            CHECK(tile.target.isSameGeometry(painted->second));
        CHECK(sameScale(tile.scale, primary->scale));
    }
    for (const BackdropClip& clip : draw.backdrop) {
        CHECK(!clip.clip.isEmpty());
        CHECK(model.cellRect(clip.cell, primary->bounds).contains(clip.clip));
        for (const TileDraw& tile : clip.tiles) {
            CHECK(backdrop.has_value());
            if (!backdrop)
                continue;
            CHECK_EQ(tile.grid, backdrop->id);
            CHECK(tile.texture != invalidTextureId);
            CHECK(sameScale(tile.scale, backdrop->scale));
            CHECK(!tile.target.isEmpty());
        }
    }

    // I5's counterpart: the reported hole count matches what the list shows.
    unsigned holes = 0;
    for (CellIndex cell : visibleCells) {
        const auto it = drawnBy.find(std::make_pair(cell.column, cell.row));
        if (it == drawnBy.end()) {
            ++holes;
            continue;
        }
    }
    for (const BackdropClip& clip : draw.backdrop) {
        if (!clip.fullyCovered)
            ++holes;
    }
    CHECK_EQ(draw.visibleHoles, holes);

    // I13 again, on the composite side.
    const unsigned store = model.storeId();
    for (TextureId id : draw.released) {
        CHECK(id != invalidTextureId);
        CHECK(g_releasedTextures.insert(std::make_pair(store, id)).second);
    }
    for (JobId id : draw.cancels)
        CHECK(g_cancelledJobs.insert(std::make_pair(store, id)).second);

    // I4 on what is actually uploaded.
    for (const UploadRecord& upload : draw.uploads) {
        CHECK(upload.texture != invalidTextureId);
        CHECK(!upload.rect.isEmpty());
    }
}

} // namespace tilegrid
