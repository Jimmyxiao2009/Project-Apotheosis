// WebCoreDriver.h — Phase 1b 渲染驱动的 C 接口(供 C++/CX MainPage 调用)。
// 实现在 WebCoreDriver.lib(clang-cl 编的 WebCore 驱动 + 146 平台 stub)。
#pragma once
#include <cstdint>

extern "C" {

// 把一段 UTF-8 HTML 渲染成 width×height 的像素缓冲(RGBA8888,白底不透明)。
// outBuf 必须 >= width*height*4 字节。返回 0 成功,负数失败(见 WebCoreDriver.cpp 错误码)。
int WebCoreRenderHtml(const char* utf8Html, int width, int height, uint8_t* outBuf);

// 验证版:只跑 init + Page::create + 纯色填充,验证 C ABI + 显示管线(引擎风险最小)。
int WebCoreRenderHtmlStub(const char* utf8Html, int width, int height, uint8_t* outBuf);

// Phase 1b 网络:加载真实 URL(curl + OpenSSL TLS 1.3)并渲染。返回 0 成功,负数失败
// (-9 URL 非法 / -10 加载失败 / -11 30s 超时,其余同 WebCoreRenderHtml)。
int WebCoreLoadUrl(const char* url, int width, int height, uint8_t* outBuf);

// 给 curl/OpenSSL 注入 CA 根证书包(PEM)。App Container 沙箱拿不到 Windows
// 系统证书库,不调用它则所有 HTTPS(TLS 1.3)握手都会因服务器证书校验失败而断。
// 须在首个 WebCoreLoadUrl() 之前调用一次;path 是 cacert.pem 的 UTF-8 路径。
void WebCoreSetCACertPath(const char* path);

// 用内存 PEM blob 注入 CA 根证书(CURLOPT_CAINFO_BLOB)。App Container 沙箱挡 OpenSSL
// 的文件式 CA 加载(即便文件可读也 curl 77),故设备上必须用 blob 绕开文件 I/O。
// data 是 cacert.pem 原始字节,须在首个 WebCoreLoadUrl 之前调用。
void WebCoreSetCACertBlob(const uint8_t* data, int len);

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

// UA 切换:mobile=1 移动 iPhone UA(默认),0 桌面 Windows UA。切后需重新加载页面生效。
void WebCoreSetUserAgentMobile(int mobile);

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
// 最近一帧像素哈希:实时模式比较连续帧,画面静止则停帧省电。
unsigned WebCoreGetFrameHash();

}
