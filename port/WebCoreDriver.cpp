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
#include <csignal>       // Apotheosis: signal(SIGABRT) leg of the crash.txt logger
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
#include <WebCore/CookieJar.h>           // WebCore::CookieJar(cookie 持久化)
#include <WebCore/NetworkStorageSession.h>   // deleteAllCookies(WebCoreClearCookies)
#include <WebCore/StorageSessionProvider.h>  // 完整类型(Ref<StorageSessionProvider> 析构需要)
#include "PortNetworkStorageSession.h"   // WebCorePort::makeStorageSessionProvider / ensureDefaultPortStorageSession
#include "PortChromeClient.h"            // WebCorePort::PortChromeClient(开合成,捕获根图层)
#include <WebCore/Page.h>                // WebCore::Page
#include <WebCore/Settings.h>            // Page::settings()
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

// Run the WebCore one-time process initialization exactly once.
// Sequence taken from Source/WebKit/Shared/WebKit2Initialize.cpp
// (the !PLATFORM(COCOA) branch — our case).
bool ensureWebCoreInitialized()
{
    static bool initialized = [] {
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
        WebCore::MemoryCache::singleton().setCapacities(0, 8u * 1024 * 1024, 16u * 1024 * 1024);
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
static bool g_gpuPresentMode = false; // WebCoreGpuInit 收到窗口表面=true → 各帧直呈现到 SwapChainPanel(省 readback)
static bool g_gpuAnimating = false;   // 最近一次合成时图层树仍有动画在跑(applyAnimationsRecursively 返回值),直呈现模式的帧变化信号之一

// 子资源加载诊断计数(主文档 + CSS/JS/图片全经 ResourceHandle 桥)。由 ResourceHandle.cpp
// 的 WebCorePortBumpLoad 累加;在 WebCoreLoadUrl 开头清零,结束并入 g_lastDiag,真机定位"子资源不加载"。
static int g_loadStarted = 0, g_loadResponse = 0, g_loadComplete = 0, g_loadFail = 0;
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
    return EXCEPTION_CONTINUE_SEARCH;
}

// (c) abort() tail. Returns; the CRT then fast-fails as usual.
static void __cdecl crashLogSignalHandler(int sig)
{
    char reason[64];
    std::snprintf(reason, sizeof(reason), "signal %d (SIGABRT)", sig);
    crashLogWrite(reason, nullptr);
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
static bool g_perfOn = false;
static bool g_perfHeaderDone = false;

// One physical line; the column order must stay in sync with perfFlush()'s fprintf.
static const char* const kPerfHeader =
    "seq,kind,url,ms_total,ms_net_commit,ms_net_load,ms_settle,ms_style_layout,ms_render_update,"
    "ms_flush,ms_backing,ms_paint,ms_readback,ms_swap,ms_blit,frames,subres_started,subres_ok,"
    "subres_fail,gpu,dfg,w,h\n";

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
static void perfFlush()
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
        std::fprintf(fp, "%u,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%d,%d,%d,%d\n",
            r.seq, r.kind, r.url,
            d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11],
            n[0], n[1], n[2], n[3], r.gpu, r.dfg, r.w, r.h);
    }
    std::fclose(fp);
    g_perfRows = 0;
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
    } else
        g_perfCur.frames = 1;
    g_perfCur.gpu = g_gpuActive ? 1 : 0;
    g_perfCur.dfg = 0;   // hardcoded: becomes a build flag once ENABLE_DFG_JIT=ON is measured (M4 §4)
    if (g_perfCur.w <= 0 && g_session) {            // viewport of the live session
        g_perfCur.w = g_session->w;
        g_perfCur.h = g_session->h;
    }
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

// Navigation phase marks — LoadingFrameLoaderClient is the only observer of these
// boundaries. Declared in PortPerf.h; first mark of an operation wins (a redirect
// chain keeps the timestamps of the load the user actually asked for).
namespace WebCorePort {

void perfNavStart()
{
    if (!g_perfOn || !g_perfInOp || g_perfNavT0Set)
        return;
    g_perfNavT0 = MonotonicTime::now();
    g_perfNavT0Set = true;
}

void perfNavCommit()
{
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
    if (!g_perfOn || !g_perfInOp || g_perfCur.netLoad >= 0)
        return;
    g_perfCur.netLoad = perfSinceNavStart();
}

} // namespace WebCorePort

// 轮询 RunLoop 直到活动文档空闲(涵盖图片/脚本/XHR)或封顶。timers 为调用局部量,返回前销毁。
//  mainDone: 指向"主文档已完成"标志的指针(可空 → 无导航语义,只看加载活动)。
//  allowEarlyStopWithoutNav: 若未发生导航完成,连续 ~0.5s 无加载活动即停(点击/滚动用)。
//  settleCapTicks: 导航完成后的最大额外轮询数(×50ms)。
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
            bool ready = navDone || allowEarlyStopWithoutNav;

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
            if (ready && quietTicks >= 16) {            // 加载器静默 ~0.8s → 异步级联已跑完,停
                stopLoop();
                return;
            }
            if (navDone && settleTicks > settleCapTicks)  // 硬封顶(8s),防长连接/永久活动拖到看门狗
                stopLoop();
        } });
    settle.startRepeating(0.05_s);
    RunLoop::Timer watchdog(Ref { RunLoop::currentSingleton() }, "WebCorePort.pump.watchdog"_s,
        WTF::Function<void()> { [&stopLoop] { stopLoop(); } });
    watchdog.startOneShot(WTF::Seconds(watchdogSeconds));
    RunLoop::run();
    settle.stop();
    watchdog.stop();
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
        view.updateLayoutAndStyleIfNeededRecursive();
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
        if (!g_gpuScrollFast)
            forceDirtyTree(glRoot);                                     // 强制全树标脏,否则脏区已被消费 → 内容 tile 空
        g_gpuScrollFast = false;
        glRoot.updateBackingStoreIncludingSubLayers(*g_textureMapper);  // 上传脏 tile 内容到 GL 纹理(递归)
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
        glRoot.layer().paint(*g_textureMapper);
        g_textureMapper->endPainting();
    }
    {
        PerfPhase perfSwap(&g_perfCur.swap);        // M4: eglSwapBuffers → SwapChainPanel
        g_glContext->swapBuffers();
    }
    return kOK;
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
                    return kOK;
                }
            } else if (gpuCompositeReadback(view, w, h, *root, outRGBA, nonWhiteOut) == kOK)
                return kOK;
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
    // ★ DOM Storage:Window.localStorage/sessionStorage 默认被 LocalStorageEnabled/SessionStorageEnabled
    //   两个 setting 门控,默认关 → 这两个全局根本没挂上 window → 现代 SPA 启动时访问 localStorage 直接
    //   ReferenceError("Can't find variable: localStorage")崩溃,React 永不挂载(白屏)。开了它们才行。
    page->settings().setLocalStorageEnabled(true);
    page->settings().setSessionStorageEnabled(true);
#if ENABLE(VIDEO)
    page->settings().setMediaEnabled(false);
#endif
    page->setIsVisible(true);

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

    if (!g_session->load.mainDone)
        return kErrLoadTimeout;
    if (g_session->load.failed)
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

extern "C" void WebCorePortRecordNetError(int code, const char* domain, const char* desc, const char* url)
{
    std::snprintf(g_lastNetError, sizeof g_lastNetError,
        "curlcode=%d domain=%s desc=%s url=%s",
        code, domain ? domain : "", desc ? desc : "", url ? url : "");
}

// Apotheosis: 内存压力释放。harness 监听 UWP MemoryManager.AppMemoryUsageIncreased,
// 到 High/OverLimit 时经引擎线程调本函数 → 一把清资源缓存 + 后退页面缓存 + JSC GC + 字体缓存。
// critical: 1=严重(连活资源解码数据也丢);0=温和。须在引擎线程调(C ABI 已串行化)。
extern "C" void WebCoreReleaseMemory(int critical)
{
    if (!ensureWebCoreInitialized()) return;
    WebCore::releaseMemory(critical ? WTF::Critical::Yes : WTF::Critical::No,
                           WTF::Synchronous::Yes,
                           WebCore::MaintainBackForwardCache::No,
                           WebCore::MaintainMemoryCache::No);
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

    if (g_crashLogInstalled)
        return;
    g_crashLogInstalled = true;
    WTFWinUWPSetCrashHook(&crashLogWtfHook);
    AddVectoredExceptionHandler(1, &crashLogVectoredHandler);
    std::signal(SIGABRT, &crashLogSignalHandler);
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
    teardownSession();
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
    PlatformMouseEvent move(p, p, MouseButton::None, PlatformEvent::Type::MouseMoved, 0, mods, t, 0.0, SyntheticClickType::NoTap);
    lf->eventHandler().handleMouseMoveEvent(move);     // 设 :hover / elementUnderMouse
    PlatformMouseEvent down(p, p, MouseButton::Left, PlatformEvent::Type::MousePressed, 1, mods, t, 0.0, SyntheticClickType::NoTap);
    lf->eventHandler().handleMousePressEvent(down);    // 安装 UserGestureIndicator
    PlatformMouseEvent up(p, p, MouseButton::Left, PlatformEvent::Type::MouseReleased, 1, mods, MonotonicTime::now(), 0.0, SyntheticClickType::NoTap);
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
//   注:焦点/滚动坐标空间在本无头配置下可能略有偏差,真机微调;核心(缩放生效+按新尺度重栅格)是主目标。
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
    // FrameView 的 scrollPosition 已是 CSS 内容坐标，pageScale 改变的是视口中每个设备像素
    // 对应的 CSS 距离。因此焦点内容点 = scroll + focal / oldScale；新 scroll = 内容点 - focal / newScale。
    // 旧公式把 scroll 也除以 oldScale、又把结果乘 scale，缩回 1x 时常被钳到 (0,0)。
    double cx = static_cast<double>(scroll.x()) + static_cast<double>(focalX) / oldScale;
    double cy = static_cast<double>(scroll.y()) + static_cast<double>(focalY) / oldScale;
    int nsx = static_cast<int>(cx - static_cast<double>(focalX) / scale + 0.5);
    int nsy = static_cast<int>(cy - static_cast<double>(focalY) / scale + 0.5);
    if (nsx < 0) nsx = 0;
    if (nsy < 0) nsy = 0;

    g_session->page->setPageScaleFactor(scale, IntPoint(nsx, nsy));
    g_session->page->isolatedUpdateRendering();
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

    // 网络 / 图片解码完成回调经 RunLoop 任务投递。仅 isolatedUpdateRendering 不会取这些任务,
    // 所以空白占位图会一直等到下一次点击/滚动的 pumpLoop 才刷新。实时 tick 先轻量转几轮队列。
    PerfOpGuard perfOp("tick", nullptr, 0, 0);   // M4
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
        doc->updateLayoutIgnorePendingStylesheets();
    }
    g_lastPendingResources = countPendingResources(*doc);
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
    return paintToRGBA(*view, g_session->w, g_session->h, outRGBA, nonWhite);
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
