// Apotheosis: the scripted scenarios of TILING-REWRITE-PLAN.md section 5.4.
// Every one of them is a device complaint from the DAY-REPORTs, plus one
// mutation-style guard per rule R1 - R12. The comment above each test says
// which rule it guards; removing that rule from the model must make it fail.
//
//   R1  ruleR1_marginIsThePreRender, scenario2_fling
//   R2  ruleR2_noHysteresis, scenario1_scroll
//   R3  ruleR3_paintModes, scenario3_pinchAndPanFar
//   R4  scenario10_uploadBudget
//   R5  scenario11_priorityAndFailure
//   R6  scenario8_stickyHeader
//   R7  scenario3_pinchAndPanFar, scenario4_zoomOutAndIn, scenario5_scaleChangeMidRaster
//   R8  ruleR8_readyCellIsNeverBackdropped
//   R9  scenario4_zoomOutAndIn (K), ruleR9_hardBound (N)
//   R10 scenario7_dirtyDuringScroll, scenario13_animation
//   R11 ruleR11_noProgressiveCover
//   R12 ruleR12_convergence

#include "check.h"

#include <algorithm>
#include <map>
#include <set>

namespace tilegrid {

namespace {

constexpr int screenWidth = 720;
constexpr int screenHeight = 1280;

PassInput input(IntSize bounds, float scale, std::optional<IntRect> visible)
{
    PassInput in;
    in.boundsScaled = bounds;
    in.scale = scale;
    in.visible = visible;
    return in;
}

IntSize scaledBounds(int width, int height, float scale)
{
    return IntSize(static_cast<int>(width * scale + 0.5f), static_cast<int>(height * scale + 0.5f));
}

unsigned readyCount(const TileGridModel& model, GridId grid)
{
    unsigned count = 0;
    for (const TileInfo& tile : model.tiles()) {
        if (tile.grid == grid && tile.state == TileState::Ready)
            ++count;
    }
    return count;
}

const TileInfo* findTile(const std::vector<TileInfo>& tiles, GridId grid, CellIndex cell)
{
    for (const TileInfo& tile : tiles) {
        if (tile.grid == grid && tile.cell == cell)
            return &tile;
    }
    return nullptr;
}

// Bring a store to a fixed point at scale 1 and return the last visible rect.
IntRect settle(Harness& harness, IntSize bounds, float scale, IntRect visible, int rounds = 3)
{
    for (int i = 0; i < rounds; ++i)
        harness.step(input(bounds, scale, visible), 64, "settle");
    return visible;
}

// -------------------------------------------------------------------------
// Scenario 1 - scroll 1:1 on a long page (R1, R2, R12).
// -------------------------------------------------------------------------
void scenario1_scroll()
{
    check::currentTest = "scenario1_scroll";
    Harness harness;
    const IntSize bounds(screenWidth, 8000);

    for (int pass = 0; pass < 40; ++pass) {
        const IntRect visible(0, pass * 200, screenWidth, screenHeight);
        PassInput in = input(bounds, 1.0f, visible);
        in.panGesture = pass > 0;
        PassOutput out = harness.pass(in, "scroll pass");
        if (pass > 0) {
            // R2 without hysteresis and R1's margin together mean at most one
            // new row per pass on a one-column page.
            CHECK_LE(out.paints.size(), 1u);
        }
        harness.finishAll(out);
        DrawList draw = harness.composite(visible, 64, "scroll composite");
        CHECK_EQ(draw.visibleHoles, 0u);
        CHECK_LE(harness.model.primaryGrid()->tileCount, harness.model.primaryGrid()->budget);
    }
    EXPECT_NO_ARROWS(harness);
}

// -------------------------------------------------------------------------
// Scenario 2 - fling (R1: the margin is the pre-render, R12: convergence).
// -------------------------------------------------------------------------
void scenario2_fling()
{
    check::currentTest = "scenario2_fling";
    Harness harness;
    const IntSize bounds(screenWidth, 20000);
    const int step = screenHeight / 3;

    IntRect visible(0, 0, screenWidth, screenHeight);
    harness.step(input(bounds, 1.0f, visible), 64, "fling warmup");

    // panGesture is deliberately false throughout: a fling after the finger has
    // left the glass is the fastest movement there is, and R1's pan-side margin
    // follows the model's own direction, not the flag (package 1 review).
    for (int i = 1; i <= 12; ++i) {
        visible = IntRect(0, i * step, screenWidth, screenHeight);
        PassInput in = input(bounds, 1.0f, visible);
        in.panGesture = false;
        PassOutput out = harness.pass(in, "fling pass");
        harness.finishAll(out);
        DrawList draw = harness.composite(visible, 64, "fling composite");
        // A jump of a third of the screen is inside the half-screen margin, so
        // the cells it reveals were already tiled.
        CHECK_EQ(draw.visibleHoles, 0u);
    }

    // R1: with no finger down, the grid still reaches further ahead of V than
    // behind it - the doubled margin is on the side V is travelling towards.
    {
        const std::vector<CellIndex> visibleCells = harness.model.cellsOf(visible, bounds);
        CHECK(!visibleCells.empty());
        int visibleTop = visibleCells.front().row;
        int visibleBottom = visibleCells.back().row;
        int topmost = visibleTop;
        int bottommost = visibleBottom;
        for (const TileInfo& tile : harness.model.tiles()) {
            topmost = std::min(topmost, tile.cell.row);
            bottommost = std::max(bottommost, tile.cell.row);
        }
        CHECK((bottommost - visibleBottom) > (visibleTop - topmost));
    }

    // Convergence once the finger leaves the glass.
    int passes = 0;
    while (harness.model.wantsPass() && passes < 5) {
        PassOutput out = harness.pass(input(bounds, 1.0f, visible), "fling settle");
        harness.finishAll(out);
        harness.composite(visible, 64, "fling settle composite");
        ++passes;
    }
    CHECK_LE(passes, 3);
    CHECK(!harness.model.wantsPass());
    CHECK_EQ(harness.model.visibleHoles(), 0u);
    EXPECT_NO_ARROWS(harness);
}

// -------------------------------------------------------------------------
// Scenario 3 - pinch 1 -> 2.85 and pan far away (R7 persistent backdrop, I6,
// I15). This is the 0.1.9.27/28 failure: old tiles far from the pinch origin.
// -------------------------------------------------------------------------
void pinchAndPanFar(bool syncOnScaleCommit)
{
    ModelConfig config;
    config.syncOnScaleCommit = syncOnScaleCommit;
    Harness harness(config);

    const IntSize boundsAt1(screenWidth, 8000);
    settle(harness, boundsAt1, 1.0f, IntRect(0, 0, screenWidth, screenHeight));

    const float scale = 2.85f;
    const IntSize zoomed = scaledBounds(screenWidth, 8000, scale);
    IntRect visible(static_cast<int>(360 * scale) - 360, static_cast<int>(540 * scale) - 540, screenWidth, screenHeight);

    PassOutput commit = harness.pass(input(zoomed, scale, visible), "pinch commit");
    if (syncOnScaleCommit) {
        bool anySync = false;
        for (const PaintRequest& request : commit.paints)
            anySync |= request.mode == PaintMode::Sync;
        CHECK(anySync);
    }
    // The scale-1 grid is now the persistent backdrop and it is not empty.
    CHECK(harness.model.backdropGrid().has_value());
    CHECK_EQ(static_cast<int>(harness.model.backdropGrid()->state), static_cast<int>(GridState::PersistentBackdrop));
    CHECK(harness.model.backdropGrid()->tileCount > 0);

    harness.finishAll(commit);
    DrawList draw = harness.composite(visible, 64, "pinch composite");
    CHECK_EQ(draw.visibleHoles, 0u);

    // Four screens away from the pinch origin.
    for (int i = 1; i <= 16; ++i) {
        visible = IntRect(visible.x, visible.y + 320, screenWidth, screenHeight);
        PassInput in = input(zoomed, scale, visible);
        in.panGesture = true;
        PassOutput out = harness.pass(in, "zoomed pan");
        harness.finishAll(out);
        DrawList panDraw = harness.composite(visible, 64, "zoomed pan composite");
        // I15: blurry (backdrop) is allowed, white is not.
        CHECK_EQ(panDraw.visibleHoles, 0u);
        // I8/I15: the persistent backdrop stays bounded by P and alive.
        CHECK(harness.model.backdropGrid().has_value());
        CHECK_LE(harness.model.backdropGrid()->tileCount, config.persistentBackdropBudget);
    }
    EXPECT_NO_ARROWS(harness);
}

void scenario3_pinchAndPanFar()
{
    check::currentTest = "scenario3_pinchAndPanFar(sync)";
    pinchAndPanFar(true);
    check::currentTest = "scenario3_pinchAndPanFar(async)";
    pinchAndPanFar(false);
}

// -------------------------------------------------------------------------
// Scenario 4 - zoom out 2.85 -> 1 and zoom in 2.85 -> 6 (R7, R9 with K, I8).
// -------------------------------------------------------------------------
void scenario4_zoomOutAndIn()
{
    check::currentTest = "scenario4_zoomOut";
    {
        Harness harness;
        const IntSize boundsAt1(screenWidth, 8000);
        settle(harness, boundsAt1, 1.0f, IntRect(0, 0, screenWidth, screenHeight));

        const float scale = 2.85f;
        const IntSize zoomed = scaledBounds(screenWidth, 8000, scale);
        IntRect zoomedVisible(600, 900, screenWidth, screenHeight);
        harness.step(input(zoomed, scale, zoomedVisible), 64, "zoom in");
        harness.step(input(zoomed, scale, zoomedVisible), 64, "zoom in settle");
        const GridId zoomedGrid = harness.model.primaryGrid()->id;

        // Back to 1:1 over the same content.
        const IntRect visible(210, 315, screenWidth, screenHeight);
        PassOutput out = harness.pass(input(boundsAt1, 1.0f, visible), "zoom out");
        CHECK(harness.model.backdropGrid().has_value());
        CHECK_EQ(harness.model.backdropGrid()->id, zoomedGrid);
        CHECK_EQ(static_cast<int>(harness.model.backdropGrid()->state), static_cast<int>(GridState::TransientBackdrop));
        // The former persistent backdrop is the primary and already has pixels.
        CHECK(readyCount(harness.model, harness.model.primaryGrid()->id) > 0);
        harness.finishAll(out);
        harness.composite(visible, 64, "zoom out composite");

        // R9: gone within K composites of full visible coverage; the commit
        // frame does not count, so K + 1 composites suffice.
        for (unsigned i = 0; i < harness.model.config().backdropCoverageComposites + 1; ++i) {
            if (!harness.model.backdropGrid())
                break;
            harness.composite(visible, 64, "zoom out drain");
        }
        CHECK(!harness.model.backdropGrid().has_value());
        EXPECT_NO_ARROWS(harness);
    }

    check::currentTest = "scenario4_zoomIn";
    {
        Harness harness;
        const IntSize boundsAt1(screenWidth, 8000);
        settle(harness, boundsAt1, 1.0f, IntRect(0, 0, screenWidth, screenHeight));

        const IntSize at285 = scaledBounds(screenWidth, 8000, 2.85f);
        harness.step(input(at285, 2.85f, IntRect(600, 900, screenWidth, screenHeight)), 64, "to 2.85");
        const GridId grid285 = harness.model.primaryGrid()->id;
        CHECK(harness.model.backdropGrid().has_value());
        const GridId persistent = harness.model.backdropGrid()->id;

        const IntSize at6 = scaledBounds(screenWidth, 8000, 6.0f);
        harness.step(input(at6, 6.0f, IntRect(1200, 1900, screenWidth, screenHeight)), 64, "to 6");
        // I8: still exactly one backdrop, and it is the scale-1 one - the 2.85
        // grid was dropped, not kept.
        CHECK(harness.model.backdropGrid().has_value());
        CHECK_EQ(harness.model.backdropGrid()->id, persistent);
        CHECK(harness.model.primaryGrid()->id != grid285);
        CHECK_EQ(static_cast<int>(harness.model.backdropGrid()->state), static_cast<int>(GridState::PersistentBackdrop));
        EXPECT_NO_ARROWS(harness);
    }
}

// -------------------------------------------------------------------------
// Scenario 5 - a scale change while replays are in flight (R7, I13).
// -------------------------------------------------------------------------
void scenario5_scaleChangeMidRaster()
{
    check::currentTest = "scenario5_scaleChangeMidRaster";
    Harness harness;
    const IntSize boundsAt1(screenWidth, 8000);
    settle(harness, boundsAt1, 1.0f, IntRect(0, 0, screenWidth, screenHeight));

    const IntSize at285 = scaledBounds(screenWidth, 8000, 2.85f);
    PassOutput commit = harness.pass(input(at285, 2.85f, IntRect(600, 900, screenWidth, screenHeight)), "commit 2.85");
    CHECK(commit.paints.size() >= 4);
    std::vector<JobId> inFlight;
    for (const PaintRequest& request : commit.paints)
        inFlight.push_back(request.job);

    // Second pinch before the first settles: the 2.85 grid is dropped because
    // the persistent backdrop takes precedence, so every replay is cancelled.
    const IntSize at6 = scaledBounds(screenWidth, 8000, 6.0f);
    PassOutput second = harness.pass(input(at6, 6.0f, IntRect(1200, 1900, screenWidth, screenHeight)), "commit 6");
    std::set<JobId> cancelled(second.cancels.begin(), second.cancels.end());
    for (JobId job : inFlight)
        CHECK(cancelled.count(job) > 0);

    // A late completion of a cancelled replay must not resurrect anything.
    const unsigned readyBefore = readyCount(harness.model, harness.model.primaryGrid()->id);
    for (JobId job : inFlight)
        harness.model.noteReplayFinished(job);
    harness.composite(IntRect(1200, 1900, screenWidth, screenHeight), 64, "after cancel");
    CHECK_EQ(readyCount(harness.model, harness.model.primaryGrid()->id), readyBefore);
    EXPECT_NO_ARROWS(harness);
}

// -------------------------------------------------------------------------
// Scenario 6 - budget smaller than the cover (R1 truncation, I3).
// -------------------------------------------------------------------------
void scenario6_budgetBelowCover()
{
    check::currentTest = "scenario6_budgetBelowCover";
    Harness harness;
    const IntSize bounds = scaledBounds(screenWidth, 4000, 6.0f);
    const IntRect visible(1000, 5000, screenWidth, screenHeight);

    PassInput in = input(bounds, 6.0f, visible);
    in.tileBudget = 8;
    in.panGesture = true;

    PassOutput first = harness.pass(in, "small budget");
    CHECK_LE(harness.model.primaryGrid()->tileCount, 8u);
    // I3: the visible cells are all there despite the truncation.
    for (CellIndex cell : harness.model.cellsOf(visible, bounds))
        CHECK(findTile(harness.model.tiles(), harness.model.primaryGrid()->id, cell) != nullptr);

    // No thrash: an identical pass wants the identical set and asks for nothing
    // it already asked for.
    std::set<std::pair<int, int>> before;
    for (const TileInfo& tile : harness.model.tiles())
        before.insert(std::make_pair(tile.cell.column, tile.cell.row));
    PassOutput second = harness.pass(in, "small budget again");
    CHECK(second.paints.empty());
    CHECK(second.cancels.empty());
    std::set<std::pair<int, int>> after;
    for (const TileInfo& tile : harness.model.tiles())
        after.insert(std::make_pair(tile.cell.column, tile.cell.row));
    CHECK(before == after);
    (void)first;
    EXPECT_NO_ARROWS(harness);
}

// -------------------------------------------------------------------------
// Scenario 7 - dirty rects during a scroll (R10, I10, I12).
// -------------------------------------------------------------------------
void scenario7_dirtyDuringScroll()
{
    check::currentTest = "scenario7_dirtyDuringScroll";
    Harness harness;
    const IntSize bounds(screenWidth, 8000);
    IntRect visible(0, 0, screenWidth, screenHeight);
    settle(harness, bounds, 1.0f, visible);

    for (int i = 0; i < 10; ++i) {
        visible = IntRect(0, i * 100, screenWidth, screenHeight);
        PassInput in = input(bounds, 1.0f, visible);
        in.dirty = IntRect(100, i * 100 + 100, 50, 50);
        const unsigned tilesBefore = harness.model.primaryGrid()->tileCount;
        PassOutput out = harness.pass(in, "dirty scroll");
        // I10: a dirty rect adds and removes nothing and cancels no job.
        CHECK(out.cancels.empty());
        CHECK_EQ(out.removed, 0u);
        CHECK(harness.model.primaryGrid()->tileCount >= tilesBefore);
        // Only the cell the rect falls into may be patched, and only if Ready.
        for (const PaintRequest& request : out.paints) {
            if (!request.isPatch)
                continue;
            CHECK(request.rect.intersects(in.dirty));
        }
        harness.finishAll(out);
        DrawList draw = harness.composite(visible, 64, "dirty scroll composite");
        CHECK_EQ(draw.visibleHoles, 0u);
    }

    // A tile dirtied while its first paint is in flight gets exactly one patch
    // once it lands - not a cancelled replay, not two jobs (I12).
    Harness second;
    const IntRect view(0, 0, screenWidth, screenHeight);
    PassOutput first = second.pass(input(bounds, 1.0f, view), "first paint");
    CHECK(!first.paints.empty());
    const CellIndex cell = first.paints.front().cell;
    PassInput dirtyWhileRastering = input(bounds, 1.0f, view);
    dirtyWhileRastering.dirty = IntRect(10, 10, 40, 40);
    PassOutput during = second.pass(dirtyWhileRastering, "dirty while rastering");
    CHECK(during.paints.empty());
    CHECK(during.cancels.empty());
    second.finishAll(first);
    second.composite(view, 64, "land first paint");
    PassOutput after = second.pass(input(bounds, 1.0f, view), "patch after landing");
    int patches = 0;
    for (const PaintRequest& request : after.paints) {
        if (request.isPatch && request.cell == cell)
            ++patches;
    }
    CHECK_EQ(patches, 1);
    EXPECT_NO_ARROWS(second);
}

// -------------------------------------------------------------------------
// Scenario 8 - sticky header: a store whose bounds change every pass (R6).
// -------------------------------------------------------------------------
void scenario8_stickyHeader()
{
    check::currentTest = "scenario8_stickyHeader";
    Harness harness;
    const IntRect visible(0, 0, 720, 100);

    harness.step(input(IntSize(720, 80), 1.0f, visible), 64, "header warmup");
    const GridId grid = harness.model.primaryGrid()->id;
    CHECK_EQ(readyCount(harness.model, grid), 1u);
    CHECK(harness.model.tiles().front().paintedRect.isSameGeometry(IntRect(0, 0, 720, 80)));

    int previousHeight = 80;
    for (int i = 0; i < 12; ++i) {
        const int height = 80 + (i % 3) * 10;
        PassInput in = input(IntSize(720, height), 1.0f, visible);
        in.dirty = IntRect(0, 0, 720, height);
        PassOutput out = harness.pass(in, "header pass");
        // R6: the one cell keeps its tile and its state. It never goes back to
        // Missing, so it never draws nothing.
        CHECK_EQ(harness.model.primaryGrid()->tileCount, 1u);
        const std::vector<TileInfo> midPass = harness.model.tiles();
        CHECK_EQ(static_cast<int>(midPass.front().state), static_cast<int>(TileState::Ready));
        // Until the patch lands it keeps drawing the pixels it has, at the rect
        // they were rasterised for - the texture is never stretched onto the
        // new height (package 1 review, R6).
        CHECK_EQ(midPass.front().paintedRect.height, previousHeight);
        CHECK(midPass.front().textureSize == midPass.front().paintedRect.size());
        if (height != previousHeight) {
            // And the repair is a whole-cell patch, not a growth strip.
            bool wholeCell = false;
            for (const PaintRequest& request : out.paints)
                wholeCell |= request.isPatch && request.rect.isSameGeometry(IntRect(0, 0, 720, height));
            CHECK(wholeCell);
        }

        harness.finishAll(out);
        DrawList draw = harness.composite(visible, 64, "header composite");
        CHECK_EQ(draw.visibleHoles, 0u);
        CHECK_EQ(draw.tiles.size(), 1u);
        // The patch landed: texture and paintedRect are replaced together, and
        // the draw follows immediately.
        CHECK_EQ(harness.model.tiles().front().paintedRect.height, height);
        CHECK_EQ(draw.tiles.front().target.height, height);
        previousHeight = height;
    }
    EXPECT_NO_ARROWS(harness);

    // A whole-cell repaint that does not change the size keeps the texture:
    // no reallocation storm when the header only redraws itself.
    const TextureId stable = harness.model.tiles().front().texture;
    for (int i = 0; i < 3; ++i) {
        PassInput in = input(IntSize(720, previousHeight), 1.0f, visible);
        in.dirty = IntRect(0, 0, 720, previousHeight);
        PassOutput out = harness.pass(in, "header repaint");
        harness.finishAll(out);
        harness.composite(visible, 64, "header repaint composite");
    }
    CHECK_EQ(harness.model.tiles().front().texture, stable);
    EXPECT_NO_ARROWS(harness);

    // R6 on its own. The loop above passes a dirty rect for the whole header,
    // which would repaint the cell anyway and mask the rule; here the layer
    // resizes and says nothing else, and the whole-cell patch has to come from
    // R6 or the tile keeps drawing the old height forever.
    {
        Harness resized;
        resized.step(input(IntSize(720, 80), 1.0f, visible), 64, "resize warmup");
        CHECK(resized.model.tiles().front().paintedRect.isSameGeometry(IntRect(0, 0, 720, 80)));

        PassOutput out = resized.pass(input(IntSize(720, 140), 1.0f, visible), "resize without a dirty rect");
        bool wholeCell = false;
        for (const PaintRequest& request : out.paints)
            wholeCell |= request.isPatch && request.rect.isSameGeometry(IntRect(0, 0, 720, 140));
        CHECK(wholeCell);
        // Still drawing the pixels it has, at the rect they were made for.
        CHECK_EQ(resized.model.tiles().front().paintedRect.height, 80);

        resized.finishAll(out);
        DrawList draw = resized.composite(visible, 64, "resize composite");
        CHECK_EQ(resized.model.tiles().front().paintedRect.height, 140);
        CHECK_EQ(draw.tiles.size(), 1u);
        CHECK_EQ(draw.tiles.front().target.height, 140);
        EXPECT_NO_ARROWS(resized);
    }

    // And a composite taken while that whole-cell patch is still in flight:
    // the tile draws the pixels it has, at the rect they were rasterised for.
    // Stretching an 80-pixel-tall texture over the 140-pixel cell is exactly
    // what the first cut of R6 did and what I4 now forbids.
    {
        Harness stretched;
        stretched.step(input(IntSize(720, 80), 1.0f, visible), 64, "stretch warmup");
        stretched.pass(input(IntSize(720, 140), 1.0f, visible), "grow, patch in flight");
        DrawList draw = stretched.composite(visible, 64, "grow composite");
        CHECK_EQ(draw.tiles.size(), 1u);
        CHECK_EQ(draw.tiles.front().target.height, 80);
        CHECK(draw.tiles.front().target.size() == stretched.model.tiles().front().textureSize);
        EXPECT_NO_ARROWS(stretched);
    }
}

// -------------------------------------------------------------------------
// Scenario 9 - image store: V unknown, no scale change, no backdrop.
// -------------------------------------------------------------------------
void scenario9_imageStore()
{
    check::currentTest = "scenario9_imageStore";
    Harness harness;
    const IntSize bounds(3000, 2000);
    PassInput in = input(bounds, 1.0f, std::nullopt);
    in.isImage = true;

    PassOutput out = harness.pass(in, "image pass");
    CHECK_EQ(harness.model.primaryGrid()->tileCount, 6u); // 3 columns x 2 rows
    CHECK(!harness.model.backdropGrid().has_value());
    harness.finishAll(out);
    harness.composite(IntRect(0, 0, 3000, 2000), 64, "image composite");
    CHECK_EQ(readyCount(harness.model, harness.model.primaryGrid()->id), 6u);

    // An image store never grows a backdrop, whatever the scale does.
    PassInput scaled = input(IntSize(6000, 4000), 2.0f, std::nullopt);
    scaled.isImage = true;
    harness.pass(scaled, "image scale");
    CHECK(!harness.model.backdropGrid().has_value());
    EXPECT_NO_ARROWS(harness);
}

// -------------------------------------------------------------------------
// Scenario 10 - the upload budget (R4): visible tiles are exempt, the rest
// drain, nothing is cancelled by waiting.
// -------------------------------------------------------------------------
void scenario10_uploadBudget()
{
    check::currentTest = "scenario10_uploadBudget";
    Harness harness;
    const IntSize bounds(3072, 4096);
    const IntRect visible(1000, 1500, screenWidth, screenHeight);

    PassOutput out = harness.pass(input(bounds, 1.0f, visible), "budget pass");
    CHECK(out.paints.size() >= 10);
    harness.finishAll(out);

    const std::vector<CellIndex> visibleCells = harness.model.cellsOf(visible, bounds);
    DrawList first = harness.composite(visible, 2, "budget composite 1");
    // Every visible cell uploaded regardless of the budget of two.
    unsigned exempt = 0;
    for (const UploadRecord& upload : first.uploads) {
        if (upload.budgetExempt)
            ++exempt;
    }
    CHECK_EQ(exempt, static_cast<unsigned>(visibleCells.size()));
    CHECK_EQ(first.uploads.size(), visibleCells.size() + 2);
    CHECK_EQ(first.visibleHoles, 0u);

    unsigned uploaded = static_cast<unsigned>(first.uploads.size());
    for (int i = 0; i < 10 && uploaded < out.paints.size(); ++i) {
        DrawList draw = harness.composite(visible, 2, "budget drain");
        CHECK_LE(draw.uploads.size(), 2u);
        uploaded += static_cast<unsigned>(draw.uploads.size());
        CHECK(draw.cancels.empty());
    }
    CHECK_EQ(uploaded, static_cast<unsigned>(out.paints.size()));
    EXPECT_NO_ARROWS(harness);
}

// -------------------------------------------------------------------------
// Scenario 11 - priority and failure (R5, I9, I12, I13).
// -------------------------------------------------------------------------
void scenario11_priorityAndFailure()
{
    check::currentTest = "scenario11_priorityAndFailure";
    Harness harness;
    const IntSize bounds(3072, 8192);
    IntRect visible(1000, 1500, screenWidth, screenHeight);

    PassOutput first = harness.pass(input(bounds, 1.0f, visible), "queue up");
    CHECK(first.paints.size() >= 10);

    // V moves so that cells that were queued far away become visible.
    visible = IntRect(2000, 3000, screenWidth, screenHeight);
    PassInput in = input(bounds, 1.0f, visible);
    in.panGesture = true;
    PassOutput second = harness.pass(in, "v moved");

    // Cells that left the desired set had their replays cancelled (I13).
    CHECK(!second.cancels.empty());

    // I9: no queued job for a non-visible cell is served before a visible one.
    unsigned worstVisible = 0;
    unsigned bestOther = 0xffffffffu;
    bool haveVisible = false;
    for (const JobInfo& job : harness.model.jobs()) {
        if (job.state != JobState::Queued)
            continue;
        const IntRect rect = harness.model.cellRect(job.cell, bounds);
        if (rect.intersects(visible)) {
            haveVisible = true;
            worstVisible = std::max(worstVisible, job.priority);
        } else
            bestOther = std::min(bestOther, job.priority);
    }
    CHECK(haveVisible);
    if (bestOther != 0xffffffffu)
        CHECK(worstVisible < bestOther);

    // A failed replay on a visible tile yields exactly one new request next
    // pass - no timeout, no escalation (R5, I12).
    CHECK(!second.paints.empty());
    const PaintRequest failing = second.paints.front();
    harness.model.noteReplayFailed(failing.job);
    PassOutput third = harness.pass(in, "after failure");
    int reissued = 0;
    for (const PaintRequest& request : third.paints) {
        if (request.cell == failing.cell && request.grid == failing.grid)
            ++reissued;
    }
    CHECK_EQ(reissued, 1);

    // The watchdog is a plain ReplayFailed too (R5).
    harness.model.noteWatchdog(third.paints.front().job);
    PassOutput fourth = harness.pass(in, "after watchdog");
    int rewatched = 0;
    for (const PaintRequest& request : fourth.paints) {
        if (request.cell == third.paints.front().cell)
            ++rewatched;
    }
    CHECK_EQ(rewatched, 1);
    EXPECT_NO_ARROWS(harness);
}

// -------------------------------------------------------------------------
// Scenario 13 - animation: the same tile dirtied on every pass (R10, I12).
// -------------------------------------------------------------------------
void scenario13_animation()
{
    check::currentTest = "scenario13_animation";
    Harness harness;
    const IntSize bounds(screenWidth, 8000);
    const IntRect visible(0, 0, screenWidth, screenHeight);
    settle(harness, bounds, 1.0f, visible);

    // Worker latency two: a replay posted in pass N lands in pass N + 2.
    std::vector<std::pair<JobId, int>> pending;
    for (int pass = 0; pass < 50; ++pass) {
        for (auto it = pending.begin(); it != pending.end();) {
            if (--it->second <= 0) {
                harness.model.noteReplayFinished(it->first);
                it = pending.erase(it);
            } else
                ++it;
        }

        PassInput in = input(bounds, 1.0f, visible);
        in.dirty = IntRect(50, 50, 120, 60);
        PassOutput out = harness.pass(in, "animation pass");
        for (const PaintRequest& request : out.paints) {
            // R10: never Sync, however often the tile is re-dirtied.
            CHECK_EQ(static_cast<int>(request.mode), static_cast<int>(PaintMode::Async));
            pending.push_back(std::make_pair(request.job, 2));
        }
        // I12: at most one patch job in flight for the animated tile.
        CHECK_LE(pending.size(), 1u + 0u);

        DrawList draw = harness.composite(visible, 64, "animation composite");
        // The tile keeps drawing throughout - the animation never blinks.
        CHECK_EQ(draw.visibleHoles, 0u);
        CHECK(draw.tiles.size() >= 1);
    }
    EXPECT_NO_ARROWS(harness);
}

// -------------------------------------------------------------------------
// Scenario 14 - grid walks over the scale set (R7, I8, I16).
// -------------------------------------------------------------------------
void scenario14_gridWalks()
{
    check::currentTest = "scenario14_gridWalks";
    const float scales[4] = { 1.0f, 2.85f, 6.0f, 2.0f };
    unsigned walks = 0;

    for (int a = 0; a < 4; ++a) {
        for (int b = 0; b < 4; ++b) {
            for (int c = 0; c < 4; ++c) {
                for (int d = 0; d < 4; ++d) {
                    const int sequence[5] = { 0, a, b, c, d };
                    const int between[5] = { 1, (a + b) % 4, (b + c) % 4, (c + d) % 4, 2 };
                    Harness harness;
                    for (int i = 0; i < 5; ++i) {
                        const float scale = scales[sequence[i]];
                        const IntSize bounds = scaledBounds(screenWidth, 6000, scale);
                        IntRect visible(std::min(200, std::max(0, bounds.width - screenWidth)),
                            std::min(1500, std::max(0, bounds.height - screenHeight)), screenWidth, screenHeight);
                        for (int p = 0; p <= between[i]; ++p) {
                            harness.step(input(bounds, scale, visible), 64, "walk");
                            // I8 is asserted inside checkInvariants after every
                            // step; here we only need the machines to stay legal.
                            EXPECT_NO_ARROWS(harness);
                        }
                    }
                    ++walks;
                }
            }
        }
    }
    CHECK_EQ(walks, 256u);
}

// -------------------------------------------------------------------------
// Rule guards
// -------------------------------------------------------------------------

// R1: the margin is the pre-render. Without it the desired set would be
// cells(V) and a scroll of one pixel would show a hole.
void ruleR1_marginIsThePreRender()
{
    check::currentTest = "ruleR1_marginIsThePreRender";
    Harness harness;
    const IntSize bounds(3072, 8000);
    const IntRect visible(1000, 2000, screenWidth, screenHeight);
    harness.pass(input(bounds, 1.0f, visible), "margin");

    // The desired set is V inflated by M, not cells(V): without the margin the
    // grid would hold exactly the visible cells and every scroll step would
    // start from Missing.
    const std::vector<CellIndex> visibleCells = harness.model.cellsOf(visible, bounds);
    CHECK(harness.model.primaryGrid()->tileCount > visibleCells.size());

    // And the margin is doubled on the pan side, where "the pan side" is the
    // sign of the last movement of V and nothing else (package 1 review, R1):
    // two passes ending on the same viewport tile differently depending on
    // whether V moved to get there, and the panGesture flag changes nothing.
    auto maxRowAfter = [&](int firstY, bool panGesture) {
        Harness local;
        const IntSize tall(screenWidth, 20000);
        local.pass(input(tall, 1.0f, IntRect(0, firstY, screenWidth, screenHeight)), "pan a");
        PassInput second = input(tall, 1.0f, IntRect(0, 2600, screenWidth, screenHeight));
        second.panGesture = panGesture;
        local.pass(second, "pan b");
        int maxRow = 0;
        for (const TileInfo& tile : local.model.tiles())
            maxRow = std::max(maxRow, tile.cell.row);
        return maxRow;
    };
    CHECK(maxRowAfter(2000, false) > maxRowAfter(2600, false));
    CHECK_EQ(maxRowAfter(2000, true), maxRowAfter(2000, false));
    CHECK_EQ(maxRowAfter(2600, true), maxRowAfter(2600, false));
}

// R2: reconcile without hysteresis. A tile that leaves the desired set goes,
// there is no keep rect and no erase threshold to argue with.
void ruleR2_noHysteresis()
{
    check::currentTest = "ruleR2_noHysteresis";
    Harness harness;
    const IntSize bounds(screenWidth, 20000);
    harness.step(input(bounds, 1.0f, IntRect(0, 0, screenWidth, screenHeight)), 64, "top");
    CHECK(findTile(harness.model.tiles(), harness.model.primaryGrid()->id, CellIndex(0, 0)) != nullptr);

    harness.pass(input(bounds, 1.0f, IntRect(0, 10000, screenWidth, screenHeight)), "far away");
    CHECK(findTile(harness.model.tiles(), harness.model.primaryGrid()->id, CellIndex(0, 0)) == nullptr);
    CHECK_LE(harness.model.primaryGrid()->tileCount, harness.model.primaryGrid()->budget);
}

// R3: the mode rules. Threaded raster off is always Sync, a small store is
// always Sync, and the visible cells of a scale commit are Sync when
// syncOnScaleCommit is set - and Async when it is not.
void ruleR3_paintModes()
{
    check::currentTest = "ruleR3_paintModes";
    {
        Harness harness;
        PassInput in = input(IntSize(screenWidth, 8000), 1.0f, IntRect(0, 0, screenWidth, screenHeight));
        in.threadedRaster = false;
        PassOutput out = harness.pass(in, "raster off");
        CHECK(!out.paints.empty());
        for (const PaintRequest& request : out.paints)
            CHECK_EQ(static_cast<int>(request.mode), static_cast<int>(PaintMode::Sync));
    }
    {
        Harness harness;
        PassOutput out = harness.pass(input(IntSize(720, 300), 1.0f, IntRect(0, 0, 720, 300)), "small store");
        CHECK(!out.paints.empty());
        for (const PaintRequest& request : out.paints)
            CHECK_EQ(static_cast<int>(request.mode), static_cast<int>(PaintMode::Sync));
    }
    {
        ModelConfig config;
        config.syncOnScaleCommit = false;
        Harness harness(config);
        const IntSize boundsAt1(screenWidth, 8000);
        settle(harness, boundsAt1, 1.0f, IntRect(0, 0, screenWidth, screenHeight));
        const IntSize zoomed = scaledBounds(screenWidth, 8000, 2.85f);
        PassOutput out = harness.pass(input(zoomed, 2.85f, IntRect(600, 900, screenWidth, screenHeight)), "commit async");
        for (const PaintRequest& request : out.paints)
            CHECK_EQ(static_cast<int>(request.mode), static_cast<int>(PaintMode::Async));
    }
}

// R8: a cell that has a Ready tile is drawn from the primary grid and the
// backdrop is never drawn through it. This is the doubled-text/ghost class of
// bugs in one assertion.
void ruleR8_readyCellIsNeverBackdropped()
{
    check::currentTest = "ruleR8_readyCellIsNeverBackdropped";
    Harness harness;
    const IntSize boundsAt1(screenWidth, 8000);
    settle(harness, boundsAt1, 1.0f, IntRect(0, 0, screenWidth, screenHeight));

    const IntSize zoomed = scaledBounds(screenWidth, 8000, 2.85f);
    const IntRect visible(600, 900, screenWidth, screenHeight);
    PassOutput out = harness.pass(input(zoomed, 2.85f, visible), "commit");
    // Land only the first request, so the composite has both a Ready cell and
    // cells that need the backdrop.
    if (!out.paints.empty())
        harness.model.noteReplayFinished(out.paints.front().job);
    DrawList draw = harness.composite(visible, 64, "mixed composite");

    std::set<std::pair<int, int>> readyCells;
    for (const TileDraw& tile : draw.tiles)
        readyCells.insert(std::make_pair(tile.cell.column, tile.cell.row));
    CHECK(!readyCells.empty());
    for (const BackdropClip& clip : draw.backdrop)
        CHECK(readyCells.count(std::make_pair(clip.cell.column, clip.cell.row)) == 0);
}

// R9: the hard bound N ends a transient backdrop even if the primary never
// finishes painting.
void ruleR9_hardBound()
{
    check::currentTest = "ruleR9_hardBound";
    ModelConfig config;
    config.backdropMaxComposites = 5;
    Harness harness(config);

    const IntSize at2 = scaledBounds(screenWidth, 6000, 2.0f);
    harness.step(input(at2, 2.0f, IntRect(100, 500, screenWidth, screenHeight)), 64, "start at 2");

    const IntSize at3 = scaledBounds(screenWidth, 6000, 3.0f);
    const IntRect visible(150, 750, screenWidth, screenHeight);
    harness.pass(input(at3, 3.0f, visible), "to 3"); // no replay ever lands
    CHECK(harness.model.backdropGrid().has_value());
    CHECK_EQ(static_cast<int>(harness.model.backdropGrid()->state), static_cast<int>(GridState::TransientBackdrop));

    for (unsigned i = 0; i < config.backdropMaxComposites; ++i) {
        CHECK(harness.model.backdropGrid().has_value());
        harness.composite(visible, 64, "age out");
    }
    CHECK(!harness.model.backdropGrid().has_value());
    EXPECT_NO_ARROWS(harness);
}

// R11: there is no progressive cover. With frozen inputs and everything Ready
// the model asks for nothing at all - a second mechanism that grew the tile set
// over idle frames would show up here immediately.
void ruleR11_noProgressiveCover()
{
    check::currentTest = "ruleR11_noProgressiveCover";
    Harness harness;
    const IntSize bounds(screenWidth, 8000);
    const IntRect visible(0, 1000, screenWidth, screenHeight);
    settle(harness, bounds, 1.0f, visible);

    const unsigned tiles = harness.model.primaryGrid()->tileCount;
    for (int i = 0; i < 20; ++i) {
        PassOutput out = harness.pass(input(bounds, 1.0f, visible), "idle pass");
        CHECK(out.paints.empty());
        CHECK(out.cancels.empty());
        CHECK_EQ(harness.model.primaryGrid()->tileCount, tiles);
        CHECK(!out.wantsPass);
    }
}

// R12: convergence. Constant inputs and a backend that completes every replay
// reach wantsPass() == false and zero holes inside the bound of I11.
void ruleR12_convergence()
{
    check::currentTest = "ruleR12_convergence";
    Harness harness;
    const IntSize bounds(3072, 8192);
    const IntRect visible(1000, 2000, screenWidth, screenHeight);

    unsigned passes = 0;
    while (harness.model.wantsPass() || !passes) {
        PassOutput out = harness.pass(input(bounds, 1.0f, visible), "converge");
        harness.finishAll(out);
        harness.composite(visible, 2, "converge composite");
        if (++passes > 40)
            break;
    }
    CHECK(!harness.model.wantsPass());
    CHECK_EQ(harness.model.visibleHoles(), 0u);
    const unsigned desired = harness.model.primaryGrid()->desiredCount;
    CHECK_LE(passes, desired + harness.model.config().persistentBackdropBudget + desired / 2 + 2);
}

// I14: the same input sequence yields the same output sequence. No time, no
// address order, no iteration over a hash map.
void invariantI14_determinism()
{
    check::currentTest = "invariantI14_determinism";
    auto run = [](std::vector<std::string>& trace) {
        TileGridModel model(7);
        const IntSize bounds(3072, 8192);
        for (int i = 0; i < 12; ++i) {
            const IntRect visible(i * 40, i * 260, screenWidth, screenHeight);
            PassInput in;
            in.boundsScaled = bounds;
            in.scale = i < 6 ? 1.0f : 2.85f;
            in.boundsScaled = i < 6 ? bounds : IntSize(8755, 23347);
            in.visible = visible;
            in.panGesture = (i % 2) == 0;
            trace.push_back(model.tracePass(in));
            PassOutput out = model.runPass(in);
            trace.push_back(model.traceOut(out));
            for (const PaintRequest& request : out.paints) {
                char buffer[128];
                std::snprintf(buffer, sizeof(buffer), "paint g%u (%d,%d) p%u m%d",
                    request.grid, request.cell.column, request.cell.row, request.priority, static_cast<int>(request.mode));
                trace.push_back(buffer);
                model.noteReplayFinished(request.job);
            }
            DrawList draw = model.composite(visible, 3);
            trace.push_back(model.traceComposite(draw));
        }
    };

    std::vector<std::string> first;
    std::vector<std::string> second;
    run(first);
    run(second);
    CHECK_EQ(first.size(), second.size());
    for (size_t i = 0; i < std::min(first.size(), second.size()); ++i)
        CHECK_EQ(first[i], second[i]);
}

// I16: an event no table accepts is reported, and the machine does not move.
void invariantI16_illegalArrowsAreReported()
{
    check::currentTest = "invariantI16_illegalArrowsAreReported";
    Harness harness;
    const IntSize bounds(screenWidth, 8000);
    const IntRect visible(0, 0, screenWidth, screenHeight);
    PassOutput out = harness.pass(input(bounds, 1.0f, visible), "arrows");
    EXPECT_NO_ARROWS(harness);

    // Uploading outside a composite is refused by the phase machine (I17).
    harness.model.noteUploaded(out.paints.front().job);
    harness.pass(input(bounds, 1.0f, visible), "upload outside composite");
    CHECK(!harness.arrows.empty());
    CHECK_EQ(harness.arrows.front(), std::string("phase:Idle->Upload"));

    // A Requested from outside the model is refused the same way.
    harness.arrows.clear();
    TileEventRecord record;
    record.kind = EventKind::Requested;
    harness.model.noteEvent(record);
    harness.pass(input(bounds, 1.0f, visible), "external request");
    CHECK(!harness.arrows.empty());
    CHECK_EQ(harness.arrows.front(), std::string("phase:Idle->Request"));

    // And a tile-level illegal arrow: an upload for a tile that never landed.
    harness.arrows.clear();
    Harness fresh;
    PassOutput freshOut = fresh.pass(input(bounds, 1.0f, visible), "fresh");
    fresh.model.noteReplayFinished(freshOut.paints.front().job);
    fresh.composite(visible, 64, "land");
    fresh.model.noteReplayFinished(freshOut.paints.front().job);
    fresh.pass(input(bounds, 1.0f, visible), "double finish");
    // The job is gone, so the completion is discarded rather than illegal (R5).
    EXPECT_NO_ARROWS(fresh);
}

// The trace of section 5.3 is the replay format; it has to be exact.
void traceFormat()
{
    check::currentTest = "traceFormat";
    TileGridModel model(3);
    PassInput in;
    in.boundsScaled = IntSize(720, 8000);
    in.scale = 2.85f;
    in.visible = IntRect(10, 20, 720, 1280);
    in.dirty = IntRect(1, 2, 3, 4);
    in.panGesture = true;
    in.tileBudget = 24;
    CHECK_EQ(model.tracePass(in), std::string("tg S3 pass  s=2.85 B=720x8000 V=10,20,720,1280 d=1,2,3,4 pan=1 budget=24"));

    PassInput unknown = in;
    unknown.visible = std::nullopt;
    unknown.scale = 1.0f;
    unknown.panGesture = false;
    CHECK_EQ(model.tracePass(unknown), std::string("tg S3 pass  s=1 B=720x8000 V=- d=1,2,3,4 pan=0 budget=24"));

    PassOutput out;
    out.missing = 1;
    out.rastering = 2;
    out.landed = 3;
    out.ready = 4;
    out.syncJobs = 5;
    out.asyncJobs = 6;
    out.cancels.push_back(1);
    out.removed = 8;
    out.visibleHoles = 9;
    out.backdropKind = 'p';
    out.backdropAge = 3;
    CHECK_EQ(model.traceOut(out),
        std::string("tg S3 out   miss=1 rast=2 land=3 ready=4 jobs=5/6 cancel=1 rm=8 holes=9 bd=p3"));

    out.backdropKind = '-';
    CHECK_EQ(model.traceOut(out),
        std::string("tg S3 out   miss=1 rast=2 land=3 ready=4 jobs=5/6 cancel=1 rm=8 holes=9 bd=-"));

    IllegalArrow arrow { MachineKind::Tile, "Rastering", "Uploaded", 0, CellIndex() };
    CHECK_EQ(formatIllegalArrow(3, arrow), std::string("tg S3 ev    tile:Rastering->Uploaded"));
}

} // namespace

void runScenarios()
{
    scenario1_scroll();
    scenario2_fling();
    scenario3_pinchAndPanFar();
    scenario4_zoomOutAndIn();
    scenario5_scaleChangeMidRaster();
    scenario6_budgetBelowCover();
    scenario7_dirtyDuringScroll();
    scenario8_stickyHeader();
    scenario9_imageStore();
    scenario10_uploadBudget();
    scenario11_priorityAndFailure();
    scenario13_animation();
    scenario14_gridWalks();

    ruleR1_marginIsThePreRender();
    ruleR2_noHysteresis();
    ruleR3_paintModes();
    ruleR8_readyCellIsNeverBackdropped();
    ruleR9_hardBound();
    ruleR11_noProgressiveCover();
    ruleR12_convergence();
    invariantI14_determinism();
    invariantI16_illegalArrowsAreReported();
    traceFormat();
}

} // namespace tilegrid
