// ============================================================================
// WebCoreDriver.h  —  C ABI for the WebCore headless software-render driver.
//
// Two entry points, both rendering into a caller-provided w*h RGBA8888 buffer
// (>= w*h*4 bytes). Both return 0 on success, negative on failure.
//
//   WebCoreRenderHtml(html, w, h, out) — render a local UTF-8 HTML string.
//   WebCoreLoadUrl(url, w, h, out)     — load an http(s):// URL over the network
//                                        (curl backend) and render the page.
//
// Error codes (negative returns):
//   -1  bad args            -7  cairo surface create failed
//   -2  page create failed  -8  cairo context create failed
//   -3  no main frame       -9  bad/invalid URL          (WebCoreLoadUrl)
//   -4  no view            -10  load failed              (WebCoreLoadUrl)
//   -5  no document loader -11  load timed out (watchdog)(WebCoreLoadUrl)
//   -6  no document
// ============================================================================

#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

int WebCoreRenderHtml(const char* utf8Html, int w, int h, uint8_t* outRGBA);

int WebCoreLoadUrl(const char* url, int w, int h, uint8_t* outRGBA);

// Point the curl/OpenSSL backend at a CA-certificate bundle (PEM) so HTTPS
// (TLS 1.3) server certificates can be verified inside the App Container, which
// has no access to the Windows system trust store. Call once before the first
// WebCoreLoadUrl(); `path` is a UTF-8 filesystem path to a cacert.pem.
void WebCoreSetCACertPath(const char* path);

// In-memory CA blob variant (CURLOPT_CAINFO_BLOB). App Container blocks OpenSSL's
// file-based CA loading (curl 77 even on a readable file), so on device this is
// the one that works. `data` = raw cacert.pem bytes; call before the first load.
void WebCoreSetCACertBlob(const uint8_t* data, int len);

// Explicit cookie-jar SQLite path. UNSAFE on device as of 2026-07-03 (crashes —
// SQLite's Win32 VFS null-derefs opening a real file in this ARM32 UWP App
// Container build; see project memory cookie-persistence). Do not call; kept
// only so the entry point exists once the underlying bug is fixed.
void WebCoreSetCookieJarPath(const char* path);

// Cookie persistence path (JSON Lines, one cookie per line). The in-memory jar
// itself stays ":memory:" (proven stable); this is a side-channel snapshot the
// engine reads/writes itself, bypassing SQLite's real-file open() entirely.
// Must be called before the first engine call; unset/empty = not persisted
// (never crashes). `path` is a UTF-8 filesystem path.
void WebCoreSetCookieJsonPath(const char* path);

// Write current persistent (non-session, has an expiry) cookies to the JSON
// Lines path. Call when the app is about to background/suspend (UWP can kill
// a suspended app without notice). Engine-thread call.
void WebCoreFlushCookiesToDisk();

// ---- M4 step 1: per-phase timing (opt-in) ----
// Switch per-phase timing on and point it at a CSV file (one row per completed
// nav/scroll/tick/click operation). The App Container only lets us write inside
// LocalState and the engine cannot discover that path itself, so the harness
// passes it in — and only when LocalState\perf.txt exists, mirroring the
// imedebug.txt opt-in. Unset/"" = off (shipping default, one branch per probe).
// Engine-thread call; call before the first navigation.
void WebCoreSetPerfLogPath(const char* path);

// Drain the in-memory perf ring to that CSV. Rows otherwise reach disk only on
// navigation completion or when the ring fills, so call this before suspend
// (UWP can terminate a suspended app without notice). No-op when off.
void WebCorePerfFlush(void);

// ---- crash reporting (always on) ----
// Point the engine at a crash log file (LocalState\crash.txt) and arm all three
// crash legs: the WTF crash hook (fires before the trap, carries the failing
// assertion's file/line), a vectored exception handler for the fatal SEH codes,
// and signal(SIGABRT) for abort(). Each crash appends a timestamped entry with
// the reason, the host module base/size and up to 48 stack frames as
// "module +0xRVA". Needed because Windows 10 Mobile's WER writes no dump for
// fast-fail / breakpoint-trap terminations. Not opt-in. Engine-thread call;
// call it as early as possible (SetupRuntimeEnv).
void WebCoreSetCrashLogPath(const char* path);

// Append one "reason:" line plus the current stack to that crash log from outside the
// engine. The harness runs on its own CRT instance (C++/CX, MSVC v143, exceptions on),
// so the engine's terminate/new/invalid-parameter/purecall handlers never see the
// harness' own fatal paths — Application::UnhandledException and its std::terminate use
// this instead, so those aborts stop looking like a silent OS memory kill. No-op until
// WebCoreSetCrashLogPath() has run. Callable from any thread, including a dying one.
void WebCoreCrashNote(const char* reason);

// ---- network resolve mode ----
// Force the curl backend to resolve names to IPv4 only (1) or let it use whatever
// the resolver returns (0, the default). The phone has global IPv6 addresses and on
// some links the v6 path is a black hole: first contact with a host then stalls for
// 14-22 s before the main resource commits, while an immediate reload takes 0.4 s.
// Takes effect for the next request, so call it before starting a navigation.
// Measure the effect with the net_dns/net_connect/net_tls/net_ttfb columns of the
// perf CSV. Engine-thread call.
void WebCoreSetIPv4Only(int enable);

// ---- diagnostics / page metadata (written by the render/load paths) ----
// Each copies a NUL-terminated UTF-8 string into buf (<= len bytes) and returns
// the number of bytes written (excluding NUL); empty string if nothing recorded.
int WebCoreGetLastError(char* buf, int len);  // last failed load: curl code + desc + URL
int WebCoreGetDiag(char* buf, int len);       // last render diag (url/title/sizes/loads/res list)
int WebCoreGetTitle(char* buf, int len);      // last loaded page title

// Download `url` to `outPath` via a standalone curl handle (no render; reuses the
// CA blob). Returns the HTTP status code (e.g. 200) or negative on failure.
int WebCoreDownload(const char* url, const char* outPath);

// Link hit-table extracted at render time: count + rect (bitmap coords) and URL
// of entry i. Returns 1 on success, 0 if i is out of range.
int WebCoreGetLinkCount();
int WebCoreGetLink(int i, int* x, int* y, int* w, int* h, char* url, int len);

// Apotheosis: 内存压力释放(防 OOM)。harness 监听 UWP 内存事件,到高水位时经引擎线程调。
// critical: 1=严重,0=温和。一把清资源/后退页面缓存 + JSC GC + 字体缓存。
void WebCoreReleaseMemory(int critical);

// Apotheosis: engine-side memory accounting, so the harness'
// mem.txt carries more than the OS view of our working set. All sizes are bytes.
// Usage: zero the struct, set structSize = sizeof(WebCoreMemoryStats), call. The driver
// writes at most structSize bytes, so the two copies of this header may drift by a trailing
// field without breaking the ABI. Engine thread only (walks the MemoryCache and the JSC heap).
typedef struct WebCoreMemoryStats {
    int      structSize;      // in: sizeof(WebCoreMemoryStats); out: bytes actually written
    // JavaScriptCore, common VM (all zero while no VM exists yet)
    uint64_t jscHeapSize;     // Heap::size()
    uint64_t jscHeapCapacity; // Heap::capacity()
    uint64_t jscExtraMemory;  // Heap::extraMemorySize() - non-GC memory owned by GC objects
    uint64_t jscObjectCount;  // Heap::objectCount() (blockBytesAllocated needs ENABLE(RESOURCE_USAGE))
    // WebCore MemoryCache
    uint64_t cacheTotal;      // MemoryCache::size() = live + dead encoded data
    uint64_t cacheLive;       // of that, resources that still have clients
    uint64_t cacheDecoded;    // decoded (bitmap/parsed) data, all types
    uint64_t cacheCapacity;   // total budget the driver currently configured
    uint64_t imagesSize;
    uint64_t imagesDecoded;   // the big unknown: decoded image bitmaps
    uint64_t cssSize;
    uint64_t scriptsSize;
    uint64_t fontsSize;
    uint32_t imagesCount;
    uint32_t cssCount;
    uint32_t scriptsCount;
    uint32_t fontsCount;
    // TextureMapper GL textures - graphics commits are charged to AppMemoryUsage too
    uint64_t texBytes;        // every live BitmapTexture (tiles + pool + filter surfaces)
    uint32_t texCount;
    uint64_t poolBytes;       // of that, parked in BitmapTexturePool (recoverable)
    uint32_t poolCount;
    int32_t  pressureLevel;   // last level pushed via WebCoreSetMemoryPressure()
} WebCoreMemoryStats;

// Fill *out. 0 on success, negative on bad args / uninitialised engine.
int WebCoreGetMemoryStats(WebCoreMemoryStats* out);

// Apotheosis: push the harness' MemoryManager view into WebCore. level: 0 = normal,
// 1 = medium (>= 65 % of AppMemoryUsageLimit), 2 = high/critical (>= 80 %, or a High/
// OverLimit MemoryManager event). Sets WTF::MemoryPressureHandler's status - the only way
// isUnderMemoryPressure() can ever become true on this port, since the Windows poll is a
// no-op in an App Container - shrinks the MemoryCache budget and, when the level rises,
// releases memory (level 2 = synchronous full GC + drop the resource cache). Idempotent:
// calling it with the level already in effect does nothing. Engine thread only.
void WebCoreSetMemoryPressure(int level);

// 清除全部 cookie(含持久 SQLite 库里的)。设置页"清除数据"用;引擎线程调。
void WebCoreClearCookies();

// ---- live interactive session (persistent Page + event forwarding) ----
// Load a URL into a persistent session, then forward clicks/scroll to the live
// document so buttons/forms/links work via real events and lazy images load on
// scroll. All calls must be serialized on the single engine thread. 0 on success.
int WebCoreSessionLoad(const char* url, int w, int h, uint8_t* outRGBA);
void WebCoreCloseSession();
int WebCoreClickAt(int x, int y, uint8_t* outRGBA);   // (x,y) = bitmap/viewport px
// Apotheosis (double-tap zoom, 2026-09-09): like WebCoreClickAt, but the dispatched mousedown/
// mouseup carry clickCount instead of an implicit 1 — WebCore's EventHandler dispatches 'dblclick'
// after 'click' when the release's clickCount is >= 2 (PlatformMouseEvent::clickCount()). Used for
// the second tap of a double tap the harness decided is NOT a zoom (WebCoreTapPolicyAt said
// not-zoomable), so the page still gets the real dblclick a two-click page expects.
// WebCoreClickAt above is unchanged (equivalent to clickCount=1).
int WebCoreClickAtCount(int x, int y, int clickCount, uint8_t* outRGBA);
int WebCoreScrollBy(int dx, int dy, uint8_t* outRGBA); // dx>0 right, dy>0 down

// Apotheosis (instant pan): where the main frame actually is. The harness applies a touch pan to
// the presenting XAML element immediately and only needs the engine to tell it how far it has
// really got, so the preview can be reduced to the not-yet-applied remainder and clamped to the
// document instead of sliding the page off its own content.
// All values are in the units WebCoreScrollBy's dx/dy use (engine viewport px at the current page
// scale). viewW/viewH are the visible size the engine clamps against, i.e. the maximum scroll
// position is exactly (contentW-viewW, contentH-viewH), floored at 0. Any pointer may be null.
// Cheap: reads the existing layout, no relayout and no paint. Returns 0, or a negative error when
// there is no session/view. Engine thread only.
int WebCoreGetScrollState(int* x, int* y, int* contentW, int* contentH, int* viewW, int* viewH);

// Nested-scroll support (cookie-consent overlays, modals, iframes): unlike WebCoreScrollBy, which
// only ever moves the main frame, these route through WebCore's real wheel-event scroll targeting
// so an overflow:auto container/modal/iframe under the point scrolls instead of the page behind it.
// (x,y) = viewport/bitmap px, same convention as WebCoreClickAt/WebCoreScrollBy.
//
// WebCoreIsScrollableAt: hit test only, no event dispatched. Returns 1 if a scrollable ancestor
// (or an iframe) is under the point, else 0. Call at gesture start to pick the fast path.
int WebCoreIsScrollableAt(int x, int y);
// WebCoreWheelAt: dispatches one synthetic wheel event (delta in px, granularity
// ScrollByPixelWheelEvent). phase: 0 none / 1 began / 2 changed / 3 ended (currently inert on this
// port, see WebCoreDriver.cpp). Returns 1 if a nested scroller consumed it (call again with the
// next delta), 0 if it did not (main-frame scroll position is left unchanged either way — on a 0
// return the harness must call WebCoreScrollBy itself for this delta). On a 1 return this already
// composited/presented the frame into outRGBA (same paintToRGBA call WebCoreScrollBy makes) — the
// harness does not need a follow-up call to see the nested scroller move.
int WebCoreWheelAt(int x, int y, float deltaX, float deltaY, int phase, uint8_t* outRGBA);

// Apotheosis (drag as pointer events): map widgets (Leaflet, MapLibre, canvas
// apps) pan by handling pointerdown/mousedown themselves and moving their own content — they
// scroll no scrollable box, so neither WebCoreScrollBy nor WebCoreWheelAt does anything useful
// over them. These two let the harness hand such a gesture to the page as a real mouse drag.
//
// WebCoreWantsDragAt: hit test only, no event dispatched, cheap enough for gesture start (run it
// next to WebCoreIsScrollableAt). 1 = the element under (x,y) or an ancestor below <body> has a
// pointerdown/mousedown/touchstart/pointermove/touchmove listener, is a <canvas>, or sets CSS
// touch-action to something other than auto/manipulation.
int WebCoreWantsDragAt(int x, int y);
// WebCoreDragAt: dispatch the gesture as a left-button mouse drag (which the engine also turns
// into pointerdown/pointermove/pointerup). phase: 0 = press, 1 = move, 2 = release, 3 = cancel;
// (x,y) = viewport/bitmap px, same convention as WebCoreClickAt/WebCoreScrollBy. Returns 1 while
// the page owns the gesture, 0 when it does not (the harness then routes the rest of the gesture
// down its normal scroll path). Only the press decides: phases 1-3 are inert — and answer 0 —
// unless the press was consumed, so an individual mousemove that the page ignores never yanks the
// gesture away mid-pan. Apotheosis (2026-09-04): the press is owned when EITHER the engine reported
// it handled OR the point is still a drag widget (the WebCoreWantsDragAt walk, re-run here). A map
// that listens for pointerdown without calling preventDefault - a map site does exactly that -
// answers "not handled" and would otherwise lose the whole gesture on its first event. On a 1
// return the frame is already composited/presented into outRGBA the
// way WebCoreWheelAt does it; outRGBA may be null (no present attempted).
int WebCoreDragAt(int phase, int x, int y, uint8_t* outRGBA);
// Apotheosis (map-site pin, 2026-09-06): LONG PRESS on a drag widget. ENABLE_TOUCH_EVENTS is 0
// on this port and nothing synthesises Touch/Pointer input, so a touch long press cannot be
// delivered as one; what this does deliver is everything a page can key a long press off with a
// mouse: mousedown, the button STAYS DOWN while the engine turns its run loop for holdMs (so a
// press-and-hold timer inside the page - a map site drops its pin from exactly such a timer - gets
// the time it waits for), then mouseup + DOM click, and optionally a 'contextmenu' event at the
// same point (on desktop Maps the right-click menu is the usable "drop a pin / What's here?" path,
// and a long press is what a browser turns into a contextmenu).
// holdMs: 0 = default 600, capped at 2000. flags: see WEBCORE_LONGPRESS_* below.
// (x,y) = viewport/bitmap px, same convention as WebCoreClickAt. Returns 0 (kOK) once delivered -
// and also on the DRAG_WIDGET_ONLY skip, with the current frame painted into outRGBA, so the caller
// never has to tell a skip from a failure. Negative = the usual driver error codes.
#define WEBCORE_LONGPRESS_CONTEXTMENU      1   // also send a 'contextmenu' event after the release
#define WEBCORE_LONGPRESS_NO_CLICK         2   // suppress the DOM 'click' the release would fire
#define WEBCORE_LONGPRESS_DRAG_WIDGET_ONLY 4   // do nothing unless (x,y) is a canvas / touch-action:none
int WebCoreLongPressAt(int x, int y, int holdMs, int flags, uint8_t* outRGBA);

// Apotheosis (pinch on map widgets, 2026-09-06): `notches` ctrl+wheel clicks at (x,y), positive =
// wheel up = zoom in, one notch = 120 px of delta and one wheel tick (what a real mouse wheel
// sends). A pinch that starts over a map must become this instead of WebCoreSetPageScale: page zoom
// scales a picture of the map (old tiles, blurry labels), while the wheel asks the map itself for
// the next zoom level around the point. Returns 1 if the page took the wheel, 0 if nothing did (the
// caller can then fall back to page zoom). The document never scrolls on this path; on a 1 the
// frame is already composited/presented into outRGBA the way WebCoreWheelAt does it.
int WebCoreZoomWheelAt(int x, int y, int notches, uint8_t* outRGBA);

// Apotheosis (double-tap zoom, 2026-09-09): hit-test (x,y) and answer whether a double tap there
// should zoom the PAGE (Safari/mobile-Chrome semantics) instead of the harness treating it as two
// ordinary clicks. Read-only: no event dispatched, no session/frame mutated, safe to call from the
// tap-hold path before deciding whether to actually click.
// The rules, in the order they are applied (0.1.9.39):
//   1. Current page scale > 1.05 — the harness' own pinch zoom — always wins: zoomable=1,
//      target 1.0, whatever the page says. A double tap must always be able to undo a pinch,
//      or an opted-out page traps the user at the scale the pinch left behind.
//   2. Page opted out of zooming altogether (viewport meta user-scalable=no, or
//      minimum-scale == maximum-scale): not zoomable.
//   3. Page is mobile-optimised (viewport meta with width=device-width, or width unset with
//      initial-scale=1) — Blink's WebViewImpl::ShouldDisableDesktopWorkarounds(): not zoomable.
//      This is what makes taps immediate on every well-behaved mobile site, because the harness
//      only pays the double-tap hold interval where a zoom could actually happen.
//   4. CSS touch-action other than auto on the hit element or an ancestor: not zoomable
//      (mirrors WebKit's own Element::allowsDoubleTapGesture(), compiled out on this port
//      since ENABLE_TOUCH_EVENTS=0).
//   5. Otherwise zoomable, at the target below.
//   *outZoomable: 1 if double-tap-to-zoom applies here, 0 otherwise (including no session and
//     no hit, and including a target that would not move the page, see below).
//   *outTargetScale: the scale a double tap here should animate to, clamped to the driver's own
//     [1.25, 3.0] double-tap range (a subrange of WebCoreSetPageScale's [0.5, 6.0]): 1.0 in
//     case 1 above; otherwise viewport-width / (width of the innermost block-level ancestor of
//     the hit point narrower than 90 % of the layout viewport — Safari's "zoom to column"
//     heuristic), or 2.0 when no such ancestor exists. A block from 90 % of the viewport upwards
//     is the page's own full-width layout, not a column: zooming to it is a no-op.
//     A target within ±5 % of the current scale is never returned - it is reported as
//     zoomable=0 instead, so the caller forwards a clickCount=2 click rather than animating
//     to where it already is.
//   *outAnchorX/*outAnchorY: the anchor for that zoom, in the same viewport/bitmap px
//     WebCoreSetPageScale takes as focalX/focalY — always just (x,y) echoed back, so the caller
//     does not have to remember the tap point across the hold interval.
// Any output pointer may be null. Returns kOK, or a negative error (no session prints
// outZoomable=0 as well, so a caller that only checks *outZoomable still degrades safely).
int WebCoreTapPolicyAt(int x, int y, int* outZoomable, float* outTargetScale, int* outAnchorX, int* outAnchorY);

int WebCoreSyncLinks();                // refresh link hit-table after scroll settles (layout+extract, no paint)
int WebCoreEditDebug(char* out, int cap); // diag: last WebCoreTypeText canEdit/focus/insert state
int WebCoreSetPageScale(float scale, int focalX, int focalY, uint8_t* outRGBA); // M4 pinch zoom: set pageScaleFactor anchored at focal
int WebCoreGetPageScale();             // M4: current pageScaleFactor ×1000
int WebCoreSessionPaint(uint8_t* outRGBA);
int WebCoreGetUrl(char* buf, int len);
int WebCoreFocusedEditable();                         // 1 if an editable element is focused
int WebCoreTypeText(const char* utf8, uint8_t* outRGBA);   // insert text into focused editable
int WebCoreKeyAction(int action, uint8_t* outRGBA);   // 0=Backspace, 1=Enter
void WebCoreSetUserAgentMobile(int mobile);           // 1=mobile iPhone UA (default), 0=desktop Edge UA
void WebCoreSetUserAgentString(const char* ua);       // custom UA override (non-empty wins over mobile/desktop; empty clears)
void WebCoreSetSpeculativePrefetch(int enabled);      // <script type="speculationrules"> prefetch, default off; sticky (live page + new sessions)
// Apotheosis (M4): warm up an origin before the user navigates to it — call it while a URL is being
// typed (debounced) so the DNS lookup is done when Enter arrives. Full URL or bare host ("example.com");
// anything else ignored. DNS only: libcurl cannot open a reusable connection ahead of time (see
// WebCoreDriver.cpp). Non-blocking (work queue), idempotent per host, engine thread.
void WebCorePreconnect(const char* url);
int WebCoreEnableCompositing();                       // M1: 1 if GPU compositing is live (root GraphicsLayer attached)
int WebCoreGpuInit(void* nativeWindow, int w, int h); // M2: init GPU present (engine thread). nativeWindow=SwapChainPanel PropertySet IInspectable*; nullptr=offscreen(readback)
int WebCoreComposite();                               // M2: composite current session layer tree to the window surface (swapBuffers)
int WebCoreCompositeReadback(uint8_t* outRGBA);       // M2: offscreen composite + readback RGBA (verify), shown via existing WriteableBitmap path
void WebCoreGpuSetFlip(int flipH, int flipV);         // M2 debug: set readback flip (find correct orientation); repaint to apply
int WebCoreGpuLayerInfo(char* outBuf, int len);       // M2 debug: FrameView scroll/contents + layerTreeAsText dump

// Apotheosis: event-driven present. Register a wake-up the
// engine calls whenever something wants to be presented (rendering update scheduled, image
// loaded, off-thread raster tile finished, a tick that ended still dirty) instead of having the
// harness poll on a fixed timer. Fired at most once per composite; disarmed at the top of every
// WebCoreLiveTick. THE CALLBACK MAY RUN ON THE ENGINE THREAD OR ON A RASTER WORKER: it must not
// block and must not call back into the engine - post to a queue and return. nullptr unregisters
// (back to pure polling). Register from the engine thread, once, before the first navigation.
void WebCoreSetPresentRequestCallback(void (*cb)(void* ctx), void* ctx);
// Apotheosis (package 5): the pan present handshake is gone - WebCoreSetPanGesture /
// WebCorePresent / WebCoreGetOwedSwapScroll / WebCorePresentFrame. It let a gesture take the
// presents away from the engine so its own composites could not race the XAML TranslateTransform
// the harness drew a pan with; that preview is deleted, and it was the only caller the mode ever
// had. The engine presents every composite as it makes it.

int WebCoreEvalJS(const char* script, char* out, int len);  // run JS in the session, result as string
int WebCoreLiveTick(uint8_t* outRGBA);                // advance + repaint one animation/SPA frame
int WebCoreGetPendingResourceCount();                 // pending cached resources in the current document
unsigned WebCoreGetFrameHash();                       // pixel hash of the last frame (idle detection)

// ---- find-in-page ----
int WebCoreFindString(const char* utf8, int matchCase, int wrap, uint8_t* outRGBA); // mark+highlight all, select first; returns match count (>=0) or neg error
int WebCoreFindNext(int forward, uint8_t* outRGBA);   // next/prev with last query (no re-mark); 1=hit, 0=none, neg=error
int WebCoreFindClear(uint8_t* outRGBA);               // clear find highlight + selection

#ifdef __cplusplus
} // extern "C"
#endif
