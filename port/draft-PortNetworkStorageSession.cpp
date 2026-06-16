// ============================================================================
// draft-PortNetworkStorageSession.cpp
//
// COMPONENT: networkstoragesession
//
// PROBLEM
//   With USE_CURL=ON, WebCore.lib now contains the curl NetworkStorageSession
//   (platform/network/curl/NetworkStorageSessionCurl.cpp) — it supplies the
//   *methods* (cookie read/write, accept policy, proxy) but NOBODY constructs
//   or owns a default instance. The in-process load path needs a live session
//   for two distinct consumers:
//
//     (1) The ResourceHandle->CurlRequest bridge (Core patch), for the HTTP
//         Cookie request header on outgoing requests and Set-Cookie capture on
//         responses. It reaches the session via the NetworkingContext that
//         ResourceLoader passes to ResourceHandle::create():
//             ResourceLoader::start()  (ResourceLoader.cpp:279)
//               -> ResourceHandle::create(frameLoader->networkingContext(), ...)
//             d->m_context is RefPtr<NetworkingContext> in ResourceHandleInternal.
//         Because NetworkingContext : public StorageSessionProvider, the bridge
//         can call  d->m_context->storageSession().
//
//     (2) The DOM cookie path (document.cookie), via CookieJar, which reaches
//         the session through the StorageSessionProvider handed to
//         CookieJar::create() inside the PageConfiguration
//         (EmptyClients.cpp:1252 wires an EmptyStorageSessionProvider -> nullptr).
//
//   In the current driver BOTH consumers see nullptr:
//     - EmptyFrameNetworkingContext::storageSession() returns nullptr
//       (EmptyClients.cpp:427)
//     - EmptyStorageSessionProvider::storageSession() returns nullptr
//       (EmptyClients.cpp:1205)
//   so cookies are silently dropped. For a *headless first-screen GET 200 render*
//   that is actually fine (NetworkDataTaskCurl null-checks the session too:
//   appendCookieHeader/handleCookieHeaders are no-ops when session==null). But
//   any page that depends on cookies (login, session, redirect-to-set-cookie)
//   needs a real session. The curl bridge already null-checks, so wiring this in
//   is purely additive and safe.
//
// MINIMAL DECISION
//   Own ONE process-wide default curl NetworkStorageSession, modeled EXACTLY on
//   WebKitLegacy's NetworkStorageSessionMap::defaultStorageSession()
//   (Source/WebKitLegacy/WebCoreSupport/NetworkStorageSessionMap.cpp:56-61):
//   a NeverDestroyed<unique_ptr<NetworkStorageSession>> created lazily for
//   PAL::SessionID::defaultSessionID(). The curl ctor signature is
//       NetworkStorageSession(PAL::SessionID, const String& altSvcDir = nullString())
//   so makeUnique<NetworkStorageSession>(defaultSessionID()) compiles against the
//   curl build with no extra args (matches the legacy call site verbatim).
//
//   Then expose it through:
//     - PortStorageSessionProvider  (for CookieJar in the PageConfiguration), and
//     - PortFrameNetworkingContext::storageSession()  (for the bridge), used by
//       Component A's LoadingFrameLoaderClient::createNetworkingContext().
//   Both return the SAME singleton, so DOM cookies and HTTP cookies share one jar.
//
//   WHY NOT put it in PlatformStrategies?  PlatformStrategies has no storage-
//   session hook (LoaderStrategy/BlobRegistry/Pasteboard/Media only). The
//   session is reached via NetworkingContext + StorageSessionProvider, NOT via
//   strategies. So this lives next to (not inside) installPortPlatformStrategies().
//
//   COOKIE JAR ON DISK:  defaultCookieJarPath() (NetworkStorageSessionCurl.cpp:46)
//   uses, on PLATFORM(WIN), FileSystem::localUserSpecificStorageDirectory() +
//   "cookie.jar.db", OR the env var CURL_COOKIE_JAR_PATH. In the App Container
//   sandbox localUserSpecificStorageDirectory() must resolve to a writable
//   ApplicationData location (VERIFY on device; if it returns empty/again-rooted,
//   set CURL_COOKIE_JAR_PATH to ApplicationData\LocalState before first use, or
//   use an ephemeral SessionID to force the :memory: jar — see EPHEMERAL note).
// ============================================================================

#include "config.h"

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

// --- the single process-wide default curl session ---------------------------
// Mirror of NetworkStorageSessionMap::defaultStorageSession(). Main-thread only
// (the WebKit main thread; the driver pins it in ensureWebCoreInitialized()).
WebCore::NetworkStorageSession& defaultPortStorageSession()
{
    ASSERT(isMainThread());
    static NeverDestroyed<std::unique_ptr<WebCore::NetworkStorageSession>> session;
    if (!session.get()) {
        // EPHEMERAL note: pass an ephemeral SessionID here to force the ":memory:"
        // CookieJarDB and skip all disk I/O (good for first device bring-up where
        // the App Container path may not be writable). For a persistent jar use
        // defaultSessionID() as below.
        session.get() = makeUnique<WebCore::NetworkStorageSession>(PAL::SessionID::defaultSessionID());
    }
    return *session.get();
}

// Optional: pre-warm (opens/creates the jar + sets accept policy) so the first
// request doesn't pay the SQLite open cost. Call from the driver init, right
// after installPortPlatformStrategies(). Safe to skip — cookieDatabase() opens
// lazily on first use anyway.
void ensureDefaultPortStorageSession()
{
    auto& s = defaultPortStorageSession();
    // Accept cookies only from the main document domain by default (sane, matches
    // most ports' default). cookieDatabase() inside this call opens the jar.
    s.setCookieAcceptPolicy(CookieAcceptPolicy::OnlyFromMainDocumentDomain);
}

// --- (2) DOM cookie path: StorageSessionProvider for CookieJar::create() ------
// Replaces EmptyStorageSessionProvider in the PageConfiguration so document.cookie
// reaches the real jar.
class PortStorageSessionProvider final : public WebCore::StorageSessionProvider {
public:
    static Ref<PortStorageSessionProvider> create()
    {
        return adoptRef(*new PortStorageSessionProvider);
    }
    WebCore::NetworkStorageSession* storageSession() const final
    {
        return &defaultPortStorageSession();
    }
private:
    PortStorageSessionProvider() = default;
};

// --- (1) HTTP path: FrameNetworkingContext for the load bridge ----------------
// Component A's LoadingFrameLoaderClient::createNetworkingContext() should return
// one of these (instead of EmptyFrameNetworkingContext) so that the
// ResourceHandle curl bridge — which calls d->m_context->storageSession() — gets
// the real jar. FrameNetworkingContext already implements
// shouldClearReferrerOnHTTPSToHTTPRedirect()/isValid(); we only add storageSession().
class PortFrameNetworkingContext final : public WebCore::FrameNetworkingContext {
public:
    static Ref<PortFrameNetworkingContext> create(WebCore::LocalFrame* frame)
    {
        return adoptRef(*new PortFrameNetworkingContext(frame));
    }

    WebCore::NetworkStorageSession* storageSession() const final
    {
        return &defaultPortStorageSession();
    }

#if PLATFORM(WIN)
    // NetworkingContext declares this pure-virtual ONLY under PLATFORM(WIN).
    // If the WinUWP build does NOT define PLATFORM(WIN) (it is App Container,
    // WINAPI_FAMILY_APP -> likely PLATFORM(WIN) is still true on Windows), this
    // override is required; harmless if guarded the same way as the declaration.
    // VERIFY: confirm PLATFORM(WIN) is set in build-clang-webcore/cmakeconfig.h /
    // generated Platform.h for the thumbv7 UWP target; keep the guard matching.
    WebCore::ResourceError blockedError(const WebCore::ResourceRequest&) const final
    {
        return { };
    }
#endif

private:
    explicit PortFrameNetworkingContext(WebCore::LocalFrame* frame)
        : WebCore::FrameNetworkingContext(frame)
    {
    }
};

} // namespace WebCorePort

// ============================================================================
// WIRING (3 touch points; all additive, none block the no-cookie 200 render):
//
//  A. Component A — LoadingFrameLoaderClient.cpp:
//       Ref<WebCore::FrameNetworkingContext>
//       LoadingFrameLoaderClient::createNetworkingContext()
//       {
//           return WebCorePort::PortFrameNetworkingContext::create(m_frame /*LocalFrame**/);
//       }
//     (The EmptyFrameLoaderClient version returns EmptyFrameNetworkingContext;
//      A simply swaps in ours. m_frame is the client's frame pointer.)
//
//  B. Driver (webcoreloadurl-draft.cpp / WebCoreDriver.cpp) — after building
//     pageConfiguration, replace the CookieJar so DOM cookies use the real jar:
//       pageConfiguration.cookieJar =
//           WebCore::CookieJar::create(WebCorePort::PortStorageSessionProvider::create());
//     (PageConfiguration.cookieJar is the Ref<CookieJar> field set at
//      EmptyClients.cpp:1252. Overwriting it before Page::create() is enough.)
//     If PageConfiguration's cookieJar is not reassignable post-construction,
//     instead fork pageConfigurationWithEmptyClients locally and pass our provider
//     into the CookieJar::create() call. (VERIFY field mutability.)
//
//  C. Driver init (ensureWebCoreInitialized lambda) — optional pre-warm, right
//     after installPortPlatformStrategies():
//           WebCorePort::ensureDefaultPortStorageSession();
//
//  If you do NONE of A/B/C, the build still links and the 200-path render still
//  works (cookies just stay empty). A/B are needed only for cookie-dependent pages.
// ============================================================================
