// PortNetworkStorageSession.cpp — 见 .h。激活自 draft-PortNetworkStorageSession.cpp。
// 拥有一个进程级默认 curl NetworkStorageSession(模仿 WebKitLegacy 的
// NetworkStorageSessionMap::defaultStorageSession()),让 cookie 真正被读写+落盘。
// 落盘路径:NetworkStorageSessionCurl 的 defaultCookieJarPath() 在 PLATFORM(WIN) 下用
// localUserSpecificStorageDirectory()+"cookie.jar.db",或环境变量 CURL_COOKIE_JAR_PATH。
// App Container 里前者可能不可写,故驱动初始化时把 CURL_COOKIE_JAR_PATH 指到 LocalState。
#include "config.h"

#include "PortNetworkStorageSession.h"

#include <WebCore/NetworkStorageSession.h>
#include <WebCore/StorageSessionProvider.h>
#include <WebCore/FrameNetworkingContext.h>
#include <WebCore/LocalFrame.h>
#include <pal/SessionID.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/MainThread.h>

#if PLATFORM(WIN)
#include <WebCore/ResourceError.h>
#include <WebCore/ResourceRequest.h>
#endif

namespace WebCorePort {

using namespace WebCore;

// 单一进程级默认 curl session。主线程(WebKit 主线程,驱动在 ensureWebCoreInitialized 里 pin)。
WebCore::NetworkStorageSession& defaultPortStorageSession()
{
    ASSERT(isMainThread());
    static NeverDestroyed<std::unique_ptr<WebCore::NetworkStorageSession>> session;
    if (!session.get())
        // ★ 临时(ephemeral)会话 → NetworkStorageSessionCurl.cpp:93 用 ":memory:" CookieJarDB(纯内存,不碰磁盘)。
        //   避开 App Container 里 defaultCookieJarPath()(localUserSpecificStorageDirectory 不可写)→ open() 崩
        //   (0.1.6.0 访问任何网页闪退的根因)。代价:cookie 不跨重启持久(登录在 app 开着时有效)。
        //   持久化后续:确认不崩后加 WebCoreSetCookieJarPath 把 CURL_COOKIE_JAR_PATH 指到 LocalState。
        session.get() = makeUnique<WebCore::NetworkStorageSession>(PAL::SessionID::generateEphemeralSessionID());
    return *session.get();
}

void ensureDefaultPortStorageSession()
{
    auto& s = defaultPortStorageSession();
    // 默认只接受主文档域 cookie(各端口惯例);cookieDatabase() 在此惰性开 jar。
    s.setCookieAcceptPolicy(CookieAcceptPolicy::OnlyFromMainDocumentDomain);
}

// DOM 路:document.cookie 经此 provider 到达真 jar。
class PortStorageSessionProvider final : public WebCore::StorageSessionProvider {
public:
    static Ref<PortStorageSessionProvider> create() { return adoptRef(*new PortStorageSessionProvider); }
    WebCore::NetworkStorageSession* storageSession() const final { return &defaultPortStorageSession(); }
private:
    PortStorageSessionProvider() = default;
};

// HTTP 路:ResourceHandle 的 curl bridge 经 d->m_context->storageSession() 到达真 jar。
class PortFrameNetworkingContext final : public WebCore::FrameNetworkingContext {
public:
    static Ref<PortFrameNetworkingContext> create(WebCore::LocalFrame* frame)
    {
        return adoptRef(*new PortFrameNetworkingContext(frame));
    }
    WebCore::NetworkStorageSession* storageSession() const final { return &defaultPortStorageSession(); }

#if PLATFORM(WIN)
    WebCore::ResourceError blockedError(const WebCore::ResourceRequest&) const final { return { }; }
#endif

private:
    explicit PortFrameNetworkingContext(WebCore::LocalFrame* frame)
        : WebCore::FrameNetworkingContext(frame) { }
};

Ref<WebCore::StorageSessionProvider> makeStorageSessionProvider()
{
    return PortStorageSessionProvider::create();
}

Ref<WebCore::FrameNetworkingContext> makeFrameNetworkingContext(WebCore::LocalFrame* frame)
{
    return PortFrameNetworkingContext::create(frame);
}

} // namespace WebCorePort
