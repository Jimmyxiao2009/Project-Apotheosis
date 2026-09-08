// Apotheosis: seeded random op sequences over the model. The invariants run
// after every single step, so "a combination nobody intended" (root cause 3 of
// the rewrite) either cannot be reached or fails here on the PC.
//
// Three properties beyond the invariants:
//   * no machine ever takes an illegal arrow, whatever the order of events (I16)
//   * once the inputs freeze and every replay completes, the model converges
//     and stops asking for passes (R12, I11)
//   * random walks over the scale set {1, 2.85, 6, 2} keep exactly one
//     backdrop alive (I8) - every path a pinch can take is a walk on the grid
//     machine's graph.

#include "check.h"

#include <algorithm>
#include <random>
#include <vector>

namespace tilegrid {

namespace {

constexpr int screenWidth = 720;
constexpr int screenHeight = 1280;
constexpr int contentWidth = 720;
constexpr int contentHeight = 9000;

const float g_scales[4] = { 1.0f, 2.85f, 6.0f, 2.0f };

IntSize boundsFor(float scale, int contentHeightNow)
{
    return IntSize(static_cast<int>(contentWidth * scale + 0.5f), static_cast<int>(contentHeightNow * scale + 0.5f));
}

int clampInt(int value, int low, int high)
{
    return std::max(low, std::min(value, high));
}

PassInput input_(IntSize bounds, float scale, IntRect visible)
{
    PassInput in;
    in.boundsScaled = bounds;
    in.scale = scale;
    in.visible = visible;
    return in;
}

struct Fuzzer {
    explicit Fuzzer(unsigned seed)
        : rng(seed)
        , harness()
    {
    }

    std::mt19937 rng;
    Harness harness;
    float scale { 1.0f };
    int contentHeightNow { contentHeight };
    IntRect visible { 0, 0, screenWidth, screenHeight };
    std::vector<std::pair<JobId, int>> pending;
    unsigned steps { 0 };

    unsigned pick(unsigned n) { return std::uniform_int_distribution<unsigned>(0, n - 1)(rng); }

    PassInput makeInput()
    {
        const IntSize bounds = boundsFor(scale, contentHeightNow);
        visible.x = clampInt(visible.x, 0, std::max(0, bounds.width - screenWidth));
        visible.y = clampInt(visible.y, 0, std::max(0, bounds.height - screenHeight));
        PassInput in;
        in.boundsScaled = bounds;
        in.scale = scale;
        in.visible = visible;
        in.panGesture = pick(2) == 0;
        in.tileBudget = 8 + pick(17);
        if (pick(4) == 0)
            in.dirty = IntRect(visible.x + static_cast<int>(pick(400)), visible.y + static_cast<int>(pick(900)),
                20 + static_cast<int>(pick(300)), 20 + static_cast<int>(pick(300)));
        return in;
    }

    void landSome(bool all)
    {
        for (auto it = pending.begin(); it != pending.end();) {
            if (all || --it->second <= 0) {
                if (!all && pick(8) == 0)
                    harness.model.noteReplayFailed(it->first);
                else
                    harness.model.noteReplayFinished(it->first);
                it = pending.erase(it);
            } else
                ++it;
        }
    }

    void run(unsigned stepCount)
    {
        for (unsigned i = 0; i < stepCount; ++i) {
            switch (pick(8)) {
            case 0:
            case 1:
            case 2:
                visible.y += static_cast<int>(pick(900)) - 200;
                break;
            case 3:
                scale = g_scales[pick(4)];
                break;
            case 4:
                contentHeightNow = 2000 + static_cast<int>(pick(12000));
                break;
            case 5:
                visible.x += static_cast<int>(pick(600)) - 300;
                break;
            default:
                break;
            }

            landSome(false);

            // A texture can vanish under us (pool reclaim). Ready -> Missing.
            if (pick(20) == 0) {
                const std::vector<TileInfo> tiles = harness.model.tiles();
                if (!tiles.empty()) {
                    const TileInfo& victim = tiles[pick(static_cast<unsigned>(tiles.size()))];
                    if (victim.state == TileState::Ready)
                        harness.model.noteTextureLost(victim.grid, victim.cell);
                }
            }

            PassInput in = makeInput();
            PassOutput out = harness.pass(in, "fuzz pass");
            for (const PaintRequest& request : out.paints)
                pending.push_back(std::make_pair(request.job, 1 + static_cast<int>(pick(3))));

            harness.composite(*in.visible, pick(4), "fuzz composite");
            EXPECT_NO_ARROWS(harness);
            ++steps;
        }
    }

    // Freeze the inputs and complete every replay: the model must come to rest.
    void converge()
    {
        PassInput frozen = makeInput();
        frozen.dirty = IntRect();
        frozen.panGesture = false;
        frozen.tileBudget = 24;

        unsigned passes = 0;
        const unsigned bound = 64;
        while (passes < bound) {
            landSome(true);
            PassOutput out = harness.pass(frozen, "fuzz converge");
            for (const PaintRequest& request : out.paints)
                pending.push_back(std::make_pair(request.job, 1));
            landSome(true);
            harness.composite(*frozen.visible, 64, "fuzz converge composite");
            ++passes;
            if (!harness.model.wantsPass())
                break;
        }
        CHECK(!harness.model.wantsPass());
        CHECK_EQ(harness.model.visibleHoles(), 0u);
        EXPECT_NO_ARROWS(harness);
        CHECK(passes < bound);
    }
};

// A pure walk on the grid machine's graph: nothing but scale commits.
void scaleWalks()
{
    check::currentTest = "fuzz_scaleWalks";
    for (unsigned seed = 1; seed <= 40; ++seed) {
        std::mt19937 rng(seed);
        Harness harness;
        for (int i = 0; i < 10; ++i) {
            const float scale = g_scales[std::uniform_int_distribution<unsigned>(0, 3)(rng)];
            const IntSize bounds = boundsFor(scale, contentHeight);
            const IntRect view(clampInt(300, 0, std::max(0, bounds.width - screenWidth)),
                clampInt(1500, 0, std::max(0, bounds.height - screenHeight)), screenWidth, screenHeight);
            const int passes = static_cast<int>(std::uniform_int_distribution<unsigned>(0, 3)(rng));
            for (int p = 0; p <= passes; ++p)
                harness.step(input_(bounds, scale, view), 64, "scale walk");
            // I8 is checked inside checkInvariants; the machines must stay legal.
            EXPECT_NO_ARROWS(harness);
        }
    }
}

} // namespace

void runFuzz()
{
    for (unsigned seed = 1; seed <= 24; ++seed) {
        check::currentTest = "fuzz_seed_" + std::to_string(seed);
        Fuzzer fuzzer(seed);
        fuzzer.run(120);
        fuzzer.converge();
    }
    scaleWalks();
}

} // namespace tilegrid
