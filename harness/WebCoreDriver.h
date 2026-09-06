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

// Apotheosis (MEMORY-PLAN.md §3 change 2): engine-side memory accounting, so the harness'
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

// Apotheosis(拖拽即指针事件):地图类控件(Google 地图 / OpenStreetMap-Leaflet / canvas 应用)
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
// pointerdown without calling preventDefault - Google Maps does exactly that - answers "not
// handled" and would otherwise lose the whole gesture on its first event.
// 返回 1 时已按 WebCoreWheelAt 的方式合成/呈现到 outBuf;outBuf 可为 null(则不呈现)。
int WebCoreDragAt(int phase, int x, int y, uint8_t* outBuf);
// Apotheosis (Google Maps pin, 2026-09-06): LONG PRESS on a drag widget. ENABLE_TOUCH_EVENTS is 0
// on this port and nothing synthesises Touch/Pointer input, so a touch long press cannot be
// delivered as one; what this does deliver is everything a page can key a long press off with a
// mouse: mousedown, the button STAYS DOWN while the engine turns its run loop for holdMs (so a
// press-and-hold timer inside the page - Google Maps drops its pin from exactly such a timer - gets
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

// Apotheosis (PRIVACY-AUDIT.md): 推测预取（<script type="speculationrules">）开关，默认关。
// enabled!=0 时网页可预取用户未点击的 URL。仅引擎线程调；对当前会话和新建会话都生效。
void WebCoreSetSpeculativePrefetch(int enabled);

// Apotheosis (M4): warm up an origin before the user navigates to it — call it while a URL is
// being typed (debounced, e.g. once per suggestion update) so the DNS lookup is already done when
// Enter arrives. Accepts a full URL or a bare host ("ntv.de"); anything else is ignored. DNS only:
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

// Apotheosis (OFFTHREAD-RASTER-LOG.md): rasterise TextureMapper tiles on a worker pool instead of
// on the engine thread (experimental, default OFF). Only tiles that already hold valid content go
// async; first paints and recycled tiles stay synchronous, so no tile is ever composited empty.
// Takes effect from the next composite and may be flipped at any time. Engine thread only.
void WebCoreSetThreadedRaster(int enabled);

// Apotheosis (2026-09-04): stale tiles. ON (the default) a TextureMapper backing store keeps the
// tiles it drops out of its cover rect and keeps drawing them, scaled to the current content rect,
// until real ones have been rasterised - so a composite that takes the scroll fast path after the
// tiles have moved on paints the old pixels instead of nothing (the "everything goes white when
// scrolling ends" symptom). OFF is the previous behaviour; the switch exists so the device can A/B
// it without a rebuild. Takes effect from the next composite. Engine thread only.
void WebCoreSetStaleTiles(int enabled);

// Apotheosis (THREADED-COMPOSITOR-PLAN.md C5): 事件驱动呈现。引擎在“有东西需要重新呈现”时
// （调度渲染更新 / 图片加载完 / 异步栅格化瓦片落地 / 一次 tick 结束时仍脏）回调它，代替
// harness 固定 200ms 轮询。每次合成最多回调一次（WebCoreLiveTick 开头重新武装）。
// ★ 回调可能在引擎线程或栅格化工作线程上跑：不得阻塞、不得反过来调引擎，只能投队列。
// 传 nullptr = 取消注册（回到纯轮询）。引擎线程上注册一次，首次导航前。
void WebCoreSetPresentRequestCallback(void (*cb)(void* ctx), void* ctx);
// Apotheosis (pan present handshake): a main-frame touch pan is shown by the harness itself, as a
// XAML TranslateTransform on the presenting element, while the engine catches up in coarse steps.
// The panel content and that transform are composed independently, so any present the harness did
// not ask for puts new content under the old translation for a frame (visible flicker/jump-back).
// WebCoreSetPanGesture(1) therefore makes the engine present nothing on its own: composites still
// happen but the eglSwapBuffers is deferred, and WebCoreLiveTick skips its composite entirely
// (content updates - rAF, timers, decodes - still run, they just become visible with the next
// scroll present). WebCorePresent() releases a deferred swap; the harness posts it once XAML has
// committed the matching translation. WebCoreSetPanGesture(0) hands presents back and asks for one
// full composite. Both engine thread only; WebCorePresent is idempotent and a no-op when nothing
// is owed. Returns 0 (kOK).
void WebCoreSetPanGesture(int active);
int WebCorePresent(void);

// Apotheosis (XAML-path consistency review, 2026-09-04): the deferred-swap handshake, made exact.
// WebCorePresent() releases "whatever is owed", which is wrong twice over: an acknowledgement can
// overtake or be overtaken by a newer scroll job, and any other export that paints during a gesture
// (click, wheel, drag, pinch, session paint) also leaves a deferred swap behind - so the ack for
// one frame could release a completely different one, under a translation committed for the frame
// it was not.
//   WebCoreGetOwedSwapScroll  1 = a swap is owed; fills the scroll position that composite is
//                             showing and its swap id. Call it in the same engine hop as the
//                             WebCoreScrollBy (next to WebCoreGetScrollState) and build the pan
//                             translation from THAT position - offset - (swapScroll -
//                             gestureStartScroll) - not from wherever the engine is now.
//                             Cheap: no layout, no paint. Engine thread.
//   WebCorePresentFrame       release the owed swap only if it is still the frame `swapId` names;
//                             an id that does not match leaves the frame owed rather than showing
//                             it under the wrong translation. swapId 0 == WebCorePresent().
//                             Engine thread, idempotent, returns 0 (kOK).
int WebCoreGetOwedSwapScroll(int* outScrollX, int* outScrollY, unsigned long long* outSwapId);
int WebCorePresentFrame(unsigned long long swapId);


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
