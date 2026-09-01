// PortNetworkStorageSession.h — 进程级默认 curl NetworkStorageSession(cookie 持久化)。
// 由 PortNetworkStorageSession.cpp 实现。两路消费者共用一个 jar:
//   HTTP(Cookie/Set-Cookie 头)经 FrameNetworkingContext::storageSession();
//   DOM(document.cookie)经 PageConfiguration.cookieJar 的 StorageSessionProvider。
#pragma once
#include <wtf/Ref.h>
#include <wtf/text/WTFString.h>

namespace WebCore {
class NetworkStorageSession;
class StorageSessionProvider;
class FrameNetworkingContext;
class LocalFrame;
}

namespace WebCorePort {

// 显式设 cookie jar 落盘路径(镜像 WebCoreSetCACertBlob 的模式:不靠环境变量跨 clang-cl 引擎与
// MSVC v143 harness 两套独立 CRT 传递 —— App Container 里两套 CRT 是否共享环境块未经验证,且
// localUserSpecificStorageDirectory() 在 WK_WINUWP 下返回空串,拼出来的默认路径本就不可信)。
// ⚠ 2026-07-03 真机验证:此路径若非空,jar 会打开一个真实磁盘 SQLite 文件,而这个 ARM32 UWP
//   App Container clang-cl 构建的 SQLite Win32 VFS 在 sqlite3_open() 内部经空函数指针崩(见项目
//   记忆 cookie-persistence,dump 定位到 SQLiteDatabase::open→sqlite3_open 内部 blx 空寄存器)。
//   在这个坑修好前,harness 不应调用本函数——cookie 持久化改走下面的 setPortCookieJsonPath
//   (jar 本身仍是 ":memory:",持久化是引擎自己的 JSON Lines 文件,不碰 SQLite 的真实文件 I/O)。
void setPortCookieJarPath(const WTF::String& path);

// cookie 的 JSON Lines 持久化文件路径(每行一个 cookie 对象,绕开崩溃的 SQLite 真实文件 open()——
// jar 本身固定 ":memory:",这个文件只是引擎自己在旁路读写的快照)。须在首次
// ensureDefaultPortStorageSession() 之前设(harness SetupRuntimeEnv 里),不设则不持久化(不崩)。
void setPortCookieJsonPath(const WTF::String& path);
// 把当前 jar 里的非会话(有过期时间)cookie 写回 JSON Lines 文件。harness 在应用切后台(即将被
// UWP 挂起/终止)时调;WebCoreClearCookies 内部也调一次(否则清了内存 jar,磁盘快照还留着旧的)。
void flushCookiesToDisk();

WebCore::NetworkStorageSession& defaultPortStorageSession();
void ensureDefaultPortStorageSession();                                         // 预热(开 jar + 设接受策略 + 从 JSON 文件灌回持久 cookie)
WTF::Ref<WebCore::StorageSessionProvider> makeStorageSessionProvider();         // 给 CookieJar::create(DOM 路)
WTF::Ref<WebCore::FrameNetworkingContext> makeFrameNetworkingContext(WebCore::LocalFrame*);  // 给 createNetworkingContext(HTTP 路)

} // namespace WebCorePort
