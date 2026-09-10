// ============================================================================
// WebCoreDriver.cpp  —  WebCore headless software-render driver (Phase 1b)
//
// Target: clang-cl --target=thumbv7-unknown-windows-msvc /std:c++23
//         App Container (WINAPI_FAMILY_APP), -DWK_WINUWP=1, exceptions OFF.
//         Software rendering only (USE_CAIRO/USE_FREETYPE/USE_FONTCONFIG/
//         USE_HARFBUZZ on; TEXTURE_MAPPER/ANGLE/SKIA off).
//
// Exposes one C entry point that turns a UTF-8 HTML string into an
// RGBA8888 pixel buffer rendered by WebCore through a Cairo image surface.
//
//   extern "C" int WebCoreRenderHtml(const char* utf8Html, int w, int h,
//                                    uint8_t* outRGBA);
//
// Returns 0 on success, negative on failure (see error codes below).
//
// Pipeline (mirrors WebCore::SVGImage::dataChanged + ::draw, the canonical
// in-tree headless render path, see Source/WebCore/svg/graphics/SVGImage.cpp):
//
//   1. process init: JSC::initialize / WTF::initializeMainThread /
//      WebCore::initializeCommonAtomStrings  (once, guarded)
//   2. PageConfiguration via pageConfigurationWithEmptyClients(...)
//   3. Page::create(...)  -> main LocalFrame is created by the empty-client
//      MainFrameCreationParameters inside the helper
//   4. localMainFrame()->setView(LocalFrameView::create(*frame)); frame->init()
//   5. feed HTML through the active DocumentLoader's DocumentWriter
//      (setMIMEType / begin / addData / end)
//   6. size the view, updateLayout()
//   7. cairo_image_surface(ARGB32) -> GraphicsContextCairo -> view->paint(...)
//   8. copy/swizzle pixels into caller's RGBA8888 buffer
//
// NOTE on pixel format: Cairo CAIRO_FORMAT_ARGB32 is, in memory on a
// little-endian machine, premultiplied BGRA bytes (B,G,R,A). The caller asked
// for RGBA8888, so step 8 swizzles B<->R and un-premultiplies alpha.
// ============================================================================

// config.h MUST be first, exactly like every WebCore TU. It pulls in
// cmakeconfig.h (HAVE_CONFIG_H + BUILDING_WITH_CMAKE are defined on the
// command line) and all of WTF/Platform.h + the WEBCORE_EXPORT export macros.
// Header on -I path: E:\Apotheosis\WebKit\Source\WebCore\config.h
#include "config.h"

#include "WebCoreDriver.h"
#include "PortPerf.h"    // Apotheosis: M4 perf probes called from the port-layer clients

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <csignal>
#include <cstdlib>     // Apotheosis: std::abort / _set_purecall_handler (crash.txt legs)
#include <atomic>      // Apotheosis: try-flag guarding perfFlush() from the crash legs
#include <exception>   // Apotheosis: std::set_terminate (crash.txt leg (d))
#include <new.h>       // Apotheosis: _set_new_handler — CRT form, gets the requested size
#include <JavaScriptCore/ExecutableAllocator.h>   // Apotheosis: JIT pool range / isJITPC for crash.txt       // Apotheosis: signal(SIGABRT) leg of the crash.txt logger
#include <vector>
#include <curl/curl.h>   // 下载用独立 curl_easy 句柄(WebCoreDownload)

// ---- Cairo (vcpkg arm-uwp, reached via -imsvc ...\include\cairo) ----
#include <cairo.h>

// ---- WTF ----
// E:\Apotheosis\build-clang-webcore\WTF\Headers\wtf\...
#include <wtf/MainThread.h>          // WTF::initializeMainThread
#include <wtf/RefPtr.h>              // RefPtr, adoptRef
#include <wtf/Ref.h>                 // Ref
#include <wtf/StdLibExtras.h>        // (also pulled by config.h) WTF::move lives in <wtf/StdLibExtras.h>/<wtf/MainThread.h> chain
#include <wtf/text/WTFString.h>      // WTF::String, _s literal
#include <wtf/text/CString.h>        // String::utf8() for render diagnostics
#include <wtf/text/MakeString.h>     // makeString(): WTF::String 不可变,拼接走 makeString(IME 直接置值)
#include <wtf/URL.h>                 // WTF::URL

// ---- JavaScriptCore ----
// E:\Apotheosis\build-clang-webcore\JavaScriptCore\PrivateHeaders\JavaScriptCore\...
#include <JavaScriptCore/InitializeThreading.h>   // JSC::initialize
#include <JavaScriptCore/JSCJSValue.h>            // JSC::JSValue(WebCoreEvalJS)
#include <JavaScriptCore/JSCJSValueInlines.h>     // JSValue::toWTFString(inline)
#include <JavaScriptCore/JSGlobalObject.h>        // JSGlobalObject::vm()
#include <JavaScriptCore/JSLock.h>                // JSC::JSLockHolder

// ---- PAL ----
// E:\Apotheosis\build-clang-webcore\PAL\Headers\pal\...
#include <pal/SessionID.h>          // PAL::SessionID

// ---- WebCore public (PrivateHeaders symlink to source) ----
// E:\Apotheosis\build-clang-webcore\WebCore\PrivateHeaders\WebCore\...
#include <WebCore/CommonAtomStrings.h>   // WebCore::initializeCommonAtomStrings
#include <WebCore/WebCoreJITOperations.h>// WebCore::populateJITOperations (no-op w/ C_LOOP)
#include <WebCore/EmptyClients.h>        // pageConfigurationWithEmptyClients
#include <WebCore/PageConfiguration.h>   // WebCore::PageConfiguration
#include <WebCore/BackForwardCache.h>    // Apotheosis: 关后退页面缓存防 OOM
#include <WebCore/MemoryCache.h>         // Apotheosis: 资源缓存上限
#include <WebCore/MemoryRelease.h>       // Apotheosis: WebCore::releaseMemory(内存压力时一把清)
#include <wtf/MemoryPressureHandler.h>   // Apotheosis: WTF::Critical / Synchronous
#include <WebCore/CommonVM.h>            // Apotheosis: g_commonVMOrNull(WebCoreGetMemoryStats 的 JSC 堆数)
#include <JavaScriptCore/VM.h>           // Apotheosis: JSC::VM::heap
#include <JavaScriptCore/HeapInlines.h>  // Apotheosis: Heap::size()/capacity()/extraMemorySize()
#include <JavaScriptCore/JSLock.h>       // Apotheosis: JSLockHolder(读堆前取 VM 锁)
#include <wtf/FastMalloc.h>              // Apotheosis: WTF::releaseFastMallocFreeMemory
// Apotheosis: texmap memory counters (WebKit winuwp 0c78243bf5). Declared by hand rather than
// included: BitmapTexture.h pulls in TextureMapperGLHeaders.h, whose GL prototypes collide with
// the ANGLE headers this file already uses (glFinish/glReadPixels/glViewport go missing).
namespace WebCore {
void wkWinUWPTexmapTextureStats(uint64_t& bytes, unsigned& count);   // BitmapTexture.h
void wkWinUWPTexmapPoolStats(uint64_t& bytes, unsigned& count);      // BitmapTexturePool.h
// Apotheosis (OFFTHREAD-RASTER-LOG.md §4): the in-flight counter of the off-thread tile
// rasterisation (TextureMapperTiledStore.h). Declared here for the same reason as the two above —
// including the texmap header would drag in TextureMapperGLHeaders.h. The runtime switch that used
// to sit beside it is gone with package 5: a layer pass is always rastered on the worker pool.
unsigned wkWinUWPTexmapPendingRasterTiles();
// Apotheosis (OFFTHREAD-RASTER-LOG.md §10, WebKit winuwp 099064a24d..c0608a6353): step 4 made
// first paints asynchronous too, so a tile whose replay has not landed draws NOTHING. Missing the
// follow-up composite is now an empty tile, not a stale one — hence the edge-triggered counter and
// the worker-side wake-up below, which cover the two cases polling alone cannot.
unsigned wkWinUWPTexmapTakeFinishedRasterTiles();          // engine thread; reading resets
void wkWinUWPSetRasterCompletionHandler(void (*)());       // called ON A WORKER thread
void wkWinUWPTexmapRasterStats(unsigned& posted, unsigned& cancelled, unsigned& blockingWaits);
// Apotheosis (2026-09-06, WebKit winuwp): per-frame tile-upload budget. A finished replay costs a
// ~4 MB glTexSubImage2D on the engine thread (~8-10 ms on the Adreno 430), and draining seven to
// nine of them in one pass is what turns a 2.9 ms scroll tick into an 80 ms one. The engine now
// uploads at most a few per composite - visible tiles that would otherwise be a hole are exempt -
// and reports how many it pushed to a later frame. Non-zero means "the pixels exist, nothing has
// shown them yet", so it owes a present exactly like wkWinUWPTexmapTakeFinishedRasterTiles();
// reading resets it. Declared by hand for the same reason as everything above.
unsigned wkWinUWPTexmapTakeDeferredUploads();
// Apotheosis (docs/TILEGRID-DESIGN.md 5.3): the engine's TileGrid input trace - one `tg` line per
// pass and per composite of every tiled layer store, a pass line being the complete PassInput, so
// a device session replays on the PC (tests/tilegrid). This hands over the lines that have not been
// read yet (oldest first, whole lines, NUL-terminated, 0 = nothing pending). Engine thread,
// allocation-free. The name predates the rewrite (it used to drain a zoom trace as well) and is
// kept so this file needs no change. Declared by hand for the same reason as everything above.
size_t wkWinUWPTakeTexmapZoomTrace(char* buffer, size_t length);
// Apotheosis (2026-09-07): one header line plus one line per live tiled backing store - size,
// contents scale, page scale, visible rect, the tile counts, and for a TileGrid v2 store the state
// of its grid. This is the dump a device session takes *while* an artefact is on screen, so
// WebCoreGpuLayerInfo() writes it before the layer tree rather than after it. Declared by hand for
// the same reason as everything above.
size_t wkWinUWPDumpTexmap(char* buffer, size_t length);
// Apotheosis (2026-09-08, docs/TILEGRID-DESIGN.md section 3, GraphicsLayerTextureMapper.cpp):
// per-cause breakdown of the wkTexmapDirtyFull count above - which of updateBackingStoreIfNeeded's
// full-repaint conditions fired (a layer that hits more than one adds to more than one counter),
// plus how many GraphicsLayerTextureMapper::setNeedsDisplay() calls actually flipped m_needsDisplay
// false->true. Both reset on read. Declared by hand for the same reason as everything above.
void wkWinUWPTexmapDirtySrcStats(unsigned& needsDisplay, unsigned& storeCreated, unsigned& sizeChange, unsigned& scaleChange, unsigned& setNeedsDisplayCalls);
// Apotheosis (2026-09-08, docs/TILEGRID-DESIGN.md sections 3 and 5.2): the one number
// the driver reads from the TileGrid (TextureMapperTiledStore.h). Declared by hand for the same
// reason as everything above - that header pulls in the texmap GL headers. The v1/v2 switch that
// used to sit here is gone with package 5; the TileGrid is the only store there is.
//   wkWinUWPTexmapVisibleHoles                   the sum of visibleHoles() over every store that
//     has composited since the last call - visible cells drawing nothing with no backdrop behind
//     them. Reading resets it. This is the ONLY number the repair reacts to (gpuPresent).
// (wkWinUWPSetTileGridPanGesture, the level that mirrored a pan gesture into every live store, went
// with the pan present handshake in package 5 - see the block at g_dragActive. The engine-side
// setter is still there and simply never called; it comes out with the next engine rebuild.)
unsigned wkWinUWPTexmapVisibleHoles();
}
#include <WebCore/CookieJar.h>           // WebCore::CookieJar(cookie 持久化)
#include <WebCore/NetworkStorageSession.h>   // deleteAllCookies(WebCoreClearCookies)
#include <WebCore/StorageSessionProvider.h>  // 完整类型(Ref<StorageSessionProvider> 析构需要)
#include "PortNetworkStorageSession.h"   // WebCorePort::makeStorageSessionProvider / ensureDefaultPortStorageSession
#include "PortChromeClient.h"            // WebCorePort::PortChromeClient(开合成,捕获根图层)
#include "PortWebSocket.h"              // WebCorePort::PortSocketProvider(WebSocket,见该头文件)
#include <WebCore/LayoutMilestone.h>     // Apotheosis (M4 load timeline): DidFirstVisuallyNonEmptyLayout
#include <WebCore/Page.h>                // WebCore::Page
#include <WebCore/Settings.h>            // Page::settings()
#include <WebCore/FontLoadTimingOverride.h>  // Apotheosis (M4): FontLoadTimingOverride::Swap
#include <WebCore/LocalFrame.h>          // WebCore::LocalFrame
#include <WebCore/LocalFrameInlines.h>   // inline LocalFrame::document()/protectedDocument()
#include <WebCore/LocalFrameView.h>      // WebCore::LocalFrameView
#include <WebCore/DocumentView.h>        // inline LocalFrame::view()/protectedView()
#include <WebCore/FrameLoader.h>         // FrameLoader::activeDocumentLoader
#include <WebCore/FrameLoaderStateMachine.h> // Apotheosis (M4 settle): committedFirstRealDocumentLoad()
#include <WebCore/DocumentLoader.h>      // DocumentLoader::writer()
#include <WebCore/DocumentWriter.h>      // DocumentWriter setMIMEType/begin/addData/end
#include <WebCore/Document.h>            // Document::updateLayout / updateLayoutIgnorePendingStylesheets
#include <WebCore/EventLoop.h>           // Document::eventLoop().performMicrotaskCheckpoint()(驱动模块求值)
#include <WebCore/SharedBuffer.h>        // WebCore::SharedBuffer::create(span)
#include <WebCore/IntRect.h>             // WebCore::IntRect
#include <WebCore/IntSize.h>             // WebCore::IntSize
#include <WebCore/FloatRect.h>           // boundingClientRect()
#include <WebCore/HTMLCollection.h>      // Document::links()
#include <WebCore/CachedResourceLoader.h> // 遍历已缓存资源表(诊断 SPA 模块图加载)
#include <WebCore/CachedResource.h>      // CachedResource::url()/status()
#include <WebCore/DocumentResourceLoader.h> // Document::cachedResourceLoader() 的 inline 定义
#include <WebCore/HTMLAnchorElement.h>   // href()
#include <WebCore/HTMLBodyElement.h>     // document.body()->childElementCount()(诊断 SPA 挂载)
#include <WebCore/ElementInlines.h>      // Element::boundingClientRect()
#include <WebCore/Color.h>               // WebCore::Color, Color::white
#include <WebCore/GraphicsContextCairo.h>// WebCore::GraphicsContextCairo
#include <WebCore/RefPtrCairo.h>         // RefPtr<cairo_t> deref traits

// ---- network-load path (WebCoreLoadUrl) ----
#include <wtf/RunLoop.h>                    // RunLoop::run / currentSingleton / Timer
#include <wtf/Seconds.h>                    // 30_s
#include <wtf/Function.h>                   // WTF::Function
#include <wtf/UniqueRef.h>                  // makeUniqueRefWithoutRefCountedCheck
#include <wtf/Variant.h>                    // std::get on the MainFrameCreationParameters variant
#include <WebCore/FrameLoadRequest.h>       // FrameLoadRequest
#include <WebCore/ResourceRequest.h>        // ResourceRequest
#include <WebCore/SubstituteData.h>         // SubstituteData
#include <WebCore/LocalFrameLoaderClient.h> // base of LoadingFrameLoaderClient
#include "LoadingFrameLoaderClient.h"       // WebCorePort::LoadingFrameLoaderClient

// ---- live interactive session (WebCoreSessionLoad/ClickAt/ScrollBy/Paint) ----
// 把一次性渲染升级为常驻会话:同一个活 Page 上转发鼠标事件(点按钮/表单/链接)、滚动
// (触发 IntersectionObserver 懒加载图片/下方内容)后重新布局并重绘。所有调用串行在唯一引擎线程。
#include <WebCore/EventHandler.h>            // LocalFrame::eventHandler() 派发鼠标事件
#include <WebCore/HandleUserInputEventResult.h> // EventHandler 鼠标方法返回类型(否则不完整类型报错)
#include <WebCore/FocusController.h>         // page->focusController().setActive/setFocused(headless 处理 JS 事件必需)
#include <WebCore/Editor.h>                  // editor().canEdit()/insertText()/command(输入法文本插入)
#include <WebCore/FindOptions.h>             // WebCore::FindOption / FindOptions(页内查找)
#include <WebCore/SimpleRange.h>             // Page::FindStringData 内含 std::optional<SimpleRange>
#include <WebCore/HTMLInputElement.h>        // IME 直接改 input.value()/dispatchInputEvent()(绕开 editor 选区)
#include <WebCore/HTMLTextAreaElement.h>     // 同上,textarea
#include <WebCore/HTMLElement.h>             // isContentEditable()(contenteditable 检测)
#include <WebCore/Document.h>                // elementFromPoint / focusedElement(命中点显式聚焦可编辑元素)
#include <WebCore/PlatformKeyboardEvent.h>   // Enter/退格 真键盘事件
#include <WebCore/ScriptController.h>        // frame->script().canExecuteScripts / executeScript(诊断 SPA)
#include <WebCore/DOMWrapperWorld.h>         // mainThreadNormalWorldSingleton()(WebCoreEvalJS)
#include <WebCore/PlatformMouseEvent.h>      // PlatformMouseEvent
#include <WebCore/MouseEventTypes.h>         // MouseButton / SyntheticClickType
#include <WebCore/ScrollView.h>              // setScrollPosition/maximumScrollPosition(LocalFrameView 基类)
#include <WebCore/DoublePoint.h>             // PlatformMouseEvent 的坐标类型
#include <wtf/MonotonicTime.h>               // PlatformMouseEvent 时间戳
#include <wtf/WallTime.h>                    // Apotheosis (M4 settle): CachedResource::apoRequestTimestamp()
#include <wtf/ApoLoadPhase.h>                // Apotheosis (M4 load): engine-thread phase buckets
#include <wtf/ApoLoadThrottle.h>             // Apotheosis (M4 load): DOM timer alignment while the pump runs
#include <wtf/OptionSet.h>                   // OptionSet<PlatformEvent::Modifier>
#include <WebCore/PlatformWheelEvent.h>      // PlatformWheelEvent(WebCoreWheelAt)
#include <WebCore/ScrollingCoordinatorTypes.h> // WheelEventProcessingSteps(full definition; EventHandler.h only forward-declares it)
#include <WebCore/RenderBox.h>               // RenderBox::canBeScrolledAndHasScrollableArea()(WebCoreIsScrollableAt)
#include <WebCore/RenderElement.h>           // RenderObject::parent()
#include <WebCore/ContainerNodeInlines.h>    // inline ContainerNode::renderer()(hit->renderer() in WebCoreIsScrollableAt)
#include <WebCore/HTMLIFrameElement.h>       // is<HTMLIFrameElement>(WebCoreIsScrollableAt)
#include <WebCore/ShadowRoot.h>              // ShadowRoot::mode()/elementFromPoint(WebCoreIsScrollableAt)
#include <WebCore/ShadowRootMode.h>          // ShadowRootMode::Open(同上)
#include <WebCore/EventTargetInlines.h>   // Node::hasEventListeners()(WebCoreWantsDragAt)
#include <WebCore/EventNames.h>           // eventNames().pointerdownEvent/... (WebCoreWantsDragAt)
#include <WebCore/HTMLCanvasElement.h>    // is<HTMLCanvasElement>(WebCoreWantsDragAt)
#include <WebCore/HTMLBodyElement.h>      // is<HTMLBodyElement>: stop the ancestor walk before <body>
#include <WebCore/StyleTouchAction.h>     // Style::TouchAction::isAuto/isManipulation
#include <WebCore/RenderObjectStyle.h>        // inline RenderObject::style()
#include <WebCore/RenderStyle+GettersInlines.h>  // RenderStyle::touchAction()(the umbrella header; the
                                              // RenderStyleProperties/ComputedStyleProperties inline files
                                              // it pulls in #error out if included directly)
#include <optional>
#include <algorithm>

// ---- curl TLS root-certificate injection (WebCoreSetCACertPath) ----
// App Container processes cannot reach the Windows system trust store, so we
// point curl/OpenSSL at a bundled Mozilla CA file (cacert.pem) instead.
#include <WebCore/CurlContext.h>            // CurlContext::singleton().sslHandle()
#include <WebCore/CurlSSLHandle.h>          // CurlSSLHandle::setCACertPath/setCACertData
#include <WebCore/CertificateInfo.h>        // CertificateInfo::Certificate == Vector<uint8_t>
#include <wtf/Vector.h>

// ---- M2 GPU 合成呈现(TextureMapper → ANGLE)----
// 把开合成后建出的 GraphicsLayerTextureMapper 图层树经 TextureMapper 合成到 GL:
//   - 离屏 BitmapTexture + glReadPixels → 复用现有 WriteableBitmap 通道(先验证合成像素正确);
//   - 或直呈现到 SwapChainPanel 窗口表面(eglSwapBuffers)。
// 仅当 g_gpuActive(WebCoreGpuInit 成功)时启用;否则纯软件 cairo(见 paintToRGBA 顶部分支)。
// ★ TextureMapper::create() 硬要求 GLContext::current()!=null(TextureMapper.cpp:216)——必须经 WebCore
//   的 GLContext/PlatformDisplay,不能用裸 EGL。GLContext::create(display, nativeWindow) 把窗口指针经
//   纯 C cast 直传 eglCreateWindowSurface(GLContext.cpp:170),正好喂 ANGLE.WindowsStore 的 PropertySet。
#define GL_GLEXT_PROTOTYPES 1               // 这版 ms-master ANGLE 的 gl2.h 把核心 GL 原型放此宏下(否则 glReadPixels/glViewport C3861)
#include <WebCore/PlatformDisplay.h>        // PlatformDisplay::sharedDisplay()(WIN→PlatformDisplayWin,起 ANGLE EGLDisplay)
#include <WebCore/GLContext.h>              // GLContext::create/createOffscreen + makeContextCurrent + swapBuffers
#include "texmap/TextureMapper.h"           // TextureMapper::create/beginPainting/endPainting(platform/graphics 已在 -I 上)
#include "texmap/TextureMapperLayer.h"      // TextureMapperLayer::paint/applyAnimationsRecursively
#include "texmap/GraphicsLayerTextureMapper.h" // 根 GraphicsLayer 实为它;.layer()/updateBackingStoreIncludingSubLayers
#include "texmap/BitmapTexture.h"           // 离屏渲染目标 + bindAsSurface
#include <WebCore/GraphicsLayer.h>          // GraphicsLayer(chrome->rootLayer() 返回类型,static_cast 基类)
#include <WebCore/RenderView.h>             // view->renderView()->compositor()
#include <WebCore/RenderLayerCompositor.h>  // compositor().frameViewDidScroll()(同步 TextureMapper 路径滚动)
#include <memory>                           // std::unique_ptr

// Installs the PlatformStrategies singleton (loader strategy = WebResourceLoadScheduler).
// Defined in port/PortPlatformStrategies.cpp. Idempotent.
extern void installPortPlatformStrategies();

namespace {

using namespace WebCore;

// Error codes returned through WebCoreRenderHtml.
enum : int {
    kOK              =  0,
    kErrBadArgs      = -1,
    kErrPageCreate   = -2,
    kErrNoMainFrame  = -3,
    kErrNoView       = -4,
    kErrNoLoader     = -5,
    kErrNoDocument   = -6,
    kErrCairoSurface = -7,
    kErrCairoContext = -8,
    kErrBadUrl       = -9,    // URL{url} parsed invalid
    kErrLoadFailed   = -10,   // terminal load state was a failure
    kErrLoadTimeout  = -11,   // watchdog fired before terminal state
    kErrNoSession    = -12,   // 交互调用时无常驻会话(需先 WebCoreSessionLoad)
    kErrBusy         = -13,   // 已在 pump 中(重入保护)
    kErrFrameGone    = -14,   // 交互后主帧消失(会话已坏)
};

// Apotheosis: public render surfaces are backed by ARM32 allocations or GPU
// textures. Bound dimensions before any area/byte multiplication so malformed
// C ABI input cannot wrap size_t or provoke an avoidable OOM.
static constexpr int kMaxSurfaceDimension = 4096;
static constexpr uint64_t kMaxSurfacePixels =
    static_cast<uint64_t>(kMaxSurfaceDimension) * static_cast<uint64_t>(kMaxSurfaceDimension);

static bool isValidSurfaceSize(int w, int h)
{
    if (w <= 0 || h <= 0 || w > kMaxSurfaceDimension || h > kMaxSurfaceDimension)
        return false;
    return static_cast<uint64_t>(w) * static_cast<uint64_t>(h) <= kMaxSurfacePixels;
}

// ---------------------------------------------------------------------------
// Apotheosis (MEMORY-PLAN.md §3 changes 3/4/6): memory-pressure state.
// The App Container cap is 1536 MB and the OS kills us at it with no exception and no dump,
// so the only defence is to shrink before we get there. The numbers come from the harness
// (MemoryManager.AppMemoryUsage / AppMemoryUsageLimit) and arrive through
// WebCoreSetMemoryPressure(); everything below runs on the single engine thread.
// ---------------------------------------------------------------------------
static constexpr unsigned kMB = 1024u * 1024u;
// MemoryCache budgets per level: (minDeadBytes, maxDeadBytes, totalBytes).
// Apotheosis: the unpressured budget was 0/16/32 MB, which is smaller than a single image-heavy
// viewport once the frames are decoded. On an image-heavy social timeline (200+ images) mem.txt showed dec=32 MB
// against cap=32 MB while the process sat at 35 % of the App Container limit: every insert pruned
// what the previous decode had just produced, so large images ping-ponged between the async decode
// queue (11b3bbd2b9) and the pruner and some never reached a paint at all. 32/64/128 MB is still a
// twelfth of the cap and leaves the decoded-bitmap ceiling where it belongs - with the texture pool
// and the tile budget (0c78243bf5, 8585f00e3f), not with a cache that was starving the decoder.
// minDeadBytes is what survives a prune, so it is non-zero only in the unpressured state; under
// pressure we still want every dead resource gone, and the two smaller budgets are unchanged.
static constexpr unsigned kMemCacheMinDeadNormal =  32u * kMB;
static constexpr unsigned kMemCacheDeadNormal   =  64u * kMB;
static constexpr unsigned kMemCacheTotalNormal  = 128u * kMB;
static constexpr unsigned kMemCacheDeadMedium   =  4u * kMB;
static constexpr unsigned kMemCacheTotalMedium  =  8u * kMB;
static constexpr unsigned kMemCacheDeadHigh     =  1u * kMB;
static constexpr unsigned kMemCacheTotalHigh    =  2u * kMB;

static int g_memPressureLevel = 0;              // last level pushed by the harness (0..2)
static unsigned g_memCacheCapacity = 0;         // total budget currently configured (for the stats log)

// setCapacities(minDeadBytes, maxDeadBytes, totalBytes) - see MemoryCache.h:124.
static void wkSetMemoryCacheCapacities(unsigned minDead, unsigned maxDead, unsigned total)
{
    WebCore::MemoryCache::singleton().setCapacities(minDead, maxDead, total);
    g_memCacheCapacity = total;
}

// The one place that actually gives memory back. level: 0 = nothing but a prune, 1 = the
// non-critical trim, 2 = the critical one (drops decoded data of *live* resources → visible
// images go white until they are re-decoded, and runs a synchronous full GC).
// keepResourceCache: keep the encoded resource cache. Yes on a navigation/tab switch, where
// the next page usually wants the same CSS/JS; No when the user closes the page for good.
// Engine thread only — releaseMemory() walks every Document and collects the JSC heap.
static void wkReleaseMemoryLevel(int level, bool keepResourceCache)
{
    using namespace WebCore;
    const auto maintainCache = keepResourceCache ? MaintainMemoryCache::Yes : MaintainMemoryCache::No;
    if (level <= 0) {
        // prune() is otherwise only ever reached from pruneSoon() on an insert, so a page that
        // just sits there never trims at all (MemoryCache.cpp:793).
        MemoryCache::singleton().prune();
        return;
    }
    // Critical::Yes also does WTF::releaseFastMallocFreeMemory() + GarbageCollectionController::
    // garbageCollectNow() (a synchronous full collection) inside releaseCriticalMemory();
    // Synchronous::Yes adds the per-thread fastMalloc cache flush. No need to duplicate either.
    releaseMemory(level >= 2 ? WTF::Critical::Yes : WTF::Critical::No,
                  WTF::Synchronous::Yes,
                  MaintainBackForwardCache::No,
                  maintainCache);
    if (level < 2)
        WTF::releaseFastMallocFreeMemory();   // Critical::No skips it; it is cheap and USE_SYSTEM_MALLOC fragments
}

// A synchronous full collection of the common VM, without releaseCriticalMemory()'s
// deleteAllCode(PreventCollectionAndDeleteAllCode). Used where we want the dead page's JS heap
// back but not to throw away every piece of JIT code the next page will have to compile again.
// No-op while no VM exists (commonVM() would create one just to collect it).
static void wkCollectJSCHeapNow()
{
    JSC::VM* vm = WebCore::g_commonVMOrNull;
    if (!vm)
        return;
    JSC::JSLockHolder locker(vm);
    vm->heap.collectNow(JSC::Synchronousness::Sync, JSC::CollectionScope::Full);
}

// Run the WebCore one-time process initialization exactly once.
// Sequence taken from Source/WebKit/Shared/WebKit2Initialize.cpp
// (the !PLATFORM(COCOA) branch — our case).
bool ensureWebCoreInitialized()
{
    static bool initialized = [] {
        // Apotheosis (M4): JSC reads JSC_* options from the environment during initialize().
        // Set them here (same CRT as the engine) rather than in the harness;
        // _putenv_s does not overwrite a value the tester set.
        //
        // JSC sizes its whole GC heuristic from Heap::m_ramSize, which on Windows is
        // GlobalMemoryStatusEx().ullTotalPhys (WTF/wtf/RAMSize.cpp) = the phone's ~3 GB of
        // *physical* RAM — not the ~1.5 GB our App Container may actually use. forceRAMSize
        // overrides that one input (Heap.cpp:330) and correctly scales everything derived
        // from it: minBytesPerCycle/minHeapSize, the growth mode, proportionalHeapSize
        // (Heap.cpp:2545) and m_maxEdenSizeWhenCritical, which is 25 % of the RAM above
        // criticalGCMemoryThreshold (Heap.cpp:462) — 25 MB at 512 MB instead of 153 MB at 3 GB.
        if (!std::getenv("JSC_forceRAMSize"))
            _putenv_s("JSC_forceRAMSize", "536870912");   // 512 MB
        // NOTE: do NOT set JSC_gcMaxHeapSize here. It is not a heap *cap*: when non-zero it
        // short-circuits Heap::collectIfNecessaryOrDefer's shouldRequestGC (Heap.cpp:2901-2906)
        // to "collect only once more than N bytes were allocated *this cycle*", bypassing the
        // proportional heuristic entirely. The 384 MB we used to set therefore made GC happen
        // *later*, not earlier — the opposite of what it was added for (see MEMORY-PLAN.md §3).
        JSC::initialize();                       // JSC heap/threading/options
        WTF::initializeMainThread();             // pins this thread as the WebKit main thread + RunLoop::main
        WebCore::initializeCommonAtomStrings();  // interns "auto", "all", content types, etc.
        installPortPlatformStrategies();         // PlatformStrategies (loader strategy) — required before any load
        // Apotheosis: 预开进程级 cookie jar(持久 SQLite;路径由 harness 在引擎线程更早的 SetupRuntimeEnv
        // 里经 WebCoreSetCookieJarPath 显式注入,见 PortNetworkStorageSession.cpp)+ 设接受策略
        // OnlyFromMainDocumentDomain(各端口惯例,挡第三方子资源 Set-Cookie)。打不开由 CookieJarDB::open()
        // 的 WK_WINUWP 补丁回退 :memory:,不崩。
        WebCorePort::ensureDefaultPortStorageSession();
        WebCore::populateJITOperations();        // no-op under ENABLE(C_LOOP) (header has inline {} fallback)
        // Apotheosis: 32 位低内存(Lumia)防 OOM —— 关后退页面缓存(整页 DOM+render 树极耗内存,
        // 是 32 位地址空间最大的隐性占用),资源缓存收紧上限。系统内存压力来时由 harness 经
        // WebCoreReleaseMemory() 主动放(WebCore::releaseMemory 一把清缓存 + JSC GC + 字体缓存)。
        WebCore::BackForwardCache::singleton().setMaxSize(0);
        // Apotheosis (MEMORY-PLAN.md §3 change 4): sized for the 1536 MB App Container cap.
        // 8/16 MB was too tight for repeat visits and bought nothing - encoded resources are a
        // rounding error next to the decoded bitmaps (§1). What actually bounds us is the
        // *decoded* data, and that needs the deletion interval below - and enough headroom that
        // the pruner does not eat the frames the decode queue has just produced (see the constants).
        wkSetMemoryCacheCapacities(kMemCacheMinDeadNormal, kMemCacheDeadNormal, kMemCacheTotalNormal);
        // Apotheosis: without this the interval is 0 and CachedResource::destroyDecodedDataIfNeeded()
        // returns immediately, so a client-less resource keeps its decoded bitmap until some
        // *insert* happens to trigger a prune - i.e. never, on a page that just sits there.
        // 5 s after the last client goes away is safe: nothing on screen references it.
        WebCore::MemoryCache::singleton().setDeadDecodedDataDeletionInterval(WTF::Seconds(5));
        // Apotheosis (MEMORY-PLAN.md §3 change 3): MemoryPressureHandler is never install()ed on
        // this port on purpose - its Windows poll (windowsMeasurementTimerFired) would reset the
        // status to Normal every 60 s, and the App Container has no CreateMemoryResourceNotification
        // anyway. We only push the status in from the harness (WebCoreSetMemoryPressure). Giving it
        // a low-memory handler makes the few in-engine paths that call releaseMemory() work.
        WTF::MemoryPressureHandler::singleton().setLowMemoryHandler([](WTF::Critical critical, WTF::Synchronous synchronous) {
            WebCore::releaseMemory(critical, synchronous,
                                   WebCore::MaintainBackForwardCache::No,
                                   WebCore::MaintainMemoryCache::No);
        });
        return true;
    }();
    return initialized;
}

} // anonymous namespace

// Apotheosis: 网络加载失败诊断通道。LoadingFrameLoaderClient 在 dispatchDidFail* 里
// 把真实的 ResourceError(curl 错误码 + 域 + 描述 + 失败 URL)记到这里;MainPage 在
// WebCoreLoadUrl 返回负值时取走写进 LocalFolder,便于真机失败定位(App Container 无
// 控制台/调试器输出通道)。单线程(WebKit 主线程)写,无需加锁。
static char g_lastNetError[512] = "";
static char g_lastDiag[4096] = "";   // 渲染诊断(URL/标题/内容尺寸/非白像素数 + 已缓存资源清单)
static char g_lastTitle[512] = "";   // 最近加载页面的标题(供历史/书签用)
static char g_lastUrl[1024] = "";    // 最近渲染文档的最终 URL(会话点击/导航后检测 URL 变化用)
static uint32_t g_lastFrameHash = 0; // 最近一帧像素哈希(实时模式判断画面是否变化 → 静止页自动停帧省电)
static int g_lastPendingResources = 0; // 最近文档仍在加载/未知状态的缓存资源数(防实时循环过早停)
extern "C" bool g_apoUaMobile = true;  // UA 开关:true=移动 iPhone(默认),false=桌面(LoadingFrameLoaderClient::userAgent 用)。extern "C" 跨命名空间一个符号
extern "C" char g_apoCustomUA[2048] = {0};  // 自定义 UA:非空则覆盖 mobile/desktop。WebCoreSetUserAgentString 设。
// Apotheosis (M4 load waterfall, 2026-09-07): "perf logging is armed", read by WebKit's
// CurlRequest::didReceiveHeader (WK_WINUWP) on the curl worker thread. Set together with
// g_perfOn in WebCoreSetPerfLogPath; off in the shipping default, so the per-response URL copy
// and main-thread hop that feed the waterfall cost nothing on a normal run.
extern "C" bool g_apoNetTimingOn = false;
// Apotheosis (PRIVACY-AUDIT.md recommended action 4): speculation-rules prefetch.
//   WebCore defaults speculationRulesPrefetchEnabled to true, so a page's
//   <script type="speculationrules"> may issue full requests for URLs the user never clicked.
//   Off unless the harness turns it on (Settings -> PRIVACY: Off / Wi-Fi only / Always).
//   WebCoreSetSpeculativePrefetch() sets it; every Page created afterwards reads it.
static bool g_apoSpecPrefetch = false;
// Apotheosis (M4): DNS warm-up, implemented in WebKit\Source\WebKitLegacy\WebCoreSupport\
// WebResourceLoadScheduler.cpp, which is compiled straight into the driver
// (local\link-driver-gpu.ps1) - a plain cross-TU call, like the one in
// LoadingFrameLoaderClient::prefetchDNS(). Used by WebCorePreconnect().
extern void apotheosisPrefetchDNS(const WTF::String& hostname);
static char g_spaProbe[512] = "";     // SPA 模块求值探针结果(诊断 <script type=module> 是否求值/抛错)
static std::vector<uint8_t> g_caBytes;  // CA 根证书字节副本,供 WebCoreDownload 的独立 curl 句柄用

// GPU 合成是否就绪:仅当 WebCoreGpuInit 成功建好 GL 上下文 + TextureMapper 后才置 true。
// 严格 gate 开合成的两个开关 + PortChromeClient——GPU 未起时走纯软件 cairo(老设备/未起 GPU 的
// 通用稳定底座,零回归)。无条件开合成是 0.1.7.1 真机闪退的根因。
static bool g_gpuActive = false;
// M2 GPU 状态(只在唯一引擎线程访问;WebCoreGpuInit 建,故意不析构=随进程存活,避免退出时跨线程 eglDestroy)。
static WebCore::GLContext* g_glContext = nullptr;
static WebCore::TextureMapper* g_textureMapper = nullptr;
static int g_gpuW = 0, g_gpuH = 0;
// 离屏 readback 的方向校正:对 glReadPixels(自下而上)结果可选水平/垂直翻转。真机朝向(TextureMapper
// 离屏渲染 + FBO 读回的净朝向)经验未定 → 运行时可调(WebCoreGpuSetFlip),harness 点 GPU 按钮循环
// 4 种组合(none/H/V/HV)找对的那个。默认 H(=把 bottom-up 读到的再水平镜像,纯 180° 解释下的校正)。
static bool g_gpuFlipH = false;   // 反转列;真机实测无翻转(GPU·-)即正确,默认 false
static bool g_gpuFlipV = false;   // 反转行;同上(仍可经 WebCoreGpuSetFlip 调,harness GPU 按钮循环)
static int g_lastContentPx = 0;   // 最近一次 GPU readback 中"与背景色不同"的像素数(诊断:内容是否真合成进来)
static bool g_gpuScrollFast = false;  // 置位时本次合成跳过 forceDirtyTree(滚动快路径,见 gpuPrepare)
// Apotheosis: the pan present handshake is gone. It let a gesture take
// the presents away from the engine (defer eglSwapBuffers, release each frame by hand) so that the
// engine's own composites could not race the XAML TranslateTransform the harness drew the pan with.
// That preview is deleted, and with it the only caller WebCoreSetPanGesture(1) ever had: on the
// shipping default (instantpanxaml OFF since package 13) the harness never armed the mode, so every
// flag, the owed-swap ledger and the scroll generation that dated it were provably unreachable.
// Presents are unconditional again. If a future gesture path wants the engine to hold frames back,
// take the mechanism from the history of this file rather than from a flag nothing sets.
// Apotheosis (drag as pointer events): a mousedown WebCoreDragAt() dispatched and the page
// consumed is still in flight — moves/releases only reach the page while this is set, and
// teardownSession() clears it. Engine thread only, like every other flag here.
static bool g_dragActive = false;
// Apotheosis (map tap, 2026-09-06): where that press landed, and whether the finger has since
// travelled far enough that the gesture stopped being a tap. A touch pan must NOT end in a click:
// on the map site every pan ended with a click on the map canvas, which is the site's own
// "toggle the full-screen map view" action - the reported "a tap/drag flips the map view".
// (A mouse drag inside one element does fire a click in a desktop browser; a touch pan never does,
// and every gesture that reaches WebCoreDragAt came from a finger.) Engine thread only.
static int g_dragPressX = 0;
static int g_dragPressY = 0;
static bool g_dragMoved = false;
static const int kDragTapSlopPx = 8;   // engine px (~4 DIP at 720 over a 360 DIP wide content area)
static bool g_gpuPresentMode = false; // WebCoreGpuInit 收到窗口表面=true → 各帧直呈现到 SwapChainPanel(省 readback)
static bool g_gpuAnimating = false;   // 最近一次合成时图层树仍有动画在跑(applyAnimationsRecursively 返回值),直呈现模式的帧变化信号之一

// Apotheosis (WHITE-AT-SCROLL-END, 2026-09-04; docs/TILEGRID-DESIGN.md section 3): a
// composite can come up with no content at all.
//
// Who produces such a composite: every live tick takes the scroll fast path (WebCoreLiveTick sets
// g_gpuScrollFast unconditionally), so gpuPrepare skips forceDirtyTree and the composite is drawn
// entirely from the tiles the backing stores still hold. When those tiles are gone - dropped
// outside the cover rect while the gesture moved the visible rect, recycled on a scale change, or
// still owed by a raster worker - the tree paints nothing at all and the whole screen is the clear
// colour.
//
// The repair ledger that used to live here - gpuArmRepair(), gpuNoteRepairResult(),
// g_gpuRepairMisses, the escalation cooldown and the `repairloop` stage line - is gone with the v1
// mechanics it read. The TileGrid model answers one question
// after a composite instead: how many VISIBLE cells drew nothing and had no backdrop behind them
// (wkWinUWPTexmapVisibleHoles). A cell can only be in that state while work the model has already
// scheduled is running, and R5/R12 guarantee that work converges, so the only thing missing is a
// frame to show the result in - exactly one more composite, no dirtying. See gpuPresent().
static bool g_gpuLastCompositeFull = false;   // gpuPrepare force-dirtied the tree this composite
static bool g_gpuForceFullNext = false;       // next composite must force-dirty whatever the caller asks
static bool g_gpuTargetedNext = false;        // next composite must NOT force-dirty: per-layer detection decides

// ---------------------------------------------------------------------------
// Apotheosis (THREADED-COMPOSITOR-PLAN.md C5): event-driven present.
//
// The harness used to poll us with a fixed 200 ms DispatcherTimer because the C ABI had no way
// of saying "something changed". It now registers a wake callback here; every invalidation
// (PortChromeClient::scheduleRenderingUpdate / triggerRenderingUpdate / setNeedsOneShotDrawing-
// Synchronization / didFinishLoadingImageForElement, the raster-completion hook, and the tick's
// own "still dirty when it finished" state) funnels into presentRequested().
//
// Contract with the harness: the callback may run on ANY thread (engine thread for the WebCore
// hooks, a raster worker for the completion hook), must not block and must not call back into
// the engine - all it may do is post to a queue. See MainPage::PresentWakeThunk.
//
// Rate control lives on both sides: here an atomic arms the callback so a burst of invalidations
// produces exactly one wake-up (disarmed again at the top of WebCoreLiveTick, i.e. once a
// composite is actually under way, so anything raised during that composite arms the next one);
// in the harness a >= ~16 ms gap between presents. g_presentWakes counts the wake-ups fired
// since the last tick and becomes the perf row's wake_count column.
// ---------------------------------------------------------------------------
static std::atomic<void (*)(void*)> g_presentCb { nullptr };
static std::atomic<void*> g_presentCbCtx { nullptr };
static std::atomic<int> g_presentWakeArmed { 0 };
static std::atomic<unsigned> g_presentWakes { 0 };

// Apotheosis (M4 load throttle): while the main document is loading, every arriving stylesheet,
// script and image invalidates layout, and each invalidation used to become its own wake-up and
// therefore a full composite on the engine thread - the same thread the parser, the scripts and
// the image decodes run on. During a load those composites show a half-built page nobody is
// looking at yet, and they cost more than the work they display. So between
// dispatchDidStartProvisionalLoad and the load event, engine-initiated presents are limited to
// one per 250 ms. Exempt: the first visually-non-empty layout (the frame the user is waiting
// for) goes out immediately. Untouched: scroll, pinch and pan presents, which do not travel
// through presentRequested() at all, and a suppressed wake is never lost - the next allowed one
// (or navLoadEnd(), at the load event) delivers it.
static std::atomic<int> g_navLoading { 0 };             // provisional start .. load event
static std::atomic<int> g_navWakeSuppressed { 0 };      // a wake was dropped by the throttle
static std::atomic<int> g_navFirstPaintPending { 0 };   // DidFirstVisuallyNonEmptyLayout, not yet shown
static std::atomic<double> g_navWakeLastSec { 0 };      // MonotonicTime of the last wake let through
static std::atomic<double> g_navLoadStartSec { 0 };
static double g_navTickCompositeSec = 0;                // engine thread only (WebCoreLiveTick)
static constexpr double kNavWakeIntervalSec = 0.25;
// Safety valve: a provisional load that never reaches a load event and never reaches a pump
// (an SPA navigation started from a timer, say) must not throttle presents forever.
static constexpr double kNavThrottleMaxSec = 20.0;

static double monotonicSeconds()
{
    return MonotonicTime::now().secondsSinceEpoch().value();
}

static bool navLoadThrottleActive()
{
    if (!g_navLoading.load(std::memory_order_acquire))
        return false;
    if (g_navFirstPaintPending.load(std::memory_order_acquire))
        return false;   // the first readable frame is never held back
    return monotonicSeconds() - g_navLoadStartSec.load(std::memory_order_relaxed) < kNavThrottleMaxSec;
}

namespace WebCorePort {

void presentRequested()
{
    void (*cb)(void*) = g_presentCb.load(std::memory_order_acquire);
    if (!cb)
        return;   // fixed-tick mode: nobody registered, the flag alone does the work
    if (navLoadThrottleActive()) {
        const double now = monotonicSeconds();
        if (now - g_navWakeLastSec.load(std::memory_order_relaxed) < kNavWakeIntervalSec) {
            g_navWakeSuppressed.store(1, std::memory_order_release);
            return;   // covered by the next allowed wake, or by navLoadEnd()
        }
        g_navWakeLastSec.store(now, std::memory_order_relaxed);
    }
    int expected = 0;
    if (!g_presentWakeArmed.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
        return;   // a wake is already on its way; it will cover this request too
    g_navWakeSuppressed.store(0, std::memory_order_release);
    g_presentWakes.fetch_add(1, std::memory_order_relaxed);
    cb(g_presentCbCtx.load(std::memory_order_acquire));
}

} // namespace WebCorePort

// Apotheosis (M4 load throttle): the main document started / finished loading. navLoadEnd() also
// hands over a wake the throttle swallowed, so the finished page is presented even if nothing
// else invalidates afterwards. Both are engine-thread only.
static void navLoadBegin()
{
    g_navLoadStartSec.store(monotonicSeconds(), std::memory_order_relaxed);
    g_navFirstPaintPending.store(0, std::memory_order_release);
    g_navLoading.store(1, std::memory_order_release);
}

static void navLoadEnd()
{
    if (!g_navLoading.exchange(0, std::memory_order_acq_rel))
        return;
    g_navFirstPaintPending.store(0, std::memory_order_release);
    if (g_navWakeSuppressed.exchange(0, std::memory_order_acq_rel))
        WebCorePort::presentRequested();
}

// Called at the top of every WebCoreLiveTick: the composite the last wake asked for is happening
// now, so the next invalidation must be able to arm a new one.
static void presentWakeDisarm()
{
    g_presentWakeArmed.store(0, std::memory_order_release);
}


// 子资源加载诊断计数(主文档 + CSS/JS/图片全经 ResourceHandle 桥)。由 ResourceHandle.cpp
// 的 WebCorePortBumpLoad 累加;在 WebCoreLoadUrl 开头清零,结束并入 g_lastDiag,真机定位"子资源不加载"。
static int g_loadStarted = 0, g_loadResponse = 0, g_loadComplete = 0, g_loadFail = 0;
// Apotheosis (M4 load timeline): <script> elements in the document at the last writeDiag()
// - the cheapest proxy for "how much script did this page bring" (this tree has no counter of
// executed scripts, and adding one would mean a hook in JSC). Read by perfEnd() into n_scripts.
static int g_lastScriptCount = -1;
extern "C" void WebCorePortBumpLoad(int kind)
{
    switch (kind) {
    case 0: ++g_loadStarted; break;
    case 1: ++g_loadResponse; break;
    case 2: ++g_loadComplete; break;
    case 3: ++g_loadFail; break;
    }
}

// ---- 网页链接命中表(点击交互的基础)----------------------------------------
// 渲染后提取页面上所有 <a href> 的视口矩形(=位图坐标,因 scroll=0)+ 绝对 URL,存表返回给 UI。
// UI 在点击时自行判断点中哪个矩形 → 导航。无需常驻 WebCore 会话、点击时不调引擎,安全。
struct LinkRect { int x, y, w, h; std::string url; };
static std::vector<LinkRect> g_links;
static std::string g_imeDiag;   // 最近一次 WebCoreTypeText 的可编辑/聚焦/插入诊断(WebCoreEditDebug 读)
// 页内查找:记住上次查找词 + 基础选项,供 WebCoreFindNext 不重新标记直接换下一个。
static WTF::String g_findText;
static WebCore::FindOptions g_findOpts;

static int countPendingResources(WebCore::Document& document)
{
    int pending = 0;
    for (auto& kv : document.cachedResourceLoader().allCachedResources()) {
        WebCore::CachedResource* res = kv.value.get();
        if (!res)
            continue;
        auto status = res->status();
        if (status == WebCore::CachedResource::Unknown || status == WebCore::CachedResource::Pending)
            ++pending;
    }
    return pending;
}

static void extractLinks(WebCore::Document* document, int renderH)
{
    // 注意:驱动以 -fno-exceptions 编译,不能用 try/catch;靠空判断保证安全。
    g_links.clear();
    if (!document)
        return;
    Ref<WebCore::HTMLCollection> links = document->links();
    unsigned n = links->length();
    for (unsigned i = 0; i < n && g_links.size() < 4000; ++i) {
        WebCore::Element* el = links->item(i);
        if (!el || !is<WebCore::HTMLAnchorElement>(*el))
            continue;
        auto href = downcast<WebCore::HTMLAnchorElement>(*el).href();
        if (href.isEmpty() || !href.isValid() || !href.protocolIsInHTTPFamily())
            continue;   // 只收 http(s) 可导航链接(跳过 javascript:/#fragment/mailto 等)
        // 视口坐标矩形(=位图坐标,因 scroll=0)。已在 caller 做过 forced layout,故 boundingClientRect 便宜。
        WebCore::FloatRect r = el->boundingClientRect();
        if (r.width() <= 0 || r.height() <= 0)
            continue;
        if (r.maxY() < 0 || r.y() > static_cast<float>(renderH))
            continue;   // 只收落在已渲染视口内的链接(屏外的点不到)
        LinkRect lr;
        lr.x = static_cast<int>(r.x());
        lr.y = static_cast<int>(r.y());
        lr.w = static_cast<int>(r.width());
        lr.h = static_cast<int>(r.height());
        lr.url = href.string().utf8().data();
        g_links.push_back(std::move(lr));
    }
}

static size_t webcoreDownloadWrite(void* ptr, size_t size, size_t nmemb, void* stream)
{
    return std::fwrite(ptr, size, nmemb, static_cast<FILE*>(stream));
}

// ============================================================================
// 常驻交互会话(live interactive session)
// 一次性渲染 → 常驻 Page:点击(EventHandler 派发真实鼠标事件,触发链接导航/表单提交/按钮
// onclick/SPA 交互)、滚动(isolatedUpdateRendering 驱动 IntersectionObserver 加载下方/懒加载
// 图片)后重新布局并重绘同一个活文档。所有调用必须串行在唯一引擎线程(WTF 主线程)。
// 关键风险见下:① 晚到加载回调的 use-after-free(teardown 顺序);② 提交后 view 被重建(每次重取);
// ③ pump 重入(g_inPump);④ 懒加载无 isolatedUpdateRendering 则永不触发。
// ============================================================================
struct DriverLoadState {
    bool mainDone = false;   // 主文档完成(成功或失败),由完成回调置位
    bool failed   = false;
    // Apotheosis (M3): the main document reached dispatchDidCommitLoad (set from
    // perfNavCommit, the driver's only commit observer — LoadingFrameLoaderClient
    // exists for the main frame only, createFrame() returns nullptr). Lets the load
    // path tell "committed but no load event" (ImageDocument / deferred-load bug →
    // render what we have) from "nothing ever arrived" (a real timeout).
    bool committed = false;
};

struct Session {
    RefPtr<WebCore::Page> page;            // 稳定根;frame/view/document 每次从它重取
    RefPtr<WebCore::LocalFrame> mainFrame; // 同帧导航间稳定;跨导航 view 会被重建
    WebCorePort::LoadingFrameLoaderClient* client = nullptr; // 原始指针,建会话时捕获,teardown 置空回调用
    WebCorePort::PortChromeClient* chrome = nullptr;          // 原始指针(Page 持有 UniqueRef);读根图层/present 标志(GPU 合成)
    int w = 0, h = 0;
    DriverLoadState load;                  // 堆上(随会话存活):晚到的 didFinishLoad 不会 deref 已释放栈
};
static std::optional<Session> g_session;
static bool g_inPump = false;              // settle 轮询 / 事件派发的重入保护

// 复位 g_inPump 的作用域守卫(无异常环境下,析构在正常返回路径也会执行)。
struct PumpGuard { ~PumpGuard() { g_inPump = false; } };

// ==================== crash.txt: last-resort crash reporting =================
// Apotheosis: on Windows 10 Mobile WER writes no dump for the way this engine
// dies (WTF's CRASH() is std::abort(), whose UWP CRT tail is __fastfail, and
// WTFBreakpointTrap() is a `bkpt #0` trap) — the app just vanishes, which is
// exactly what a news site does on a Lumia 950. So the driver writes the crashing
// stack itself, appended to LocalState\crash.txt, from three independent
// sources that all funnel into crashLogWrite():
//   (a) the WTF crash hook (Source/WTF/wtf/Assertions.cpp, WK_WINUWP) — fires
//       *before* the trap, so the captured stack is the crashing stack and the
//       reason carries file/line/assertion;
//   (b) a vectored exception handler (first in the chain) for the fatal codes —
//       catches AVs and traps that never reach WTF at all;
//   (c) signal(SIGABRT) — the tail of abort() before the CRT fast-fails.
// Constraints on the crash path: no heap allocation beyond fopen's, no C++
// exceptions (_HAS_EXCEPTIONS=0), no DbgHelp and no SetUnhandledExceptionFilter
// (neither exists in the App Container partition), re-entrancy guarded, and a
// hard cap on entries so a repeating first-chance AV cannot fill LocalState.
// The engine and driver are statically linked into Harness.exe, so nearly every
// frame is an RVA into Harness.exe — resolve them offline against its map/pdb.

// AddVectoredExceptionHandler is declared DESKTOP-only in the 10.0.22621 SDK
// headers, but the import is present in WindowsApp.lib and the API works inside
// the App Container, so declare it here instead of widening WINAPI_FAMILY.
extern "C" WINBASEAPI PVOID WINAPI AddVectoredExceptionHandler(ULONG First, PVECTORED_EXCEPTION_HANDLER Handler);

// Installed into WTF by WebCoreSetCrashLogPath(). Defined in
// Source/WTF/wtf/Assertions.cpp under WK_WINUWP; declared here on purpose so no
// WTF header changes (a header edit would rebuild the whole engine). Plain C++
// linkage at global scope — the definition must match this exactly.
extern void WTFWinUWPSetCrashHook(void (*hook)(const char* reason));

static char g_crashLogPath[512] = { 0 };
static bool g_crashLogInstalled = false;
static bool g_crashLogInProgress = false;   // re-entrancy guard (crash while logging)
static int  g_crashLogEntries = 0;          // hard cap, see kMaxCrashLogEntries
static const int kMaxCrashLogEntries = 16;

// Apotheosis (2026-09-03): a device abort left only "signal 22 (SIGABRT)" and the
// handler frame — abort() reached us without going through WTFCrash, so the reason
// was invisible. Everything that can abort *behind* WTF's back now gets its own
// leg, each writing a distinct `reason:` line before falling through:
//   (d) std::terminate      — uncaught exception, or ANGLE's RunOnUIThread timeout
//                             (the thread rule in CLAUDE.md) reaching our CRT;
//   (e) _set_new_handler    — operator new failed: address-space/commit exhaustion,
//                             which with _HAS_EXCEPTIONS=0 aborts with no message;
//   (f) invalid parameter   — CRT contract violation (bad handle, bad printf, ...);
//   (g) purecall            — call through a partially destroyed object's vtable.
// All four are CRT-level, so they see aborts the WTF hook and the VEH never do.
static void perfFlush();   // fwd decl: drain the perf ring before we die (see (3) below)
static void consoleFlush(); // fwd decl: drain the console.txt ring before we die, same reason

// Memory numbers for the OOM-shaped legs. GlobalMemoryStatusEx is APP-partition and
// comes from WindowsApp.lib; K32GetProcessMemoryInfo is APP-partition too but lives in
// a psapi apiset we do not otherwise link, so resolve it once at install time and keep
// the pointer — never call GetProcAddress from inside a crash handler.
struct CrashProcessMemoryCounters {   // layout of PROCESS_MEMORY_COUNTERS (psapi.h)
    DWORD cb;
    DWORD pageFaultCount;
    SIZE_T peakWorkingSetSize;
    SIZE_T workingSetSize;
    SIZE_T quotaPeakPagedPoolUsage;
    SIZE_T quotaPagedPoolUsage;
    SIZE_T quotaPeakNonPagedPoolUsage;
    SIZE_T quotaNonPagedPoolUsage;
    SIZE_T pagefileUsage;
    SIZE_T peakPagefileUsage;
};
using CrashGetProcessMemoryInfoFn = BOOL (WINAPI*)(HANDLE, CrashProcessMemoryCounters*, DWORD);
static CrashGetProcessMemoryInfoFn g_crashGetProcessMemoryInfo = nullptr;

// Driver and harness are both /MD, so they share one vcruntime and one set of CRT
// handler slots: whoever installs last wins. The harness installs its own terminate
// handler in App() (it has exceptions and can describe the exception object; we cannot),
// and SetupRuntimeEnv arms us afterwards — so keep the previous handlers and call them
// once we have logged. Both reasons then appear in crash.txt, ours with the stack.
static std::terminate_handler g_crashPrevTerminate = nullptr;
static _invalid_parameter_handler g_crashPrevInvalidParameter = nullptr;
static _purecall_handler g_crashPrevPureCall = nullptr;

// "ws=..M priv=..M availVirt=..M load=..%" — appended to the reason of the OOM-shaped
// legs so an abort can be told apart from a genuine logic failure at a glance. Never
// allocates; every value that cannot be had is simply left out.
static void crashLogMemorySuffix(char* out, size_t outSize)
{
    out[0] = 0;
    int off = 0;
    if (g_crashGetProcessMemoryInfo) {
        CrashProcessMemoryCounters pmc = { };
        pmc.cb = sizeof(pmc);
        if (g_crashGetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
            off += std::snprintf(out + off, outSize - off, " ws=%lluM commit=%lluM",
                static_cast<unsigned long long>(pmc.workingSetSize >> 20),
                static_cast<unsigned long long>(pmc.pagefileUsage >> 20));
    }
    if (off >= static_cast<int>(outSize) - 1)
        return;
    MEMORYSTATUSEX ms = { };
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms))
        std::snprintf(out + off, outSize - off, " availPhys=%lluM availVirt=%lluM load=%lu%%",
            static_cast<unsigned long long>(ms.ullAvailPhys >> 20),
            static_cast<unsigned long long>(ms.ullAvailVirtual >> 20),
            static_cast<unsigned long>(ms.dwMemoryLoad));
}

// Basename of a module path, ASCII-folded into `out` (module names are ASCII).
static void crashLogModuleName(HMODULE module, char* out, size_t outSize)
{
    out[0] = 0;
    if (!module || outSize < 2)
        return;
    wchar_t wide[MAX_PATH];
    DWORD n = GetModuleFileNameW(module, wide, MAX_PATH);
    if (!n)
        return;
    wide[MAX_PATH - 1] = 0;
    const wchar_t* base = wide;
    for (const wchar_t* p = wide; *p; ++p) {
        if (*p == L'\\' || *p == L'/')
            base = p + 1;
    }
    size_t i = 0;
    for (; base[i] && i + 1 < outSize; ++i)
        out[i] = (base[i] < 128) ? static_cast<char>(base[i]) : '?';
    out[i] = 0;
}

// SizeOfImage straight out of the mapped PE headers (no DbgHelp in App Container).
static DWORD crashLogModuleSize(HMODULE module)
{
    if (!module)
        return 0;
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const uint8_t*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;
    return nt->OptionalHeader.SizeOfImage;
}

// The one writer. `ctxOrNull` is the vectored handler's CONTEXT when we have one.
static void crashLogWrite(const char* reason, const CONTEXT* ctxOrNull)
{
    if (!g_crashLogPath[0] || g_crashLogInProgress || g_crashLogEntries >= kMaxCrashLogEntries)
        return;
    g_crashLogInProgress = true;
    ++g_crashLogEntries;

    FILE* fp = nullptr;
    if (fopen_s(&fp, g_crashLogPath, "ab") != 0 || !fp) {
        g_crashLogInProgress = false;
        return;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    std::fprintf(fp, "\n==== crash %04u-%02u-%02u %02u:%02u:%02u.%03u tid=%lu ====\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        static_cast<unsigned long>(GetCurrentThreadId()));
    std::fprintf(fp, "reason: %s\n", reason ? reason : "(none)");

    // Image base/size of the host exe first, so every RVA below can be matched
    // offline against the exact Harness.exe that produced this file.
    HMODULE exe = GetModuleHandleW(nullptr);
    char exeName[64];
    crashLogModuleName(exe, exeName, sizeof(exeName));
    std::fprintf(fp, "module %s base=0x%08llx size=0x%08lx\n",
        exeName[0] ? exeName : "?",
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(exe)),
        static_cast<unsigned long>(crashLogModuleSize(exe)));

#if defined(_M_ARM) || defined(_ARM_)
    if (ctxOrNull) {
        std::fprintf(fp, "context: pc=0x%08lx lr=0x%08lx sp=0x%08lx r0=0x%08lx r1=0x%08lx\n",
            static_cast<unsigned long>(ctxOrNull->Pc), static_cast<unsigned long>(ctxOrNull->Lr),
            static_cast<unsigned long>(ctxOrNull->Sp), static_cast<unsigned long>(ctxOrNull->R0),
            static_cast<unsigned long>(ctxOrNull->R1));
    }
#else
    (void)ctxOrNull;
#endif

    void* frames[48] = { nullptr };
    USHORT captured = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
    // Apotheosis: say how many frames the unwinder actually produced. On ARM32 Thumb
    // it regularly returns 1 (no unwind data for the CRT's abort tail), and a file
    // with a single frame must be readable as "the unwinder failed", not "shallow stack".
    std::fprintf(fp, "stack: captured=%u\n", static_cast<unsigned>(captured));
    for (USHORT i = 0; i < captured; ++i) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(frames[i]);
        HMODULE mod = nullptr;
        char name[64] = { 0 };
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(frames[i]), &mod) && mod) {
            crashLogModuleName(mod, name, sizeof(name));
            std::fprintf(fp, "frame %2u: %s +0x%08llx\n", static_cast<unsigned>(i),
                name[0] ? name : "?",
                static_cast<unsigned long long>(addr - reinterpret_cast<uintptr_t>(mod)));
        } else
            std::fprintf(fp, "frame %2u: ? 0x%08llx\n", static_cast<unsigned>(i),
                static_cast<unsigned long long>(addr));
    }

    // Apotheosis: unwinder fallback. When RtlCaptureStackBackTrace gives us almost
    // nothing (the ARM32 abort tail), raw-scan this thread's stack for words that look
    // like Thumb return addresses inside the host image and log them as candidates.
    // Noisy by construction — stale frames survive on the stack — but "cand" lines feed
    // local\symbolize-crash.ps1 exactly like "frame" lines and usually contain the real
    // caller. Bounded scan, no allocation, VirtualQuery for the stack extent.
    if (captured < 4 && exe) {
        const uintptr_t exeBase = reinterpret_cast<uintptr_t>(exe);
        const uintptr_t exeEnd = exeBase + crashLogModuleSize(exe);
        volatile uintptr_t probe = 0;
        const uintptr_t here = reinterpret_cast<uintptr_t>(const_cast<uintptr_t*>(&probe));
        MEMORY_BASIC_INFORMATION mbi = { };
        if (crashLogModuleSize(exe) && VirtualQuery(reinterpret_cast<const void*>(here), &mbi, sizeof(mbi))) {
            uintptr_t top = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (top > here + 16384)
                top = here + 16384;          // 4k words is plenty and keeps the file small
            int found = 0;
            for (uintptr_t p = here; p + sizeof(uintptr_t) <= top && found < 24; p += sizeof(uintptr_t)) {
                const uintptr_t v = *reinterpret_cast<const uintptr_t*>(p);
                if (v <= exeBase || v >= exeEnd || !(v & 1))
                    continue;                // Thumb return addresses have bit 0 set
                std::fprintf(fp, "cand %2d: %s +0x%08llx\n", found, exeName[0] ? exeName : "?",
                    static_cast<unsigned long long>((v & ~static_cast<uintptr_t>(1)) - exeBase));
                ++found;
            }
        }
    }

    std::fflush(fp);
    std::fclose(fp);
    g_crashLogInProgress = false;
}

// (a) WTF hook: reason already carries file/line/assertion or reason/misc values.
static void crashLogWtfHook(const char* reason)
{
    char buffer[832];
    std::snprintf(buffer, sizeof(buffer), "WTF %s", reason ? reason : "(none)");
    crashLogWrite(buffer, nullptr);
}

// (b) Vectored handler, first in the chain. Logs only the terminal codes and
// always returns EXCEPTION_CONTINUE_SEARCH — we observe, we never swallow.
static LONG NTAPI crashLogVectoredHandler(EXCEPTION_POINTERS* info)
{
    if (!info || !info->ExceptionRecord)
        return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_BREAKPOINT:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case 0xC00000FDu:   // STATUS_STACK_OVERFLOW
    case 0xC0000409u:   // STATUS_STACK_BUFFER_OVERRUN — what __fastfail raises
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
    char reason[200];
    // For access violations ExceptionInformation[0] is 0 = read, 1 = write, 8 = execute
    // (DEP/no-execute page) and [1] the faulting address - tells a non-executable JIT
    // pool apart from a bad pointer inside JIT code.
    const auto* rec = info->ExceptionRecord;
    std::snprintf(reason, sizeof(reason), "SEH code=0x%08lx address=0x%08llx access=%lu fault=0x%08llx",
        static_cast<unsigned long>(code),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(rec->ExceptionAddress)),
        rec->NumberParameters > 0 ? static_cast<unsigned long>(rec->ExceptionInformation[0]) : 99ul,
        rec->NumberParameters > 1 ? static_cast<unsigned long long>(rec->ExceptionInformation[1]) : 0ull);
    crashLogWrite(reason, info->ContextRecord);
    // For an execute fault (access=8) the decisive question is what protection the page
    // at the faulting address actually has: VirtualQuery is App-Container-safe and the
    // JIT pool is the prime suspect (W^X commit path in OSAllocatorWin.cpp).
    if (rec->NumberParameters > 1) {
        MEMORY_BASIC_INFORMATION mbi = { };
        if (VirtualQuery(reinterpret_cast<const void*>(rec->ExceptionInformation[1]), &mbi, sizeof(mbi))) {
            char extra[200];
            std::snprintf(extra, sizeof(extra), "fault page: base=0x%08llx size=0x%08llx state=0x%lx protect=0x%lx allocProtect=0x%lx type=0x%lx",
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(mbi.BaseAddress)),
                static_cast<unsigned long long>(mbi.RegionSize),
                static_cast<unsigned long>(mbi.State), static_cast<unsigned long>(mbi.Protect),
                static_cast<unsigned long>(mbi.AllocationProtect), static_cast<unsigned long>(mbi.Type));
            crashLogWrite(extra, nullptr);
        }
        // Which modules do pc and lr point into? (a pc outside Harness.exe with a DLL name
        // beats a bare address - the ANGLE DLLs are the usual suspects for present/resize races)
        {
            const uintptr_t regs[2] = { static_cast<uintptr_t>(info->ContextRecord->Pc), static_cast<uintptr_t>(info->ContextRecord->Lr) };
            char where[200]; int off = 0;
            for (int k = 0; k < 2; ++k) {
                HMODULE mod = nullptr; char name[64] = { 0 };
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(regs[k]), &mod) && mod) {
                    crashLogModuleName(mod, name, sizeof(name));
                    off += std::snprintf(where + off, sizeof(where) - off, "%s in %s +0x%08llx  ", k ? "lr" : "pc", name[0] ? name : "?", static_cast<unsigned long long>(regs[k] - reinterpret_cast<uintptr_t>(mod)));
                } else
                    off += std::snprintf(where + off, sizeof(where) - off, "%s in no module  ", k ? "lr" : "pc");
                if (off >= static_cast<int>(sizeof(where)) - 1) break;
            }
            crashLogWrite(where, nullptr);
        }
        // Is the fault inside JSC's fixed executable pool? (W^X commit gap vs. stray jump)
        char pool[120];
        std::snprintf(pool, sizeof(pool), "jit pool: [0x%08llx, 0x%08llx) isJITPC(fault)=%d",
            static_cast<unsigned long long>(JSC::startOfFixedExecutableMemoryPool<uintptr_t>()),
            static_cast<unsigned long long>(JSC::endOfFixedExecutableMemoryPool<uintptr_t>()),
            JSC::isJITPC(reinterpret_cast<void*>(rec->ExceptionInformation[1])) ? 1 : 0);
        crashLogWrite(pool, nullptr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// (c) abort() tail. Returns; the CRT then fast-fails as usual.
// The perf ring is drained first: abort() is the one exit where the rows leading up to
// the crash (the `scale` rows of a pinch, say) would otherwise be lost with the process.
static void __cdecl crashLogSignalHandler(int sig)
{
    char reason[128];
    char mem[160];
    crashLogMemorySuffix(mem, sizeof mem);
    std::snprintf(reason, sizeof(reason), "signal %d (SIGABRT)%s", sig, mem);
    crashLogWrite(reason, nullptr);
    perfFlush();
    consoleFlush();
}

// (d) std::terminate. Reached by an uncaught exception, a noexcept violation, or a
// terminate() call — on this port most plausibly ANGLE's RunOnUIThread timeout (see
// the thread rule in CLAUDE.md) unwinding into our CRT. The driver is built with
// _HAS_EXCEPTIONS=0, so there is no exception object to describe: say so explicitly
// rather than leaving the reader guessing. Must not return — the CRT would abort
// anyway; we abort ourselves so leg (c) also runs and the RVAs land in the file.
static void __cdecl crashLogTerminateHandler()
{
    char mem[160];
    crashLogMemorySuffix(mem, sizeof mem);
    char reason[256];
#if defined(_HAS_EXCEPTIONS) && !_HAS_EXCEPTIONS
    std::snprintf(reason, sizeof(reason),
        "terminate (driver _HAS_EXCEPTIONS=0: no exception object; uncaught throw came from another module)%s", mem);
#else
    std::snprintf(reason, sizeof(reason), "terminate (pending exception=%d)%s",
        std::current_exception() ? 1 : 0, mem);
#endif
    crashLogWrite(reason, nullptr);
    perfFlush();
    consoleFlush();
    if (g_crashPrevTerminate && g_crashPrevTerminate != &crashLogTerminateHandler)
        g_crashPrevTerminate();   // harness leg: names the exception; aborts itself
    std::abort();
}

// (e) operator new failure. Uses the CRT hook rather than std::set_new_handler because
// this one is handed the requested size — the number that tells an ordinary large
// allocation apart from 32-bit address-space exhaustion, which is what a 770 MB working
// set on a Lumia produces. Returning 0 means "do not retry", so the CRT proceeds to its
// usual bad_alloc/terminate path; returning non-zero here would spin operator new forever.
static int __cdecl crashLogNewHandler(size_t size)
{
    char mem[160];
    crashLogMemorySuffix(mem, sizeof mem);
    char reason[256];
    std::snprintf(reason, sizeof(reason), "operation new failed size=%llu%s",
        static_cast<unsigned long long>(size), mem);
    crashLogWrite(reason, nullptr);
    perfFlush();
    consoleFlush();
    return 0;
}

// (f) CRT contract violation (bad handle, malformed printf spec, out-of-range index in a
// checked call). In a release CRT every string argument is null — that is expected; the
// stack in the same entry is what identifies the call site.
static void __cdecl crashLogInvalidParameterHandler(const wchar_t* expression, const wchar_t* function,
    const wchar_t* file, unsigned int line, uintptr_t /*reserved*/)
{
    auto narrow = [](const wchar_t* w, char* out, size_t outSize) {
        size_t i = 0;
        if (!w) {
            std::snprintf(out, outSize, "(null)");
            return;
        }
        for (; w[i] && i + 1 < outSize; ++i)
            out[i] = (w[i] < 128) ? static_cast<char>(w[i]) : '?';
        out[i] = 0;
    };
    char e[96], f[96], fl[128];
    narrow(expression, e, sizeof e);
    narrow(function, f, sizeof f);
    narrow(file, fl, sizeof fl);
    char reason[400];
    std::snprintf(reason, sizeof(reason), "invalid parameter expr=%s func=%s file=%s line=%u", e, f, fl, line);
    crashLogWrite(reason, nullptr);
    perfFlush();
    consoleFlush();
    if (g_crashPrevInvalidParameter && g_crashPrevInvalidParameter != &crashLogInvalidParameterHandler)
        g_crashPrevInvalidParameter(expression, function, file, line, 0);
    // Return: the CRT then fast-fails exactly as it would have without us.
}

// (g) pure virtual call — a virtual dispatched on a half-constructed/half-destroyed
// object. On this port the prime suspects are the teardown paths (session/tab destruction
// racing a queued engine call), which look identical to an OOM kill from outside.
static void __cdecl crashLogPureCallHandler()
{
    crashLogWrite("pure virtual call", nullptr);
    perfFlush();
    consoleFlush();
    if (g_crashPrevPureCall && g_crashPrevPureCall != &crashLogPureCallHandler)
        g_crashPrevPureCall();
    // Return: the CRT fast-fails as before.
}

// ==================== M4 step 1: per-phase timing (PerfLog) ==================
// Apotheosis: opt-in per-phase timing for the performance milestone. Off unless
// the harness calls WebCoreSetPerfLogPath() — which it only does when the tester
// dropped LocalState\perf.txt (same device-side opt-in as imedebug.txt) — so a
// shipping build pays one predictable branch per probe and nothing else.
// One CSV row per completed top-level operation (nav/scroll/tick/click); rows
// buffer in a fixed ring and reach disk with a single fopen_s("ab")+fprintf+
// fclose at nav completion / ring-full / explicit flush. Never one file open per
// frame — that would dominate the numbers on a Lumia 950. Timestamps are taken
// only at phase boundaries, never inside the per-pixel loops.
// Engine thread only, like the g_last* diagnostics above (deliberately lock-free).
static std::string g_perfPath;
// Apotheosis (M4 load timeline): LocalState\stage.txt, derived from crash.txt's directory in
// WebCoreSetCrashLogPath (the same trick console.txt uses below). One human-readable
// "timeline ..." line per navigation, so the load breakdown is legible without perf.csv.
static std::string g_stagePath;
static bool g_perfOn = false;
static bool g_perfHeaderDone = false;

// One physical line; the column order must stay in sync with perfFlush()'s fprintf.
static const char* const kPerfHeader =
    "seq,kind,url,ms_total,ms_net_commit,ms_net_load,ms_settle,ms_style_layout,ms_render_update,"
    "ms_flush,ms_backing,ms_paint,ms_readback,ms_swap,ms_blit,frames,subres_started,subres_ok,"
    "subres_fail,gpu,dfg,w,h,dirty_full,dirty_partial,net_dns,net_connect,net_tls,net_ttfb,http_ver,"
    "raster_pending,raster_done,raster_posted,raster_cancelled,raster_blocked,wake_count,"
    // Apotheosis (M4 load timeline): ms since dispatchDidStartProvisionalLoad, empty = never
    // reached; the n_* pair is cumulative at t_load. ms_style_layout / ms_paint above already
    // are the per-navigation sums, so the timeline does not duplicate them.
    "t_firstbyte,t_commit,t_dcl,t_firstpaint,t_load,t_settle,n_scripts,n_subres,"
    // Apotheosis (2026-09-06): finished tile replays the engine's upload budget pushed to a later
    // frame. Appended at the END rather than next to the other raster_ columns on purpose - the
    // analysis notes and scripts address the existing columns by index (the raster_ block starts at
    // column 31 counting from one), and shifting eight of them to group one new one with them would
    // break every one of those.
    "raster_deferred,"
    // Apotheosis (M4 load waterfall, 2026-09-07): the subresource side of the network, which
    // the four net_* columns above never covered (they are the main document only). Filled on
    // "nav" rows from WebCorePortNetTiming, empty everywhere else.
    //   net_sub_n        subresource responses whose headers arrived during this navigation
    //   net_sub_conn     of those, how many had to open a TCP connection. With HTTP/2 and a
    //                    working connection cache this is ~ the number of distinct hosts; if
    //                    it tracks net_sub_n instead, connection reuse/multiplexing is broken
    //   net_sub_h1       how many came back over HTTP/1.x (h2 negotiation failed for that host)
    //   net_sub_ttfb     sum of the per-transfer waits. Divided by the wall time between commit
    //                    and load it gives the effective parallelism: ~1 means the loader is
    //                    serialised, ~6-8 means the connection limits are actually being used
    //   net_sub_last     ms since nav start of the LAST subresource header. Close to t_load =
    //                    the load event waited for the network; far below it = it waited for us
    //   settle_why       which pumpLoop rule ended the navigation (quiet / cap-load / cap-dcl /
    //                    watchdog / frame-gone) - the difference between "the page was done" and
    //                    "we gave up on it"
    "net_sub_n,net_sub_conn,net_sub_h1,net_sub_ttfb,net_sub_last,settle_why,"
    // Apotheosis (M4 load, 2026-09-07): where the *engine thread* was during a navigation. The
    // pump's settle timer repeats every 50 ms, and RunLoopGeneric schedules the next fire from
    // the moment the callback starts - so the spacing between two callbacks is 50 ms plus
    // whatever else ran on the run loop in between (WebCore's own timers: the HTML parser's
    // yield timer, DOM timers, ScriptRunner, plus every callOnMainThread hop the loader makes
    // per response). That is the one large bucket nothing measured:
    //   ms_tick_cb   sum of the settle callbacks themselves (ms_render_update is inside this)
    //   ms_offpump   sum of max(0, period - max(50 ms, callback)) = main-thread work that was
    //                NOT ours. On the 2026-09-07 log this is the biggest single bucket of a
    //                cold news-site load, and it is invisible in every other column
    //   pump_ticks   settle callbacks (same number as `frames`, kept next to the two above so
    //                the row is readable on its own)
    "ms_tick_cb,ms_offpump,pump_ticks,"
    // Apotheosis (M4 load): the split of ms_offpump, from the scoped
    // accounting in wtf/ApoLoadPhase.h (armed by pumpLoop, only with perf logging on). Each
    // bucket holds the *self* time of its scopes, so they never overlap and an inner scope
    // always beats the generic run-loop one around it:
    //   off_js       DOMTimer::fired + ScriptController::evaluateInWorld - third-party script
    //   off_parse    HTMLDocumentParser::pumpTokenizer, including the yield timer's slices
    //   off_style    Document::resolveStyle + LocalFrameViewLayoutContext::layout that no
    //                script or parser scope was already paying for
    //   off_decode   synchronous image decode on the engine thread
    //   off_timer    a RunLoop timer none of the above named (WebCore has many small ones)
    //   off_dispatch RunLoop::dispatch / callOnMainThread functions - the loader makes one
    //                hop per response and per body chunk; off_disp_n is how many ran
    //   off_other    ms_offpump minus the six, i.e. the run loop's own overhead and whatever
    //                still has no scope. A large off_other means this list is incomplete
    "off_js,off_parse,off_style,off_decode,off_timer,off_dispatch,off_disp_n,off_other,"
    // Apotheosis (M4 load, 2026-09-07): how often, not how long. off_style
    // is one number for two very different things, and 1.4 s of it on a tech-news site reads the same
    // whether one style resolution was expensive or the pump forced twenty. Counted in
    // wtf/ApoLoadPhase.h at the same scopes, over the same window as the buckets:
    //   style_n   Document::resolveStyle entries          (> ~10 per navigation = the driver is
    //   layout_n  LocalFrameViewLayoutContext::layout      forcing passes, not the page)
    //   timer_n   DOMTimer::fired entries                 (drops when the C2.9 alignment works)
    "style_n,layout_n,timer_n,"
    // Apotheosis (2026-09-08, docs/TILEGRID-DESIGN.md section 3): why the layers dirty_full
    // counts were repainted in full, one string cell instead of six more columns so existing
    // scripts that address raster_deferred and everything before it by index (see the raster_
    // comment above) keep working - appended at the very END for the same reason. Format
    // "a/b/c/d/e/f", each a running sum over this operation except f (0 or 1):
    //   a  layers where m_needsDisplay was set (GraphicsLayerTextureMapper::wkTexmapDirtySrcStats)
    //   b  layers whose backing store was just created (m_wkNeedsFullRepaint)
    //   c  layers whose backing store size changed (m_wkBackingStoreSize != m_size)
    //   d  layers whose contents scale changed (m_wkBackingStoreScale != wkContentsScale)
    //   e  GraphicsLayerTextureMapper::setNeedsDisplay() calls that flipped m_needsDisplay
    //      false->true (a superset of forceDirtyTree()'s own calls - see f)
    //   f  1 if any composite in this operation was one forceDirtyTree() had just walked
    //      (g_gpuLastCompositeFull), else 0 - the only field NOT counted inside WebCore, so a
    //      row with e > 0 and f = 0 means WebCore itself asked for the repaint, not the driver
    "dirty_src,"
    // Apotheosis (2026-09-08, docs/TILEGRID-DESIGN.md 2.5/3): TileGrid v2's only output
    // the driver reacts to - the sum over this operation's composites of visible cells that drew
    // NOTHING and had no backdrop behind them (wkWinUWPTexmapVisibleHoles, read once per composite
    // in gpuPresent). Empty while the v2 switch is off. 0 = every visible cell had pixels; a small
    // number for a frame or two after a pinch or a fast pan is the model working (each of those
    // composites also asks for one more present); a row that stays non-zero with the finger off the
    // glass is the thing to chase. Appended at the very END for the same reason as dirty_src.
    "tg_holes\n";

struct PerfRow {
    unsigned seq = 0;
    char kind[12] = { 0 };    // nav | scroll | tick | click
    char url[192] = { 0 };
    // Phase durations in ms; < 0 means "phase does not apply" → empty CSV cell.
    double total = -1, netCommit = -1, netLoad = -1, settle = -1, styleLayout = -1,
           renderUpdate = -1, flush = -1, backing = -1, paint = -1, readback = -1,
           swap = -1, blit = -1;
    double domReady = -1;     // no column of its own; ms_net_load falls back to it
    int frames = -1, subStarted = -1, subOk = -1, subFail = -1;
    int gpu = 0, dfg = 0, w = 0, h = 0;
    // Apotheosis (M4): TextureMapper layers repainted in full vs. by dirty rect in this operation
    // (wkWinUWPTexmapDirtyStats, WebKit winuwp f14d05ff1f); -1 = no composite happened.
    int dirtyFull = -1, dirtyPartial = -1;
    // Apotheosis (2026-09-08, docs/TILEGRID-DESIGN.md section 3): per-cause breakdown of
    // dirtyFull above (wkWinUWPTexmapDirtySrcStats — see dirty_src in kPerfHeader),
    // accumulated across this operation the same way dirtyFull/dirtyPartial are. -1 = no
    // composite happened. dirtySrcForceDirty is not a sum: it is 1 if ANY composite in this
    // operation was one forceDirtyTree() had just walked (g_gpuLastCompositeFull).
    int dirtySrcNeedsDisplay = -1, dirtySrcStoreCreated = -1, dirtySrcSizeChange = -1,
        dirtySrcScaleChange = -1, dirtySrcSetNeedsDisplay = -1, dirtySrcForceDirty = -1;
    // Apotheosis (2026-09-08, docs/TILEGRID-DESIGN.md section 3): tg_holes - the sum of
    // wkWinUWPTexmapVisibleHoles() over this operation's composites (see the column in
    // kPerfHeader). -1 = the TileGrid v2 switch was off, i.e. nobody could have reported a hole,
    // which is a different statement from "no hole was reported" and therefore a different cell.
    int tgHoles = -1;
    // Apotheosis (M4): curl's breakdown of the main resource of this navigation
    // (WebCorePortNetTiming, WebKit CurlRequest::didReceiveHeader). Milliseconds since
    // the transfer started; net_connect is the TCP handshake only, net_tls the TLS one,
    // net_ttfb the whole wait until the first response byte. -1 = never reported.
    // Chasing the sporadic 14-22 s "first contact" stalls seen on the Lumia over Wi-Fi.
    double netDns = -1, netConnect = -1, netTls = -1, netTtfb = -1;
    int httpVer = -1;         // 10 / 11 / 20 / 30; 0 = curl could not tell
    // Apotheosis (OFFTHREAD-RASTER-LOG.md §4.3): tile replays still running on the worker pool when
    // this operation's last composite finished. 0 with threaded raster on = the pool kept up;
    // -1 = no composite happened (or the feature is off), i.e. an empty CSV cell.
    int rasterPending = -1;
    // Apotheosis (OFFTHREAD-RASTER-LOG.md §10): raster_done = replays that finished since the last
    // composite and are therefore owed a present (edge-triggered, wkWinUWPTexmapTakeFinishedRasterTiles).
    // The other three are running totals since process start: replays posted, replays cancelled
    // before a worker picked them up, and times the engine thread had to BLOCK on a replay. The
    // last one is the one to watch: it must stay near zero, otherwise off-thread raster is worse
    // than synchronous raster.
    int rasterDone = -1, rasterPosted = -1, rasterCancelled = -1, rasterBlocked = -1;
    // Apotheosis (2026-09-06): raster_deferred = finished replays the engine's per-pass upload
    // budget moved to a later frame (edge-triggered, wkWinUWPTexmapTakeDeferredUploads). It is the
    // price side of the ms_backing improvement: a row with a low ms_backing and a high
    // raster_deferred means the burst was spread, a row where it never falls back to zero means
    // the budget is too small for what the page produces.
    int rasterDeferred = -1;
    // Apotheosis (event-driven present): wake-ups fired to the harness since the previous tick.
    // 0 on an idle page (the loop is asleep, only the 1 s fallback tick runs), ~1 per frame on an
    // animating one; a number much larger than 1 means the arming atomic stopped collapsing bursts.
    // -1 = the operation was not a tick (only WebCoreLiveTick reads and resets the counter).
    int wakes = -1;
    // Apotheosis (M4 load timeline): milestones of one navigation, in ms since the provisional
    // load started (perfSinceNavStart); -1 = never reached -> empty CSV cell. t_commit and t_dcl
    // get no fields of their own: the CSV prints netCommit and domReady in those columns.
    //   t_firstbyte  main-resource response headers (WebCorePortNetTiming, curl didReceiveHeader)
    //   t_firstpaint DidFirstVisuallyNonEmptyLayout  (dispatchDidReachLayoutMilestone)
    //   t_load       load event                      (dispatchDidFinishLoad)
    //   t_settle     pumpLoop returned               (buildSession: the driver calls it done)
    double tFirstByte = -1, tFirstPaint = -1, tLoad = -1, tSettle = -1;
    // Cumulative at t_load: <script> elements in the document, subresources completed.
    int nScripts = -1, nSubres = -1;
    // Apotheosis (M4 load waterfall): subresource network aggregate of this navigation; see the
    // net_sub_* block in kPerfHeader. -1 = not a nav row / nothing reported.
    int netSubN = -1, netSubConn = -1, netSubH1 = -1;
    double netSubTtfb = -1, netSubLast = -1;
    char settleWhy[16] = { 0 };   // pumpLoop's stop reason, "" on non-nav rows
    // Apotheosis (M4 load): engine-thread accounting of the pump; see kPerfHeader.
    double msTickCb = -1, msOffPump = -1;
    int pumpTicks = -1;
    // Apotheosis (M4 load, 2026-09-07): the ms_offpump split; see kPerfHeader and
    // wtf/ApoLoadPhase.h. -1 = not a nav row.
    double offJs = -1, offParse = -1, offStyle = -1, offDecode = -1, offTimer = -1,
           offDispatch = -1, offOther = -1;
    int offDispatchN = -1;
    // Apotheosis (M4 load, 2026-09-07): pass counts over the same window as the buckets above;
    // see kPerfHeader and wtf/ApoLoadPhase.h. -1 = not a nav row.
    int styleN = -1, layoutN = -1, timerN = -1;
};

static constexpr int kPerfRingSize = 256;
static PerfRow g_perfRing[kPerfRingSize];
static int g_perfRows = 0;
static unsigned g_perfSeq = 0;

static PerfRow g_perfCur;                 // operation currently being measured
static bool g_perfInOp = false;
static bool g_perfOpIsNav = false;
static MonotonicTime g_perfOpStart;       // C ABI entry of the current operation
static MonotonicTime g_perfNavT0;         // first provisional load start of the operation
static bool g_perfNavT0Set = false;
static int g_perfPumpTicks = 0;           // pumpLoop settle ticks of the current operation
// Apotheosis (M4 load, 2026-09-07): which pumpLoop rule ended the navigation - see the
// settle_why column in kPerfHeader. Written by pumpLoop (perf-independent, it is also the
// cheapest breadcrumb when a load feels stuck), read by perfEnd.
static char g_settleWhy[16] = { 0 };
// Perf-independent "this navigation has painted something readable" flag, set from
// perfNavVisuallyNonEmpty and cleared at navLoadBegin. pumpLoop's wall-clock cap on the
// pre-load phase only fires once the user actually has a page to look at.
static std::atomic<int> g_navSawFirstPaint { 0 };
// Apotheosis (M4 load, 2026-09-07): the wall clock of the two events pumpLoop's caps are counted
// from, taken *at the event* instead of at the settle tick that happens to notice it. Both are
// perf-independent (the caps run with logging off) and 0 = "not reached in this navigation".
//
// This is a real bug, not tidiness. The settle timer ticks every 50 ms plus its callback plus
// whatever the run loop does in between, which on a news site is 160 ms on average and over a second
// when an ad timer chain runs; anchoring a 1 s deadline on the first tick that observed the event
// therefore pushed the deadline out by a whole gap. Measured (log 20260907-100044): the news site warm
// fired its load event at 1820 ms and cap-load ended the pump at 4124 ms - 2.3 s for a 1 s cap.
static std::atomic<double> g_navLoadEventSec { 0 };   // dispatchDidFinishLoad
static std::atomic<double> g_navDomReadySec { 0 };    // dispatchDidFinishDocumentLoad
// Apotheosis (M4 load): the DOM timer alignment the load window asks for.
// Declared here because the stage.txt timeline line prints it; used by loadTimerThrottleSet().
static constexpr unsigned kLoadTimerAlignMs = 250;
static constexpr unsigned kLoadTimerNestedAlignMs = 1000;

// Scoped phase timer: adds its own lifetime to one PerfRow field (accumulating,
// so repeated phases inside one operation sum up). Reads no clock at all when
// logging is off. Never place one inside a pixel loop.
struct PerfPhase {
    double* slot;
    MonotonicTime t;
    explicit PerfPhase(double* s)
        : slot(g_perfOn ? s : nullptr)
        , t(g_perfOn ? MonotonicTime::now() : MonotonicTime())
    {
    }
    ~PerfPhase()
    {
        if (!slot)
            return;
        const double ms = (MonotonicTime::now() - t).milliseconds();
        *slot = (*slot < 0) ? ms : (*slot + ms);
    }
};

// CSV-safe copy: URLs carry commas/quotes and a row must stay one physical line.
static void perfSetUrl(const char* url)
{
    g_perfCur.url[0] = '\0';
    if (!url)
        return;
    size_t n = 0;
    for (; url[n] && n + 1 < sizeof g_perfCur.url; ++n) {
        const char c = url[n];
        g_perfCur.url[n] = (c == ',' || c == '"' || c == '\r' || c == '\n') ? '_' : c;
    }
    g_perfCur.url[n] = '\0';
}

static void perfFmtD(char* buf, size_t cap, double v)
{
    if (v < 0) {
        buf[0] = '\0';
        return;
    }
    std::snprintf(buf, cap, "%.2f", v);
}

static void perfFmtI(char* buf, size_t cap, int v)
{
    if (v < 0) {
        buf[0] = '\0';
        return;
    }
    std::snprintf(buf, cap, "%d", v);
}

// Drain the ring to disk. Single open/append/close (App-Container-safe, the same
// shape PortNetworkStorageSession uses for its cookie diag).
// Apotheosis: the crash legs above call this so the rows leading up to an abort are not
// lost with the process. There is no lock to deadlock on, but the crash may well *be*
// inside this function (a fopen/fprintf on an exhausted heap), so entry is gated by an
// atomic try-flag: a flush already in flight is never re-entered, we simply skip it.
static std::atomic<int> g_perfFlushBusy { 0 };

static void perfFlushLocked()
{
    if (!g_perfOn || g_perfPath.empty() || !g_perfRows)
        return;
    FILE* fp = nullptr;
    if (fopen_s(&fp, g_perfPath.c_str(), "ab") != 0 || !fp) {
        g_perfRows = 0;   // an unwritable path must not make the ring grow forever
        return;
    }
    if (!g_perfHeaderDone) {
        std::fputs(kPerfHeader, fp);
        g_perfHeaderDone = true;
    }
    for (int i = 0; i < g_perfRows; ++i) {
        const PerfRow& r = g_perfRing[i];
        const double vals[12] = { r.total, r.netCommit, r.netLoad, r.settle, r.styleLayout,
                                  r.renderUpdate, r.flush, r.backing, r.paint, r.readback,
                                  r.swap, r.blit };
        char d[12][16];
        for (int k = 0; k < 12; ++k)
            perfFmtD(d[k], sizeof d[k], vals[k]);
        const int ints[4] = { r.frames, r.subStarted, r.subOk, r.subFail };
        char n[4][12];
        for (int k = 0; k < 4; ++k)
            perfFmtI(n[k], sizeof n[k], ints[k]);
        char df[12], dp[12];
        perfFmtI(df, sizeof df, r.dirtyFull);
        perfFmtI(dp, sizeof dp, r.dirtyPartial);
        // M4: network breakdown of the main resource (empty cells when never reported).
        const double netVals[4] = { r.netDns, r.netConnect, r.netTls, r.netTtfb };
        char nt[4][16];
        for (int k = 0; k < 4; ++k)
            perfFmtD(nt[k], sizeof nt[k], netVals[k]);
        char hv[12];
        perfFmtI(hv, sizeof hv, r.httpVer);
        const int rasterVals[5] = { r.rasterPending, r.rasterDone, r.rasterPosted,
                                    r.rasterCancelled, r.rasterBlocked };
        char rs[5][12];
        for (int k = 0; k < 5; ++k)
            perfFmtI(rs[k], sizeof rs[k], rasterVals[k]);
        char wk[12];
        perfFmtI(wk, sizeof wk, r.wakes);
        // M4 load timeline (t_commit = netCommit, t_dcl = domReady - no second copy is kept).
        const double tlVals[6] = { r.tFirstByte, r.netCommit, r.domReady, r.tFirstPaint,
                                   r.tLoad, r.tSettle };
        char tl[6][16];
        for (int k = 0; k < 6; ++k)
            perfFmtD(tl[k], sizeof tl[k], tlVals[k]);
        char nsc[12], nsr[12];
        perfFmtI(nsc, sizeof nsc, r.nScripts);
        perfFmtI(nsr, sizeof nsr, r.nSubres);
        char rdf[12];
        perfFmtI(rdf, sizeof rdf, r.rasterDeferred);
        // M4 load waterfall: subresource network aggregate (nav rows only).
        const int subInts[3] = { r.netSubN, r.netSubConn, r.netSubH1 };
        char sb[3][12];
        for (int k = 0; k < 3; ++k)
            perfFmtI(sb[k], sizeof sb[k], subInts[k]);
        char sbt[16], sbl[16];
        perfFmtD(sbt, sizeof sbt, r.netSubTtfb);
        perfFmtD(sbl, sizeof sbl, r.netSubLast);
        // M4 load: engine-thread accounting of the pump.
        char tcb[16], offp[16], pt[12];
        perfFmtD(tcb, sizeof tcb, r.msTickCb);
        perfFmtD(offp, sizeof offp, r.msOffPump);
        perfFmtI(pt, sizeof pt, r.pumpTicks);
        // M4 load C2.1: the ms_offpump split (empty cells on non-nav rows).
        const double offVals[7] = { r.offJs, r.offParse, r.offStyle, r.offDecode, r.offTimer,
                                    r.offDispatch, r.offOther };
        char of[7][16];
        for (int k = 0; k < 7; ++k)
            perfFmtD(of[k], sizeof of[k], offVals[k]);
        char ofn[12];
        perfFmtI(ofn, sizeof ofn, r.offDispatchN);
        // M4 load C2.10: style / layout / DOM timer pass counts.
        const int passInts[3] = { r.styleN, r.layoutN, r.timerN };
        char pc[3][12];
        for (int k = 0; k < 3; ++k)
            perfFmtI(pc[k], sizeof pc[k], passInts[k]);
        // Apotheosis (2026-09-08, docs/TILEGRID-DESIGN.md section 3): dirty_src, appended at the
        // very end - see the comment on the column in kPerfHeader. -1 on dirtySrcNeedsDisplay means
        // no composite happened this operation, same convention as dirtyFull/dirtyPartial -> empty
        // cell rather than "0/0/0/0/0/0", which would misleadingly claim a composite ran clean.
        char ds[48];
        if (r.dirtySrcNeedsDisplay < 0)
            ds[0] = '\0';
        else
            std::snprintf(ds, sizeof ds, "%d/%d/%d/%d/%d/%d",
                r.dirtySrcNeedsDisplay, r.dirtySrcStoreCreated, r.dirtySrcSizeChange,
                r.dirtySrcScaleChange, r.dirtySrcSetNeedsDisplay,
                r.dirtySrcForceDirty > 0 ? 1 : 0);
        // Apotheosis (2026-09-08, docs/TILEGRID-DESIGN.md section 3): tg_holes, after it. Same
        // convention: -1 (the v2 switch was off for every composite of this operation) -> empty
        // cell, so a 0 always means "v2 ran and saw no hole".
        char tgh[12];
        perfFmtI(tgh, sizeof tgh, r.tgHoles);
        std::fprintf(fp, "%u,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%d,%d,%d,%d,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,"
                         "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,"
                         "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
            r.seq, r.kind, r.url,
            d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11],
            n[0], n[1], n[2], n[3], r.gpu, r.dfg, r.w, r.h, df, dp,
            nt[0], nt[1], nt[2], nt[3], hv,
            rs[0], rs[1], rs[2], rs[3], rs[4], wk,
            tl[0], tl[1], tl[2], tl[3], tl[4], tl[5], nsc, nsr, rdf,
            sb[0], sb[1], sb[2], sbt, sbl, r.settleWhy,
            tcb, offp, pt,
            of[0], of[1], of[2], of[3], of[4], of[5], ofn, of[6],
            pc[0], pc[1], pc[2], ds, tgh);
    }
    std::fclose(fp);
    g_perfRows = 0;
}

static void perfFlush()
{
    int expected = 0;
    if (!g_perfFlushBusy.compare_exchange_strong(expected, 1))
        return;
    perfFlushLocked();
    g_perfFlushBusy.store(0);
}

// ---- Apotheosis (M4 load waterfall, 2026-09-07) ------------------------------------------
// The slowest subresource transfers of the current navigation, kept as a tiny insertion-sorted
// table (no allocation, no lock - every entry arrives on the engine thread from
// WebCorePortNetTiming). Sorted by TTFB, which on this port is the interesting number: a
// resource whose own transfer was fast but whose headers arrived late was queued, and the pair
// (t, b) below shows exactly that. Printed once per navigation as a "loadwaterfall" line.
struct NetSubEntry {
    char name[40];    // last path segment, truncated - enough to recognise the resource
    double tHeader;   // ms since nav start when the response headers landed
    double dns, connect, tls, ttfb;
    int ver;
};
static constexpr int kNetTopN = 8;
static NetSubEntry g_netTop[kNetTopN];
static int g_netTopUsed = 0;
static double perfSinceNavStart();   // defined below, next to the navigation phase marks

static void netSubReset()
{
    g_netTopUsed = 0;
}

// Copy the last path segment of a URL (query stripped) into a fixed buffer.
static void netSubShortName(char* out, size_t cap, const char* url)
{
    out[0] = '\0';
    if (!url || !*url)
        return;
    const char* end = std::strpbrk(url, "?#");
    const char* stop = end ? end : url + std::strlen(url);
    const char* begin = stop;
    while (begin > url && begin[-1] != '/')
        --begin;
    size_t n = static_cast<size_t>(stop - begin);
    if (!n) {                       // directory URL ("https://host/") - fall back to the host
        begin = url;
        n = static_cast<size_t>(stop - url);
    }
    if (n > cap - 1)
        n = cap - 1;
    std::memcpy(out, begin, n);
    out[n] = '\0';
}

static void netSubAdd(double dnsMs, double connectMs, double tlsMs, double ttfbMs,
                      int httpVersion, const char* url)
{
    const double now = perfSinceNavStart();
    // Aggregate first - it counts every response, not just the slow ones.
    if (g_perfCur.netSubN < 0) {
        g_perfCur.netSubN = 0;
        g_perfCur.netSubConn = 0;
        g_perfCur.netSubH1 = 0;
        g_perfCur.netSubTtfb = 0;
    }
    ++g_perfCur.netSubN;
    if (connectMs > 0 || tlsMs > 0)          // a new TCP/TLS connection had to be opened
        ++g_perfCur.netSubConn;
    if (httpVersion && httpVersion < 20)     // 0 = curl could not tell; do not blame it on h1
        ++g_perfCur.netSubH1;
    g_perfCur.netSubTtfb += ttfbMs;
    if (now > g_perfCur.netSubLast)
        g_perfCur.netSubLast = now;

    // Then the top-N by TTFB.
    if (g_netTopUsed == kNetTopN && ttfbMs <= g_netTop[kNetTopN - 1].ttfb)
        return;
    int at = g_netTopUsed < kNetTopN ? g_netTopUsed : kNetTopN - 1;
    while (at > 0 && g_netTop[at - 1].ttfb < ttfbMs) {
        g_netTop[at] = g_netTop[at - 1];
        --at;
    }
    NetSubEntry& e = g_netTop[at];
    netSubShortName(e.name, sizeof e.name, url);
    e.tHeader = now;
    e.dns = dnsMs;
    e.connect = connectMs;
    e.tls = tlsMs;
    e.ttfb = ttfbMs;
    e.ver = httpVersion;
    if (g_netTopUsed < kNetTopN)
        ++g_netTopUsed;
}

// One "loadwaterfall" line per navigation, next to the "timeline" line. Read it as: n/conn/h1
// say whether the connection cache and HTTP/2 are doing their job, ttfbsum/(load-commit) is the
// effective parallelism, last is when the network actually went quiet, and the entries are the
// eight transfers that waited longest - "name t=<header arrived> b=<ttfb> d/c/s=<dns/tcp/tls> v=<http>".
static void perfWriteStageWaterfall(const PerfRow& r)
{
    if (g_stagePath.empty() || r.netSubN < 0)
        return;
    FILE* fp = nullptr;
    if (fopen_s(&fp, g_stagePath.c_str(), "ab") != 0 || !fp)
        return;
    std::fprintf(fp, "loadwaterfall url=%s n=%d conn=%d h1=%d ttfbsum=%.0f last=%.0f slowest=[",
        r.url, r.netSubN, r.netSubConn, r.netSubH1,
        r.netSubTtfb < 0 ? 0.0 : r.netSubTtfb, r.netSubLast < 0 ? 0.0 : r.netSubLast);
    for (int i = 0; i < g_netTopUsed; ++i) {
        const NetSubEntry& e = g_netTop[i];
        std::fprintf(fp, "%s%s t=%.0f b=%.0f d=%.0f c=%.0f s=%.0f v=%d",
            i ? " | " : "", e.name, e.tHeader, e.ttfb, e.dns, e.connect, e.tls, e.ver);
    }
    std::fputs("]\n", fp);
    std::fclose(fp);
}

// Apotheosis (M4 load timeline): the same navigation timeline as one readable line in
// LocalState\stage.txt - the file the device scripts already pull and a human can read on the
// phone. One open/append/close per navigation (never per frame), so the cost is irrelevant.
// "-" means the milestone was never reached, which is itself the interesting case: a load that
// never paints has fp=-, one that never fires a load event has load=-.
static void perfWriteStageTimeline(const PerfRow& r)
{
    if (g_stagePath.empty())
        return;
    const double vals[6] = { r.tFirstByte, r.netCommit, r.domReady, r.tFirstPaint, r.tLoad, r.tSettle };
    char t[6][16];
    for (int k = 0; k < 6; ++k) {
        if (vals[k] < 0)
            std::snprintf(t[k], sizeof t[k], "-");
        else
            std::snprintf(t[k], sizeof t[k], "%.0f", vals[k]);
    }
    FILE* fp = nullptr;
    if (fopen_s(&fp, g_stagePath.c_str(), "ab") != 0 || !fp)
        return;
    // Apotheosis (M4 load, 2026-09-07): why= is pumpLoop's stop reason. Without it a long
    // settle and a settle that hit a cap look identical in this line, and they call for
    // opposite fixes (wait longer vs. stop waiting).
    std::fprintf(fp, "timeline url=%s firstbyte=%s commit=%s dcl=%s fp=%s load=%s settle=%s"
                     " why=%s total=%.0f subres=%d/%d scripts=%d ticks=%d cb=%.0f offpump=%.0f\n",
        r.url, t[0], t[1], t[2], t[3], t[4], t[5],
        r.settleWhy[0] ? r.settleWhy : "-",
        r.total < 0 ? 0.0 : r.total, r.subOk, r.subStarted, r.nScripts,
        r.pumpTicks, r.msTickCb < 0 ? 0.0 : r.msTickCb, r.msOffPump < 0 ? 0.0 : r.msOffPump);
    // Apotheosis (M4 load): the same split as the off_* columns, on a line
    // of its own so the timeline line above keeps the shape the existing notes quote.
    // Apotheosis (M4 load, 2026-09-07): style_n/layout_n/timer_n are pass *counts* over the
    // same window (wtf/ApoLoadPhase.h). A style time that is large because one resolution is
    // expensive and one that is large because the pump forces twenty need opposite fixes, and
    // timer_n is how the C2.9 timer alignment shows up: the same work in fewer, larger runs.
    // tthr= is the alignment the load window asked for, so a log says which build it is.
    std::fprintf(fp, "offpump url=%s total=%.0f js=%.0f parse=%.0f style=%.0f decode=%.0f"
                     " timers=%.0f dispatch=%.0f/%d other=%.0f"
                     " style_n=%d layout_n=%d timer_n=%d tthr=%u\n",
        r.url, r.msOffPump < 0 ? 0.0 : r.msOffPump,
        r.offJs < 0 ? 0.0 : r.offJs, r.offParse < 0 ? 0.0 : r.offParse,
        r.offStyle < 0 ? 0.0 : r.offStyle, r.offDecode < 0 ? 0.0 : r.offDecode,
        r.offTimer < 0 ? 0.0 : r.offTimer, r.offDispatch < 0 ? 0.0 : r.offDispatch,
        r.offDispatchN < 0 ? 0 : r.offDispatchN, r.offOther < 0 ? 0.0 : r.offOther,
        r.styleN, r.layoutN, r.timerN, kLoadTimerAlignMs);
    std::fclose(fp);
}

// Apotheosis (2026-09-07, ghost after a pinch): the engine's zoom trace into stage.txt. WebCore
// writes one "zoom ..." line per composite for the few composites that follow a contents-scale
// change (TextureMapperTiledBackingStore::wkTraceZoomComposite) and this drains whatever is
// pending. Called at the end of every operation, not only of a navigation: the composites the
// trace describes happen on the ticks *after* the pinch, never on the pinch's own row. Between
// two zooms the take returns 0 and the file is not opened at all.
// Apotheosis (2026-09-08, docs/TILEGRID-DESIGN.md 5.3): the same take now also drains the
// TileGrid v2 input trace ("tg ..."), whose ring is 256 lines of up to 320 characters - up to ~82 kB
// against the 2 kB buffer this used to pass, i.e. one call could carry seven lines and the model's
// replay input (§5.4) would be full of gaps. The take copies WHOLE lines and leaves the rest in the
// ring for the next call, so draining is a loop: keep an 8 kB buffer off the stack pressure of a
// single 82 kB one and go round until the ring is empty. The cap only exists so a bug in the engine
// ring cannot spin here; 16 rounds is twice the worst case (three full rings).
static void perfWriteStageZoomTrace()
{
    if (g_stagePath.empty())
        return;
    char buf[8192];
    size_t used = WebCore::wkWinUWPTakeTexmapZoomTrace(buf, sizeof buf);
    if (!used)
        return;
    FILE* fp = nullptr;
    if (fopen_s(&fp, g_stagePath.c_str(), "ab") != 0 || !fp)
        return;
    for (int round = 0; used && round < 16; ++round) {
        std::fwrite(buf, 1, used, fp);
        used = WebCore::wkWinUWPTakeTexmapZoomTrace(buf, sizeof buf);
    }
    std::fclose(fp);
}

static void perfBegin(const char* kind, const char* url, int w, int h)
{
    if (!g_perfOn)
        return;
    g_perfCur = PerfRow{};
    std::snprintf(g_perfCur.kind, sizeof g_perfCur.kind, "%s", kind ? kind : "?");
    perfSetUrl(url);
    g_perfCur.w = w;
    g_perfCur.h = h;
    g_perfOpIsNav = kind && !std::strcmp(kind, "nav");
    g_perfPumpTicks = 0;
    g_perfNavT0Set = false;
    netSubReset();          // M4 load waterfall: the slow-transfer table is per navigation
    WTF::apoPhaseReset();   // M4 load C2.1: the ms_offpump split is per operation too
    g_settleWhy[0] = '\0';
    g_perfInOp = true;
    g_perfOpStart = MonotonicTime::now();
}

static void perfEnd()
{
    if (!g_perfOn || !g_perfInOp)
        return;
    g_perfInOp = false;
    g_perfCur.total = (MonotonicTime::now() - g_perfOpStart).milliseconds();
    g_perfCur.seq = ++g_perfSeq;
    if (!g_perfCur.url[0])
        perfSetUrl(g_lastUrl);
    // ms_net_load: the load event when it fired, else DOM ready — redirect chains
    // and in-page (SPA) navigations may never reach a second load event.
    if (g_perfCur.netLoad < 0)
        g_perfCur.netLoad = g_perfCur.domReady;
    if (g_perfOpIsNav) {
        g_perfCur.frames = g_perfPumpTicks;         // rendering updates driven while settling
        g_perfCur.subStarted = g_loadStarted;
        g_perfCur.subOk = g_loadComplete;
        g_perfCur.subFail = g_loadFail;
        // M4 load timeline: n_subres is the count at t_load (set in perfNavLoadEvent); a
        // navigation that never fired a load event falls back to the final count so the column
        // is not blank for a page that did render. n_scripts comes from the last writeDiag().
        if (g_perfCur.nSubres < 0)
            g_perfCur.nSubres = g_loadComplete;
        g_perfCur.nScripts = g_lastScriptCount;
        std::snprintf(g_perfCur.settleWhy, sizeof g_perfCur.settleWhy, "%s",
                      g_settleWhy[0] ? g_settleWhy : "-");
        g_perfCur.pumpTicks = g_perfPumpTicks;
        // M4 load C2.1: the ms_offpump split. The buckets are self times and therefore disjoint;
        // off_other is what ms_offpump has left over - run-loop overhead plus anything that still
        // has no scope. It is the number that says whether the attribution can be trusted: a load
        // whose off_other dominates has not been explained.
        g_perfCur.offJs = WTF::apoPhaseMilliseconds(WTF::ApoPhaseBucket::Js);
        g_perfCur.offParse = WTF::apoPhaseMilliseconds(WTF::ApoPhaseBucket::Parse);
        g_perfCur.offStyle = WTF::apoPhaseMilliseconds(WTF::ApoPhaseBucket::Style);
        g_perfCur.offDecode = WTF::apoPhaseMilliseconds(WTF::ApoPhaseBucket::Decode);
        g_perfCur.offTimer = WTF::apoPhaseMilliseconds(WTF::ApoPhaseBucket::Timer);
        g_perfCur.offDispatch = WTF::apoPhaseMilliseconds(WTF::ApoPhaseBucket::Dispatch);
        g_perfCur.offDispatchN = static_cast<int>(WTF::apoPhaseEntries(WTF::ApoPhaseBucket::Dispatch));
        const double offSum = g_perfCur.offJs + g_perfCur.offParse + g_perfCur.offStyle
            + g_perfCur.offDecode + g_perfCur.offTimer + g_perfCur.offDispatch;
        const double offTotal = g_perfCur.msOffPump < 0 ? 0.0 : g_perfCur.msOffPump;
        g_perfCur.offOther = offTotal > offSum ? offTotal - offSum : 0.0;
        // Apotheosis (M4 load C2.10): pass counts over the same window.
        g_perfCur.styleN = static_cast<int>(WTF::apoPhaseEvents(WTF::ApoPhaseCounter::StyleRecalc));
        g_perfCur.layoutN = static_cast<int>(WTF::apoPhaseEvents(WTF::ApoPhaseCounter::Layout));
        g_perfCur.timerN = static_cast<int>(WTF::apoPhaseEvents(WTF::ApoPhaseCounter::DomTimer));
    } else
        g_perfCur.frames = 1;
    g_perfCur.gpu = g_gpuActive ? 1 : 0;
    g_perfCur.dfg = 0;   // hardcoded: becomes a build flag once ENABLE_DFG_JIT=ON is measured (M4 §4)
    if (g_perfCur.w <= 0 && g_session) {            // viewport of the live session
        g_perfCur.w = g_session->w;
        g_perfCur.h = g_session->h;
    }
    if (g_perfOpIsNav) {
        perfWriteStageTimeline(g_perfCur);
        perfWriteStageWaterfall(g_perfCur);
    }
    // Apotheosis (2026-09-07): ... and the engine's zoom trace on any operation - see
    // perfWriteStageZoomTrace(); it is a no-op unless a pinch happened a few frames ago.
    perfWriteStageZoomTrace();
    if (g_perfRows < kPerfRingSize)
        g_perfRing[g_perfRows++] = g_perfCur;
    // Flush on nav completion, and every 32 rows so a crash mid-session (seen
    // on a news site) does not take the whole ring with it - still one file open per
    // ~6 s of ticks, not per frame.
    if (g_perfOpIsNav || g_perfRows >= 32)
        perfFlush();
}

// Scope guard so every early return of an entry point still emits its row.
struct PerfOpGuard {
    PerfOpGuard(const char* kind, const char* url, int w, int h) { perfBegin(kind, url, w, h); }
    ~PerfOpGuard() { perfEnd(); }
};

// Network phases are measured from the first provisional load start, so page
// setup/teardown before the load does not leak into the network columns.
static double perfSinceNavStart()
{
    return (MonotonicTime::now() - (g_perfNavT0Set ? g_perfNavT0 : g_perfOpStart)).milliseconds();
}

// Apotheosis (M4 load timeline): t_settle - the moment the driver itself calls the navigation
// done, i.e. pumpLoop returned in buildSession (load event, settle cap or watchdog). Whatever
// is left between it and ms_total is the final layout, link extraction and first paint.
static void perfMarkNavSettled()
{
    if (!g_perfOn || !g_perfInOp || !g_perfOpIsNav || g_perfCur.tSettle >= 0)
        return;
    g_perfCur.tSettle = perfSinceNavStart();
}

// Navigation phase marks — LoadingFrameLoaderClient is the only observer of these
// boundaries. Declared in PortPerf.h; first mark of an operation wins (a redirect
// chain keeps the timestamps of the load the user actually asked for).
namespace WebCorePort {

void perfNavStart()
{
    // Apotheosis (M4 load throttle): runs with perf logging off too — this is the
    // "main document is loading" edge the present throttle above is keyed on.
    navLoadBegin();
    g_navSawFirstPaint.store(0, std::memory_order_release);   // M4 load: per navigation
    g_navLoadEventSec.store(0, std::memory_order_relaxed);    // M4 load: pumpLoop's cap anchors
    g_navDomReadySec.store(0, std::memory_order_relaxed);
    if (!g_perfOn || !g_perfInOp || g_perfNavT0Set)
        return;
    g_perfNavT0 = MonotonicTime::now();
    g_perfNavT0Set = true;
}

void perfNavCommit()
{
    // Apotheosis (M3): commit bookkeeping runs even with perf logging off — the load
    // path needs it to decide timeout vs. "committed, just no load event".
    if (g_session)
        g_session->load.committed = true;
    if (!g_perfOn || !g_perfInOp || g_perfCur.netCommit >= 0)
        return;
    g_perfCur.netCommit = perfSinceNavStart();
}

void perfNavDocumentReady()
{
    // Apotheosis (M4 load): perf-independent - pumpLoop's cap-dcl deadline is counted from here.
    if (!g_navDomReadySec.load(std::memory_order_relaxed))
        g_navDomReadySec.store(monotonicSeconds(), std::memory_order_relaxed);
    if (!g_perfOn || !g_perfInOp || g_perfCur.domReady >= 0)
        return;
    g_perfCur.domReady = perfSinceNavStart();
}

void perfNavLoadEvent()
{
    navLoadEnd();   // Apotheosis (M4 load throttle): full present rate from here on (perf-independent)
    // Apotheosis (M4 load): perf-independent - pumpLoop's cap-load deadline is counted from here.
    if (!g_navLoadEventSec.load(std::memory_order_relaxed))
        g_navLoadEventSec.store(monotonicSeconds(), std::memory_order_relaxed);
    if (!g_perfOn || !g_perfInOp || g_perfCur.netLoad >= 0)
        return;
    g_perfCur.netLoad = perfSinceNavStart();
    // M4 load timeline: t_load, plus the subresource count reached by the load event.
    g_perfCur.tLoad = g_perfCur.netLoad;
    g_perfCur.nSubres = g_loadComplete;
}

// t_firstpaint. WebCore fires DidFirstVisuallyNonEmptyLayout from
// LocalFrameView::fireLayoutRelatedMilestonesIfNeeded as soon as the content qualifies as
// visually non-empty - the engine-side answer to "when did something readable appear", and far
// cheaper than scanning pixels. buildSession asks for it via Page::addLayoutMilestones (WebCore
// only tracks milestones a client requested).
void perfNavVisuallyNonEmpty()
{
    // Apotheosis (M4 load throttle): the frame the user is waiting for. Exempt it from the
    // throttle (navLoadThrottleActive) and ask for it right away; the flag is consumed by the
    // composite in WebCoreLiveTick. Perf-independent, like the marks above.
    if (g_navLoading.load(std::memory_order_acquire)) {
        g_navFirstPaintPending.store(1, std::memory_order_release);
        presentRequested();
    }
    // Apotheosis (M4 load): perf-independent, pumpLoop's pre-load cap is keyed on it.
    g_navSawFirstPaint.store(1, std::memory_order_release);
    if (!g_perfOn || !g_perfInOp || g_perfCur.tFirstPaint >= 0)
        return;
    g_perfCur.tFirstPaint = perfSinceNavStart();
}

} // namespace WebCorePort

// Apotheosis (M4): curl's DNS/TCP/TLS/TTFB breakdown for the main resource of the
// current navigation. Called from WebKit's CurlRequest::didReceiveHeader (WK_WINUWP)
// on the main thread, once per response, for VeryHigh-priority requests only — that
// is WebKit's priority for the main document, but subresources can be raised to it
// too, so the FIRST report inside a nav operation wins (that is the main resource:
// nothing else can have finished its headers before it). Only recorded for "nav"
// rows; scroll/tick/click operations have no main resource of their own.
//
// Apotheosis (M4 load waterfall, 2026-09-07): every other response lands here too and feeds
// two things - the net_sub_* aggregate of the perf row, and a small "slowest transfers" table
// that perfWriteStageWaterfall() prints into stage.txt. Both exist to answer the one question
// the old main-resource-only columns could not: on a news front page, whose load event is 6.6 s
// after DOMContentLoaded and whose engine-thread work in that window is ~1.1 s, are the 261
// subresources waiting on the network or on us?
extern "C" void WebCorePortNetTiming(int isMainResource, double dnsMs, double connectMs,
    double tlsMs, double ttfbMs, int httpVersion, const char* url)
{
    if (!g_perfOn || !g_perfInOp || !g_perfOpIsNav)
        return;
    if (!isMainResource) {
        netSubAdd(dnsMs, connectMs, tlsMs, ttfbMs, httpVersion, url);
        return;
    }
    if (g_perfCur.netTtfb >= 0)   // first main-resource report of this navigation wins
        return;
    // M4 load timeline: t_firstbyte. The response headers of the main resource are in hand
    // right now, so the wall clock since the provisional load started IS time-to-first-byte as
    // the navigation experienced it - net_ttfb below is curl's own per-transfer number and
    // excludes everything WebKit did before the request reached curl.
    g_perfCur.tFirstByte = perfSinceNavStart();
    g_perfCur.netDns = dnsMs;
    g_perfCur.netConnect = connectMs;
    g_perfCur.netTls = tlsMs;
    g_perfCur.netTtfb = ttfbMs;
    g_perfCur.httpVer = httpVersion;
}

// ==================== JS console -> LocalState\console.txt =================
// Apotheosis: buffered mirror of PortChromeClient::addMessageToConsole() to disk — the JS-console
// analogue of the perf/crash logs above, reachable without a debugger attached to a headless
// ARM32 App Container. Same opt-in family as perf.txt/imedebug.txt (see WebCoreSetPerfLogPath):
// on when perf logging is on (g_perfOn, reused rather than adding a second harness->driver
// setter) OR when LocalState\console.txt already exists, so a tester who wants console output
// without perf can just drop an empty file. The path is derived once, in WebCoreSetCrashLogPath,
// from the crash log's own directory — crash logging is always armed (not opt-in), so its path is
// the one thing guaranteed known by the time any JS can run, and deriving from it means no third
// harness->driver path setter is needed for this feature.
// One physical line per message: "HH:MM:SS.mmm level source:line message\n", message truncated to
// 512 chars. Buffers in a small ring, same shape as the perf ring; flushes every ~16 lines, on
// session teardown (teardownSession above), and from the crash legs / WebCoreCrashNote /
// WebCorePerfFlush (suspend path) so a dying or backgrounded process does not take the trailing
// console output with it. Engine thread only (PortChromeClient callbacks run on it).
static std::string g_consolePath;
static bool g_consolePathReady = false;
static bool g_consoleFileExisted = false;   // opt-in probe result, computed once when the path becomes known

static bool consoleLoggingEnabled()
{
    return g_consolePathReady && (g_perfOn || g_consoleFileExisted);
}

static constexpr int kConsoleRingSize = 16;
static constexpr int kConsoleRowSize = 600;
// Apotheosis: POD rows, exactly like PerfRow — NOT std::string. Rows are written on the engine
// thread (PortChromeClient::addMessageToConsole) while consoleFlush() can run from the UI thread
// (WebCorePerfFlush on suspend) and from the crash legs (VEH / SIGABRT / terminate, on whatever
// thread died). std::string assignment there means malloc/free on a heap another thread may be
// inside → heap corruption in exactly the situation the log exists to explain. A fixed char array
// only ever races on bytes: a torn row, never a crash.
static char g_consoleRing[kConsoleRingSize][kConsoleRowSize];
static int g_consoleRows = 0;
static std::atomic<int> g_consoleFlushBusy { 0 };   // same re-entrancy guard shape as g_perfFlushBusy

static void consoleFlushLocked()
{
    if (g_consolePath.empty() || !g_consoleRows)
        return;
    FILE* fp = nullptr;
    if (fopen_s(&fp, g_consolePath.c_str(), "ab") != 0 || !fp) {
        g_consoleRows = 0;   // an unwritable path must not make the ring grow forever
        return;
    }
    for (int i = 0; i < g_consoleRows; ++i) {
        g_consoleRing[i][kConsoleRowSize - 1] = '\0';   // paranoia: never fwrite past the row
        std::fwrite(g_consoleRing[i], 1, std::strlen(g_consoleRing[i]), fp);
    }
    std::fclose(fp);
    g_consoleRows = 0;
}

static void consoleFlush()
{
    int expected = 0;
    if (!g_consoleFlushBusy.compare_exchange_strong(expected, 1))
        return;
    consoleFlushLocked();
    g_consoleFlushBusy.store(0);
}

// Called by WebCorePort::consoleLogAppend() below. Plain C strings so PortChromeClient.cpp (which
// calls that bridge) does not need to know about the ring/path statics living in this TU.
static void consoleAppendLine(const char* levelStr, const char* sourceID, unsigned lineNumber, const char* utf8Message)
{
    if (!consoleLoggingEnabled())
        return;
    if (g_consoleRows >= kConsoleRingSize)
        return;   // full and the flush below could not drain it: drop rather than overrun the ring
    SYSTEMTIME st;
    GetLocalTime(&st);
    char* line = g_consoleRing[g_consoleRows];
    // %.128s / %.512s: printf precision truncates for us, no separate strncpy needed. sourceID is
    // a page-controlled URL and used to be unbounded — a long data:/blob: script URL alone could
    // fill the row and push the terminating newline out of it, gluing two console lines together.
    // Cap it, and then append the newline by hand from the *clamped* length so every row ends in
    // exactly one '\n' no matter how snprintf truncated.
    int n = std::snprintf(line, kConsoleRowSize - 1, "%02u:%02u:%02u.%03u %s %.128s:%u %.512s",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        levelStr ? levelStr : "?", sourceID ? sourceID : "", lineNumber, utf8Message ? utf8Message : "");
    if (n < 0)
        return;                                  // encoding error: leave the slot unused
    if (n > kConsoleRowSize - 2)
        n = kConsoleRowSize - 2;                 // truncated: snprintf returns what it *wanted* to write
    line[n] = '\n';
    line[n + 1] = '\0';
    ++g_consoleRows;
    if (g_consoleRows >= kConsoleRingSize)
        consoleFlush();
}

namespace WebCorePort {

// Bridge for PortChromeClient::addMessageToConsole() (PortChromeClient.cpp) — kept as plain C
// strings/ints rather than exposing JSC::MessageSource/MessageLevel or WTF::String here.
void consoleLogAppend(const char* levelStr, const char* sourceID, unsigned lineNumber, const char* utf8Message)
{
    consoleAppendLine(levelStr, sourceID, lineNumber, utf8Message);
}

} // namespace WebCorePort

// ---- Apotheosis (M4 load, 2026-09-07) ------------------------
// Can a pending load still change what the user is looking at?
//
// pumpLoop's "quiet" rule used to be DocumentLoader::isLoadingInAPISense(), i.e. "the frame tree
// has any load in flight at all". On a news site that is never satisfied: the consent and
// analytics stack keeps firing beacons and XHRs for as long as the page is open, so every
// navigation ran to a cap instead. Measured on build-driver\logs\20260907-084407, the news site warm:
// the load event fired at 1645 ms, the pump held the engine thread until 3501 ms, and the six
// transfers still open at that moment were draws / count / raw / get_site_data - trackers, none
// of which paints a pixel.
//
// So judge the loads by type instead. Render-affecting = the main resource, stylesheets, scripts,
// fonts, SVG documents and images; ignored = RawResource (XHR/fetch), Beacon, Ping, LinkPrefetch,
// Icon, media and the rest. Nothing is cancelled either way - what is still running keeps
// streaming into the live tick and repaints there, exactly as after the post-load cap.
//
// Two deliberate simplifications. (1) Only the main frame is inspected: a subframe that is still
// loading is an ad or a social embed far more often than it is the article, and letting one hold
// the pump is the behaviour being removed. (2) "images intersecting the viewport" is approximated
// by "images", because a pending CachedResource cannot be walked back to a renderer cheaply -
// CachedResourceClient carries no geometry. Lazy image loading (C1.1) is what keeps that honest:
// on a page that marks its images the off-screen ones are never requested in the first place, and
// The news site went from 261 subresources to 88 because of it.
//
// Apotheosis (2026-09-07, second pass): images stop blocking once the load event has fired.
// The load event is by definition "every subresource the document declared is done", so an image
// that is still pending after it is a lazy one, a JS-inserted one or a late `srcset` pick - all
// of them below the fold or invisible, none of them worth holding the engine thread for. Before
// the load event images still block: that is the article's own imagery and the first screen.
// Measured motivation: the news site warm fired `load` at 1820 ms and settled at 4124 ms on the cap,
// with only image and beacon traffic left (the new settle-pending line below names it).
static bool apoIsRenderAffecting(WebCore::CachedResource::Type type, bool afterLoadEvent)
{
    switch (type) {
    case WebCore::CachedResource::Type::ImageResource:
        return !afterLoadEvent;
    case WebCore::CachedResource::Type::MainResource:
    case WebCore::CachedResource::Type::CSSStyleSheet:
    case WebCore::CachedResource::Type::Script:
    case WebCore::CachedResource::Type::FontResource:
    case WebCore::CachedResource::Type::SVGFontResource:
    case WebCore::CachedResource::Type::SVGDocumentResource:
        return true;
    default:
        return false;
    }
}

static const char* apoResourceTypeName(WebCore::CachedResource::Type type)
{
    switch (type) {
    case WebCore::CachedResource::Type::MainResource:        return "main";
    case WebCore::CachedResource::Type::ImageResource:       return "img";
    case WebCore::CachedResource::Type::CSSStyleSheet:       return "css";
    case WebCore::CachedResource::Type::Script:              return "js";
    case WebCore::CachedResource::Type::FontResource:        return "font";
    case WebCore::CachedResource::Type::SVGFontResource:     return "svgfont";
    case WebCore::CachedResource::Type::SVGDocumentResource: return "svgdoc";
    default:                                                 return "other";
    }
}

static bool apoHasRenderAffectingLoads(WebCore::LocalFrame& frame, bool afterLoadEvent)
{
    RefPtr<WebCore::Document> doc = frame.document();
    if (!doc)
        return false;
    if (doc->parsing())
        return true;
    RefPtr<WebCore::DocumentLoader> dl = frame.loader().documentLoader();
    if (dl && dl->isLoadingMainResource())
        return true;
    // Same walk as countPendingResources() above: Unknown = not started, Pending = in flight.
    // A few hundred entries once per 50 ms tick, no allocation.
    for (auto& kv : doc->cachedResourceLoader().allCachedResources()) {
        WebCore::CachedResource* res = kv.value.get();
        if (!res)
            continue;
        auto status = res->status();
        if (status != WebCore::CachedResource::Unknown && status != WebCore::CachedResource::Pending)
            continue;
        if (apoIsRenderAffecting(res->type(), afterLoadEvent))
            return true;
    }
    return false;
}

// Apotheosis (M4 load, 2026-09-07): "settle-pending" - what was still open when a settle cap
// fired. The caps exist because the viewport-quiet rule did not trip, and until now nothing said
// what kept it from tripping: on a news site and a tech-news site every navigation of log 20260907-100044
// ended on cap-load / cap-dcl, never on quiet-vp. One line per capped navigation, listing up to
// eight pending render-affecting loads with type, age and the tail of the URL, plus the count of
// the pending loads the rule already ignores. Read it as: many old `img` entries mean the page
// does not mark its images lazy (C1.1 cannot help it, and the after-load rule above should);
// an old `js` entry is a long-poll or a stalled tracker script; an old `css`/`font` entry is a
// genuine reason to keep waiting and would argue for raising the cap instead of lowering it.
static void perfWriteStagePending(WebCore::LocalFrame& frame, const char* why, bool afterLoadEvent)
{
    if (!g_perfOn || g_stagePath.empty())
        return;
    RefPtr<WebCore::Document> doc = frame.document();
    if (!doc)
        return;
    struct Entry { char name[40]; const char* type; double ageMs; };
    Entry entries[8];
    int used = 0, blocking = 0, ignored = 0;
    const WallTime now = WallTime::now();
    for (auto& kv : doc->cachedResourceLoader().allCachedResources()) {
        WebCore::CachedResource* res = kv.value.get();
        if (!res)
            continue;
        auto status = res->status();
        if (status != WebCore::CachedResource::Unknown && status != WebCore::CachedResource::Pending)
            continue;
        if (!apoIsRenderAffecting(res->type(), afterLoadEvent)) {
            ++ignored;
            continue;
        }
        ++blocking;
        const double ageMs = (now - res->apoRequestTimestamp()).milliseconds();
        // Keep the eight oldest: those are the ones that made the cap fire.
        int at = used < 8 ? used : 7;
        if (used == 8 && ageMs <= entries[7].ageMs)
            continue;
        while (at > 0 && entries[at - 1].ageMs < ageMs) {
            entries[at] = entries[at - 1];
            --at;
        }
        netSubShortName(entries[at].name, sizeof entries[at].name, res->url().string().utf8().data());
        entries[at].type = apoResourceTypeName(res->type());
        entries[at].ageMs = ageMs;
        if (used < 8)
            ++used;
    }
    FILE* fp = nullptr;
    if (fopen_s(&fp, g_stagePath.c_str(), "ab") != 0 || !fp)
        return;
    std::fprintf(fp, "settle-pending why=%s parsing=%d blocking=%d ignored=%d oldest=[",
        why ? why : "-", doc->parsing() ? 1 : 0, blocking, ignored);
    for (int i = 0; i < used; ++i)
        std::fprintf(fp, "%s%s age=%.0f %s", i ? " | " : "",
            entries[i].type, entries[i].ageMs, entries[i].name);
    std::fputs("]\n", fp);
    std::fclose(fp);
}

// ---- Apotheosis (M4 load, 2026-09-07): DOM timer throttle -------------
// Between the commit of a real document and t_settle, align every DOM timer of every document
// onto a 250 ms grid (1 s for the maximally nested ones WebCore already treats as pollers). The
// engine thread is the only thread there is here, and during a load it has to serve the parser,
// the first style resolution and the first paint *and* the ad/consent stack's setTimeout chains:
// the ms_offpump split of a warm news-site load is timers=1030 js=867 style=911 of 2508 ms.
// Alignment does not drop a timer and delays none by more than one grid step; it makes a burst
// of unrelated timers land in one wake-up, so the page pays one script entry and one style
// resolution for the lot. See wtf/ApoLoadThrottle.h for the engine half.
//
// The window is opened and closed by pumpLoop alone, so it cannot outlive the navigation. User
// input cannot be starved by it either: while the pump owns the engine thread no C ABI call is
// serviced at all, so by the time input is processed the window is already closed.
static void loadTimerThrottleSet(WebCore::Page* page, bool on)
{
    const unsigned align = on ? kLoadTimerAlignMs : 0;
    if (WTF::g_apoLoadTimerAlignMs.load(std::memory_order_relaxed) == align)
        return;
    WTF::g_apoLoadTimerAlignMs.store(align, std::memory_order_relaxed);
    WTF::g_apoLoadTimerNestedAlignMs.store(on ? kLoadTimerNestedAlignMs : 0, std::memory_order_relaxed);
    // Re-align what is already scheduled - without this the new grid would only apply to timers
    // installed after the edge, which on the closing edge means the throttle lingers.
    if (page)
        page->forEachDocument([](WebCore::Document& document) {
            document.didChangeTimerAlignmentInterval();
        });
}

// 轮询 RunLoop 直到活动文档空闲(涵盖图片/脚本/XHR)或封顶。timers 为调用局部量,返回前销毁。
//  mainDone: 指向"主文档已完成"标志的指针(可空 → 无导航语义,只看加载活动)。
//  allowEarlyStopWithoutNav: 若未发生导航完成,连续 ~0.5s 无加载活动即停(点击/滚动用)。
//  settleCapTicks: 导航完成后的最大额外轮询数(×50ms)。Apotheosis (M4 settle): 一旦 load 事件到达,
//    内部再收紧到 20 tick(1s)并只要 300ms 静默即停 —— 见下方 quietNeeded/capTicks。
//  pageForRendering: 非空则每 tick 调 isolatedUpdateRendering 驱动 rAF/IntersectionObserver(懒加载/SPA 必需)。
static void pumpLoop(WebCore::LocalFrame& frame, const bool* mainDone, bool allowEarlyStopWithoutNav,
                     int settleCapTicks, double watchdogSeconds, WebCore::Page* pageForRendering)
{
    using namespace WebCore;
    // Apotheosis (M4 load): the ms_offpump split is accounted only while a
    // pump is running - that is exactly the window ms_offpump measures. Saved and restored rather
    // than cleared, because isolatedUpdateRendering() can reach a nested pump. See
    // wtf/ApoLoadPhase.h; with perf logging off this stays false and every scope is a branch.
    const bool apoPhaseWasOn = WTF::g_apoPhaseEnabled;
    WTF::g_apoPhaseEnabled = g_perfOn;
    bool stopped = false;
    // Apotheosis (M4 load, 2026-09-07): every stop takes a reason, recorded in g_settleWhy and
    // printed as why= in the stage.txt timeline / settle_why in perf.csv. A stop on one of the
    // caps also dumps what was still pending (settle-pending), because a cap is by definition
    // "the quiet rule never tripped" and that is the thing to fix next.
    RefPtr<LocalFrame> frameForStop = &frame;
    auto stopLoop = [&stopped, frameForStop](const char* why) {
        if (stopped)
            return;
        stopped = true;
        std::snprintf(g_settleWhy, sizeof g_settleWhy, "%s", why);
        if (frameForStop && why && (!std::strncmp(why, "cap", 3) || !std::strcmp(why, "watchdog")))
            perfWriteStagePending(*frameForStop, why,
                g_navLoadEventSec.load(std::memory_order_relaxed) != 0);
        RunLoop::currentSingleton().stop();
    };
    int settleTicks = 0;
    int quietTicks = 0;
    // Apotheosis (M4 load, 2026-09-07): the tick counters below were written as time budgets
    // ("20 ticks = 1 s") but a tick is 50 ms *plus* whatever isolatedUpdateRendering costs, and
    // on a heavy page that is 100-200 ms. Measured on the Lumia (logs 20260907-023940): a news site
    // warm fired its load event at 1716 ms and the pump did not return until 4849 ms - 3.1 s of
    // "1 s hard cap"; a map site 2034 -> 3427 ms. Deadlines are therefore wall clock now,
    // and the tick counts stay only as the secondary guard they always were.
    MonotonicTime navDoneAt;      // the load event (fallback: first tick that saw it)
    MonotonicTime domReadyAt;     // DOMContentLoaded (fallback: first tick that saw it)
    // Apotheosis (M4 load, 2026-09-07): ms_tick_cb / ms_offpump accounting - see kPerfHeader.
    MonotonicTime lastTickStart;
    double cbTotalAtLastTick = 0;
    bool timerThrottleArmed = false;   // Apotheosis (M4 load C2.9): see loadTimerThrottleSet()
    RefPtr<LocalFrame> frameRef = &frame;
    RunLoop::Timer settle(Ref { RunLoop::currentSingleton() }, "WebCorePort.pump.settle"_s,
        WTF::Function<void()> { [&stopLoop, &settleTicks, &quietTicks, &navDoneAt, &domReadyAt, &lastTickStart, &cbTotalAtLastTick, &timerThrottleArmed, frameRef, mainDone, allowEarlyStopWithoutNav, settleCapTicks, pageForRendering] {
            // Apotheosis (M4 load C2.1): this callback is already measured as ms_tick_cb.
            // Opening the Pump scope keeps the RunLoop Timer scope that contains it from
            // charging itself our work, so the buckets describe the *other* run-loop work only.
            WTF::ApoPhase apoPumpPhase(WTF::ApoPhaseBucket::Pump);
            // Apotheosis (M4 load): PerfPhase accumulates this callback's own duration into
            // ms_tick_cb on every exit path; the block below turns the spacing between two
            // callbacks into ms_offpump = run-loop work that was not ours.
            PerfPhase perfTickCb(&g_perfCur.msTickCb);
            if (g_perfOn) {
                const MonotonicTime tickStart = MonotonicTime::now();
                const double cbTotal = g_perfCur.msTickCb < 0 ? 0.0 : g_perfCur.msTickCb;
                if (lastTickStart) {
                    const double period = (tickStart - lastTickStart).milliseconds();
                    const double prevCb = cbTotal - cbTotalAtLastTick;
                    // The timer re-arms from the START of the previous callback (RunLoopGeneric
                    // ScheduledTask::fired -> updateReadyTime), so an idle loop gives a period of
                    // max(50 ms, previous callback). Anything beyond that ran on the run loop
                    // between the two ticks: WebCore timers, the parser, loader dispatches.
                    const double expected = prevCb > 50.0 ? prevCb : 50.0;
                    const double off = period - expected;
                    if (off > 0)
                        g_perfCur.msOffPump = (g_perfCur.msOffPump < 0 ? off : g_perfCur.msOffPump + off);
                }
                lastTickStart = tickStart;
                cbTotalAtLastTick = cbTotal;
                ++g_perfPumpTicks;   // Apotheosis: settle ticks of the current operation (M4)
            }
            // EmptyChromeClient 下自动 RenderingUpdateScheduler 是 no-op;不显式调它则 rAF /
            // IntersectionObserver / 懒加载图片永不触发(滚动加载与 SPA 渲染必需)。
            if (pageForRendering) {
                {
                    PerfPhase perfRender(&g_perfCur.renderUpdate);
                    pageForRendering->isolatedUpdateRendering();   // 跑 rAF/IntersectionObserver(可能跑 JS 改 DOM 甚至导航)
                }
                // isolatedUpdateRendering 可能因导航替换主帧;若已不是当初那帧,本轮停止(调用方随后重取帧),
                // 避免在同一 tick 里对脱离的 frameRef->loader() 解引用半毁状态。
                RefPtr<LocalFrame> mf = pageForRendering->localMainFrame();
                if (mf.get() != frameRef.get()) {
                    stopLoop("frame-gone");
                    return;
                }
            }
            // 排微任务:推进 promise 图(ES module 加载/求值这条异步链靠它 + 下方 0_s WebCore 定时器,
            // 只要 RunLoop 持续转就会触发)。isolatedUpdateRendering 自身不排微任务、不跑事件循环任务。
            if (RefPtr<Document> doc = frameRef->document())
                doc->eventLoop().performMicrotaskCheckpoint();

            RefPtr<DocumentLoader> dl = frameRef->loader().activeDocumentLoader();
            bool loading = dl && dl->isLoadingInAPISense();
            bool navDone = mainDone && *mainDone;
            // Apotheosis (M3): some main-frame loads commit and go idle without ever
            // dispatching a load event — ImageDocument (image URL in the main frame),
            // and occasionally an HTML page hit by the defer/cancel bug. *mainDone then
            // stays false forever and only the watchdog ends the pump (30 s of dead UI,
            // then kErrLoadTimeout). Accept "committed document loader, no longer
            // loading, parser finished" as a second readiness signal. It only opens the
            // *gate*: the quiet-tick hysteresis below still has to be satisfied, so a
            // live load (which keeps the loader busy) cannot be cut short by this.
            RefPtr<DocumentLoader> committedLoader = frameRef->loader().documentLoader();
            RefPtr<Document> frameDoc = frameRef->document();
            // Apotheosis (M4 settle): "parser finished on a committed document" = DOMContentLoaded.
            // The loader may still be pulling subresources; the quiet-tick hysteresis below decides.
            //
            // Apotheosis (2026-09-07): committedFirstRealDocumentLoad() is not decoration. Until
            // the navigation commits, loader().documentLoader() is still the *initial empty
            // document* that LocalFrame::init() created - committed, parsed, and holding no
            // resources - so this expression was true on the very first tick of every load. That
            // anchored cap-dcl at the start of the pump instead of at DOMContentLoaded (a tech-news site
            // in log 20260907-100044: dcl at 2390 ms, cap-dcl at 3184 ms, i.e. 0.8 s after a
            // "3 s" cap), and with the viewport-quiet rule of 831097a it could also let a
            // navigation settle on the empty document's zero pending resources after 500 ms.
            bool realDocCommitted = committedLoader && committedLoader->isCommitted()
                && frameRef->loader().stateMachine().committedFirstRealDocumentLoad();
            bool domReady = realDocCommitted && frameDoc && !frameDoc->parsing();
            // Apotheosis (M4 load): the DOM timer window opens at the
            // commit of the real document - the parse is exactly where the ad timers hurt most -
            // and closes when this pump returns. Navigation pumps only.
            // The apoLoadTimerThrottleActive() term keeps a nested pump (isolatedUpdateRendering
            // can reach one) from taking ownership of a window the outer pump opened and then
            // closing it early - same reasoning as the g_apoPhaseEnabled save/restore above.
            if (!timerThrottleArmed && realDocCommitted && !allowEarlyStopWithoutNav
                && !WTF::apoLoadTimerThrottleActive()) {
                timerThrottleArmed = true;
                loadTimerThrottleSet(pageForRendering, true);
            }
            bool ready = navDone || allowEarlyStopWithoutNav || domReady;

            // Apotheosis (M4 settle): how long the pump keeps the UI hostage after the page is
            // usable. It used to be one rule for everything - "the loader has been quiet for 16
            // ticks (0.8 s)", capped at settleCapTicks (160 = 8 s) after the load event. A page
            // with hundreds of subresources never gives us 0.8 s of quiet in a row (a news site: 255
            // subresources), so every navigation ran to the cap: stage.txt showed load at 1.4 s
            // and settle at 4.8-6.0 s, i.e. ~4 s of spinner after the page was done. The load
            // event is the point at which the *page* calls itself loaded, so make it ours too:
            //   load event fired  -> 300 ms of idle, hard cap 1 s after the event
            //   no load event yet -> DOMContentLoaded + 500 ms without any loading activity
            //   neither (click/type pumps with allowEarlyStopWithoutNav) -> unchanged 0.8 s
            // Everything still in flight keeps streaming into the live tick and repaints there.
            int quietNeeded = 16;                                    // 0.8 s (unchanged default)
            int capTicks = settleCapTicks;
            if (navDone) {
                quietNeeded = 6;                                     // 300 ms of idle after load
                capTicks = settleCapTicks < 20 ? settleCapTicks : 20; // and 1 s hard cap
            } else if (domReady)
                quietNeeded = 10;                                    // 500 ms after DOMContentLoaded

            // Apotheosis (M4 load, 2026-09-07): wall-clock deadlines. Two of them.
            //
            // (1) after the load event: the "1 s hard cap" above is 20 ticks, and a tick on a
            //     heavy page is 150-200 ms, so it was really 3 s. Make it 1 s of real time.
            //
            // (2) before the load event: nothing bounded this phase at all except the 30 s
            //     watchdog, and on a page whose load event waits for hundreds of images it is
            //     the whole problem. The news site cold (same log): DOMContentLoaded 2349 ms, first
            //     paint 1311 ms, load event 8961 ms - 6.6 s in which the page was already on
            //     screen but every C ABI call (scroll, tap, the next navigation) queued behind
            //     this pump on the engine thread. That is the "initial page load is still quite
            //     slow" the user reports; the pixels were there at 1.3 s.
            //     So: once the document has parsed AND something readable has been painted,
            //     give the rest of the subresource cascade 3 s and then hand the page over.
            //     Nothing is cancelled - the loads keep running and keep repainting through
            //     WebCoreLiveTick, exactly as they already do after the post-load cap fires.
            //     3 s (not 1) because a page that is merely slow to finish should still finish
            //     inside the pump; this is meant to catch the pathological tail, and settle_why
            //     = cap-dcl in the next log says how often it does.
            //
            // Apotheosis (2026-09-07, second pass): and anchor them on the *events*, not on the
            // tick that noticed them. A tick is 50 ms plus the callback plus whatever the run
            // loop did in between, which on a news site averages 160 ms and exceeds a second when an
            // ad timer chain runs, so "first tick that saw the load event" was up to a full gap
            // late and the 1 s cap measured 2.3 s (log 20260907-100044: load 1820, cap-load
            // 4124). perfNavLoadEvent / perfNavDocumentReady now stamp the wall clock at the
            // event itself; the tick time stays as the fallback for a pump that has no
            // navigation of its own (click/type). The deadline is still only *checked* on a tick,
            // so a run loop blocked inside one long script still overshoots by that script - that
            // is C2.1's problem, not this one's.
            const MonotonicTime nowT = MonotonicTime::now();
            const double navEventSec = g_navLoadEventSec.load(std::memory_order_relaxed);
            const double domEventSec = g_navDomReadySec.load(std::memory_order_relaxed);
            if (navDone && !navDoneAt) {
                navDoneAt = navEventSec ? MonotonicTime::fromRawSeconds(navEventSec) : nowT;
                if (navDoneAt > nowT)
                    navDoneAt = nowT;
            }
            if (domReady && !domReadyAt) {
                domReadyAt = domEventSec ? MonotonicTime::fromRawSeconds(domEventSec) : nowT;
                if (domReadyAt > nowT)
                    domReadyAt = nowT;
            }
            if (navDone && navDoneAt && (nowT - navDoneAt) > 1_s) {
                stopLoop("cap-load");
                return;
            }
            if (!navDone && domReadyAt && (nowT - domReadyAt) > 3_s
                && g_navSawFirstPaint.load(std::memory_order_acquire)) {
                stopLoop("cap-dcl");
                return;
            }

            // ★ 关键:绝不在 isLoadingInAPISense 一转 false 就停。模块求值(<script type=module>)和
            //   重定向后最终文档的样式表应用都发生在"加载器空闲之后",经 ScriptRunner/WindowEventLoop 的
            //   0_s 定时器 + 微任务级联触发——这要求 RunLoop 继续转若干 tick。改为"加载器持续静默 ~0.8s
            //   才停":期间求值/挂载会引发新活动(渲染/拉字体),重置静默计数,自然等到真稳定。
            // Apotheosis (M4 load, 2026-09-07): once the document has parsed, "quiet" means the
            // viewport is quiet - see apoHasRenderAffectingLoads() above. Before that, and for the
            // click/type pumps that have no navigation semantics, the old all-loads rule stands.
            // Apotheosis (2026-09-07, second pass): after the load event images no longer count -
            // everything the document declared is done by then, so what is left is lazy or
            // script-inserted and belongs to the live tick. See apoIsRenderAffecting().
            bool loadingForQuiet = loading;
            if (loading && !allowEarlyStopWithoutNav && (navDone || domReady))
                loadingForQuiet = apoHasRenderAffectingLoads(*frameRef, navDone);
            if (loadingForQuiet)
                quietTicks = 0;
            else
                ++quietTicks;
            if (navDone)
                ++settleTicks;
            if (ready && quietTicks >= quietNeeded) {    // 加载器静默 → 异步级联已跑完,停
                // "quiet-vp": the loader is still busy, but only with things that cannot change
                // the viewport. Distinguished from "quiet" so the device log says how often the
                // new rule is what ended the navigation.
                stopLoop(loading ? "quiet-vp" : "quiet");
                return;
            }
            if (navDone && settleTicks > capTicks)        // 硬封顶,防长连接/永久活动拖到看门狗
                stopLoop("cap-ticks");
        } });
    settle.startRepeating(0.05_s);
    RunLoop::Timer watchdog(Ref { RunLoop::currentSingleton() }, "WebCorePort.pump.watchdog"_s,
        WTF::Function<void()> { [&stopLoop] { stopLoop("watchdog"); } });
    watchdog.startOneShot(WTF::Seconds(watchdogSeconds));
    RunLoop::run();
    settle.stop();
    watchdog.stop();
    // Apotheosis (M4 load C2.9): t_settle closes the DOM timer window. Unconditional, so the
    // alignment cannot survive a pump that ended on the watchdog or on frame-gone.
    if (timerThrottleArmed)
        loadTimerThrottleSet(pageForRendering, false);
    WTF::g_apoPhaseEnabled = apoPhaseWasOn;   // M4 load C2.1: see the top of this function
    // Apotheosis (M4 load throttle): catch-all end of the loading window. The load event
    // normally ends it (perfNavLoadEvent); this covers the pump that stopped on the settle
    // cap, the watchdog or "committed but no load event", so a page can never leave the
    // driver with presents still throttled.
    navLoadEnd();
}

// ★ 打字/退格专用轻量 settle:pumpLoop 的"加载器连续静默 16 tick(≈0.8s)才停"是为导航场景设计
//   (等模块脚本求值/重定向后样式表落地),按键场景没有导航却也套用这个门槛 → 每敲一下键最少卡
//   ~0.8s(真机实测:打字"能进框了但更新巨慢",越打越积压)。这里只推进 1~2 轮微任务/渲染更新
//   (够把 input 事件里的同步 JS 落地),不等"静默"。异步跟随效果(防抖搜索等 setTimeout 回调)
//   交给随后 200ms 一次的实时 tick(StartLiveMode/WebCoreLiveTick)在后续帧自然补上。
static void pumpQuick(WebCore::LocalFrame& frame, WebCore::Page* pageForRendering)
{
    using namespace WebCore;
    for (int i = 0; i < 2; ++i)
        RunLoop::cycle();
    if (pageForRendering)
        pageForRendering->isolatedUpdateRendering();
    if (RefPtr<Document> doc = frame.document())
        doc->eventLoop().performMicrotaskCheckpoint();
}

// ============================ M2 GPU 合成 recipe ============================
// 递归把整棵 GraphicsLayer 标脏(setNeedsDisplay)。同步 TextureMapper 路径下,内容 tile 仅在 m_needsDisplay/
// m_needsDisplayRect 非空时才被 updateBackingStoreIfNeeded 重绘;而页面布局产生的脏区在 pumpLoop 的若干次
// rendering-update 中已被消费,轮到我们手动合成时内容层已"干净"→ tile 空 → 内容根本没画进 readback(真机实测
// contentPx≈0、整屏只剩 clearColor 背景)。合成前强制全树标脏,确保每帧内容都重绘上传。drawsContent=false 的层
// setNeedsDisplay 内部直接返回,无害。
static void forceDirtyTree(WebCore::GraphicsLayer& l)
{
    l.setNeedsDisplay();
    for (const auto& c : l.children())
        forceDirtyTree(c.get());
    if (auto* r = l.replicaLayer())
        forceDirtyTree(*r);
    if (auto* m = l.maskLayer())
        m->setNeedsDisplay();
}

// 把已提交的图层变更刷进 TextureMapperLayer 树、上传脏 tile、推进动画。调用前 g_glContext 已 current。
// 仿 WCScene::update 的顺序(同步 GraphicsLayerTextureMapper 路径)。
static void gpuPrepare(WebCore::LocalFrameView& view, WebCore::GraphicsLayerTextureMapper& glRoot)
{
    using namespace WebCore;
    // flushCompositingStateForThisFrame 在 needsLayout() 时直接返回不 flush → 先确保布局就绪。
    {
        PerfPhase perfLayout(&g_perfCur.styleLayout);
        // Apotheosis (M4): run the pending compositing-geometry update in the same layout
        // (as Page::updateRendering does). Without the option it is deferred to the next
        // tick, whose RenderLayerBacking::updateGeometry then dirties whole layers ->
        // heavy/light ping-pong between consecutive ticks on animated pages.
        view.updateLayoutAndStyleIfNeededRecursive({ WebCore::LayoutOptions::UpdateCompositingLayers });
    }
    // 文档/base 背景:合成路径下不会自动进图层(无 embedder 给根层设背景色)→ 离屏 FBO 透出底白,
    // 任何页面背景都丢。显式把文档背景色设到根层(TextureMapperLayer::paintSelf 会以纯色渲染有效
    // backgroundColor)。只解决纯色/base 背景;body 背景图仍靠各自元素图层的 backing(若仍缺另议)。
    {
        Color docBg = view.documentBackgroundColor();
        glRoot.setBackgroundColor(docBg.isValid() ? docBg : Color::white);
    }
    {
        PerfPhase perfFlushPhase(&g_perfCur.flush);        // M4: compositing flush (+ scroll-layer positioning)
        // Apotheosis (C2): position the scrolled-contents layer *before* the flush as well.
        // GraphicsLayerTextureMapper::flushCompositingState() now derives every layer's visible
        // rect from the layer positions it walks over, so with -scrollPosition applied only
        // after the flush each visible rect would lag one tick behind and tiles would be created
        // for the previous viewport (blank strips while scrolling). The call after the flush
        // stays the authoritative one for compositing.
        if (auto* renderViewBeforeFlush = view.renderView())
            renderViewBeforeFlush->compositor().frameViewDidScroll();
        view.flushCompositingStateIncludingSubframes();    // GraphicsLayer 变更 → TextureMapperLayer 树(递归全帧)
        // 同步 TextureMapper 路径(无 async scrolling):主帧滚动靠 compositor 把 -scrollPosition 设到
        // scrolled-contents 层(updateScrollLayerPosition)。★ 必须在 flush 之后:flush 内的合成几何更新会按
        // 当时状态重置滚动层位置,放在 flush 前会被它覆盖 → 画面不滚。这里在 flush 后、paint 前显式定位一次,
        // 让 -scrollPosition 成为合成前对 scrolled-contents 层的最后一次定位。无滚动层时为 no-op。
        if (auto* renderView = view.renderView())
            renderView->compositor().frameViewDidScroll();
    }
    {
        PerfPhase perfBacking(&g_perfCur.backing);         // M4: dirty-tree + tile upload + animations
        // 滚动帧跳过强制全树重绘:滚动不改内容,tile 早已画好,只需移动滚动层重新合成 → 避免每帧重画所有 tile
        //   (长页尤其卡)。加载/点击/输入/动画帧仍全量重绘保正确。g_gpuScrollFast 由 WebCoreScrollBy 置位、此处消费。
        // Apotheosis (WHITE-AT-SCROLL-END): g_gpuForceFullNext is the recovery lever - a composite
        // that published nothing because it drew nothing asks the next one to force-dirty, whatever
        // the caller wanted. g_gpuLastCompositeFull tells gpuPresent whether this frame is allowed
        // to be trusted when it comes out empty (a force-dirtied tree that paints background IS
        // background; a fast-path tree that paints background has probably lost its tiles).
        // Apotheosis (2026-09-06): g_gpuTargetedNext is the same "do not force-dirty" statement as
        // g_gpuScrollFast, made by a caller that is not riding a scroll job - the end of a pan
        // gesture and the unpainted-tile repair below. Both want the composite that follows to
        // repaint the stores that have actually lost pixels and nothing else; the per-layer
        // detection in GraphicsLayerTextureMapper::updateBackingStoreIfNeeded() is what decides,
        // and updateBackingStoreIncludingSubLayers() runs for a targeted composite just as it does
        // for a forced one, so a real invalidation (m_needsDisplay/m_needsDisplayRect) is repainted
        // either way.
        const bool wkTargeted = g_gpuScrollFast || g_gpuTargetedNext;
        const bool wkFullDirty = !wkTargeted || g_gpuForceFullNext;
        if (wkFullDirty)
            forceDirtyTree(glRoot);                                     // 强制全树标脏,否则脏区已被消费 → 内容 tile 空
        g_gpuScrollFast = false;
        g_gpuTargetedNext = false;
        g_gpuForceFullNext = false;
        g_gpuLastCompositeFull = wkFullDirty;
        glRoot.updateBackingStoreIncludingSubLayers(*g_textureMapper);  // 上传脏 tile 内容到 GL 纹理(递归)
        {
            // Apotheosis (M4): how many layers were repainted in full vs. by dirty rect (accumulates per op).
            unsigned full = 0, partial = 0;
            WebCore::wkWinUWPTexmapDirtyStats(full, partial);
            if (g_perfOn) {
                if (g_perfCur.dirtyFull < 0) { g_perfCur.dirtyFull = 0; g_perfCur.dirtyPartial = 0; }
                g_perfCur.dirtyFull += static_cast<int>(full);
                g_perfCur.dirtyPartial += static_cast<int>(partial);
            }
        }
        {
            // Apotheosis (2026-09-08, docs/TILEGRID-DESIGN.md section 3): per-cause breakdown of
            // the full-repaint count above, plus whether THIS composite is the one forceDirtyTree()
            // just walked (g_gpuLastCompositeFull, set two lines above from wkFullDirty) - together
            // they tell "WebCore dirtied it" apart from "forceDirtyTree ran", which is what the
            // a code-hosting site's dirty_full=54 rows need (the repair-escalation cap, 9df4438, never fired for
            // them, so something else is force-dirtying the tree). Accumulated across this
            // operation like dirtyFull/dirtyPartial above; dirtySrcForceDirty is an OR, not a sum -
            // one force-dirtied composite in the operation is enough to answer "did it happen".
            unsigned nd = 0, sc = 0, sz = 0, scl = 0, snd = 0;
            WebCore::wkWinUWPTexmapDirtySrcStats(nd, sc, sz, scl, snd);
            if (g_perfOn) {
                if (g_perfCur.dirtySrcNeedsDisplay < 0) {
                    g_perfCur.dirtySrcNeedsDisplay = 0;
                    g_perfCur.dirtySrcStoreCreated = 0;
                    g_perfCur.dirtySrcSizeChange = 0;
                    g_perfCur.dirtySrcScaleChange = 0;
                    g_perfCur.dirtySrcSetNeedsDisplay = 0;
                    g_perfCur.dirtySrcForceDirty = 0;
                }
                g_perfCur.dirtySrcNeedsDisplay += static_cast<int>(nd);
                g_perfCur.dirtySrcStoreCreated += static_cast<int>(sc);
                g_perfCur.dirtySrcSizeChange += static_cast<int>(sz);
                g_perfCur.dirtySrcScaleChange += static_cast<int>(scl);
                g_perfCur.dirtySrcSetNeedsDisplay += static_cast<int>(snd);
                if (g_gpuLastCompositeFull)
                    g_perfCur.dirtySrcForceDirty = 1;
            }
        }
        g_gpuAnimating = glRoot.layer().applyAnimationsRecursively(MonotonicTime::now()); // 推进动画到当前时刻;返回值=仍有动画在跑
    }
}

// 离屏合成 + 读回:把图层树合成进 w*h 的 BitmapTexture(FBO),glReadPixels 出 RGBA 到 outRGBA。
// 顺带统计非白像素 + 帧哈希(与 cairo 路径一致,供实时循环/诊断)。返回 kOK / 负错误码。
static int gpuCompositeReadback(WebCore::LocalFrameView& view, int w, int h,
                                WebCore::GraphicsLayer& root, uint8_t* outRGBA, int& nonWhiteOut)
{
    using namespace WebCore;
    if (!g_glContext || !g_textureMapper)
        return kErrNoView;
    g_glContext->makeContextCurrent();
    auto& glRoot = static_cast<GraphicsLayerTextureMapper&>(root);
    gpuPrepare(view, glRoot);

    Ref<BitmapTexture> texture = BitmapTexture::create(IntSize(w, h),
        { BitmapTexture::Flags::SupportsAlpha, BitmapTexture::Flags::DepthBuffer });
    Color docBg = view.documentBackgroundColor();
    if (!docBg.isValid())
        docBg = Color::white;
    g_textureMapper->beginPainting(TextureMapper::FlipY::No, texture.ptr());   // 绑 texture 的 FBO + 设视口
    // 文档 base 背景:根层 0×0、合成路径不把"传播到视口的 body/html 背景色"画进任何图层 → FBO 透出
    // 透明黑(bindAsSurface 清的)→ readback 后呈白。这里在 paint 前用文档背景色清整张 FBO(此刻 scissor
    // 已是全表面)。documentBackgroundColor 已混合 base+html+body 纯色;背景图无法纳入(见 LocalFrameView
    // 注释),故纯色页背景就此修复,背景图仍待其元素图层自身绘制。
    g_textureMapper->clearColor(docBg);
    {
        PerfPhase perfPaint(&g_perfCur.paint);   // M4: TextureMapper composite of the layer tree
        glRoot.layer().paint(*g_textureMapper);
    }
    // Apotheosis (docs/TILEGRID-DESIGN.md section 3): this composite added to the engine's
    // visible-hole accumulator like any other, and nothing here would ever read it - so gpuPresent()
    // would find this readback's holes on top of its own and owe a present for a frame the user is
    // not looking at. Drain and discard: the readback is a snapshot (a tab thumbnail, the offscreen
    // validation path), not the screen, and a hole in it is not a reason to composite again.
    wkWinUWPTexmapVisibleHoles();
    // texture 的 FBO 此刻仍绑定 → 直接读回(endPainting 会还原帧缓冲绑定,故必须读在前)。
    // 读回缓冲静态复用:仅引擎线程用,免每帧 3MB 分配+释放(readback 模式滚动/实时 tick 是热路径)。
    static std::vector<uint8_t> tmp;
    tmp.resize(static_cast<size_t>(w) * h * 4);
    PerfPhase perfReadback(&g_perfCur.readback);   // M4: glFinish + glReadPixels + the copy/hash pass below
    glFinish();
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, tmp.data());
    g_textureMapper->endPainting();

    // 取像素 + 方向校正(g_gpuFlipH/V)+ 非白统计 + 内容像素统计(与背景色不同,诊断内容是否合成进来)+ 帧哈希。
    auto [bgrf, bggf, bgbf, bgaf] = docBg.toColorTypeLossy<SRGBA<float>>().resolved();
    const int bgR = (int)(bgrf * 255 + 0.5f), bgG = (int)(bggf * 255 + 0.5f), bgB = (int)(bgbf * 255 + 0.5f);
    int nonWhite = 0, contentPx = 0;
    uint32_t hash = 2166136261u;
    for (int y = 0; y < h; ++y) {
        const uint8_t* srow = tmp.data() + static_cast<size_t>(g_gpuFlipV ? (h - 1 - y) : y) * w * 4;
        uint8_t* drow = outRGBA + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            const uint8_t* s = srow + static_cast<size_t>(g_gpuFlipH ? (w - 1 - x) : x) * 4;
            const uint8_t r = s[0], g = s[1], b = s[2], a = s[3];
            drow[x * 4 + 0] = r; drow[x * 4 + 1] = g; drow[x * 4 + 2] = b; drow[x * 4 + 3] = a;
            if (r != 255 || g != 255 || b != 255)
                ++nonWhite;
            if (std::abs((int)r - bgR) + std::abs((int)g - bgG) + std::abs((int)b - bgB) > 24)
                ++contentPx;
            if (((x | y) & 3) == 0) {
                hash = (hash ^ r) * 16777619u;
                hash = (hash ^ g) * 16777619u;
                hash = (hash ^ b) * 16777619u;
            }
        }
    }
    nonWhiteOut = nonWhite;
    g_lastContentPx = contentPx;
    g_lastFrameHash = hash;
    return kOK;
}

// Apotheosis: paint the layer tree into whatever framebuffer is bound.
static void gpuPaintTree(WebCore::GraphicsLayerTextureMapper& glRoot)
{
    glRoot.layer().paint(*g_textureMapper);
}

// 直呈现:把图层树合成进默认帧缓冲(GpuInit 绑的窗口表面)并 eglSwapBuffers。返回 kOK / 负错误码。
static int gpuPresent(WebCore::LocalFrameView& view, int w, int h, WebCore::GraphicsLayer& root)
{
    using namespace WebCore;
    if (!g_glContext || !g_textureMapper)
        return kErrNoView;
    g_glContext->makeContextCurrent();
    auto& glRoot = static_cast<GraphicsLayerTextureMapper&>(root);
    gpuPrepare(view, glRoot);

    glViewport(0, 0, w, h);
    g_textureMapper->beginPainting(TextureMapper::FlipY::No, nullptr);   // nullptr → 默认帧缓冲
    {
        Color docBg = view.documentBackgroundColor();
        g_textureMapper->clearColor(docBg.isValid() ? docBg : Color::white);   // 文档 base 背景(同 readback,见上)
    }
    {
        PerfPhase perfPaint(&g_perfCur.paint);      // M4: TextureMapper composite into the default framebuffer
        gpuPaintTree(glRoot);
        g_textureMapper->endPainting();
    }
    // Apotheosis (2026-09-08, docs/TILEGRID-DESIGN.md 2.2b "driver repair machine" and 3): the
    // present-only repair. The TileGrid model answers one question after a composite - how many
    // VISIBLE cells drew nothing and had no backdrop behind them - and a cell can only be in that
    // state while its replay is in flight or its texture is waiting for the upload budget, i.e.
    // while work the model has already scheduled is running. R5 and R12 guarantee that work
    // converges, so the only thing missing is a frame to show the result in: exactly one more
    // composite. No dirtying (dirtying is what made the v1 repair loop feed itself, 9df4438), no
    // second paint in this call, no cooldown, no escalation ledger - two states, Settled and
    // PresentOwed.
    //
    // The reading also drains the engine-side accumulator, so it happens on every composite. A v1
    // store never adds to it, so with the switch off this is a read of a zero.
    {
        const unsigned holes = wkWinUWPTexmapVisibleHoles();
        if (g_perfOn) {
            if (g_perfCur.tgHoles < 0)
                g_perfCur.tgHoles = 0;
            g_perfCur.tgHoles += static_cast<int>(holes);
        }
        // Apotheosis (device round 1, 0.1.9.29): PresentOwed has a bound. R12 promises that a
        // constant input converges, and the guard above is written as if it always does - but a
        // store whose hole count is *stuck* (in 0.1.9.29: a store with an unknown visible rect
        // reporting holes over cells beyond its own budget) turned "holes > 0 => one more
        // composite" into a busy loop on a page at rest: 2749 perf rows with tg_holes=3 and
        // nothing on screen changing. So the composite is owed only while the number is still
        // moving, or for kTgMaxOwedComposites in a row after it stopped moving; after that the
        // driver goes quiet until something actually changes. Anything that closes a hole changes
        // the count, so a converging store is never cut short - the bound only ends a loop that is
        // not converging, and it is released again the moment the count moves.
        static unsigned tgHolesPrevious = 0;
        static unsigned tgOwedRun = 0;
        static bool tgLoopNoted = false;
        const unsigned kTgMaxOwedComposites = 8;
        if (holes != tgHolesPrevious) {
            tgOwedRun = 0;
            tgLoopNoted = false;
        }
        tgHolesPrevious = holes;
        if (!holes) {
            tgOwedRun = 0;
            tgLoopNoted = false;
        } else if (tgOwedRun < kTgMaxOwedComposites) {
            ++tgOwedRun;
            if (g_session && g_session->chrome) {
                g_session->chrome->setNeedsPresent();
                WebCorePort::presentRequested();
            }
        } else if (!tgLoopNoted) {
            // Grep for "tgloop": the hole count did not move for kTgMaxOwedComposites composites,
            // so the extra present is not helping and is not asked for again until it does.
            tgLoopNoted = true;
            if (!g_stagePath.empty()) {
                FILE* fp = nullptr;
                if (fopen_s(&fp, g_stagePath.c_str(), "ab") == 0 && fp) {
                    const float ps = g_session && g_session->page ? g_session->page->pageScaleFactor() : -1.f;
                    std::fprintf(fp, "tgloop holes=%u owed=%u ps=%.3f\n",
                        holes, tgOwedRun, ps);
                    std::fclose(fp);
                }
            }
        }
    }
    {
        PerfPhase perfSwap(&g_perfCur.swap);        // M4: eglSwapBuffers → SwapChainPanel
        g_glContext->swapBuffers();
    }
    return kOK;
}

// Apotheosis (OFFTHREAD-RASTER-LOG.md §4.2/§4.3): call right after a composite. A replay that
// finishes after this composite has no one to upload it — the tile would keep its old pixels until
// the page happens to be dirtied again. Arming m_needsPresent makes the harness' next live tick
// composite (and the tick's own peekNeedsPresent() fast-path check take the heavy branch), where
// wkFinishPendingPaints() picks the finished buffers up. Bumping the frame hash keeps the live loop
// from judging the frame static and stopping before that tick happens. The count also becomes the
// perf row's raster_pending column, which is the only way to see whether the worker pool kept up.
static void notePendingRasterTiles()
{
    // Apotheosis (2026-09-08, docs/TILEGRID-DESIGN.md section 3): the TileGrid needs this even for
    // the passes it replays inline (an image store never goes to a worker), where nothing is
    // pending and nothing finishes on a worker - the tile still has to be UPLOADED, and the
    // per-composite upload budget applies to the inline path too. A pass that rasterises more cells
    // than the budget uploads leaves the rest Landed: pixels that exist and that only the next
    // composite's drain can put on screen. On a page that has stopped dirtying itself nobody would
    // ask for that composite (the visible-holes present in gpuPresent covers a hole, not a Landed
    // tile behind an already drawn one), so the last tiles of every burst would sit in memory, off
    // the screen.
    // Both are needed. `pending` covers "a replay is still running" — one more composite will be
    // owed. `finished` (edge-triggered, reading resets) covers the replay that started AND ended
    // between two composites, which leaves `pending` at zero although nothing has uploaded the new
    // pixels yet. With step 4's asynchronous first paints (OFFTHREAD-RASTER-LOG.md §10) that case
    // is an *empty* tile on screen, not a stale one, so it is the more important of the two.
    // Apotheosis (2026-09-06): and `deferred` covers the third case, which the tile-upload budget
    // introduced — a replay that finished long ago and whose pixels the last pass chose not to
    // upload because it had already spent its budget on nearer tiles. Nothing is running for it
    // (so `pending` is zero), its completion edge was consumed frames ago (so `finished` is zero),
    // and no worker will ever wake anybody for it again: only the next composite's drain can put
    // it on screen. Without this a page that stops dirtying itself the moment a burst is deferred
    // would keep the last one or two tiles of that burst in memory and off the screen.
    const unsigned pending = WebCore::wkWinUWPTexmapPendingRasterTiles();
    const unsigned finished = WebCore::wkWinUWPTexmapTakeFinishedRasterTiles();
    const unsigned deferred = WebCore::wkWinUWPTexmapTakeDeferredUploads();
    if (g_perfOn) {
        g_perfCur.rasterPending = static_cast<int>(pending);
        g_perfCur.rasterDone = static_cast<int>(finished);
        g_perfCur.rasterDeferred = static_cast<int>(deferred);
        unsigned posted = 0, cancelled = 0, blocking = 0;
        WebCore::wkWinUWPTexmapRasterStats(posted, cancelled, blocking);
        g_perfCur.rasterPosted = static_cast<int>(posted);
        g_perfCur.rasterCancelled = static_cast<int>(cancelled);
        g_perfCur.rasterBlocked = static_cast<int>(blocking);
    }
    if (!pending && !finished && !deferred)
        return;
    if (g_session && g_session->chrome)
        g_session->chrome->setNeedsPresent();
    ++g_lastFrameHash;
}

// Apotheosis (OFFTHREAD-RASTER-LOG.md §10): the case polling cannot cover — a replay lands while
// the harness' tick loop has already gone idle, so no composite is coming and the tile stays empty.
// wkWinUWPSetRasterCompletionHandler() calls this ON A WORKER THREAD, so the contract is strict:
// non-blocking, thread-safe, must not touch WebCore. All it does is hop to the engine thread
// through WTF's main-thread function queue — the same queue the finished image decodes arrive on,
// drained by the RunLoop::cycle() at the top of WebCoreScrollBy / WebCoreLiveTick — and set
// m_needsPresent there, on the thread that owns it. The atomic collapses a burst of completions
// into one hop: several tiles finishing together only need one composite.
static std::atomic<int> g_rasterWakeQueued { 0 };

static void rasterCompletedOnWorker()
{
    int expected = 0;
    if (!g_rasterWakeQueued.compare_exchange_strong(expected, 1))
        return;   // a hop is already queued; it will pick this completion up too
    WTF::callOnMainThread([] {
        g_rasterWakeQueued.store(0);
        if (g_session && g_session->chrome)
            g_session->chrome->setNeedsPresent();
        ++g_lastFrameHash;   // so the harness' live loop does not judge the frame static and stop
    });
}

// view->paint → Cairo ARGB32 → 调用方 RGBA8888 缓冲(B<->R 交换 + 去预乘)。统计非白像素数。
// 同时供一次性 WebCoreLoadUrl 与会话各入口复用(单一绘制实现)。
static int paintToRGBA(WebCore::LocalFrameView& view, int w, int h, uint8_t* outRGBA, int& nonWhiteOut)
{
    using namespace WebCore;
    nonWhiteOut = 0;

    // M2:GPU 已起且本次绘制的正是当前会话的 view(其图层树已建)→ 经 TextureMapper 合成 + 离屏 readback
    //   出像素,替代下面的 cairo 软件绘制。任一前提不满足(主页/一次性渲染无 session/无图层树)或合成失败
    //   → 落回 cairo(软件兜底,零回归)。view 匹配检查防止用旧会话图层树画无关 view。
    if (g_gpuActive && g_textureMapper && g_session && g_session->chrome
        && g_session->mainFrame && g_session->mainFrame->view() == &view) {
        if (WebCore::GraphicsLayer* root = g_session->chrome->rootLayer()) {
            // 直呈现模式(GpuInit 收到窗口表面):合成直接 swapBuffers 到可见 SwapChainPanel,省掉 readback+blit
            //   两次 3MB 拷贝(冲 60fps)。outRGBA 不填(调用方据 g_directPresent 跳过 BlitToBitmap)。
            if (g_gpuPresentMode) {
                if (gpuPresent(view, w, h, *root) == kOK) {
                    // 直呈现没有像素可算哈希 → 用"引擎请求过重绘(triggerRenderingUpdate)/合成动画在跑"当帧
                    // 变化信号混进哈希。否则 g_lastFrameHash 恒不变:实时循环 ~8s 误判静止停帧(GPU 模式下
                    // 动画冻结),ForwardClickToEngine 的 changed 检测也恒 false(模态关闭被误判成死点击 →
                    // 链接表兜底误导航)。takeNeedsPresent 此前无人消费,在此消费正好。
                    if (g_session->chrome->takeNeedsPresent() || g_gpuAnimating)
                        ++g_lastFrameHash;
                    notePendingRasterTiles();   // after takeNeedsPresent(), so it is not consumed again
                    return kOK;
                }
            } else if (gpuCompositeReadback(view, w, h, *root, outRGBA, nonWhiteOut) == kOK) {
                notePendingRasterTiles();
                return kOK;
            }
        }
    }

    // 走到这=本帧不经 GPU 合成(GPU 未起/无图层树/合成失败)。滚动快路径标志只对"紧接着的那次 GPU 合成"
    // 有意义,这里必须清掉,否则残留到下一次真 GPU 合成(如点击后)会错误跳过 forceDirtyTree → 停留旧内容。
    g_gpuScrollFast = false;

    const IntSize size(w, h);
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (!surface || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        if (surface) cairo_surface_destroy(surface);
        return kErrCairoSurface;
    }
    cairo_t* cr = cairo_create(surface);
    if (!cr || cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        if (cr) cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return kErrCairoContext;
    }
    {
        GraphicsContextCairo context(adoptRef(cr));
        // ScrollView::paint 内部已按 -scrollPosition 平移,始终从原点绘制,绝不另加 scrollY。
        // ★ M1:开合成后页面内容进 GraphicsLayer,普通 paint 会漏合成层 → 软件渲染变空。
        //   设 FlattenCompositingLayers 把合成层拍平进这次软件绘制(M2 起改 GPU 呈现就不走这条)。
        //   非合成路径(RenderHtml/LoadUrl)无合成层,此标志无害。
        auto oldBehavior = view.paintBehavior();
        view.setPaintBehavior(oldBehavior | PaintBehavior::FlattenCompositingLayers | PaintBehavior::Snapshotting);
        {
            PerfPhase perfPaint(&g_perfCur.paint);   // M4: software raster (Cairo)
            view.paint(context, IntRect(IntPoint(), size));
        }
        view.setPaintBehavior(oldBehavior);
    }
    cairo_surface_flush(surface);

    const unsigned char* src = cairo_image_surface_get_data(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    int nonWhite = 0;
    uint32_t hash = 2166136261u;   // FNV-ish 滚动哈希,实时模式判断画面是否变化(顺带在同一遍像素循环里算)
    {
    PerfPhase perfBlit(&g_perfCur.blit);   // M4: ARGB32 → RGBA8888 convert/unpremultiply pass (one probe, not per pixel)
    for (int y = 0; y < h; ++y) {
        const unsigned char* srow = src + static_cast<size_t>(y) * stride;
        uint8_t* drow = outRGBA + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            const unsigned char b = srow[x * 4 + 0];
            const unsigned char g = srow[x * 4 + 1];
            const unsigned char r = srow[x * 4 + 2];
            const unsigned char a = srow[x * 4 + 3];
            if (a == 0 || a == 255) {
                drow[x * 4 + 0] = r;
                drow[x * 4 + 1] = g;
                drow[x * 4 + 2] = b;
                drow[x * 4 + 3] = a;
            } else {
                drow[x * 4 + 0] = static_cast<uint8_t>((r * 255 + a / 2) / a);
                drow[x * 4 + 1] = static_cast<uint8_t>((g * 255 + a / 2) / a);
                drow[x * 4 + 2] = static_cast<uint8_t>((b * 255 + a / 2) / a);
                drow[x * 4 + 3] = a;
            }
            if (drow[x * 4 + 0] != 255 || drow[x * 4 + 1] != 255 || drow[x * 4 + 2] != 255)
                ++nonWhite;
            // 每 4 像素采样进哈希(原 16px 网格太疏,漏掉搜索页小加载圈等小动画 → 误判静止停帧;
            // 4px 网格密 16 倍,能侦测到小圈圈的变化,让实时循环对动画持续重绘;真静止页仍会停帧省电)。
            if (((x | y) & 3) == 0) {
                hash = (hash ^ drow[x * 4 + 0]) * 16777619u;
                hash = (hash ^ drow[x * 4 + 1]) * 16777619u;
                hash = (hash ^ drow[x * 4 + 2]) * 16777619u;
            }
        }
    }
    }
    cairo_surface_destroy(surface);
    nonWhiteOut = nonWhite;
    g_lastFrameHash = hash;
    return kOK;
}

// 渲染诊断写入 g_lastTitle/g_lastDiag(白屏定性 + 子资源计数),供 WebCoreGetDiag/GetTitle 取走。
static void writeDiag(WebCore::Document& document, WebCore::LocalFrameView& view, int w, int h, int nonWhite)
{
    using namespace WebCore;
    auto urlStr = document.url().string().utf8();
    auto titleStr = document.title().utf8();
    IntSize cs = view.contentsSize();
    // JS 诊断:jsEnabled=设置开关;canExec=ScriptController 实际允许执行(沙箱/无 page 会变 0);
    // scripts=<script> 元素数。SPA 显示 noscript/空白时,这三个数能区分"脚本被禁"vs"脚本没下来"vs"下来没跑"。
    int jsEnabled = document.settings().isScriptEnabled() ? 1 : 0;
    int canExec = view.frame().script().canExecuteScripts(ReasonForCallingCanExecuteScripts::NotAboutToExecuteScript) ? 1 : 0;
    unsigned scriptCount = document.scripts()->length();
    g_lastScriptCount = static_cast<int>(scriptCount);   // Apotheosis (M4): n_scripts column
    // SPA 挂载判据(纯 DOM 读,不依赖 JS eval):#root 子元素数>0 = React/Vue 挂载了;=0 = 没挂(白屏);
    // bodyKids = body 子元素数。配合 loads=/js= 分清:模块没下来(loads.F 高)vs 下来没执行(rootKids=0)vs 挂了。
    int rootKids = -1;
    if (RefPtr root = document.getElementById(AtomString { "root"_s }))
        rootKids = static_cast<int>(root->childElementCount());
    int bodyKids = document.body() ? static_cast<int>(document.body()->childElementCount()) : -1;
    int pendingResources = countPendingResources(document);
    g_lastPendingResources = pendingResources;
    std::snprintf(g_lastTitle, sizeof g_lastTitle, "%s", titleStr.data());
    std::snprintf(g_lastUrl, sizeof g_lastUrl, "%s", urlStr.data());
    int mainLen = std::snprintf(g_lastDiag, sizeof g_lastDiag,
        "url=%s title=%s contents=%dx%d body=%d nonwhite=%d/%d loads=S%d/R%d/C%d/F%d pending=%d js=%d/%d scripts=%u rootKids=%d bodyKids=%d spa=[%.220s] lasterr=[%.150s]",
        urlStr.data(), titleStr.data(), cs.width(), cs.height(),
        document.body() ? 1 : 0, nonWhite, w * h,
        g_loadStarted, g_loadResponse, g_loadComplete, g_loadFail, pendingResources,
        jsEnabled, canExec, scriptCount, rootKids, bodyKids, g_spaProbe, g_lastNetError);
    // 已请求资源清单(诊断 SPA 模块图):每项 文件名(s状态)。status: 0未知 1加载中 2成功 3加载失败 4解码失败。
    // 若 pigai.shop 的 5 个 chunk(react-core/semi-ui/...)根本不在表里 = import 没去拉(模块图没解析);
    // 在表里但 s3 = 拉了但失败(网络/CORS)。
    if (mainLen > 0 && mainLen < static_cast<int>(sizeof g_lastDiag) - 8) {
        char* p = g_lastDiag + mainLen;
        int rem = static_cast<int>(sizeof g_lastDiag) - mainLen;
        int n = std::snprintf(p, rem, " res:[");
        if (n > 0 && n < rem) { p += n; rem -= n; }
        for (auto& kv : document.cachedResourceLoader().allCachedResources()) {
            WebCore::CachedResource* res = kv.value.get();
            if (!res)
                continue;
            auto u8 = res->url().string().utf8();
            const char* full = u8.data() ? u8.data() : "";
            const char* slash = std::strrchr(full, '/');
            const char* name = (slash && slash[1]) ? slash + 1 : full;
            int wn = std::snprintf(p, rem, "%.44s(s%d) ", name, static_cast<int>(res->status()));
            if (wn < 0 || wn >= rem)
                break;
            p += wn; rem -= wn;
        }
        if (rem > 1) { *p++ = ']'; *p = '\0'; }
    }
}

// 在主世界执行一段 JS,把结果转成字符串写入 out。供 SPA 诊断(动态 import 探针)与未来注入用。
static int evalJS(WebCore::LocalFrame& frame, const char* script, char* out, int len)
{
    using namespace WebCore;
    if (!out || len <= 0)
        return kErrBadArgs;
    out[0] = '\0';
    DOMWrapperWorld& world = mainThreadNormalWorldSingleton();
    auto* globalObject = frame.script().globalObject(world);
    if (!globalObject)
        return kErrNoDocument;
    JSC::JSValue result = frame.script().executeScriptInWorldIgnoringException(
        world, String::fromUTF8(script), JSC::SourceTaintedOrigin::Untainted);
    JSC::JSLockHolder lock(globalObject->vm());
    String s = result.toWTFString(globalObject);
    auto u8 = s.utf8();
    std::snprintf(out, static_cast<size_t>(len), "%s", u8.data() ? u8.data() : "");
    return kOK;
}

// SPA 模块求值探针:对 <script type=module> 站点,动态 import 入口模块(已求值则复用结果/错误;
// 未求值则此刻触发求值,可能顺带挂载 React)。pump 让 import promise 求值,再读回结果到 g_spaProbe。
// 返回后 frame/view/document 可能因挂载而变,调用方需重取。
static void probeSpaModule(WebCore::Page& page, WebCore::LocalFrame& frame)
{
    using namespace WebCore;
    char kick[80] = "";
    evalJS(frame,
        "(function(){try{var s=document.querySelector('script[type=\"module\"][src]');"
        "if(!s)return 'no-mod';window.__spaProbe='importing';"
        "import(s.src).then(function(){window.__spaProbe='eval-ok rootCh='+((document.getElementById('root')||{children:[]}).children.length);})"
        ".catch(function(e){window.__spaProbe='EVAL-ERR:'+(e&&(e.message||e.name||String(e))||'?');});"
        "return 'kicked';}catch(e){return 'PROBE-EX:'+(e.message||e);}})()",
        kick, sizeof kick);
    if (std::strcmp(kick, "no-mod") == 0) {
        g_spaProbe[0] = '\0';   // 非模块站点,不探
        return;
    }
    // 动态 import 异步,需 pump 微任务/事件循环让其求值(最多 ~4s)。
    pumpLoop(frame, nullptr, true, 0, 4.0, &page);
    RefPtr<LocalFrame> lf = page.localMainFrame();
    if (lf)
        evalJS(*lf, "window.__spaProbe||'no-probe'", g_spaProbe, sizeof g_spaProbe);
}

// 销毁当前会话。顺序关乎 use-after-free(晚到的 didFinishLoad / curl 完成回调可能在销毁中触发):
//  (b) 先把完成回调置空 → 晚到回调变 no-op;
//  (c) 再 stopAllLoaders 取消在途子资源(可能同步回调 dispatchDidFailProvisionalLoad,此时已 no-op);
//  (d) 丢 frame/client 引用;(e) 丢最后一个 Page 引用 → ~Page 做标准 detach;
//  (f) 转几圈 RunLoop 排空延迟清理 / curl 取消,再建替代会话。
// 注意:内部使用,不检查 g_inPump(交互入口在 pump 中遇致命错误时需直接调它)。
static void teardownSession()
{
    using namespace WebCore;
    if (!g_session)
        return;
    consoleFlush();   // Apotheosis: this session's console.txt lines otherwise wait for the next ~16-line batch
    // Apotheosis: (a) GPU sessions must be torn down with the ANGLE context current.
    // ~Page destroys the GraphicsLayerTextureMapper tree, and with it the BitmapTextures /
    // FBOs it owns — those destructors call into GL. Outside gpuPresent nothing makes the
    // context current, so on the second tab the GL deletes ran against no current context
    // (suspected device crash). Make it current first. Also drop the per-frame flags, which
    // describe the session that is going away; g_gpuActive / g_gpuPresentMode / g_glContext /
    // g_textureMapper are deliberately process-lifetime and stay untouched.
    if (g_gpuActive && g_glContext)
        g_glContext->makeContextCurrent();
    g_gpuAnimating = false;
    g_gpuScrollFast = false;
    // Apotheosis (WHITE-AT-SCROLL-END): the tile bookkeeping describes the page that is going away.
    // The new session's first composite must force-dirty.
    g_gpuForceFullNext = true;
    g_gpuTargetedNext = false;
    g_gpuLastCompositeFull = false;
    navLoadEnd();   // Apotheosis (M4 load throttle): the load this belonged to is gone
    // Apotheosis (drag as pointer events): the page that owned an in-flight drag is going
    // away — drop the flag, or the first phase-1/2 call of the next session would dispatch a
    // mousemove/mouseup into a document that never saw the press.
    g_dragActive = false;
    if (g_session->client)
        g_session->client->setLoadCompletionHandler({});       // (b)
    if (g_session->mainFrame)
        g_session->mainFrame->loader().stopAllLoaders();        // (c)
    g_session->client = nullptr;
    g_session->mainFrame = nullptr;                              // (d)
    // (e) 关键:在 reset() 之前显式丢最后一个 Page 引用,触发 ~Page。此时 g_session(及其 load 成员)仍存活,
    //     ~Page 内若有晚到回调写 load 也是写活内存。若改为直接 reset(),~Session 按反声明序先析构 load 再析构
    //     page,~Page 的回调就会写到已析构的 load → UAF。
    g_session->page = nullptr;
    g_session.reset();                                          // (f) 此时 Session.page 已空,~Session 不再触发回调
    for (int i = 0; i < 4; ++i)                                  // (g) 排空延迟清理 / curl 取消
        RunLoop::cycle();
    // Apotheosis (MEMORY-PLAN.md §3 change 6): (h) ~Page freed the layer tree and its textures,
    // but the JSC heap and the MemoryCache survive a tab switch untouched — that is why the
    // second tab measured 951 MB while the same page alone costs 620 MB. Give it back here,
    // after the Page is gone (nothing can re-populate the caches at this point) and after the
    // RunLoop drain (so the deferred deletions above are already counted as garbage).
    // keepResourceCache: this path also runs at the start of every WebCoreSessionLoad, and the
    // next page usually wants the same CSS/JS — dropping the encoded cache here would cost a
    // full re-download on every navigation. WebCoreCloseSession() clears it as well.
    // Deliberately *not* wkReleaseMemoryLevel(2): the critical path's
    // deleteAllCode(PreventCollectionAndDeleteAllCode) would discard every piece of JIT code on
    // every navigation, which is exactly what we bought the JIT tree for. The explicit full
    // collection below reclaims the dead page's heap without that.
    wkReleaseMemoryLevel(1, /*keepResourceCache*/ true);
    wkCollectJSCHeapNow();
}

// 在已 emplace 的 g_session 上建立 Page、发起网络加载、settle、布局、提链接、绘制。
// 约定:调用前 g_session 已 emplace 且 w/h 已设、load 已清零。成功返回 kOK 并把 page/mainFrame/client
// 存入会话;失败返回负值(调用方 WebCoreSessionLoad 负责 teardown 不留半截会话)。
static int buildSession(const char* url, int w, int h, uint8_t* outRGBA)
{
    using namespace WebCore;
    URL parsedURL { String::fromUTF8(url) };
    if (!parsedURL.isValid())
        return kErrBadUrl;

    auto pageConfiguration = pageConfigurationWithEmptyClients(
        std::nullopt, PAL::SessionID::defaultSessionID());

    // cookie 持久化:DOM(document.cookie)路换成真 jar(默认是 EmptyStorageSessionProvider→nullptr→cookie 被丢)。
    // HTTP(Cookie/Set-Cookie 头)路由 LoadingFrameLoaderClient::createNetworkingContext 提供,二者共用同一 jar。
    pageConfiguration.cookieJar = WebCore::CookieJar::create(WebCorePort::makeStorageSessionProvider());

    // Apotheosis (2026-09-07): WebSocket. pageConfigurationWithEmptyClients installs
    // EmptyClients.cpp's EmptySocketProvider, whose createWebSocketChannel() returns nullptr -
    // and WebSocket.cpp:288 answers that with RELEASE_ASSERT(m_channel) ("Every
    // ScriptExecutionContext should have a SocketProvider"). So the first `new WebSocket(...)`
    // on a page killed the app: a map site every time, and any SPA with a live connection.
    // PortSocketProvider hands out a real channel that speaks the protocol over curl streams;
    // see PortWebSocket.h.
    pageConfiguration.socketProvider = WebCorePort::PortSocketProvider::create();

    // GPU 合成:仅当 GPU(GL 上下文 + TextureMapper)已初始化才用真 ChromeClient(PortChromeClient,
    // 它在 attachRootGraphicsLayer 捕获根 GraphicsLayer)+ 下面开合成。GPU 未起时保持
    // pageConfigurationWithEmptyClients 设的 EmptyChromeClient + 关合成 = 纯软件 cairo 路径(零回归)。
    // ⚠ 0.1.7.1 真机闪退教训:无条件开合成但 M1 还没 GL/TextureMapper 后端,网络页一加载就在
    //   合成更新/PlatformDisplay 路径 fail-fast(before-load 后进程消失、无 after-load、连 dump/WER 都没有)。
    if (g_gpuActive) {
        auto chrome = WTF::makeUniqueRefWithoutRefCountedCheck<WebCorePort::PortChromeClient>();
        g_session->chrome = chrome.ptr();             // Page 持有 UniqueRef,裸指针随 Page 存活
        pageConfiguration.chromeClient = WTF::move(chrome);
    }

    DriverLoadState* loadPtr = &g_session->load;   // 稳定:g_session 在建会话期间不 reset
    WebCorePort::LoadingFrameLoaderClient** clientSlot = &g_session->client;
    {
        auto& params = std::get<PageConfiguration::LocalMainFrameCreationParameters>(
            pageConfiguration.mainFrameCreationParameters);
        // ★ 关键:pageConfigurationWithEmptyClients 给主帧默认设了 SandboxFlags::all()(含 SandboxScripts),
        //   于是 ScriptController::canExecuteScripts() 永远返回 false —— setScriptEnabled(true) 被 sandbox 压住,
        //   JS 从来没真正执行过(SPA 全显示 noscript、懒加载 IntersectionObserver 不触发、按钮无反应)。
        //   顶层浏览页本就不该有 sandbox,清空它,JS/表单/弹窗等才放行。
        params.effectiveSandboxFlags = { };
        params.clientCreator =
            CompletionHandler<UniqueRef<LocalFrameLoaderClient>(LocalFrame&, FrameLoader&)> {
            [loadPtr, clientSlot](LocalFrame&, FrameLoader& frameLoader) mutable
                -> UniqueRef<LocalFrameLoaderClient> {
                auto client = makeUniqueRefWithoutRefCountedCheck<WebCorePort::LoadingFrameLoaderClient>(frameLoader);
                *clientSlot = client.ptr();   // 捕获原始指针供 teardown 置空回调
                client->setLoadCompletionHandler([loadPtr](bool failed) {
                    if (loadPtr->mainDone)
                        return;
                    loadPtr->mainDone = true;
                    loadPtr->failed = failed;
                });
                return client;
            } };
    }

    Ref<Page> page = Page::create(WTF::move(pageConfiguration));
    g_session->page = page.ptr();   // 立即存活到会话:后续失败路径 teardown 才能安全访问 client/frame

    page->settings().setScriptEnabled(true);
    page->settings().setLoadsImagesAutomatically(true);
    page->settings().setAcceleratedCompositingEnabled(g_gpuActive);   // 仅 GPU 就绪才开合成 → 建 GraphicsLayer 树(PortChromeClient 捕获根层),经 TextureMapper GPU 呈现
    page->settings().setForceCompositingMode(g_gpuActive);            // 同上;GPU 未起时关闭 → 纯软件 cairo,零回归
    // Apotheosis (fixed/sticky at scroll, 2026-09-07): WebCore's own default for this preference is
    // false off iOS, and we build the Page directly - so position:fixed and position:sticky elements
    // never got a composited layer of their own. They are painted into the scrolled-contents tile
    // grid instead, and because the grid is translated by -scrollPosition on every scroll frame,
    // WebCore has to REPAINT them into it at their new place every single frame
    // (LocalFrameView::scrollContentsFastPath -> setBackingNeedsRepaintInRect of the old-plus-new
    // band). With off-thread raster and the per-composite tile-upload budget that repaint lands one
    // or more composites late, so a fixed/sticky header is drawn where it was, rides the page down
    // (or up) and snaps back when the replay arrives - the "header comes down piecewise and jumps
    // back" of 0.1.9.21/22, worst exactly where the raster workers are busy with something else
    // (a news site's lazy images in the lower page, a chat/AI site's polling bot check, a map site's tiles).
    // With the preference on, RenderLayerCompositor::updateCompositingLayers(OnScroll) - which
    // WebCore runs for us because there is no ScrollingCoordinator - re-positions those layers
    // instead: a layer move per scroll frame, no raster and no upload. Sticky needs the WK_WINUWP
    // branch in RenderLayerCompositor::isAsyncScrollableStickyLayer() as well. Gated on g_gpuActive
    // like the two above: without compositing there is nothing to promote.
    page->settings().setAcceleratedCompositingForFixedPositionEnabled(g_gpuActive);
    page->settings().setShouldAllowUserInstalledFonts(false);
    // Apotheosis (M4): never let a web font hold text back. CSSFontFace::fontLoadTiming() maps the
    // default (FontLoadTimingOverride::None with font-display:auto/block, which is what most sites
    // end up with) to a 3 s block period: for those three seconds the text is laid out but painted
    // with nothing - on a phone whose first visually-non-empty layout we measure at 0.4 s. Swap
    // overrides every face to { block 0 s, swap infinite }: the fallback font is painted
    // immediately and replaced when the web font arrives. The trade-off is icon fonts - their code
    // points have no fallback glyph, so a Font-Awesome-style icon is a blank box until it loads,
    // and it cannot be exempted (the timing is per-face and nothing tells us a face is an icon
    // font). Readable text at first paint is worth more here than icons arriving at the same time.
    page->settings().setFontLoadTimingOverride(FontLoadTimingOverride::Swap);
    // Apotheosis (M4 load, 2026-09-07): honour loading="lazy". WebCore's own default for this
    // preference is false (UnifiedWebPreferences.yaml: WebKit true, WebCore false) and we build
    // the Page directly, so we inherited the off state and fetched every image of a document up
    // front. On a news front page that is 261 subresources for a 42424 px tall page whose viewport is 1080
    // px - the load event waits 6.6 s past DOMContentLoaded for images ~39 screens down. The
    // implementation is complete in this tree (html/LazyLoadImageObserver.cpp): a deferred image
    // is observed by an IntersectionObserver with a 100 % root margin, i.e. it loads one screen
    // before it is reached, and the observer is driven by the isolatedUpdateRendering that
    // pumpLoop and WebCoreLiveTick already call every tick. Sites that do not use the attribute
    // are unaffected.
    page->settings().setLazyImageLoadingEnabled(true);
    page->settings().setSpeculationRulesPrefetchEnabled(g_apoSpecPrefetch);   // Apotheosis: privacy, see g_apoSpecPrefetch
    // ★ DOM Storage:Window.localStorage/sessionStorage 默认被 LocalStorageEnabled/SessionStorageEnabled
    //   两个 setting 门控,默认关 → 这两个全局根本没挂上 window → 现代 SPA 启动时访问 localStorage 直接
    //   ReferenceError("Can't find variable: localStorage")崩溃,React 永不挂载(白屏)。开了它们才行。
    page->settings().setLocalStorageEnabled(true);
    page->settings().setSessionStorageEnabled(true);
#if ENABLE(VIDEO)
    page->settings().setMediaEnabled(false);
#endif
    page->setIsVisible(true);
    // Apotheosis (M4 load timeline): opt in to the visually-non-empty milestone. WebCore only
    // computes and dispatches the milestones a client asked for (Page::requestedLayoutMilestones),
    // and this is the mark behind the t_firstpaint column and the stage.txt timeline line.
    page->addLayoutMilestones({ LayoutMilestone::DidFirstVisuallyNonEmptyLayout });

    RefPtr<LocalFrame> localMainFrame = page->localMainFrame();
    if (!localMainFrame)
        return kErrNoMainFrame;
    localMainFrame->setView(LocalFrameView::create(*localMainFrame));
    localMainFrame->init();
    g_session->mainFrame = localMainFrame;

    RefPtr<LocalFrameView> view = localMainFrame->view();
    if (!view)
        return kErrNoView;
    view->setTransparent(false);
    view->setBaseBackgroundColor(Color::white);
    view->setCanHaveScrollbars(true);
    view->resize(IntSize(w, h));

    // headless 页面标记为 active + focused,否则 EventHandler 命中/默认动作、:focus、表单交互、依赖
    // document.hasFocus()/可见性的脚本会被当后台页抑制 → 点击像没反应。
    // ⚠ 必须在 setView()+init() 之后调:setActiveInternal 的 selection().pageActivationChanged()
    //   不判空,若帧还没 document/view 会解引用 null+0x858 崩溃(0.1.0.9 真机崩因,RVA 0x1E9309)。
    page->focusController().setActive(true);
    page->focusController().setFocused(true);

    ResourceRequest request { WTF::move(parsedURL) };
    FrameLoadRequest frameLoadRequest { *localMainFrame, WTF::move(request), SubstituteData { } };
    Ref<FrameLoader> loader = localMainFrame->loader();
    loader->load(WTF::move(frameLoadRequest));

    // 初次加载:等主文档完成 + 空闲;每 tick isolatedUpdateRendering 让 SPA(聊天/AI 站等)的
    // rAF 驱动渲染推进(否则 JS 站点 settle 后仍空白)。
    {
        PerfPhase perfSettle(&g_perfCur.settle);   // M4: network + settle wall time (tick count → `frames`)
        pumpLoop(*localMainFrame, &g_session->load.mainDone, /*allowEarlyStopWithoutNav*/ false,
                 /*settleCapTicks*/ 160, /*watchdog*/ 30.0, /*pageForRendering*/ page.ptr());
    }
    perfMarkNavSettled();   // Apotheosis (M4 load timeline): t_settle

    // Apotheosis (M3): "no load event" is not "nothing loaded". An ImageDocument (main
    // frame navigated to an image URL) and, occasionally, an HTML page hit by the
    // defer/cancel bug commit, parse and paint but never dispatch a load event, so
    // mainDone stays false and pumpLoop only ends via its watchdog. If the main document
    // did commit, render what we have and report success (the diag is tagged below);
    // only a load that never committed anything is a genuine timeout.
    bool noLoadEvent = false;
    if (!g_session->load.mainDone) {
        RefPtr<DocumentLoader> committedLoader = localMainFrame->loader().documentLoader();
        bool committed = g_session->load.committed || (committedLoader && committedLoader->isCommitted());
        if (!committed || !localMainFrame->document())
            return kErrLoadTimeout;
        noLoadEvent = true;
    } else if (g_session->load.failed)
        return kErrLoadFailed;

    // 提交后 WebKit 给新文档新建了 LocalFrameView,加载前的 view 已失效 → 重取 + 重设背景/尺寸。
    view = localMainFrame->view();
    if (!view)
        return kErrNoView;
    view->setTransparent(false);
    view->setBaseBackgroundColor(Color::white);
    view->resize(IntSize(w, h));

    RefPtr<Document> document = localMainFrame->protectedDocument();
    if (!document)
        return kErrNoDocument;
    {
        PerfPhase perfLayout(&g_perfCur.styleLayout);   // M4: forced style + layout
        document->updateLayoutIgnorePendingStylesheets();
    }

    // SPA 模块求值探针:动态 import 入口模块,触发/复用其求值(可能挂载 React)。之后重取帧/view/document,
    // 因挂载可能改了 DOM/布局。非模块站点(probeSpaModule 内判 no-mod)不跑、零开销。
    probeSpaModule(page.get(), *localMainFrame);
    localMainFrame = page->localMainFrame();
    if (!localMainFrame)
        return kErrFrameGone;
    g_session->mainFrame = localMainFrame;
    view = localMainFrame->view();
    if (!view)
        return kErrNoView;
    view->setTransparent(false);
    view->setBaseBackgroundColor(Color::white);
    view->resize(IntSize(w, h));
    document = localMainFrame->protectedDocument();
    if (!document)
        return kErrNoDocument;
    {
        PerfPhase perfLayout(&g_perfCur.styleLayout);   // M4: forced style + layout
        document->updateLayoutIgnorePendingStylesheets();
    }
    extractLinks(document.get(), h);

    int nonWhite = 0;
    int prc = paintToRGBA(*view, w, h, outRGBA, nonWhite);
    if (prc != kOK)
        return prc;
    writeDiag(*document, *view, w, h, nonWhite);
    if (noLoadEvent) {   // Apotheosis (M3): keep the device log honest about why we returned early
        const size_t used = std::strlen(g_lastDiag);
        if (used + 1 < sizeof g_lastDiag)
            std::snprintf(g_lastDiag + used, sizeof g_lastDiag - used, " (no load event)");
    }
    return kOK;
}

// 交互(点击/输入/键)后的统一收尾:重取主帧(可能换帧)、重设 view、布局、提链接、绘制、写诊断。
static int finishInteractionPaint(uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!g_session || !g_session->page)
        return kErrNoSession;
    RefPtr<LocalFrame> lf = g_session->page->localMainFrame();
    if (!lf) {
        teardownSession();
        return kErrFrameGone;
    }
    g_session->mainFrame = lf;
    RefPtr<LocalFrameView> view = lf->view();
    if (!view)
        return kErrNoView;
    view->setTransparent(false);
    view->setBaseBackgroundColor(Color::white);
    view->resize(IntSize(g_session->w, g_session->h));
    RefPtr<Document> doc = lf->document();
    if (!doc)
        return kErrNoDocument;
    doc->updateLayoutIgnorePendingStylesheets();
    extractLinks(doc.get(), g_session->h);
    int nonWhite = 0;
    int prc = paintToRGBA(*view, g_session->w, g_session->h, outRGBA, nonWhite);
    if (prc != kOK)
        return prc;
    writeDiag(*doc, *view, g_session->w, g_session->h, nonWhite);
    return kOK;
}

// Apotheosis: `type` is WebCore::ResourceError::Type (0=Null 1=General 2=AccessControl
// 3=Cancellation 4=Timeout — see ResourceErrorBase.h) as an int, so the driver
// need not include WebCore headers here. Added while chasing the "every redirect ends in
// the harness' error page" regression that surfaced once da68fe9fdd stopped the redirect
// completion lambda from running on a cancelled ResourceHandle: our own
// dispatchDidFailProvisionalLoad (LoadingFrameLoaderClient.cpp) swallows any
// Type::Cancellation failure unconditionally, on the (previously accurate — pre-fix — but
// now stale) assumption that WebKit always restarts the load itself. Logging the type
// alongside domain/desc/url lets the next device run show, without guessing, whether the
// stuck load is really hitting that Cancellation branch (in which case the swallow is the
// bug) or failing immediately with a different type (AccessControl/General), which points
// at a specific WebCore-side redirect check instead.
extern "C" void WebCorePortRecordNetError(int code, int type, const char* domain, const char* desc, const char* url)
{
    std::snprintf(g_lastNetError, sizeof g_lastNetError,
        "curlcode=%d type=%d domain=%s desc=%s url=%s",
        code, type, domain ? domain : "", desc ? desc : "", url ? url : "");
}

// Apotheosis: 内存压力释放。harness 监听 UWP MemoryManager.AppMemoryUsageIncreased,
// 到 High/OverLimit 时经引擎线程调本函数 → 一把清资源缓存 + 后退页面缓存 + JSC GC + 字体缓存。
// critical: 1=严重(连活资源解码数据也丢);0=温和。须在引擎线程调(C ABI 已串行化)。
extern "C" void WebCoreReleaseMemory(int critical)
{
    if (!ensureWebCoreInitialized()) return;
    wkReleaseMemoryLevel(critical ? 2 : 1, /*keepResourceCache*/ false);
}

// Apotheosis (MEMORY-PLAN.md §3 change 3): push the harness' MemoryManager numbers into WebCore.
// Two effects, both of which the port was missing entirely:
//  (1) MemoryPressureHandler's status. It is never install()ed here (the Windows 60 s poll would
//      just reset it to Normal — MemoryPressureHandlerWin.cpp is a stub in an App Container), so
//      isUnderMemoryPressure() was permanently false at all ~22 WebCore call sites: FontCache,
//      WidthCache, GlyphDisplayListCache, RenderLayerCompositor's cover-area multiplier, the
//      pruning reason in MemoryRelease. Setting it is what makes those react.
//  (2) our own MemoryCache budget, plus a release when the level rises.
// Level transitions only — the harness applies hysteresis and calls on change; re-calling with
// the level already in effect is a no-op, so this is safe to call from a tick.
// Engine thread only.
extern "C" void WebCoreSetMemoryPressure(int level)
{
    if (!ensureWebCoreInitialized()) return;
    if (level < 0) level = 0;
    if (level > 2) level = 2;
    const int previous = g_memPressureLevel;
    if (level == previous)
        return;
    g_memPressureLevel = level;

    using SysStatus = WTF::SystemMemoryPressureStatus;
    WTF::MemoryPressureHandler::singleton().setMemoryPressureStatus(
        level >= 2 ? SysStatus::Critical : (level == 1 ? SysStatus::Warning : SysStatus::Normal));

    switch (level) {
    case 0:
        wkSetMemoryCacheCapacities(kMemCacheMinDeadNormal, kMemCacheDeadNormal, kMemCacheTotalNormal);
        break;
    case 1:
        wkSetMemoryCacheCapacities(0, kMemCacheDeadMedium, kMemCacheTotalMedium);
        break;
    default:
        wkSetMemoryCacheCapacities(0, kMemCacheDeadHigh, kMemCacheTotalHigh);
        break;
    }

    // Only release when the pressure *rises*. Falling back to 0 just restores the budget —
    // running a full GC on the way down would be pure cost.
    if (level > previous) {
        // Keep the encoded resource cache at level 1; at level 2 we are close enough to the
        // kill threshold that a white image is better than a dead app.
        wkReleaseMemoryLevel(level, /*keepResourceCache*/ level < 2);
    }
}

// Apotheosis (MEMORY-PLAN.md §3 change 2): engine-side memory numbers for the harness' mem.txt.
// Without these, mem.txt only carries the OS view (AppMemoryUsage) and every one of the eight
// buckets in §1 is a guess. Engine thread only: getStatistics() walks the whole resource map
// and the JSC accessors touch the heap.
extern "C" int WebCoreGetMemoryStats(WebCoreMemoryStats* out)
{
    if (!out)
        return kErrBadArgs;
    int cap = out->structSize;
    if (cap < (int)sizeof(int) || cap > (int)sizeof(WebCoreMemoryStats))
        cap = (int)sizeof(WebCoreMemoryStats);
    WebCoreMemoryStats s;
    std::memset(&s, 0, sizeof s);
    s.structSize = cap;
    s.pressureLevel = g_memPressureLevel;

    if (!ensureWebCoreInitialized()) {
        std::memcpy(out, &s, cap);
        return kErrBadArgs;
    }

    // JSC — commonVM() would *create* a VM, so go through the raw pointer: a page that never
    // ran script leaves these zero instead of allocating a heap just to measure it.
    if (JSC::VM* vm = WebCore::g_commonVMOrNull) {
        JSC::JSLockHolder locker(vm);
        s.jscHeapSize     = vm->heap.size();
        s.jscHeapCapacity = vm->heap.capacity();
        s.jscExtraMemory  = vm->heap.extraMemorySize();
        s.jscObjectCount  = vm->heap.objectCount();
    }

    auto& cache = WebCore::MemoryCache::singleton();
    auto stats = cache.getStatistics();
    s.cacheTotal    = cache.size();
    s.cacheCapacity = g_memCacheCapacity;
    const WebCore::MemoryCache::TypeStatistic* types[] = {
        &stats.images, &stats.cssStyleSheets, &stats.scripts, &stats.fonts, &stats.xslStyleSheets };
    for (auto* t : types) {
        s.cacheLive    += (uint64_t)(t->liveSize    > 0 ? t->liveSize    : 0);
        s.cacheDecoded += (uint64_t)(t->decodedSize > 0 ? t->decodedSize : 0);
    }
    s.imagesSize    = (uint64_t)(stats.images.size > 0 ? stats.images.size : 0);
    s.imagesDecoded = (uint64_t)(stats.images.decodedSize > 0 ? stats.images.decodedSize : 0);
    s.cssSize       = (uint64_t)(stats.cssStyleSheets.size > 0 ? stats.cssStyleSheets.size : 0);
    s.scriptsSize   = (uint64_t)(stats.scripts.size > 0 ? stats.scripts.size : 0);
    s.fontsSize     = (uint64_t)(stats.fonts.size > 0 ? stats.fonts.size : 0);
    s.imagesCount   = (uint32_t)(stats.images.count > 0 ? stats.images.count : 0);
    s.cssCount      = (uint32_t)(stats.cssStyleSheets.count > 0 ? stats.cssStyleSheets.count : 0);
    s.scriptsCount  = (uint32_t)(stats.scripts.count > 0 ? stats.scripts.count : 0);
    s.fontsCount    = (uint32_t)(stats.fonts.count > 0 ? stats.fonts.count : 0);

    // TextureMapper. The counters are process-wide levels maintained in BitmapTexture's
    // ctor/reset/dtor (WebKit 0c78243bf5) — no GL calls, no context needed. Only ask the pool
    // once GPU compositing is actually up: BitmapTexturePool::singleton() would otherwise
    // construct the pool (and its RunLoop timer) just to report zero.
    uint64_t texBytes = 0; unsigned texCount = 0;
    WebCore::wkWinUWPTexmapTextureStats(texBytes, texCount);
    s.texBytes = texBytes;
    s.texCount = texCount;
    if (g_gpuActive) {
        uint64_t poolBytes = 0; unsigned poolCount = 0;
        WebCore::wkWinUWPTexmapPoolStats(poolBytes, poolCount);
        s.poolBytes = poolBytes;
        s.poolCount = poolCount;
    }

    std::memcpy(out, &s, cap);
    return kOK;
}

// Apotheosis: 清除全部 cookie。设置页"清除数据"用。引擎线程调。清完立即落盘(空文件),否则
// JSON 快照还留着旧的,下次启动 ensureDefaultPortStorageSession 会把清掉的 cookie 又灌回来。
extern "C" void WebCoreClearCookies()
{
    if (!ensureWebCoreInitialized()) return;
    WebCorePort::defaultPortStorageSession().deleteAllCookies([] { });
    WebCorePort::flushCookiesToDisk();
}

extern "C" {

// Point curl/OpenSSL at a CA-certificate bundle (PEM) for TLS verification.
// Required in the App Container sandbox, which cannot reach the Windows system
// trust store: without this, every HTTPS handshake fails server-trust eval.
// Must be called before the first network request; idempotent (last call wins).
// `path` is a UTF-8 filesystem path to a Mozilla-style cacert.pem.
void WebCoreSetCACertPath(const char* path)
{
    if (!path || !*path)
        return;

    // CurlContext::singleton() also boots libcurl + OpenSSL the same way
    // ResourceHandle::start() does, so this is safe to call standalone.
    WebCore::CurlContext::singleton().sslHandle().setCACertPath(String::fromUTF8(path));
}

// Apotheosis: 显式设 cookie jar 落盘路径(镜像本函数的模式:显式注入,不靠环境变量跨 clang-cl 引擎
// 与 MSVC v143 harness 两套独立 CRT 传递)。
// ⚠ 2026-07-03 真机验证:调了这个会崩(SQLite 真实文件 open() 在此平台的 VFS 层空指针,见项目记忆
//   cookie-persistence)。当前 harness 不调用它,保留只是留着接口;真正持久化见下面两个函数。
void WebCoreSetCookieJarPath(const char* path)
{
    if (!path || !*path)
        return;
    WebCorePort::setPortCookieJarPath(String::fromUTF8(path));
}

// cookie 的 JSON Lines 持久化文件路径(jar 本身固定 ":memory:",这只是引擎自己旁路读写的快照,
// 不碰 SQLite 真实文件 open())。须在首个引擎调用(ensureWebCoreInitialized)之前调 —— harness 的
// SetupRuntimeEnv 在引擎线程最早处调用。空/未调用则 cookie 不持久(等同旧版临时会话,不崩)。
void WebCoreSetCookieJsonPath(const char* path)
{
    if (!path || !*path)
        return;
    WebCorePort::setPortCookieJsonPath(String::fromUTF8(path));
}

// Apotheosis: arm crash.txt logging and point it at a file inside LocalState
// (the App Container's only writable place, and the driver cannot discover it —
// the harness passes it in from SetupRuntimeEnv on the engine thread). Unlike
// the perf log this is NOT opt-in: a crash with no dump is exactly the case we
// can never reproduce on the build machine. Installs all three legs once; later
// calls only update the path. See the block near the top of this file.
void WebCoreSetCrashLogPath(const char* path)
{
    if (!path || !*path)
        return;
    // Copy into a fixed buffer — the crash path must not touch the heap and the
    // caller's string may be gone by the time we crash.
    size_t i = 0;
    for (; path[i] && i + 1 < sizeof(g_crashLogPath); ++i)
        g_crashLogPath[i] = path[i];
    g_crashLogPath[i] = 0;

    // Apotheosis: derive console.txt's path from crash.txt's directory — see the "JS console"
    // block above consoleAppendLine() for why crash.txt's path is the one this rides on. Ordinary
    // heap/file use, fine here (unlike the crash legs): this runs once from SetupRuntimeEnv, never
    // from a dying process.
    {
        std::string crashPath(g_crashLogPath);
        const size_t slash = crashPath.find_last_of("\\/");
        g_consolePath = (slash == std::string::npos) ? std::string("console.txt")
            : crashPath.substr(0, slash + 1) + "console.txt";
        g_consolePathReady = true;
        // Apotheosis (M4 load timeline): stage.txt lives in the same LocalState directory.
        g_stagePath = (slash == std::string::npos) ? std::string("stage.txt")
            : crashPath.substr(0, slash + 1) + "stage.txt";
        FILE* probe = nullptr;
        if (fopen_s(&probe, g_consolePath.c_str(), "rb") == 0 && probe) {
            g_consoleFileExisted = true;   // opt-in: console.txt already exists on disk
            std::fclose(probe);
        }
    }

    if (g_crashLogInstalled)
        return;
    g_crashLogInstalled = true;
    // Resolve the memory probe once, here — never from inside a crash handler.
    if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll"))
        g_crashGetProcessMemoryInfo = reinterpret_cast<CrashGetProcessMemoryInfoFn>(
            reinterpret_cast<void*>(GetProcAddress(k32, "K32GetProcessMemoryInfo")));
    WTFWinUWPSetCrashHook(&crashLogWtfHook);
    AddVectoredExceptionHandler(1, &crashLogVectoredHandler);
    std::signal(SIGABRT, &crashLogSignalHandler);
    // The four CRT legs (see the block comment above crashLogMemorySuffix): every path
    // that aborts without ever reaching WTFCrash now names itself in crash.txt.
    g_crashPrevTerminate = std::set_terminate(&crashLogTerminateHandler);
    _set_new_handler(&crashLogNewHandler);
    g_crashPrevInvalidParameter = _set_invalid_parameter_handler(&crashLogInvalidParameterHandler);
    g_crashPrevPureCall = _set_purecall_handler(&crashLogPureCallHandler);
}

// Apotheosis: append one reason line (plus the current stack) to crash.txt from outside
// the engine. The harness owns the terminate/UnhandledException paths of its own CRT —
// C++/CX, exceptions enabled, a different CRT instance from this DLL's — so it cannot
// reuse the handlers above and needs a way to record *why* it is about to die. No-op
// until WebCoreSetCrashLogPath() has run. Safe from any thread and from a dying one.
void WebCoreCrashNote(const char* reason)
{
    crashLogWrite(reason && *reason ? reason : "(note)", nullptr);
    perfFlush();
    consoleFlush();
}

// Apotheosis (M4): resolve-mode switch for the curl backend. The phone has global
// IPv6 addresses; on some links the v6 path is a black hole and the first contact
// with a host stalls 14-22 s before the main resource commits, while an immediate
// reload takes 0.4 s. Rather than hardcoding IPv4 we let the harness flip it at
// runtime so it can be A/B-measured against the net_dns/net_connect/net_ttfb columns.
// Takes effect for handles created after the call (i.e. the next request), so call
// it before starting a navigation. Implemented in WebKit's CurlContext.cpp.
extern "C" void WebCorePortSetIPv4Only(int on);

void WebCoreSetIPv4Only(int enable)
{
    WebCorePortSetIPv4Only(enable);
}

// Apotheosis (M4 step 1): switch per-phase timing on and point it at a CSV file.
// The App Container only lets us write inside LocalState and the driver cannot
// discover that path itself, so the harness passes it in — and only when the
// tester dropped LocalState\perf.txt, mirroring the imedebug.txt opt-in. Unset or
// "" = off, which is the shipping default and costs one branch per probe.
// Engine-thread call; call it before the first navigation.
void WebCoreSetPerfLogPath(const char* path)
{
    if (!path || !*path) {
        g_perfOn = false;
        g_apoNetTimingOn = false;
        g_perfPath.clear();
        return;
    }
    g_perfPath = path;
    g_perfOn = true;
    // Apotheosis (M4 load waterfall): WebKit's CurlRequest reads this flag on the curl worker
    // thread to decide whether a response is worth a URL copy + a main-thread hop. It is a
    // plain bool written once here, before any load; a torn read would only cost one missing
    // or one extra waterfall entry, so no atomic.
    g_apoNetTimingOn = true;
    // Write the CSV header only into a fresh file (the log is appended across runs).
    g_perfHeaderDone = false;
    FILE* fp = nullptr;
    if (fopen_s(&fp, g_perfPath.c_str(), "rb") == 0 && fp) {
        std::fseek(fp, 0, SEEK_END);
        if (std::ftell(fp) > 0)
            g_perfHeaderDone = true;
        std::fclose(fp);
    }
}

// Apotheosis (M4 step 1): drain the in-memory perf ring to disk. Rows otherwise
// reach the file only when a navigation completes or the ring fills, so an app
// that UWP terminates while suspended would lose them — the harness calls this
// from the suspend path and at the end of the autodiag loop. No-op when off.
void WebCorePerfFlush()
{
    perfFlush();
    consoleFlush();
}

// 把当前 jar 里的持久(有过期时间、非会话)cookie 写回 JSON Lines 文件。harness 在应用切后台
// (即将被 UWP 挂起/可能被系统直接终止)时调,引擎线程串行。
void WebCoreFlushCookiesToDisk()
{
    if (!ensureWebCoreInitialized()) return;
    WebCorePort::flushCookiesToDisk();
}

// Inject the CA-certificate bundle as an in-memory PEM blob (CURLOPT_CAINFO_BLOB).
// App Container blocks OpenSSL's file-based CA loading (SSL_CTX_load_verify_locations
// fails even on a readable file in the app's own LocalState → curl 77), so the
// path-based WebCoreSetCACertPath does not work on device; the blob bypasses all
// file I/O. `data` is the raw cacert.pem bytes (PEM text). Call before first load.
void WebCoreSetCACertBlob(const uint8_t* data, int len)
{
    if (!data || len <= 0)
        return;
    Vector<uint8_t> bytes(static_cast<size_t>(len));
    std::memcpy(bytes.mutableSpan().data(), data, static_cast<size_t>(len));
    // CACertInfo holds the Vector; curl_blob uses CURL_BLOB_NOCOPY, so the bytes must
    // outlive requests — the singleton CurlSSLHandle owns them for the process lifetime.
    WebCore::CurlContext::singleton().sslHandle().setCACertData(WTF::move(bytes));
    // Keep a copy for the standalone WebCoreDownload curl handle (separate from the render bridge).
    g_caBytes.assign(data, data + len);
}

// Copy the last recorded network-load error (set on WebCoreLoadUrl failure) into
// `buf`. Returns the number of bytes written (excluding NUL). Empty if no error.
int WebCoreGetLastError(char* buf, int len)
{
    if (!buf || len <= 0)
        return 0;
    int n = std::snprintf(buf, static_cast<size_t>(len), "%s", g_lastNetError);
    return n < 0 ? 0 : n;
}

// Copy the last render diagnostic (final URL / title / contents size / non-white
// pixel count from the most recent WebCoreLoadUrl) into `buf`. For debugging blank
// renders: distinguishes "engine rendered nothing" from "bitmap not displayed".
int WebCoreGetDiag(char* buf, int len)
{
    if (!buf || len <= 0)
        return 0;
    int n = std::snprintf(buf, static_cast<size_t>(len), "%s", g_lastDiag);
    return n < 0 ? 0 : n;
}

// Copy the most recent loaded page title (UTF-8) into buf. Empty if none.
int WebCoreGetTitle(char* buf, int len)
{
    if (!buf || len <= 0)
        return 0;
    int n = std::snprintf(buf, static_cast<size_t>(len), "%s", g_lastTitle);
    return n < 0 ? 0 : n;
}

// Copy the most recently rendered document's final URL (UTF-8) into buf. Used by the
// harness to detect a navigation triggered inside the live session (click default action).
int WebCoreGetUrl(char* buf, int len)
{
    if (!buf || len <= 0)
        return 0;
    int n = std::snprintf(buf, static_cast<size_t>(len), "%s", g_lastUrl);
    return n < 0 ? 0 : n;
}

// 在当前会话主世界执行一段 JS,结果转字符串写入 out。诊断/注入用。返回 0 成功。
int WebCoreEvalJS(const char* script, char* out, int len)
{
    if (!script || !out || len <= 0)
        return kErrBadArgs;
    out[0] = '\0';
    if (!g_session || !g_session->mainFrame)
        return kErrNoSession;
    if (g_inPump)
        return kErrBusy;
    return evalJS(*g_session->mainFrame, script, out, len);
}

// 当前页链接命中表:数量。
int WebCoreGetLinkCount()
{
    return static_cast<int>(g_links.size());
}

// 取第 i 个链接的矩形(位图坐标)+ URL。返回 1 成功 0 越界。
int WebCoreGetLink(int i, int* x, int* y, int* w, int* h, char* url, int len)
{
    if (i < 0 || i >= static_cast<int>(g_links.size()))
        return 0;
    const LinkRect& lr = g_links[i];
    if (x) *x = lr.x;
    if (y) *y = lr.y;
    if (w) *w = lr.w;
    if (h) *h = lr.h;
    if (url && len > 0)
        std::snprintf(url, static_cast<size_t>(len), "%s", lr.url.c_str());
    return 1;
}

// Download `url` to file `outPath` via a standalone curl handle (no render). Reuses the
// CA blob set by WebCoreSetCACertBlob. Returns the HTTP status code on success (e.g. 200),
// or negative on failure (-1 bad args, -2 file open, -3 curl init, -100-curlcode transfer).
// curl is already globally initialized by CurlContext (touched in SetupRuntimeEnv).
int WebCoreDownload(const char* url, const char* outPath)
{
    if (!url || !*url || !outPath || !*outPath)
        return -1;
    // 先写到 .part 临时文件,成功才改名到目标;失败则删除 .part。避免:① 传输中途失败残留半截
    // 文件;② "wb" 直接截断会在新下载失败时毁掉同名旧文件。
    std::string partPath = std::string(outPath) + ".part";
    FILE* fp = nullptr;
    if (fopen_s(&fp, partPath.c_str(), "wb") != 0 || !fp)
        return -2;
    CURL* h = curl_easy_init();
    if (!h) {
        std::fclose(fp);
        std::remove(partPath.c_str());
        return -3;
    }
    curl_easy_setopt(h, CURLOPT_URL, url);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, webcoreDownloadWrite);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, 120L);
    curl_easy_setopt(h, CURLOPT_USERAGENT,
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/16.4 Safari/605.1.15");
    if (!g_caBytes.empty()) {
        curl_blob blob;
        blob.data = g_caBytes.data();
        blob.len = g_caBytes.size();
        blob.flags = CURL_BLOB_COPY;
        curl_easy_setopt(h, CURLOPT_CAINFO_BLOB, &blob);
    }
    CURLcode rc = curl_easy_perform(h);
    long code = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(h);
    std::fclose(fp);
    if (rc != CURLE_OK) {
        std::remove(partPath.c_str());
        return -100 - static_cast<int>(rc);
    }
    if (code < 200 || code >= 400) {   // HTTP 错误:不留文件
        std::remove(partPath.c_str());
        return static_cast<int>(code);
    }
    // 成功:.part → 目标(覆盖旧的)。rename 在目标已存在时可能失败,先删目标。
    std::remove(outPath);
    if (std::rename(partPath.c_str(), outPath) != 0) {
        std::remove(partPath.c_str());
        return -4;
    }
    return static_cast<int>(code);
}

// Render `utf8Html` into a w*h RGBA8888 buffer.
// outRGBA must point to at least w*h*4 bytes. Returns 0 on success.
int WebCoreRenderHtml(const char* utf8Html, int w, int h, uint8_t* outRGBA)
{
    if (!utf8Html || !outRGBA || !isValidSurfaceSize(w, h))
        return kErrBadArgs;

    ensureWebCoreInitialized();

    // ---- 2. PageConfiguration with all-empty clients ----
    // pageConfigurationWithEmptyClients also wires up the main-frame creation
    // parameters (an EmptyLocalFrameLoaderClient), so Page::create() yields a
    // Page whose localMainFrame() is already present.
    auto pageConfiguration = pageConfigurationWithEmptyClients(
        std::nullopt, PAL::SessionID::defaultSessionID());

    // ---- 3. Page ----
    Ref<Page> page = Page::create(WTF::move(pageConfiguration));

    // Headless software render: no script, no compositing, no media.
    page->settings().setScriptEnabled(false);
    page->settings().setAcceleratedCompositingEnabled(false);
    page->settings().setShouldAllowUserInstalledFonts(false);
    page->settings().setSpeculationRulesPrefetchEnabled(g_apoSpecPrefetch);   // Apotheosis: privacy, see g_apoSpecPrefetch
#if ENABLE(VIDEO)
    page->settings().setMediaEnabled(false);
#endif

    // ---- 4. Main frame + view ----
    RefPtr<LocalFrame> localMainFrame = page->localMainFrame();
    if (!localMainFrame)
        return kErrNoMainFrame;

    localMainFrame->setView(LocalFrameView::create(*localMainFrame));
    localMainFrame->init();   // creates the initial empty document + DocumentLoader

    RefPtr<LocalFrameView> view = localMainFrame->view();
    if (!view)
        return kErrNoView;

    // Opaque white page background so text is visible (default would be
    // transparent and you'd get the raw transparency over the surface).
    view->setTransparent(false);
    view->setBaseBackgroundColor(Color::white);
    view->setCanHaveScrollbars(false);

    // ---- 5. Feed the HTML string through the DocumentWriter ----
    Ref<FrameLoader> loader = localMainFrame->loader();
    RefPtr<DocumentLoader> activeLoader = loader->activeDocumentLoader();
    if (!activeLoader)
        return kErrNoLoader;

    DocumentWriter& writer = activeLoader->writer();
    writer.setMIMEType("text/html"_s);
    writer.begin(URL());   // empty/about:blank-ish base URL; creates the document
    {
        const size_t len = std::strlen(utf8Html);
        Ref<SharedBuffer> buffer = SharedBuffer::create(
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(utf8Html), len));
        writer.addData(buffer.get());
    }
    writer.end();   // finishes parsing synchronously for this in-memory document

    // ---- 6. Size + layout ----
    const IntSize size(w, h);
    view->resize(size);   // Widget::resize -> setFrameRect; establishes layout viewport

    RefPtr<Document> document = localMainFrame->protectedDocument();
    if (!document)
        return kErrNoDocument;

    document->updateLayoutIgnorePendingStylesheets();   // force full style+layout now
    extractLinks(document.get(), h);                    // 提取链接命中表(点击交互)

    // ---- 7. Cairo image surface + GraphicsContextCairo + paint ----
    cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (!surface || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
        if (surface) cairo_surface_destroy(surface);
        return kErrCairoSurface;
    }

    cairo_t* cr = cairo_create(surface);
    if (!cr || cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
        if (cr) cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return kErrCairoContext;
    }

    {
        // GraphicsContextCairo adopts a RefPtr<cairo_t>. We created cr with a
        // refcount of 1, so hand ownership over via adoptRef (no extra ref).
        GraphicsContextCairo context(adoptRef(cr));   // RefPtr<cairo_t>&& ctor

        // Paint the whole view. ScrollView::paint(GraphicsContext&, const IntRect&)
        // (trailing args default to AnyOrigin / nullptr).
        view->paint(context, IntRect(IntPoint(), size));
    }   // context dtor derefs cr -> back to refcount 0, cairo_t destroyed

    cairo_surface_flush(surface);

    // ---- 8. Copy + swizzle into caller's RGBA8888 buffer ----
    // CAIRO_FORMAT_ARGB32 in memory (little-endian) == premultiplied B,G,R,A.
    const unsigned char* src = cairo_image_surface_get_data(surface);
    const int stride = cairo_image_surface_get_stride(surface);   // bytes per row, >= 4*w

    for (int y = 0; y < h; ++y) {
        const unsigned char* srow = src + static_cast<size_t>(y) * stride;
        uint8_t* drow = outRGBA + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            const unsigned char b = srow[x * 4 + 0];
            const unsigned char g = srow[x * 4 + 1];
            const unsigned char r = srow[x * 4 + 2];
            const unsigned char a = srow[x * 4 + 3];
            // Un-premultiply so the caller gets straight-alpha RGBA8888.
            if (a == 0 || a == 255) {
                drow[x * 4 + 0] = r;
                drow[x * 4 + 1] = g;
                drow[x * 4 + 2] = b;
                drow[x * 4 + 3] = a;
            } else {
                drow[x * 4 + 0] = static_cast<uint8_t>((r * 255 + a / 2) / a);
                drow[x * 4 + 1] = static_cast<uint8_t>((g * 255 + a / 2) / a);
                drow[x * 4 + 2] = static_cast<uint8_t>((b * 255 + a / 2) / a);
                drow[x * 4 + 3] = a;
            }
        }
    }

    cairo_surface_destroy(surface);

    // Page/frame/view are released here as the RefPtrs go out of scope.
    return kOK;
}

// ---------------------------------------------------------------------------
// WebCoreLoadUrl — load an http(s):// URL over the network (curl backend) and
// render the resulting page into a w*h RGBA8888 buffer. Sibling of
// WebCoreRenderHtml(): instead of feeding a local HTML string through the
// DocumentWriter, it drives a real provisional load through the FrameLoader,
// pumps the WebKit main-thread run loop until the main frame finishes (or a
// 30 s watchdog fires), then reuses the same Cairo paint + RGBA swizzle tail.
// Returns 0 on success, negative on failure (see kErr* above).
// ---------------------------------------------------------------------------
int WebCoreLoadUrl(const char* url, int w, int h, uint8_t* outRGBA)
{
    using namespace WebCore;

    if (!url || !outRGBA || !isValidSurfaceSize(w, h))
        return kErrBadArgs;

    g_lastNetError[0] = '\0';   // clear any stale diagnostic from a prior call
    g_loadStarted = g_loadResponse = g_loadComplete = g_loadFail = 0;   // 重置子资源计数

    // process init (JSC/MainThread/AtomStrings) + installPortPlatformStrategies()
    ensureWebCoreInitialized();

    URL parsedURL { String::fromUTF8(url) };
    if (!parsedURL.isValid())
        return kErrBadUrl;

    // ---- PageConfiguration with empty clients, then swizzle the main-frame
    //      loader-client factory to our LoadingFrameLoaderClient. ----
    auto pageConfiguration = pageConfigurationWithEmptyClients(
        std::nullopt, PAL::SessionID::defaultSessionID());

    // Shared terminal-state signal. The client completion handler, settle timer and
    // watchdog all run on this (main) thread, so no locking is needed.
    // Apotheosis: 主文档 didFinishLoad 后不立即停——JS 驱动型站点(bilibili 等)的视频封面等图片
    // 是 JS 动态/异步加载的,load 事件即停会在它们加载完前就快照(图片缺失)。改为等文档真正空闲
    // (isLoadingInAPISense:涵盖图片/脚本/XHR)再停,封顶 8s(之前撞的字体崩溃已修,可安全多跑)。
    struct LoadState {
        bool mainDone = false;
        bool failed   = false;
        bool stopped  = false;
        bool timedOut = false;
    } loadState;

    auto onLoadDone = [&loadState](bool failed) {
        if (loadState.mainDone)
            return;
        loadState.mainDone = true;
        loadState.failed = failed;
    };

    // Replace the EmptyLocalFrameLoaderClient factory with ours (preserving the
    // sandboxFlags/referrerPolicy defaults populated at index 0).
    {
        auto& params = std::get<PageConfiguration::LocalMainFrameCreationParameters>(
            pageConfiguration.mainFrameCreationParameters);
        params.clientCreator =
            CompletionHandler<UniqueRef<LocalFrameLoaderClient>(LocalFrame&, FrameLoader&)> {
            [onLoadDone](LocalFrame&, FrameLoader& frameLoader) mutable
                -> UniqueRef<LocalFrameLoaderClient> {
                auto client = makeUniqueRefWithoutRefCountedCheck<WebCorePort::LoadingFrameLoaderClient>(frameLoader);
                // Function<>'s ctor needs an rvalue; wrap a copy of onLoadDone in
                // a fresh rvalue lambda.
                client->setLoadCompletionHandler([onLoadDone](bool failed) { onLoadDone(failed); });
                return client;
            } };
    }

    // ---- Page ----
    Ref<Page> page = Page::create(WTF::move(pageConfiguration));

    // Apotheosis: 解禁 JavaScript(JSC CLoop 解释器,无 JIT)。JS 驱动型站点(如百度首页)
    // 禁脚本时主文档渲染为空白;开启后 JS 跑起来才会填充内容。代价是慢 + 触发大量 DOM 绑定。
    page->settings().setScriptEnabled(true);
    page->settings().setLoadsImagesAutomatically(true);   // 确保 <img>/CSS 背景图自动加载
    page->settings().setAcceleratedCompositingEnabled(false);
    page->settings().setShouldAllowUserInstalledFonts(false);
    page->settings().setSpeculationRulesPrefetchEnabled(g_apoSpecPrefetch);   // Apotheosis: privacy, see g_apoSpecPrefetch
#if ENABLE(VIDEO)
    page->settings().setMediaEnabled(false);
#endif
    // 标记页面可见,否则后台节流会推迟图片/定时器/资源加载(headless 默认可能非可见)。
    page->setIsVisible(true);

    // ---- Main frame + view ----
    RefPtr<LocalFrame> localMainFrame = page->localMainFrame();
    if (!localMainFrame)
        return kErrNoMainFrame;

    localMainFrame->setView(LocalFrameView::create(*localMainFrame));
    localMainFrame->init();   // creates initial empty document; FrameLoader ready

    RefPtr<LocalFrameView> view = localMainFrame->view();
    if (!view)
        return kErrNoView;

    view->setTransparent(false);
    view->setBaseBackgroundColor(Color::white);
    view->setCanHaveScrollbars(true);
    view->resize(IntSize(w, h));   // establish viewport BEFORE load

    // ---- Issue the network load ----
    ResourceRequest request { WTF::move(parsedURL) };
    FrameLoadRequest frameLoadRequest { *localMainFrame, WTF::move(request), SubstituteData { } };

    Ref<FrameLoader> loader = localMainFrame->loader();
    loader->load(WTF::move(frameLoadRequest));   // async: provisional load -> ResourceHandle::start -> curl

    // ---- Pump the main-thread run loop until the document is idle (or watchdog) ----
    auto stopLoop = [&loadState] {
        if (loadState.stopped)
            return;
        loadState.stopped = true;
        RunLoop::currentSingleton().stop();
    };

    // settle:主文档完成后每 50ms 查 isLoadingInAPISense(涵盖图片/脚本/XHR)。空闲即停,让 JS
    // 动态加载的图片(bilibili 封面等)有机会加载完。封顶"主文档完成后 8s"(160×50ms),避免有
    // 后台长连接的站点拖到 30s 看门狗。字体崩溃已修,多跑安全。
    int settleTicks = 0;
    RefPtr<LocalFrame> frameForSettle = localMainFrame;
    RunLoop::Timer settle(Ref { RunLoop::currentSingleton() }, "WebCoreLoadUrl.settle"_s,
        WTF::Function<void()> { [&loadState, &stopLoop, &settleTicks, frameForSettle] {
            if (!loadState.mainDone)
                return;
            ++settleTicks;
            RefPtr<DocumentLoader> dl = frameForSettle->loader().activeDocumentLoader();
            if (!dl || !dl->isLoadingInAPISense() || settleTicks > 160)
                stopLoop();
        } });
    settle.startRepeating(0.05_s);

    RunLoop::Timer watchdog(Ref { RunLoop::currentSingleton() }, "WebCoreLoadUrl.watchdog"_s,
        WTF::Function<void()> { [&loadState, &stopLoop] {
            loadState.timedOut = !loadState.mainDone;
            stopLoop();
        } });
    watchdog.startOneShot(30_s);

    if (!loadState.stopped)
        RunLoop::run();

    settle.stop();
    watchdog.stop();

    if (loadState.timedOut)
        return kErrLoadTimeout;
    if (loadState.failed)
        return kErrLoadFailed;

    // ---- Final layout ----
    RefPtr<Document> document = localMainFrame->protectedDocument();
    if (!document)
        return kErrNoDocument;

    // Apotheosis: 真实导航提交(commit)后,WebKit 给新文档新建了 LocalFrameView,加载前缓存
    // 的 `view` 已失效(指向旧的空视图)→ 绘制全白。这里重新取当前 view 并重设背景/尺寸再绘。
    view = localMainFrame->view();
    if (!view)
        return kErrNoView;
    view->setTransparent(false);
    view->setBaseBackgroundColor(Color::white);
    view->resize(IntSize(w, h));

    document->updateLayoutIgnorePendingStylesheets();
    extractLinks(document.get(), h);                    // 提取链接命中表(点击交互)

    // ---- Cairo paint + 诊断(与常驻会话路径共用同一实现)----
    int nonWhite = 0;
    int prc = paintToRGBA(*view, w, h, outRGBA, nonWhite);
    if (prc != kOK)
        return prc;
    writeDiag(*document, *view, w, h, nonWhite);
    return kOK;
}

// ===========================================================================
// 常驻交互会话 C ABI 导出
// ===========================================================================

// 加载 URL 并建立常驻会话(替代一次性 WebCoreLoadUrl)。之后可调 WebCoreClickAt / WebCoreScrollBy
// 在同一活文档上交互。返回 0 成功(语义同 WebCoreLoadUrl),失败已自动清理会话。
int WebCoreSessionLoad(const char* url, int w, int h, uint8_t* outRGBA)
{
    if (!url || !outRGBA || !isValidSurfaceSize(w, h))
        return kErrBadArgs;
    ensureWebCoreInitialized();
    if (g_inPump)
        return kErrBusy;
    teardownSession();
    g_lastNetError[0] = '\0';
    g_spaProbe[0] = '\0';
    g_lastPendingResources = 0;
    g_loadStarted = g_loadResponse = g_loadComplete = g_loadFail = 0;
    PerfOpGuard perfOp("nav", url, w, h);   // M4: one CSV row for this navigation (flushed on completion)
    g_session.emplace();
    g_session->w = w;
    g_session->h = h;
    int rc = buildSession(url, w, h, outRGBA);
    if (rc != kOK)
        teardownSession();   // 失败不留半截会话
    return rc;
}

// 关闭并销毁当前会话(导航到本地页 / 应用挂起时调用)。释放 Page 并取消在途加载。
void WebCoreCloseSession()
{
    if (g_inPump)
        return;
    const bool hadSession = g_session.has_value();
    teardownSession();
    // Apotheosis (MEMORY-PLAN.md §3 change 6): the user left the page for good (home screen,
    // suspend, tab closed) — unlike the navigation path there is no next page that would reuse
    // the encoded resources or the compiled code, so take the critical route as well.
    if (hadSession)
        wkReleaseMemoryLevel(2, /*keepResourceCache*/ false);
}

// ===========================================================================
// Apotheosis (synthetic mouse input, 2026-09-04): PlatformMouseEvent carries TWO button fields.
// `button` says which button this event is about; `buttons` is the W3C uievents bitmask of what is
// held down right now (1 = primary). The public constructor takes only the first and leaves
// m_buttons at 0, and nothing derives it later: MouseEvent::create() copies event.buttons()
// straight through (MouseEvent.cpp:70), and PointerEvent takes both the DOM `buttons` and - for
// the mouse pointer type - `pressure` from it (PointerEvent.cpp:202/207,
// pressureForPressureInsensitiveInputDevices(buttons())). So every mouse event this driver
// synthesised reached the page as `buttons: 0, pressure: 0`, i.e. a pointermove with nothing
// pressed. Widgets that pan on pointer events (Leaflet, MapLibre, canvas apps) test exactly
// that field to tell a drag from a hover, which is why the one-finger drag did nothing on Maps.
// PlatformMouseEventWin.cpp is the reference for the real values: a WM_MOUSEMOVE during a left
// drag carries button=Left AND buttons=1 (buttonsForEvent(), GDIUtilities.h:44), WM_LBUTTONDOWN
// carries 1, and WM_LBUTTONUP carries 0 because the button being released is no longer down.
// m_buttons is protected with no setter, so this three-line subclass is how it gets set.
// ===========================================================================
namespace {
class DriverMouseEvent final : public WebCore::PlatformMouseEvent {
public:
    DriverMouseEvent(const WebCore::DoublePoint& position, WebCore::MouseButton button,
                     WebCore::PlatformEvent::Type type, int clickCount,
                     OptionSet<WebCore::PlatformEvent::Modifier> modifiers, MonotonicTime timestamp,
                     unsigned short buttons)
        : WebCore::PlatformMouseEvent(position, position, button, type, clickCount, modifiers, timestamp,
                                      /*force*/ 0.0, WebCore::SyntheticClickType::NoTap)
    {
        m_buttons = buttons;
    }
};
}
static const unsigned short kButtonsLeftDown = 1;   // MouseEvent.buttons bit for the primary button
static const unsigned short kButtonsRightDown = 2;  // ...and for the secondary one (contextmenu, WebCoreLongPressAt)

// Apotheosis (2026-09-04): unwind a mousedown the page never got a mouseup for.
// EventHandler::handleMousePressEvent() sets m_mousePressed (EventHandler.cpp:2030) before it even
// hit-tests, and PointerCaptureController marks the pointer pressed when it dispatches the
// pointerdown - both regardless of whether anything consumed the event. A press left dangling
// therefore poisons the rest of the page's life: every later hover is treated as a drag move, and
// the next mousedown is turned into a pointermove instead of a pointerdown, because the chorded
// button rules see the pointer as already pressed (PointerCaptureController.cpp:425-429). That is
// the reported "a pin can be placed on the map exactly once". invalidateClick() first: this
// release is repair work, it must not fire a click of its own.
static void releaseDanglingPress(WebCore::LocalFrame& frame, const WebCore::DoublePoint& at,
                                 OptionSet<WebCore::PlatformEvent::Modifier> modifiers)
{
    if (!frame.eventHandler().mousePressed())
        return;
    frame.eventHandler().invalidateClick();
    DriverMouseEvent up(at, WebCore::MouseButton::Left, WebCore::PlatformEvent::Type::MouseReleased,
                        /*clickCount*/ 0, modifiers, MonotonicTime::now(), /*buttons*/ 0);
    frame.eventHandler().handleMouseReleaseEvent(up);
}

// Apotheosis (map tap / long press, 2026-09-06): one plain line per input gesture into crash.txt,
// the only writable path the driver knows. Same channel as pan-swap-drop / drag-press (no crash
// record, no crash-entry budget), capped so a session of tapping cannot fill the file. The
// caller formats the text with snprintf. Grep for "tap ", "drag-release", "long-press".
static void inputNote(const char* text)
{
    static int notes = 0;
    if (!text || !g_crashLogPath[0] || notes >= 24)
        return;
    ++notes;
    FILE* fp = nullptr;
    if (fopen_s(&fp, g_crashLogPath, "ab") != 0 || !fp)
        return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::fprintf(fp, "%s %02u:%02u:%02u.%03u\n", text, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    std::fclose(fp);
}

// Apotheosis: the drag-widget walk (canvas / touch-action:none), defined with WebCoreWantsDragAt
// further down — the tap and long-press paths want the same answer about the same point.
static bool dragWidgetAtPoint(WebCore::Document& doc, int x, int y);

// Apotheosis (long press, 2026-09-06): turn the run loop for `seconds` with the mouse button held
// down. A touch long press is a *timed* gesture: the widget starts a timer on pointerdown and does
// its thing (a map site drops a pin) when the timer fires, with the pointer still down. Nothing in
// this driver ever kept a button pressed across time before — WebCoreClickAt presses and releases
// in the same call, and WebCoreDragAt only holds the press between two harness calls — so the hold
// needs its own pump. Same shape as pumpLoop's settle timer (isolatedUpdateRendering per tick to
// keep rAF/animations running, microtask checkpoint, bail out if a navigation replaces the main
// frame), but it stops on the clock instead of on idleness.
static void holdPump(WebCore::LocalFrame& frame, WebCore::Page* pageForRendering, double seconds)
{
    using namespace WebCore;
    if (seconds <= 0.0)
        return;
    bool stopped = false;
    auto stopLoop = [&stopped] {
        if (stopped)
            return;
        stopped = true;
        RunLoop::currentSingleton().stop();
    };
    MonotonicTime deadline = MonotonicTime::now() + WTF::Seconds(seconds);
    RefPtr<LocalFrame> frameRef = &frame;
    RunLoop::Timer hold(Ref { RunLoop::currentSingleton() }, "WebCorePort.longpress.hold"_s,
        WTF::Function<void()> { [&stopLoop, deadline, frameRef, pageForRendering] {
            if (pageForRendering) {
                pageForRendering->isolatedUpdateRendering();
                RefPtr<LocalFrame> mf = pageForRendering->localMainFrame();
                if (mf.get() != frameRef.get()) {   // navigated away under us: the caller re-fetches
                    stopLoop();
                    return;
                }
            }
            if (RefPtr<Document> doc = frameRef->document())
                doc->eventLoop().performMicrotaskCheckpoint();
            if (MonotonicTime::now() >= deadline)
                stopLoop();
        } });
    hold.startRepeating(0.05_s);
    RunLoop::Timer watchdog(Ref { RunLoop::currentSingleton() }, "WebCorePort.longpress.watchdog"_s,
        WTF::Function<void()> { [&stopLoop] { stopLoop(); } });
    watchdog.startOneShot(WTF::Seconds(seconds + 2.0));   // a stuck tick must not hold the button for ever
    RunLoop::run();
    hold.stop();
    watchdog.stop();
}

// 在 (x,y)(位图/视口像素,无需减 scroll —— EventHandler 内部 windowToContents 会加 scrollY)派发一次
// 完整鼠标点击 move→down→up 到活文档,经真实命中测试 + 默认动作(链接导航 / 表单提交 / 按钮 onclick /
// SPA 交互)。之后等待可能的异步导航 settle、每 tick 驱动 rAF,然后重布局/提链接/重绘。返回 0 成功。
// Apotheosis (double-tap zoom, 2026-09-09): factored out with a clickCount parameter so
// WebCoreClickAtCount() (the second tap of a double-tap-to-zoom decision, clickCount=2 — WebCore's
// EventHandler dispatches 'dblclick' off exactly this number, see PlatformMouseEvent::clickCount())
// can share every line of this with the ordinary single click. WebCoreClickAt below is unchanged
// (clickCount=1); no existing caller's signature moves.
static int clickAtImpl(int x, int y, int clickCount, uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!outRGBA)
        return kErrBadArgs;
    if (!g_session || !g_session->mainFrame)
        return kErrNoSession;
    if (g_inPump)
        return kErrBusy;
    g_inPump = true;
    PumpGuard guard;   // 任何返回路径复位 g_inPump
    PerfOpGuard perfOp("click", nullptr, 0, 0);   // M4

    RefPtr<LocalFrame> lf = g_session->mainFrame;
    RefPtr<LocalFrameView> view = lf->view();
    if (!view)
        return kErrNoView;
    RefPtr<Document> doc = lf->document();
    if (!doc)
        return kErrNoDocument;
    // Apotheosis (2026-09-04): a press from an earlier gesture must not still be "down" here, or
    // this click's mousedown becomes a pointermove and the page never sees a press at all - see
    // releaseDanglingPress(). Normally a no-op; WebCoreDragAt now unwinds its own presses.
    const bool tapUnwound = lf->eventHandler().mousePressed();
    if (tapUnwound) {
        releaseDanglingPress(*lf, DoublePoint(static_cast<double>(x), static_cast<double>(y)), { });
        // That release dispatches a mouseup, which runs script and in the worst case navigates.
        lf = g_session->page ? g_session->page->localMainFrame() : nullptr;
        if (!lf)
            return kErrFrameGone;
        g_session->mainFrame = lf;
        view = lf->view();
        if (!view)
            return kErrNoView;
        doc = lf->document();
        if (!doc)
            return kErrNoDocument;
    }
    {
        PerfPhase perfLayout(&g_perfCur.styleLayout);
        doc->updateLayoutIgnorePendingStylesheets();   // 命中测试需要最新布局(尤其滚动后)
    }
    // Apotheosis (map tap, 2026-09-06): does this tap land on something that drags itself (map,
    // canvas)? Diagnostics only — asked here, before dispatching, because the click can navigate
    // and `doc` would then be a detached document.
    const bool tapWantsDrag = dragWidgetAtPoint(*doc, x, y);

    // 重新武装加载检测:点击触发的导航能被 pump 捕获。关键:signalLoadComplete 触发一次后会把完成回调
    // move 走(LoadingFrameLoaderClient.cpp:131),初次加载完成后回调已空 —— 故每次点击都必须重装,否则
    // 点击导航的完成永不被记录,pumpLoop 收不到 mainDone,会快照到导航中途的空白/旧页。
    g_session->load = DriverLoadState{};
    if (g_session->client) {
        g_session->client->resetLoadState();
        DriverLoadState* lp = &g_session->load;   // 稳定:g_session 在本次调用内不 reset
        g_session->client->setLoadCompletionHandler([lp](bool failed) {
            if (lp->mainDone)
                return;
            lp->mainDone = true;
            lp->failed = failed;
        });
    }

    DoublePoint p(static_cast<double>(x), static_cast<double>(y));
    OptionSet<PlatformEvent::Modifier> mods;
    MonotonicTime t = MonotonicTime::now();
    // Apotheosis (2026-09-04): the press carries buttons=1, the release buttons=0, exactly as the
    // Windows port derives them from the WM_ message (see DriverMouseEvent). Without it the page
    // got pointerdown/pointerup with buttons=0 and pressure=0, which is what a hover looks like.
    DriverMouseEvent move(p, MouseButton::None, PlatformEvent::Type::MouseMoved, 0, mods, t, 0);
    lf->eventHandler().handleMouseMoveEvent(move);     // 设 :hover / elementUnderMouse
    DriverMouseEvent down(p, MouseButton::Left, PlatformEvent::Type::MousePressed, clickCount, mods, t, kButtonsLeftDown);
    const bool tapDownHandled = lf->eventHandler().handleMousePressEvent(down).wasHandled();   // 安装 UserGestureIndicator
    DriverMouseEvent up(p, MouseButton::Left, PlatformEvent::Type::MouseReleased, clickCount, mods, MonotonicTime::now(), 0);
    const bool tapUpHandled = lf->eventHandler().handleMouseReleaseEvent(up).wasHandled();     // 派发 DOM 'click'(clickCount>=2 时 EventHandler 接着派发 'dblclick')+ 默认动作(导航/提交)
    // Apotheosis (map tap, 2026-09-06): settle on device what a plain tap actually delivers. The
    // sequence above IS a clean click - hover move, press (clickCount 1, buttons 1), release
    // (buttons 0, which is what makes EventHandler dispatch the DOM 'click'), all three at exactly
    // the same point - so if a tap on a map does not do what a tap in a real browser does, the
    // cause is on the page's side (Maps' mobile UI wants a LONG press for a pin, see
    // WebCoreLongPressAt) and not a missing or mismatched event here. `wants` says whether the tap
    // landed on a drag widget, i.e. whether the drag route would have claimed the same point.
    {
        char note[160];
        std::snprintf(note, sizeof note, "tap at=%d,%d cc=%d wants=%d down=%d up=%d unwound=%d",
            x, y, clickCount, tapWantsDrag ? 1 : 0, tapDownHandled ? 1 : 0,
            tapUpHandled ? 1 : 0, tapUnwound ? 1 : 0);
        inputNote(note);
    }

    // ★ 显式聚焦命中点的可编辑元素:headless 下合成点击对"设置焦点"的副作用不稳定(时灵时不灵 → 键盘
    //   时弹时不弹)。这里命中测试点击点,若落在 text input / textarea / contenteditable 上就直接 focus(),
    //   让 WebCoreFocusedEditable 稳定返回 1(弹键盘)、后续 WebCoreTypeText 有确定的插入目标。
    if (RefPtr<Document> hdoc = lf->document()) {
        if (RefPtr<Element> hit = hdoc->elementFromPoint(static_cast<double>(x), static_cast<double>(y))) {
            RefPtr<Element> target;
            for (RefPtr<Element> e = hit; e; e = e->parentElement()) {
                if ((is<HTMLInputElement>(*e) && downcast<HTMLInputElement>(*e).isTextField())
                    || is<HTMLTextAreaElement>(*e)) { target = e; break; }
            }
            if (!target && is<HTMLElement>(*hit) && downcast<HTMLElement>(*hit).isContentEditable())
                target = hit;
            if (target)
                target->focus();
        }
    }

    // 同步处理器(JS onclick 等)已返回;导航(若有)异步 → settle。无导航则空闲早停。
    {
        PerfPhase perfSettle(&g_perfCur.settle);   // M4
        pumpLoop(*lf, &g_session->load.mainDone, /*allowEarlyStopWithoutNav*/ true,
                 /*settleCapTicks*/ 160, /*watchdog*/ 30.0, /*pageForRendering*/ g_session->page.get());
    }

    // 导航会重建 view/frame,重新校验 + 重取。
    lf = g_session->page->localMainFrame();
    if (!lf) {
        teardownSession();
        return kErrFrameGone;
    }
    g_session->mainFrame = lf;
    view = lf->view();
    if (!view)
        return kErrNoView;
    view->setTransparent(false);
    view->setBaseBackgroundColor(Color::white);
    view->resize(IntSize(g_session->w, g_session->h));
    doc = lf->document();
    if (!doc)
        return kErrNoDocument;
    {
        PerfPhase perfLayout(&g_perfCur.styleLayout);
        doc->updateLayoutIgnorePendingStylesheets();
    }
    extractLinks(doc.get(), g_session->h);

    int nonWhite = 0;
    int prc = paintToRGBA(*view, g_session->w, g_session->h, outRGBA, nonWhite);
    if (prc != kOK)
        return prc;
    writeDiag(*doc, *view, g_session->w, g_session->h, nonWhite);
    return kOK;
}

int WebCoreClickAt(int x, int y, uint8_t* outRGBA)
{
    return clickAtImpl(x, y, /*clickCount*/ 1, outRGBA);
}

// Apotheosis (double-tap zoom, 2026-09-09): the second tap of a double tap the harness decided is
// NOT going to zoom (WebCoreTapPolicyAt said not-zoomable) — dispatched with clickCount=2 so the
// page gets the 'dblclick' a two-click page (e.g. text selection, some custom widgets) expects,
// exactly like a real touch browser forwards it when double-tap-to-zoom does not apply.
int WebCoreClickAtCount(int x, int y, int clickCount, uint8_t* outRGBA)
{
    return clickAtImpl(x, y, clickCount, outRGBA);
}

// 垂直滚动 dy 像素(正=向下)并重绘。每 tick isolatedUpdateRendering 驱动 IntersectionObserver,
// 使下方/懒加载图片(bilibili 封面等)真正加载。位置钳制到 [min,max]。返回 0 成功。
int WebCoreScrollBy(int dx, int dy, uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!outRGBA)
        return kErrBadArgs;
    if (!g_session || !g_session->mainFrame)
        return kErrNoSession;
    if (g_inPump)
        return kErrBusy;
    g_inPump = true;
    PumpGuard guard;
    PerfOpGuard perfOp("scroll", nullptr, 0, 0);   // M4

    // Apotheosis (M4): large images now decode on WebCore's ImageFrameWorkQueue
    // (RenderBoxModelObject::decodingModeForImageDraw, WK_WINUWP). The decoder
    // hands the NativeImage back via callOnMainThread, i.e. through the RunLoop
    // function queue - without draining it here a finished decode would only
    // become visible on the next live tick, so images would never appear while
    // the finger keeps scrolling. One iteration is enough and is what
    // WebCoreLiveTick already does (three times).
    RunLoop::cycle();

    RefPtr<LocalFrame> lf = g_session->mainFrame;
    RefPtr<LocalFrameView> view = lf->view();
    if (!view)
        return kErrNoView;
    RefPtr<Document> doc = lf->document();
    if (!doc)
        return kErrNoDocument;
    {
        PerfPhase perfLayout(&g_perfCur.styleLayout);
        doc->updateLayoutIgnorePendingStylesheets();   // contentsSize/最大滚动有效
    }

    ScrollPosition cur = view->scrollPosition();
    ScrollPosition minP = view->minimumScrollPosition();
    ScrollPosition maxP = view->maximumScrollPosition();
    int tx = cur.x() + dx;
    int ty = cur.y() + dy;
    if (tx < minP.x()) tx = minP.x();
    if (tx > maxP.x()) tx = maxP.x();
    if (ty < minP.y()) ty = minP.y();
    if (ty > maxP.y()) ty = maxP.y();
    view->setScrollPosition(ScrollPosition(tx, ty));

    // ★ M3 快滚:不再每帧跑 pumpLoop(8s 看门狗的多轮 rendering-update)+ 重取帧 + resize + 二次 layout
    //   —— 那是"很卡"的元凶。这里只一次 isolatedUpdateRendering(驱动 scroll steps/IntersectionObserver 注册,
    //   轻量)+ 刷新链接表 + 合成。懒加载图片/动画交给滚动停止后的 StartLiveMode(WebCoreLiveTick 逐帧补)。
    //   纯滚动不跑 JS 不会导航,故不重取帧(导航只发生在 click/输入/load)。
    {
        PerfPhase perfRender(&g_perfCur.renderUpdate);   // M4
        g_session->page->isolatedUpdateRendering();
    }
    // ★ 提速:滚动期间不再每帧 extractLinks(其对每个锚点调 boundingClientRect,长页/链接多时是每帧大头)
    //   也不写诊断串。点击走引擎真实命中测试(权威,不依赖链接表);链接表由滚动停止后 WebCoreSyncLinks 一次性刷新。
    int nonWhite = 0;
    g_gpuScrollFast = true;   // 滚动快路径:本次合成跳过 forceDirtyTree(内容未变,只移动滚动层)→ 去卡顿
    int prc = paintToRGBA(*view, g_session->w, g_session->h, outRGBA, nonWhite);
    if (prc != kOK)
        return prc;
    return kOK;
}

// Apotheosis (instant pan): where the main frame actually is, so the harness can turn "what the
// finger asked for" into "what the engine has not applied yet" and clamp its own pan preview to the
// document. Deliberately does NO layout and NO paint: it is called straight after a
// WebCoreScrollBy on the same engine-thread hop, whose updateLayoutIgnorePendingStylesheets() has
// just run, and at gesture start where a stale-by-one-frame answer is harmless. Not gated on
// g_inPump either — it changes nothing, so it cannot re-enter anything.
//
// viewW/viewH are reported as contentsSize - maximumScrollPosition rather than as
// visibleContentRect(), so that the harness' max = content - view is *exactly* the position
// WebCoreScrollBy clamps to (headers/footers and the minimum scroll position included). Everything
// is in the same units as WebCoreScrollBy's dx/dy.
int WebCoreGetScrollState(int* x, int* y, int* contentW, int* contentH, int* viewW, int* viewH)
{
    using namespace WebCore;
    if (!g_session || !g_session->mainFrame)
        return kErrNoSession;
    RefPtr<LocalFrameView> view = g_session->mainFrame->view();
    if (!view)
        return kErrNoView;

    const ScrollPosition cur = view->scrollPosition();
    const ScrollPosition maxP = view->maximumScrollPosition();
    const IntSize contents = view->contentsSize();
    if (x) *x = cur.x();
    if (y) *y = cur.y();
    if (contentW) *contentW = contents.width();
    if (contentH) *contentH = contents.height();
    if (viewW) *viewW = contents.width() - maxP.x();
    if (viewH) *viewH = contents.height() - maxP.y();
    return kOK;
}

// Apotheosis (THREADED-COMPOSITOR-PLAN.md C5): register the harness' present wake-up. Pass
// nullptr to go back to pure polling (the fixed-interval live tick). See the "event-driven
// present" block near the top of this file for the threading contract: the callback can be
// invoked on the engine thread OR on a raster worker, must not block and must not re-enter the
// engine - it may only post to a queue. Registration itself is expected on the engine thread,
// once at startup, but the atomics make a late or repeated registration harmless.
void WebCoreSetPresentRequestCallback(void (*cb)(void*), void* ctx)
{
    g_presentCbCtx.store(ctx, std::memory_order_release);
    g_presentCb.store(cb, std::memory_order_release);
    g_presentWakeArmed.store(0, std::memory_order_release);
}

// Apotheosis (nested-scroll support): cheap probe so the harness can decide, at gesture start,
// whether a touch-pan should route through WebCoreWheelAt (nested scroller under the finger) or
// go straight to the WebCoreScrollBy main-frame fast path — without dispatching a real event.
// Hit test only (elementFromPoint(), same call WebCoreClickAt already uses to find the focus
// target); walks the render tree up from the hit element the same way WebCore's own wheel/touch
// default-action target search does (Source/WebCore/dom/Node.cpp defaultEventHandler(), the
// PAN_SCROLLING and TOUCH_EVENTS legs both walk renderer()->parent() looking for the first
// RenderBox::canBeScrolledAndHasScrollableArea()). Also counts an ancestor <iframe> as scrollable
// (its own EventHandler/FrameView owns that, not this frame's). Stops at the RenderView — the
// main frame itself is never "nested". (x,y) = viewport/bitmap px, same convention as
// WebCoreClickAt/WebCoreScrollBy. Returns 1/0; no session or bad hit test also returns 0.
int WebCoreIsScrollableAt(int x, int y)
{
    using namespace WebCore;
    if (!g_session || !g_session->mainFrame)
        return 0;
    if (g_inPump)
        return 0;
    // Apotheosis (review 2026-09-03): hold the pump guard for the layout below, like every other
    // entry point that runs one (WebCoreScrollBy/WebCoreWheelAt/WebCoreClickAt). This function
    // only *checked* g_inPump and never set it, so its updateLayoutIgnorePendingStylesheets() —
    // which can run scripts through pending-stylesheet/font callbacks and re-enter the driver —
    // was the one layout in the driver with nothing serialising it against a concurrent op.
    g_inPump = true;
    PumpGuard guard;

    RefPtr<LocalFrame> lf = g_session->mainFrame;
    RefPtr<Document> doc = lf->document();
    if (!doc)
        return 0;
    doc->updateLayoutIgnorePendingStylesheets();   // hit test needs current layout, as in WebCoreClickAt

    RefPtr<Element> hit = doc->elementFromPoint(static_cast<double>(x), static_cast<double>(y));
    if (!hit)
        return 0;

    // Apotheosis (review 2026-09-03): TreeScope::elementFromPoint retargets its result to the tree
    // scope it was called on (TreeScope.cpp: retargetToScope() after nodeFromPoint), so on the
    // document scope a point inside a web component always resolves to the *host* element, not to
    // the overflow:auto div the finger is actually over. Cookie banners and consent modals are
    // routinely built that way, and the walk below then only sees the host's (unscrollable)
    // renderer chain. Descend instead: as long as the current hit is a shadow host whose root is
    // open, ask that root the same question — its elementFromPoint retargets to *its* scope, one
    // level deeper. Closed and UA shadow roots are deliberately skipped: they are not script-
    // reachable either, and DisallowUserAgentShadowContent is what the hit test uses anyway.
    // (Element::shadowRoot() is inline in ElementRareData.h, which this port does not export as a
    // private header — openOrClosedShadowRoot() is the out-of-line accessor for the same field.)
    for (int depth = 0; depth < 16; ++depth) {
        RefPtr<ShadowRoot> shadow = hit->openOrClosedShadowRoot();
        if (!shadow || shadow->mode() != ShadowRootMode::Open)
            break;
        RefPtr<Element> inner = shadow->elementFromPoint(static_cast<double>(x), static_cast<double>(y));
        if (!inner || inner == hit)
            break;
        hit = WTF::move(inner);
    }

    for (RenderObject* r = hit->renderer(); r; r = r->parent()) {
        if (r->isRenderView())
            break;   // reached the main frame's own box — not nested, stop here
        if (Node* node = r->node()) {
            if (is<HTMLIFrameElement>(*node))
                return 1;
        }
        if (is<RenderBox>(*r) && downcast<RenderBox>(*r).canBeScrolledAndHasScrollableArea())
            return 1;
    }
    return 0;
}

// Apotheosis (nested-scroll support): dispatch a synthetic wheel event at (x,y) so WebCore's own
// scroll routing — the same default-action walk WebCoreIsScrollableAt above inspects
// (EventHandler::handleWheelEventInAppropriateEnclosingBox, Source/WebCore/page/EventHandler.cpp)
// — can hand the delta to the innermost overflow:auto container, a modal, or an iframe under the
// point, instead of only ever moving the main frame the way WebCoreScrollBy does. (x,y) =
// viewport/bitmap px, same convention as WebCoreClickAt/ScrollBy (EventHandler's
// windowToContents() adds the scroll offset internally, see the comment on WebCoreClickAt).
// deltaX/deltaY = px, granularity ScrollByPixelWheelEvent. `phase` (0 none/1 began/2 changed/
// 3 ended) is accepted for a future gesture-latching port but currently inert: OptionsWinUWP.cmake
// sets ENABLE_ASYNC_SCROLLING OFF (KINETIC_SCROLLING is likewise off), so on this port
// PlatformWheelEventPhase has only the `None` enumerator — Began/Changed/Ended do not exist to
// name — and PlatformWheelEvent's only public constructor (the one used below) does not expose a
// phase setter regardless. EventHandler still routes the event correctly with Phase::None;
// phases only ever refine latching/momentum on the platforms that have them.
//
// Returns 1 if a nested scroller consumed the event (WebCore reported it handled AND the
// main-frame scroll position did not move), 0 to tell the harness to fall back to its
// WebCoreScrollBy path for this delta. handleWheelEvent()'s own default action can itself scroll
// the main FrameView when nothing nested claims the delta first
// (EventHandler::processWheelEventForScrolling -> handleWheelEventInScrollableArea(view)) — so
// whenever that happens (or nothing was handled at all) the main-frame scroll position is
// explicitly restored here before returning 0. That keeps WebCoreScrollBy the *only* thing that
// ever moves the main frame, so the harness can always call it unconditionally on a 0 return
// without risking a double-scroll.
//
// outRGBA: on a 1 (consumed) return this composites/presents the frame the same way
// WebCoreScrollBy does (paintToRGBA — direct swap in GPU present mode, cairo readback into
// outRGBA otherwise), so the harness's per-gesture coalescing loop (PumpNestedScroll) gets exactly
// one frame per flushed job instead of waiting for the next live tick to notice m_lastFrameHash
// changed. A null outRGBA is tolerated (no present attempted, same as passing one in but the
// caller not looking at it) — kept optional-by-null rather than added to the bad-args check
// because a failed present must not turn a real "consumed" answer into a 0 (that would risk
// WebCoreScrollBy double-moving the main frame for the same delta).
// Apotheosis (pinch on map widgets, 2026-09-06): the body of WebCoreWheelAt, with the two knobs
// the zoom variant below needs.
//   ctrlKey     — a wheel with ctrl held is what a page reads as "zoom me" (Leaflet, MapLibre
//                 and OpenLayers all zoom on a plain wheel over the map AND on ctrl+wheel; the
//                 modifier is what stops an embedded map from merely scrolling the page).
//   zoomMode    — changes what counts as "the page took it" and what happens to the main frame.
//                 For the scroll route only an actual nested scroll counts (a preventDefault that
//                 scrolled nothing must fall back to WebCoreScrollBy, see the long comment above).
//                 A zooming map does exactly that, though: it preventDefaults the wheel and moves
//                 nothing scrollable — so in zoom mode "handled OR default-prevented" is the
//                 answer, and the main frame is restored unconditionally, because a pinch must
//                 never scroll the document underneath it.
// ticksX/ticksY are the wheel's tick count (DOM wheelDelta = ticks x 120); the scroll route passes
// its pixel deltas there as it always did.
static int wheelAtImpl(int x, int y, float deltaX, float deltaY, float ticksX, float ticksY,
                       bool ctrlKey, bool zoomMode, uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!g_session || !g_session->mainFrame)
        return 0;
    if (g_inPump)
        return 0;
    g_inPump = true;
    PumpGuard guard;

    // Same reason WebCoreScrollBy does this first: drain a decode callback that finished mid-
    // gesture so it is visible before we hit-test/scroll, not one tick later.
    RunLoop::cycle();

    RefPtr<LocalFrame> lf = g_session->mainFrame;
    RefPtr<LocalFrameView> view = lf->view();
    if (!view)
        return 0;
    RefPtr<Document> doc = lf->document();
    if (!doc)
        return 0;
    doc->updateLayoutIgnorePendingStylesheets();   // hit test + scrollable-area lookup need current layout

    const ScrollPosition beforeMain = view->scrollPosition();

    IntPoint p(x, y);
    PlatformWheelEvent wheelEvent(p, p, deltaX, deltaY, ticksX, ticksY,
        PlatformWheelEventGranularity::ScrollByPixelWheelEvent,
        /* shiftKey */ false, ctrlKey, /* altKey */ false, /* metaKey */ false);
    OptionSet<WheelEventProcessingSteps> steps { WheelEventProcessingSteps::SynchronousScrolling,
        WheelEventProcessingSteps::BlockingDOMEventDispatch };   // the default/synchronous steps (see EventHandlerMac/IOS)
    auto [result, handling] = lf->eventHandler().handleWheelEvent(wheelEvent, steps);

    const ScrollPosition afterMain = view->scrollPosition();
    // Apotheosis: "consumed by a nested scroller" must mean something actually scrolled.
    // HandleUserInputEventResult::wasHandled() is ALSO true when the page's own wheel listener
    // merely called preventDefault() and scrolled nothing: EventHandler::handleWheelEventInternal
    // (Source/WebCore/page/EventHandler.cpp, the `if (!element->dispatchWheelEvent(...))` leg)
    // returns handled and records EventHandling::DefaultPrevented in `handling`. Reporting that as
    // 1 made the harness skip its WebCoreScrollBy fallback for the delta, so every site carrying a
    // non-passive wheel listener (analytics/sticky-header scripts, most cookie banners) became
    // completely unscrollable. Require all three: handled, not default-prevented, main frame
    // still where it was.
    const bool consumedByNested = zoomMode
        ? (result.wasHandled() || handling.contains(EventHandling::DefaultPrevented))
        : (result.wasHandled()
            && !handling.contains(EventHandling::DefaultPrevented)
            && (afterMain == beforeMain));

    if ((zoomMode || !consumedByNested) && afterMain != beforeMain)
        view->setScrollPosition(beforeMain);   // undo any main-frame move: that is WebCoreScrollBy's job
                                               // (and in zoom mode nothing may scroll the page at all)

    if (consumedByNested) {
        // Same present path WebCoreScrollBy uses (578cbc3/82c5cef): commit the moved layer's
        // compositing update now — this is what arms PortChromeClient::m_needsPresent, via
        // scheduleRenderingUpdate().
        g_session->page->isolatedUpdateRendering();
        // NOTE: deliberately NOT g_gpuScrollFast here (unlike WebCoreScrollBy). That flag skips
        // forceDirtyTree for the next composite, which is only correct when the moved content has
        // its own composited layer whose tiles are already painted — true for the main frame's
        // scrolled-contents layer, but an overflow:auto container usually has no compositing layer
        // of its own and is painted into its enclosing layer's backing store. Skipping the dirty
        // pass there would re-present the identical tiles, i.e. the nested scroller would not move
        // on screen at all.
        // Coalescing fix: without this, the moved nested scroller only reached the screen on the
        // next live tick (up to 200ms later, or never mid-drag since ticks pause while a gesture
        // holds the engine busy) — a consumed wheel event changed the DOM/layer position but this
        // function returned before anyone composited/presented it. Present now, right here, so
        // PumpNestedScroll's one-job-in-flight loop yields one frame per flush, same as
        // WebCoreScrollBy below.
        if (outRGBA) {
            int nonWhite = 0;
            paintToRGBA(*view, g_session->w, g_session->h, outRGBA, nonWhite);   // best-effort: a paint
            // failure must not flip this back to 0 — the main frame is already confirmed untouched
            // above, so WebCoreScrollBy must NOT also run for this delta regardless of paint outcome.
        }
    }

    return consumedByNested ? 1 : 0;
}

int WebCoreWheelAt(int x, int y, float deltaX, float deltaY, int phase, uint8_t* outRGBA)
{
    (void)phase;   // see comment above: inert on this port (no ASYNC/KINETIC scrolling, no phase setter)
    return wheelAtImpl(x, y, deltaX, deltaY, deltaX, deltaY, /*ctrlKey*/ false, /*zoomMode*/ false, outRGBA);
}

// Apotheosis (pinch on map widgets, 2026-09-06): `notches` wheel clicks with ctrl held at (x,y),
// positive = wheel up = zoom in. A pinch over a map has to become this: the harness' pinch scales
// the whole page (WebCoreSetPageScale), which on a map means zooming a picture of the map instead
// of asking the map for more detail — the tiles stay at the old zoom level and the labels grow
// blurry. One notch is 120 px of delta and one wheel tick, i.e. exactly what a mouse wheel sends
// (DOM deltaY = -120 per notch up, wheelDeltaY = +120), so the map's own wheel handler applies its
// normal one-step zoom around the point. Returns 1 if the page took it, 0 if nothing did — on a 0
// the caller can fall back to page zoom. The document itself never scrolls on this path.
int WebCoreZoomWheelAt(int x, int y, int notches, uint8_t* outRGBA)
{
    if (!notches)
        return 0;
    if (notches > 8) notches = 8;            // one gesture step should never be a whole zoom range
    if (notches < -8) notches = -8;
    const float ticks = static_cast<float>(notches);
    return wheelAtImpl(x, y, /*deltaX*/ 0.0f, /*deltaY*/ ticks * 120.0f, /*ticksX*/ 0.0f, ticks,
                       /*ctrlKey*/ true, /*zoomMode*/ true, outRGBA);
}

// ===========================================================================
// Apotheosis (drag as pointer events): map widgets — Leaflet, MapLibre /
// Leaflet, canvas apps — pan by listening to pointerdown/mousedown themselves and
// moving their own content. They scroll no scrollable box at all, so BOTH of the
// harness' existing gesture routes are wrong for them: the main-frame fast path
// (WebCoreScrollBy) scrolls the document behind the map, and the nested route
// (WebCoreWheelAt) finds nothing that consumes a wheel and falls back to exactly
// that. WebCore already knows what such a page wants — it only never receives the
// events, because a touch pan in this port is translated to scrolling, never to
// input. The two exports below give the harness (a) a cheap "does this point
// belong to something that drags itself?" probe to run at gesture start, next to
// WebCoreIsScrollableAt, and (b) a way to feed the gesture to the page as a real
// left-button mouse drag (mousedown → mousemove* → mouseup, which the engine also
// turns into pointerdown/pointermove/pointerup — PointerEvent is built in this
// port even though ENABLE_TOUCH_EVENTS is off, so pointer-first libraries like
// Leaflet and MapLibre get the events they actually listen for).
// ===========================================================================

// Hit test only, no event dispatched — same shape as WebCoreIsScrollableAt above
// (pump guard, layout, elementFromPoint, open-shadow descent, walk up the element
// chain). Returns 1 when the element under (x,y) or one of its ancestors looks
// like it handles dragging itself:
//   * a JS listener for pointerdown / mousedown / touchstart / pointermove /
//     touchmove (Leaflet, OpenLayers, MapLibre and every canvas app
//     register at least one of these on their container; hasEventListeners() is a
//     hash lookup on the target's listener map, so this stays cheap even though
//     five names are asked per ancestor). touchstart/touchmove are worth asking
//     even though ENABLE_TOUCH_EVENTS is 0 here: addEventListener() stores the
//     listener regardless of whether the engine ever fires that event, so the
//     name is still a reliable marker of "this widget wants the gesture".
//   * a <canvas> (the app draws and pans its own content — there is nothing else
//     a drag over it could sensibly mean)
//   * CSS touch-action other than auto/manipulation: none / pan-x / pan-y is
//     precisely how a widget tells the UA "I take this gesture", and it is what
//     .leaflet-container, .maplibregl-canvas and the map roots all set.
// The walk deliberately stops at <body>/<html>: page-wide mousedown handlers
// (dropdown menus, "click outside to close", analytics) sit on the document and
// body of half the web, and treating those as drag widgets would make ordinary
// pages stop scrolling. (x,y) = viewport/bitmap px, same convention as
// WebCoreClickAt/WebCoreScrollBy/WebCoreIsScrollableAt. No session, no hit, or a
// concurrent engine op all answer 0 — the harness then keeps its scroll routing.
// Apotheosis (2026-09-04): the walk itself, factored out of WebCoreWantsDragAt() so WebCoreDragAt()
// can ask the same question about the same point. Requires current layout (both callers run
// updateLayoutIgnorePendingStylesheets() first) and dispatches nothing.
// Apotheosis (2026-09-04, device package 14): ONE criterion, used by both callers. There used to be
// a loose variant for the routing probe (WebCoreWantsDragAt) that accepted a bare
// pointerdown/mousedown/touchstart listener, on the theory that being wrong only costs one engine
// hop. It costs more than that - practically every interactive container on a normal page has such
// a listener, so every pan on such a page was routed as a drag first, and the press round trip put
// a visible hitch at the start of each gesture even when it was unwound. And because the routing
// probe and the ownership decision must agree anyway (the harness only calls WebCoreDragAt after
// the probe said yes), a criterion that only one of them applies is a bug generator: 037eef0's
// `handled || wants` with the loose walk handed EVERY gesture on an ordinary page to the document
// as a mouse drag and the page stopped scrolling altogether.
//
// What is left is the evidence that actually means "this element drags itself": a canvas (Google
// Maps, the widget 037eef0 was written for, is one), or touch-action: none. Note that pan-x/pan-y
// deliberately do NOT count: they say the page wants the BROWSER to pan in the other axis, which is
// the opposite of claiming the gesture, and a `touch-action: pan-y` wrapper (an extremely common
// way to suppress horizontal overscroll - a news site has one) is exactly what stole every vertical pan
// on device package 14 with `wants=1 own=1` on ordinary article text.
static bool dragWidgetAtPoint(WebCore::Document& doc, int x, int y)
{
    using namespace WebCore;
    RefPtr<Element> hit = doc.elementFromPoint(static_cast<double>(x), static_cast<double>(y));
    if (!hit)
        return false;

    // Same open-shadow descent as WebCoreIsScrollableAt: TreeScope::elementFromPoint retargets its
    // result to the scope it was called on, so on the document scope a point inside a web component
    // resolves to the host element and the listeners on the real target would never be seen. Map
    // widgets are increasingly shipped as custom elements (gmp-map is one), so this matters here too.
    for (int depth = 0; depth < 16; ++depth) {
        RefPtr<ShadowRoot> shadow = hit->openOrClosedShadowRoot();
        if (!shadow || shadow->mode() != ShadowRootMode::Open)
            break;
        RefPtr<Element> inner = shadow->elementFromPoint(static_cast<double>(x), static_cast<double>(y));
        if (!inner || inner == hit)
            break;
        hit = WTF::move(inner);
    }

    Element* root = doc.documentElement();
    for (RefPtr<Element> e = hit; e; e = e->parentElement()) {
        if (e.get() == root || is<HTMLBodyElement>(*e))
            break;   // page-wide handlers are not a drag widget — see the comment above
        if (is<HTMLCanvasElement>(*e))
            return true;
        if (RenderObject* r = e->renderer()) {
            if (r->style().touchAction().isNone())
                return true;   // none: the element takes the whole gesture, in both axes
        }
    }
    return false;
}

int WebCoreWantsDragAt(int x, int y)
{
    using namespace WebCore;
    if (!g_session || !g_session->mainFrame)
        return 0;
    if (g_inPump)
        return 0;
    g_inPump = true;
    PumpGuard guard;

    RefPtr<LocalFrame> lf = g_session->mainFrame;
    RefPtr<Document> doc = lf->document();
    if (!doc)
        return 0;
    doc->updateLayoutIgnorePendingStylesheets();   // hit test needs current layout, as in WebCoreClickAt
    return dragWidgetAtPoint(*doc, x, y) ? 1 : 0;
}

// Apotheosis (drag as pointer events): drive one touch pan through WebCore as a left-button mouse
// drag. `phase`: 0 = press, 1 = move, 2 = release, 3 = cancel. (x,y) = viewport/bitmap px, same
// convention as every other input export (EventHandler's windowToContents() adds the scroll offset
// internally). Returns 1 while the page owns the gesture, 0 to tell the harness to route the rest
// of it down its normal scroll path.
//
// The press is the decision point, and (2026-09-04) it is NOT decided by handleMousePressEvent()'s
// result alone. That result is only true when a listener called preventDefault() or a WebCore
// default action took the press, and the widgets this export exists for do neither: a map site
// listens for pointerdown, stores the anchor and lets the event through. On device every press on
// the map site answered `handled=0`, so the gesture was handed back before a single mousemove was
// sent. The press is therefore owned when EITHER it was handled OR the point still looks like a drag
// widget (dragWidgetAtPoint(), the same walk WebCoreWantsDragAt() runs - the harness has already
// asked it once at gesture start, and asking again here costs one hit test and keeps the two
// answers from drifting apart across the press).
//
// Phases 1–3 are inert unless the press was taken (g_dragActive). That keeps the contract simple for
// the caller — once the press answers 0 the whole gesture is the harness' again, and no half-drag can
// leak into the page — and it means individual mousemove results are never consulted: a map that
// preventDefaults pointerdown but not mousemove (most of them) would otherwise look "not consumed"
// on its second event and have the gesture yanked away mid-pan. (g_dragActive is declared with the
// other engine-thread session flags at the top of this file, because teardownSession() clears it.)
//
// outRGBA (optional, may be null): on a 1 return the moved content is composited/presented exactly
// the way WebCoreWheelAt does it — isolatedUpdateRendering() to arm PortChromeClient::m_needsPresent,
// then paintToRGBA. Without this the page would only reach the screen on the next live tick, i.e.
// never during a gesture that keeps the engine busy. Deliberately NOT g_gpuScrollFast: the widget
// repaints its own content into an ordinary backing store, so the dirty pass must run.
// Apotheosis (drag as pointer events, diagnostics 2026-09-04): one line per gesture into crash.txt,
// the only writable path the driver knows. Same plain-line channel as pan-swap-drop (no crash
// record, no crash-entry budget), capped so a session of panning cannot fill the file.
// Grep for "drag-press".
static void dragPressNote(int x, int y, bool handled, bool wants, bool own)
{
    static int notes = 0;
    if (!g_crashLogPath[0] || notes >= 12)
        return;
    ++notes;
    FILE* fp = nullptr;
    if (fopen_s(&fp, g_crashLogPath, "ab") != 0 || !fp)
        return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::fprintf(fp, "drag-press %02u:%02u:%02u.%03u at=%d,%d handled=%d wants=%d own=%d move buttons=%u unwound=%d\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, x, y, handled ? 1 : 0, wants ? 1 : 0,
        own ? 1 : 0, static_cast<unsigned>(own ? kButtonsLeftDown : 0), own ? 0 : 1);
    std::fclose(fp);
}

int WebCoreDragAt(int phase, int x, int y, uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!g_session || !g_session->mainFrame) {
        g_dragActive = false;
        return 0;
    }
    if (g_inPump)
        return 0;
    if (phase != 0 && !g_dragActive)
        return 0;   // nothing took the press (or there was none): the gesture is not ours
    g_inPump = true;
    PumpGuard guard;

    // Same reason WebCoreScrollBy/WebCoreWheelAt do this first: drain a decode callback that
    // finished mid-gesture so it is visible before we dispatch, not one tick later.
    RunLoop::cycle();

    RefPtr<LocalFrame> lf = g_session->mainFrame;
    RefPtr<LocalFrameView> view = lf->view();
    if (!view) {
        g_dragActive = false;
        return 0;
    }
    RefPtr<Document> doc = lf->document();
    if (!doc) {
        g_dragActive = false;
        return 0;
    }
    if (phase == 0)
        doc->updateLayoutIgnorePendingStylesheets();   // the press hit-tests; moves reuse that layout

    DoublePoint p(static_cast<double>(x), static_cast<double>(y));
    OptionSet<PlatformEvent::Modifier> mods;
    MonotonicTime t = MonotonicTime::now();
    bool handled = false;

    if (phase == 0) {
        // Apotheosis (2026-09-04): a press left over from a gesture that was abandoned without a
        // release turns this mousedown into a pointermove (the chorded-button rules see the pointer
        // as already pressed), so the page would never see a press at all. Same guard, same reason,
        // as at the top of WebCoreClickAt.
        if (lf->eventHandler().mousePressed())
            releaseDanglingPress(*lf, p, mods);
        // Apotheosis (map site, 2026-09-04): ask the SAME question WebCoreWantsDragAt asked, on
        // the same point and the layout we just updated, BEFORE dispatching - the press itself can
        // run script that changes the tree. See the decision below.
        const bool wants = dragWidgetAtPoint(*doc, x, y);
        // Hover first, exactly as WebCoreClickAt does: it sets elementUnderMouse/:hover, which is
        // what several widgets key their pointerdown handling off.
        DriverMouseEvent hover(p, MouseButton::None, PlatformEvent::Type::MouseMoved, 0, mods, t, 0);
        lf->eventHandler().handleMouseMoveEvent(hover);
        DriverMouseEvent down(p, MouseButton::Left, PlatformEvent::Type::MousePressed, 1, mods, t, kButtonsLeftDown);
        handled = lf->eventHandler().handleMousePressEvent(down).wasHandled();
        // Apotheosis (map site, 2026-09-04): "the press was handled" was the WRONG criterion for
        // owning the gesture. handleMousePressEvent() reports handled only when a listener called
        // preventDefault() or a WebCore default action took the press - and a map does neither: it
        // listens for pointerdown, records the anchor and returns without preventing anything
        // (preventing it would break its own click handling). Device evidence: every single
        // drag-press line on the map site read `handled=0 unwound=1`, so not one mousemove was
        // ever dispatched, while the canvas visibly grew on first contact - the events did arrive,
        // we just threw the gesture away one event in. The honest question is the one
        // WebCoreWantsDragAt already answers ("does this point belong to something that drags
        // itself?"), and the harness only calls us after it has said yes; so own the gesture when
        // EITHER the press was handled OR the point is still a drag widget. Everything else is
        // unchanged: phases 1-3 stay gated on g_dragActive, and a press nothing wanted is still
        // unwound here (the harness drops the queued move/release the moment we answer 0, so no
        // phase 2 would ever arrive to undo it, and a stuck press breaks every later click on the
        // page - see releaseDanglingPress).
        const bool own = handled || wants;
        g_dragActive = own;
        // Apotheosis (map tap, 2026-09-06): remember where the gesture pressed, so the release can
        // tell a pan (no click) from a gesture that never really moved (click, exactly as a tap).
        g_dragPressX = x; g_dragPressY = y; g_dragMoved = false;
        if (!own)
            releaseDanglingPress(*lf, p, mods);
        dragPressNote(x, y, handled, wants, own);
        handled = own;
    } else if (phase == 1) {
        // Button held: EventHandler's m_mousePressed is still set from the press, so this is a drag
        // move, not a hover move. clickCount 0 is what a real platform move carries, and buttons=1
        // is what makes it a drag for the page - a pointermove with buttons=0 is a hover, which is
        // precisely what map widgets ignore (see DriverMouseEvent).
        DriverMouseEvent move(p, MouseButton::Left, PlatformEvent::Type::MouseMoved, 0, mods, t, kButtonsLeftDown);
        lf->eventHandler().handleMouseMoveEvent(move);
        if (!g_dragMoved) {
            const int mdx = x - g_dragPressX, mdy = y - g_dragPressY;
            if (mdx * mdx + mdy * mdy > kDragTapSlopPx * kDragTapSlopPx)
                g_dragMoved = true;   // past the slop: this gesture is a pan, its release fires no click
        }
        handled = true;   // see the header comment: the press decided, per-move results are noise
    } else {
        // 2 = release, 3 = cancel (and any unknown phase): both must end with a mouseup, otherwise
        // EventHandler keeps m_mousePressed set and every later hover/tap behaves like a drag.
        // buttons=0: the button being released is no longer down (WM_LBUTTONUP does the same), and
        // a mouseup that still claimed a pressed button would be turned into a pointermove by the
        // chorded-button rules (PointerCaptureController.cpp:432) - no pointerup, pointer stays
        // pressed for ever. A cancel additionally drops the click: an aborted gesture must not
        // activate what happens to be under the finger.
        //
        // Apotheosis (map tap, 2026-09-06): a gesture that actually travelled must not end in a
        // click either. WebCore fires the DOM 'click' on the release whenever press and release
        // share the same target — on a map that is the one canvas for the whole pan, so every
        // finger pan on the map site ended with a click on the map, which is the site's own
        // "toggle full-screen map view" action. A touch pan never produces a click in a real
        // browser; a gesture that stayed inside the slop is a tap that only reached this route
        // because XAML raised a manipulation, and it keeps its click.
        // Apotheosis (2026-09-07): g_dragMoved is only ever updated in phase 1 (above), which assumed
        // every intermediate move gets dispatched before the release. That is not guaranteed — a
        // coalesced or dropped PumpDrag move, or a gesture whose whole travel happened between two
        // posts, would reach here with g_dragMoved still false and fire a click at the release point
        // regardless of how far the finger actually travelled from the press. Compare the release
        // point to the press point directly so the click decision does not depend on move delivery.
        if (phase == 2 && !g_dragMoved) {
            const int rdx = x - g_dragPressX, rdy = y - g_dragPressY;
            if (rdx * rdx + rdy * rdy > kDragTapSlopPx * kDragTapSlopPx)
                g_dragMoved = true;   // travelled, however the moves were coalesced
        }
        const bool fireClick = (phase == 2 && !g_dragMoved);
        if (!fireClick)
            lf->eventHandler().invalidateClick();
        DriverMouseEvent up(p, MouseButton::Left, PlatformEvent::Type::MouseReleased, fireClick ? 1 : 0, mods, t, 0);
        lf->eventHandler().handleMouseReleaseEvent(up);
        g_dragActive = false;
        handled = true;
        {
            char note[160];
            std::snprintf(note, sizeof note, "drag-release at=%d,%d from=%d,%d phase=%d moved=%d click=%d",
                x, y, g_dragPressX, g_dragPressY, phase, g_dragMoved ? 1 : 0, fireClick ? 1 : 0);
            inputNote(note);
        }
    }

    if (!handled)
        return 0;

    // A mouseup can navigate (a link inside the widget), which rebuilds frame and view — re-fetch
    // before painting, the way WebCoreClickAt does after its pump.
    lf = g_session->page ? g_session->page->localMainFrame() : nullptr;
    if (!lf)
        return 1;
    view = lf->view();
    if (!view)
        return 1;

    g_session->page->isolatedUpdateRendering();   // arms PortChromeClient::m_needsPresent (578cbc3/82c5cef)
    if (outRGBA) {
        int nonWhite = 0;
        paintToRGBA(*view, g_session->w, g_session->h, outRGBA, nonWhite);   // best-effort, as in WebCoreWheelAt
    }
    return 1;
}

// ===========================================================================
// Apotheosis (map-site pin, 2026-09-06): LONG PRESS.
//
// What a long press is on this port: ENABLE_TOUCH_EVENTS is 0 (OptionsWinUWP.cmake) and nothing
// synthesises Touch or raw Pointer input, so every gesture reaches the page as mouse input, which
// the engine then also turns into pointer events (pointerType "mouse"). A touch long press is
// therefore not expressible one-to-one; what IS expressible is everything a widget can key a long
// press off with a mouse:
//   * mousedown / pointerdown, then the button STAYS DOWN while the run loop turns, so a
//     press-and-hold timer inside the page (a map site drops its pin from exactly such a timer)
//     gets the time it is waiting for. This is the part no existing export could do: WebCoreClickAt
//     releases in the same call, and WebCoreDragAt's press is only held between harness calls
//     during which the engine dispatches nothing.
//   * the release afterwards, with the DOM 'click' (Maps' mobile UI reacts to a click on the map)
//     unless the caller suppresses it.
//   * a 'contextmenu' event at the same point (flag 1) — on desktop Maps the right-click menu is
//     the usable "drop a pin / What's here?" path, and a touch long press is exactly what a browser
//     turns into a contextmenu. Sent through EventHandler::sendContextMenuEvent(), i.e. the same
//     entry the Windows port uses for WM_CONTEXTMENU: it hit-tests, clears the press state and
//     dispatches the DOM event. ENABLE_CONTEXT_MENUS is 0 here, so there is no UA menu to show and
//     nothing but the page's own listener can react — which is what we want.
//
// flags (WEBCORE_LONGPRESS_* in WebCoreDriver.h): 1 = also send contextmenu, 2 = suppress the
// click, 4 = do nothing unless the point is a drag widget (canvas / touch-action:none — the same
// walk WebCoreWantsDragAt runs). 4 is what the harness passes: a long press anywhere else on the
// page must keep behaving the way it does today (XAML raises Holding over ordinary text too, and a
// press-hold-click on a link would open it).
//
// holdMs: how long the button stays down (default 600, capped at 2000). (x,y) = viewport/bitmap px,
// same convention as every other input export. Returns kOK once the gesture was delivered — and
// also on the "not a drag widget" skip, with the current frame painted into outRGBA, so the caller
// never has to tell a skip from a failure to keep its screen correct. Engine thread only.
int WebCoreLongPressAt(int x, int y, int holdMs, int flags, uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!g_session || !g_session->mainFrame)
        return kErrNoSession;
    if (g_inPump)
        return kErrBusy;
    g_inPump = true;
    PumpGuard guard;
    PerfOpGuard perfOp("longpress", nullptr, 0, 0);

    RefPtr<LocalFrame> lf = g_session->mainFrame;
    RefPtr<LocalFrameView> view = lf->view();
    if (!view)
        return kErrNoView;
    RefPtr<Document> doc = lf->document();
    if (!doc)
        return kErrNoDocument;
    doc->updateLayoutIgnorePendingStylesheets();   // the press hit-tests, as in WebCoreClickAt

    const bool wants = dragWidgetAtPoint(*doc, x, y);
    if ((flags & 4) && !wants) {
        char note[128];
        std::snprintf(note, sizeof note, "long-press skip at=%d,%d wants=0", x, y);
        inputNote(note);
        if (outRGBA) {
            int nonWhite = 0;
            paintToRGBA(*view, g_session->w, g_session->h, outRGBA, nonWhite);   // keep the caller's frame valid
        }
        return kOK;
    }

    DoublePoint p(static_cast<double>(x), static_cast<double>(y));
    OptionSet<PlatformEvent::Modifier> mods;
    MonotonicTime t = MonotonicTime::now();

    // A press left down by an abandoned gesture would turn this mousedown into a pointermove — same
    // guard, same reason, as at the top of WebCoreClickAt/WebCoreDragAt.
    const bool unwound = lf->eventHandler().mousePressed();
    if (unwound) {
        releaseDanglingPress(*lf, p, mods);
        // That release dispatches a mouseup, which runs script and in the worst case navigates —
        // re-fetch before pressing, exactly as WebCoreClickAt does.
        lf = g_session->page ? g_session->page->localMainFrame() : nullptr;
        if (!lf)
            return kErrFrameGone;
        g_session->mainFrame = lf;
        view = lf->view();
        if (!view)
            return kErrNoView;
    }
    g_dragActive = false;   // this call owns the press from here to its release

    DriverMouseEvent hover(p, MouseButton::None, PlatformEvent::Type::MouseMoved, 0, mods, t, 0);
    lf->eventHandler().handleMouseMoveEvent(hover);   // :hover / elementUnderMouse, as in WebCoreClickAt
    DriverMouseEvent down(p, MouseButton::Left, PlatformEvent::Type::MousePressed, 1, mods, t, kButtonsLeftDown);
    const bool downHandled = lf->eventHandler().handleMousePressEvent(down).wasHandled();

    int hold = holdMs <= 0 ? 600 : holdMs;
    if (hold > 2000)
        hold = 2000;
    holdPump(*lf, g_session->page.get(), hold / 1000.0);

    // The hold ran script (timers, rAF) and can have navigated — re-fetch before the release, the
    // way WebCoreClickAt does after its pump.
    lf = g_session->page ? g_session->page->localMainFrame() : nullptr;
    if (!lf) {
        teardownSession();
        return kErrFrameGone;
    }
    g_session->mainFrame = lf;
    view = lf->view();
    if (!view)
        return kErrNoView;

    const bool fireClick = !(flags & 2);
    if (!fireClick)
        lf->eventHandler().invalidateClick();
    DriverMouseEvent up(p, MouseButton::Left, PlatformEvent::Type::MouseReleased, fireClick ? 1 : 0,
                        mods, MonotonicTime::now(), 0);
    const bool upHandled = lf->eventHandler().handleMouseReleaseEvent(up).wasHandled();

    bool ctxSwallowed = false;
    bool ctxSent = false;
#if ENABLE(CONTEXT_MENU_EVENT)
    if (flags & 1) {
        // The release can navigate too (a link under the finger): re-fetch once more before asking
        // for a context menu on a frame that may be gone.
        lf = g_session->page ? g_session->page->localMainFrame() : nullptr;
        if (lf) {
            g_session->mainFrame = lf;
            DriverMouseEvent ctx(p, MouseButton::Right, PlatformEvent::Type::MousePressed, 1, mods,
                                 MonotonicTime::now(), kButtonsRightDown);
            ctxSwallowed = lf->eventHandler().sendContextMenuEvent(ctx);
            ctxSent = true;
        }
    }
#endif
    {
        char note[192];
        std::snprintf(note, sizeof note,
            "long-press at=%d,%d wants=%d hold=%d down=%d up=%d click=%d ctx=%d/%d unwound=%d",
            x, y, wants ? 1 : 0, hold, downHandled ? 1 : 0, upHandled ? 1 : 0, fireClick ? 1 : 0,
            ctxSent ? 1 : 0, ctxSwallowed ? 1 : 0, unwound ? 1 : 0);
        inputNote(note);
    }

    // Let the synchronous handlers land (pumpQuick, the same light settle typing uses — the pin's
    // own network work streams into the live tick), then present.
    lf = g_session->page ? g_session->page->localMainFrame() : nullptr;
    if (!lf) {
        teardownSession();
        return kErrFrameGone;
    }
    g_session->mainFrame = lf;
    view = lf->view();
    if (!view)
        return kErrNoView;
    pumpQuick(*lf, g_session->page.get());
    lf = g_session->page ? g_session->page->localMainFrame() : nullptr;
    if (!lf) {
        teardownSession();
        return kErrFrameGone;
    }
    g_session->mainFrame = lf;
    view = lf->view();
    if (!view)
        return kErrNoView;
    if (RefPtr<Document> d2 = lf->document()) {
        d2->updateLayoutIgnorePendingStylesheets();
        extractLinks(d2.get(), g_session->h);
    }
    if (outRGBA) {
        int nonWhite = 0;
        paintToRGBA(*view, g_session->w, g_session->h, outRGBA, nonWhite);
    }
    return kOK;
}

// 滚动停止后刷新链接命中表(滚动期间为提速跳过了 extractLinks)。轻量:仅布局 + 提取,不绘制、不派发事件。
// 点击路径用引擎实时命中测试(权威),链接表只作兜底/主页用,故滚动中暂时陈旧无碍,停手时这里补齐。
int WebCoreSyncLinks()
{
    using namespace WebCore;
    if (!g_session || !g_session->mainFrame)
        return kErrNoSession;
    if (g_inPump)
        return kErrBusy;
    RefPtr<LocalFrame> lf = g_session->mainFrame;
    RefPtr<LocalFrameView> view = lf->view();
    if (!view)
        return kErrNoView;
    RefPtr<Document> doc = lf->document();
    if (!doc)
        return kErrNoDocument;
    doc->updateLayoutIgnorePendingStylesheets();
    extractLinks(doc.get(), g_session->h);
    return kOK;
}

// 页内查找:标记并高亮全部匹配 + 选中(从当前选区起)第一个,滚动到它,重绘。返回匹配数(>=0)或负错误码。
//   matchCase!=0 区分大小写;wrap!=0 到底回绕。空串=清除高亮(等价 WebCoreFindClear)。
int WebCoreFindString(const char* utf8, int matchCase, int wrap, uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!utf8 || !outRGBA)
        return kErrBadArgs;
    if (!g_session || !g_session->page || !g_session->mainFrame)
        return kErrNoSession;
    if (g_inPump)
        return kErrBusy;
    g_inPump = true;
    PumpGuard guard;

    String text = String::fromUTF8(utf8);
    OptionSet<FindOption> opts;
    if (!matchCase) opts.add(FindOption::CaseInsensitive);
    if (wrap)       opts.add(FindOption::WrapAround);
    g_findText = text;
    g_findOpts = opts;

    if (text.isEmpty()) {
        g_session->page->unmarkAllTextMatches();
        int prc = finishInteractionPaint(outRGBA);
        return prc == kOK ? 0 : prc;
    }
    unsigned count = g_session->page->markAllMatchesForText(text, opts, /*shouldHighlight*/ true, /*max*/ 1000);
    auto data = g_session->page->findString(text, opts);
    if (data.range)
        g_session->page->revealCurrentSelection();
    int prc = finishInteractionPaint(outRGBA);
    if (prc != kOK)
        return prc;
    return static_cast<int>(count);
}

// 查找下一个/上一个(沿用上次查找词+选项,不重新标记)。forward!=0 向下。返回 1=命中 / 0=无 / 负=错误。
int WebCoreFindNext(int forward, uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!outRGBA)
        return kErrBadArgs;
    if (!g_session || !g_session->page)
        return kErrNoSession;
    if (g_inPump)
        return kErrBusy;
    if (g_findText.isEmpty())
        return 0;
    g_inPump = true;
    PumpGuard guard;

    OptionSet<FindOption> opts = g_findOpts;
    if (!forward) opts.add(FindOption::Backwards);
    auto data = g_session->page->findString(g_findText, opts);
    if (data.range)
        g_session->page->revealCurrentSelection();
    int prc = finishInteractionPaint(outRGBA);
    if (prc != kOK)
        return prc;
    return data.range ? 1 : 0;
}

// 清除查找高亮/选区,重绘。返回 0 成功。
int WebCoreFindClear(uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!outRGBA)
        return kErrBadArgs;
    if (!g_session || !g_session->page)
        return kErrNoSession;
    if (g_inPump)
        return kErrBusy;
    g_inPump = true;
    PumpGuard guard;

    g_session->page->unmarkAllTextMatches();
    g_findText = WTF::String();
    int prc = finishInteractionPaint(outRGBA);
    return prc == kOK ? 0 : prc;
}

// M4 捏合缩放:把页面缩放因子设为 scale(钳到 [0.5,6.0]),以屏幕焦点 (focalX,focalY) 为锚 —— 缩放后让焦点
//   下的内容点仍停在焦点处(据此算新滚动原点)。setPageScaleFactor 触发按新尺度重栅格(TextureMapper backing 的
//   contentsScale = pageScaleFactor*deviceScale → 文字清晰)。重绘到 outRGBA。引擎线程串行调。返回 0。
//   focalX/focalY are ENGINE VIEWPORT PIXELS (0..w, 0..h) — the same space the harness paints
//   into, not CSS/document coordinates. See the anchor derivation below for the scroll units.
int WebCoreSetPageScale(float scale, int focalX, int focalY, uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!outRGBA)
        return kErrBadArgs;
    if (!g_session || !g_session->page || !g_session->mainFrame)
        return kErrNoSession;
    if (g_inPump)
        return kErrBusy;
    g_inPump = true;
    PumpGuard guard;
    PerfOpGuard perfOp("scale", nullptr, 0, 0);   // Apotheosis (M4): pinch/double-tap zoom gets its own perf.csv row

    RefPtr<LocalFrame> lf = g_session->mainFrame;
    RefPtr<LocalFrameView> view = lf->view();
    if (!view)
        return kErrNoView;
    RefPtr<Document> doc = lf->document();
    if (!doc)
        return kErrNoDocument;
    doc->updateLayoutIgnorePendingStylesheets();

    if (scale < 0.5f) scale = 0.5f;
    if (scale > 6.0f) scale = 6.0f;

    float oldScale = g_session->page->pageScaleFactor();
    if (oldScale <= 0.0f) oldScale = 1.0f;
    ScrollPosition scroll = view->scrollPosition();
    // Apotheosis (M4): pinch anchor. Page::setPageScaleFactor(scale, origin) hands `origin`
    // straight to LocalFrameView::setScrollPosition() — it is the NEW SCROLL POSITION, not a
    // focal point. Its unit is the frame view's own scroll space, and that space is SCALED:
    //   * ScrollView::contentsSize() comes from LocalFrameView::adjustViewSize() ->
    //     RenderView::documentRect(), which maps unscaledDocumentRect() through the RenderView
    //     layer transform (i.e. multiplies by the page scale) — see RenderView.cpp:779.
    //   * LocalFrameView::getPossiblyFixedRectToExpose() says it outright ("exposeRect is in
    //     absolute coords, affected by page scale") and scales its result by frameScaleFactor()
    //     before returning it as a scroll position — LocalFrameView.cpp:7141/7159.
    // So one viewport pixel is one scroll unit at every scale, and the document (CSS) point
    // under a viewport-pixel focal F is doc = (P0 + F) / s0. Keeping it under the finger:
    //
    //     P1 = doc * s1 - F = (P0 + F) * (s1 / s0) - F
    //
    // Worked example s0 = 1, s1 = 2, P0 = (0, 1000), F = (360, 540):
    //     doc = (360, 1540)  ->  P1 = (720, 3080) - (360, 540) = (360, 2540).
    // The old code treated P0 as CSS units (doc = P0 + F/s0, P1 = doc - F/s1) and produced
    // (180, 1270) here — roughly half the intended offset, i.e. the page jumped a long way
    // towards the top on release, exactly the reported symptom. The error grows with P0, so
    // it looked like "it snaps to the top-of-page view" far down a long page.
    double ratio = static_cast<double>(scale) / static_cast<double>(oldScale);
    double nx = (static_cast<double>(scroll.x()) + static_cast<double>(focalX)) * ratio - static_cast<double>(focalX);
    double ny = (static_cast<double>(scroll.y()) + static_cast<double>(focalY)) * ratio - static_cast<double>(focalY);
    int nsx = static_cast<int>(nx < 0 ? nx - 0.5 : nx + 0.5);
    int nsy = static_cast<int>(ny < 0 ? ny - 0.5 : ny + 0.5);
    IntPoint wanted = view->constrainedScrollPosition(IntPoint(nsx, nsy));

    g_session->page->setPageScaleFactor(scale, wanted);
    g_session->page->isolatedUpdateRendering();
    // Apotheosis: re-apply the anchor AFTER the relayout at the new scale. setPageScaleFactor
    // clamps `origin` inside setScrollPosition against the contents size of that moment, and
    // when zooming in that is still the OLD (smaller) scaled document — the clamp then eats
    // most of the new offset near the bottom of a page. Constraining again once adjustViewSize()
    // has published the new scaled contents size gives the correct final position.
    doc->updateLayoutIgnorePendingStylesheets({ WebCore::LayoutOptions::UpdateCompositingLayers });
    IntPoint settled = view->constrainedScrollPosition(IntPoint(nsx, nsy));
    if (view->scrollPosition() != settled)
        view->setScrollPosition(settled);
    doc->updateLayoutIgnorePendingStylesheets();

    int nonWhite = 0;
    // 不置 g_gpuScrollFast:缩放改变尺度,需全树重绘按新 contentsScale 重栅格(否则文字模糊)。
    int prc = paintToRGBA(*view, g_session->w, g_session->h, outRGBA, nonWhite);
    if (prc != kOK)
        return prc;
    writeDiag(*doc, *view, g_session->w, g_session->h, nonWhite);
    return kOK;
}

// M4:取当前页面缩放因子 ×1000 的整数(1000=1.0x,2500=2.5x),供 harness 跟踪缩放状态。
int WebCoreGetPageScale()
{
    if (!g_session || !g_session->page)
        return 1000;
    float s = g_session->page->pageScaleFactor();
    if (s <= 0.0f) s = 1.0f;
    return static_cast<int>(s * 1000.0f + 0.5f);
}

// Apotheosis (double-tap zoom, 2026-09-09): read-only tap policy for double-tap-to-zoom — see the
// full contract in WebCoreDriver.h. Hit-tests (x,y) and answers whether a second tap here should
// zoom the page (mobile-Safari/Chrome semantics) rather than being forwarded as an ordinary second
// click, and if so, to what scale. No event dispatched, no session/document state changed — safe to
// call from the harness' tap-hold path before it has decided whether to click at all.
int WebCoreTapPolicyAt(int x, int y, int* outZoomable, float* outTargetScale, int* outAnchorX, int* outAnchorY)
{
    using namespace WebCore;
    if (outZoomable) *outZoomable = 0;
    if (outTargetScale) *outTargetScale = 1.0f;
    if (outAnchorX) *outAnchorX = x;
    if (outAnchorY) *outAnchorY = y;
    if (!g_session || !g_session->mainFrame)
        return kErrNoSession;
    if (g_inPump)
        return kErrBusy;
    g_inPump = true;
    PumpGuard guard;   // 任何返回路径复位 g_inPump

    RefPtr<LocalFrame> lf = g_session->mainFrame;
    RefPtr<Document> doc = lf->document();
    if (!doc)
        return kErrNoDocument;
    doc->updateLayoutIgnorePendingStylesheets();   // hit test needs current layout, as in WebCoreClickAt

    RefPtr<Element> hit = doc->elementFromPoint(static_cast<double>(x), static_cast<double>(y));
    if (!hit)
        return kOK;   // nothing under the point: leave *outZoomable at 0, not an error

    // Apotheosis: document-level opt-out first (cheap, one struct already on Document) — a page
    // whose viewport meta disables zoom is asking every zoom gesture, not just pinch, to leave it
    // alone. userZoom is a tri-state float (ValueAuto=-1 unset, else the parsed boolean as 0.0/1.0 —
    // see ViewportArguments.h / dom/ViewportArguments.cpp findBooleanValue()); minZoom==maxZoom only
    // counts when BOTH were actually set (ValueAuto==ValueAuto would otherwise always match).
    ViewportArguments va = doc->viewportArguments();
    const bool viewportDisablesZoom = (va.userZoom == 0.0f)
        || (va.minZoom != ViewportArguments::ValueAuto && va.maxZoom != ViewportArguments::ValueAuto
            && va.minZoom == va.maxZoom);
    if (viewportDisablesZoom)
        return kOK;

    // Apotheosis: same open-shadow descent as dragWidgetAtPoint()/WebCoreIsScrollableAt — a point
    // inside a web component's shadow tree would otherwise only ever see the host element.
    for (int depth = 0; depth < 16; ++depth) {
        RefPtr<ShadowRoot> shadow = hit->openOrClosedShadowRoot();
        if (!shadow || shadow->mode() != ShadowRootMode::Open)
            break;
        RefPtr<Element> inner = shadow->elementFromPoint(static_cast<double>(x), static_cast<double>(y));
        if (!inner || inner == hit)
            break;
        hit = WTF::move(inner);
    }

    // Apotheosis: element-level opt-out. Mirrors WebKit's own Element::allowsDoubleTapGesture()
    // (dom/Element.cpp) — compiled out on this port under !ENABLE_TOUCH_EVENTS, so re-derived here —
    // which disallows the gesture when ANY ancestor (hit element included) has a touch-action other
    // than auto, not just none/manipulation: pan-x/pan-y are as much an opt-out for double-tap-zoom
    // as they are there, because a page that has claimed one axis for its own panning has claimed
    // the gesture, not just a rectangle (see the same reasoning in dragWidgetAtPoint's comment).
    Element* root = doc->documentElement();
    for (RefPtr<Element> e = hit; e; e = e->parentElement()) {
        if (RenderObject* r = e->renderer()) {
            if (!r->style().touchAction().isAuto())
                return kOK;   // opted out; *outZoomable stays 0
        }
        if (e.get() == root)
            break;
    }

    if (outZoomable) *outZoomable = 1;

    RefPtr<Page> page = g_session->page;
    float curScale = page ? page->pageScaleFactor() : 1.0f;
    if (!(curScale > 0.0f)) curScale = 1.0f;
    if (curScale > 1.05f) {
        if (outTargetScale) *outTargetScale = 1.0f;   // already zoomed: a double tap always returns to 1:1
        return kOK;
    }

    // Apotheosis: "zoom to column" — the innermost block-level ancestor of the hit point that is
    // narrower than the layout viewport (Safari's own double-tap-zoom heuristic), walked from the
    // hit element up towards <html> so the FIRST candidate found is the innermost/smallest one.
    // boundingClientRect() and (x,y) are read in the same viewport/bitmap px space this driver
    // already uses them in elsewhere with no extra scale conversion (extractLinks, dragWidgetAtPoint)
    // — exact at scale 1.0 and close enough up to the 1.05 threshold just checked above; a bigger
    // current scale would need an extra factor of curScale (see WebCoreSetPageScale's own derivation
    // of engine-px-vs-CSS-px), which this branch never runs at.
    const float viewportW = static_cast<float>(g_session->w);
    const float kZoomPadding = 8.0f;   // small breathing room so the column edge is not flush with the screen edge
    float targetScale = 2.0f;          // Safari's fallback when no narrower block ancestor exists
    for (RefPtr<Element> e = hit; e; e = e->parentElement()) {
        if (RenderObject* r = e->renderer()) {
            if (r->isRenderBlock()) {
                float w = e->boundingClientRect().width();
                if (w > 0.0f && w < viewportW) {
                    targetScale = viewportW / (w + kZoomPadding);
                    break;
                }
            }
        }
        if (e.get() == root)
            break;
    }
    if (targetScale < 1.0f) targetScale = 1.0f;
    if (targetScale > 3.0f) targetScale = 3.0f;
    if (outTargetScale) *outTargetScale = targetScale;
    return kOK;
}

// 当前会话是否有可编辑元素聚焦(输入框/textarea/contenteditable)→ harness 据此弹/收输入法。
// UA 切换:mobile=1 移动 iPhone UA(默认),0 桌面 Windows UA。切后由 UI 重新加载页面生效。
void WebCoreSetUserAgentMobile(int mobile)
{
    g_apoUaMobile = (mobile != 0);
}

// 自定义 UA:非空则 userAgent() 直接返回它(覆盖 mobile/desktop);空串=清除回退开关。切后 UI 重载生效。
void WebCoreSetUserAgentString(const char* ua)
{
    if (!ua || !*ua) { g_apoCustomUA[0] = '\0'; return; }
    size_t n = std::strlen(ua);
    if (n >= sizeof(g_apoCustomUA)) n = sizeof(g_apoCustomUA) - 1;
    std::memcpy(g_apoCustomUA, ua, n);
    g_apoCustomUA[n] = '\0';
}

// M1 验证:GPU 合成是否在跑。PortChromeClient 的 attachRootGraphicsLayer 被调=合成激活+图层树已建;
// 根图层非空即证。加载后查(图层树在布局/合成更新时建)。返回 1=合成在跑,0=未。
// Apotheosis (PRIVACY-AUDIT.md recommended action 4): switch speculation-rules prefetch on/off.
// enabled!=0 -> a page's <script type="speculationrules"> may prefetch URLs the user has not clicked.
// Engine thread only (touches the live Page). Sticky: applies to the current session and to every
// session created afterwards, so the harness sets it at startup and on every network-cost change.
void WebCoreSetSpeculativePrefetch(int enabled)
{
    g_apoSpecPrefetch = (enabled != 0);
    if (g_session && g_session->page)
        g_session->page->settings().setSpeculationRulesPrefetchEnabled(g_apoSpecPrefetch);
}

// Apotheosis (M4): warm up an origin the user is about to visit - the harness calls this while
// a URL is being typed, so the name is resolved before Enter.
//
// DNS is all a "preconnect" can be in this port, and that is a deliberate finding, not a stub:
// libcurl has no preconnect primitive, and its closest relative CURLOPT_CONNECT_ONLY (including
// =2, which does complete the TLS handshake) takes the connection *out* of the pool and binds it
// to the one easy handle that opened it, so a warm-up would burn a TCP+TLS connection the real
// request can never reuse. What CurlContext's CURLSH handle does share across handles is DNS,
// TLS sessions and cookies (CURL_LOCK_DATA_DNS / SSL_SESSION / COOKIE with WTF::Lock callbacks,
// CurlContext.cpp; every CurlHandle opts in via enableShareHandle()), which means the second
// request to a host reuses its TLS session anyway - the resolver is the part still worth
// warming. Same conclusion as WebResourceLoadScheduler::preconnectTo() for <link rel=preconnect>.
//
// Accepts a full URL or a bare host ("example.com"); anything else is ignored. Idempotent and cheap
// (apotheosisPrefetchDNS resolves on a work queue and keeps a capped set of hosts it has already
// done), and it never touches the live Page.
void WebCorePreconnect(const char* url)
{
    using namespace WebCore;
    if (!url || !*url)
        return;
    String input = String::fromUTF8(url);
    if (input.isEmpty())
        return;
    URL parsed { input };
    if (!parsed.isValid() || parsed.host().isEmpty())
        parsed = URL { makeString("https://"_s, input) };   // still being typed: no scheme yet
    if (!parsed.isValid() || !parsed.protocolIsInHTTPFamily())
        return;
    String host = parsed.host().toString();
    if (host.isEmpty())
        return;
    ::apotheosisPrefetchDNS(host);
}

int WebCoreEnableCompositing()
{
    if (!g_session || !g_session->chrome)
        return 0;
    return g_session->chrome->rootLayer() != nullptr ? 1 : 0;
}

// M2:初始化 GPU 合成呈现。引擎线程调一次。
//   nativeWindow = ANGLE 原生窗口(SwapChainPanel 的 PropertySet 的 IInspectable*,harness 端构造)→ 直呈现窗口表面;
//   nullptr → 离屏(surfaceless/pbuffer),仅 readback,用于先验证合成正确(本版默认走这条)。
//   w/h = 呈现像素尺寸。成功后置 g_gpuActive=true(此后 buildSession 才开合成、建 GraphicsLayerTextureMapper 树)。
// 返回 0 成功;-1 bad args;-20 建 GLContext 失败;-21 makeCurrent 失败;-22 建 TextureMapper 失败。
int WebCoreGpuInit(void* nativeWindow, int w, int h)
{
    using namespace WebCore;
    if (!isValidSurfaceSize(w, h))
        return kErrBadArgs;
    if (g_gpuActive)
        return kOK;   // 幂等
    ensureWebCoreInitialized();
    // Apotheosis (package 5): tile rasterisation always runs on the engine's worker pool, so the
    // completion hook is installed once here rather than by a runtime switch. It is called ON A
    // WORKER THREAD (see rasterCompletedOnWorker) and must exist before the first composite can
    // post a replay; WebCoreGpuInit is idempotent and is the only way a tile ever gets created.
    wkWinUWPSetRasterCompletionHandler(&rasterCompletedOnWorker);
    PlatformDisplay& display = PlatformDisplay::sharedDisplay();   // WIN → PlatformDisplayWin,起 ANGLE EGLDisplay

    std::unique_ptr<GLContext> ctx = nativeWindow
        ? GLContext::create(display, reinterpret_cast<GLNativeWindowType>(nativeWindow))   // 窗口表面:指针经纯 C cast 直传 eglCreateWindowSurface
        : GLContext::createOffscreen(display);                                              // 离屏:surfaceless→pbuffer
    if (!ctx)
        return -20;
    if (!ctx->makeContextCurrent())
        return -21;
    std::unique_ptr<TextureMapper> tm = TextureMapper::create();   // 需 GLContext::current() 非空(刚 makeCurrent 满足)
    if (!tm)
        return -22;
    g_glContext = ctx.release();        // 故意泄漏=随进程存活(避免退出时在错误线程 eglDestroyContext)
    g_textureMapper = tm.release();
    g_gpuW = w;
    g_gpuH = h;
    g_gpuPresentMode = (nativeWindow != nullptr);   // 有窗口表面 → 直呈现;否则离屏 readback
    g_gpuActive = true;
    return kOK;
}

// M2:把当前会话图层树直呈现到 GpuInit 绑定的窗口表面(eglSwapBuffers)。引擎线程调。
//   仅在 WebCoreGpuInit(nativeWindow!=null) 后有意义(离屏模式无窗口表面,swapBuffers 为 no-op)。返回 0 成功。
int WebCoreComposite()
{
    using namespace WebCore;
    if (!g_gpuActive || !g_session || !g_session->page || !g_session->chrome)
        return kErrNoSession;
    RefPtr<LocalFrame> lf = g_session->page->localMainFrame();
    RefPtr<LocalFrameView> view = lf ? lf->view() : nullptr;
    GraphicsLayer* root = g_session->chrome->rootLayer();
    if (!view || !root)
        return kErrNoView;
    return gpuPresent(*view, g_gpuW, g_gpuH, *root);
}

// M2(离屏验证):把当前会话图层树经 TextureMapper 合成到离屏纹理,readback 出 RGBA 到 outRGBA(>= w*h*4)。
//   用现有 WriteableBitmap 通道显示,先证合成像素正确。返回 0 成功。
//   注:本版会话各绘制点已在 paintToRGBA 顶部自动走此路(GPU 起后),此导出供需要显式呈现时用。
int WebCoreCompositeReadback(uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!g_gpuActive || !g_session || !g_session->page || !g_session->chrome)
        return kErrNoSession;
    if (!outRGBA)
        return kErrBadArgs;
    RefPtr<LocalFrame> lf = g_session->page->localMainFrame();
    RefPtr<LocalFrameView> view = lf ? lf->view() : nullptr;
    GraphicsLayer* root = g_session->chrome->rootLayer();
    if (!view || !root)
        return kErrNoView;
    int nonWhite = 0;
    return gpuCompositeReadback(*view, g_gpuW, g_gpuH, *root, outRGBA, nonWhite);
}

// M2 调试:运行时设离屏 readback 的翻转(找正确朝向用)。flipH/flipV 非0=反转列/行。
void WebCoreGpuSetFlip(int flipH, int flipV)
{
    g_gpuFlipH = (flipH != 0);
    g_gpuFlipV = (flipV != 0);
}

// M2 调试:把当前会话的 FrameView 滚动/内容尺寸 + 合成图层树文本写入 out(供定位背景丢失/滚动失效)。
// 首行=关键标量(scrollPos/contents/view/docBg有效/usesCompositing),其后是 GraphicsLayer::layerTreeAsText()。
int WebCoreGpuLayerInfo(char* out, int len)
{
    using namespace WebCore;
    if (!out || len <= 0)
        return kErrBadArgs;
    out[0] = 0;
    if (!g_session || !g_session->page || !g_session->chrome)
        return kErrNoSession;
    std::string s;
    RefPtr<LocalFrame> lf = g_session->page->localMainFrame();
    RefPtr<LocalFrameView> view = lf ? lf->view() : nullptr;
    if (view) {
        IntPoint sp = view->scrollPosition();
        IntPoint minP = view->minimumScrollPosition();
        IntPoint maxP = view->maximumScrollPosition();
        IntSize cs = view->contentsSize();
        Color bg = view->documentBackgroundColor();
        auto [r, g, b, a] = (bg.isValid() ? bg : Color::white).toColorTypeLossy<SRGBA<float>>().resolved();
        bool usesComp = view->renderView() && view->renderView()->usesCompositing();
        char h[512];
        snprintf(h, sizeof h,
                 "docBg=#%02X%02X%02X%02X valid=%d lastContentPx=%d\n"
                 "scrollPos=%d,%d min=%d,%d max=%d,%d contents=%dx%d view=%dx%d usesCompositing=%d\n",
                 (int)(r * 255 + 0.5f), (int)(g * 255 + 0.5f), (int)(b * 255 + 0.5f), (int)(a * 255 + 0.5f),
                 bg.isValid() ? 1 : 0, g_lastContentPx,
                 sp.x(), sp.y(), minP.x(), minP.y(), maxP.x(), maxP.y(),
                 cs.width(), cs.height(), g_session->w, g_session->h, usesComp ? 1 : 0);
        s += h;
        // Apotheosis (2026-09-07): the scroll probe this used to do here - setScrollPosition(0,300)
        // + frameViewDidScroll, to see whether the scrolled-contents layer really moves - is gone.
        // It was bring-up code for "the page does not scroll" and it is actively harmful now: the
        // dump is taken *while* a rendering artefact is on screen, and scrolling the view moves
        // every visible rect, re-tiles the backing stores and repaints the tree, so the state the
        // dump is supposed to describe is destroyed before it is written. It also left the layer
        // tree below describing scroll position 300 rather than the page the user is looking at.
    }
    // Apotheosis (2026-09-07): the texmap diagnostics before the layer tree, and the tree capped to
    // whatever is left. Device (0.1.9.25, build-driver\logs\20260907-140444\layertree.txt): the file
    // was exactly 65535 bytes of layerTreeAsText() and did not contain a single texmap line - the
    // tree of a real page fills any buffer, so everything appended after it is lost. The per-store
    // block is the smaller and, for a ghost/stale-tile question, the only useful half.
    {
        std::vector<char> texmap(64 * 1024, 0);
        const size_t used = WebCore::wkWinUWPDumpTexmap(texmap.data(), texmap.size());
        s.append(texmap.data(), used);
    }
    if (GraphicsLayer* root = g_session->chrome->rootLayer()) {
        String tree = root->layerTreeAsText(AllLayerTreeAsTextOptions);   // 全调试标志:paintsIntoWindow/tileCache/drawsContent/backingStoreAttached
        CString u = tree.utf8();
        // Truncated here rather than by the memcpy below, so that "the tree was cut off" is visible
        // in the file instead of looking like a dump that simply ends.
        const size_t room = (s.size() + 1 < static_cast<size_t>(len)) ? static_cast<size_t>(len) - 1 - s.size() : 0;
        if (u.length() <= room)
            s.append(u.data(), u.length());
        else if (room > 64) {
            s.append(u.data(), room - 64);
            s += "\n-- layer tree truncated --\n";
        }
    } else {
        s += "(no root GraphicsLayer)\n";
    }
    int n = static_cast<int>(s.size());
    if (n > len - 1) n = len - 1;
    memcpy(out, s.data(), static_cast<size_t>(n));
    out[n] = 0;
    return kOK;
}

int WebCoreFocusedEditable()
{
    using namespace WebCore;
    if (!g_session || !g_session->mainFrame || g_inPump)
        return 0;
    RefPtr<LocalFrame> lf = g_session->mainFrame;
    // 优先按聚焦元素类型判定(确定性):text input / textarea / contenteditable → 可编辑(弹键盘)。
    //   canEdit() 在 headless 下时有假阴,故只作兜底。
    if (RefPtr<Document> doc = lf->document()) {
        if (RefPtr<Element> fe = doc->focusedElement()) {
            if (is<HTMLInputElement>(*fe))
                return downcast<HTMLInputElement>(*fe).isTextField() ? 1 : 0;
            if (is<HTMLTextAreaElement>(*fe))
                return 1;
            if (is<HTMLElement>(*fe) && downcast<HTMLElement>(*fe).isContentEditable())
                return 1;
        }
    }
    return lf->editor().canEdit() ? 1 : 0;
}

// 向聚焦的可编辑元素插入文本,pump 让 JS 反应,重绘。返回 0 成功。
int WebCoreTypeText(const char* utf8, uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!utf8 || !outRGBA)
        return kErrBadArgs;
    if (!g_session || !g_session->mainFrame)
        return kErrNoSession;
    if (g_inPump)
        return kErrBusy;
    g_inPump = true;
    PumpGuard guard;
    RefPtr<LocalFrame> lf = g_session->mainFrame;
    RefPtr<Document> doc = lf->document();
    String text = String::fromUTF8(utf8);

    // ★ "字进了 DOM 却 value 恒空"根因(真机 imedebug 实证:fe=input canEditAfter=1 inserted=1 但 valueLen 恒=0):
    //   App Container/headless 下 editor().canEdit() 虽真,FrameSelection 却不在该 input 的内嵌编辑器里 →
    //   insertTextWithoutSendingTextEvent 把字插到别处,目标框 value 一直空、屏幕也没变化。
    //   修复:聚焦元素是文本 input / textarea 时,绕开 editor 与选区,直接改元素 value(确定性,经 input 内建
    //   净化/maxlength)+ 派发 input 事件(让搜索建议/受控组件响应;不发 change,免每键误触发表单提交)。
    //   仅 contenteditable / 未知可编辑元素才退回 editor / 合成键路径(原行为)。
    int canEditBefore = lf->editor().canEdit() ? 1 : 0;
    const char* feTag = "none";
    const char* pathTag = "none";
    int inserted = 0;
    RefPtr<Element> fe = doc ? doc->focusedElement() : nullptr;
    if (fe && is<HTMLInputElement>(*fe) && downcast<HTMLInputElement>(*fe).isTextField()) {
        feTag = "input"; pathTag = "direct";
        auto& input = downcast<HTMLInputElement>(*fe);
        String cur = input.value();                                   // ValueOrReference<String> → const String&
        (void)input.setValue(makeString(cur, text), DispatchNoEvent); // 末尾追加(默认 SetSelectionToEnd 置光标)
        input.dispatchInputEvent();
        inserted = 1;
    } else if (fe && is<HTMLTextAreaElement>(*fe)) {
        feTag = "textarea"; pathTag = "direct";
        auto& ta = downcast<HTMLTextAreaElement>(*fe);
        String cur = ta.value();
        (void)ta.setValue(makeString(cur, text), DispatchNoEvent);
        ta.dispatchInputEvent();
        inserted = 1;
    } else {
        feTag = fe ? "other" : "none"; pathTag = "editor";   // contenteditable / 自定义编辑器
        if (lf->editor().canEdit()) {
            lf->editor().insertTextWithoutSendingTextEvent(text, false, nullptr);
            inserted = 1;
        } else {
            // 兜底:合成键事件(Char 默认动作)。
            OptionSet<PlatformEvent::Modifier> mods;
            MonotonicTime t = MonotonicTime::now();
            PlatformKeyboardEvent raw(PlatformEvent::Type::RawKeyDown, ""_s, ""_s, ""_s, ""_s, ""_s, 0, false, false, false, mods, t);
            lf->eventHandler().keyEvent(raw);
            PlatformKeyboardEvent ch(PlatformEvent::Type::Char, text, text, ""_s, ""_s, ""_s, 0, false, false, false, mods, t);
            lf->eventHandler().keyEvent(ch);
            PlatformKeyboardEvent up(PlatformEvent::Type::KeyUp, ""_s, ""_s, ""_s, ""_s, ""_s, 0, false, false, false, mods, MonotonicTime::now());
            lf->eventHandler().keyEvent(up);
        }
    }
    int canEditAfter = lf->editor().canEdit() ? 1 : 0;
    // 诊断:回读聚焦元素的 value 长度,确认文本是否真进了 DOM,但不把用户输入内容写入 LocalState 日志。
    unsigned feValLen = 0;
    if (doc) {
        if (RefPtr<Element> fe2 = doc->focusedElement()) {
            if (is<HTMLInputElement>(*fe2))
                feValLen = downcast<HTMLInputElement>(*fe2).value()->length();
            else if (is<HTMLTextAreaElement>(*fe2))
                feValLen = downcast<HTMLTextAreaElement>(*fe2).value()->length();
        }
    }
    g_imeDiag = std::string("canEditBefore=") + std::to_string(canEditBefore)
              + " fe=" + feTag + " path=" + pathTag + " canEditAfter=" + std::to_string(canEditAfter)
              + " inserted=" + std::to_string(inserted) + " valueLen=" + std::to_string(feValLen);
    pumpQuick(*lf, g_session->page.get());
    return finishInteractionPaint(outRGBA);
}

// 诊断:最近一次 WebCoreTypeText 的可编辑/聚焦/插入状态(排查"打字不进框")。
int WebCoreEditDebug(char* out, int cap)
{
    if (!out || cap <= 0)
        return kErrBadArgs;
    std::snprintf(out, static_cast<size_t>(cap), "%s", g_imeDiag.c_str());
    return kOK;
}

// 特殊键:0=退格(DeleteBackward),1=回车(派发真键盘事件:单行 input 触发表单提交、textarea 换行,可能导航)。
int WebCoreKeyAction(int action, uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!outRGBA)
        return kErrBadArgs;
    if (!g_session || !g_session->mainFrame)
        return kErrNoSession;
    if (g_inPump)
        return kErrBusy;
    g_inPump = true;
    PumpGuard guard;
    RefPtr<LocalFrame> lf = g_session->mainFrame;

    // 重装加载检测,使回车触发的导航(表单提交)能被 pump 捕获。
    g_session->load = DriverLoadState{};
    if (g_session->client) {
        g_session->client->resetLoadState();
        DriverLoadState* lp = &g_session->load;
        g_session->client->setLoadCompletionHandler([lp](bool failed) {
            if (lp->mainDone) return;
            lp->mainDone = true;
            lp->failed = failed;
        });
    }

    if (action == 0) {
        // 退格:与 WebCoreTypeText 同理——聚焦的是文本 input/textarea 时直接删末字符改 value(确定性),
        //   否则才走 editor 的 DeleteBackward(contenteditable)。harness 的退格模型恒作用于末尾。
        RefPtr<Document> kdoc = lf->document();
        RefPtr<Element> kfe = kdoc ? kdoc->focusedElement() : nullptr;
        if (kfe && is<HTMLInputElement>(*kfe) && downcast<HTMLInputElement>(*kfe).isTextField()) {
            auto& input = downcast<HTMLInputElement>(*kfe);
            String cur = input.value();
            if (!cur.isEmpty()) {
                (void)input.setValue(cur.left(cur.length() - 1), DispatchNoEvent);
                input.dispatchInputEvent();
            }
        } else if (kfe && is<HTMLTextAreaElement>(*kfe)) {
            auto& ta = downcast<HTMLTextAreaElement>(*kfe);
            String cur = ta.value();
            if (!cur.isEmpty()) {
                (void)ta.setValue(cur.left(cur.length() - 1), DispatchNoEvent);
                ta.dispatchInputEvent();
            }
        } else {
            lf->editor().command("DeleteBackward"_s).execute();
        }
        // 退格不导航:同 WebCoreTypeText,用轻量 pump 而非"静默 0.8s"的 pumpLoop(防连续退格积压卡顿)。
        pumpQuick(*lf, g_session->page.get());
        return finishInteractionPaint(outRGBA);
    } else if (action == 1) {
        OptionSet<PlatformEvent::Modifier> mods;
        MonotonicTime t = MonotonicTime::now();
        // RawKeyDown → Char(生成 keypress,charCode 13,触发表单隐式提交)→ KeyUp。
        PlatformKeyboardEvent raw(PlatformEvent::Type::RawKeyDown, ""_s, ""_s, "Enter"_s, "Enter"_s, "Enter"_s, 0x0D, false, false, false, mods, t);
        lf->eventHandler().keyEvent(raw);
        PlatformKeyboardEvent ch(PlatformEvent::Type::Char, "\r"_s, "\r"_s, "Enter"_s, "Enter"_s, "Enter"_s, 0x0D, false, false, false, mods, t);
        lf->eventHandler().keyEvent(ch);
        PlatformKeyboardEvent up(PlatformEvent::Type::KeyUp, ""_s, ""_s, "Enter"_s, "Enter"_s, "Enter"_s, 0x0D, false, false, false, mods, MonotonicTime::now());
        lf->eventHandler().keyEvent(up);
    } else {
        return kErrBadArgs;
    }
    pumpLoop(*lf, &g_session->load.mainDone, true, 160, 30.0, g_session->page.get());
    return finishInteractionPaint(outRGBA);
}

// 不交互,仅按当前会话状态重绘(UI 需要刷新时)。返回 0 成功。
int WebCoreSessionPaint(uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!outRGBA)
        return kErrBadArgs;
    if (!g_session || !g_session->mainFrame)
        return kErrNoSession;
    if (g_inPump)
        return kErrBusy;
    RefPtr<LocalFrame> lf = g_session->mainFrame;
    RefPtr<LocalFrameView> view = lf->view();
    if (!view)
        return kErrNoView;
    RefPtr<Document> doc = lf->document();
    if (!doc)
        return kErrNoDocument;
    doc->updateLayoutIgnorePendingStylesheets();
    int nonWhite = 0;
    int prc = paintToRGBA(*view, g_session->w, g_session->h, outRGBA, nonWhite);
    if (prc != kOK)
        return prc;
    writeDiag(*doc, *view, g_session->w, g_session->h, nonWhite);
    return kOK;
}

// 实时一帧:推进 rAF/动画/IntersectionObserver(isolatedUpdateRendering)+ 布局 + 重绘,不发起导航、不 pump、
// 不写 diag(高频低开销)。供 harness 定时器以低帧率驱动:让 CSS/JS 动画动起来、SPA 多帧渐进挂载。
// 帧像素哈希存 g_lastFrameHash(WebCoreGetFrameHash 取),harness 据此在画面静止时停帧省电。
int WebCoreLiveTick(uint8_t* outRGBA)
{
    using namespace WebCore;
    if (!outRGBA)
        return kErrBadArgs;
    if (!g_session || !g_session->page)
        return kErrNoSession;
    if (g_inPump)
        return kErrBusy;

    // Apotheosis (event-driven present): the composite the last wake-up asked for starts here, so
    // re-arm the wake path. Anything invalidated from now on - including from inside this very
    // tick's isolatedUpdateRendering (a rAF callback re-registering, a CSS animation asking for the
    // next frame) - fires a fresh wake and therefore schedules the next tick. Nothing asking =
    // no wake = the harness goes idle after this tick. This is what makes the loop event-driven.
    presentWakeDisarm();

    // 网络 / 图片解码完成回调经 RunLoop 任务投递。仅 isolatedUpdateRendering 不会取这些任务,
    // 所以空白占位图会一直等到下一次点击/滚动的 pumpLoop 才刷新。实时 tick 先轻量转几轮队列。
    PerfOpGuard perfOp("tick", nullptr, 0, 0);   // M4
    // After perfBegin() (it resets the row): wake-ups fired since the previous tick -> wake_count.
    if (g_perfOn)
        g_perfCur.wakes = static_cast<int>(g_presentWakes.exchange(0, std::memory_order_relaxed));
    for (int i = 0; i < 3; ++i)
        RunLoop::cycle();
    {
        PerfPhase perfRender(&g_perfCur.renderUpdate);   // M4
        g_session->page->isolatedUpdateRendering();   // 推进一帧动画/rAF/IO(可能跑 JS,甚至导航/换帧)
    }
    RefPtr<LocalFrame> lf = g_session->page->localMainFrame();
    if (!lf)
        return kErrFrameGone;
    g_session->mainFrame = lf;
    RefPtr<LocalFrameView> view = lf->view();
    if (!view)
        return kErrNoView;
    RefPtr<Document> doc = lf->document();
    if (!doc)
        return kErrNoDocument;
    doc->eventLoop().performMicrotaskCheckpoint();
    {
        PerfPhase perfLayout(&g_perfCur.styleLayout);   // M4
        doc->updateLayoutIgnorePendingStylesheets({ WebCore::LayoutOptions::UpdateCompositingLayers });   // see gpuPrepare
    }
    g_lastPendingResources = countPendingResources(*doc);
    // Apotheosis (M4 load throttle): while the main document loads everything above has
    // run (parser-driven layout, rAF, finished decodes), only the composite - the most expensive
    // thing on this thread - is limited to one per 250 ms, so the arriving subresources get the
    // engine thread instead of repeatedly rasterising a half-built page. The frame that first
    // qualifies as visually non-empty is never skipped. needsPresent stays set, so the next tick
    // composites everything at once. Only in event-driven mode: a fixed-tick harness stops its
    // timer when the frame hash does not change, and nothing would restart it.
    const bool navFirstPaintFrame = g_navFirstPaintPending.exchange(0, std::memory_order_acq_rel);
    if (!navFirstPaintFrame && g_presentCb.load(std::memory_order_acquire) && navLoadThrottleActive()) {
        const double nowSec = monotonicSeconds();
        if (nowSec - g_navTickCompositeSec < kNavWakeIntervalSec) {
            WebCorePort::presentRequested();   // throttled: comes back as the next allowed wake
            return kOK;
        }
        g_navTickCompositeSec = nowSec;
    }
    int nonWhite = 0;
    // Apotheosis (M4): perf.csv showed every idle tick paying ~1 s in
    // updateBackingStoreIncludingSubLayers because gpuPrepare force-dirties the
    // whole layer tree. If WebCore did not ask for a rendering update since the
    // last present and no layer animation is running, nothing can have changed:
    // take the scroll fast path and keep the uploaded tiles. Any invalidation
    // (JS/DOM change, image decode, new layers) sets needsPresent through
    // triggerRenderingUpdate and still gets the full dirty-tree composite.
    // Experiment F (2026-09-02) then set it on EVERY tick, because on a code-hosting site not
    // one of 84 ticks took the fast path - the page requests a rendering update every
    // tick, so each tick still re-rasterised the whole tree (0.8-1.1 s).
    //
    // Apotheosis (review 2026-09-04 item 6) then narrowed it to "riding on a scroll the
    // engine has just applied AND nothing asked for a rendering update since the last
    // present", on the theory that a tick which fails either test may have changed
    // content that only forceDirtyTree can recover.
    //
    // Apotheosis (2026-09-06, package 0.1.9.16 device regression): that theory is wrong
    // and its cost is the stall the user sees. peekNeedsPresent() is "somebody asked for
    // a rendering update", which on any page with a timer, an animation or an
    // IntersectionObserver is EVERY tick - on a news site all 626 ticks of session
    // 20260906-180145 force-dirtied all ~90 layers at 400-1100 ms of ms_backing each,
    // against 15 such ticks in ~900 rows of the 0.1.9.15 session (20260904-161535) and 17
    // in 1895 rows of 0.1.9.12. Scrolling is smooth until a tick lands, then stalls for
    // most of a second - a quarter page apart, exactly as reported.
    //
    // forceDirtyTree() only ever existed to recover content the BACKING STORE lost, never
    // content the page changed: a real change arrives as m_needsDisplay/m_needsDisplayRect
    // from RenderLayerBacking and is repainted on the fast path too, and gpuPrepare()
    // runs updateBackingStoreIncludingSubLayers() either way. Every way a store can end up
    // without its pixels is now detected per layer, so the sledgehammer has nothing left to
    // fix and only repaints what is already correct:
    //   - store freshly created            -> m_wkNeedsFullRepaint (GraphicsLayerTextureMapper.cpp
    //                                         :413, the "images flicker to a white box" fix that
    //                                         experiment F used to expose)
    //   - layer resized / contents rescaled -> m_wkBackingStoreSize / m_wkBackingStoreScale (:718)
    //   - tiles entering the viewport       -> m_wkVisibleRectChanged, they paint themselves in full
    //   - tiles that lost their rasterisation -> wkHasUnpaintedVisibleTiles() (035bae976c, 9ac9296254)
    // and the composite that still comes out short of pixels is redone in full by the
    // unpainted-tile repair in gpuPresent() - the safety net, now back to being rare.
    if (g_gpuActive)
        g_gpuScrollFast = true;
    const int prc = paintToRGBA(*view, g_session->w, g_session->h, outRGBA, nonWhite);
    // Apotheosis (event-driven present): belt and braces for the "still dirty when the tick ended"
    // case - a repaint request that landed after gpuPresent() consumed takeNeedsPresent(), or one
    // notePendingRasterTiles() re-armed. presentRequested() is a no-op when a wake is already armed
    // (the usual case for an animating page, which armed one during isolatedUpdateRendering above).
    if (g_session && g_session->chrome && g_session->chrome->peekNeedsPresent())
        WebCorePort::presentRequested();
    return prc;
}

int WebCoreGetPendingResourceCount()
{
    return g_lastPendingResources;
}

// 取最近一帧的像素哈希。实时模式下 harness 比较连续帧哈希:不变即画面静止 → 停帧省电(下次交互/滚动/导航再启)。
unsigned WebCoreGetFrameHash()
{
    return g_lastFrameHash;
}

// ---------------------------------------------------------------------------
// Minimal stub variant for bring-up: only does process init + Page creation,
// then fills the buffer with opaque solid red. Lets you validate the init +
// Page::create path (steps 1-3) and the C ABI/buffer plumbing before trusting
// the full layout+paint path. Build with -DWEBCOREDRIVER_STUB to substitute it
// for the real entry point.
// ---------------------------------------------------------------------------
#ifdef WEBCOREDRIVER_STUB
int WebCoreRenderHtmlStub(const char* utf8Html, int w, int h, uint8_t* outRGBA)
{
    (void)utf8Html;
    if (!outRGBA || !isValidSurfaceSize(w, h))
        return kErrBadArgs;
    ensureWebCoreInitialized();
    auto cfg = pageConfigurationWithEmptyClients(std::nullopt, PAL::SessionID::defaultSessionID());
    Ref<Page> page = Page::create(WTF::move(cfg));
    if (!page->localMainFrame())
        return kErrNoMainFrame;
    for (int i = 0; i < w * h; ++i) {
        outRGBA[i * 4 + 0] = 0xFF; // R
        outRGBA[i * 4 + 1] = 0x00; // G
        outRGBA[i * 4 + 2] = 0x00; // B
        outRGBA[i * 4 + 3] = 0xFF; // A
    }
    return kOK;
}
#endif

} // extern "C"
