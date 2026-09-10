# Tile management (TileGrid v2) – design

> This is the design document of TileGrid v2, the tile bookkeeping that lives in
> `Source/WebCore/platform/graphics/texmap/TextureMapperTileGrid*` of the WinUWP
> patch set; those sources and the host tests in `tests/tilegrid/` cite its
> section numbers, so the numbering here is fixed. Sections 6 (work packages)
> and 7 (device test list) of the working plan are omitted: they were the
> schedule, not the design.

## 0. Summary

`TextureMapperTiledBackingStore` was patched some twenty times before this
rewrite. Every fix was real and every fix was insufficient, because the class
has no single model of "which tiles should exist and what each of them is
allowed to draw". It has a cover rect, a keep rect, a stale-tile stash, a
progressive cover, a lattice backstop, four per-tile flags, three traces, and a
driver-side repair loop that force-dirties the whole layer tree when the store's
answers do not add up. These mechanisms fight each other; the device shows the
fights as holes, old tiles far from the pinch origin, and one-to-two-second
stalls.

The rewrite replaces the tile bookkeeping with a **pure, host-testable grid
model** and a thin WebCore store around it. The model is modelled on WebKit's
Cocoa `TileController`/`TileGrid` (index-addressed lattice, one primary grid,
one frozen previous-scale grid, cohort-style retention), reduced to what this
port needs. It is:

- **derived, not accumulated** – every pass computes the desired tile set from
  (layer bounds, scale, visible rect, budget) and reconciles the grid to it;
  nothing survives by accident;
- **index-addressed** – a tile is `(column, row)` on the lattice anchored at the
  layer origin; rects are derived, never compared;
- **explicit about scale changes** – the scale-1 grid stays alive as a
  low-resolution backdrop for as long as the page is zoomed (Cocoa's
  zoomed-out grid), any other old grid is a frozen transient backdrop; both are
  drawn only through the cells the new grid has not painted yet and dropped
  wholesale, never tile by tile;
- **free of timeouts** – no grace counters, no unpainted budgets; a request is
  served in priority order or fails explicitly;
- **explicit about what may be drawn** – a tile draws either nothing or pixels
  rasterised for exactly its index and scale; there is no "old pixels are better
  than a hole" case, that job belongs to the backdrop;
- **tested on the PC** – invariants and the failing device scenarios run as
  plain C++ tests, plus a replay of device traces.

It was implemented and brought up in five steps against the invariants of §4,
carried through its device rounds by a runtime switch that is gone again (§5.2).

## 1. What is wrong today (root causes, not symptoms)

Read with `TextureMapperTiledBackingStore.{h,cpp}` (2308 lines, ~1900 of them
this port's) and `TextureMapperTile.{h,cpp}` open.

1. **Tiles are identified by rect, not by index.** `createOrDestroyTilesIfNeeded`
   lays tiles from `wkCoverRect.x()/y()` and matches old tiles by `oldTile ==
   newTile`. Every clamp, intersection with an unsnapped rect, or float rounding
   moves the origin and produces a second, overlapping grid (the lattice
   snapping fixed two of these paths; the trace shows more remain). With indices
   there is no such state.
2. **Two authorities for "should this tile exist".** The cover rect says what to
   create, the keep rect (`TileEraseThreshold`, `wkKeepRect`, lattice backstop)
   says what to keep, the progressive cover adds more, `wkClampRectToTileBudget`
   shrinks both, `wkDropStaleTilesOverBudget` steals from the placeholders. No
   single place can answer "is this tile supposed to be here".
3. **Per-tile state is a bag of flags** (`m_wkNeedsFullPaint`,
   `m_wkHideUntilPainted`, `m_wkPendingFirstPaint`, `m_wkForceSyncPaint`,
   pending buffer, deferred passes, unpainted composites, foreign texture).
   Whether a tile is drawn is decided by `wkDrawsNothing()` from four of them;
   whether it is repainted by three others. Combinations nobody intended are
   reachable, e.g. a tile painted into a wrong-sized texture that owes nothing,
   or a hidden tile with no placeholder behind it (the sticky-header flicker).
4. **Placeholders are tile-granular and released by coverage arithmetic**
   (`wkPaintedRect`, `wkUncoveredParts`). The stash is capped at 8 tiles, aged by
   two counters, released per tile – so a zoomed pan away from the pinch origin
   runs out of placeholders precisely where new tiles are still being rastered.
5. **The unpainted budget is 1 composite when zoomed.** With threaded raster,
   every freshly posted tile reports itself unpainted on the next composite,
   which arms the driver's repair, which escalates to `forceDirtyTree()`, which
   posts hundreds of paints, which are unpainted on the next composite. The cap
   on that escalation did not fire in the last v1 package, so at least one more
   path leads to the whole-tree dirty (code-hosting-site rows with
   `dirty_full=54`, 1.7–2.5 s backing).
6. **Traces measure the store's own beliefs.** `cover` counted cells the store
   thought it had; it never produced a line while holes were on screen. A
   trace that records *inputs* and can be replayed on the PC is what was missing.

## 2. The model

### 2.1 Coordinates and vocabulary

- **Layer bounds** `B` – `(0, 0, w·s, h·s)` in *scaled layer coordinates*, `s`
  = contents scale (page scale × device scale). Image stores: `s = 1`.
- **Visible rect** `V` – the part of `B` the compositor will show, same
  coordinates, already mapped by `GraphicsLayerTextureMapper::wkVisibleRectForChild`
  (unchanged). `V` may be *unknown* (masks, replicas, 3D subtrees, image
  stores); then the model tiles all of `B` within the budget.
- **Tile size** `T` – 1024 (or `maxTextureSize` if smaller). Fixed per store.
- **Lattice** – cell `(c, r)` has rect `(c·T, r·T, T, T) ∩ B`. The lattice is
  anchored at `(0, 0)` of `B`. **Always.** There is no other origin.
- **Cell rect** vs **tile** – a cell is a lattice position; a tile is a cell
  that currently has a record in the grid.
- **Grid** – `map<CellIndex, Tile>` for one scale.
- **Backdrop** – a second grid drawn underneath the primary one where the
  primary has no `Ready` tile. Either *persistent* (the scale-1 grid, kept and
  maintained while the page is zoomed) or *transient* (the previous scale's
  grid, frozen). At most one alive at a time (R7).

### 2.2 Tile states (exactly these)

```
Missing    no usable pixels; draws nothing
Rastering  a replay is in flight (worker) for the full cell; draws nothing
Landed     replay finished, pixels in CPU buffer, not uploaded; draws nothing
Ready      texture holds pixels rasterised for this cell at this grid's scale;
           draws the texture
```

Plus one orthogonal bit on `Ready`: `dirtyRect` (non-empty ⇒ the next pass
repaints that part; the tile keeps drawing meanwhile – it is the *current*
content with a small stale patch, which is what the dirty-rect path has always
meant and what sticky headers need).

Transitions:

```
(new)               -> Missing
Missing  --Requested-->      Rastering       (a replay job exists; Sync = the pass waits for it)
Rastering --ReplayFinished--> Landed
Rastering --ReplayFailed-->  Missing         (re-requested by the next pass)
Landed   --Uploaded-->       Ready
Rastering/Landed --Cancelled--> Missing      (cell left the desired set, or scale changed)
Ready    --TextureLost-->    Missing         (pool reclaim)
any      --Removed-->        (gone; job cancelled, texture back to the pool)
```

Dirty rects do not change this state. They live in a **patch sub-machine**
that only a `Ready` tile has:

```
Patch: None --Dirtied--> Pending --Requested--> InFlight --ReplayFinished--> Landed
       --Uploaded--> None;   InFlight/Landed --Dirtied--> (same state, rect accumulates)
       --ReplayFailed--> Pending;   any --Cancelled/Removed--> None
```

A `Ready` tile keeps drawing its texture throughout; at most one patch job is
in flight per tile; a dirty rect that arrives while one is in flight is
accumulated and becomes the next job when the current one lands (R10). Sync
patches (small stores, switch off) go `Pending → InFlight → Landed → None`
within one pass.

**Enforced, not documented.** The model has exactly one function
`transition(Tile&, Event)` backed by a table of the arrows above (tile and
patch machine). The store never assigns a state; it reports events
(`Requested`, `ReplayFinished`, `ReplayFailed`, `Uploaded`, `TextureLost`,
`Cancelled`, `Dirtied`, `Removed`). An arrow that is not in the table is a test
failure (`CHECK`) and, in the engine, one log line plus a reset to `Missing`.
The fuzz test asserts that no illegal transition is ever attempted, so "a
combination nobody intended" is unreachable by construction.

### 2.2b The other state machines

Everything in the tiling that has a lifecycle gets the same treatment: one
enum, one transition table, one `transition()` function, illegal arrows are
test failures. Four more, in decreasing order of what they buy:

**Grid machine** (one per grid; the scale-change rules R7/R9 are its table):

```
Primary --ScaleLeaves1--> PersistentBackdrop      (scale was 1)
Primary --ScaleChanged--> TransientBackdrop        (no persistent backdrop alive)
Primary --ScaleChanged--> Dropped                  (a persistent backdrop is alive)
PersistentBackdrop --ScaleReturnsTo1--> Primary
TransientBackdrop --Covered(K) | Aged(N) | BoundsChanged | ScaleChanged--> Dropped
```

R7 and R9 stop being prose: "at most one backdrop" (I8) is the assertion that
never two grids are in a Backdrop state, and every path a pinch can take (1 →
2.85 → 6 → 1 → 2, a second pinch before the first settles) is a walk on this
graph the fuzz test takes at random.

**Replay job machine** (one per raster request; the only object that crosses
threads):

```
Queued --Started--> Running --Finished--> Finished
Queued --Cancelled--> Cancelled
Running --Cancelled--> Cancelled            (result discarded on completion)
Running --Failed | Watchdog--> Failed
```

Owned by the raster backend. The worker touches only *its* job; completion
is posted to an **event inbox** that the core drains at exactly two points –
the start of a pass and the start of a composite – and turns into tile events
in job order. No tile state changes outside those two points, so the model is
deterministic on the engine thread whatever the workers do, and the fakes
implement the same machine with scripted latencies.

**Store phase machine** (one per store; enforces the call order the store
depends on):

```
Idle --SetScale--> Idle   --SetVisible--> Idle   --BeginPass--> InPass
InPass --EndPass--> Idle
Idle --BeginComposite--> InComposite --EndComposite--> Idle
```

with `Upload` legal only in `InComposite` (GL current), `Requested` legal only
in `InPass`, `SetScale`/`SetVisible` illegal in either. This is where a scale
that flip-flops between passes, a composite that re-enters a pass, or an
upload from the wrong place shows up as an assertion instead of a ghost.

**Library.** All five machines are written with **Boost.SML**
(`boost-ext/sml`, single header `sml.hpp`, Boost Software License, C++14):
transition tables as a constexpr DSL (`"Missing"_s + event<Requested> [guard]
/ action = "Rastering"_s`), compile-time dispatch, no allocation, no RTTI, no
exceptions – WebKit builds with both off. SML reports an unhandled event
through its `unexpected_event` hook, which is the "illegal arrow" of I16 for
free; guards carry the K/N/persistent-alive conditions of R7/R9 so the tables
stay declarative. A per-tile instance is a few bytes of state index. Vendored
as `Source/ThirdParty/sml/sml.hpp` + LICENSE (the WebKit way), included only
from the two new `.cpp` files so it cannot leak into the rest of the build.
Verified with MSVC x64 on the PC and with clang-cl for ARM32 on the device;
clang and MSVC are in SML's own CI, ARM32 is not, but the header is
platform-free standard C++. Fallback, only if an ARM32 build ever refuses it:
a hand-written table with the same `process(event)` interface behind the same
tests – the tests do not know which is underneath.

**Driver repair machine** (two states, replaces the escalation ledger):

```
Settled --holes>0 after paint--> PresentOwed --composite ran--> Settled
```

`PresentOwed` requests one composite. Nothing else. There is no third state.

There is no "recycled tile keeps old pixels" state. A texture that is reused
for another cell is attached to a `Missing` tile and stays invisible until
painted. Sticky-header flicker (the reason v1 kept drawing old pixels) is
handled by rule R6 below, not by drawing wrong pixels.

### 2.3 Inputs of one pass (all of them)

```
struct PassInput {
    IntSize   boundsScaled;      // B
    float     scale;             // s
    optional<IntRect> visible;   // V, none = unknown
    IntRect   dirty;             // dirty rect in scaled coords, may be empty
    unsigned  tileBudget;        // max tiles in the primary grid (24 today)
    bool      isImage;           // image store: no scale changes, no backdrop
    bool      threadedRaster;    // runtime switch
    bool      panGesture;        // driver: finger on the glass
};
```

Everything else the model needs it keeps itself: the previous `V` (for the pan
direction), composite counters, the backdrop.

### 2.4 Rules (the whole algorithm)

**R1 – desired set.** `cover = V` inflated by the margin `M` (§2.6), snapped
outwards to the lattice, intersected with `B`. `desired = cells(cover)`. If
`|desired| > budget`: order cells by (distance from `V`'s centre, then against
the pan direction) and keep the first `budget`. If `V` is unknown:
`desired = cells(B)` truncated the same way from the top-left. Cells of `V`
itself always come first, so `V` is fully covered whenever `budget ≥ |cells(V)|`
(true on the device for every scale up to 6: V is at most 3×5 cells).

**R2 – reconcile.** For each cell in `desired` without a tile: add `Missing`.
For each tile not in `desired`: remove (cancel replay, texture to pool). No
hysteresis, no keep rect. Retention against scroll jitter is achieved by the
margin, not by keeping stragglers: a tile just outside `cover` is by definition
more than `M` away from the viewport.

**R3 – paint requests.** After reconciling, every `Missing` tile in the grid
yields a paint request for its whole cell; every `Ready(dirty)` tile yields one
for `dirty ∩ cell`. Mode:

- `Sync` if `threadedRaster` is off, **or** the store is small (§2.6), **or**
  the cell intersects `V` and the scale changed this pass (`syncOnScaleCommit`,
  a constant, so the device can A/B it: one frame of backdrop instead of ~150 ms
  once per pinch);
- `Async` otherwise – including visible cells while zoomed and visible cells a
  fling brings in. What is on screen meanwhile is the backdrop (R7–R9) or, at
  scale 1 beyond the margin, the layer background for one to two composites.
  There is no timeout that turns an Async request into a Sync one (R5).

Every request, Sync or Async, becomes a replay job in the priority queue of
R5 – there is exactly one raster path. `Sync` means the pass **waits** for the
jobs it marked Sync (all of them together, so the visible cells of a scale
commit replay in parallel on every worker: 6 cells on 4 workers cost two
replay times, not six) and uploads them before it ends. With `threadedRaster`
off the worker count is 0 and the queue is replayed inline on the engine
thread in priority order; nothing else changes.

**R4 – uploads.** Per composite the store may upload `Landed` tiles within the
upload budget (2 / 12 ms today). Tiles whose cell intersects `V` are exempt
from the budget: a visible hole is never traded for frame time. The remainder
waits in the same priority order as R5 (distance from `V`). Nothing ages,
nothing becomes "overdue": a `Landed` tile outside `V` is either uploaded by a
later composite or removed by R2. The store may stop a composite's uploads
early on wall-clock time (12 ms today); the model only sees "how many".

**R5 – priority and failure, no timeouts.** The raster queue is a priority
queue, not FIFO: cells intersecting `V` first, then by distance from `V`, pan
side first; the persistent backdrop's requests (R7) last. When a cell leaves
the desired set its replay is cancelled, so visible work is never queued behind
work nobody will look at. A `Rastering` tile leaves that state only through
`ReplayFinished`, `ReplayFailed` or `Cancelled`; the worker pool's existing
10 s watchdog reports `ReplayFailed`, which means `Missing` and a new request
on the next pass. No composite counter, no "unpainted budget", nothing that
escalates to the engine thread or to other tiles. The in-flight cap of the
pool bounds memory; overflow waits in the queue in priority order instead of
falling back to a synchronous paint.

Parallelism: `workers = clamp(cores − 2, 2, 4)` (the Snapdragon 808 has six
cores; v1's pool had two), in-flight cap = `workers + 2` (each running job
holds one 4 MB tile buffer). Tiles are independent, so replays run fully in
parallel; the only serial parts are the record on the engine thread (a
display list, cheap) and the GL upload (R4). The queue is re-prioritised at
the start of every pass for jobs still `Queued`; `Running` jobs are never
pre-empted, only cancelled if their cell left the desired set. Cairo's thread
safety for exactly this pattern was verified before the worker pool was built.

This replaces `wkUnpaintedCompositeBudget()` (4 / 1) and the driver's
escalation. Why the old budget of one composite fed on itself: a first paint
through the pool needs two to three composites (record on the engine thread,
replay on a worker, upload on the next composite), so every new tile reported
itself unpainted, the driver repaired, the repair escalated. Why not simply
Sync for visible tiles: at scale 1 synchronous raster on a scroll tick is the
25 ms vs 2.9 ms of the threaded-raster A/B, i.e. the whole reason the pool
exists; and when zoomed the persistent backdrop makes an Async first paint
show blurry content instead of white, which is what the eye accepts.

**R6 – bounds change without scale change** (a resized layer, e.g. a sticky
header, or a page that grew). Cells that still lie within the new `B` keep
their tiles and their state – a `Ready` tile's content is still the content of
that cell; the layer tells us what changed through the dirty rect, as it does
for every other repaint. Cells outside the new `B` are removed. A
right/bottom edge cell whose rect changed size (a partial cell) stays `Ready`
and keeps drawing its texture **at the rect it was rasterised for**
(`paintedRect`, tracked per tile; I4 is "texture size == paintedRect size"),
and receives a whole-cell patch; when that lands, the texture and
`paintedRect` are replaced. A texture is never stretched over a rect it was
not rasterised for. This is what makes a header that resizes every frame
stable: its one cell keeps drawing, and for a small store the patch is Sync,
so the new size is on screen in the same pass. (The first cut updated the
texture size at the bounds change, which bent I4.)

**R7 – scale change.** Two kinds of backdrop, at most one alive at a time:

- **Persistent (the zoomed-out grid, Cocoa's `zoomedOutTileGrid`).** When the
  scale leaves 1, the scale-1 primary grid becomes the persistent backdrop *as
  it is* (`Rastering`/`Landed` tiles cancelled and dropped, `Ready` tiles kept).
  Its `B` is always the primary's `B` divided by the scale, so a layer that
  grows while zoomed (a page still loading) is handled by R6 on the backdrop
  too, not by dropping it. It stays alive for as long as `scale != 1` and is
  **maintained**: each pass derives its own small desired set – the cells of
  `V` mapped through `1/scale` plus one cell of margin, budget `P = 8` – and
  requests them `Async` at the lowest priority. Dirty rects while zoomed drop
  the intersecting backdrop tiles to `Missing` (re-requested lazily), they are
  never patched. A zoomed pan therefore always has something correct, if
  blurry, behind every cell; far from the pinch origin included. When the
  scale returns to 1 the persistent backdrop becomes the primary grid again
  (already `Ready` wherever it was maintained) and the old primary becomes a
  transient backdrop.
- **Transient.** Any other scale change (s → 1, s1 → s2 with s1 ≠ 1): the old
  primary becomes a transient backdrop with the lifetime of R9 – except that
  while a persistent backdrop exists it takes precedence and the old primary
  is simply dropped (2.85 → 6 keeps the scale-1 backdrop, drops the 2.85 grid).

In both cases a new empty primary grid is created for the new scale and R1–R3
run (visible cells `Sync` if `syncOnScaleCommit`).

The persistent backdrop is never repainted in place, never receives partial
dirty rects, never grows beyond `P` tiles. A transient backdrop is never
touched at all.

**R8 – backdrop drawing.** On composite, for each cell of the primary grid
that intersects `V` and is not `Ready`, the backdrop is drawn *clipped to that
cell's rect*, its own tiles mapped through the scale ratio (old `B` onto new
`B`). `Ready` cells are drawn from the primary grid. Hence every visible pixel
comes from exactly one of the two – no doubled text, no ghost over a correct
tile – and the picture behind an unpainted cell is the geometrically correct
content at the previous resolution.

**R9 – transient backdrop lifetime.** A transient backdrop is dropped
(wholesale) when the first of these holds:

- every cell of the primary grid that intersects `V` is `Ready` for `K = 2`
  consecutive composites (the commit frame itself does not count);
- `N = 60` composites have passed since the scale change (hard bound, ~one
  second of ticks; the device never keeps a pinch unsettled that long without
  a second scale change, which drops it anyway);
- a bounds change (a resized layer is a different picture, as in v1).

Nothing else ends it. In particular it is *not* ended per tile, not by a
coverage-area computation, not by a tile cap. The persistent backdrop is not
subject to R9; it ends only when the scale returns to 1 (R7).

**R10 – dirty rects.** A dirty rect never adds, removes, hides or cancels a
tile, and never cancels a job. On a `Ready` tile it feeds the patch
sub-machine (§2.2): accumulate, one patch job in flight per tile, the next one
is requested when the current lands. On a `Rastering`/`Landed` tile (first
paint in flight) the rect is accumulated the same way and becomes the tile's
first patch once it is `Ready` – the replay in flight is still newer than
anything on screen, so letting it land is never wrong. Content therefore lags
a re-dirtied tile (an animation, a ticker) by at most one replay latency and
never starves, never blocks the engine thread, and never turns Sync. V1's
cancel-and-re-post branch and `m_wkForceSyncPaint` have no counterpart.

**R11 – progressive cover.** Absent in v2. The margin `M` is the pre-render.
If idle pre-render beyond `M` is wanted later, it is an *input* (a larger
budget and margin while `panGesture` is false and nothing is `Missing` in `V`),
not a second mechanism inside the grid.

**R12 – convergence.** With constant inputs (no dirty rect, `V` and `scale`
still) and a backend that finishes or fails every job, the model reaches a
fixed point in at most `|desired| + P` passes plus the upload budget's drain,
after which it reports `wantsPass() == false` and `visibleHoles() == 0`. The
driver relies on this (§3). `wantsPass()` is true exactly when a tile in the
primary's desired set or the persistent backdrop's desired set is not `Ready`
with an empty patch machine, or a transient backdrop is alive (it needs
composites to end). A fully maintained persistent backdrop under a still
viewport does **not** keep it true – a zoomed page at rest is at rest.

### 2.5 Outputs of one pass

```
struct PaintRequest { GridId grid; CellIndex cell; IntRect rect; Mode mode; unsigned priority; };
struct PassOutput {
    Vector<PaintRequest> paints;    // new jobs, already in priority order (primary first,
                                    // persistent backdrop last); at most one per tile
    Vector<JobId>        cancels;   // jobs whose cell left the desired set / whose grid died
    Vector<TextureId>    released;  // textures to return to the pool
    bool wantsPass;                 // see R12
    unsigned visibleHoles;          // cells in V drawing nothing AND no backdrop
                                    // behind them (the only number the driver reacts to)
};
```

And a composite-time query `drawList()` (backdrop clips + ready tiles) that the
store turns into `TextureMapper` calls.

### 2.6 Constants (initial; all are `PassInput` or model constants, testable)

| name | value | meaning |
|---|---|---|
| `T` | 1024 | tile edge |
| `M` | ½ `V` on each side at scale 1; ¼ `V` when zoomed, doubled on the pan side | cover margin; the pan side is the sign of the last movement of `V`, whether or not a finger is down – a fling after release moves fastest |
| `budget` | 24 tiles (~96 MB RGBA) | primary grid cap; a transient backdrop is bounded by its former budget, the persistent one by `P` |
| `workers` | clamp(cores − 2, 2, 4) | raster threads; 0 = inline replay |
| in-flight | `workers + 2` | running + started jobs, bounds tile buffers |
| `P` | 8 tiles | persistent (scale-1) backdrop budget while zoomed |
| `syncOnScaleCommit` | true | visible cells of a new scale are painted on the engine thread |
| `K` | 2 composites | transient backdrop drop after full visible coverage |
| `N` | 60 composites | transient backdrop hard bound |
| small store | both edges ≤ 1024 | never `Async`, never budgeted (v1's `wkIsSmallStore`) |

### 2.7 What is deliberately gone

Cover rect as state, keep rect, `TileEraseThreshold`, rect-equality tile
matching, lattice backstop, `wkStashStaleTiles` and the four stale counters,
`wkPaintedRect`/`wkUncoveredParts`, progressive cover (four members), the
visible-rect span (replaced by the margin on the pan side plus the priority queue), unpainted
composite budgets and every other timeout, `wkUploadIsOverdue`, `wkTextureIsForeign` (impossible by
construction: a texture is created for a cell rect and dies with the tile),
`hideUntilPainted` (Missing draws nothing, always), the zoom/ghost/cover
traces (replaced by the input trace, §5.3).

## 3. Driver and GraphicsLayerTextureMapper contract

The model is only as good as its inputs. These are the promises each side
makes; the ones marked **new** are what TileGrid v2 changed.

**GraphicsLayerTextureMapper → store**

- `V` is passed every flush, in scaled layer coordinates, for the scroll
  position the composite will show (unchanged: `flushCompositingState` rect
  mapped per child; the driver calls `frameViewDidScroll()` before and after
  the flush).
- The contents scale is passed before the visible rect and before the update
  (unchanged order: `wkSetPageScale`, `updateContentsScale`, `wkSetVisibleRect`,
  `updateContents`), so a pass sees the new scale and the new `V` together.
- `m_wkNeedsFullRepaint` / size / scale change → full dirty rect (unchanged).
- **new:** the early-out asks the store `wantsPass()` instead of
  `wkHasUnpaintedVisibleTiles() || wkHasPendingProgressiveCover()`.

**Driver → engine**

- Scroll fast path (`g_gpuScrollFast`), targeted composites and the
  scroll-position handshake stay as they are.
- **new:** `forceDirtyTree()` is never used to repair tiles. `gpuPresent()`
  reads the sum of `visibleHoles()` over the tree after the paint; if it is
  non-zero it requests **one more composite** (present) – no dirtying, no
  escalation, no cooldown. R5 and R12 guarantee the holes close on their own.
  `gpuArmRepair`, `gpuNoteRepairResult`, `g_gpuRepairMisses`,
  `g_gpuForceFullNext`'s repair use and the `repairloop` line went with the old
  path.
- `forceDirtyTree()` stays for its original purpose only: content dirtied by
  the pump before a manual composite (loads, clicks, input).
- **new:** the driver tells the store when a pan gesture is running
  (`wkWinUWPSetPanGesture(bool)`, a level, mirrors `g_panGesture`). The model
  derives the pan *direction* from consecutive visible rects itself; the flag
  is recorded in the trace and reserved for R11-style idle work, it does not
  gate the margin.
- **new, diagnostic:** per-composite counters of *why* a layer was
  repainted in full (`needsDisplay` / `forceDirtyTree` / size change / scale
  change / store created), appended to the perf row as `dirty_src=<a>/<b>/<c>/<d>/<e>`.
  This is what identifies the code-hosting site's `dirty_full=54` caller that
  the repair cap did not catch. Cheap, and independent of the rewrite.

**Harness → driver** (no change, restated because the model depends on it)

- The harness commits at most **one page scale per pinch**; the preview is a
  XAML transform on the last frame. The engine sees a step, not a ramp.
- During a pan the harness presents the last engine frame translated by at
  most the coarse step (≤ ⅓ screen). `M` at scale 1 is ½ screen, so the engine
  tiles what the finger will reveal before it is revealed; when zoomed the
  presenter's step is the same in screen pixels, which is ⅓·`V` in scaled
  coordinates too – `M` = ¼ `V` doubled on the pan side covers it.

## 4. Invariants (what the tests assert after every step)

```
I1  lattice        every tile's rect == cellRect(index) ∩ B; no two tiles share an index
I2  budget         |primary grid| ≤ budget
I3  visible first  if budget ≥ |cells(V)| then every cell of V has a tile
I4  ready is true  a Ready tile's texture size == its cell rect size and its scale == grid scale
I5  draws once     for every visible cell exactly one of {primary Ready tile, backdrop clip,
                   nothing} is drawn; "nothing" only when no backdrop exists
I6  no foreign     a tile never draws pixels rasterised for another index or scale
I7  backdrop bound backdrop age ≤ N composites; gone within K composites of full visible coverage
I8  one backdrop   at most one backdrop at any time
I9  no timeouts    a tile leaves Rastering only via ReplayFinished/ReplayFailed/Cancelled;
                   a visible cell's job is never queued behind a non-visible one
I10 dirty          a dirty rect changes no tile's existence or drawability and cancels no job
I11 convergence    constant inputs and a backend that completes or fails every replay ⇒
                   wantsPass() false and visibleHoles() == 0 within |desired| + P +
                   ceil(|desired| / uploadsPerComposite) passes
I12 one job        at most one replay job per tile at any time (first paint or patch);
                   a pass with unchanged V, scale and empty dirty rect creates jobs only for
                   tiles that are Missing or Pending at its start
I13 removal        a removed tile's job is cancelled and its texture returned exactly once
I14 determinism    the same input sequence yields the same output sequence (no time, no address order)
I15 blurry not white while scale != 1 every visible cell draws a Ready tile or the persistent
                   backdrop (where the backdrop has a Ready tile for it); the persistent backdrop
                   holds ≤ P tiles and is gone at scale 1
I16 legal arrows   every transition of every machine (tile, patch, grid, job, phase) is in its
                   table – checked on every event in the tests, logged in the engine
I17 phases         Requested only InPass, Uploaded only InComposite, SetScale/SetVisible only Idle;
                   the event inbox is drained only at BeginPass/BeginComposite
I18 nothing to show, nothing to raster
                   a store that cannot be composited (its visible rect does not meet its bounds)
                   desires no cell, so it never rasters and never asks for another pass
```

## 5. Code layout

### 5.1 New files (WebKit tree, all under `WK_WINUWP`)

```
Source/WebCore/platform/graphics/texmap/
  TextureMapperTileGridModel.h/.cpp   pure model: PassInput → PassOutput, drawList(); no WebCore
                                       includes except <wtf/Vector.h>-free plain std types
                                       (std::vector, own IntRect-like struct) so the PC test
                                       builds it without WebKit
  TextureMapperTileGridCore.h/.cpp    the store's core, also WebCore-free: tile records
                                       (cell, state, texture handle, pending replay handle),
                                       runs the model, drives two backend interfaces
                                         RasterBackend  { record(cell, rect, sync) -> ReplayHandle;
                                                          poll(ReplayHandle) -> done?; cancel() }
                                         TextureBackend { acquire(size) -> TextureHandle; upload(handle,
                                                          pixels); release(handle); budgetLeft() }
                                       and produces a DrawList (backdrop clips + ready tiles with
                                       their handles). Everything the device shows is decided here.
  TextureMapperTileGridStore.h/.cpp   the WebCore adapter (~200 lines): TextureMapperTiledStore
                                       interface; implements the two backends with the cairo
                                       replay / worker pool and BitmapTexture / pool; turns the
                                       DrawList into TextureMapper calls (beginClip/drawTexture);
                                       owns
                                       TileGridTile records (texture, pending buffer), calls
                                       the model, turns PaintRequests into raster/upload calls,
                                       turns drawList() into TextureMapper calls
  TextureMapperTiledStore.h            the interface GraphicsLayerTextureMapper uses
                                       (setContentsToImage, updateContentsScale, wkSetPageScale,
                                       wkSetVisibleRect, updateContents ×2, wkFinishPendingPaints,
                                       wantsPass, visibleHoles, wkAppendDiagnostics);
                                       TextureMapperTileGridStore is its one implementation
```

`TextureMapper.cmake` gets the new `.cpp` files. `GraphicsLayerTextureMapper.h`
holds `RefPtr<TextureMapperTiledStore>` (two members) and both `create()` call
sites make a `TextureMapperTileGridStore`; everything else in
`GraphicsLayerTextureMapper.cpp` stays.

Raster/upload mechanics – the worker pool, the cairo replay and the
per-composite upload budget – live in `TextureMapperTileGridStore`. The pool
and the budget are process-wide and reached through the existing exported
functions, so they are shared by every store.

### 5.2 Runtime switch — gone

A `wkWinUWPSetTileGridV2()` / `wkWinUWPTileGridV2()` pair, read at store
creation, carried v2 through three device rounds beside v1, together with a
`wkWinUWPSetThreadedRaster()` switch for the worker pool. Both are gone:
`TextureMapperTiledBackingStore.{h,cpp}` are byte-identical to
`webkitgtk-2.52.4` again, nothing under `WK_WINUWP` references the class, a
layer pass is always rastered on the worker pool and an image pass always
inline. `TextureMapperTiledStore` stays as the interface the layer names, so a
second implementation is again a change at the two `create()` sites only.

### 5.3 Input trace (replaces zoom/ghost/cover traces)

One line per pass and per composite, per store, into a 256-line ring drained
by `wkWinUWPTakeTexmapZoomTrace` (name kept so the driver needs no change):

```
tg S<id> pass  s=<scale> B=<w>x<h> V=<x>,<y>,<w>,<h> d=<x>,<y>,<w>,<h> pan=<0|1> budget=<n>
tg S<id> out   miss=<n> rast=<n> land=<n> ready=<n> jobs=<sync>/<async> cancel=<n> rm=<n> holes=<n> bd=<p|t><age>|-
tg S<id> ev    <machine>:<from>-><to>            only for an illegal arrow (engine build)
tg S<id> comp  V=<...> holes=<n> bdclips=<n>
```

`pass` lines are the exact `PassInput`; `tests/tilegrid/replay.cpp` feeds a
`stage.txt` back into the model on the PC and asserts the invariants along the
way. This is the property the old traces lacked: a device session becomes a
test case.

### 5.4 Host tests (port repo, `tests/tilegrid/`)

```
CMakeLists.txt         -DWEBKIT_SOURCE_DIR=<tree>; builds the model and the core out of the
                       WebKit tree with MSVC x64 - no engine, no cross build, no device
check.h                20-line CHECK/CHECK_EQ macro, no framework
fakes.h/.cpp           FakeRasterBackend: a replay "lands" after a configurable number of
                       composites and yields a pixel signature (storeId, cell, scale,
                       generation) instead of pixels; FakeTextureBackend: handles, upload
                       budget, optional pool reclaim; DrawRecorder: what each visible cell
                       shows (signature / backdrop signature / nothing)
invariants.cpp         I1–I18 as functions over a model instance, plus the screen-level
                       forms over the DrawRecorder (I5, I6 as "the signature in this cell
                       is this cell at this scale, or the backdrop's, never anything else")
scenarios.cpp          the scripted scenarios below, each run twice: against the model
                       alone and against the core with the fakes (worker latency 0, 1, 3
                       and never; upload budget 2 and unlimited)
core-scenarios.cpp     the same scenarios against the core with the fakes
fuzz.cpp               random op sequences (seeded), invariants after every step,
                       convergence check whenever inputs freeze
replay.cpp             stage.txt `tg … pass` lines → model, invariants per line
replays/               device sessions captured as stage.txt, replayed by the above
run-tests.ps1          configure + build + run, exit code = failures
```

Scenarios (each one is a symptom the device showed):

1. **Scroll 1:1, long page** – V moves 200 px per pass over 40 passes: no
   holes, tile count ≤ budget, at most one new row per pass.
2. **Fling** – V jumps ⅓ screen per pass for 12 passes, then stops: holes only
   in cells beyond `M` (must be 0 with the margin rule), convergence ≤ 3 passes.
3. **Pinch 1 → 2.85 at (360, 540), then pan 4 screens away** (the pan-away-from-
   the-pinch-origin failure): after the commit every visible cell is `Ready`
   (Sync) or shows the backdrop; while panning no visible cell ever draws
   foreign pixels (I6) and no visible cell is ever white (I15) – the persistent
   backdrop follows `V` with ≤ P tiles; with `syncOnScaleCommit` off the same
   holds one frame later.
4. **Zoom out 2.85 → 1** – the persistent backdrop becomes primary and is
   already `Ready` under `V`; the 2.85 grid is a transient backdrop, gone within
   K; clips map through 1/2.85. Then **2.85 → 6**: the 2.85 grid is dropped, the
   scale-1 backdrop stays (I8).
5. **Scale change mid-raster** – 6 tiles `Rastering` when the scale changes:
   all cancelled (I13), none reach `Ready` in the new grid.
6. **Budget < cells(cover)** – scale 6, wide layer: V fully covered (I3), the
   pan side preferred for the rest, no thrash (the same cells are desired on
   consecutive identical passes).
7. **Dirty rects during scroll** – a 50×50 dirty rect every pass: only
   intersecting `Ready` tiles get patches; no tile added/removed, no job
   cancelled by it (I10); a tile dirtied while its first paint is in flight
   gets exactly one patch job after it lands (I12).
13. **Animation** – the same tile dirtied on every pass for 50 passes with
    worker latency 2: one patch job in flight at all times, the tile draws
    on every composite, its content lags by at most one job, no Sync request
    is ever issued (R10).
14. **Grid walks** – every scale sequence over {1, 2.85, 6, 1, 2} of length ≤ 5,
    with 0–3 passes between commits: I8 always, the grid machine never takes
    an illegal arrow (I16), zoom-in shows the persistent backdrop, zoom-out
    finds it `Ready` under `V`.
15. **Sync commit in parallel** – `syncOnScaleCommit` with 6 visible cells and
    4 fake workers: the pass ends with all 6 `Ready` and the fake clock has
    advanced by two replay latencies, not six.
8. **Sticky header** – a 720×80 store whose `B` changes height every pass: its
   tile stays `Ready`, repaints only the dirty part, never draws nothing (R6).
9. **Image store** – `V` unknown, `B` 3000×2000: cells(B) within budget, no
   scale change ever applies.
10. **Upload budget** – 10 tiles land in one composite, 2 uploads allowed:
    visible ones upload regardless (R4), the rest drain over later composites,
    none is ever cancelled by waiting (I11 timing).
11. **Priority and failure** – 20 cells queued, `V` moves so that 3 queued cells
    become visible: their replays are served before the earlier non-visible
    ones and the non-visible ones that left the desired set are cancelled
    (I9, I13); a `ReplayFailed` on a visible tile yields exactly one new request
    on the next pass (I12).
12. **Replay** – a `stage.txt` pulled off the device after a v2 session is fed
    back through the model line by line and every store must converge.

### 5.5 Resolutions taken during the implementation

Bounds comparison in unscaled units with 1 px tolerance (a scale commit is not
a bounds change); the `ev` trace line prints `<machine>:<from>-><event>`; SML's
`process_event` return value is the illegal-arrow hook; a completion for a
`Cancelled` job is discarded silently (R5); a dirty rect on the persistent
backdrop is `Removed` plus re-add by R2; the model assigns `TextureId`s at
upload. Two corrections were folded into R6 and §2.6 above: `paintedRect`
semantics for partial-cell resizes, and the pan-side margin by direction
rather than by gesture flag.

## 6. Work packages

Omitted: this was the implementation schedule.

## 7. Device test list

Omitted: this was the bring-up checklist for the first device package.

## 8. Risks and open decisions

- **Backdrop clipping cost.** One `beginClip`/`endClip` pair per unpainted
  visible cell (≤ 15) per composite while a backdrop lives, i.e. only for a few
  frames after a pinch. Acceptable; if the stencil path on the Adreno 430 is
  slow, the fallback is scissor clipping (axis-aligned cells, no rotation in
  this port).
- **Sync commit blocks the engine thread** for two replay latencies (~50 ms at
  4 workers) once per pinch; the harness shows the scaled preview meanwhile.
  If it is felt, `syncOnScaleCommit = false` costs one blurry frame instead.
  Both are host-tested; the device decides.
- **Transparent layers.** R8 draws the backdrop only through non-Ready cells,
  so a translucent layer never shows old + new content at once. A translucent
  *page* over a background layer is unaffected (the backdrop belongs to one
  store).
- **Budget vs. |cells(V)|.** V in scaled layer coordinates is the screen in
  device pixels at every page scale (the scale is a transform on the RenderView
  layer, see `wkVisibleRectForChild`), i.e. 720×1280 ⇒ at most 2×3 cells of
  1024 whatever the zoom. I3 holds with budget 24 at every scale; scenario 6
  exercises the truncation with an artificially small budget.
- **Memory.** Primary ≤ 24 tiles, plus either a transient backdrop ≤ 24 tiles
  for at most N composites or a persistent one ≤ P = 8 tiles while zoomed.
  Same worst case as v1's grid + stash, shorter-lived.
- **A whole-tree dirty may have a cause outside tiling** (a scale flip-flop
  from the harness, a layer resize storm). The `dirty_src` counters of §3
  answer that independently of the rewrite; if it is the harness, that is a
  separate one-line fix.
