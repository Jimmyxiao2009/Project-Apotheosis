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
// Apotheosis (OFFTHREAD-RASTER-LOG.md §4): off-thread tile rasterisation switch and its in-flight
// counter (TextureMapperTile.h, WebKit winuwp 54db7b8987). Declared here for the same reason as
// the two above — including the texmap header would drag in TextureMapperGLHeaders.h.
void wkWinUWPSetThreadedRaster(bool);
bool wkWinUWPThreadedRaster();
unsigned wkWinUWPTexmapPendingRasterTiles();
// Apotheosis (OFFTHREAD-RASTER-LOG.md §10, WebKit winuwp 099064a24d..c0608a6353): step 4 made
// first paints asynchronous too, so a tile whose replay has not landed draws NOTHING. Missing the
// follow-up composite is now an empty tile, not a stale one — hence the edge-triggered counter and
// the worker-side wake-up below, which cover the two cases polling alone cannot.
unsigned wkWinUWPTexmapTakeFinishedRasterTiles();          // engine thread; reading resets
void wkWinUWPSetRasterCompletionHandler(void (*)());       // called ON A WORKER thread
void wkWinUWPTexmapRasterStats(unsigned& posted, unsigned& cancelled, unsigned& blockingWaits);
// Apotheosis (WHITE-AT-SCROLL-END, 2026-09-04): keep the tiles a layer drops out of its cover rect
// and re-draw them, scaled, until fresh ones exist (TextureMapperTiledBackingStore). Default ON;
// the switch is here so the device can A/B it without a rebuild. Declared by hand for the same
// reason as everything above - the texmap header would drag in TextureMapperGLHeaders.h.
void wkWinUWPSetStaleTiles(bool);
// Apotheosis (WHITE-AT-SCROLL-END, 2026-09-04): how often a visible tile has run out of composites
// to draw something (TextureMapperTiledBackingStore::wkTrackUnpaintedVisibleTiles, counted inside
// paintToTextureMapper). A LEVEL, not a snapshot - the interesting quantity is the delta across one
// composite: "this paint walked over visible tiles that hold no pixels". That is the authoritative
// version of the pixel probe this replaces. Declared by hand for the same reason as everything
// above - the texmap header would drag in TextureMapperGLHeaders.h.
unsigned wkWinUWPTexmapUnpaintedVisibleTiles();
}
#include <WebCore/CookieJar.h>           // WebCore::CookieJar(cookie 持久化)
#include <WebCore/NetworkStorageSession.h>   // deleteAllCookies(WebCoreClearCookies)
#include <WebCore/StorageSessionProvider.h>  // 完整类型(Ref<StorageSessionProvider> 析构需要)
#include "PortNetworkStorageSession.h"   // WebCorePort::makeStorageSessionProvider / ensureDefaultPortStorageSession
#include "PortChromeClient.h"            // WebCorePort::PortChromeClient(开合成,捕获根图层)
#include <WebCore/LayoutMilestone.h>     // Apotheosis (M4 load timeline): DidFirstVisuallyNonEmptyLayout
#include <WebCore/Page.h>                // WebCore::Page
#include <WebCore/Settings.h>            // Page::settings()
#include <WebCore/FontLoadTimingOverride.h>  // Apotheosis (M4): FontLoadTimingOverride::Swap
#include <WebCore/LocalFrame.h>          // WebCore::LocalFrame
#include <WebCore/LocalFrameInlines.h>   // inline LocalFrame::document()/protectedDocument()
#include <WebCore/LocalFrameView.h>      // WebCore::LocalFrameView
#include <WebCore/DocumentView.h>        // inline LocalFrame::view()/protectedView()
#include <WebCore/FrameLoader.h>         // FrameLoader::activeDocumentLoader
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

// ---- Apotheosis (presenter thread): a second GL context on its own thread owns the swap chain ----
// Raw EGL is needed for exactly two things the WebCore GLContext wrapper does not expose: the
// cross-thread fence (EGL_KHR_fence_sync, resolved through eglGetProcAddress so the driver keeps no
// link-time dependency on an extension ANGLE 2.1.13 may or may not advertise at runtime) and the
// EGLDisplay to pass to it. Everything else - context creation, share group, makeCurrent,
// eglSwapBuffers - still goes through WebCore::GLContext, which already knows how to hand a
// SwapChainPanel PropertySet to eglCreateWindowSurface.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <wtf/Condition.h>
#include <wtf/Lock.h>
#include <wtf/Threading.h>

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
// viewport once the frames are decoded. On chaos.social (200+ images) mem.txt showed dec=32 MB
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
// Apotheosis (pan present handshake): during a main-frame touch pan the harness moves the already
// presented pixels itself with a XAML TranslateTransform (instant pan) while the engine catches up
// in coarse steps. The SwapChainPanel content and that transform are composed by DWM
// independently, so ANY present the harness did not ask for shows new content under the old
// translation for a frame - the "it briefly jumps back" flicker seen on device. While g_panGesture
// is set the engine therefore presents nothing on its own: eglSwapBuffers is deferred (g_swapOwed)
// and released by WebCorePresent(), which the harness posts once XAML has committed the matching
// translation. Engine thread only, like every other flag here.
static bool g_panGesture = false;
static bool g_swapOwed = false;       // a composite ran whose swap was deferred and is still owed
// Apotheosis (stale deferred swap, 2026-09-04): a deferred swap is only correct for the scroll
// position it was composited at. The harness releases it two XAML frames after the completion of
// the scroll job it belongs to, and a newer scroll frame may supersede that wait and release both
// at once (MainPage::DisarmPanAck) - so the WebCorePresent() that arrives can be the acknowledgement
// of a job that is no longer the newest one the engine has applied. Swapping then puts the older
// composite on screen underneath a translation that has already been reduced by the newer delta,
// i.e. the content jumps back towards where the gesture started for one frame. Tag every owed swap
// with the scroll generation (and position) it was composited at; WebCorePresent() releases it only
// if it still belongs to the newest applied scroll, and otherwise drops the frame and asks for a
// fresh composite. Engine thread only, like every other flag here.
//
// Apotheosis (XAML-path consistency review, 2026-09-04): that tag is also the frame's IDENTITY, and
// the harness now names it when it acknowledges (WebCorePresentFrame). Two holes the "is it still
// the newest scroll?" test alone cannot close:
//   * the acknowledgement and a newer WebCoreScrollBy can reach the engine queue in either order.
//     Arriving after it, an ack meant for frame N releases frame N+1 - which is exactly the frame
//     under an older translation the whole handshake exists to prevent.
//   * the composite the ack names need not be a scroll composite at all. Every export that paints
//     while a gesture runs (WebCoreClickAt, WebCoreWheelAt, WebCoreDragAt, WebCoreSetPageScale,
//     WebCoreSessionPaint, WebCoreComposite) also defers its swap and re-arms g_swapOwed with the
//     current generation, so a pan ack could put a double-tap-zoom frame on screen under a pan
//     translation.
// Every owed swap therefore carries a swap id the harness reads back with
// WebCoreGetOwedSwapScroll() and hands to WebCorePresentFrame(); an id that does not match is
// dropped WITHOUT clearing g_swapOwed - the composite in the back buffer is newer than the ack and
// is still owed to its own acknowledgement, and nothing would ever recomposite it during a gesture
// (WebCoreLiveTick skips its composite while g_panGesture is set).
static uint64_t g_scrollGen = 0;      // bumped by every main-frame scroll the engine applies
static uint64_t g_swapOwedGen = 0;    // g_scrollGen as it was when the owed composite ran
static uint64_t g_swapOwedId = 0;     // identity of the owed frame; what WebCorePresentFrame matches on
static uint64_t g_swapIdNext = 1;     // 0 stays reserved for "no frame" / "any frame" (legacy WebCorePresent)
static int g_swapOwedScrollX = 0;     // scroll position that composite is showing (diagnostics + harness residual)
static int g_swapOwedScrollY = 0;
// Apotheosis (drag as pointer events): a mousedown WebCoreDragAt() dispatched and the page
// consumed is still in flight — moves/releases only reach the page while this is set, and
// teardownSession() clears it. Engine thread only, like every other flag here.
static bool g_dragActive = false;
static bool g_gpuPresentMode = false; // WebCoreGpuInit 收到窗口表面=true → 各帧直呈现到 SwapChainPanel(省 readback)
static bool g_gpuAnimating = false;   // 最近一次合成时图层树仍有动画在跑(applyAnimationsRecursively 返回值),直呈现模式的帧变化信号之一

// Apotheosis (presenter thread): RoInitialize/RoUninitialize for the presenter thread. WinRT
// apartment bring-up, not WRL - nothing here uses C++/CX or WRL types, ANGLE only needs the
// calling thread to have an apartment when it QIs the SwapChainPanel. Declared in the App
// partition of the SDK, imported from WindowsApp.lib, which the driver already links.
#include <roapi.h>

// ===========================================================================
// Apotheosis (presenter thread) - the swap chain gets exactly one owner.
//
// The problem the pan-present handshake above could not solve. Two producers ended up on the same
// screen: the engine thread's eglSwapBuffers and the UI thread's XAML TranslateTransform on the
// same SwapChainPanel. DWM commits them in different frames, so every engine step showed one frame
// of content-without-transform (or transform-without-content) - a visible jump. Coarser engine
// steps only made the jumps rarer, and no UI-thread handshake can close it, because a XAML property
// change becomes visible at the next composition commit while an eglSwapBuffers is visible at once.
// On top of that, while the engine thread ran JS or layout it could not present at all, so the page
// simply stopped moving (github).
//
// The fix is to take the swap chain away from both of them and give it to a third thread that does
// nothing else:
//
//   engine thread    composites the layer tree into an OFFSCREEN FBO-backed texture (two of them,
//                    double buffered, viewport sized) with its own EGL context - which is now an
//                    offscreen context, it never touches the window surface again - and publishes
//                    {texture, fence, scroll position at composite time, background colour} here.
//   presenter thread owns a second EGL context in the SAME share group (both are created through
//                    PlatformDisplay, so both share with PlatformDisplay::sharingGLContext()) and
//                    owns the window surface. It draws the newest published texture as a full-screen
//                    quad, translated by the pan residual, and swaps. Nothing else runs on it, so a
//                    busy engine can no longer stop the screen from moving.
//   UI thread        pushes the raw finger offset in with WebCoreSetPanOffset() at touch rate and
//                    goes away again. No engine hop, no XAML transform, no handshake.
//
// The pan residual is computed HERE, not in the harness: every published frame carries the scroll
// position it was composited at, so the presenter subtracts the engine's real progress itself
//   residual = panOffset - (scrollAtComposite - scrollAtGestureStart)
// which is exactly what the old InstantPanApplied() bookkeeping did on the UI thread, one round
// trip later. Clamped to one screen; the strip the translation uncovers is cleared to the page
// background colour of the frame being shown.
//
// Threading rules kept intact (repo CLAUDE.md 线程铁律): the presenter never calls into WebCore and
// never waits on the UI thread; the UI thread never waits on the presenter (WebCoreSetPanOffset
// takes one uncontended lock and returns); the engine waits on the presenter only for the few
// microseconds it takes to issue one quad if it wants to overwrite the buffer being sampled, and
// gives up after 50 ms. ANGLE marshals window-surface CREATION to the panel dispatcher, which is
// why the presenter creates its surface while the UI thread is idle (WebCoreGpuInit is already a
// posted job) and why WebCoreGpuInit falls back to the old engine-owned window surface if that
// does not come back within 5 s.
//
// Everything here is behind WebCoreSetPresenterThread(); with it off WebCoreGpuInit takes exactly
// the path it took before and g_presenterActive stays false, so every branch below is skipped.
// ===========================================================================
// Apotheosis (review 2026-09-04 item 1): DEFAULT OFF. 0.1.9.14 on device showed background-only
// frames at the end of a scroll with the presenter on, so the shipping default is the engine-owned
// window surface (the pre-presenter behaviour). The harness mirrors this default in
// MainPage.xaml.h; a settings.ini that already carries "presenter=1" still wins for that install.
static bool g_presenterWanted = false;                 // switch, read once per WebCoreGpuInit
static std::atomic<bool> g_presenterActive { false };  // the presenter really owns the swap chain
// Apotheosis (review 2026-09-04 item 2): the presenter can retire ITSELF, mid-session, on a swap
// that fails for good. It clears g_presenterActive from its own thread, where it may not touch
// WebCore, so it leaves this behind instead: the next gpuPresent on the ENGINE thread consumes it,
// force-dirties the whole tree and re-arms needsPresent, so the engine goes back to compositing
// and swapping on its own context - the only thing left that can still put pixels anywhere.
static std::atomic<int> g_presenterRetired { 0 };
static void* g_presenterWindow = nullptr;              // native window handed to the presenter thread
static EGLDisplay g_presenterEglDisplay = nullptr;

// Apotheosis (presenter thread, WHITE-SCREEN fix 2026-09-04): the cross-context EGL fence
// handshake is GONE. Both halves of it - the engine's eglCreateSyncKHR + the presenter's
// eglClientWaitSyncKHR, and the presenter's fence the engine waited on before reusing a slot -
// were waits on an EGLSync created by ANOTHER context on ANOTHER thread. ANGLE 2.1.13 (the
// WindowsStore NuGet we ship) implements EGL_KHR_fence_sync on D3D11 as an ID3D11Query(EVENT)
// whose End()/GetData() run on the ONE immediate device context the whole display shares; asking
// for it from the presenter thread does not order anything against the engine thread's command
// stream, and it came back EGL_CONDITION_SATISFIED_KHR straight away. The presenter therefore
// sampled the slot texture while the engine's composite had only got as far as
// TextureMapper::clearColor(documentBackgroundColor) - i.e. an all-white texture - and swapped
// that. On device: the page shows up once (the last composite of a load lands while the presenter
// is asleep) and then goes white for the rest of the session, flashing in whenever the race is
// won. The synchronisation is now a plain glFinish() on the producing side of each handover:
// the engine finishes before it publishes, the presenter finishes before it releases the slot.
// That is what the no-extension branch always did; it is simply the only branch left. The
// extension string is still queried, but only so the diagnostics can say what the display claims.
static bool g_presFenceAdvertised = false;

// One published frame. `texture` is set once at start-up and never replaced, so the presenter may
// read `texture->id()` under the lock and sample it outside; BitmapTexture is ThreadSafeRefCounted
// and these two are held for the life of the process (like g_glContext, deliberately never
// destroyed - a cross-thread glDeleteTextures at exit is what we are avoiding everywhere here).
struct PresenterFrame {
    RefPtr<WebCore::BitmapTexture> texture;
    int scrollX { 0 };
    int scrollY { 0 };
    float bg[4] { 1.0f, 1.0f, 1.0f, 1.0f };
};

// Apotheosis (presenter diagnostics): counted on the presenter thread (and one on the engine
// thread), dumped into crash.txt every kPresStatsEvery draws and on the first one. A device run
// that is still wrong must be able to say WHY without another guess - see presenterStatsDump().
static const unsigned kPresStatsEvery = 240;
static std::atomic<unsigned> g_presDraws { 0 };          // quads issued
static std::atomic<unsigned> g_presSwaps { 0 };          // eglSwapBuffers done
static std::atomic<unsigned> g_presSkipNoPub { 0 };      // woke with nothing published
static std::atomic<unsigned> g_presSkipDedupe { 0 };     // same generation and same translation
static std::atomic<unsigned> g_presSkipSuspended { 0 };  // app suspended
static std::atomic<unsigned> g_presSkipNoTex { 0 };      // published slot had texture id 0 -> clear only
static std::atomic<unsigned> g_presEngineDrops { 0 };    // engine dropped a composite: no free slot in 50 ms
static std::atomic<int> g_presLastEglError { 0 };        // eglGetError() after the last swap
static std::atomic<int> g_presProbeContent { -1 };       // pixels != clear colour in the last probe block
static std::atomic<int> g_presProbeTotal { 0 };
static std::atomic<unsigned> g_presSkipEmpty { 0 };      // light composites redrawn in full: unpainted visible tiles

// Apotheosis (WHITE-AT-SCROLL-END, 2026-09-04): the engine may publish a slot that holds nothing
// but the clear colour.
//
// In the old engine-owned-window-surface world an "empty" composite was harmless: a frame that
// painted no tile simply left the back buffer holding the PREVIOUS frame's pixels and re-swapped
// it, so the screen did not change. With two offscreen slots every composite starts with
// target.reset() + bindAsSurface(), i.e. a full clear (3c866bf) - so the same no-op composite now
// publishes a freshly cleared, white texture, and the presenter dutifully puts it on screen. That
// is the device symptom: content while the finger moves, everything white the moment scrolling
// ends, with draws==swaps, skip_notex=0, engine_drops=0 and the probe of the drawn slot falling
// 256 -> 94 -> 0.
//
// Who produces such a composite: every live tick takes the scroll fast path (WebCoreLiveTick sets
// g_gpuScrollFast unconditionally), so gpuPrepare skips forceDirtyTree and the composite is drawn
// entirely from the tiles the backing stores still hold. When those tiles are gone - dropped
// outside the keep rect while the gesture moved the visible rect, recycled on a scale change, or
// still owed by a raster worker - the tree paints nothing at all and the whole screen is the clear
// colour. The engine-side repair (keeping stale tiles) is a separate change; what belongs HERE is
// that a composite which drew nothing must not be published over a good frame.
//
// Apotheosis (2026-09-04, device package 13): the pixel probe this block first described was the
// wrong instrument. On device it PASSED - the light composite draws the layer's background colour,
// which is not the clear colour, so "did anything at all get drawn" says yes - while the frame held
// no content tile whatsoever and the screen showed nothing but github's dark grey. The engine
// already knows the answer exactly: wkWinUWPTexmapUnpaintedVisibleTiles() counts, inside the paint
// itself, every visible tile that has run out of composites to produce pixels. Its delta across one
// paint is therefore the authoritative "this frame is missing content".
//
// So the rule is now: a LIGHT composite that walked over unpainted visible tiles is not published.
// It is redrawn in full - force-dirty the whole tree, update every backing store, paint again into
// the same slot - and THAT frame is published. Exactly what the pre-presenter path did for every
// frame, at the cost of one full composite in the rare case; a full composite is trusted
// unconditionally, so this can never loop.
static bool g_gpuLastCompositeFull = false;   // gpuPrepare force-dirtied the tree this composite
static bool g_gpuForceFullNext = false;       // next composite must force-dirty whatever the caller asks
static int g_gpuLastUnpainted = 0;            // unpainted-visible-tile events in the last paint (diagnostics)

struct PresenterState {
    WTF::Lock lock;
    WTF::Condition cond;
    PresenterFrame slot[2];
    int published { -1 };          // newest published slot, -1 = nothing composited yet
    int inUse { -1 };              // slot the presenter is sampling right now
    uint64_t generation { 0 };     // bumped on every publish
    // Pan, all in engine px. panX/panY = what the finger has asked for since the gesture started.
    float panX { 0.0f };
    float panY { 0.0f };
    bool panActive { false };      // finger (or inertia) is driving the offset
    bool panEnding { false };      // gesture over, the residual is decaying as the engine catches up
    MonotonicTime panDeadline;     // hard snap-to-zero if the engine never catches up
    int panBaseX { 0 };            // scroll position the frame on screen had when the gesture began
    int panBaseY { 0 };
    bool panBaseValid { false };
    bool wake { false };
    bool stop { false };
    bool suspended { false };
    int initState { 0 };           // 0 = starting, 1 = ready, -1 = failed
};
// Apotheosis: written exactly once, by the engine thread in presenterStart(), and read by the
// presenter thread and - the reason this is an atomic - by the UI thread in WebCoreSetPanOffset /
// WebCoreSetPresenterSuspended. The release store publishes the fully built PresenterState (its
// lock, its textures) to whoever acquire-loads a non-null pointer; a plain pointer gave the
// compiler and the ARMv7 memory model every right to hand out a half-initialised object. It is
// never freed and never reset to null once non-null, so an acquire-load that saw it stays valid
// for the life of the process (see presenterStart's detach path).
static std::atomic<PresenterState*> g_pres { nullptr };
static RefPtr<WTF::Thread> g_presenterThread;

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
// exactly what ntv.de does on a Lumia 950. So the driver writes the crashing
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
    "t_firstbyte,t_commit,t_dcl,t_firstpaint,t_load,t_settle,n_scripts,n_subres\n";

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
        std::fprintf(fp, "%u,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%d,%d,%d,%d,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,"
                         "%s,%s,%s,%s,%s,%s,%s,%s\n",
            r.seq, r.kind, r.url,
            d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11],
            n[0], n[1], n[2], n[3], r.gpu, r.dfg, r.w, r.h, df, dp,
            nt[0], nt[1], nt[2], nt[3], hv,
            rs[0], rs[1], rs[2], rs[3], rs[4], wk,
            tl[0], tl[1], tl[2], tl[3], tl[4], tl[5], nsc, nsr);
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
    std::fprintf(fp, "timeline url=%s firstbyte=%s commit=%s dcl=%s fp=%s load=%s settle=%s"
                     " total=%.0f subres=%d/%d scripts=%d\n",
        r.url, t[0], t[1], t[2], t[3], t[4], t[5],
        r.total < 0 ? 0.0 : r.total, r.subOk, r.subStarted, r.nScripts);
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
    } else
        g_perfCur.frames = 1;
    g_perfCur.gpu = g_gpuActive ? 1 : 0;
    g_perfCur.dfg = 0;   // hardcoded: becomes a build flag once ENABLE_DFG_JIT=ON is measured (M4 §4)
    if (g_perfCur.w <= 0 && g_session) {            // viewport of the live session
        g_perfCur.w = g_session->w;
        g_perfCur.h = g_session->h;
    }
    if (g_perfOpIsNav)
        perfWriteStageTimeline(g_perfCur);
    if (g_perfRows < kPerfRingSize)
        g_perfRing[g_perfRows++] = g_perfCur;
    // Flush on nav completion, and every 32 rows so a crash mid-session (seen
    // on ntv.de) does not take the whole ring with it - still one file open per
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
    if (!g_perfOn || !g_perfInOp || g_perfCur.domReady >= 0)
        return;
    g_perfCur.domReady = perfSinceNavStart();
}

void perfNavLoadEvent()
{
    navLoadEnd();   // Apotheosis (M4 load throttle): full present rate from here on (perf-independent)
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
extern "C" void WebCorePortNetTiming(int isMainResource, double dnsMs, double connectMs,
    double tlsMs, double ttfbMs, int httpVersion)
{
    if (!g_perfOn || !g_perfInOp || !g_perfOpIsNav || !isMainResource)
        return;
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
    bool stopped = false;
    auto stopLoop = [&stopped] {
        if (stopped)
            return;
        stopped = true;
        RunLoop::currentSingleton().stop();
    };
    int settleTicks = 0;
    int quietTicks = 0;
    RefPtr<LocalFrame> frameRef = &frame;
    RunLoop::Timer settle(Ref { RunLoop::currentSingleton() }, "WebCorePort.pump.settle"_s,
        WTF::Function<void()> { [&stopLoop, &settleTicks, &quietTicks, frameRef, mainDone, allowEarlyStopWithoutNav, settleCapTicks, pageForRendering] {
            if (g_perfOn)
                ++g_perfPumpTicks;   // Apotheosis: settle ticks of the current operation (M4)
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
                    stopLoop();
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
            bool domReady = committedLoader && committedLoader->isCommitted()
                && frameDoc && !frameDoc->parsing();
            bool ready = navDone || allowEarlyStopWithoutNav || domReady;

            // Apotheosis (M4 settle): how long the pump keeps the UI hostage after the page is
            // usable. It used to be one rule for everything - "the loader has been quiet for 16
            // ticks (0.8 s)", capped at settleCapTicks (160 = 8 s) after the load event. A page
            // with hundreds of subresources never gives us 0.8 s of quiet in a row (ntv.de: 255
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

            // ★ 关键:绝不在 isLoadingInAPISense 一转 false 就停。模块求值(<script type=module>)和
            //   重定向后最终文档的样式表应用都发生在"加载器空闲之后",经 ScriptRunner/WindowEventLoop 的
            //   0_s 定时器 + 微任务级联触发——这要求 RunLoop 继续转若干 tick。改为"加载器持续静默 ~0.8s
            //   才停":期间求值/挂载会引发新活动(渲染/拉字体),重置静默计数,自然等到真稳定。
            if (loading)
                quietTicks = 0;
            else
                ++quietTicks;
            if (navDone)
                ++settleTicks;
            if (ready && quietTicks >= quietNeeded) {    // 加载器静默 → 异步级联已跑完,停
                stopLoop();
                return;
            }
            if (navDone && settleTicks > capTicks)        // 硬封顶,防长连接/永久活动拖到看门狗
                stopLoop();
        } });
    settle.startRepeating(0.05_s);
    RunLoop::Timer watchdog(Ref { RunLoop::currentSingleton() }, "WebCorePort.pump.watchdog"_s,
        WTF::Function<void()> { [&stopLoop] { stopLoop(); } });
    watchdog.startOneShot(WTF::Seconds(watchdogSeconds));
    RunLoop::run();
    settle.stop();
    watchdog.stop();
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
        const bool wkFullDirty = !g_gpuScrollFast || g_gpuForceFullNext;
        if (wkFullDirty)
            forceDirtyTree(glRoot);                                     // 强制全树标脏,否则脏区已被消费 → 内容 tile 空
        g_gpuScrollFast = false;
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

// ===========================================================================
// Apotheosis (presenter thread) - implementation. See the big comment at g_presenterWanted.
// ===========================================================================

// A textured quad, drawn once per presented frame. GLES2, written out here rather than borrowed
// from TextureMapper because a second TextureMapper on a second thread would drag WebCore's shader
// cache, texture pool and their singletons onto a thread that must never touch WebCore.
static const char* kPresenterVertexShader =
    "attribute vec2 a_pos;\n"
    "uniform vec2 u_trans;\n"
    "varying vec2 v_tex;\n"
    "void main() {\n"
    // The engine renders into an FBO, where TextureMapper forces the flipped projection
    // (updateProjectionMatrix: a bound surface is always flipY=true), so texture row 0 holds the
    // TOP of the page. The window surface is the other way round, hence 1.0 - y here. Derived from
    // the untranslated corner so the translation moves texture and quad together.
    "  v_tex = vec2((a_pos.x + 1.0) * 0.5, (1.0 - a_pos.y) * 0.5);\n"
    "  gl_Position = vec4(a_pos + u_trans, 0.0, 1.0);\n"
    "}\n";
static const char* kPresenterFragmentShader =
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "varying vec2 v_tex;\n"
    "void main() { gl_FragColor = texture2D(u_tex, v_tex); }\n";

struct PresenterGL {
    GLuint program { 0 };
    GLuint vbo { 0 };
    GLint aPos { -1 };
    GLint uTrans { -1 };
    GLint uTex { -1 };
    GLuint probeFbo { 0 };   // diagnostics only, created on the first probe (see presenterProbe)
};

// Apotheosis: whole-token search in a space-separated EGL/GL extension string. eglGetProcAddress
// returning a non-null pointer says nothing about whether the extension is supported - ANGLE
// resolves entry points for extensions it does not expose on the current display - so the string
// is the only answer that counts. strstr() alone would also accept a prefix of a longer name.
static bool presenterHasExtension(const char* extensions, const char* name)
{
    if (!extensions || !name)
        return false;
    const size_t len = std::strlen(name);
    for (const char* p = extensions; (p = std::strstr(p, name)); p += len) {
        const bool leftOk = (p == extensions) || (p[-1] == ' ');
        const bool rightOk = (p[len] == ' ') || (p[len] == '\0');
        if (leftOk && rightOk)
            return true;
    }
    return false;
}

static GLuint presenterCompile(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    if (!s)
        return 0;
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static bool presenterBuildGL(PresenterGL& gl)
{
    GLuint vs = presenterCompile(GL_VERTEX_SHADER, kPresenterVertexShader);
    GLuint fs = presenterCompile(GL_FRAGMENT_SHADER, kPresenterFragmentShader);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return false;
    }
    gl.program = glCreateProgram();
    glAttachShader(gl.program, vs);
    glAttachShader(gl.program, fs);
    glBindAttribLocation(gl.program, 0, "a_pos");
    glLinkProgram(gl.program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = 0;
    glGetProgramiv(gl.program, GL_LINK_STATUS, &linked);
    if (!linked) {
        glDeleteProgram(gl.program);
        gl.program = 0;
        return false;
    }
    gl.aPos = glGetAttribLocation(gl.program, "a_pos");
    gl.uTrans = glGetUniformLocation(gl.program, "u_trans");
    gl.uTex = glGetUniformLocation(gl.program, "u_tex");
    static const GLfloat quad[8] = { -1.0f, -1.0f,  1.0f, -1.0f,  -1.0f, 1.0f,  1.0f, 1.0f };
    glGenBuffers(1, &gl.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, gl.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    return gl.program != 0 && gl.vbo != 0 && gl.aPos >= 0;
}

// Apotheosis (presenter diagnostics): is what the presenter is about to swap actually the page, or
// an empty frame? Attach the slot texture the presenter has just drawn from to a throwaway FBO in
// the PRESENTER's context and read a small block out of its middle. The count of pixels that
// differ from the frame's own clear colour is the one number that separates "the engine composited
// nothing" from "the presenter is not showing what the engine composited": non-zero here with a
// white screen means the bug is on this side of the handover, zero means it is on the engine's.
// Expensive (a full pipeline stall plus a readback), so it runs once every kPresStatsEvery draws.
static void presenterProbe(PresenterGL& gl, GLuint texId, const float bg[4])
{
    if (!texId)
        return;
    if (!gl.probeFbo) {
        glGenFramebuffers(1, &gl.probeFbo);
        if (!gl.probeFbo)
            return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, gl.probeFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texId, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
        const int side = 16;
        const int x = (g_gpuW > side) ? (g_gpuW - side) / 2 : 0;
        const int y = (g_gpuH > side) ? (g_gpuH - side) / 2 : 0;
        uint8_t px[16 * 16 * 4] = { 0 };
        glReadPixels(x, y, side, side, GL_RGBA, GL_UNSIGNED_BYTE, px);
        const int br = static_cast<int>(bg[0] * 255.0f + 0.5f);
        const int bgc = static_cast<int>(bg[1] * 255.0f + 0.5f);
        const int bb = static_cast<int>(bg[2] * 255.0f + 0.5f);
        int differ = 0;
        for (int i = 0; i < side * side; ++i) {
            const int dr = static_cast<int>(px[i * 4 + 0]) - br;
            const int dg = static_cast<int>(px[i * 4 + 1]) - bgc;
            const int db = static_cast<int>(px[i * 4 + 2]) - bb;
            if (std::abs(dr) + std::abs(dg) + std::abs(db) > 24)
                ++differ;
        }
        g_presProbeContent.store(differ, std::memory_order_relaxed);
        g_presProbeTotal.store(side * side, std::memory_order_relaxed);
    }
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// Apotheosis (presenter diagnostics): one line into crash.txt - the file the device tooling always
// pulls (local\pull-lumia-logs.ps1) and the only writable path the driver knows. Deliberately NOT
// routed through WebCoreCrashNote/crashLogWrite: those write a full crash record and are capped at
// kMaxCrashLogEntries, which a periodic stats line would exhaust. Grep for "presenter-stats".
static void presenterStatsDump(const char* why)
{
    if (!g_crashLogPath[0])
        return;
    FILE* fp = nullptr;
    if (fopen_s(&fp, g_crashLogPath, "ab") != 0 || !fp)
        return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    const int probe = g_presProbeContent.load(std::memory_order_relaxed);
    std::fprintf(fp,
        "presenter-stats %02u:%02u:%02u.%03u %s fence_adv=%d sync=glFinish draws=%u swaps=%u "
        "skip_nopub=%u skip_dedupe=%u skip_susp=%u skip_notex=%u skip_empty=%u engine_drops=%u "
        "probe=%d/%d drew=%d eglerr=0x%04x size=%dx%d\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, why ? why : "-",
        g_presFenceAdvertised ? 1 : 0,
        g_presDraws.load(std::memory_order_relaxed), g_presSwaps.load(std::memory_order_relaxed),
        g_presSkipNoPub.load(std::memory_order_relaxed), g_presSkipDedupe.load(std::memory_order_relaxed),
        g_presSkipSuspended.load(std::memory_order_relaxed), g_presSkipNoTex.load(std::memory_order_relaxed),
        g_presSkipEmpty.load(std::memory_order_relaxed),
        g_presEngineDrops.load(std::memory_order_relaxed),
        probe, g_presProbeTotal.load(std::memory_order_relaxed), g_gpuLastUnpainted,
        static_cast<unsigned>(g_presLastEglError.load(std::memory_order_relaxed)),
        g_gpuW, g_gpuH);
    std::fclose(fp);
}

// Apotheosis (stale deferred swap): confirmation on device that the guard in WebCorePresent()
// actually fires, and by how much the dropped frame was out of date. Same plain-line channel as
// presenter-stats (crash.txt, no crash record, no crash-entry budget), capped so a long pan cannot
// fill the file. Grep for "pan-swap-drop".
static void panSwapDropNote(int nowX, int nowY)
{
    static int notes = 0;
    if (!g_crashLogPath[0] || notes >= 8)
        return;
    ++notes;
    FILE* fp = nullptr;
    if (fopen_s(&fp, g_crashLogPath, "ab") != 0 || !fp)
        return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::fprintf(fp, "pan-swap-drop %02u:%02u:%02u.%03u owed_gen=%llu scroll_gen=%llu owed=%d,%d now=%d,%d\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        static_cast<unsigned long long>(g_swapOwedGen), static_cast<unsigned long long>(g_scrollGen),
        g_swapOwedScrollX, g_swapOwedScrollY, nowX, nowY);
    std::fclose(fp);
}

// Apotheosis (pinch vs. pan residual, 2026-09-04): drop the presenter's pan base and offset.
//
// What the presenter draws during a gesture is  residual = pan - (scrollAtComposite - panBase),
// with panBase latched ONCE from the frame on screen when the gesture began (WebCoreSetPanOffset,
// or presenterPublish for a gesture that started before the first frame) and never touched again.
// Every one of those numbers lives in the engine px of ONE page scale. A pinch changes the scale
// AND re-anchors the scroll position under it (WebCoreSetPageScale), so panBase and pan now
// describe a layout that no longer exists: the residual computed from them is meaningless, and if
// the pan was still "ending" the presenter keeps translating the freshly zoomed frame by it until
// the one second deadline snaps it to zero - the page drifting away for a second after a pinch.
// Nothing else invalidated this: teardownSession() clears it for a new page, and
// WebCoreSetPanOffset(0,0,0) is the harness' own hard reset, but a scale change went unnoticed.
//
// Engine-thread callable (takes the presenter lock like every other writer), no-op when the
// presenter is not running or has no pan state. Deliberately does NOT drop P.published: the frame
// on screen is still the right pixels, only the translation applied to it is wrong.
static void presenterResetPan()
{
    PresenterState* const pres = g_pres.load(std::memory_order_acquire);
    if (!pres)
        return;
    PresenterState& P = *pres;
    Locker locker { P.lock };
    if (!P.panActive && !P.panEnding && !P.panBaseValid && P.panX == 0.0f && P.panY == 0.0f)
        return;
    P.panActive = false;
    P.panEnding = false;
    P.panBaseValid = false;
    P.panX = P.panY = 0.0f;
    P.wake = true;      // redraw the frame on screen untranslated instead of waiting for a publish
    P.cond.notifyAll();
}

static void presenterThreadMain()
{
    using namespace WebCore;
    PresenterState& P = *g_pres.load(std::memory_order_acquire);

    // Apotheosis: this thread must be a WinRT/COM thread before it touches EGL. ANGLE's
    // SwapChainPanel path QIs ISwapChainPanelNative off the IInspectable* we hand it, on the
    // CALLING thread - on a thread that never initialised the apartment that QI fails with
    // CO_E_NOTINITIALIZED, eglCreateWindowSurface fails, and the only symptom is the silent
    // fallback to the engine-owned window surface. Multi-threaded (MTA), never STA: an STA would
    // need a message pump this thread does not run, and the presenter must not be re-entered.
    // RO_INIT_MULTITHREADED is CoInitializeEx(COINIT_MULTITHREADED) plus the WinRT metadata
    // bring-up; RoInitialize/RoUninitialize are App-Container APIs and come from WindowsApp.lib.
    // S_FALSE (already initialised) is a success, RPC_E_CHANGED_MODE is not - in both cases the
    // apartment exists, so only a hard failure is worth giving up on.
    const HRESULT roHr = RoInitialize(RO_INIT_MULTITHREADED);
    const bool roOwned = SUCCEEDED(roHr);

    // The window surface is created HERE, on the thread that will own it. ANGLE marshals the
    // SwapChainPanel work inside eglCreateWindowSurface to the panel's dispatcher, so this blocks
    // until the (idle) UI thread has run it - which is why WebCoreGpuInit only ever waits for this
    // with a timeout, and why nothing on the UI thread may be waiting for the engine at that point.
    std::unique_ptr<GLContext> ctx = GLContext::create(PlatformDisplay::sharedDisplay(),
        reinterpret_cast<GLNativeWindowType>(g_presenterWindow));
    PresenterGL gl;
    bool ok = ctx && ctx->makeContextCurrent() && presenterBuildGL(gl);
    {
        Locker locker { P.lock };
        P.initState = ok ? 1 : -1;
        P.cond.notifyAll();
    }
    if (!ok) {
        // The context (and with it the window surface, which holds COM references to the panel)
        // must go before the apartment it was created in - releasing COM objects after
        // RoUninitialize is undefined. Same order on the way out of the loop below.
        ctx = nullptr;
        if (roOwned)
            RoUninitialize();
        return;
    }

    uint64_t drawnGeneration = 0;
    float drawnTx = 0.0f, drawnTy = 0.0f;
    bool drawnAnything = false;
    bool wasSuspended = false;

    for (;;) {
        int idx = -1;
        GLuint texId = 0;
        uint64_t generation = 0;
        float tx = 0.0f, ty = 0.0f;
        float bg[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        {
            Locker locker { P.lock };
            if (!P.stop && !P.wake) {
                // A live pan needs a heartbeat: the offset may keep changing without the engine
                // publishing anything. Otherwise sleep until someone wakes us (the 500 ms is only
                // a safety net, not a poll).
                P.cond.waitFor(P.lock, (P.panActive || P.panEnding) ? Seconds::fromMilliseconds(8) : Seconds(0.5));
            }
            if (P.stop)
                break;
            P.wake = false;
            if (P.suspended) {
                wasSuspended = true;
                g_presSkipSuspended.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (wasSuspended) {
                // Apotheosis: coming back from suspend, what is on the swap chain is not ours to
                // reason about - the shell may have dropped or resized it while we were frozen,
                // and the first swap after a resume is the one that puts pixels back. Forget what
                // we believe we drew, so the checks below cannot decide that an unchanged frame
                // needs no redraw and leave the panel showing whatever survived the suspend.
                wasSuspended = false;
                drawnAnything = false;
                drawnGeneration = 0;
                drawnTx = drawnTy = 0.0f;
            }
            if (P.published < 0) {
                g_presSkipNoPub.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            idx = P.published;
            PresenterFrame& f = P.slot[idx];
            generation = P.generation;

            float rx = 0.0f, ry = 0.0f;
            if (P.panActive || P.panEnding) {
                if (P.panBaseValid) {
                    rx = P.panX - static_cast<float>(f.scrollX - P.panBaseX);
                    ry = P.panY - static_cast<float>(f.scrollY - P.panBaseY);
                    const float maxX = static_cast<float>(g_gpuW), maxY = static_cast<float>(g_gpuH);
                    if (rx > maxX) rx = maxX; else if (rx < -maxX) rx = -maxX;
                    if (ry > maxY) ry = maxY; else if (ry < -maxY) ry = -maxY;
                }
                // The gesture is over: the residual shrinks with every frame the engine publishes.
                // At zero (or after one second of the engine not catching up) the engine frame alone
                // is on screen again and the pan state is dropped.
                // Apotheosis: this must run WITHOUT a valid base too. A gesture that ended before
                // any frame carried a base (WebCoreSetPanOffset found published < 0, and no publish
                // latched one afterwards) left panEnding set for ever inside the old
                // `&& P.panBaseValid` guard, and panEnding is what makes the loop take the 8 ms
                // heartbeat instead of sleeping - so the presenter woke 125 times a second for the
                // rest of the session and drew nothing. With no base there is no residual to
                // decay: rx/ry are zero, so the test below drops the pan state on the next pass.
                if (P.panEnding && ((rx > -0.5f && rx < 0.5f && ry > -0.5f && ry < 0.5f) || MonotonicTime::now() >= P.panDeadline)) {
                    P.panEnding = false;
                    P.panBaseValid = false;
                    P.panX = P.panY = 0.0f;
                    rx = ry = 0.0f;
                }
            }
            // Engine px -> clip space. A positive residual means "the engine still owes us that
            // much scroll", i.e. the content must move up/left on screen.
            tx = (g_gpuW > 0) ? (-2.0f * rx / static_cast<float>(g_gpuW)) : 0.0f;
            ty = (g_gpuH > 0) ? ( 2.0f * ry / static_cast<float>(g_gpuH)) : 0.0f;

            if (drawnAnything && generation == drawnGeneration && tx == drawnTx && ty == drawnTy) {
                g_presSkipDedupe.fetch_add(1, std::memory_order_relaxed);
                continue;   // nothing moved and no new frame: do not burn a swap
            }

            P.inUse = idx;
            texId = f.texture ? f.texture->id() : 0;
            bg[0] = f.bg[0]; bg[1] = f.bg[1]; bg[2] = f.bg[2]; bg[3] = f.bg[3];
        }
        if (!texId)
            g_presSkipNoTex.fetch_add(1, std::memory_order_relaxed);

        // Apotheosis (WHITE-SCREEN fix): nothing to wait for here any more. presenterPublish()
        // glFinish()es on the engine thread before it sets P.published, so by the time this slot
        // can be read the composite is complete on the GPU - not merely submitted, and not merely
        // "an EGLSync from another context said so". See the comment at g_presFenceAdvertised.
        glViewport(0, 0, g_gpuW, g_gpuH);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_STENCIL_TEST);
        glClearColor(bg[0], bg[1], bg[2], bg[3]);   // the strip the translation uncovers
        glClear(GL_COLOR_BUFFER_BIT);
        if (texId) {
            glUseProgram(gl.program);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, texId);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glUniform1i(gl.uTex, 0);
            glUniform2f(gl.uTrans, tx, ty);
            glBindBuffer(GL_ARRAY_BUFFER, gl.vbo);
            glEnableVertexAttribArray(static_cast<GLuint>(gl.aPos));
            glVertexAttribPointer(static_cast<GLuint>(gl.aPos), 2, GL_FLOAT, GL_FALSE, 0, nullptr);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glDisableVertexAttribArray(static_cast<GLuint>(gl.aPos));
        }
        // Apotheosis: releasing inUse only says "no longer ISSUING from this slot". The quad above
        // may still be executing, and the engine is free to composite into the slot the moment it
        // sees inUse clear - straight into the texture this draw is sampling. glFinish() closes
        // that window on the only side that can: this one. (The EGL fence that used to be handed
        // to the engine here was a cross-context sync object and did not order anything - see
        // g_presFenceAdvertised.)
        const unsigned drawCount = g_presDraws.fetch_add(1, std::memory_order_relaxed) + 1;
        if (drawCount == 1 || (drawCount % kPresStatsEvery) == 0)
            presenterProbe(gl, texId, bg);   // reads the slot back BEFORE it is released
        glFinish();

        {
            // Released before the swap: the engine may start overwriting the other buffer while
            // this one is on its way to DWM, so a vsync-blocked eglSwapBuffers never stalls it.
            Locker locker { P.lock };
            P.inUse = -1;
            P.cond.notifyAll();
        }
        ctx->swapBuffers();
        g_presSwaps.fetch_add(1, std::memory_order_relaxed);
        // Apotheosis: a swap can fail for good. On a lost device (EGL_CONTEXT_LOST) or a surface
        // the shell has pulled out from under us (EGL_BAD_SURFACE / EGL_BAD_NATIVE_WINDOW) every
        // further swap fails too, and the loop would spin on a dead swap chain for the rest of the
        // session. Give the presenter up: stop, and clear g_presenterActive so the engine stops
        // publishing and takes the ordinary path in gpuPresent again.
        //
        // What that path can do at this point, honestly: g_glContext is the OFFSCREEN context
        // GpuInit created for presenter mode, so it composites into a pbuffer and its
        // eglSwapBuffers is a no-op. The engine keeps loading, laying out and painting, the app
        // stays responsive and nothing deadlocks - but the screen stops updating until the session
        // is torn down and set up again. Moving a live window surface to the engine thread is not
        // possible (contexts are per-thread) and creating a second one for a panel whose surface
        // has just been lost is not either, so this is a soft landing, not a recovery. It is
        // recorded in crash.txt so a frozen screen can be told apart from a hung engine.
        const EGLint swapErr = eglGetError();
        g_presLastEglError.store(static_cast<int>(swapErr), std::memory_order_relaxed);
        if (drawCount == 1 || (drawCount % kPresStatsEvery) == 0)
            presenterStatsDump("periodic");
        if (swapErr == EGL_CONTEXT_LOST || swapErr == EGL_BAD_SURFACE || swapErr == EGL_BAD_NATIVE_WINDOW) {
            g_presenterActive.store(false);
            // Apotheosis (review 2026-09-04 item 2): hand the engine back its own present path in
            // the same breath. g_presenterRetired is consumed by the next gpuPresent (engine
            // thread) and turns it into a full, force-dirtied composite + eglSwapBuffers on
            // g_glContext; presentRequested() makes sure such a composite is actually asked for,
            // because in event-driven mode nothing else would wake the loop again. Both are
            // atomics-only, which is all this thread is allowed to touch.
            //
            // HONEST LIMIT: in presenter mode g_glContext was created with a PBUFFER surface
            // (GpuInit gave the window surface to the presenter), and a window surface cannot be
            // moved or recreated on another thread's context - ANGLE binds it to the thread that
            // created it. So the engine's eglSwapBuffers goes to an offscreen buffer and the SCREEN
            // STAYS FROZEN until the session is rebuilt. What this buys is that the engine keeps a
            // correct, fully painted frame ready (no half-dirty tree left over from the fast path),
            // that the harness' WebCorePresenterActive() poll flips back to 0 and it stops routing
            // pan to WebCoreSetPanOffset, and that the reason is in crash.txt.
            g_presenterRetired.store(1, std::memory_order_release);
            presenterStatsDump("swap-lost");
            WebCoreCrashNote(swapErr == EGL_CONTEXT_LOST
                ? "presenter: EGL_CONTEXT_LOST on swap, presenter stopped - engine presents again "
                  "(offscreen context: screen frozen until the session is rebuilt)"
                : "presenter: surface lost on swap, presenter stopped - engine presents again "
                  "(offscreen context: screen frozen until the session is rebuilt)");
            WebCorePort::presentRequested();   // atomics only: legal from this thread
            Locker locker { P.lock };
            P.stop = true;
            break;
        }
        drawnGeneration = generation;
        drawnTx = tx;
        drawnTy = ty;
        drawnAnything = true;
    }

    ctx = nullptr;              // before RoUninitialize: it releases the panel's COM references
    if (roOwned)
        RoUninitialize();
}

// Engine thread: pick the slot to composite into. Never the one that is published (the presenter
// would sample a half-drawn frame), and wait out the short window in which the presenter is issuing
// its quad from that very slot. Bounded at 50 ms so a wedged presenter can never wedge the engine.
// The presenter glFinish()es before it clears inUse, so "no longer in use" now also means "its
// quad has finished reading the texture" - no second, cross-context fence is needed (and the one
// that used to be here never ordered anything: see g_presFenceAdvertised).
// Returns -1 when the 50 ms are up and the presenter is still in the slot: there is no safe buffer
// to composite into, so the caller drops this frame rather than drawing into the one being
// sampled (which is what the old code did - it returned the slot anyway).
static int presenterAcquireSlot()
{
    PresenterState& P = *g_pres.load(std::memory_order_acquire);
    int idx = 0;
    Locker locker { P.lock };
    idx = (P.published == 0) ? 1 : 0;
    const MonotonicTime deadline = MonotonicTime::now() + Seconds::fromMilliseconds(50);
    while (P.inUse == idx) {
        if (!P.cond.waitUntil(P.lock, deadline)) {
            g_presEngineDrops.fetch_add(1, std::memory_order_relaxed);
            return -1;
        }
    }
    return idx;
}

// Engine thread: the composite into slot `idx` is issued - hand it to the presenter.
static void presenterPublish(int idx, int scrollX, int scrollY, const WebCore::Color& background)
{
    PresenterState& P = *g_pres.load(std::memory_order_acquire);
    // Apotheosis (WHITE-SCREEN fix 2026-09-04): the composite must be COMPLETE, not merely
    // submitted, before P.published names this slot - the presenter thread is on another EGL
    // context and there is no working cross-context sync on this ANGLE (see g_presFenceAdvertised).
    // glFinish() before the lock, so the presenter is not waiting on us while the GPU drains.
    glFinish();

    auto [r, g, b, a] = background.toColorTypeLossy<WebCore::SRGBA<float>>().resolved();
    Locker locker { P.lock };
    PresenterFrame& f = P.slot[idx];
    f.scrollX = scrollX;
    f.scrollY = scrollY;
    f.bg[0] = r; f.bg[1] = g; f.bg[2] = b; f.bg[3] = a;
    P.published = idx;
    ++P.generation;
    // A gesture that started before the first frame was published takes its base from this one.
    if ((P.panActive || P.panEnding) && !P.panBaseValid) {
        P.panBaseX = scrollX;
        P.panBaseY = scrollY;
        P.panBaseValid = true;
    }
    P.wake = true;
    P.cond.notifyAll();
}

// Engine thread, from WebCoreGpuInit: allocate the two render targets (the engine context must be
// current) and bring the presenter thread up. Returns false if it did not come up in time, in which
// case everything it allocated is released again and the caller falls back to the old path.
static bool presenterStart(void* nativeWindow, int w, int h)
{
    using namespace WebCore;
    g_presenterEglDisplay = PlatformDisplay::sharedDisplay().eglDisplay();
    // Apotheosis: recorded for the diagnostics only. Whatever the display advertises, we do NOT
    // use EGL fences across the two contexts any more - on ANGLE 2.1.13/D3D11 the wait returned
    // immediately and the presenter swapped half-composited (i.e. just-cleared, white) frames.
    // See the comment at g_presFenceAdvertised; both handovers are glFinish()-ordered now.
    g_presFenceAdvertised = presenterHasExtension(eglQueryString(g_presenterEglDisplay, EGL_EXTENSIONS),
        "EGL_KHR_fence_sync");

    if (g_pres.load(std::memory_order_acquire))
        return false;   // one presenter per process; a second attempt would orphan the first

    // Built locally and only published into g_pres once it is complete: from the moment the
    // pointer is visible, the presenter thread and the UI-thread exports may touch it, and from
    // that moment on it is never freed again (see the timeout below).
    std::unique_ptr<PresenterState> pres = std::make_unique<PresenterState>();
    for (int i = 0; i < 2; ++i) {
        pres->slot[i].texture = BitmapTexture::create(IntSize(w, h),
            { BitmapTexture::Flags::SupportsAlpha, BitmapTexture::Flags::DepthBuffer });
        if (!pres->slot[i].texture)
            return false;   // nothing published yet, nothing to clean up but this object
    }
    g_presenterWindow = nativeWindow;
    PresenterState& P = *pres;
    g_pres.store(pres.release(), std::memory_order_release);
    g_presenterThread = WTF::Thread::create("ApotheosisPresenter"_s, [] { presenterThreadMain(); },
        WTF::ThreadType::Graphics);   // 1 MB reserved stack (WTF stackSize(), WK_WINUWP branch)

    bool up = false;
    {
        Locker locker { P.lock };
        const MonotonicTime deadline = MonotonicTime::now() + Seconds(5);
        while (!P.initState) {
            if (!P.cond.waitUntil(P.lock, deadline))
                break;
        }
        up = (P.initState == 1);
        if (!up)
            P.stop = true;      // it leaves its loop as soon as it is able to
        P.cond.notifyAll();
    }
    if (up)
        return true;

    // Apotheosis: the timeout DETACHES the presenter, it does not tear it down. The one reason
    // initState is still 0 after five seconds is that the thread is stuck inside
    // eglCreateWindowSurface (ANGLE marshals that to the panel dispatcher, and a UI thread that is
    // busy or waiting makes it arbitrarily slow) - so the thread is alive, holds a reference to
    // this PresenterState and will still write to it. Joining it here would block the engine
    // thread for exactly as long as the thing the timeout exists to escape, and deleting the
    // state (the old code) left it writing to freed memory, as did every later
    // WebCoreSetPanOffset / WebCoreSetPresenterSuspended from the UI thread. So: leave the thread,
    // the state and the two render targets alive for the life of the process, drop our own
    // reference to the thread (WTF::Thread keeps itself alive while it runs) and fall back to the
    // engine-owned window surface. g_presenterActive stays false, so nothing is ever published
    // into those slots and the detached thread finds stop=true and exits the moment it unblocks.
    g_presenterThread = nullptr;
    return false;
}

// Apotheosis (WHITE-AT-SCROLL-END, 2026-09-04): paint the tree and report how many visible tiles
// ran out of composites to produce pixels while doing so. wkWinUWPTexmapUnpaintedVisibleTiles() is
// a level that TextureMapperTiledBackingStore::wkTrackUnpaintedVisibleTiles() bumps from inside
// paintToTextureMapper(), so the delta across this call is exactly "this frame is missing content",
// measured by the engine itself instead of guessed from a pixel sample. Costs nothing: two reads of
// a static unsigned.
static unsigned gpuPaintTree(WebCore::GraphicsLayerTextureMapper& glRoot)
{
    const unsigned before = WebCore::wkWinUWPTexmapUnpaintedVisibleTiles();
    glRoot.layer().paint(*g_textureMapper);
    return WebCore::wkWinUWPTexmapUnpaintedVisibleTiles() - before;
}

// 直呈现:把图层树合成进默认帧缓冲(GpuInit 绑的窗口表面)并 eglSwapBuffers。返回 kOK / 负错误码。
static int gpuPresent(WebCore::LocalFrameView& view, int w, int h, WebCore::GraphicsLayer& root)
{
    using namespace WebCore;
    if (!g_glContext || !g_textureMapper)
        return kErrNoView;
    g_glContext->makeContextCurrent();
    auto& glRoot = static_cast<GraphicsLayerTextureMapper&>(root);
    // Apotheosis (review 2026-09-04 item 2): the presenter retired itself since the last composite.
    // Nothing below has run on the engine's own default framebuffer since GpuInit, and the last
    // composites went into presenter slots that nobody samples any more, so this one must be a
    // full one - the fast path would draw a tree that was last force-dirtied minutes ago. Re-arm
    // needsPresent too, so the loop keeps coming back rather than judging the frame static.
    if (g_presenterRetired.exchange(0, std::memory_order_acq_rel)) {
        g_gpuForceFullNext = true;              // consumed by the gpuPrepare below
        if (g_session && g_session->chrome)
            g_session->chrome->setNeedsPresent();
    }
    gpuPrepare(view, glRoot);

    // Apotheosis (presenter thread): the window surface belongs to the presenter now. Composite
    // into one of the two offscreen render targets and publish it; the presenter decides when and
    // with which pan offset it reaches the screen. Nothing below this branch runs in that mode -
    // no default framebuffer, no g_panGesture handshake, no eglSwapBuffers on this thread.
    PresenterState* const pres = g_pres.load(std::memory_order_acquire);
    if (g_presenterActive.load() && pres) {
        const int idx = presenterAcquireSlot();
        if (idx < 0) {
            // Apotheosis: 50 ms and the presenter is still sampling the only free slot. Drop this
            // frame - the previous one stays on screen, which is right for a composite that is by
            // definition late - and ask for another present so nothing that was flushed above is
            // lost. Painting anyway would tear the frame the user is looking at.
            if (g_session && g_session->chrome)
                g_session->chrome->setNeedsPresent();
            return kOK;
        }
        BitmapTexture& target = *pres->slot[idx].texture;
        Color docBg = view.documentBackgroundColor();
        if (!docBg.isValid())
            docBg = Color::white;
        glViewport(0, 0, w, h);
        // Apotheosis: these two render targets live for the whole process, which no other
        // TextureMapper client does - everywhere else a BitmapTexture is taken from the pool,
        // which reset()s it, or is freshly created. A reused one keeps the clip stack the previous
        // paint left behind AND, because m_shouldClear is only true once, its depth and stencil
        // buffers keep the previous frame's contents while beginPainting/bindAsSurface re-enable
        // GL_DEPTH_TEST (the Flags::DepthBuffer branch) with glDepthFunc(GL_LEQUAL). reset() with
        // the same size and flags is the cheap, upstream way to say "treat this as fresh": it
        // returns early at `m_size == size` (no reallocation, same texture id, same FBO) and only
        // re-arms m_shouldClear, so bindAsSurface() clears colour+depth+stencil and resets the clip
        // stack exactly as it does for a pool texture.
        target.reset(IntSize(w, h), { BitmapTexture::Flags::SupportsAlpha, BitmapTexture::Flags::DepthBuffer });
        g_textureMapper->beginPainting(TextureMapper::FlipY::No, &target);
        g_textureMapper->clearColor(docBg);
        unsigned unpainted = 0;
        {
            PerfPhase perfPaint(&g_perfCur.paint);
            unpainted = gpuPaintTree(glRoot);
            g_textureMapper->endPainting();
        }
        g_gpuLastUnpainted = static_cast<int>(unpainted);
        // Apotheosis (WHITE-AT-SCROLL-END, device package 13): a LIGHT composite that walked over
        // visible tiles holding no pixels is not a frame, it is a background-coloured slot - which
        // is precisely what the device shows at the end of a pan: the coarse WebCoreScrollBy steps
        // move the visible rect past the painted cover, the tiles outside the keep rect are gone,
        // and the fast path (g_gpuScrollFast: no forceDirtyTree, no backing-store update) has
        // nothing to draw. The old pixel probe could not see this - the layer background IS drawn,
        // so "something was painted" was true while no content existed.
        //
        // Do what the pre-presenter path did for every frame: redraw this one in full, right here,
        // into the same slot, and publish THAT. One extra composite in the rare case, and it cannot
        // loop - a force-dirtied tree is trusted unconditionally (it repaints every backing store,
        // so tiles that draw nothing afterwards genuinely have nothing to draw).
        if (unpainted && !g_gpuLastCompositeFull) {
            g_presSkipEmpty.fetch_add(1, std::memory_order_relaxed);
            g_gpuForceFullNext = true;              // consumed by the gpuPrepare on the next line
            gpuPrepare(view, glRoot);               // force-dirty the tree + update every backing store
            docBg = view.documentBackgroundColor();
            if (!docBg.isValid())
                docBg = Color::white;
            glViewport(0, 0, w, h);
            target.reset(IntSize(w, h), { BitmapTexture::Flags::SupportsAlpha, BitmapTexture::Flags::DepthBuffer });
            g_textureMapper->beginPainting(TextureMapper::FlipY::No, &target);
            g_textureMapper->clearColor(docBg);
            {
                PerfPhase perfPaint(&g_perfCur.paint);
                glRoot.layer().paint(*g_textureMapper);
                g_textureMapper->endPainting();
            }
        }
        {
            PerfPhase perfSwap(&g_perfCur.swap);   // M4: fence + publish (the swap itself is the presenter's)
            const IntPoint scroll = view.scrollPosition();
            presenterPublish(idx, scroll.x(), scroll.y(), docBg);
        }
        return kOK;
    }

    glViewport(0, 0, w, h);
    g_textureMapper->beginPainting(TextureMapper::FlipY::No, nullptr);   // nullptr → 默认帧缓冲
    {
        Color docBg = view.documentBackgroundColor();
        g_textureMapper->clearColor(docBg.isValid() ? docBg : Color::white);   // 文档 base 背景(同 readback,见上)
    }
    unsigned unpaintedDirect = 0;
    {
        PerfPhase perfPaint(&g_perfCur.paint);      // M4: TextureMapper composite into the default framebuffer
        unpaintedDirect = gpuPaintTree(glRoot);
        g_textureMapper->endPainting();
    }
    g_gpuLastUnpainted = static_cast<int>(unpaintedDirect);
    // Apotheosis (WHITE-AT-SCROLL-END, 2026-09-04): the same rule as in the presenter branch above,
    // and for the same reason - a coarse WebCoreScrollBy step can move the visible rect past the
    // painted cover, and the scroll fast path has no way to paint the tiles that were dropped. Here
    // the back buffer is not published but swapped, so the repair has to happen before the swap:
    // redo the composite in full (force-dirty + backing-store update, i.e. what this path always did
    // before g_gpuScrollFast existed) and swap THAT. Cannot loop: a full composite is never redone.
    if (unpaintedDirect && !g_gpuLastCompositeFull) {
        g_presSkipEmpty.fetch_add(1, std::memory_order_relaxed);
        g_gpuForceFullNext = true;                  // consumed by the gpuPrepare on the next line
        gpuPrepare(view, glRoot);
        glViewport(0, 0, w, h);
        g_textureMapper->beginPainting(TextureMapper::FlipY::No, nullptr);
        {
            Color docBg = view.documentBackgroundColor();
            g_textureMapper->clearColor(docBg.isValid() ? docBg : Color::white);
        }
        {
            PerfPhase perfPaint(&g_perfCur.paint);
            glRoot.layer().paint(*g_textureMapper);
            g_textureMapper->endPainting();
        }
    }
    // Apotheosis (pan present handshake): the frame is composited into the back buffer, but during
    // a pan gesture the harness decides WHEN it becomes visible - it must first put the matching
    // TranslateTransform on screen, or the two disagree for a frame. WebCorePresent() does the swap.
    if (g_panGesture) {
        g_swapOwed = true;
        // Apotheosis (stale deferred swap): remember which scroll this frame is showing, so
        // WebCorePresent() can tell "the frame the harness is acknowledging" from "a frame the
        // engine has already moved past". See the block at g_scrollGen.
        g_swapOwedGen = g_scrollGen;
        g_swapOwedId = g_swapIdNext++;   // identity the harness acknowledges (WebCorePresentFrame)
        const IntPoint owedScroll = view.scrollPosition();
        g_swapOwedScrollX = owedScroll.x();
        g_swapOwedScrollY = owedScroll.y();
        return kOK;
    }
    {
        PerfPhase perfSwap(&g_perfCur.swap);        // M4: eglSwapBuffers → SwapChainPanel
        g_glContext->swapBuffers();
    }
    return kOK;
}

// Apotheosis (OFFTHREAD-RASTER-LOG.md §4.2/§4.3): call right after a composite. With threaded
// raster on, a replay that finishes after this composite has no one to upload it — the tile would
// keep its old pixels until the page happens to be dirtied again. Arming m_needsPresent makes the
// harness' next live tick composite (and the tick's own peekNeedsPresent() fast-path check take the
// heavy branch), where wkFinishPendingPaints() picks the finished buffers up. Bumping the frame
// hash keeps the live loop from judging the frame static and stopping before that tick happens.
// The count also becomes the perf row's raster_pending column, which is the only way to see
// whether the worker pool kept up. No-op (and no counter read) while the feature is off.
static void notePendingRasterTiles()
{
    if (!WebCore::wkWinUWPThreadedRaster())
        return;
    // Both are needed. `pending` covers "a replay is still running" — one more composite will be
    // owed. `finished` (edge-triggered, reading resets) covers the replay that started AND ended
    // between two composites, which leaves `pending` at zero although nothing has uploaded the new
    // pixels yet. With step 4's asynchronous first paints (OFFTHREAD-RASTER-LOG.md §10) that case
    // is an *empty* tile on screen, not a stale one, so it is the more important of the two.
    const unsigned pending = WebCore::wkWinUWPTexmapPendingRasterTiles();
    const unsigned finished = WebCore::wkWinUWPTexmapTakeFinishedRasterTiles();
    if (g_perfOn) {
        g_perfCur.rasterPending = static_cast<int>(pending);
        g_perfCur.rasterDone = static_cast<int>(finished);
        unsigned posted = 0, cancelled = 0, blocking = 0;
        WebCore::wkWinUWPTexmapRasterStats(posted, cancelled, blocking);
        g_perfCur.rasterPosted = static_cast<int>(posted);
        g_perfCur.rasterCancelled = static_cast<int>(cancelled);
        g_perfCur.rasterBlocked = static_cast<int>(blocking);
    }
    if (!pending && !finished)
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
            // 每 4 像素采样进哈希(原 16px 网格太疏,漏掉 Bing 小加载圈等小动画 → 误判静止停帧;
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
    g_gpuLastCompositeFull = false;
    g_gpuLastUnpainted = 0;
    navLoadEnd();   // Apotheosis (M4 load throttle): the load this belonged to is gone
    // Apotheosis (pan present handshake): the pan belonged to the page that is going away. Drop
    // the deferral (a new session must present normally) and the swap it may still owe - the back
    // buffer is about to be redrawn by the next session anyway.
    g_panGesture = false;
    g_swapOwed = false;
    // Apotheosis (stale deferred swap): the scroll ledger belonged to the page that is going away.
    ++g_scrollGen;
    g_swapOwedGen = g_scrollGen;
    g_swapOwedId = 0;   // no frame is owed: a late ack from the old page must match nothing
    g_swapOwedScrollX = g_swapOwedScrollY = 0;
    // Apotheosis (presenter thread): the frames in the presenter's slots were composited from the
    // layer tree that is about to be destroyed. Mark them stale, or the 8 ms pan heartbeat (and
    // any pan offset that arrives before the next session publishes) would keep re-swapping a
    // closed tab's last frame. Nothing is freed here - the textures are process-lifetime and the
    // presenter may still be sampling one; it simply has nothing to show until the next publish,
    // and what is already on the swap chain stays there. Its pan state belonged to that page too.
    if (PresenterState* pres = g_pres.load(std::memory_order_acquire)) {
        Locker locker { pres->lock };
        pres->published = -1;
        pres->panActive = false;
        pres->panEnding = false;
        pres->panBaseValid = false;
        pres->panX = pres->panY = 0.0f;
        pres->wake = true;
        pres->cond.notifyAll();
    }
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

    // 初次加载:等主文档完成 + 空闲;每 tick isolatedUpdateRendering 让 SPA(claude.ai 等)的
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
        g_perfPath.clear();
        return;
    }
    g_perfPath = path;
    g_perfOn = true;
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
// pressed. Widgets that pan on pointer events (Google Maps, Leaflet, canvas apps) test exactly
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

// 在 (x,y)(位图/视口像素,无需减 scroll —— EventHandler 内部 windowToContents 会加 scrollY)派发一次
// 完整鼠标点击 move→down→up 到活文档,经真实命中测试 + 默认动作(链接导航 / 表单提交 / 按钮 onclick /
// SPA 交互)。之后等待可能的异步导航 settle、每 tick 驱动 rAF,然后重布局/提链接/重绘。返回 0 成功。
int WebCoreClickAt(int x, int y, uint8_t* outRGBA)
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
    if (lf->eventHandler().mousePressed()) {
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
    DriverMouseEvent down(p, MouseButton::Left, PlatformEvent::Type::MousePressed, 1, mods, t, kButtonsLeftDown);
    lf->eventHandler().handleMousePressEvent(down);    // 安装 UserGestureIndicator
    DriverMouseEvent up(p, MouseButton::Left, PlatformEvent::Type::MouseReleased, 1, mods, MonotonicTime::now(), 0);
    lf->eventHandler().handleMouseReleaseEvent(up);    // 派发 DOM 'click' + 默认动作(导航/提交)

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
    // Apotheosis (stale deferred swap): this job's composite, further down, is the only frame that
    // shows this scroll position - anything composited before this line is now out of date. See
    // the block at g_scrollGen.
    ++g_scrollGen;

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

// Apotheosis (OFFTHREAD-RASTER-LOG.md §4.1): flip off-thread tile rasterisation. The engine-side
// switch is a plain file-static bool read once per tile update, so this is safe between composites
// and needs no teardown when it goes off (in-flight replays are still collected by
// wkFinishPendingPaints/the 4-composite deadline).
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

void WebCoreSetThreadedRaster(int enabled)
{
    // The completion handler is installed together with the feature (and removed with it) so that
    // no worker can call into a driver that has stopped expecting it. Both calls are engine thread.
    WebCore::wkWinUWPSetRasterCompletionHandler(enabled ? &rasterCompletedOnWorker : nullptr);
    WebCore::wkWinUWPSetThreadedRaster(enabled != 0);
}

// Apotheosis (WHITE-AT-SCROLL-END, 2026-09-04): stale tiles on/off. ON (the default) a backing
// store keeps the tiles it drops out of its cover rect and keeps drawing them - scaled to the
// current content rect - until real ones have been rasterised, so a fast-path composite whose tiles
// have moved on paints the old pixels instead of nothing. OFF is the pre-2026-09-04 behaviour and
// exists so the device can A/B the two without a rebuild. Takes effect from the next composite.
// Engine thread only.
void WebCoreSetStaleTiles(int enabled)
{
    WebCore::wkWinUWPSetStaleTiles(enabled != 0);
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
int WebCoreWheelAt(int x, int y, float deltaX, float deltaY, int phase, uint8_t* outRGBA)
{
    using namespace WebCore;
    (void)phase;   // see comment above: inert on this port (no ASYNC/KINETIC scrolling, no phase setter)
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
    PlatformWheelEvent wheelEvent(p, p, deltaX, deltaY, deltaX, deltaY,
        PlatformWheelEventGranularity::ScrollByPixelWheelEvent,
        /* shiftKey */ false, /* ctrlKey */ false, /* altKey */ false, /* metaKey */ false);
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
    const bool consumedByNested = result.wasHandled()
        && !handling.contains(EventHandling::DefaultPrevented)
        && (afterMain == beforeMain);

    if (!consumedByNested && afterMain != beforeMain)
        view->setScrollPosition(beforeMain);   // undo any main-frame move: that is WebCoreScrollBy's job

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

// ===========================================================================
// Apotheosis (drag as pointer events): map widgets — Google Maps, OpenStreetMap /
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
// Leaflet and Google Maps get the events they actually listen for).
// ===========================================================================

// Hit test only, no event dispatched — same shape as WebCoreIsScrollableAt above
// (pump guard, layout, elementFromPoint, open-shadow descent, walk up the element
// chain). Returns 1 when the element under (x,y) or one of its ancestors looks
// like it handles dragging itself:
//   * a JS listener for pointerdown / mousedown / touchstart / pointermove /
//     touchmove (Leaflet, Google Maps, OpenLayers, MapLibre and every canvas app
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
//     .leaflet-container, .maplibregl-canvas and the Google Maps root all set.
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
// way to suppress horizontal overscroll - ntv.de has one) is exactly what stole every vertical pan
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
// default action took the press, and the widgets this export exists for do neither: Google Maps
// listens for pointerdown, stores the anchor and lets the event through. On device every press on
// maps.google.com answered `handled=0`, so the gesture was handed back before a single mousemove was
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
// the only writable path the driver knows. Same plain-line channel as presenter-stats/pan-swap-drop
// (no crash record, no crash-entry budget), capped so a session of panning cannot fill the file.
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
        // Apotheosis (Google Maps, 2026-09-04): ask the SAME question WebCoreWantsDragAt asked, on
        // the same point and the layout we just updated, BEFORE dispatching - the press itself can
        // run script that changes the tree. See the decision below.
        const bool wants = dragWidgetAtPoint(*doc, x, y);
        // Hover first, exactly as WebCoreClickAt does: it sets elementUnderMouse/:hover, which is
        // what several widgets key their pointerdown handling off.
        DriverMouseEvent hover(p, MouseButton::None, PlatformEvent::Type::MouseMoved, 0, mods, t, 0);
        lf->eventHandler().handleMouseMoveEvent(hover);
        DriverMouseEvent down(p, MouseButton::Left, PlatformEvent::Type::MousePressed, 1, mods, t, kButtonsLeftDown);
        handled = lf->eventHandler().handleMousePressEvent(down).wasHandled();
        // Apotheosis (Google Maps, 2026-09-04): "the press was handled" was the WRONG criterion for
        // owning the gesture. handleMousePressEvent() reports handled only when a listener called
        // preventDefault() or a WebCore default action took the press - and a map does neither: it
        // listens for pointerdown, records the anchor and returns without preventing anything
        // (preventing it would break its own click handling). Device evidence: every single
        // drag-press line on maps.google.com read `handled=0 unwound=1`, so not one mousemove was
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
        handled = true;   // see the header comment: the press decided, per-move results are noise
    } else {
        // 2 = release, 3 = cancel (and any unknown phase): both must end with a mouseup, otherwise
        // EventHandler keeps m_mousePressed set and every later hover/tap behaves like a drag.
        // buttons=0: the button being released is no longer down (WM_LBUTTONUP does the same), and
        // a mouseup that still claimed a pressed button would be turned into a pointermove by the
        // chorded-button rules (PointerCaptureController.cpp:432) - no pointerup, pointer stays
        // pressed for ever. A cancel additionally drops the click: an aborted gesture must not
        // activate what happens to be under the finger.
        if (phase != 2)
            lf->eventHandler().invalidateClick();
        DriverMouseEvent up(p, MouseButton::Left, PlatformEvent::Type::MouseReleased, phase == 2 ? 1 : 0, mods, t, 0);
        lf->eventHandler().handleMouseReleaseEvent(up);
        g_dragActive = false;
        handled = true;
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

    // Apotheosis (2026-09-04): the presenter's pan base is about to become meaningless - the scale
    // and the scroll position it was measured against both change on the next line. See
    // presenterResetPan(); without this the residual of the pan that led into the pinch keeps
    // moving the zoomed frame around until its one second deadline expires.
    presenterResetPan();
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
// Accepts a full URL or a bare host ("ntv.de"); anything else is ignored. Idempotent and cheap
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
    PlatformDisplay& display = PlatformDisplay::sharedDisplay();   // WIN → PlatformDisplayWin,起 ANGLE EGLDisplay

    // Apotheosis (presenter thread): try the split first - engine context offscreen, window surface
    // owned by a presenter thread. Both contexts are created through PlatformDisplay, so both share
    // with PlatformDisplay::sharingGLContext() and the engine's render targets are visible to the
    // presenter. The engine context is created (and thereby the sharing context) BEFORE the
    // presenter thread starts, so nothing races over sharingGLContext()'s lazy construction.
    // Any failure falls through to the original engine-owned window surface below.
    if (nativeWindow && g_presenterWanted) {
        std::unique_ptr<GLContext> engineCtx = GLContext::createOffscreen(display);
        if (engineCtx && engineCtx->makeContextCurrent()) {
            std::unique_ptr<TextureMapper> tm = TextureMapper::create();
            if (tm && presenterStart(nativeWindow, w, h)) {
                g_glContext = engineCtx.release();
                g_textureMapper = tm.release();
                g_gpuW = w;
                g_gpuH = h;
                g_gpuPresentMode = true;
                g_presenterActive.store(true);
                g_gpuActive = true;
                return kOK;
            }
        }
        // ~GLContext does eglMakeCurrent(none) + eglDestroyContext on this thread - correct here,
        // this is the thread that created it and nothing has used it yet.
    }

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

// Apotheosis (pan present handshake): tell the engine that a main-frame pan gesture owns the
// screen. While it does, nothing presents on its own initiative:
//   - gpuPresent() composites into the back buffer but defers the eglSwapBuffers (g_swapOwed),
//   - WebCoreLiveTick() skips its composite altogether, which also leaves the engine thread free
//     for the harness' scroll jobs on a heavy page (github),
// and the harness releases each swap with WebCorePresent() at the moment its own
// TranslateTransform for that scroll position has been committed by XAML. Anything a wake-up,
// timer, rAF or image decode changes in the meantime still happens - it simply becomes visible
// with the next scroll present instead of racing the transform.
// Engine thread only (post it like every other export); active=0 hands presents back and asks for
// one full composite, because the ticks that ran during the gesture produced no pixels.
void WebCoreSetPanGesture(int active)
{
    // Apotheosis (presenter thread): the presenter owns the swap chain, there is no swap on this
    // thread to defer and no XAML transform to stay in step with - but the END of a gesture still
    // means something here (2026-09-04, device package 13). Every composite during the pan took the
    // scroll fast path, so the last frame of the gesture is drawn from whatever tiles survived the
    // coarse steps; the first frame after it must be a FULL one, or the page settles on a
    // background-coloured slot. The harness posts this at PanGestureEnd in presenter mode too.
    if (g_presenterActive.load()) {
        if (!active) {
            g_gpuForceFullNext = true;
            if (g_session && g_session->chrome)
                g_session->chrome->setNeedsPresent();
            WebCorePort::presentRequested();
        }
        return;
    }
    const bool on = (active != 0);
    if (g_panGesture == on)
        return;
    g_panGesture = on;
    if (!on && g_session && g_session->chrome) {
        // Apotheosis (WHITE-AT-SCROLL-END, 2026-09-04): and make that composite a FULL one - see
        // the presenter branch above, the reasoning is identical for the window-surface path.
        g_gpuForceFullNext = true;
        g_session->chrome->setNeedsPresent();
        // Apotheosis (XAML-path consistency review, 2026-09-04): and ASK for that composite. Every
        // tick during the gesture disarmed the present wake at its top (presentWakeDisarm) and then
        // returned before the `peekNeedsPresent -> presentRequested` re-arm at the bottom, so in
        // event-driven mode the loop is idle here and setNeedsPresent alone is a flag nobody reads.
        // Without this the page stays on the last gesture frame until the user touches it again -
        // "the flicker comes back a few seconds later" is partly this: the frame on screen is stale
        // and the next unrelated wake finally replaces it in one jump.
        WebCorePort::presentRequested();
    }
}

// Apotheosis (pan present handshake): perform the swap a composite deferred while g_panGesture was
// set. Cheap and idempotent - with nothing owed it does nothing, so the harness may post it freely
// (it also flushes a swap left over after the gesture ended). Engine thread only.
static int presentOwedSwap(uint64_t ackId)
{
    if (g_presenterActive.load())
        return kOK;   // Apotheosis (presenter thread): nothing is ever owed on this thread
    if (!g_swapOwed)
        return kOK;
    // Apotheosis (XAML-path consistency review, 2026-09-04): an acknowledgement that names a frame
    // other than the one in the back buffer is not this frame's acknowledgement. Releasing it puts
    // content on screen under a translation that was committed for a different scroll position -
    // the artefact this whole handshake exists to prevent, only in the other direction. Drop the
    // ACK, not the frame: g_swapOwed stays set, so the composite is still released by the
    // acknowledgement that does belong to it (or by the WebCoreLiveTick / WebCoreSetPanGesture(0)
    // safety nets once the gesture is over). ackId 0 = "whatever is owed", the legacy
    // WebCorePresent() contract.
    if (ackId && ackId != g_swapOwedId)
        return kOK;
    if (!g_gpuActive || !g_gpuPresentMode || !g_glContext) {
        g_swapOwed = false;
        return kOK;
    }
    // Apotheosis (stale deferred swap, 2026-09-04): only release a frame that still belongs to the
    // newest scroll the engine has applied. If a newer WebCoreScrollBy has already run, this
    // acknowledgement belongs to a job the engine has moved past: its composite shows the OLD
    // scroll position, while the harness' translation has already been reduced by the newer delta,
    // and swapping it puts the content back where the gesture came from for one frame. Drop it
    // instead - the back buffer is about to be redrawn anyway - and arm a present so the next
    // composite (at the current position) reaches the screen without waiting for a wake-up.
    if (g_swapOwedGen != g_scrollGen) {
        g_swapOwed = false;
        int nowX = 0, nowY = 0;
        if (g_session && g_session->mainFrame) {
            if (WebCore::LocalFrameView* v = g_session->mainFrame->view()) {
                nowX = v->scrollPosition().x();
                nowY = v->scrollPosition().y();
            }
        }
        panSwapDropNote(nowX, nowY);
        if (g_session && g_session->chrome)
            g_session->chrome->setNeedsPresent();
        // Apotheosis: setNeedsPresent() alone is a flag nobody polls in event-driven mode - the
        // tick that would notice it is exactly the one that returns early while g_panGesture is
        // set. Arm a real wake so the replacement composite happens.
        WebCorePort::presentRequested();
        return kOK;
    }
    g_swapOwed = false;
    g_glContext->makeContextCurrent();
    g_glContext->swapBuffers();
    return kOK;
}

int WebCorePresent()
{
    return presentOwedSwap(0);
}

// Apotheosis (XAML-path consistency review, 2026-09-04): release the owed swap only if it is still
// the frame `swapId` names (an id from WebCoreGetOwedSwapScroll). Anything else is left owed - see
// presentOwedSwap(). swapId 0 behaves exactly like WebCorePresent(). Engine thread only.
int WebCorePresentFrame(unsigned long long swapId)
{
    return presentOwedSwap(static_cast<uint64_t>(swapId));
}

// Apotheosis (XAML-path consistency review, 2026-09-04): what the deferred frame in the back buffer
// actually shows. Called from the same engine hop as the WebCoreScrollBy it belongs to (next to
// WebCoreGetScrollState), it lets the harness build the pan translation from the FRAME's own scroll
// position - offset - (swapScroll - gestureStartScroll) - instead of from "wherever the engine
// happens to be now", which is what made a coarse engine step visible as a jump. The id comes back
// with it and is handed to WebCorePresentFrame() when XAML has committed that translation.
// Returns 1 when a swap is owed, 0 otherwise (also whenever the presenter thread owns the swap
// chain - nothing is ever owed there). Cheap: no layout, no paint. Engine thread only.
int WebCoreGetOwedSwapScroll(int* outScrollX, int* outScrollY, unsigned long long* outSwapId)
{
    const bool owed = g_swapOwed && !g_presenterActive.load();
    if (outScrollX) *outScrollX = owed ? g_swapOwedScrollX : 0;
    if (outScrollY) *outScrollY = owed ? g_swapOwedScrollY : 0;
    if (outSwapId)  *outSwapId  = owed ? static_cast<unsigned long long>(g_swapOwedId) : 0ull;
    return owed ? 1 : 0;
}

// Apotheosis (presenter thread): pick the presentation model. ON = a presenter thread owns the
// swap chain; OFF (the default since the 2026-09-04 review) = exactly the behaviour before it
// existed, the engine thread presents into the window surface itself and the harness does its pan
// preview with a XAML transform.
// Read once, inside WebCoreGpuInit - flipping it afterwards cannot move a live EGL window surface
// between threads, so the harness persists it and it applies at the next start.
void WebCoreSetPresenterThread(int enabled)
{
    g_presenterWanted = (enabled != 0);
}

// Apotheosis (presenter thread): did the split actually come up? The harness routes its pan either
// to WebCoreSetPanOffset (1) or to its own XAML transform (0) on the strength of this, so it must
// be asked after WebCoreGpuInit rather than assumed from the setting - GpuInit falls back silently.
int WebCorePresenterActive(void)
{
    return g_presenterActive.load() ? 1 : 0;
}

// Apotheosis (presenter thread): where the finger is, in engine px, accumulated since the gesture
// began (NOT a delta). ★ THE ONE EXPORT THAT IS CALLED FROM THE UI THREAD ★ - it touches nothing
// but the presenter's own state behind one uncontended lock, never WebCore, and never blocks, so
// the 线程铁律 holds: no engine hop, no waiting, at ManipulationDelta rate.
//   gestureActive != 0 : the finger (or its inertia) is driving this offset.
//   gestureActive == 0, offset != 0 : the gesture ended - the presenter keeps showing the residual
//       and lets it shrink as the engine catches up, snapping to zero after at most a second.
//   gestureActive == 0, offset == 0 : hard reset (a pinch takes over, a navigation, the setting
//       being switched off) - the next frame is drawn untranslated.
// No-op when the presenter is not running, so the harness may call it unconditionally.
void WebCoreSetPanOffset(float x, float y, int gestureActive)
{
    PresenterState* const pres = g_pres.load(std::memory_order_acquire);
    if (!pres)
        return;
    PresenterState& P = *pres;
    Locker locker { P.lock };
    const bool on = (gestureActive != 0);
    if (!on && x == 0.0f && y == 0.0f) {
        P.panActive = false;
        P.panEnding = false;
        P.panBaseValid = false;
        P.panX = P.panY = 0.0f;
    } else if (on) {
        if (!P.panActive) {
            // The gesture starts from the frame that is on screen: that frame's scroll position is
            // the zero point every later residual is measured against.
            if (P.published >= 0) {
                P.panBaseX = P.slot[P.published].scrollX;
                P.panBaseY = P.slot[P.published].scrollY;
                P.panBaseValid = true;
            } else
                P.panBaseValid = false;   // presenterPublish latches it on the first frame instead
        }
        P.panActive = true;
        P.panEnding = false;
        P.panX = x;
        P.panY = y;
    } else {
        if (P.panActive || P.panEnding) {
            P.panEnding = true;
            P.panDeadline = MonotonicTime::now() + Seconds(1);
        }
        P.panActive = false;
        P.panX = x;
        P.panY = y;
    }
    P.wake = true;
    P.cond.notifyAll();
}

// Apotheosis (presenter thread): stop/resume the presenter's swaps around app suspend. UWP freezes
// every thread once the suspend deferral completes, but between the Suspending/VisibilityChanged
// event and that moment the presenter would happily keep swapping a swap chain the shell is tearing
// down. UI thread callable, same contract as WebCoreSetPanOffset.
void WebCoreSetPresenterSuspended(int suspended)
{
    PresenterState* const pres = g_pres.load(std::memory_order_acquire);
    if (!pres)
        return;
    PresenterState& P = *pres;
    Locker locker { P.lock };
    P.suspended = (suspended != 0);
    P.wake = true;
    P.cond.notifyAll();
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
    IntPoint origScroll;
    bool didProbe = false;
    if (view) {
        IntPoint sp = view->scrollPosition();
        origScroll = sp;
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
        // 探针滚动:setScrollPosition(0,300)+frameViewDidScroll,看 ① 滚动量是否被钳到 0(maxScroll=0?)
        // ② scrolled-contents 层是否真移到 (0,-300)。其后的 layerTreeAsText 即反映探针后的层位置。最后复位。
        view->setScrollPosition(ScrollPosition(0, 300));
        if (auto* rv = view->renderView())
            rv->compositor().frameViewDidScroll();
        IntPoint sp2 = view->scrollPosition();
        char h2[160];
        snprintf(h2, sizeof h2, "-- after setScrollPosition(0,300)+frameViewDidScroll: scrollPos=%d,%d (层树为此刻状态) --\n",
                 sp2.x(), sp2.y());
        s += h2;
        didProbe = true;
    }
    if (GraphicsLayer* root = g_session->chrome->rootLayer()) {
        String tree = root->layerTreeAsText(AllLayerTreeAsTextOptions);   // 全调试标志:paintsIntoWindow/tileCache/drawsContent/backingStoreAttached
        CString u = tree.utf8();
        s.append(u.data(), u.length());
    } else {
        s += "(no root GraphicsLayer)\n";
    }
    if (didProbe && view) {   // 复位滚动,别让调试 tap 把页面留在 300
        view->setScrollPosition(origScroll);
        if (auto* rv = view->renderView())
            rv->compositor().frameViewDidScroll();
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

    // Apotheosis (pan present handshake): safety net for an acknowledgement that never came back
    // (a lost RunAsync, the app going to the background mid-gesture). The gesture is over, so a
    // composited-but-unswapped frame must not stay invisible.
    if (!g_panGesture && g_swapOwed)
        WebCorePresent();

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
    // Apotheosis (pan present handshake): a pan gesture owns the screen. Everything above (rAF, WTF
    // timers, finished decodes, style/layout) has run and stays in the tree; only the composite is
    // skipped, because the sole thing allowed to reach the panel during a gesture is a present the
    // harness asked for. That also leaves the engine thread to the scroll jobs, which is what the
    // finger actually sees. needsPresent stays set (nothing consumed takeNeedsPresent), so the
    // first composite after the gesture is a full one.
    if (g_panGesture)
        return kOK;
    // Apotheosis (M4 load throttle): same idea while the main document loads. Everything above has
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
    // Experiment F (2026-09-02): on github.com not one of 84 ticks took the fast
    // path - the page requests a rendering update every tick, so each tick still
    // re-rasterised the whole tree (0.8-1.1 s). Scroll frames prove the retained
    // tiles are correct: take the fast path on every tick. WebCore's own dirty
    // rects (setNeedsDisplayInRect via RenderLayerBacking) still repaint what
    // changed; navigation/click/type keep the full force-dirty composite.
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
