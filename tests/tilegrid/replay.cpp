// Apotheosis: replay of a device trace. A `tg S<id> pass ...` line of
// TILING-REWRITE-PLAN.md section 5.3 IS a PassInput, so a stage.txt pulled off
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
    return true;
}

} // namespace

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
}

} // namespace tilegrid
