// PortNetworkStorageSession.h — 进程级默认 curl NetworkStorageSession(cookie 持久化)。
// 由 PortNetworkStorageSession.cpp 实现。两路消费者共用一个 jar:
//   HTTP(Cookie/Set-Cookie 头)经 FrameNetworkingContext::storageSession();
//   DOM(document.cookie)经 PageConfiguration.cookieJar 的 StorageSessionProvider。
#pragma once
#include <wtf/Ref.h>

namespace WebCore {
class NetworkStorageSession;
class StorageSessionProvider;
class FrameNetworkingContext;
class LocalFrame;
}

namespace WebCorePort {

WebCore::NetworkStorageSession& defaultPortStorageSession();
void ensureDefaultPortStorageSession();                                         // 预热(开 jar + 设接受策略)
WTF::Ref<WebCore::StorageSessionProvider> makeStorageSessionProvider();         // 给 CookieJar::create(DOM 路)
WTF::Ref<WebCore::FrameNetworkingContext> makeFrameNetworkingContext(WebCore::LocalFrame*);  // 给 createNetworkingContext(HTTP 路)

} // namespace WebCorePort
