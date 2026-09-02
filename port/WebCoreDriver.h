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

// 清除全部 cookie(含持久 SQLite 库里的)。设置页"清除数据"用;引擎线程调。
void WebCoreClearCookies();

// ---- live interactive session (persistent Page + event forwarding) ----
// Load a URL into a persistent session, then forward clicks/scroll to the live
// document so buttons/forms/links work via real events and lazy images load on
// scroll. All calls must be serialized on the single engine thread. 0 on success.
int WebCoreSessionLoad(const char* url, int w, int h, uint8_t* outRGBA);
void WebCoreCloseSession();
int WebCoreClickAt(int x, int y, uint8_t* outRGBA);   // (x,y) = bitmap/viewport px
int WebCoreScrollBy(int dx, int dy, uint8_t* outRGBA); // dx>0 right, dy>0 down
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
int WebCoreEnableCompositing();                       // M1: 1 if GPU compositing is live (root GraphicsLayer attached)
int WebCoreGpuInit(void* nativeWindow, int w, int h); // M2: init GPU present (engine thread). nativeWindow=SwapChainPanel PropertySet IInspectable*; nullptr=offscreen(readback)
int WebCoreComposite();                               // M2: composite current session layer tree to the window surface (swapBuffers)
int WebCoreCompositeReadback(uint8_t* outRGBA);       // M2: offscreen composite + readback RGBA (verify), shown via existing WriteableBitmap path
void WebCoreGpuSetFlip(int flipH, int flipV);         // M2 debug: set readback flip (find correct orientation); repaint to apply
int WebCoreGpuLayerInfo(char* outBuf, int len);       // M2 debug: FrameView scroll/contents + layerTreeAsText dump
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
