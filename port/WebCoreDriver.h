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

// ---- live interactive session (persistent Page + event forwarding) ----
// Load a URL into a persistent session, then forward clicks/scroll to the live
// document so buttons/forms/links work via real events and lazy images load on
// scroll. All calls must be serialized on the single engine thread. 0 on success.
int WebCoreSessionLoad(const char* url, int w, int h, uint8_t* outRGBA);
void WebCoreCloseSession();
int WebCoreClickAt(int x, int y, uint8_t* outRGBA);   // (x,y) = bitmap/viewport px
int WebCoreScrollBy(int dy, uint8_t* outRGBA);        // dy>0 scrolls down
int WebCoreSessionPaint(uint8_t* outRGBA);
int WebCoreGetUrl(char* buf, int len);
int WebCoreFocusedEditable();                         // 1 if an editable element is focused
int WebCoreTypeText(const char* utf8, uint8_t* outRGBA);   // insert text into focused editable
int WebCoreKeyAction(int action, uint8_t* outRGBA);   // 0=Backspace, 1=Enter
int WebCoreEvalJS(const char* script, char* out, int len);  // run JS in the session, result as string
int WebCoreLiveTick(uint8_t* outRGBA);                // advance + repaint one animation/SPA frame
unsigned WebCoreGetFrameHash();                       // pixel hash of the last frame (idle detection)

#ifdef __cplusplus
} // extern "C"
#endif
