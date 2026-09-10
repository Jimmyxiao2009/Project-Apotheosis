// Apotheosis: host tests for the TileGrid v2 model (docs/TILEGRID-DESIGN.md 5.4).
// No test framework: a CHECK macro, a failure counter, and a tiny harness that
// every test file shares. The executable's exit code is the failure count.

#pragma once

#include "TextureMapperTileGridModel.h"

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace check {

inline int failures = 0;
inline int checks = 0;
inline std::string currentTest = "<none>";

inline void fail(const char* file, int line, const char* expr, const std::string& detail)
{
    ++failures;
    std::printf("FAIL %s\n  %s:%d\n  %s%s\n", currentTest.c_str(), file, line, expr,
        detail.empty() ? "" : ("\n  " + detail).c_str());
}

template<typename T> std::string text(const T& value)
{
    std::ostringstream stream;
    stream << value;
    return stream.str();
}

} // namespace check

#define CHECK(expr) do { ++::check::checks; if (!(expr)) ::check::fail(__FILE__, __LINE__, #expr, ""); } while (0)
#define CHECK_EQ(a, b) do { ++::check::checks; const auto va_ = (a); const auto vb_ = (b); \
    if (!(va_ == vb_)) ::check::fail(__FILE__, __LINE__, #a " == " #b, ::check::text(va_) + " != " + ::check::text(vb_)); } while (0)
#define CHECK_LE(a, b) do { ++::check::checks; const auto va_ = (a); const auto vb_ = (b); \
    if (!(va_ <= vb_)) ::check::fail(__FILE__, __LINE__, #a " <= " #b, ::check::text(va_) + " > " + ::check::text(vb_)); } while (0)

// ---------------------------------------------------------------------------
// Shared harness. Drives the model and runs the invariants after every step;
// the raster/upload fakes arrive in package 1b, so replay completion is fed in
// from the tests directly.
// ---------------------------------------------------------------------------

namespace tilegrid {

using namespace WebCore::TileGrid;

std::string describe(const IntRect&);
std::string describe(CellIndex);

// invariants.cpp - I1 to I17. Callable after every step.
void checkInvariants(const TileGridModel&, const char* where);
void checkPassOutput(const TileGridModel&, const PassOutput&, const char* where);
void checkDrawList(const TileGridModel&, const DrawList&, const IntRect& visible, const char* where);
void resetReleaseLedger(unsigned storeId);

struct Harness {
    explicit Harness(const ModelConfig& config = ModelConfig(), unsigned storeId = 1)
        : model(storeId, config)
    {
        model.setIllegalArrowObserver([this](const IllegalArrow& arrow) {
            arrows.push_back(std::string(machineKindName(arrow.machine)) + ":" + arrow.from + "->" + arrow.event);
        });
        resetReleaseLedger(storeId);
    }

    TileGridModel model;
    std::vector<std::string> arrows;
    std::vector<std::string> trace;
    PassInput lastInput;

    PassOutput pass(const PassInput& in, const char* where = "pass")
    {
        lastInput = in;
        trace.push_back(model.tracePass(in));
        PassOutput out = model.runPass(in);
        trace.push_back(model.traceOut(out));
        checkInvariants(model, where);
        checkPassOutput(model, out, where);
        return out;
    }

    DrawList composite(const IntRect& visible, unsigned maxUploads, const char* where = "composite")
    {
        DrawList draw = model.composite(visible, maxUploads);
        trace.push_back(model.traceComposite(draw));
        checkInvariants(model, where);
        checkDrawList(model, draw, visible, where);
        return draw;
    }

    // Every replay of this pass reports success.
    void finishAll(const PassOutput& out)
    {
        for (const PaintRequest& request : out.paints)
            model.noteReplayFinished(request.job);
    }

    void finishOnly(const PassOutput& out, bool patches)
    {
        for (const PaintRequest& request : out.paints) {
            if (request.isPatch == patches)
                model.noteReplayFinished(request.job);
        }
    }

    // One full step: pass, replays land, composite.
    DrawList step(const PassInput& in, unsigned maxUploads = 64, const char* where = "step")
    {
        PassOutput out = pass(in, where);
        finishAll(out);
        return composite(in.visible ? *in.visible : IntRect(0, 0, in.boundsScaled.width, in.boundsScaled.height), maxUploads, where);
    }
};

// Reports every illegal arrow with its text and clears the list, so one arrow
// does not cascade into a failure per later step.
inline void expectNoArrows(Harness& harness, const char* file, int line)
{
    for (const std::string& arrow : harness.arrows) {
        ++::check::checks;
        ::check::fail(file, line, "unexpected illegal arrow", arrow);
    }
    harness.arrows.clear();
}

#define EXPECT_NO_ARROWS(harness) ::tilegrid::expectNoArrows((harness), __FILE__, __LINE__)

// Entry points of the four test files.
void runScenarios();
void runFuzz();
void runReplay();

} // namespace tilegrid
