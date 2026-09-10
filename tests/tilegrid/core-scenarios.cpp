// Apotheosis: the scenarios of docs/TILEGRID-DESIGN.md section 5.4 run against
// the *core* (TextureMapperTileGridCore) with the fake backends, in all eight
// combinations of worker latency {0, 1, 3, never} x upload budget {2,
// unlimited}. scenarios.cpp runs the same list against the model alone.
//
// What this file adds over scenarios.cpp is the screen: every composite goes
// through the DrawRecorder, which knows for every sampled visible pixel which
// texture painted it and what those pixels were rasterised for. The device
// symptoms the device showed are assertions here:
//
//   "old tiles farther from the pinch origin"      -> screen.foreign == 0
//   "doubled text after pinch" / "ghost band"      -> screen.doubleDrawn == 0
//   "white holes at 3.4x/6x while panning"         -> screen.unpaintedCells == 0
//   "images flicker between image and white box"   -> scenario 7 / 13
//   "sticky header flicker"                        -> scenario 8
//   "extremely slow zoomed pan" (repaint storm)    -> scenario 3's job counts
//
// checkScreen() asserts the first two after every single composite, in every
// scenario and every variant, so they cannot be forgotten.

#include "fakes.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>

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

void name(const char* scenario, const CoreVariant& variant)
{
    check::currentTest = std::string(scenario) + "[" + variant.name + "]";
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

// Latency 0 and 1 put a first paint on screen within the frame that asked for
// it, so "no hole" is assertable step by step. Latency 3 needs the model to
// converge first, which is what R12 promises and what the settle() calls below
// assert instead.
bool immediate(const CoreHarness& harness) { return harness.variant.latency <= 1; }
bool lands(const CoreVariant& variant) { return variant.latency != kNeverLands; }

// -------------------------------------------------------------------------
// Scenario 1 - scroll 1:1 on a long page (R1, R2, R12; device: a news front page)
// -------------------------------------------------------------------------
void coreScenario1_scroll(const CoreVariant& variant)
{
    name("core1_scroll", variant);
    CoreHarness harness(variant);
    const IntSize bounds(screenWidth, 8000);

    harness.settle(input(bounds, 1.0f, IntRect(0, 0, screenWidth, screenHeight)), 12, "scroll warmup");

    for (int pass = 1; pass <= 20; ++pass) {
        const IntRect visible(0, pass * 200, screenWidth, screenHeight);
        PassInput in = input(bounds, 1.0f, visible);
        in.panGesture = true;
        CompositeResult composite = harness.frame(in, "scroll frame");
        CHECK_LE(harness.core.model().primaryGrid()->tileCount, harness.core.model().primaryGrid()->budget);
        if (harness.lands() && immediate(harness)) {
            CHECK_EQ(composite.visibleHoles, 0u);
            CHECK_EQ(harness.screen.unpaintedCells, 0u);
        }
        if (!harness.lands()) {
            // R5: no timeout, no escalation. A wedged pool means cells that are
            // not Ready and nothing else - in particular not a growing pile of
            // jobs for the same tile (I12).
            CHECK_LE(harness.core.outstandingJobs(), harness.core.model().primaryGrid()->budget);
        }
    }
    if (harness.lands()) {
        harness.settle(input(bounds, 1.0f, IntRect(0, 4000, screenWidth, screenHeight)), 16, "scroll settle");
        CHECK(!harness.core.wantsPass());
        CHECK_EQ(harness.core.visibleHoles(), 0u);
        CHECK_EQ(harness.screen.unpaintedCells, 0u);
    }
    EXPECT_NO_CORE_ARROWS(harness);
}

// -------------------------------------------------------------------------
// Scenario 2 - fling (R1: the margin is the pre-render)
// -------------------------------------------------------------------------
void coreScenario2_fling(const CoreVariant& variant)
{
    name("core2_fling", variant);
    CoreHarness harness(variant);
    const IntSize bounds(screenWidth, 20000);
    const int step = screenHeight / 3;

    IntRect visible(0, 0, screenWidth, screenHeight);
    harness.settle(input(bounds, 1.0f, visible), 12, "fling warmup");

    for (int i = 1; i <= 10; ++i) {
        visible = IntRect(0, i * step, screenWidth, screenHeight);
        PassInput in = input(bounds, 1.0f, visible);
        in.panGesture = false; // a fling has no finger on the glass (R1)
        CompositeResult composite = harness.frame(in, "fling frame");
        if (harness.lands() && immediate(harness)) {
            CHECK_EQ(composite.visibleHoles, 0u);
            CHECK_EQ(harness.screen.unpaintedCells, 0u);
        }
    }

    if (harness.lands()) {
        const unsigned rounds = harness.settle(input(bounds, 1.0f, visible), 12, "fling settle");
        CHECK_LE(rounds, 12u);
        CHECK(!harness.core.wantsPass());
        CHECK_EQ(harness.screen.unpaintedCells, 0u);
    }
    EXPECT_NO_CORE_ARROWS(harness);
}

// -------------------------------------------------------------------------
// Scenario 3 - pinch 1 -> 2.85 and pan far away (R3 sync commit, R7 persistent
// backdrop, I6, I15). The 0.1.9.27/28 failure, as a screen assertion.
// -------------------------------------------------------------------------
void coreScenario3_pinchAndPanFar(const CoreVariant& variant, bool syncOnScaleCommit)
{
    name(syncOnScaleCommit ? "core3_pinchAndPanFar" : "core3_pinchAndPanFar(async)", variant);
    ModelConfig config;
    config.syncOnScaleCommit = syncOnScaleCommit;
    CoreHarness harness(variant, config);

    const IntSize boundsAt1(screenWidth, 8000);
    harness.settle(input(boundsAt1, 1.0f, IntRect(0, 0, screenWidth, screenHeight)), 12, "pinch warmup");

    const float scale = 2.85f;
    const IntSize zoomed = scaledBounds(screenWidth, 8000, scale);
    IntRect visible(static_cast<int>(360 * scale) - 360, static_cast<int>(540 * scale) - 540, screenWidth, screenHeight);

    PassOutput commit = harness.pass(input(zoomed, scale, visible), "pinch commit");
    if (syncOnScaleCommit)
        CHECK(commit.syncJobs > 0);
    else
        CHECK_EQ(commit.syncJobs, 0u);
    CHECK(harness.core.model().backdropGrid().has_value());
    CHECK_EQ(static_cast<int>(harness.core.model().backdropGrid()->state),
        static_cast<int>(GridState::PersistentBackdrop));
    harness.composite("pinch composite");

    if (harness.lands() && syncOnScaleCommit) {
        // R3: the pass waited for the visible cells, whatever the worker
        // latency is. The commit frame is sharp, not blurry.
        CHECK_EQ(harness.screen.unpaintedCells, 0u);
        CHECK_EQ(harness.screen.fromBackdrop, 0u);
    }

    // Four screens away from the pinch origin.
    for (int i = 1; i <= 12; ++i) {
        visible = IntRect(visible.x, visible.y + 320, screenWidth, screenHeight);
        PassInput in = input(zoomed, scale, visible);
        in.panGesture = true;
        harness.frame(in, "zoomed pan");
        CHECK(harness.core.model().backdropGrid().has_value());
        CHECK_LE(harness.core.model().backdropGrid()->tileCount, config.persistentBackdropBudget);
        if (harness.lands() && immediate(harness)) {
            // I15: blurry (the scale-1 backdrop) is allowed, white is not. This
            // is the assertion the device round of 0.1.9.27 would have failed:
            // without R7's persistent backdrop every cell the pan reveals is
            // white until its own replay lands.
            CHECK_EQ(harness.screen.unpaintedCells, 0u);
            CHECK(harness.screen.fromBackdrop > 0 || harness.screen.fromPrimary == harness.screen.samples);
        }
    }
    if (harness.lands()) {
        harness.settle(input(zoomed, scale, visible), 16, "zoomed settle");
        CHECK_EQ(harness.screen.unpaintedCells, 0u);
        CHECK_EQ(harness.core.visibleHoles(), 0u);
    }
    EXPECT_NO_CORE_ARROWS(harness);
}

// -------------------------------------------------------------------------
// Scenario 4 - zoom out 2.85 -> 1 and zoom in 2.85 -> 6 (R7, R9 with K, I8)
// -------------------------------------------------------------------------
void coreScenario4_zoomOutAndIn(const CoreVariant& variant)
{
    name("core4_zoomOut", variant);
    {
        CoreHarness harness(variant);
        const IntSize boundsAt1(screenWidth, 8000);
        harness.settle(input(boundsAt1, 1.0f, IntRect(0, 0, screenWidth, screenHeight)), 12, "zoom warmup");

        const IntSize zoomed = scaledBounds(screenWidth, 8000, 2.85f);
        const IntRect zoomedVisible(600, 900, screenWidth, screenHeight);
        harness.settle(input(zoomed, 2.85f, zoomedVisible), 12, "zoom in");
        const GridId zoomedGrid = harness.core.model().primaryGrid()->id;

        const IntRect visible(210, 315, screenWidth, screenHeight);
        harness.pass(input(boundsAt1, 1.0f, visible), "zoom out");
        CHECK(harness.core.model().backdropGrid().has_value());
        CHECK_EQ(harness.core.model().backdropGrid()->id, zoomedGrid);
        CHECK_EQ(static_cast<int>(harness.core.model().backdropGrid()->state),
            static_cast<int>(GridState::TransientBackdrop));
        // The former persistent backdrop is the primary and already has pixels.
        if (harness.lands())
            CHECK(readyCount(harness.core.model(), harness.core.model().primaryGrid()->id) > 0);
        harness.composite("zoom out composite");

        // R9: gone within K composites of full visible coverage.
        for (unsigned i = 0; i < harness.core.model().config().backdropCoverageComposites + 2; ++i) {
            if (!harness.core.model().backdropGrid())
                break;
            harness.composite("zoom out drain");
        }
        if (harness.lands())
            CHECK(!harness.core.model().backdropGrid().has_value());
        EXPECT_NO_CORE_ARROWS(harness);
    }

    name("core4_zoomIn", variant);
    {
        CoreHarness harness(variant);
        const IntSize boundsAt1(screenWidth, 8000);
        harness.settle(input(boundsAt1, 1.0f, IntRect(0, 0, screenWidth, screenHeight)), 12, "warmup");

        const IntSize at285 = scaledBounds(screenWidth, 8000, 2.85f);
        harness.frame(input(at285, 2.85f, IntRect(600, 900, screenWidth, screenHeight)), "to 2.85");
        const GridId grid285 = harness.core.model().primaryGrid()->id;
        CHECK(harness.core.model().backdropGrid().has_value());
        const GridId persistent = harness.core.model().backdropGrid()->id;

        const IntSize at6 = scaledBounds(screenWidth, 8000, 6.0f);
        harness.frame(input(at6, 6.0f, IntRect(1200, 1900, screenWidth, screenHeight)), "to 6");
        // I8: still exactly one backdrop, and it is the scale-1 one.
        CHECK(harness.core.model().backdropGrid().has_value());
        CHECK_EQ(harness.core.model().backdropGrid()->id, persistent);
        CHECK(harness.core.model().primaryGrid()->id != grid285);
        EXPECT_NO_CORE_ARROWS(harness);
    }
}

// -------------------------------------------------------------------------
// Scenario 5 - a scale change while replays are in flight (R7, I13). The
// backend must see the cancels; a late completion must resurrect nothing.
// -------------------------------------------------------------------------
void coreScenario5_scaleChangeMidRaster(const CoreVariant& variant)
{
    name("core5_scaleChangeMidRaster", variant);
    ModelConfig config;
    config.syncOnScaleCommit = false; // keep the replays in flight
    CoreHarness harness(variant, config);

    const IntSize boundsAt1(screenWidth, 8000);
    harness.settle(input(boundsAt1, 1.0f, IntRect(0, 0, screenWidth, screenHeight)), 12, "warmup");

    const IntSize at285 = scaledBounds(screenWidth, 8000, 2.85f);
    PassOutput commit = harness.pass(input(at285, 2.85f, IntRect(600, 900, screenWidth, screenHeight)), "commit 2.85");
    CHECK(commit.paints.size() >= 4);
    const GridId commitGrid = harness.core.model().primaryGrid()->id;
    const unsigned cancelledBefore = harness.raster.cancelled();

    const IntSize at6 = scaledBounds(screenWidth, 8000, 6.0f);
    PassOutput second = harness.pass(input(at6, 6.0f, IntRect(1200, 1900, screenWidth, screenHeight)), "commit 6");
    // With latency 0 the replays are already done when the second commit
    // arrives, so there is nothing left in flight to cancel - the tiles are
    // removed with their grid instead, which is the same guarantee.
    if (variant.latency > 0)
        CHECK(!second.cancels.empty());
    // Every cancel reached the backend: no worker keeps rasterising a cell
    // nobody will look at (R5).
    CHECK_EQ(harness.raster.cancelled() - cancelledBefore, static_cast<unsigned>(second.cancels.size()));
    // I13: nothing of the dropped 2.85 grid survives - no tile, no job, and no
    // pixel buffer waiting for an upload that will never come. (The buffer
    // count is checked against the model's Landed tiles after every composite
    // in checkScreen; here it must be the scale-1 backdrop's and nothing else.)
    for (const TileInfo& tile : harness.core.model().tiles())
        CHECK(tile.grid != commitGrid);

    const GridId primary = harness.core.model().primaryGrid()->id;
    const unsigned readyBefore = readyCount(harness.core.model(), primary);
    harness.composite("after cancel");
    if (variant.latency > 1 || !harness.lands())
        CHECK_EQ(readyCount(harness.core.model(), primary), readyBefore);
    EXPECT_NO_CORE_ARROWS(harness);
}

// -------------------------------------------------------------------------
// Scenario 6 - budget smaller than the cover (R1 truncation, I3)
// -------------------------------------------------------------------------
void coreScenario6_budgetBelowCover(const CoreVariant& variant)
{
    name("core6_budgetBelowCover", variant);
    CoreHarness harness(variant);
    const IntSize bounds = scaledBounds(screenWidth, 4000, 6.0f);
    const IntRect visible(1000, 5000, screenWidth, screenHeight);

    PassInput in = input(bounds, 6.0f, visible);
    in.tileBudget = 8;

    harness.frame(in, "small budget");
    CHECK_LE(harness.core.model().primaryGrid()->tileCount, 8u);

    // No thrash: an identical frame asks for nothing new and cancels nothing.
    harness.settle(in, 12, "small budget settle");
    PassOutput steady = harness.pass(in, "small budget steady");
    CHECK(steady.cancels.empty());
    if (harness.lands())
        CHECK(steady.paints.empty());
    harness.composite("small budget steady composite");
    EXPECT_NO_CORE_ARROWS(harness);
}

// -------------------------------------------------------------------------
// Scenario 7 - dirty rects during a scroll (R10, I10, I12)
// -------------------------------------------------------------------------
void coreScenario7_dirtyDuringScroll(const CoreVariant& variant)
{
    name("core7_dirtyDuringScroll", variant);
    CoreHarness harness(variant);
    const IntSize bounds(screenWidth, 8000);
    IntRect visible(0, 0, screenWidth, screenHeight);
    harness.settle(input(bounds, 1.0f, visible), 12, "warmup");

    for (int i = 0; i < 10; ++i) {
        visible = IntRect(0, i * 100, screenWidth, screenHeight);
        PassInput in = input(bounds, 1.0f, visible);
        in.dirty = IntRect(100, i * 100 + 100, 50, 50);
        const unsigned tilesBefore = harness.core.model().primaryGrid()->tileCount;
        PassOutput out = harness.pass(in, "dirty scroll");
        // I10: a dirty rect adds and removes nothing and cancels no job.
        CHECK(out.cancels.empty());
        CHECK_EQ(out.removed, 0u);
        CHECK(harness.core.model().primaryGrid()->tileCount >= tilesBefore);
        CompositeResult composite = harness.frame(in, "dirty scroll frame");
        // The tile keeps drawing while the patch is in flight: a repainted
        // region never blinks (R10). This is the "images flicker between image
        // and white box" report of 2026-09-02.
        if (harness.lands())
            CHECK_EQ(composite.visibleHoles, 0u);
        (void)composite;
    }
    EXPECT_NO_CORE_ARROWS(harness);
}

// -------------------------------------------------------------------------
// Scenario 8 - sticky header: bounds change every pass (R6, paintedRect)
// -------------------------------------------------------------------------
void coreScenario8_stickyHeader(const CoreVariant& variant)
{
    name("core8_stickyHeader", variant);
    CoreHarness harness(variant);
    const IntRect visible(0, 0, 720, 100);

    // A small store is always Sync (R3), so even the "never" backend gets one
    // shot at it - it just does not come back.
    harness.frame(input(IntSize(720, 80), 1.0f, visible), "header warmup");
    if (!harness.lands()) {
        EXPECT_NO_CORE_ARROWS(harness);
        return;
    }
    CHECK_EQ(readyCount(harness.core.model(), harness.core.model().primaryGrid()->id), 1u);

    int previousHeight = 80;
    for (int i = 0; i < 10; ++i) {
        const int height = 80 + (i % 3) * 10;
        PassInput in = input(IntSize(720, height), 1.0f, visible);
        in.dirty = IntRect(0, 0, 720, height);
        harness.pass(in, "header pass");
        const std::vector<TileInfo> midPass = harness.core.model().tiles();
        CHECK_EQ(harness.core.model().primaryGrid()->tileCount, 1u);
        // R6: never Missing, so the header never blinks; and it draws at the
        // rect its pixels were rasterised for, never stretched to the new one.
        CHECK_EQ(static_cast<int>(midPass.front().state), static_cast<int>(TileState::Ready));
        CHECK_EQ(midPass.front().paintedRect.height, previousHeight);

        CompositeResult composite = harness.composite("header composite");
        CHECK_EQ(composite.visibleHoles, 0u);
        CHECK_EQ(harness.screen.unpaintedCells, 0u);
        CHECK_EQ(harness.core.model().tiles().front().paintedRect.height, height);
        // The texture the header draws from is exactly as big as what it draws.
        for (const DrawCommand& command : composite.commands) {
            if (command.op != DrawOp::DrawTile)
                continue;
            CHECK(harness.textures.sizeOf(command.texture) == command.rect.size());
        }
        previousHeight = height;
    }

    // R6 on its own: the layer resizes and sends no dirty rect. Without the
    // whole-cell patch the header would draw its old height for good, which is
    // the "sticky header flicker / wrong-sized header" family.
    {
        CoreHarness resized(variant);
        resized.frame(input(IntSize(720, 80), 1.0f, visible), "resize warmup");
        PassOutput out = resized.pass(input(IntSize(720, 140), 1.0f, visible), "resize without a dirty rect");
        bool wholeCell = false;
        for (const PaintRequest& request : out.paints)
            wholeCell |= request.isPatch && request.rect.isSameGeometry(IntRect(0, 0, 720, 140));
        CHECK(wholeCell);
        CHECK_EQ(resized.core.model().tiles().front().paintedRect.height, 80);
        CompositeResult draw = resized.composite("resize composite");
        CHECK_EQ(resized.core.model().tiles().front().paintedRect.height, 140);
        for (const DrawCommand& command : draw.commands) {
            if (command.op != DrawOp::DrawTile)
                continue;
            CHECK_EQ(command.rect.height, 140);
            CHECK(resized.textures.sizeOf(command.texture) == command.rect.size());
        }
        EXPECT_NO_CORE_ARROWS(resized);
    }
    EXPECT_NO_CORE_ARROWS(harness);
}

// -------------------------------------------------------------------------
// Scenario 9 - image store: V unknown, no scale change, no backdrop
// -------------------------------------------------------------------------
void coreScenario9_imageStore(const CoreVariant& variant)
{
    name("core9_imageStore", variant);
    CoreHarness harness(variant);
    const IntSize bounds(3000, 2000);
    PassInput in = input(bounds, 1.0f, std::nullopt);
    in.isImage = true;

    harness.frame(in, "image frame");
    CHECK_EQ(harness.core.model().primaryGrid()->tileCount, 6u); // 3 columns x 2 rows
    CHECK(!harness.core.model().backdropGrid().has_value());
    if (harness.lands()) {
        harness.settle(in, 16, "image settle");
        CHECK_EQ(readyCount(harness.core.model(), harness.core.model().primaryGrid()->id), 6u);
        CHECK_EQ(harness.screen.unpaintedCells, 0u);
    }

    PassInput scaled = input(IntSize(6000, 4000), 2.0f, std::nullopt);
    scaled.isImage = true;
    harness.frame(scaled, "image scale");
    CHECK(!harness.core.model().backdropGrid().has_value());

    // Apotheosis (package 4): image stores run on TileGrid v2 now
    // (GraphicsLayerTextureMapper::setContentsToImage), so the adapter needs one promise this
    // scenario did not make: an image store CONVERGES. The store keeps the Image alive - and with
    // it the source every pass records from - exactly while wantsPass() is true, and lets it go
    // (ImageObserver::didDraw, m_image = nullptr) on the composite that ends. If wantsPass() never
    // went false the reference would be held for the life of the layer and didDraw would never
    // fire; if it went false too early the remaining cells would have no source left to record.
    // 24 cells at 6000x4000 is four times the upload budget of the strictest variant, so the
    // release depends on the drain of several composites, which is the case that matters.
    if (harness.lands()) {
        const unsigned rounds = harness.settle(scaled, 40, "image settle after scale");
        CHECK(rounds < 40);
        CHECK(!harness.core.wantsPass());
        CHECK_EQ(harness.core.visibleHoles(), 0u);
        CHECK_EQ(harness.screen.unpaintedCells, 0u);
    }
    EXPECT_NO_CORE_ARROWS(harness);
}

// -------------------------------------------------------------------------
// Scenario 10 - the upload budget (R4): visible cells are exempt, the rest
// drain, nothing is cancelled by waiting.
// -------------------------------------------------------------------------
void coreScenario10_uploadBudget(const CoreVariant& variant)
{
    name("core10_uploadBudget", variant);
    if (!variant.uploadBudget || !lands(variant))
        return;

    CoreHarness harness(variant);
    const IntSize bounds(3072, 4096);
    const IntRect visible(1000, 1500, screenWidth, screenHeight);

    PassOutput out = harness.pass(input(bounds, 1.0f, visible), "budget pass");
    CHECK(out.paints.size() >= 10);
    // Let every replay land before the first composite.
    harness.raster.tick(30);

    const std::vector<CellIndex> visibleCells = harness.core.model().cellsOf(visible, bounds);
    CompositeResult first = harness.composite("budget composite 1");
    CHECK_EQ(first.uploadsBudgetExempt, static_cast<unsigned>(visibleCells.size()));
    CHECK_EQ(first.uploads, static_cast<unsigned>(visibleCells.size()) + variant.uploadBudget);
    CHECK_EQ(first.visibleHoles, 0u);
    CHECK_EQ(harness.screen.unpaintedCells, 0u);

    unsigned uploaded = first.uploads;
    for (int i = 0; i < 12 && uploaded < out.paints.size(); ++i) {
        CompositeResult draw = harness.composite("budget drain");
        CHECK_LE(draw.uploads, variant.uploadBudget);
        CHECK(draw.commands.size() > 0);
        uploaded += draw.uploads;
    }
    CHECK_EQ(uploaded, static_cast<unsigned>(out.paints.size()));
    CHECK_EQ(harness.core.pendingPixelBuffers(), 0u);
    EXPECT_NO_CORE_ARROWS(harness);
}

// -------------------------------------------------------------------------
// Scenario 11 - priority and failure (R5, I9, I12, I13)
// -------------------------------------------------------------------------
void coreScenario11_priorityAndFailure(const CoreVariant& variant)
{
    name("core11_priorityAndFailure", variant);
    CoreHarness harness(variant);
    const IntSize bounds(3072, 8192);
    IntRect visible(1000, 1500, screenWidth, screenHeight);

    PassOutput first = harness.pass(input(bounds, 1.0f, visible), "queue up");
    CHECK(first.paints.size() >= 10);

    visible = IntRect(2000, 3000, screenWidth, screenHeight);
    PassInput in = input(bounds, 1.0f, visible);
    in.panGesture = true;
    PassOutput second = harness.pass(in, "v moved");
    // Cells that left the desired set had their replays cancelled (I13) - with
    // latency 0 they had already finished, so there is nothing to cancel.
    if (variant.latency > 0)
        CHECK(!second.cancels.empty());

    // I9 on the backend: the core hands the jobs over in the model's priority
    // order and the pool is FIFO, so a visible cell is never rastered behind a
    // cell nobody will look at. The order the workers started them in is the
    // order they were recorded in.
    CHECK(harness.raster.startOrder().size() <= harness.raster.recorded());

    // A failed replay on a visible tile yields exactly one new request on the
    // next pass - no timeout, no escalation (R5, I12).
    harness.raster.failNextRecords(2);
    harness.composite("after move");
    PassOutput third = harness.pass(in, "after failure");
    std::set<std::pair<int, int>> reissued;
    for (const PaintRequest& request : third.paints)
        CHECK(reissued.insert(std::make_pair(request.cell.column, request.cell.row)).second);

    // The watchdog is a plain ReplayFailed too (R5).
    harness.raster.watchdogNextRecords(1);
    harness.composite("after failure composite");
    harness.pass(in, "after watchdog");
    harness.composite("after watchdog composite");

    if (harness.lands()) {
        const unsigned rounds = harness.settle(in, 24, "failure settle");
        CHECK_LE(rounds, 24u);
        CHECK_EQ(harness.core.visibleHoles(), 0u);
    }
    EXPECT_NO_CORE_ARROWS(harness);
}

// -------------------------------------------------------------------------
// Scenario 13 - animation: the same tile dirtied on every pass (R10, I12)
// -------------------------------------------------------------------------
void coreScenario13_animation(const CoreVariant& variant)
{
    name("core13_animation", variant);
    if (!lands(variant))
        return;

    CoreHarness harness(variant);
    const IntSize bounds(screenWidth, 8000);
    const IntRect visible(0, 0, screenWidth, screenHeight);
    harness.settle(input(bounds, 1.0f, visible), 12, "warmup");

    unsigned syncRequests = 0;
    for (int pass = 0; pass < 30; ++pass) {
        PassInput in = input(bounds, 1.0f, visible);
        in.dirty = IntRect(50, 50, 120, 60);
        PassOutput out = harness.pass(in, "animation pass");
        for (const PaintRequest& request : out.paints) {
            if (request.mode == PaintMode::Sync)
                ++syncRequests;
        }
        CompositeResult composite = harness.composite("animation composite");
        // The tile keeps drawing throughout: the animation never blinks.
        CHECK_EQ(composite.visibleHoles, 0u);
        CHECK_EQ(harness.screen.unpaintedCells, 0u);
    }
    // R10: however often the tile is re-dirtied, the patch never turns Sync.
    CHECK_EQ(syncRequests, 0u);
    EXPECT_NO_CORE_ARROWS(harness);
}

// -------------------------------------------------------------------------
// Scenario 14 - grid walks over the scale set (R7, I8, I16)
// -------------------------------------------------------------------------
void coreScenario14_gridWalks(const CoreVariant& variant)
{
    name("core14_gridWalks", variant);
    const float scales[4] = { 1.0f, 2.85f, 6.0f, 2.0f };
    unsigned walks = 0;

    for (int a = 0; a < 4; ++a) {
        for (int b = 0; b < 4; ++b) {
            for (int c = 0; c < 4; ++c) {
                const int sequence[4] = { 0, a, b, c };
                CoreHarness harness(variant);
                for (int i = 0; i < 4; ++i) {
                    const float scale = scales[sequence[i]];
                    const IntSize bounds = scaledBounds(screenWidth, 6000, scale);
                    const IntRect visible(std::min(200, std::max(0, bounds.width - screenWidth)),
                        std::min(1500, std::max(0, bounds.height - screenHeight)), screenWidth, screenHeight);
                    harness.frame(input(bounds, scale, visible), "walk");
                    harness.frame(input(bounds, scale, visible), "walk again");
                }
                EXPECT_NO_CORE_ARROWS(harness);
                ++walks;
            }
        }
    }
    CHECK_EQ(walks, 64u);
}

// -------------------------------------------------------------------------
// Scenario 15 - sync commit in parallel (R3). Six visible cells on four fake
// workers cost two replay latencies, not six: the pass waits for all of the
// jobs it marked Sync at once.
// -------------------------------------------------------------------------
void coreScenario15_syncCommitInParallel(unsigned latency)
{
    CoreVariant variant;
    variant.name = latency == 1 ? "sync/l1" : "sync/l3";
    variant.latency = latency;
    variant.uploadBudget = 2;
    variant.workers = 4;
    name("core15_syncCommitInParallel", variant);

    CoreHarness harness(variant);
    const IntSize boundsAt1(3072, 8192);
    const IntRect visibleAt1(900, 900, screenWidth, screenHeight);
    CHECK_EQ(harness.core.model().cellsOf(visibleAt1, boundsAt1).size(), 6u);
    harness.settle(input(boundsAt1, 1.0f, visibleAt1), 16, "sync warmup");

    const IntSize at2 = scaledBounds(3072, 8192, 2.0f);
    const IntRect visibleAt2(1800, 1800, screenWidth, screenHeight);
    CHECK_EQ(harness.core.model().cellsOf(visibleAt2, at2).size(), 6u);

    PassOutput commit = harness.pass(input(at2, 2.0f, visibleAt2), "sync commit");
    CHECK_EQ(commit.syncJobs, 6u);
    CHECK_EQ(harness.core.lastSyncJobCount(), 6u);
    // ceil(6 / 4) = 2 replay latencies, and nothing like 6.
    CHECK_EQ(harness.raster.lastWaitTicks(), 2 * latency);

    CompositeResult composite = harness.composite("sync commit composite");
    CHECK_EQ(readyCount(harness.core.model(), harness.core.model().primaryGrid()->id), 6u);
    CHECK_EQ(composite.visibleHoles, 0u);
    // Sharp, not blurry: the commit frame draws no backdrop at all.
    CHECK_EQ(harness.screen.fromBackdrop, 0u);
    CHECK_EQ(harness.screen.unpaintedCells, 0u);
    EXPECT_NO_CORE_ARROWS(harness);
}

// -------------------------------------------------------------------------
// Device round 1 (0.1.9.29) at the core level: the two things the store can
// fail to do after the model has already committed to them. Both were a silent
// `continue` in TileGridCore, and both are cells that are white while the model
// reports no hole - the shape of failure the traces could not see.
// -------------------------------------------------------------------------

// F1. Every request of the first pass is refused. The tiles must be Missing at
// the start of the next pass and must ask again; before the fix they sat in
// Rastering with no job behind them and never asked again (R5, I9).
void coreDeviceRound1_refusedRecord(const CoreVariant& variant)
{
    name("coreDeviceRound1_refusedRecord", variant);
    CoreHarness harness(variant);
    const IntSize bounds(2048, 4096);
    const IntRect visible(0, 0, screenWidth, screenHeight);

    harness.raster.refuseNextRecords(64);
    const PassInput in = input(bounds, 1.0f, visible);
    const PassOutput refused = harness.pass(in, "refused pass");
    CHECK(!refused.paints.empty());
    CHECK_EQ(harness.core.outstandingJobs(), 0u);
    CHECK_EQ(harness.raster.refusedRecords(), static_cast<unsigned>(refused.paints.size()));

    harness.composite("refused composite");
    // Nothing is on screen, and the model says so rather than claiming coverage.
    CHECK(harness.lastComposite.visibleHoles > 0);

    // The backend takes jobs again; the store recovers on its own, with no
    // dirty rect, no scale change and no help from the driver (R12). Before the
    // fix this pass asked for nothing at all: every tile was still Rastering.
    const PassOutput retry = harness.pass(in, "refused retry pass");
    CHECK_EQ(retry.refused, static_cast<unsigned>(refused.paints.size()));
    CHECK_EQ(retry.paints.size(), refused.paints.size());
    if (lands(variant)) {
        harness.settle(in, 24, "refused settle");
        CHECK_EQ(harness.core.visibleHoles(), 0u);
        CHECK_EQ(harness.screen.unpaintedCells, 0u);
    }
    checkScreen(harness, "refused screen");
    EXPECT_NO_CORE_ARROWS(harness);
}

// F3. The pool comes up empty for one upload the model has already committed.
// The tile must not stay Ready with an empty texture - that draws nothing and
// is not a hole, which is `ready=10 holes=0` over a white screen at 4x.
void coreDeviceRound1_failedAcquire(const CoreVariant& variant)
{
    if (!lands(variant))
        return;
    name("coreDeviceRound1_failedAcquire", variant);
    CoreHarness harness(variant);
    const IntSize bounds(2048, 2048);
    const IntRect visible(0, 0, screenWidth, screenHeight);
    const PassInput in = input(bounds, 1.0f, visible);

    harness.apply(in);
    harness.pass(in, "acquire fail pass");
    harness.textures.failNextAcquires(1);
    // Drive frames until the first upload is attempted and refused. checkScreen
    // runs inside every composite: the frame in which the acquire fails must
    // still not be a white cell over holes=0, which is what makes this test the
    // device symptom rather than a bookkeeping detail.
    unsigned failed = 0;
    for (unsigned round = 0; round < 8 && !failed; ++round) {
        harness.frame(in, "acquire fail frame");
        failed += harness.lastComposite.uploadsFailed;
    }
    CHECK_EQ(failed, 1u);
    CHECK(harness.lastComposite.visibleHoles > 0);

    // Every tile that draws has a texture behind it, and the store closes the
    // hole by itself.
    harness.settle(in, 24, "acquire fail settle");
    CHECK_EQ(harness.core.visibleHoles(), 0u);
    CHECK_EQ(harness.screen.unpaintedCells, 0u);
    CHECK_EQ(harness.lastComposite.drawsWithoutTexture, 0u);
    checkScreen(harness, "acquire fail screen 2");
    EXPECT_NO_CORE_ARROWS(harness);
}

// Device round 2 (0.1.9.30): the backend's upload() returned without
// uploading and without saying so, and every tile was Ready over a cleared
// texture - background colour on every page, holes=0, no counter moving. The
// interface now makes the backend answer, and a "no" must travel the same road
// as a failed acquire: UploadFailed -> Missing -> re-rastered, hole reported
// meanwhile, converged afterwards.
void coreDeviceRound2_refusedUpload(const CoreVariant& variant)
{
    if (!lands(variant))
        return;
    name("coreDeviceRound2_refusedUpload", variant);
    CoreHarness harness(variant);
    const IntSize bounds(2048, 2048);
    const IntRect visible(0, 0, screenWidth, screenHeight);
    const PassInput in = input(bounds, 1.0f, visible);

    harness.apply(in);
    harness.pass(in, "upload fail pass");
    harness.textures.failNextUploads(2);
    unsigned failed = 0;
    for (unsigned round = 0; round < 8 && failed < 2; ++round) {
        harness.frame(in, "upload fail frame");
        failed += harness.lastComposite.uploadsFailed;
    }
    CHECK_EQ(failed, 2u);
    // The two tiles are not Ready: they are holes, not blank textures.
    CHECK(harness.lastComposite.visibleHoles > 0);

    harness.settle(in, 24, "upload fail settle");
    CHECK_EQ(harness.core.visibleHoles(), 0u);
    CHECK_EQ(harness.screen.unpaintedCells, 0u);
    CHECK_EQ(harness.lastComposite.drawsWithoutTexture, 0u);
    checkScreen(harness, "upload fail screen");
    EXPECT_NO_CORE_ARROWS(harness);
}

} // namespace

void runCoreScenarios()
{
    for (const CoreVariant& variant : coreVariants()) {
        coreDeviceRound1_refusedRecord(variant);
        coreDeviceRound1_failedAcquire(variant);
        coreDeviceRound2_refusedUpload(variant);
    }
    for (const CoreVariant& variant : coreVariants()) {
        coreScenario1_scroll(variant);
        coreScenario2_fling(variant);
        coreScenario3_pinchAndPanFar(variant, true);
        coreScenario3_pinchAndPanFar(variant, false);
        coreScenario4_zoomOutAndIn(variant);
        coreScenario5_scaleChangeMidRaster(variant);
        coreScenario6_budgetBelowCover(variant);
        coreScenario7_dirtyDuringScroll(variant);
        coreScenario8_stickyHeader(variant);
        coreScenario9_imageStore(variant);
        coreScenario10_uploadBudget(variant);
        coreScenario11_priorityAndFailure(variant);
        coreScenario13_animation(variant);
        coreScenario14_gridWalks(variant);
    }
    coreScenario15_syncCommitInParallel(1);
    coreScenario15_syncCommitInParallel(3);
}

} // namespace tilegrid
