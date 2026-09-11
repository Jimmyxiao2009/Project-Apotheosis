# Changelog

Notable changes to the port layer, the UWP harness and the WebKit patch set.
Version numbers are the packaging counter the work was tested under on a
Lumia 950 (Windows 10 Mobile 15254.603); the app manifest carries the
project's own version.

## 0.1.9.48 (2026-09-10) — TileGrid v2 and mobile interaction

### Engine and tiling

- The tile bookkeeping in `TextureMapperTiledBackingStore` is rebuilt as
  **TileGrid v2**: a pure model plus a thin WebCore adapter.
- Every pass derives the desired tile set from layer bounds, scale, visible
  rect and budget and reconciles the grid to it; nothing survives by accident.
- A tile is addressed by `(column, row)` on a lattice anchored at the layer
  origin, so a clamp or a rounding can no longer produce a second grid.
- There are no grace counters and no timeouts: a raster or upload request is
  served in priority order or fails explicitly, and a failure is an event.
- Holes are counted over the desired cells, so the driver asks for one more
  present instead of dirtying the layer tree.
- `tests/tilegrid/` builds the real model and core sources against fake
  backends — no WebKit build, no ANGLE, no device — and runs the design
  invariants, scripted scenarios, a fuzzer and replays of captured device
  traces: 2.5 million checks in well under a minute.
- `docs/TILEGRID-DESIGN.md` is the design document that the sources and the
  tests cite by section number.
- The previous tile management is gone with it: `TextureMapperTile` is
  byte-identical to the tag again and `TextureMapperTiledBackingStore` is back
  to upstream plus about forty lines of viewport-limited 1024-pixel tiling.
- An off-screen layer store no longer keeps raster work alive for ever.

### Rendering correctness

- `Cairo::OperationRecorder::applyDeviceScaleFactor()` is an empty override, so
  a recorded tile paint was translated but never scaled. Both recording call
  sites scale explicitly now — this was the cause of every white or misplaced
  pixel while zoomed since rasterisation moved off the engine thread.
- Sticky and fixed elements get a compositing layer
  (`AcceleratedCompositingForFixedPosition` was never enabled), instead of
  being repainted into the scrolling layer on every frame.
- `ImageBackingStore::create()` returns null when its pixel reservation fails.
  It used to hand back a live store with an empty buffer, and a large photo
  then made the JPEG decoder write a scanline to a null address.
- A failed asynchronous decode notifies its waiting clients instead of leaving
  a white box, and an already decoded image is drawn rather than decoded again
  at a new scale.
- The software present blits each frame at the viewport the engine actually
  rendered it at, not at the viewport the harness currently wants. A frame in
  flight across a viewport change was read at the new, larger rectangle out of
  a buffer allocated for the old one, which walked past the end of the block
  into reserved address space and killed the app.
- Text: glyphs no longer rasterise with subpixel antialiasing and the synthetic
  bold offset walks in device-pixel steps, which removes the doubled text after
  a pinch when only the regular font cuts are installed.
- The package carries fallback faces for the characters the eight Latin cuts do
  not have: Simplified Chinese, the symbol blocks (arrows, check marks,
  technical and miscellaneous symbols) and emoji as monochrome outlines. They
  are Noto under the SIL Open Font License 1.1, fetched by `port/fetch-fonts.ps1`
  against pinned URLs and SHA-256 hashes, and the generated `fonts.conf` orders
  them after the Latin faces so a missing glyph is filled instead of drawn as a
  box. The fontconfig cache is stamped with the font set it was built for and
  dropped when that changes.

### Performance

- Tile rasterisation runs off the engine thread by default: a paint is recorded
  into a display list and replayed by a worker pool. Measured on the device,
  2.9 ms instead of 25 ms of engine-thread time per scroll tick.
- Tile uploads have a per-composite time budget, and a deferred upload
  schedules the composite that lands it.
- The baseline JIT is actually enabled on ARMv7 (`APOTHEOSIS_JIT`), including
  the `replaceWithJump` displacement fix that used to corrupt the JIT pool.
  Animation-heavy pages got 20 to 30 percent faster.
- Networking: HTTP/2 multiplexing, 32 connections, eight parallel loads per
  host, deferred transfers paused instead of cancelled, DNS prefetch for
  `preconnect` hints. Time to commit on a news site went from 14.8 s to 0.4 s.
- Three loader defects came out of that work: a cancelled `ResourceHandle` left
  a dangling client, every finished load leaked handle, delegate, request and
  response, and a paused redirect was never cancelled on the terminal paths.
- WTF threads get a stack reservation per thread type (JavaScript 8 MB, the
  rest 1 MB). The inherited 16 MB default exhausted the 32-bit address space on
  a page with many workers and aborted in `WTF::Thread::create`.
- Memory: the JavaScript heap is sized from the application's own cap, UWP
  memory pressure reaches WebCore, and the texture pool is accounted and
  bounded.
- Page load: images below the fold are loaded lazily, DOM timers are aligned
  onto a coarse grid while a load owns the engine thread, and a navigation
  settles on the viewport instead of on trailing beacons.

### Interaction

- **Double-tap zoom** with the tap policy a mobile browser is expected to have:
  a page that opts out of zooming or declares itself mobile-optimised is not
  zoomable, a pinch is always undoable, and the target is the innermost block
  under the finger, anchored on that block's centre.
- **Double tap returns to 1:1 from either direction**: the undo-a-pinch rule
  measures the distance from 1:1 instead of only the scale above it, so a page
  the user pinched out below 1:1 is brought back by a double tap as well. From
  below 1:1 the content grows to the right from its own left edge, where the
  committed frame lands, instead of growing around the tap and sliding back.
- **Long press on a link** opens a context menu with one action, open in new
  tab. The card is placed clear of the finger, shows a long target that can be
  dragged sideways, and is dismissed by a rotation or a window change.
- **Landscape**: `WebCoreResize` gives the engine a viewport it can change, the
  ANGLE window surface follows the panel instead of being created at a fixed
  size, and the built-in start and error pages are re-rendered on a rotation.
- **Keyboard**: the focused field is brought above the on-screen keyboard —
  on the first tap after a load and after a rotation as well — and the chrome
  is inset by the rectangle the input pane really occludes.
- Panning has an axis lock, a pinch is anchored on the finger and snaps back to
  1:1, nested scrollers are found through open shadow roots, and a pan over a
  map or canvas widget is delivered as pointer events.
- Address bar: the text is selected on focus, Enter hides the keyboard, a
  tapped suggestion navigates exactly like Enter, and the text area spans the
  whole field.
- The loading strip and the page title row are overlays: neither displaces the
  page nor costs a relayout when it appears.
- A tab switch shows the target tab's last frame while it reloads.

### Start page

- The built-in start page is BUILT for the viewport and the language it is
  shown at, every time it is shown, instead of being rendered once and replayed:
  the page at app start, the page a new tab gets and the page after a rotation
  or a language change are the same page.
- A language change reaches it as well. Before, the page that was on screen when
  the first-run language choice was made kept the language it was built with -
  and with no CJK face in the package, Chinese strings on an engine-rendered
  page are empty boxes.
- The speed-dial grid is exactly two columns in portrait and four in landscape.
  The auto-fill rule it replaces was written for a 360 px CSS viewport and fitted
  four postage stamps across a portrait screen.

### Settings and cleanup

- Every user-visible string goes through the language table, and the language
  can be chosen in Settings.
- Privacy defaults: the update check is opt-in, Qwant is available and the
  default search engine, speculation-rules prefetch is off, and `<a ping>` is
  never sent.
- The developer switches that carried features through bring-up — the two tile
  grids, off-thread rasterisation, the presenter thread, stale placeholders,
  event-driven present — are deleted now that the features are proven; axis
  lock moved to its own Scrolling section.
- The presenter thread is gone: ANGLE 2.1.13 on D3D11 cannot be driven from two
  GL threads, and serialising every call behind one lock removed its advantage.

### Diagnostics

- Opt-in `perf.csv` columns and `stage.txt` trace families cover loads,
  scrolling, tiles, gestures and the keyboard, `crash.txt` records the aborts an
  App Container leaves no dump for, and everything stays off without its file.

### Build and packaging

- `port/build-harness.ps1` takes the engine and driver directories as
  parameters, for the XAML code generation pass as well, so the two-Visual-Studio
  pipeline no longer depends on one fixed build directory name.
- The host tests look for a WebKit checkout beside the repository instead of an
  absolute path, and say so when there is none.
- `nghttp2.dll` and the bold font cuts are packaged with the application.
- `.gitattributes` keeps `wk-winuwp.patch` out of the CRLF conversion, so the
  file a checkout produces is byte-for-byte the one `git diff` wrote.
- `wk-winuwp.patch` is regenerated from the whole patched tree, and
  `WK_WINUWP-PATCH.md` groups every commit over the base tag by area.
