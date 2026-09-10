// Apotheosis: fake backends and the DrawRecorder for the TileGrid v2 core
// (docs/TILEGRID-DESIGN.md section 5.4).
//
// The core (TextureMapperTileGridCore) talks to two interfaces. Here they are
// implemented without pixels, without threads and without a clock: a replay
// "lands" after a configurable number of composites and yields a signature
// naming the store, grid, cell, scale and generation of the pixels it would
// have produced, and a texture is a handle with a size and a coverage rect.
//
// That is enough to assert on the PC what the device otherwise has to show:
//   * every visible pixel comes from exactly one source (I5, R8) - the
//     doubled-text / ghost class of bugs;
//   * the source of a pixel is the tile that owns it at the scale it is drawn
//     at (I6) - the "old tiles far from the pinch origin" class;
//   * while zoomed no visible cell is white (I15);
//   * a texture is never written outside the rect it was rasterised for (I4,
//     R6's paintedRect).
//
// The fake raster backend implements the replay job machine of section 2.2b
// with the same table the engine uses, so an illegal arrow in the *fake* (a
// completion for a cancelled job, a start after a failure) is observable
// instead of being a silently ignored race.

#pragma once

#include "TextureMapperTileGridCore.h"
#include "check.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tilegrid {

using namespace WebCore::TileGrid;

// ---------------------------------------------------------------------------
// Pixels, in name only
// ---------------------------------------------------------------------------

struct PixelSignature {
    bool valid { false };
    unsigned storeId { 0 };
    GridId grid { invalidGridId };
    CellIndex cell {};
    float scale { 1.0f };
    IntRect rect {};             // what the replay covered, in its grid's coordinates
    unsigned generation { 0 };   // bumped for every replay of the same cell
};

// A replay that never lands: the pool is wedged, the watchdog has not fired
// yet. R5 says the model must cope with it - no timeout, no escalation, just a
// cell that is not Ready.
constexpr unsigned kNeverLands = 0xffffffffu;

// ---------------------------------------------------------------------------
// FakeRasterBackend
// ---------------------------------------------------------------------------

class FakeRasterBackend final : public RasterBackend {
public:
    FakeRasterBackend(unsigned storeId, unsigned latency, unsigned workers = 4, unsigned inFlightCap = 6);
    ~FakeRasterBackend() override;

    JobHandle record(GridId, CellIndex, const IntRect&, float scale, bool sync) override;
    void cancel(JobHandle) override;
    void waitFor(const std::vector<JobHandle>&) override;
    std::vector<RasterEvent> poll() override;
    unsigned workerCount() const override;

    // One composite of worker time. Queued jobs are promoted into free worker
    // slots first, then every running job spends one unit.
    void tick(unsigned times = 1);

    unsigned clock() const;
    unsigned lastWaitTicks() const;    // scenario 15: ceil(n / workers) * latency
    unsigned recorded() const;
    unsigned queued() const;
    unsigned running() const;
    unsigned peakInFlight() const;
    unsigned finished() const;
    unsigned cancelled() const;

    // Scripted failures: the next `count` jobs recorded report ReplayFailed
    // (or Watchdog) instead of finishing.
    void failNextRecords(unsigned count);
    void watchdogNextRecords(unsigned count);
    // ... and the next `count` records are refused outright (invalidJobHandle):
    // no source layer, no buffer memory. R5 says the model must survive it.
    void refuseNextRecords(unsigned count);
    unsigned refusedRecords() const;

    // The order jobs were started in, for the priority assertions of I9.
    const std::vector<JobHandle>& startOrder() const;

    // I16 in the fake: the replay job machine refused an arrow.
    const std::vector<std::string>& illegalArrows() const;

    static PixelSignature signature(std::uint64_t);

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

// ---------------------------------------------------------------------------
// FakeTextureBackend
// ---------------------------------------------------------------------------

class FakeTextureBackend final : public TextureBackend {
public:
    // uploadBudget == 0 means unlimited.
    explicit FakeTextureBackend(unsigned uploadBudget);
    ~FakeTextureBackend() override;

    TextureHandle acquire(IntSize) override;
    bool upload(TextureHandle, const PixelBuffer&, const IntRect&) override;
    void release(TextureHandle) override;
    void beginComposite() override;
    unsigned uploadsRemainingThisComposite() const override;
    std::vector<TextureHandle> takeReclaimedTextures() override;

    PixelSignature signatureOf(TextureHandle) const;
    IntSize sizeOf(TextureHandle) const;
    IntRect coverageOf(TextureHandle) const;
    bool isLive(TextureHandle) const;

    unsigned liveCount() const;
    unsigned acquired() const;
    unsigned released() const;
    unsigned uploadsThisComposite() const;
    unsigned totalUploads() const;
    unsigned budget() const;

    // The next `count` acquires come up empty: the pool is exhausted. R4's
    // upload then cannot happen at all, which the model has to be told about.
    void failNextAcquires(unsigned count);

    // The next `count` uploads return false without touching the texture: the
    // backend could not perform them (no pixels, wrong job, out of bounds).
    // Device round 2: an upload that silently does nothing is the blank page.
    void failNextUploads(unsigned count);

    // Pool reclaim: the next drain reports these as TextureLost.
    void reclaim(TextureHandle);
    void reclaimOldestLive();

    // Double release, an upload into a released handle, an upload outside the
    // rect the texture was rasterised for. takeErrors() drains, so one error
    // does not fail every later composite as well.
    const std::vector<std::string>& errors() const;
    std::vector<std::string> takeErrors();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

// ---------------------------------------------------------------------------
// DrawRecorder - what the screen shows
// ---------------------------------------------------------------------------

struct ScreenReport {
    unsigned samples { 0 };
    unsigned fromPrimary { 0 };
    unsigned fromBackdrop { 0 };
    unsigned unpainted { 0 };      // white
    unsigned doubleDrawn { 0 };    // two sources for one pixel: I5 / R8
    unsigned foreign { 0 };        // a source that is not this cell at this scale: I6
    unsigned cells { 0 };
    unsigned unpaintedCells { 0 }; // cells with no source at all
    unsigned backdropCells { 0 };
};

class DrawRecorder {
public:
    explicit DrawRecorder(const FakeTextureBackend&, int step = 32);

    ScreenReport record(const CompositeResult&, const IntRect& visible, const TileGridModel&);

private:
    const FakeTextureBackend& m_textures;
    int m_step;
};

// ---------------------------------------------------------------------------
// The harness the core scenarios drive
// ---------------------------------------------------------------------------

struct CoreVariant {
    const char* name { "l1/b2" };
    unsigned latency { 1 };
    unsigned uploadBudget { 2 };   // 0 = unlimited
    unsigned workers { 4 };
};

// The eight combinations of section 5.4: worker latency {0, 1, 3, never} x
// upload budget {2, unlimited}.
const std::vector<CoreVariant>& coreVariants();

struct CoreHarness {
    CoreHarness(const CoreVariant&, const ModelConfig& = ModelConfig(), unsigned storeId = 1);

    CoreVariant variant;
    FakeRasterBackend raster;
    FakeTextureBackend textures;
    TileGridCore core;
    DrawRecorder recorder;
    std::vector<std::string> arrows;

    PassInput lastInput;
    ScreenReport screen;
    CompositeResult lastComposite;

    bool lands() const { return variant.latency != kNeverLands; }

    void apply(const PassInput&);
    PassOutput pass(const PassInput&, const char* where = "core pass");
    CompositeResult composite(const char* where = "core composite");
    // pass + composite, the frame the engine runs.
    CompositeResult frame(const PassInput&, const char* where = "core frame");
    // Repeat the same frame until the model comes to rest (R12) or the cap.
    unsigned settle(const PassInput&, unsigned maxRounds = 16, const char* where = "core settle");
};

// Screen-level I5 / I6 plus the backends' own error lists. Called after every
// composite; a scenario adds I15 and the hole counts on top.
void checkScreen(CoreHarness&, const char* where);

void expectNoBackendArrows(CoreHarness&, const char* file, int line);
#define EXPECT_NO_CORE_ARROWS(harness) ::tilegrid::expectNoBackendArrows((harness), __FILE__, __LINE__)

void runCoreScenarios();

} // namespace tilegrid
