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
