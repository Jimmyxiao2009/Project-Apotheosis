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

#include <cstdint>
#include <cstring>
#include <cstdio>
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
#include <WebCore/CookieJar.h>           // WebCore::CookieJar(cookie 持久化)
#include <WebCore/StorageSessionProvider.h>  // 完整类型(Ref<StorageSessionProvider> 析构需要)
#include "PortNetworkStorageSession.h"   // WebCorePort::makeStorageSessionProvider / ensureDefaultPortStorageSession
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
        WebCore::populateJITOperations();        // no-op under ENABLE(C_LOOP) (header has inline {} fallback)
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
extern "C" bool g_apoUaMobile = true;  // UA 开关:true=移动 iPhone(默认),false=桌面(LoadingFrameLoaderClient::userAgent 用)。extern "C" 跨命名空间一个符号
static char g_spaProbe[512] = "";     // SPA 模块求值探针结果(诊断 <script type=module> 是否求值/抛错)
static std::vector<uint8_t> g_caBytes;  // CA 根证书字节副本,供 WebCoreDownload 的独立 curl 句柄用

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
    int w = 0, h = 0;
    DriverLoadState load;                  // 堆上(随会话存活):晚到的 didFinishLoad 不会 deref 已释放栈
};
static std::optional<Session> g_session;
static bool g_inPump = false;              // settle 轮询 / 事件派发的重入保护

// 复位 g_inPump 的作用域守卫(无异常环境下,析构在正常返回路径也会执行)。
struct PumpGuard { ~PumpGuard() { g_inPump = false; } };

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
            // EmptyChromeClient 下自动 RenderingUpdateScheduler 是 no-op;不显式调它则 rAF /
            // IntersectionObserver / 懒加载图片永不触发(滚动加载与 SPA 渲染必需)。
            if (pageForRendering) {
                pageForRendering->isolatedUpdateRendering();   // 跑 rAF/IntersectionObserver(可能跑 JS 改 DOM 甚至导航)
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

// view->paint → Cairo ARGB32 → 调用方 RGBA8888 缓冲(B<->R 交换 + 去预乘)。统计非白像素数。
// 同时供一次性 WebCoreLoadUrl 与会话各入口复用(单一绘制实现)。
static int paintToRGBA(WebCore::LocalFrameView& view, int w, int h, uint8_t* outRGBA, int& nonWhiteOut)
{
    using namespace WebCore;
    nonWhiteOut = 0;
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
        view.paint(context, IntRect(IntPoint(), size));
    }
    cairo_surface_flush(surface);

    const unsigned char* src = cairo_image_surface_get_data(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    int nonWhite = 0;
    uint32_t hash = 2166136261u;   // FNV-ish 滚动哈希,实时模式判断画面是否变化(顺带在同一遍像素循环里算)
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
    std::snprintf(g_lastTitle, sizeof g_lastTitle, "%s", titleStr.data());
    std::snprintf(g_lastUrl, sizeof g_lastUrl, "%s", urlStr.data());
    int mainLen = std::snprintf(g_lastDiag, sizeof g_lastDiag,
        "url=%s title=%s contents=%dx%d body=%d nonwhite=%d/%d loads=S%d/R%d/C%d/F%d js=%d/%d scripts=%u rootKids=%d bodyKids=%d spa=[%.220s] lasterr=[%.150s]",
        urlStr.data(), titleStr.data(), cs.width(), cs.height(),
        document.body() ? 1 : 0, nonWhite, w * h,
        g_loadStarted, g_loadResponse, g_loadComplete, g_loadFail,
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
    page->settings().setAcceleratedCompositingEnabled(false);
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
    pumpLoop(*localMainFrame, &g_session->load.mainDone, /*allowEarlyStopWithoutNav*/ false,
             /*settleCapTicks*/ 160, /*watchdog*/ 30.0, /*pageForRendering*/ page.ptr());

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
    document->updateLayoutIgnorePendingStylesheets();

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
    document->updateLayoutIgnorePendingStylesheets();
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
    if (!utf8Html || !outRGBA || w <= 0 || h <= 0)
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

    if (!url || !outRGBA || w <= 0 || h <= 0)
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
    if (!url || !outRGBA || w <= 0 || h <= 0)
        return kErrBadArgs;
    ensureWebCoreInitialized();
    if (g_inPump)
        return kErrBusy;
    teardownSession();
    g_lastNetError[0] = '\0';
    g_spaProbe[0] = '\0';
    g_loadStarted = g_loadResponse = g_loadComplete = g_loadFail = 0;
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

    RefPtr<LocalFrame> lf = g_session->mainFrame;
    RefPtr<LocalFrameView> view = lf->view();
    if (!view)
        return kErrNoView;
    RefPtr<Document> doc = lf->document();
    if (!doc)
        return kErrNoDocument;
    doc->updateLayoutIgnorePendingStylesheets();   // 命中测试需要最新布局(尤其滚动后)

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

    // 同步处理器(JS onclick 等)已返回;导航(若有)异步 → settle。无导航则空闲早停。
    pumpLoop(*lf, &g_session->load.mainDone, /*allowEarlyStopWithoutNav*/ true,
             /*settleCapTicks*/ 160, /*watchdog*/ 30.0, /*pageForRendering*/ g_session->page.get());

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
    doc->updateLayoutIgnorePendingStylesheets();
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
int WebCoreScrollBy(int dy, uint8_t* outRGBA)
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
    RefPtr<LocalFrameView> view = lf->view();
    if (!view)
        return kErrNoView;
    RefPtr<Document> doc = lf->document();
    if (!doc)
        return kErrNoDocument;
    doc->updateLayoutIgnorePendingStylesheets();   // contentsSize/最大滚动有效

    ScrollPosition cur = view->scrollPosition();
    ScrollPosition minP = view->minimumScrollPosition();
    ScrollPosition maxP = view->maximumScrollPosition();
    int tx = cur.x();
    int ty = cur.y() + dy;
    if (tx < minP.x()) tx = minP.x();
    if (tx > maxP.x()) tx = maxP.x();
    if (ty < minP.y()) ty = minP.y();
    if (ty > maxP.y()) ty = maxP.y();
    view->setScrollPosition(ScrollPosition(tx, ty));

    g_session->page->isolatedUpdateRendering();   // 立即触发一次 runScrollSteps/IntersectionObserver
    // 无导航语义:空闲(懒加载图片都到位)即停,最多 8s。每 tick 继续驱动多轮 IO/懒加载。
    pumpLoop(*lf, /*mainDone*/ nullptr, /*allowEarlyStopWithoutNav*/ true,
             /*settleCapTicks*/ 0, /*watchdog*/ 8.0, /*pageForRendering*/ g_session->page.get());

    // 滚动处理器 / 懒加载 JS 可能触发导航并重建/替换主帧,重新校验 + 重取(否则 lf->view() 解引用已脱离的帧)。
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
    doc->updateLayoutIgnorePendingStylesheets();
    extractLinks(doc.get(), g_session->h);

    int nonWhite = 0;
    int prc = paintToRGBA(*view, g_session->w, g_session->h, outRGBA, nonWhite);
    if (prc != kOK)
        return prc;
    writeDiag(*doc, *view, g_session->w, g_session->h, nonWhite);
    return kOK;
}

// 当前会话是否有可编辑元素聚焦(输入框/textarea/contenteditable)→ harness 据此弹/收输入法。
// UA 切换:mobile=1 移动 iPhone UA(默认),0 桌面 Windows UA。切后由 UI 重新加载页面生效。
void WebCoreSetUserAgentMobile(int mobile)
{
    g_apoUaMobile = (mobile != 0);
}

int WebCoreFocusedEditable()
{
    if (!g_session || !g_session->mainFrame || g_inPump)
        return 0;
    return g_session->mainFrame->editor().canEdit() ? 1 : 0;
}

// 向聚焦的可编辑元素插入文本(派发 beforeinput/input,SPA 框架可感知),pump 让 JS 反应,重绘。返回 0 成功。
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
    if (!lf->editor().canEdit())
        return kErrNoDocument;   // 没有可编辑焦点,忽略
    lf->editor().insertText(String::fromUTF8(utf8), nullptr);
    pumpLoop(*lf, nullptr, true, 0, 4.0, g_session->page.get());
    return finishInteractionPaint(outRGBA);
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
        lf->editor().command("DeleteBackward"_s).execute();
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

    g_session->page->isolatedUpdateRendering();   // 推进一帧动画/rAF/IO(可能跑 JS,甚至导航/换帧)
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
    doc->updateLayoutIgnorePendingStylesheets();
    int nonWhite = 0;
    return paintToRGBA(*view, g_session->w, g_session->h, outRGBA, nonWhite);
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
    if (!outRGBA || w <= 0 || h <= 0)
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
