// Apotheosis: implementation of the fake backends and the DrawRecorder
// (TILING-REWRITE-PLAN.md section 5.4). See fakes.h for what they are for.

#include "fakes.h"

#include "TextureMapperTileGridMachines.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

namespace tilegrid {

namespace {

// The signature registry. A PixelBuffer carries an index into this vector, so
// the fakes never have to pack five fields into 64 bits and the recorder can
// compare them exactly.
std::vector<PixelSignature> g_signatures;

std::uint64_t registerSignature(const PixelSignature& signature)
{
    g_signatures.push_back(signature);
    return static_cast<std::uint64_t>(g_signatures.size()); // 1-based; 0 = none
}

std::string describeArrow(const IllegalArrow& arrow)
{
    return std::string(machineKindName(arrow.machine)) + ":" + arrow.from + "->" + arrow.event;
}

bool sameScale(float a, float b)
{
    return std::fabs(a - b) < 1e-4f;
}

std::string rectText(const IntRect& rect)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%d,%d %dx%d", rect.x, rect.y, rect.width, rect.height);
    return std::string(buffer);
}

} // namespace

PixelSignature FakeRasterBackend::signature(std::uint64_t key)
{
    if (!key || key > g_signatures.size())
        return PixelSignature();
    return g_signatures[static_cast<size_t>(key - 1)];
}

// ---------------------------------------------------------------------------
// FakeRasterBackend
// ---------------------------------------------------------------------------

class FakeRasterBackend::Impl {
public:
    // Section 2.2b's replay job machine, the same table the engine uses. The
    // fake steps it on every transition it makes, so an illegal arrow inside
    // the *fake* (a completion for a cancelled job, a second start) shows up
    // as a test failure rather than as a silently ignored race.
    struct Job {
        JobHandle handle { invalidJobHandle };
        JobMachine machine;
        GridId grid { invalidGridId };
        CellIndex cell {};
        IntRect rect {};
        float scale { 1.0f };
        bool sync { false };
        unsigned remaining { 0 };
        bool fail { false };
        bool watchdog { false };
        bool reported { false };
    };

    Impl(unsigned storeId, unsigned latency, unsigned workers, unsigned inFlightCap)
        : storeId(storeId)
        , latency(latency)
        , workers(workers ? workers : 1)
        , inFlightCap(std::max(inFlightCap, workers ? workers : 1))
    {
    }

    template<typename Event>
    void stepJob(Job& job, const Event& event)
    {
        WebCore::TileGrid::step(job.machine, event, [&](const IllegalArrow& arrow) {
            arrows.push_back(describeArrow(arrow));
        });
    }

    unsigned runningCount() const
    {
        unsigned count = 0;
        for (const auto& entry : jobs) {
            if (entry.second.machine.state() == JobState::Running)
                ++count;
        }
        return count;
    }

    unsigned queuedCount() const
    {
        unsigned count = 0;
        for (const auto& entry : jobs) {
            if (entry.second.machine.state() == JobState::Queued)
                ++count;
        }
        return count;
    }

    // Promote queued jobs into free worker slots, oldest first (the core hands
    // them over in the model's priority order, so oldest-first is
    // priority-first within a pass).
    void promote()
    {
        const unsigned slots = std::min(workers, inFlightCap);
        unsigned busy = runningCount();
        for (auto& entry : jobs) {
            if (busy >= slots)
                break;
            Job& job = entry.second;
            if (job.machine.state() != JobState::Queued)
                continue;
            stepJob(job, ev::job::Started { });
            startOrder.push_back(job.handle);
            pending.push_back(RasterEvent { RasterEventKind::Started, job.handle, PixelBuffer() });
            ++busy;
        }
        peakInFlight = std::max(peakInFlight, busy);
    }

    void tick()
    {
        ++clock;
        promote();
        for (auto& entry : jobs) {
            Job& job = entry.second;
            if (job.machine.state() != JobState::Running)
                continue;
            if (job.remaining == kNeverLands)
                continue;
            if (job.remaining)
                --job.remaining;
            if (job.remaining)
                continue;
            complete(job);
        }
        harvest();
    }

    void complete(Job& job)
    {
        RasterEvent event;
        event.job = job.handle;
        if (job.watchdog) {
            stepJob(job, ev::job::Watchdog { });
            event.kind = RasterEventKind::Watchdog;
            ++failedJobs;
        } else if (job.fail) {
            stepJob(job, ev::job::Failed { });
            event.kind = RasterEventKind::Failed;
            ++failedJobs;
        } else {
            stepJob(job, ev::job::Finished { });
            event.kind = RasterEventKind::Finished;
            PixelSignature signature;
            signature.valid = true;
            signature.storeId = storeId;
            signature.grid = job.grid;
            signature.cell = job.cell;
            signature.scale = job.scale;
            signature.rect = job.rect;
            signature.generation = ++generations[std::make_pair(job.grid,
                std::make_pair(job.cell.column, job.cell.row))];
            event.pixels.rect = job.rect;
            event.pixels.scale = job.scale;
            event.pixels.signature = registerSignature(signature);
            ++finishedJobs;
        }
        job.reported = true;
        pending.push_back(event);
    }

    // Terminal jobs leave the table; a late event for one of them is exactly
    // the race R5 says the model must discard, and the core forgets the handle
    // when it reports it.
    void harvest()
    {
        for (auto it = jobs.begin(); it != jobs.end();) {
            const JobState state = it->second.machine.state();
            const bool terminal = state == JobState::Finished || state == JobState::Failed
                || state == JobState::Cancelled;
            it = (terminal && it->second.reported) ? jobs.erase(it) : std::next(it);
        }
    }

    unsigned storeId;
    unsigned latency;
    unsigned workers;
    unsigned inFlightCap;

    JobHandle nextHandle { 1 };
    std::map<JobHandle, Job> jobs;
    std::vector<RasterEvent> pending;
    std::vector<JobHandle> startOrder;
    std::vector<std::string> arrows;
    std::map<std::pair<GridId, std::pair<int, int>>, unsigned> generations;

    unsigned clock { 0 };
    unsigned lastWaitTicks { 0 };
    unsigned recorded { 0 };
    unsigned finishedJobs { 0 };
    unsigned cancelledJobs { 0 };
    unsigned failedJobs { 0 };
    unsigned peakInFlight { 0 };
    unsigned failNext { 0 };
    unsigned watchdogNext { 0 };
    unsigned refuseNext { 0 };
    unsigned refused { 0 };
};

FakeRasterBackend::FakeRasterBackend(unsigned storeId, unsigned latency, unsigned workers, unsigned inFlightCap)
    : m_impl(std::make_unique<Impl>(storeId, latency, workers, inFlightCap))
{
}

FakeRasterBackend::~FakeRasterBackend() = default;

JobHandle FakeRasterBackend::record(GridId grid, CellIndex cell, const IntRect& rect, float scale, bool sync)
{
    Impl& impl = *m_impl;
    ++impl.recorded;

    // Device round 1: the real backend refuses when it has no source layer, no
    // buffer memory - and, before the fix, whenever the global in-flight cap was
    // reached, which a pinch hits on every pass. R5 says the model must survive
    // it; before deviceRound1_refusedRequestIsMissingAgain() nothing here could
    // produce one, which is exactly why the device found it instead.
    if (impl.refuseNext) {
        --impl.refuseNext;
        ++impl.refused;
        return invalidJobHandle;
    }

    Impl::Job job;
    job.handle = impl.nextHandle++;
    job.grid = grid;
    job.cell = cell;
    job.rect = rect;
    job.scale = scale;
    job.sync = sync;
    job.remaining = impl.latency == kNeverLands ? kNeverLands : std::max(1u, impl.latency);
    if (impl.watchdogNext) {
        job.watchdog = true;
        --impl.watchdogNext;
    } else if (impl.failNext) {
        job.fail = true;
        --impl.failNext;
    }

    const JobHandle handle = job.handle;
    impl.jobs[handle] = job;

    // Latency 0: the replay is already done by the time the pass returns. It
    // still goes through the job machine, so the arrows are the same.
    if (impl.latency == 0) {
        Impl::Job& stored = impl.jobs[handle];
        impl.stepJob(stored, ev::job::Started { });
        impl.startOrder.push_back(handle);
        impl.pending.push_back(RasterEvent { RasterEventKind::Started, handle, PixelBuffer() });
        impl.complete(stored);
        impl.harvest();
    }
    return handle;
}

void FakeRasterBackend::cancel(JobHandle handle)
{
    Impl& impl = *m_impl;
    auto it = impl.jobs.find(handle);
    if (it == impl.jobs.end())
        return;
    impl.stepJob(it->second, ev::job::Cancelled { });
    ++impl.cancelledJobs;
    it->second.reported = true;
    impl.harvest();
}

// R3: all of them together, so n cells on w workers cost ceil(n / w) replay
// latencies. The bound is the fake's own; the core has no timeout (R5).
void FakeRasterBackend::waitFor(const std::vector<JobHandle>& handles)
{
    Impl& impl = *m_impl;
    const unsigned before = impl.clock;
    const unsigned bound = 32;
    unsigned spun = 0;
    while (spun < bound) {
        bool outstanding = false;
        for (JobHandle handle : handles) {
            auto it = impl.jobs.find(handle);
            if (it == impl.jobs.end())
                continue;
            const JobState state = it->second.machine.state();
            if (state == JobState::Queued || state == JobState::Running)
                outstanding = true;
        }
        if (!outstanding)
            break;
        impl.tick();
        ++spun;
    }
    impl.lastWaitTicks = impl.clock - before;
}

std::vector<RasterEvent> FakeRasterBackend::poll()
{
    std::vector<RasterEvent> events;
    events.swap(m_impl->pending);
    return events;
}

unsigned FakeRasterBackend::workerCount() const { return m_impl->workers; }
void FakeRasterBackend::tick(unsigned times) { for (unsigned i = 0; i < times; ++i) m_impl->tick(); }
unsigned FakeRasterBackend::clock() const { return m_impl->clock; }
unsigned FakeRasterBackend::lastWaitTicks() const { return m_impl->lastWaitTicks; }
unsigned FakeRasterBackend::recorded() const { return m_impl->recorded; }
unsigned FakeRasterBackend::queued() const { return m_impl->queuedCount(); }
unsigned FakeRasterBackend::running() const { return m_impl->runningCount(); }
unsigned FakeRasterBackend::peakInFlight() const { return m_impl->peakInFlight; }
unsigned FakeRasterBackend::finished() const { return m_impl->finishedJobs; }
unsigned FakeRasterBackend::cancelled() const { return m_impl->cancelledJobs; }
void FakeRasterBackend::failNextRecords(unsigned count) { m_impl->failNext = count; }
void FakeRasterBackend::watchdogNextRecords(unsigned count) { m_impl->watchdogNext = count; }
void FakeRasterBackend::refuseNextRecords(unsigned count) { m_impl->refuseNext = count; }
unsigned FakeRasterBackend::refusedRecords() const { return m_impl->refused; }
const std::vector<JobHandle>& FakeRasterBackend::startOrder() const { return m_impl->startOrder; }
const std::vector<std::string>& FakeRasterBackend::illegalArrows() const { return m_impl->arrows; }

// ---------------------------------------------------------------------------
// FakeTextureBackend
// ---------------------------------------------------------------------------

class FakeTextureBackend::Impl {
public:
    struct Texture {
        IntSize size;
        IntRect coverage;          // the rect this texture's pixels stand for
        PixelSignature signature;
        unsigned uploads { 0 };
        bool live { true };
    };

    explicit Impl(unsigned budget)
        : budget(budget)
    {
    }

    void error(const std::string& text) { errors.push_back(text); }

    unsigned budget;
    unsigned used { 0 };
    unsigned uploadsThisComposite { 0 };
    unsigned totalUploads { 0 };
    unsigned acquired { 0 };
    unsigned released { 0 };
    unsigned failAcquireNext { 0 };
    unsigned failUploadNext { 0 };
    TextureHandle nextHandle { 1 };
    std::map<TextureHandle, Texture> textures;
    std::vector<TextureHandle> reclaimed;
    std::vector<std::string> errors;
};

FakeTextureBackend::FakeTextureBackend(unsigned uploadBudget)
    : m_impl(std::make_unique<Impl>(uploadBudget))
{
}

FakeTextureBackend::~FakeTextureBackend() = default;

TextureHandle FakeTextureBackend::acquire(IntSize size)
{
    Impl& impl = *m_impl;
    if (size.isEmpty()) {
        impl.error("acquire of an empty texture");
        return invalidTextureHandle;
    }
    // Device round 1: the pool can come up empty (a 32-bit App Container with a
    // bounded texture pool). R4 has to hear about it, because the model has
    // already moved the tile to Ready by the time the core asks for a handle.
    if (impl.failAcquireNext) {
        --impl.failAcquireNext;
        return invalidTextureHandle;
    }
    const TextureHandle handle = impl.nextHandle++;
    Impl::Texture texture;
    texture.size = size;
    impl.textures[handle] = texture;
    ++impl.acquired;
    return handle;
}

bool FakeTextureBackend::upload(TextureHandle handle, const PixelBuffer& pixels, const IntRect& rect)
{
    Impl& impl = *m_impl;
    if (impl.failUploadNext) {
        --impl.failUploadNext;
        return false;
    }
    auto it = impl.textures.find(handle);
    if (it == impl.textures.end()) {
        impl.error("upload into an unknown texture");
        return false;
    }
    Impl::Texture& texture = it->second;
    if (!texture.live) {
        impl.error("upload into a released texture");
        return false;
    }

    const PixelSignature signature = FakeRasterBackend::signature(pixels.signature);
    if (!signature.valid) {
        impl.error("upload of pixels without a signature");
        return false;
    }

    if (rect.size() == texture.size) {
        // A full write: the texture now stands for this rect.
        texture.coverage = rect;
    } else if (texture.coverage.isEmpty()) {
        impl.error("partial upload into a texture that was never fully painted, rect " + rectText(rect));
        return false;
    } else if (!texture.coverage.contains(rect)) {
        // I4/R6: a texture is never written outside the rect it was
        // rasterised for.
        impl.error("upload " + rectText(rect) + " outside the texture's rect " + rectText(texture.coverage));
        return false;
    }

    if (!signature.rect.contains(rect))
        impl.error("upload " + rectText(rect) + " outside what the replay produced " + rectText(signature.rect));

    texture.signature = signature;
    ++texture.uploads;
    ++impl.uploadsThisComposite;
    ++impl.totalUploads;
    if (impl.budget && impl.used < impl.budget)
        ++impl.used;
    return true;
}

void FakeTextureBackend::release(TextureHandle handle)
{
    Impl& impl = *m_impl;
    auto it = impl.textures.find(handle);
    if (it == impl.textures.end()) {
        impl.error("release of an unknown texture");
        return;
    }
    if (!it->second.live) {
        impl.error("double release");
        return;
    }
    it->second.live = false;
    ++impl.released;
}

void FakeTextureBackend::beginComposite()
{
    m_impl->used = 0;
    m_impl->uploadsThisComposite = 0;
}

unsigned FakeTextureBackend::uploadsRemainingThisComposite() const
{
    const Impl& impl = *m_impl;
    if (!impl.budget)
        return 0xffffffffu;
    return impl.used >= impl.budget ? 0 : impl.budget - impl.used;
}

std::vector<TextureHandle> FakeTextureBackend::takeReclaimedTextures()
{
    std::vector<TextureHandle> taken;
    taken.swap(m_impl->reclaimed);
    for (TextureHandle handle : taken) {
        auto it = m_impl->textures.find(handle);
        if (it != m_impl->textures.end())
            it->second.live = false;
    }
    return taken;
}

PixelSignature FakeTextureBackend::signatureOf(TextureHandle handle) const
{
    auto it = m_impl->textures.find(handle);
    return it == m_impl->textures.end() ? PixelSignature() : it->second.signature;
}

IntSize FakeTextureBackend::sizeOf(TextureHandle handle) const
{
    auto it = m_impl->textures.find(handle);
    return it == m_impl->textures.end() ? IntSize() : it->second.size;
}

IntRect FakeTextureBackend::coverageOf(TextureHandle handle) const
{
    auto it = m_impl->textures.find(handle);
    return it == m_impl->textures.end() ? IntRect() : it->second.coverage;
}

bool FakeTextureBackend::isLive(TextureHandle handle) const
{
    auto it = m_impl->textures.find(handle);
    return it != m_impl->textures.end() && it->second.live;
}

unsigned FakeTextureBackend::liveCount() const
{
    unsigned count = 0;
    for (const auto& entry : m_impl->textures) {
        if (entry.second.live)
            ++count;
    }
    return count;
}

unsigned FakeTextureBackend::acquired() const { return m_impl->acquired; }
unsigned FakeTextureBackend::released() const { return m_impl->released; }
unsigned FakeTextureBackend::uploadsThisComposite() const { return m_impl->uploadsThisComposite; }
unsigned FakeTextureBackend::totalUploads() const { return m_impl->totalUploads; }
unsigned FakeTextureBackend::budget() const { return m_impl->budget; }
const std::vector<std::string>& FakeTextureBackend::errors() const { return m_impl->errors; }

std::vector<std::string> FakeTextureBackend::takeErrors()
{
    std::vector<std::string> taken;
    taken.swap(m_impl->errors);
    return taken;
}

void FakeTextureBackend::failNextAcquires(unsigned count) { m_impl->failAcquireNext = count; }
void FakeTextureBackend::failNextUploads(unsigned count) { m_impl->failUploadNext = count; }

void FakeTextureBackend::reclaim(TextureHandle handle)
{
    if (isLive(handle))
        m_impl->reclaimed.push_back(handle);
}

void FakeTextureBackend::reclaimOldestLive()
{
    for (const auto& entry : m_impl->textures) {
        if (entry.second.live) {
            m_impl->reclaimed.push_back(entry.first);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// DrawRecorder
// ---------------------------------------------------------------------------

DrawRecorder::DrawRecorder(const FakeTextureBackend& textures, int step)
    : m_textures(textures)
    , m_step(step > 0 ? step : 32)
{
}

ScreenReport DrawRecorder::record(const CompositeResult& composite, const IntRect& visible, const TileGridModel& model)
{
    ScreenReport report;
    const auto primary = model.primaryGrid();
    if (!primary || visible.isEmpty())
        return report;

    const std::vector<CellIndex> cells = model.cellsOf(visible, primary->bounds);
    report.cells = static_cast<unsigned>(cells.size());

    // An image store's "visible rect" is the whole image; keep the sample count
    // bounded so the suite stays under a couple of minutes.
    const int step = std::max(m_step, std::max(visible.width, visible.height) / 40);

    // Resolve the clip stack once: a tile draw is visible exactly inside
    // (its target rect) n (the clip it sits in), so the per-sample loop is a
    // plain point-in-rect test and the clip semantics are asserted here.
    struct Item {
        IntRect area;
        const DrawCommand* command { nullptr };
    };
    std::vector<Item> items;
    items.reserve(composite.commands.size());
    bool clipped = false;
    IntRect clip;
    for (const DrawCommand& command : composite.commands) {
        switch (command.op) {
        case DrawOp::BeginClip:
            clipped = true;
            clip = command.rect;
            continue;
        case DrawOp::EndClip:
            clipped = false;
            continue;
        case DrawOp::DrawTile:
            break;
        }
        Item item;
        item.area = clipped ? command.rect.intersection(clip) : command.rect;
        item.command = &command;
        if (!item.area.isEmpty())
            items.push_back(item);
    }

    // cell -> (samples, painted)
    std::map<std::pair<int, int>, std::pair<unsigned, unsigned>> perCell;
    std::set<std::pair<int, int>> backdropCells;
    const IntRect boundsRect(0, 0, primary->bounds.width, primary->bounds.height);
    const int edge = model.config().tileEdge;

    for (int y = visible.y; y < visible.maxY(); y += step) {
        for (int x = visible.x; x < visible.maxX(); x += step) {
            const IntPoint point(x, y);
            if (!boundsRect.containsPoint(point))
                continue;
            ++report.samples;
            auto& cellCounts = perCell[std::make_pair(x / edge, y / edge)];
            ++cellCounts.first;

            unsigned sources = 0;
            const DrawCommand* winner = nullptr;
            for (const Item& item : items) {
                if (!item.area.containsPoint(point))
                    continue;
                ++sources;
                winner = item.command;
            }

            if (!sources) {
                ++report.unpainted;
                continue;
            }
            ++cellCounts.second;
            if (sources > 1)
                ++report.doubleDrawn;

            if (winner->backdrop) {
                ++report.fromBackdrop;
                backdropCells.insert(std::make_pair(x / edge, y / edge));
            } else
                ++report.fromPrimary;

            // I6: the pixels under this point were rasterised for exactly this
            // cell of this grid at this scale. This is the assertion the
            // "old tiles far from the pinch origin" and "doubled text" reports
            // never had.
            const PixelSignature signature = m_textures.signatureOf(winner->texture);
            const bool foreign = !signature.valid
                || signature.grid != winner->grid
                || !(signature.cell == winner->cell)
                || !sameScale(signature.scale, winner->scale);
            if (foreign)
                ++report.foreign;
        }
    }

    for (const auto& entry : perCell) {
        if (entry.second.first && !entry.second.second)
            ++report.unpaintedCells;
    }
    report.backdropCells = static_cast<unsigned>(backdropCells.size());
    return report;
}

// ---------------------------------------------------------------------------
// CoreHarness
// ---------------------------------------------------------------------------

const std::vector<CoreVariant>& coreVariants()
{
    static const std::vector<CoreVariant> variants = {
        { "l0/b2", 0, 2, 4 },
        { "l0/binf", 0, 0, 4 },
        { "l1/b2", 1, 2, 4 },
        { "l1/binf", 1, 0, 4 },
        { "l3/b2", 3, 2, 4 },
        { "l3/binf", 3, 0, 4 },
        { "lnever/b2", kNeverLands, 2, 4 },
        { "lnever/binf", kNeverLands, 0, 4 },
    };
    return variants;
}

namespace {

CoreConfig coreConfigFor(const CoreVariant& variant, const ModelConfig& model)
{
    CoreConfig config;
    config.model = model;
    config.maxUploadsPerComposite = variant.uploadBudget ? variant.uploadBudget : 0xffffffffu;
    return config;
}

} // namespace

CoreHarness::CoreHarness(const CoreVariant& variant, const ModelConfig& model, unsigned storeId)
    : variant(variant)
    , raster(storeId, variant.latency, variant.workers, variant.workers + 2)
    , textures(variant.uploadBudget)
    , core(storeId, raster, textures, coreConfigFor(variant, model))
    , recorder(textures)
{
    core.setIllegalArrowObserver([this](const IllegalArrow& arrow) {
        arrows.push_back(describeArrow(arrow));
    });
    resetReleaseLedger(storeId);
}

void CoreHarness::apply(const PassInput& in)
{
    lastInput = in;
    core.setBounds(in.boundsScaled);
    core.setScale(in.scale);
    core.setVisibleRect(in.visible);
    core.setTileBudget(in.tileBudget);
    core.setImageStore(in.isImage);
    core.setThreadedRaster(in.threadedRaster);
    core.setPanGesture(in.panGesture);
    if (!in.dirty.isEmpty())
        core.invalidate(in.dirty);
}

PassOutput CoreHarness::pass(const PassInput& in, const char* where)
{
    apply(in);
    PassOutput out = core.runPass();
    checkInvariants(core.model(), where);
    checkPassOutput(core.model(), out, where);
    return out;
}

CompositeResult CoreHarness::composite(const char* where)
{
    raster.tick();
    lastComposite = core.composite();
    checkInvariants(core.model(), where);
    screen = recorder.record(lastComposite, core.compositeVisibleRect(), core.model());
    checkScreen(*this, where);
    return lastComposite;
}

CompositeResult CoreHarness::frame(const PassInput& in, const char* where)
{
    pass(in, where);
    return composite(where);
}

unsigned CoreHarness::settle(const PassInput& in, unsigned maxRounds, const char* where)
{
    unsigned rounds = 0;
    while (rounds < maxRounds) {
        frame(in, where);
        ++rounds;
        if (!core.wantsPass())
            break;
    }
    return rounds;
}

void checkScreen(CoreHarness& harness, const char* where)
{
    const ScreenReport& screen = harness.screen;
    // I5 / R8: every visible pixel comes from exactly one source. Two sources
    // for one pixel is the doubled text and the ghost band.
    if (screen.doubleDrawn) {
        ::check::fail(__FILE__, __LINE__, "I5: a visible pixel had two sources",
            std::string(where) + " [" + harness.variant.name + "] doubleDrawn="
            + std::to_string(screen.doubleDrawn));
    } else
        ++::check::checks;

    // I6: the pixels a cell draws were rasterised for that cell at that scale.
    if (screen.foreign) {
        ::check::fail(__FILE__, __LINE__, "I6: a cell drew foreign pixels",
            std::string(where) + " [" + harness.variant.name + "] foreign="
            + std::to_string(screen.foreign));
    } else
        ++::check::checks;

    // I5's other half, and I15's screen form: when the model says there is no
    // hole, the screen must actually show something everywhere - a texture
    // handle the core lost, a clip that does not cover its cell or a backdrop
    // tile drawn at the wrong ratio would all show up here and nowhere else.
    // While the page is zoomed this is exactly I15, "blurry, never white".
    if (!harness.lastComposite.visibleHoles) {
        if (screen.unpaintedCells) {
            ::check::fail(__FILE__, __LINE__, "I15: a visible cell was white although the model reports no hole",
                std::string(where) + " [" + harness.variant.name + "] unpaintedCells="
                + std::to_string(screen.unpaintedCells));
        } else
            ++::check::checks;
    }

    // The core holds exactly one pixel buffer per tile whose replay landed and
    // has not been uploaded yet - a buffer of a removed tile or a dropped grid
    // would be a leak and, worse, pixels that could still reach a texture.
    unsigned landed = 0;
    for (const TileInfo& tile : harness.core.model().tiles()) {
        if (tile.state == TileState::Landed)
            ++landed;
        if (tile.patch == PatchState::Landed)
            ++landed;
    }
    CHECK_LE(harness.core.pendingPixelBuffers(), landed);

    for (const std::string& error : harness.textures.takeErrors()) {
        ++::check::checks;
        ::check::fail(__FILE__, __LINE__, "texture backend error", std::string(where) + ": " + error);
    }
}

void expectNoBackendArrows(CoreHarness& harness, const char* file, int line)
{
    for (const std::string& arrow : harness.arrows) {
        ++::check::checks;
        ::check::fail(file, line, "unexpected illegal arrow (model)", arrow);
    }
    harness.arrows.clear();
    for (const std::string& arrow : harness.raster.illegalArrows()) {
        ++::check::checks;
        ::check::fail(file, line, "unexpected illegal arrow (job machine)", arrow);
    }
}

} // namespace tilegrid
