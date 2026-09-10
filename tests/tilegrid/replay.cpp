// Apotheosis: replay of a device trace. A `tg S<id> pass ...` line of
// docs/TILEGRID-DESIGN.md section 5.3 IS a PassInput, so a stage.txt pulled off
// the Lumia becomes a test case: one model per store id, every line fed in,
// the invariants of section 4 asserted after each one.
//
// This is the property the old zoom/ghost/cover traces lacked - they recorded
// the store's own beliefs, so a session in which the screen had holes produced
// no line at all. sample-stage.txt is a synthetic session (scroll, pinch to
// 2.85, pan far, zoom back) until the first v2 device round provides a real
// one; that one becomes scenario 12.

#include "check.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>

#ifndef TILEGRID_TEST_DATA_DIR
#define TILEGRID_TEST_DATA_DIR "."
#endif

namespace tilegrid {

namespace {

struct ParsedPass {
    unsigned storeId { 0 };
    PassInput input;
};

// The `out` line of section 5.3, i.e. what the device's model answered for the
// pass line above it. Only the counters; `ref=` is new in this package and the
// 0.1.9.29 log does not have it, so it is optional.
struct ParsedOut {
    unsigned storeId { 0 };
    unsigned missing { 0 };
    unsigned rastering { 0 };
    unsigned landed { 0 };
    unsigned ready { 0 };
    unsigned syncJobs { 0 };
    unsigned asyncJobs { 0 };
    unsigned cancels { 0 };
    unsigned removed { 0 };
    unsigned holes { 0 };
    char backdrop[16] { };
};

bool parseOutLine(const std::string& line, ParsedOut& parsed)
{
    if (line.compare(0, 3, "tg ") != 0)
        return false;
    const int fields = std::sscanf(line.c_str(),
        "tg S%u out miss=%u rast=%u land=%u ready=%u jobs=%u/%u cancel=%u rm=%u holes=%u bd=%15s",
        &parsed.storeId, &parsed.missing, &parsed.rastering, &parsed.landed, &parsed.ready,
        &parsed.syncJobs, &parsed.asyncJobs, &parsed.cancels, &parsed.removed, &parsed.holes,
        parsed.backdrop);
    return fields == 11;
}

bool parseCompositeLine(const std::string& line, unsigned& storeId)
{
    if (line.compare(0, 3, "tg ") != 0)
        return false;
    char visible[64] = { 0 };
    unsigned holes = 0;
    unsigned clips = 0;
    return std::sscanf(line.c_str(), "tg S%u comp V=%63s holes=%u bdclips=%u",
        &storeId, visible, &holes, &clips) == 4;
}

bool parsePassLine(const std::string& line, ParsedPass& parsed)
{
    if (line.compare(0, 3, "tg ") != 0)
        return false;
    unsigned storeId = 0;
    char scaleText[32] = { 0 };
    int boundsWidth = 0;
    int boundsHeight = 0;
    char visibleText[64] = { 0 };
    char dirtyText[64] = { 0 };
    int pan = 0;
    unsigned budget = 0;

    const int fields = std::sscanf(line.c_str(),
        "tg S%u pass s=%31s B=%dx%d V=%63s d=%63s pan=%d budget=%u",
        &storeId, scaleText, &boundsWidth, &boundsHeight, visibleText, dirtyText, &pan, &budget);
    if (fields != 8)
        return false;

    parsed.storeId = storeId;
    parsed.input = PassInput();
    parsed.input.scale = static_cast<float>(std::atof(scaleText));
    parsed.input.boundsScaled = IntSize(boundsWidth, boundsHeight);
    parsed.input.panGesture = pan != 0;
    parsed.input.tileBudget = budget;

    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    if (std::strcmp(visibleText, "-") != 0) {
        if (std::sscanf(visibleText, "%d,%d,%d,%d", &x, &y, &w, &h) != 4)
            return false;
        parsed.input.visible = IntRect(x, y, w, h);
    }
    if (std::sscanf(dirtyText, "%d,%d,%d,%d", &x, &y, &w, &h) == 4)
        parsed.input.dirty = IntRect(x, y, w, h);

    // Apotheosis (0.1.9.34): `img=` is the last field and was added after the
    // 0.1.9.29 log was taken, so it is optional - the sscanf above stops at
    // budget= and ignores whatever follows.
    const std::size_t image = line.find(" img=");
    if (image != std::string::npos)
        parsed.input.isImage = line[image + 5] != '0';
    return true;
}

} // namespace

// Scenario 12, defined below runReplay() because it is the long one.
static void runDeviceReplay();
static void runDeviceReplayOffscreen();

void runReplay()
{
    check::currentTest = "replay_parser";

    // The parser has to round-trip what the model prints.
    {
        TileGridModel model(5);
        PassInput in;
        in.boundsScaled = IntSize(2052, 22800);
        in.scale = 2.85f;
        in.visible = IntRect(666, 999, 720, 1280);
        in.dirty = IntRect(0, 0, 0, 0);
        in.panGesture = true;
        in.tileBudget = 24;
        ParsedPass parsed;
        CHECK(parsePassLine(model.tracePass(in), parsed));
        CHECK_EQ(parsed.storeId, 5u);
        CHECK_EQ(parsed.input.boundsScaled.width, in.boundsScaled.width);
        CHECK_EQ(parsed.input.boundsScaled.height, in.boundsScaled.height);
        CHECK(parsed.input.visible.has_value());
        CHECK(parsed.input.visible->isSameGeometry(*in.visible));
        CHECK_EQ(parsed.input.tileBudget, 24u);
        CHECK(parsed.input.panGesture);

        in.visible = std::nullopt;
        CHECK(parsePassLine(model.tracePass(in), parsed));
        CHECK(!parsed.input.visible.has_value());
    }

    check::currentTest = "replay_sample_stage";
    const std::string path = std::string(TILEGRID_TEST_DATA_DIR) + "/sample-stage.txt";
    std::ifstream stream(path.c_str());
    if (!stream) {
        ::check::fail(__FILE__, __LINE__, "sample-stage.txt not found", path);
        return;
    }

    std::map<unsigned, std::unique_ptr<Harness>> stores;
    std::string line;
    unsigned replayed = 0;
    while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        ParsedPass parsed;
        if (!parsePassLine(line, parsed))
            continue;
        auto it = stores.find(parsed.storeId);
        if (it == stores.end())
            it = stores.emplace(parsed.storeId, std::make_unique<Harness>(ModelConfig(), parsed.storeId)).first;
        Harness& harness = *it->second;

        PassOutput out = harness.pass(parsed.input, "replay");
        harness.finishAll(out);
        const IntRect visible = parsed.input.visible
            ? *parsed.input.visible
            : IntRect(0, 0, parsed.input.boundsScaled.width, parsed.input.boundsScaled.height);
        harness.composite(visible, 2, "replay composite");
        EXPECT_NO_ARROWS(harness);
        ++replayed;
    }
    CHECK(replayed > 10);

    runDeviceReplay();
    runDeviceReplayOffscreen();
}

// -------------------------------------------------------------------------
// Scenario 12 - the first v2 device session (0.1.9.29, a news site, 2026-09-08),
// replays/lumia-0.1.9.29-news.txt: the `tg` lines of the first 26 000 lines of
// that session's stage.txt, 107 stores, 12 407 passes, covering the load, the
// 1:1 scroll, the pinch to 4.05x and the long zoomed steady state after it.
//
// What it asserts, and why each one is a device symptom:
//
//  * the invariants of section 4 hold on every line of a real session (the
//    Harness runs them after every pass and every composite);
//  * no machine of section 2.2b takes an illegal arrow anywhere in it;
//  * every store CONVERGES once its input stops changing (R12) - this is F1.
//    On the device store S100 (the news site's sticky header) reported
//    `rast=4 land=0 ready=0 jobs=0/0` for 1 900 consecutive passes because a
//    refused raster request left its tiles in Rastering with no job behind
//    them, and store S1 held the driver in an extra-composite loop.
//
// The counters of the `out` lines are compared and the first divergence per
// store is printed rather than failed. Two reasons, both structural: the replay
// lands every job in the pass that asked for it, so rast/land/ready cannot
// match a device with a worker pool; and the hole count of a store with an
// unknown visible rect legitimately differs now, because holes are counted over
// the desired cells and not over cells(B) (F2). What the log's own numbers are
// evidence for is written above - they are the failure, not the reference.
// -------------------------------------------------------------------------
static void runDeviceReplay()
{
    check::currentTest = "replay_device_0_1_9_29";
    const std::string path = std::string(TILEGRID_TEST_DATA_DIR) + "/replays/lumia-0.1.9.29-news.txt";
    std::ifstream stream(path.c_str());
    if (!stream) {
        ::check::fail(__FILE__, __LINE__, "device replay not found", path);
        return;
    }

    struct Store {
        std::unique_ptr<Harness> harness;
        PassInput lastInput;
        bool sawPass { false };
        bool knownVisible { false };
        unsigned passes { 0 };
        unsigned composites { 0 };
        unsigned compared { 0 };
        unsigned diverged { 0 };
        std::string firstDivergence;
        PassOutput lastOut;
    };

    std::map<unsigned, Store> stores;
    std::string line;
    unsigned passes = 0;
    unsigned composites = 0;

    auto visibleOf = [](const PassInput& in) {
        return in.visible ? *in.visible : IntRect(0, 0, in.boundsScaled.width, in.boundsScaled.height);
    };

    while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();

        ParsedPass parsedPass;
        if (parsePassLine(line, parsedPass)) {
            Store& store = stores[parsedPass.storeId];
            if (!store.harness)
                store.harness = std::make_unique<Harness>(ModelConfig(), parsedPass.storeId);
            store.lastInput = parsedPass.input;
            store.sawPass = true;
            store.knownVisible = parsedPass.input.visible.has_value();
            ++store.passes;
            ++passes;
            store.lastOut = store.harness->pass(parsedPass.input, "device replay pass");
            store.harness->finishAll(store.lastOut);
            EXPECT_NO_ARROWS(*store.harness);
            continue;
        }

        ParsedOut parsedOut;
        if (parseOutLine(line, parsedOut)) {
            auto it = stores.find(parsedOut.storeId);
            if (it == stores.end() || !it->second.sawPass)
                continue;
            Store& store = it->second;
            ++store.compared;
            const PassOutput& out = store.lastOut;
            const unsigned deviceTiles = parsedOut.missing + parsedOut.rastering + parsedOut.landed + parsedOut.ready;
            const unsigned modelTiles = out.missing + out.rastering + out.landed + out.ready;
            if (deviceTiles != modelTiles || parsedOut.removed != out.removed
                || parsedOut.syncJobs + parsedOut.asyncJobs != out.syncJobs + out.asyncJobs) {
                ++store.diverged;
                if (store.firstDivergence.empty()) {
                    store.firstDivergence = "pass " + std::to_string(store.passes)
                        + ": device tiles=" + std::to_string(deviceTiles)
                        + " jobs=" + std::to_string(parsedOut.syncJobs + parsedOut.asyncJobs)
                        + " rm=" + std::to_string(parsedOut.removed)
                        + " holes=" + std::to_string(parsedOut.holes)
                        + " | model tiles=" + std::to_string(modelTiles)
                        + " jobs=" + std::to_string(out.syncJobs + out.asyncJobs)
                        + " rm=" + std::to_string(out.removed)
                        + " holes=" + std::to_string(out.visibleHoles);
                }
            }
            continue;
        }

        unsigned compositeStore = 0;
        if (parseCompositeLine(line, compositeStore)) {
            auto it = stores.find(compositeStore);
            if (it == stores.end() || !it->second.sawPass)
                continue;
            Store& store = it->second;
            ++store.composites;
            ++composites;
            store.harness->composite(visibleOf(store.lastInput), 2, "device replay composite");
            EXPECT_NO_ARROWS(*store.harness);
        }
    }

    CHECK(passes > 10000);
    CHECK(composites > 1000);
    CHECK(stores.size() > 50);

    // R12 on a real session: with the last input repeated and a backend that
    // completes every job, every store comes to rest. This is the assertion the
    // 0.1.9.29 device build fails - not by being slow, but by never asking for
    // the tiles it is missing again.
    unsigned unsettled = 0;
    for (auto& entry : stores) {
        Store& store = entry.second;
        if (!store.harness || !store.sawPass)
            continue;
        unsigned rounds = 0;
        while (rounds < 24 && store.harness->model.wantsPass()) {
            PassOutput out = store.harness->pass(store.lastInput, "device replay settle");
            store.harness->finishAll(out);
            store.harness->composite(visibleOf(store.lastInput), 64, "device replay settle composite");
            ++rounds;
        }
        if (store.harness->model.wantsPass() || store.harness->model.visibleHoles()) {
            ++unsettled;
            ::check::fail(__FILE__, __LINE__, "a replayed store never settled",
                "S" + std::to_string(entry.first) + " wantsPass="
                + std::to_string(store.harness->model.wantsPass() ? 1 : 0)
                + " holes=" + std::to_string(store.harness->model.visibleHoles())
                + " passes=" + std::to_string(store.passes));
        } else
            ++::check::checks;
        EXPECT_NO_ARROWS(*store.harness);
    }
    CHECK_EQ(unsettled, 0u);

    // Evidence, not a verdict: the counters the device wrote next to the ones
    // the model produces here. See the comment above for why they cannot be
    // equal and what the difference is worth.
    unsigned divergentStores = 0;
    for (const auto& entry : stores) {
        if (!entry.second.diverged)
            continue;
        ++divergentStores;
        if (divergentStores <= 6) {
            std::printf("  replay divergence S%u (V %s, %u/%u passes): %s\n", entry.first,
                entry.second.knownVisible ? "known" : "unknown",
                entry.second.diverged, entry.second.compared,
                entry.second.firstDivergence.c_str());
        }
    }
    std::printf("  replay 0.1.9.29: %u passes, %u composites, %u stores, %u with divergent counters\n",
        passes, composites, static_cast<unsigned>(stores.size()), divergentStores);
}

// -------------------------------------------------------------------------
// The 0.1.9.34 session (2026-09-09, an image-heavy social timeline, then a news site, package 4 with
// image stores on v2), replays/lumia-0.1.9.34-offscreen.txt: the `tg` lines of
// lines 14 047 - 32 000 of that session's stage.txt, 17 909
// lines over ~220 stores.
//
// What it adds to scenario 12 is the composite side of the trace being ABSENT.
// A store whose layer is off screen produces `pass` and `out` lines on every
// tick and never a `comp` line, because TextureMapperLayer never paints it -
// and an upload happens only in a composite (R4). 23 stores of this session
// therefore sat at `land=1 ready=0` for hundreds of consecutive passes with a
// raster buffer held, and R12 kept asking for the pass that could not help
// them. So this replay deliberately does NOT composite a store the trace did
// not composite, and asserts what the model owes on its own:
//
//   * a store whose visible rect does not meet its bounds holds no tile and
//     wants no pass (I18, checked by the harness after every line anyway);
//   * every store settles once its input stops changing - the settle loop
//     below composites only the stores the device composited.
// -------------------------------------------------------------------------
static void runDeviceReplayOffscreen()
{
    check::currentTest = "replay_device_0_1_9_34_offscreen";
    const std::string path = std::string(TILEGRID_TEST_DATA_DIR) + "/replays/lumia-0.1.9.34-offscreen.txt";
    std::ifstream stream(path.c_str());
    if (!stream) {
        ::check::fail(__FILE__, __LINE__, "device replay not found", path);
        return;
    }

    struct Store {
        std::unique_ptr<Harness> harness;
        PassInput lastInput;
        bool sawPass { false };
        bool sawComposite { false };
        unsigned passes { 0 };
    };

    std::map<unsigned, Store> stores;
    std::string line;
    unsigned passes = 0;
    unsigned composites = 0;

    auto visibleOf = [](const PassInput& in) {
        return in.visible ? *in.visible : IntRect(0, 0, in.boundsScaled.width, in.boundsScaled.height);
    };
    auto showsNothing = [](const PassInput& in) {
        if (!in.visible)
            return false;
        return in.visible->intersection(IntRect(0, 0, in.boundsScaled.width, in.boundsScaled.height)).isEmpty();
    };

    while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();

        ParsedPass parsedPass;
        if (parsePassLine(line, parsedPass)) {
            Store& store = stores[parsedPass.storeId];
            if (!store.harness)
                store.harness = std::make_unique<Harness>(ModelConfig(), parsedPass.storeId);
            store.lastInput = parsedPass.input;
            store.sawPass = true;
            ++store.passes;
            ++passes;
            PassOutput out = store.harness->pass(parsedPass.input, "0.1.9.34 replay pass");
            store.harness->finishAll(out);
            EXPECT_NO_ARROWS(*store.harness);
            continue;
        }

        unsigned compositeStore = 0;
        if (parseCompositeLine(line, compositeStore)) {
            auto it = stores.find(compositeStore);
            if (it == stores.end() || !it->second.sawPass)
                continue;
            Store& store = it->second;
            store.sawComposite = true;
            ++composites;
            store.harness->composite(visibleOf(store.lastInput), 2, "0.1.9.34 replay composite");
            EXPECT_NO_ARROWS(*store.harness);
        }
    }

    CHECK(passes > 7000);
    CHECK(stores.size() > 100);

    unsigned offscreen = 0;
    unsigned unsettled = 0;
    for (auto& entry : stores) {
        Store& store = entry.second;
        if (!store.harness || !store.sawPass)
            continue;

        // The device's own ending: the last frame repeated, with a composite
        // only where the device had one. A store that was never composited must
        // come to rest all the same - that is the bug this replay carries.
        for (unsigned round = 0; round < 24 && store.harness->model.wantsPass(); ++round) {
            PassOutput out = store.harness->pass(store.lastInput, "0.1.9.34 replay settle");
            store.harness->finishAll(out);
            if (store.sawComposite)
                store.harness->composite(visibleOf(store.lastInput), 64, "0.1.9.34 replay settle composite");
        }

        if (showsNothing(store.lastInput)) {
            ++offscreen;
            if (!store.harness->model.tiles().empty() || store.harness->model.wantsPass()) {
                ++unsettled;
                ::check::fail(__FILE__, __LINE__, "an off-screen store kept raster work alive",
                    "S" + std::to_string(entry.first) + " tiles="
                    + std::to_string(store.harness->model.tiles().size()) + " wantsPass="
                    + std::to_string(store.harness->model.wantsPass() ? 1 : 0)
                    + " passes=" + std::to_string(store.passes));
            } else
                ++::check::checks;
        } else if (store.sawComposite && store.harness->model.wantsPass()) {
            ++unsettled;
            ::check::fail(__FILE__, __LINE__, "a composited store never settled",
                "S" + std::to_string(entry.first) + " passes=" + std::to_string(store.passes));
        }
        EXPECT_NO_ARROWS(*store.harness);
    }
    CHECK_EQ(unsettled, 0u);
    CHECK(offscreen > 20);

    std::printf("  replay 0.1.9.34: %u passes, %u composites, %u stores, %u of them off screen\n",
        passes, composites, static_cast<unsigned>(stores.size()), offscreen);
}

} // namespace tilegrid
