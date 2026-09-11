// WebCoreDriver.h — Phase 1b 渲染驱动的 C 接口(供 C++/CX MainPage 调用)。
// 实现在 WebCoreDriver.lib(clang-cl 编的 WebCore 驱动 + 146 平台 stub)。
#pragma once
#include <cstdint>

extern "C" {

// 把一段 UTF-8 HTML 渲染成 width×height 的像素缓冲(RGBA8888,白底不透明)。
// outBuf 必须 >= width*height*4 字节。返回 0 成功,负数失败(见 WebCoreDriver.cpp 错误码)。
int WebCoreRenderHtml(const char* utf8Html, int width, int height, uint8_t* outBuf);

// Phase 1b 网络:加载真实 URL(curl + OpenSSL TLS 1.3)并渲染。返回 0 成功,负数失败
// (-9 URL 非法 / -10 加载失败 / -11 30s 超时,其余同 WebCoreRenderHtml)。
int WebCoreLoadUrl(const char* url, int width, int height, uint8_t* outBuf);

// 给 curl/OpenSSL 注入 CA 根证书包(PEM)。App Container 沙箱拿不到 Windows
// 系统证书库,不调用它则所有 HTTPS(TLS 1.3)握手都会因服务器证书校验失败而断。
// 须在首个 WebCoreLoadUrl() 之前调用一次;path 是 cacert.pem 的 UTF-8 路径。
void WebCoreSetCACertPath(const char* path);

// Apotheosis: 内存压力释放(防 OOM)。监听 UWP MemoryManager 内存事件,到高水位时经引擎线程调。
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

// 用内存 PEM blob 注入 CA 根证书(CURLOPT_CAINFO_BLOB)。App Container 沙箱挡 OpenSSL
// 的文件式 CA 加载(即便文件可读也 curl 77),故设备上必须用 blob 绕开文件 I/O。
// data 是 cacert.pem 原始字节,须在首个 WebCoreLoadUrl 之前调用。
void WebCoreSetCACertBlob(const uint8_t* data, int len);

// ⚠ 设 cookie jar 落盘 SQLite 路径。2026-07-03 真机验证会崩(这个 ARM32 UWP App Container 构建
// 的 SQLite Win32 VFS 打开真实文件时空指针,详见项目记忆 cookie-persistence)。harness 不要调用
// 这个 —— 保留仅为坑修好后备用。cookie 持久化改用下面两个(JSON Lines 旁路快照)。
void WebCoreSetCookieJarPath(const char* path);

// cookie 的 JSON Lines 持久化文件路径(每行一个 cookie 对象;jar 本身固定 ":memory:",不碰
// SQLite 真实文件 open())。须在首个引擎调用之前调(SetupRuntimeEnv 里);
// 空/未调用则不持久化(不崩)。path 是 UTF-8 文件系统路径。
void WebCoreSetCookieJsonPath(const char* path);

// 把当前 jar 里的持久(有过期时间、非会话)cookie 写回 JSON Lines 文件。app 切后台(即将被
// UWP 挂起/可能被系统直接终止)时调,引擎线程串行。
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

// 取回上次 WebCoreLoadUrl 失败时记录的网络错误(curl 错误码 + 描述 + URL)。
// 写入 buf(最多 len 字节,含 NUL),返回写入字节数(不含 NUL)。无错误则为空串。
int WebCoreGetLastError(char* buf, int len);

// 取上次 WebCoreLoadUrl 的渲染诊断(最终URL/标题/内容尺寸/非白像素数),用于定位白屏。
int WebCoreGetDiag(char* buf, int len);

// 取最近加载页面的标题(UTF-8),供历史/书签显示。返回写入字节数。
int WebCoreGetTitle(char* buf, int len);

// 取最近渲染文档的最终 URL(UTF-8)。会话内点击触发导航后,用它检测 URL 变化以同步地址栏/前进后退栈。
int WebCoreGetUrl(char* buf, int len);

// 直接下载 url 到 outPath(独立 curl,不渲染,复用 CA blob)。成功返回 HTTP 状态码(如 200),
// 失败返回负数。须先经一次网络初始化(SetupRuntimeEnv 已触发 curl 全局初始化)。
int WebCoreDownload(const char* url, const char* outPath);

// 当前页链接命中表(渲染时提取):数量 + 取第 i 个的矩形(位图坐标)和 URL。
// 用于网页点击交互:UI 点击时判断点中哪个链接矩形 → 导航。
int WebCoreGetLinkCount();
int WebCoreGetLink(int i, int* x, int* y, int* w, int* h, char* url, int len);

// ---- 常驻交互会话(live interactive session)----------------------------------
// 把一次性快照升级为常驻 Page:点击转发真实鼠标事件(按钮/表单/链接统一),滚动触发懒加载图片。
// 必须串行在单引擎线程上调用。返回 0 成功,负数失败(-12 无会话 / -13 忙 / -14 帧丢失,其余同上)。

// 加载 URL 并建立常驻会话(替代 WebCoreLoadUrl,用于需要后续交互的页面)。
int WebCoreSessionLoad(const char* url, int width, int height, uint8_t* outBuf);

// 关闭并销毁当前会话(导航到本地主页 / 挂起时调用)。
void WebCoreCloseSession();

// 在 (x,y)(位图/视口像素)点一下:命中测试 + 默认动作(导航/提交/onclick),然后重绘到 outBuf。
int WebCoreClickAt(int x, int y, uint8_t* outBuf);

// Apotheosis(双击缩放,2026-09-09):同 WebCoreClickAt,但派发的 mousedown/mouseup 带 clickCount
// (而非隐含的 1)——WebCore 的 EventHandler 在释放事件 clickCount>=2 时,'click' 后接着派发
// 'dblclick'(见 PlatformMouseEvent::clickCount())。用于 harness 判定"不缩放"(WebCoreTapPolicyAt
// 答 not-zoomable)的第二次点击,让双击语义的页面仍能收到真正的 dblclick。WebCoreClickAt 不变
// (等价于 clickCount=1)。
int WebCoreClickAtCount(int x, int y, int clickCount, uint8_t* outBuf);

// 垂直滚动 dy 像素(正=向下),触发懒加载图片后重绘到 outBuf。
int WebCoreScrollBy(int dx, int dy, uint8_t* outBuf);   // dx>0 右,dy>0 下

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

// 嵌套滚动支持(cookie 同意浮层/模态框/iframe):WebCoreScrollBy 只会移动主帧,这两个走 WebCore
// 真实的 wheel 事件滚动目标查找,让点下方的 overflow:auto 容器/模态框/iframe 自己滚,而不是滚到
// 它背后的整页。(x,y) = 位图/视口像素,同 WebCoreClickAt/WebCoreScrollBy 约定。
//
// WebCoreIsScrollableAt:只命中测试,不派发事件。点下方有可滚动祖先(或 iframe)返回 1,否则 0。
// 手势开始时调一次,决定走这条路还是直接 WebCoreScrollBy 快路径。
int WebCoreIsScrollableAt(int x, int y);
// WebCoreWheelAt:派发一次合成 wheel 事件(delta 像素,granularity=ScrollByPixelWheelEvent)。
// phase:0 none / 1 began / 2 changed / 3 ended(此 port 上目前不生效,见 WebCoreDriver.cpp)。
// 返回 1 = 被嵌套滚动体消费(下一个 delta 继续调它),0 = 未消费(无论哪种情况主帧滚动位置都不变——
// 返回 0 时 harness 必须自己为这个 delta 调 WebCoreScrollBy)。返回 1 时已经把这帧合成/呈现进 outBuf
// (与 WebCoreScrollBy 相同的 paintToRGBA 调用)——harness 不需要再补一次呈现才能看到嵌套滚动体动。
int WebCoreWheelAt(int x, int y, float deltaX, float deltaY, int phase, uint8_t* outBuf);

// Apotheosis(拖拽即指针事件):地图类控件(Leaflet / MapLibre / canvas 应用)
// 自己监听 pointerdown/mousedown 并移动自身内容,不滚动任何可滚动盒 —— 对它们来说
// WebCoreScrollBy 和 WebCoreWheelAt 都是错的路。下面两个导出让 harness 把这种手势按真实鼠标
// 拖拽交给页面。
//
// WebCoreWantsDragAt:仅命中测试,不派发事件,足够便宜可在手势开始时调(与 WebCoreIsScrollableAt
// 并列)。返回 1 = (x,y) 下的元素或其 <body> 以下的祖先带 pointerdown/mousedown/touchstart/
// pointermove/touchmove 监听器、是 <canvas>、或 CSS touch-action 非 auto/manipulation。
int WebCoreWantsDragAt(int x, int y);
// WebCoreDragAt:把手势按左键鼠标拖拽派发(引擎会一并生成 pointerdown/pointermove/pointerup)。
// phase:0=按下 / 1=移动 / 2=抬起 / 3=取消;(x,y)=视口/位图像素,同 WebCoreClickAt/WebCoreScrollBy。
// 返回 1 = 这次手势归页面所有,0 = 不归(harness 把余下手势走回正常滚动路径)。只有按下这一步做决定:
// 按下未被消费时 phase 1-3 直接返回 0 且不派发,故某个页面不理会的 mousemove 不会在拖拽中途把手势夺走。
// Apotheosis (2026-09-04): the press is owned when EITHER the engine reported it handled OR the
// point is still a drag widget (the WebCoreWantsDragAt walk, re-run here). A map that listens for
// pointerdown without calling preventDefault - a map site does exactly that - answers "not
// handled" and would otherwise lose the whole gesture on its first event.
// 返回 1 时已按 WebCoreWheelAt 的方式合成/呈现到 outBuf;outBuf 可为 null(则不呈现)。
int WebCoreDragAt(int phase, int x, int y, uint8_t* outBuf);
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
int WebCoreLongPressAt(int x, int y, int holdMs, int flags, uint8_t* outBuf);

// Apotheosis (pinch on map widgets, 2026-09-06): `notches` ctrl+wheel clicks at (x,y), positive =
// wheel up = zoom in, one notch = 120 px of delta and one wheel tick (what a real mouse wheel
// sends). A pinch that starts over a map must become this instead of WebCoreSetPageScale: page zoom
// scales a picture of the map (old tiles, blurry labels), while the wheel asks the map itself for
// the next zoom level around the point. Returns 1 if the page took the wheel, 0 if nothing did (the
// caller can then fall back to page zoom). The document never scrolls on this path; on a 1 the
// frame is already composited/presented into outBuf the way WebCoreWheelAt does it.
int WebCoreZoomWheelAt(int x, int y, int notches, uint8_t* outBuf);

// Apotheosis(双击缩放,2026-09-09):命中测试 (x,y),回答这里的第二次点击是否应该缩放页面
// (Safari/移动 Chrome 语义)而不是被当成普通的第二次点击转发。只读:不派发事件,不改会话/文档
// 状态——可以在 harness 还没决定要不要真的点击之前,在"按住"路径里调用。
// 判定顺序(0.1.9.39;规则 1 在 0.1.9.50 改为对称):
//   1. 当前页面尺度与 1:1 相差超过 5%(两个方向都算,= harness 自己的捏合缩放)一律优先:
//      zoomable=1、target=1.0,不管页面怎么说。双击必须永远能撤销一次捏合,否则关掉双击
//      缩放的页面会把用户困在捏合留下的尺度上。两个方向都要,因为本 harness 也会提交小于
//      1:1 的页面尺度(捏出去的"总览"视图,最低 0.5);0.1.9.50 之前这条只判 > 1.05,于是
//      在用户捏小了的页面上双击会落到规则 2-5 被拒绝。
//   2. 页面整体关闭缩放(viewport meta user-scalable=no,或 minimum-scale == maximum-scale):不缩放。
//   3. 页面是移动端优化的(viewport meta 带 width=device-width,或未设 width 而 initial-scale=1)
//      —— 即 Blink 的 WebViewImpl::ShouldDisableDesktopWorkarounds():不缩放。这条让所有规矩的
//      移动站点点击立即生效,因为 harness 只在真可能缩放的地方才付双击等待间隔。
//   4. 命中元素或某个祖先的 CSS touch-action 非 auto:不缩放(镜像 WebKit 自己的
//      Element::allowsDoubleTapGesture(),此 port 因 ENABLE_TOUCH_EVENTS=0 被编掉,故在此重新实现)。
//   5. 否则可缩放,目标尺度见下。
//   *outZoomable:1=此处适用双击缩放,0=不适用(含无会话/未命中,也含"目标尺度不会真的改变
//     页面"的情况,见下)。
//   *outTargetScale:双击应动画到的尺度(已钳到本驱动自己的双击区间 [1.25, 3.0],是
//     WebCoreSetPageScale [0.5, 6.0] 的子区间):上面第 1 条答 1.0;否则答 视口宽度 /
//     (命中点最内层、宽度小于布局视口 90% 的块级祖先宽度)(Safari 的"缩放到栏"启发式),
//     没有这样的祖先时答 2.0。宽度在视口 90% 以上的块是页面自己的整宽布局而非栏,缩放到它
//     等于什么都不做。与当前尺度相差不到 ±5% 的目标永远不会被返回——那种情况改答 zoomable=0,
//     调用方转发 clickCount=2 的点击,而不是动画到原地。
//   *outAnchorX/*outAnchorY:这次缩放的锚点,同 WebCoreSetPageScale 的 focalX/focalY 一样的
//     视口/位图像素。*outAnchorY 总是 y(点击 y)——竖直方向点击点始终留在手指下。*outAnchorX
//     是 x(点击 x),除非目标尺度来自上面的"缩放到栏"分支——此时是那一栏自己的水平中心
//     (0.1.9.40):Safari 把窄栏居中显示,而不是锚定在点击点在栏内的任意位置,否则点在栏边缘
//     会缩放到栏的大半截跑出屏幕外。两种情况下调用方都不必在按住间隔内自己记着点击点。
//   *outReason (0.1.9.40):上面哪条规则决定了答案——0=可缩放(已算出 target),1=已放大
//     (规则 1),7=已缩小(规则 1 的另一半,0.1.9.50),2=命中点下无元素,3=viewport 关闭缩放
//     (规则 2),4=移动端优化 viewport(规则 3),5=touch-action opt-out(规则 4),6=目标与当前
//     尺度相差不到 5%(不值得动画)。负数(错误)返回时留 -1——无会话/忙/无文档不是规则决定。
// 任一输出指针可为 null。返回 kOK,或负数错误(无会话时也把 outZoomable 置 0,只查它的调用方
// 照样安全降级)。
int WebCoreTapPolicyAt(int x, int y, int* outZoomable, float* outTargetScale, int* outAnchorX, int* outAnchorY, int* outReason);

// Apotheosis(链接上下文菜单 0.1.9.42):(x,y)(视口/位图像素,同 WebCoreClickAt)处最内层 <a> 的
// 绝对 http(s) 链接,UTF-8 + NUL 写入 outUrl。只读:不派发事件、不改会话/帧,可在长按路径上先问
// 它,再决定这次长按是弹菜单还是照旧转发给页面。
//   * 命中测试与 WebCoreTapPolicyAt 走同一套客户端像素换算,故任意页面缩放下都正确,并且尊重
//     z-order —— 这是 WebCoreGetLink 那张矩形表做不到的(它是布局后的一次性采集,看不见谁盖住了链接)。
//   * 可拖拽控件优先:命中 canvas / touch-action:none(dragWidgetAtPoint,与捏合/拖拽/点击路由同一
//     探针)时报"无链接",那里的长按行为一如既往 —— 该手势归控件所有。
//   * 只报 http(s);javascript:、mailto:、纯 fragment 锚点新标签页打不开。
// 返回 1=已写入链接,0=此处无链接(outUrl 置空),负数=常规驱动错误(kErrNoSession/kErrBusy/
// kErrNoDocument,或 cap 装不下 URL 时 kErrBadArgs)。
int WebCoreLinkAt(int x, int y, char* outUrl, int cap);

// 滚动停止后刷新链接命中表(滚动期间为提速跳过了链接提取)。轻量:仅布局+提取,不绘制。返回 0。
int WebCoreSyncLinks();
// 诊断:最近一次 WebCoreTypeText 的可编辑/聚焦/插入状态(排查"打字不进框")。
int WebCoreEditDebug(char* out, int cap);

// M4 捏合缩放:把页面缩放因子设为 scale(钳 [0.5,6.0]),以屏幕焦点 (focalX,focalY) 锚定,重栅格(文字清晰)后重绘到 outBuf。返回 0。
int WebCoreSetPageScale(float scale, int focalX, int focalY, uint8_t* outBuf);
// M4:当前页面缩放因子 ×1000(1000=1.0x)。
int WebCoreGetPageScale();

// 不交互,仅按当前会话状态重绘到 outBuf。
int WebCoreSessionPaint(uint8_t* outBuf);

// Apotheosis (landscape/rotation, 0.1.9.41): re-establish the engine viewport at w*h. Call it
// whenever the presenting panel changes size - a device rotation is the case that matters, but a
// window resize on any host is the same event. Engine thread only, like every other session call.
//
// Until this existed the viewport was written exactly twice, by WebCoreGpuInit (the GL viewport)
// and by WebCoreSessionLoad (the LocalFrameView), and never again: after a rotation the engine
// kept laying out and compositing at the old size while the window surface under it had the new
// shape, so the previous picture was simply stretched over it. This does the full job - GL
// viewport, LocalFrameView, a real relayout at the new width (media queries, percentage widths and
// the layout viewport all move), the scroll position re-clamped against the document the relayout
// produced, the link table re-extracted, and one composite so the caller has a correct frame - and
// forces the next composite to re-raster the whole tree, because every tile was painted for the
// old viewport.
//
// A focused editable element is scrolled back into view after the relayout: the keyboard survives a
//   rotation, so the field being typed into has to survive it too. No-op when the field already
//   sits comfortably inside the visible area. "Visible" here means the viewport MINUS the strip
//   WebCoreSetBottomOcclusion() reported, plus a comfort gap, so the field ends up above the
//   keyboard rather than one sliver inside the viewport's bottom edge.
//
// outRGBA: as everywhere else, the software path fills it with w*h*4 bytes and the GPU
//   direct-present path does not touch it (the frame goes to the swap chain). It may be null ONLY
//   when there is no live session, which is also the case where this call does nothing but record
//   the size for the next WebCoreSessionLoad.
// outSurfaceW/outSurfaceH (either may be null): DIAGNOSTIC. The size of the EGL window surface
//   after the composite, i.e. what ANGLE actually resized the swap chain to for the panel, or 0x0
//   when there is no window surface. The caller asks for w*h; ANGLE derives its own size from the
//   panel and the resolution scale the caller set at WebCoreGpuInit time, and these two numbers
//   are how a device log shows whether the two agree. Nothing in the engine acts on them.
// Returns kOK, or one of five negative errors. With a live session the call is ALL-OR-NOTHING: on
//   every one of them the GL viewport and the LocalFrameView are still the pair the previous call
//   left, so the caller must keep the size it had rather than the size it asked for.
//     kErrBadArgs     w/h outside the surface limits, or outRGBA null while a session is live.
//     kErrBusy        a pump is already running (re-entrancy guard) - retry after it.
//     kErrNoView      the main frame has no LocalFrameView.
//     kErrNoDocument  the main frame has no Document.
//     kErrFrameGone   the main frame is gone. THE ONE EXCEPTION to the sentence above: the session
//                     has been torn down before returning, so treat it as a lost session exactly as
//                     for the interaction entry points, not as a resize to retry.
int WebCoreResize(int w, int h, int* outSurfaceW, int* outSurfaceH, uint8_t* outRGBA);

// 视口底部被"引擎看不见的东西"遮住多少引擎像素——实为屏幕键盘:它是覆盖在窗口之上的系统浮层,
// 并不改变 harness 交给引擎的视口大小。粘性设置,0(默认)=整个视口都可见。引擎在需要把东西
// 露给用户看时读它(目前是 WebCoreResize 的聚焦框回滚)。
void WebCoreSetBottomOcclusion(int enginePx);

// Apotheosis (0.1.9.46): 单独执行 WebCoreResize 里的"聚焦框回滚",不做 resize。
// 旋转后屏幕键盘在新方向下的矩形要晚几拍才到(壳先回答上一个方向的矩形,
// 0.1.9.44 会拒绝它),所以 resize 当时那次回滚是按遮挡 0 算的,输入框仍在键盘后面。
// 等真矩形到手后:先 WebCoreSetBottomOcclusion(),再调这个(引擎线程)。
// 开销很小:文档不脏就不重排,也不自己合成(滚动会点亮下一帧)。
// 返回 kOK(没有聚焦可编辑元素也是 kOK)/ kErrNoSession / kErrBusy。
int WebCoreRevealFocusedElement(void);

// ---- 输入法/键盘 ----
// 当前是否有可编辑元素聚焦(输入框/textarea/contenteditable)→ 据此弹/收屏幕键盘。返回 1/0。
int WebCoreFocusedEditable();
// 向聚焦的可编辑元素插入文本(UTF-8),重绘到 outBuf。返回 0 成功。
int WebCoreTypeText(const char* utf8, uint8_t* outBuf);
// 特殊键:0=退格,1=回车(可能触发表单提交导航),重绘到 outBuf。返回 0 成功。
int WebCoreKeyAction(int action, uint8_t* outBuf);

// UA 切换:mobile=1 移动 iPhone UA(默认),0 桌面 Edge UA。切后需重新加载页面生效。
void WebCoreSetUserAgentMobile(int mobile);
// 自定义 UA:非空覆盖 mobile/desktop(绕开按 UA 拦截的站点如 microsoft);空串=清除回退开关。切后重载生效。
void WebCoreSetUserAgentString(const char* ua);

// Apotheosis（隐私审查）：推测预取（<script type="speculationrules">）开关，默认关。
// enabled!=0 时网页可预取用户未点击的 URL。仅引擎线程调；对当前会话和新建会话都生效。
void WebCoreSetSpeculativePrefetch(int enabled);

// Apotheosis (M4): warm up an origin before the user navigates to it — call it while a URL is
// being typed (debounced, e.g. once per suggestion update) so the DNS lookup is already done when
// Enter arrives. Accepts a full URL or a bare host ("example.com"); anything else is ignored. DNS only:
// libcurl cannot open a reusable connection ahead of time (see WebCoreDriver.cpp). Non-blocking
// (resolves on a work queue), idempotent per host, engine thread.
void WebCorePreconnect(const char* url);

// M1:GPU 合成是否在跑(根 GraphicsLayer 已附)。加载后查,返回 1/0。
int WebCoreEnableCompositing();

// M2:GPU 合成呈现(引擎线程调)。详见 WebCoreDriver.cpp。
// nativeWindow=SwapChainPanel 的 PropertySet 的 IInspectable*(直呈现);nullptr=离屏(仅 readback)。成功后引擎对网络会话开合成。
int WebCoreGpuInit(void* nativeWindow, int w, int h);
// 把当前会话图层树直呈现到窗口表面(eglSwapBuffers)。仅 GpuInit(nativeWindow!=null) 后有意义。返回 0 成功。
int WebCoreComposite();
// 离屏合成 + readback 出 RGBA 到 outBuf(>= w*h*4),用现有 WriteableBitmap 显示。返回 0 成功。
int WebCoreCompositeReadback(uint8_t* outBuf);
// 调试:设离屏 readback 翻转(找正确朝向)。flipH/flipV 非0=反转列/行。设完重绘当前帧生效。
void WebCoreGpuSetFlip(int flipH, int flipV);
// 调试:把 FrameView 滚动/内容尺寸 + 合成图层树文本写入 outBuf(定位背景丢失/滚动失效)。返回 0 成功。
int WebCoreGpuLayerInfo(char* outBuf, int len);

// Apotheosis：事件驱动呈现。引擎在“有东西需要重新呈现”时
// （调度渲染更新 / 图片加载完 / 异步栅格化瓦片落地 / 一次 tick 结束时仍脏）回调它，代替
// harness 固定 200ms 轮询。每次合成最多回调一次（WebCoreLiveTick 开头重新武装）。
// ★ 回调可能在引擎线程或栅格化工作线程上跑：不得阻塞、不得反过来调引擎，只能投队列。
// 传 nullptr = 取消注册（回到纯轮询）。引擎线程上注册一次，首次导航前。
void WebCoreSetPresentRequestCallback(void (*cb)(void* ctx), void* ctx);
// Apotheosis (package 5): the pan present handshake is gone - WebCoreSetPanGesture /
// WebCorePresent / WebCoreGetOwedSwapScroll / WebCorePresentFrame. It let a gesture take the
// presents away from the engine so its own composites could not race the XAML TranslateTransform
// the harness drew a pan with; that preview is deleted, and it was the only caller the mode ever
// had. The engine presents every composite as it makes it.


// 在当前会话主世界执行 JS,结果转字符串写入 out。诊断/注入用。返回 0 成功。
int WebCoreEvalJS(const char* script, char* out, int len);

// 实时一帧:推进动画/rAF/SPA 一帧并重绘到 outBuf(供低帧率定时器驱动,让动画动起来、SPA 渐进挂载)。
int WebCoreLiveTick(uint8_t* outBuf);
// 当前文档仍处于 Pending/Unknown 的缓存资源数。用于图片/解码未完成时保持实时 tick。
int WebCoreGetPendingResourceCount();
// 最近一帧像素哈希:实时模式比较连续帧,画面静止则停帧省电。
unsigned WebCoreGetFrameHash();

// ---- 页内查找 find-in-page ----
// 标记并高亮全部匹配 + 选中第一个,滚动到它,重绘到 outBuf。matchCase!=0 区分大小写;wrap!=0 回绕。
// 空串=清除高亮。返回匹配数(>=0)或负错误码。
int WebCoreFindString(const char* utf8, int matchCase, int wrap, uint8_t* outBuf);
// 沿用上次查找词查下一个/上一个(不重新标记)。forward!=0 向下。返回 1=命中 / 0=无 / 负=错误。
int WebCoreFindNext(int forward, uint8_t* outBuf);
// 清除查找高亮/选区,重绘。返回 0 成功。
int WebCoreFindClear(uint8_t* outBuf);

}
