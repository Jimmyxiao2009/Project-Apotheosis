// ============================================================================
// LoadingFrameLoaderClient.cpp
//
// Implementation of LoadingFrameLoaderClient: a verbatim copy of WebCore's
// EmptyFrameLoaderClient (Source/WebCore/loader/EmptyClients.cpp) with only the
// handful of behavioral changes required to let a real network navigation
// proceed (see LoadingFrameLoaderClient.h header comment for the diff).
//
// Build with the same clang-cl flags as a WebCore TU (config.h first); validate
// with port/compile-driver.ps1.
// ============================================================================

#include "config.h"
#include "LoadingFrameLoaderClient.h"

// Mirror the EmptyClients.cpp include set needed by the FrameLoaderClient bodies.
#include <WebCore/DocumentLoader.h>
#include <WebCore/FrameLoaderClient.h>            // FramePolicyFunction, PolicyAction
#include <WebCore/FrameLoaderTypes.h>             // PolicyAction enum
#include <WebCore/FrameNetworkingContext.h>
#include <WebCore/HistoryItem.h>
#include <WebCore/LayoutMilestone.h>          // Apotheosis (M4): DidFirstVisuallyNonEmptyLayout
#include <WebCore/LocalFrame.h>
#include <WebCore/NetworkStorageSession.h>
#include "PortNetworkStorageSession.h"   // cookie 持久化:真 storageSession
#include "PortPerf.h"                    // Apotheosis: M4 per-phase timing (navigation marks)
#include <WebCore/ResourceError.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/ResourceResponse.h>
#include <wtf/CompletionHandler.h>
#include <wtf/Unexpected.h>
#include <wtf/text/CString.h>

// Apotheosis: 诊断通道,定义在 WebCoreDriver.cpp。把失败的 ResourceError 细节
// (curl 错误码 + type + 域 + 描述 + 失败 URL)送给驱动,供真机网络失败定位。type 是
// ResourceError::Type(见下方 recordNetError)——投递重定向 bug 时加的,好在下一次真机
// 测试里不用猜就能看出卡住的加载到底是不是撞见了 dispatchDidFailProvisionalLoad 里那条
// Cancellation 一律吞掉的分支。
extern "C" void WebCorePortRecordNetError(int code, int type, const char* domain, const char* desc, const char* url);

// Apotheosis: DNS prefetch for <link rel="dns-prefetch">. Implemented in
// WebKit\Source\WebKitLegacy\WebCoreSupport\WebResourceLoadScheduler.cpp, which is
// compiled straight into the driver (local\link-driver-gpu.ps1), so this is a plain
// cross-TU call. WebCore::prefetchDNS() itself is a no-op in the curl port - see the
// comment on apotheosisPrefetchDNS() there.
extern void apotheosisPrefetchDNS(const WTF::String& hostname);

namespace WebCorePort {

using namespace WebCore;

// Forward a failed load's ResourceError into the driver's diagnostic channel.
static void recordNetError(const ResourceError& error)
{
    auto domain = error.domain().utf8();
    auto desc = error.localizedDescription().utf8();
    auto url = error.failingURL().string().utf8();
    WebCorePortRecordNetError(error.errorCode(), static_cast<int>(error.type()), domain.data(), desc.data(), url.data());
}

// ---------------------------------------------------------------------------
// Networking context. EmptyClients.cpp defines an EmptyFrameNetworkingContext
// whose storageSession() returns nullptr; we reuse the same shape. (cookie /
// credential support is a later milestone — see report "STILL MISSING".)
// ---------------------------------------------------------------------------
namespace {

class LoadingFrameNetworkingContext : public FrameNetworkingContext {
public:
    static Ref<LoadingFrameNetworkingContext> create() { return adoptRef(*new LoadingFrameNetworkingContext); }

private:
    LoadingFrameNetworkingContext()
        : FrameNetworkingContext { nullptr }
    { }

    bool shouldClearReferrerOnHTTPSToHTTPRedirect() const final { return true; }
    NetworkStorageSession* storageSession() const final { return nullptr; }

    // Pure-virtual on PLATFORM(WIN): a generic blocked-load error is fine for the
    // headless render path (we never actually block requests here).
    ResourceError blockedError(const ResourceRequest& request) const final
    {
        return ResourceError("WebKitInternal"_s, 0, request.url(), "Request blocked"_s, ResourceError::Type::Cancellation);
    }
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// BEHAVIORAL CHANGES (vs EmptyFrameLoaderClient)
// ---------------------------------------------------------------------------

// Approve the navigation so the provisional load actually starts.
void LoadingFrameLoaderClient::dispatchDecidePolicyForNavigationAction(const NavigationAction&, const ResourceRequest&, const ResourceResponse&, FormState*, const String&, std::optional<NavigationIdentifier>, std::optional<HitTestResult>&&, bool, NavigationUpgradeToHTTPSBehavior, SandboxFlags, PolicyDecisionMode, FramePolicyFunction&& policyFunction)
{
    policyFunction(PolicyAction::Use);
}

// Approve the response so the body is committed to the DocumentLoader.
void LoadingFrameLoaderClient::dispatchDecidePolicyForResponse(const ResourceResponse&, const ResourceRequest&, const String&, FramePolicyFunction&& policyFunction)
{
    policyFunction(PolicyAction::Use);
}

// We have no UI to open new windows in a headless render; ignore the request
// (but still complete the policy check so the loader is not left hanging).
void LoadingFrameLoaderClient::dispatchDecidePolicyForNewWindowAction(const NavigationAction&, const ResourceRequest&, FormState*, const String&, std::optional<HitTestResult>&&, FramePolicyFunction&& policyFunction)
{
    policyFunction(PolicyAction::Ignore);
}

bool LoadingFrameLoaderClient::canHandleRequest(const ResourceRequest&) const
{
    return true;
}

bool LoadingFrameLoaderClient::canShowMIMEType(const String&) const
{
    return true;
}

bool LoadingFrameLoaderClient::canShowMIMETypeAsHTML(const String&) const
{
    return true;
}

// Same as Empty: just construct the DocumentLoader.
Ref<DocumentLoader> LoadingFrameLoaderClient::createDocumentLoader(ResourceRequest&& request, SubstituteData&& substituteData)
{
    return DocumentLoader::create(WTF::move(request), WTF::move(substituteData));
}

// Load-completion: set the poll flags AND fire the optional completion handler
// (exactly once) so the driver can either poll or run/stop the RunLoop.
void LoadingFrameLoaderClient::signalLoadComplete(bool failed)
{
    if (m_loadFinished)
        return;
    m_loadFinished = true;
    m_loadFailed = failed;
    if (m_loadCompletionHandler) {
        // Move out before invoking so a re-entrant terminal dispatch is a no-op.
        auto handler = WTF::move(m_loadCompletionHandler);
        handler(failed);
    }
}

void LoadingFrameLoaderClient::dispatchDidFinishLoad()
{
    perfNavLoadEvent();   // Apotheosis (M4): ms_net_load — before the completion handler stops the pump
    signalLoadComplete(false);
}

void LoadingFrameLoaderClient::dispatchDidFailLoad(const ResourceError& error)
{
    recordNetError(error);
    signalLoadComplete(true);
}

void LoadingFrameLoaderClient::dispatchDidFailProvisionalLoad(const ResourceError& error, WillContinueLoading willContinue, WillInternallyHandleFailure)
{
    recordNetError(error);
    // Apotheosis: 服务器重定向(尤其 http→https 301,跨 scheme=换源)会以"取消"(Type::Cancellation)
    //   失败掉当前 provisional load,但紧接着续起新的 provisional load(WillContinueLoading::Yes)。
    //   原来一律 signalLoadComplete(true) → 把这种"将继续"的中间取消误判成加载失败,导致 gov.cn 等
    //   301→https 站点白报 -10。故:将继续 / 取消 类一律不终结,等真正 didFinishLoad 或真实网络错误
    //   (curl 非取消错误仍立即终结显示错误页);真卡住由 pumpLoop 的看门狗兜底。
    if (willContinue == WillContinueLoading::Yes || error.isCancellation())
        return;
    signalLoadComplete(true);
}

// This is not an Empty client.
bool LoadingFrameLoaderClient::isEmptyFrameLoaderClient() const
{
    return false;
}

// ---------------------------------------------------------------------------
// Everything below is the verbatim EmptyFrameLoaderClient no-op behavior.
// ---------------------------------------------------------------------------

RefPtr<LocalFrame> LoadingFrameLoaderClient::createFrame(const AtomString&, HTMLFrameOwnerElement&)
{
    return nullptr;
}

RefPtr<Widget> LoadingFrameLoaderClient::createPlugin(HTMLPlugInElement&, const URL&, const Vector<AtomString>&, const Vector<AtomString>&, const String&, bool)
{
    return nullptr;
}

bool LoadingFrameLoaderClient::hasWebView() const
{
    return true; // mainly for assertions
}

void LoadingFrameLoaderClient::makeRepresentation(DocumentLoader*)
{
}

#if PLATFORM(IOS_FAMILY)

bool LoadingFrameLoaderClient::forceLayoutOnRestoreFromBackForwardCache()
{
    return false;
}

#endif

void LoadingFrameLoaderClient::forceLayoutForNonHTML()
{
}

void LoadingFrameLoaderClient::setCopiesOnScroll()
{
}

void LoadingFrameLoaderClient::detachedFromParent2()
{
}

void LoadingFrameLoaderClient::detachedFromParent3()
{
}

void LoadingFrameLoaderClient::convertMainResourceLoadToDownload(DocumentLoader*, const ResourceRequest&, const ResourceResponse&)
{
}

void LoadingFrameLoaderClient::assignIdentifierToInitialRequest(ResourceLoaderIdentifier, DocumentLoader*, const ResourceRequest&)
{
}

bool LoadingFrameLoaderClient::shouldUseCredentialStorage(DocumentLoader*, ResourceLoaderIdentifier)
{
    return false;
}

void LoadingFrameLoaderClient::dispatchWillSendRequest(DocumentLoader*, ResourceLoaderIdentifier, ResourceRequest&, const ResourceResponse&)
{
}

void LoadingFrameLoaderClient::dispatchDidReceiveAuthenticationChallenge(DocumentLoader*, ResourceLoaderIdentifier, const AuthenticationChallenge&)
{
}

#if USE(PROTECTION_SPACE_AUTH_CALLBACK)

bool LoadingFrameLoaderClient::canAuthenticateAgainstProtectionSpace(DocumentLoader*, ResourceLoaderIdentifier, const ProtectionSpace&)
{
    return false;
}

#endif

#if PLATFORM(IOS_FAMILY)

RetainPtr<CFDictionaryRef> LoadingFrameLoaderClient::connectionProperties(DocumentLoader*, ResourceLoaderIdentifier)
{
    return nullptr;
}

#endif

void LoadingFrameLoaderClient::dispatchDidReceiveResponse(DocumentLoader*, ResourceLoaderIdentifier, const ResourceResponse&)
{
}

void LoadingFrameLoaderClient::dispatchDidReceiveContentLength(DocumentLoader*, ResourceLoaderIdentifier, int)
{
}

void LoadingFrameLoaderClient::dispatchDidFinishLoading(DocumentLoader*, ResourceLoaderIdentifier)
{
}

#if ENABLE(DATA_DETECTION)

void LoadingFrameLoaderClient::dispatchDidFinishDataDetection(NSArray *)
{
}

#endif

void LoadingFrameLoaderClient::dispatchDidFailLoading(DocumentLoader*, ResourceLoaderIdentifier, const ResourceError& error)
{
    recordNetError(error);
}

bool LoadingFrameLoaderClient::dispatchDidLoadResourceFromMemoryCache(DocumentLoader*, const ResourceRequest&, const ResourceResponse&, int)
{
    return false;
}

void LoadingFrameLoaderClient::dispatchDidDispatchOnloadEvents()
{
}

void LoadingFrameLoaderClient::dispatchDidReceiveServerRedirectForProvisionalLoad()
{
}

void LoadingFrameLoaderClient::dispatchDidCancelClientRedirect()
{
}

void LoadingFrameLoaderClient::dispatchWillPerformClientRedirect(const URL&, double, WallTime, LockBackForwardList)
{
}

void LoadingFrameLoaderClient::dispatchDidChangeLocationWithinPage()
{
}

void LoadingFrameLoaderClient::dispatchDidPushStateWithinPage()
{
}

void LoadingFrameLoaderClient::dispatchDidReplaceStateWithinPage()
{
}

void LoadingFrameLoaderClient::dispatchDidPopStateWithinPage()
{
}

void LoadingFrameLoaderClient::dispatchWillClose()
{
}

void LoadingFrameLoaderClient::dispatchDidStartProvisionalLoad()
{
    // Apotheosis (M4): start of the network clock — the driver measures the
    // net_commit / net_load columns from here, so page setup before the load
    // does not leak into them. No-op unless perf logging is on.
    perfNavStart();
}

void LoadingFrameLoaderClient::dispatchDidReceiveTitle(const StringWithDirection&)
{
}

void LoadingFrameLoaderClient::dispatchDidCommitLoad(std::optional<HasInsecureContent>, std::optional<UsedLegacyTLS>, std::optional<WasPrivateRelayed>)
{
    perfNavCommit();          // Apotheosis (M4): ms_net_commit
}

void LoadingFrameLoaderClient::dispatchDidFinishDocumentLoad()
{
    perfNavDocumentReady();   // Apotheosis (M4): DOM ready (ms_net_load fallback)
}

// Apotheosis (M4 load timeline): t_firstpaint. Only the milestones a client requested via
// Page::addLayoutMilestones ever reach here - WebCoreDriver::buildSession asks for
// DidFirstVisuallyNonEmptyLayout, which LocalFrameView fires the first time the laid-out
// content qualifies as visually non-empty. That is the engine-side "something readable is on
// screen" mark, and it costs nothing when perf logging is off (perfNavVisuallyNonEmpty
// returns on the g_perfOn branch).
void LoadingFrameLoaderClient::dispatchDidReachLayoutMilestone(OptionSet<LayoutMilestone> milestones)
{
    if (milestones.contains(LayoutMilestone::DidFirstVisuallyNonEmptyLayout))
        perfNavVisuallyNonEmpty();
}

void LoadingFrameLoaderClient::dispatchDidReachVisuallyNonEmptyState()
{
}

LocalFrame* LoadingFrameLoaderClient::dispatchCreatePage(const NavigationAction&, NewFrameOpenerPolicy)
{
    return nullptr;
}

void LoadingFrameLoaderClient::dispatchShow()
{
}

void LoadingFrameLoaderClient::updateSandboxFlags(SandboxFlags)
{
}

void LoadingFrameLoaderClient::updateOpener(std::optional<FrameIdentifier>)
{
}

void LoadingFrameLoaderClient::setPrinting(bool, FloatSize, FloatSize, float, AdjustViewSize)
{
}

void LoadingFrameLoaderClient::cancelPolicyCheck()
{
}

void LoadingFrameLoaderClient::dispatchUnableToImplementPolicy(const ResourceError&)
{
}

void LoadingFrameLoaderClient::dispatchWillSendSubmitEvent(Ref<FormState>&&)
{
}

void LoadingFrameLoaderClient::dispatchWillSubmitForm(FormState&, URL&&, String&&, CompletionHandler<void()>&& completionHandler)
{
    completionHandler();
}

void LoadingFrameLoaderClient::revertToProvisionalState(DocumentLoader*)
{
}

void LoadingFrameLoaderClient::setMainDocumentError(DocumentLoader*, const ResourceError&)
{
}

void LoadingFrameLoaderClient::setMainFrameDocumentReady(bool)
{
}

void LoadingFrameLoaderClient::startDownload(const ResourceRequest&, const String&, FromDownloadAttribute)
{
}

void LoadingFrameLoaderClient::willChangeTitle(DocumentLoader*)
{
}

void LoadingFrameLoaderClient::didChangeTitle(DocumentLoader*)
{
}

void LoadingFrameLoaderClient::willReplaceMultipartContent()
{
}

void LoadingFrameLoaderClient::didReplaceMultipartContent()
{
}

void LoadingFrameLoaderClient::committedLoad(DocumentLoader* loader, const SharedBuffer& data)
{
    // Apotheosis: Empty 版是空操作 → 网络响应数据被丢弃,文档永远空白(白屏)。把收到的
    // 数据 commit 给 DocumentLoader,由它驱动 DocumentWriter/解析器,文档才有内容可渲染。
    if (loader)
        loader->commitData(data);
}

void LoadingFrameLoaderClient::finishedLoading(DocumentLoader*)
{
}

bool LoadingFrameLoaderClient::shouldFallBack(const ResourceError&) const
{
    return false;
}

void LoadingFrameLoaderClient::loadStorageAccessQuirksIfNeeded()
{
}

bool LoadingFrameLoaderClient::representationExistsForURLScheme(StringView) const
{
    return false;
}

String LoadingFrameLoaderClient::generatedMIMETypeForURLScheme(StringView) const
{
    return emptyString();
}

void LoadingFrameLoaderClient::frameLoadCompleted()
{
}

void LoadingFrameLoaderClient::restoreViewState()
{
}

void LoadingFrameLoaderClient::provisionalLoadStarted()
{
}

void LoadingFrameLoaderClient::didFinishLoad()
{
}

void LoadingFrameLoaderClient::prepareForDataSourceReplacement()
{
}

void LoadingFrameLoaderClient::updateCachedDocumentLoader(DocumentLoader&)
{
}

void LoadingFrameLoaderClient::setTitle(const StringWithDirection&, const URL&)
{
}

extern "C" bool g_apoUaMobile;        // WebCoreDriver.cpp 定义;UI 可切换手机/桌面,切后重载生效
extern "C" char g_apoCustomUA[2048];  // WebCoreDriver.cpp 定义;非空则覆盖 mobile/desktop(WebCoreSetUserAgentString 设)

String LoadingFrameLoaderClient::userAgent(const URL&) const
{
    // Apotheosis: 自定义 UA 优先(用户在设置里填,绕开按 UA 拦截的站点,如 microsoft)。
    if (g_apoCustomUA[0])
        return String::fromUTF8(g_apoCustomUA);
    // 默认移动版 iPhone Safari UA——站点发移动布局(更窄更轻,适配 ~390px 视口)。
    if (g_apoUaMobile)
        return "Mozilla/5.0 (iPhone; CPU iPhone OS 16_4 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/16.4 Mobile/15E148 Safari/604.1"_s;
    // 桌面版:用当代 Edge/Chromium UA(旧 Safari-on-Windows 串会被 microsoft 等站点拦)。
    return "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36 Edg/120.0.0.0"_s;
}

void LoadingFrameLoaderClient::savePlatformDataToCachedFrame(CachedFrame*)
{
}

void LoadingFrameLoaderClient::transitionToCommittedFromCachedFrame(CachedFrame*)
{
}

#if PLATFORM(IOS_FAMILY)

void LoadingFrameLoaderClient::didRestoreFrameHierarchyForCachedFrame()
{
}

#endif

void LoadingFrameLoaderClient::transitionToCommittedForNewPage(InitializingIframe)
{
}

void LoadingFrameLoaderClient::didRestoreFromBackForwardCache()
{
}

void LoadingFrameLoaderClient::updateGlobalHistory()
{
}

void LoadingFrameLoaderClient::updateGlobalHistoryRedirectLinks()
{
}

ShouldGoToHistoryItem LoadingFrameLoaderClient::shouldGoToHistoryItem(HistoryItem&, IsSameDocumentNavigation, ProcessSwapDisposition) const
{
    return ShouldGoToHistoryItem::No;
}

bool LoadingFrameLoaderClient::supportsAsyncShouldGoToHistoryItem() const
{
    return false;
}

void LoadingFrameLoaderClient::shouldGoToHistoryItemAsync(HistoryItem&, CompletionHandler<void(ShouldGoToHistoryItem)>&& completionHandler) const
{
    // Apotheosis (M4): was RELEASE_ASSERT_NOT_REACHED() - i.e. std::abort() without a dump the
    // moment a page triggers a history navigation (history.go/back, some SPA routers). Allow it.
    completionHandler(ShouldGoToHistoryItem::Yes);
}

void LoadingFrameLoaderClient::saveViewStateToItem(HistoryItem&)
{
}

bool LoadingFrameLoaderClient::canCachePage() const
{
    return false;
}

ObjectContentType LoadingFrameLoaderClient::objectContentType(const URL&, const String&)
{
    return ObjectContentType::None;
}

AtomString LoadingFrameLoaderClient::overrideMediaType() const
{
    return nullAtom();
}

void LoadingFrameLoaderClient::redirectDataToPlugin(Widget&)
{
}

void LoadingFrameLoaderClient::dispatchDidClearWindowObjectInWorld(DOMWrapperWorld&)
{
}

#if PLATFORM(COCOA)

RemoteAXObjectRef LoadingFrameLoaderClient::accessibilityRemoteObject()
{
    return nullptr;
}

IntPoint LoadingFrameLoaderClient::accessibilityRemoteFrameOffset()
{
    return { };
}

#if ENABLE(ACCESSIBILITY_ISOLATED_TREE)
void LoadingFrameLoaderClient::setIsolatedTree(Ref<WebCore::AXIsolatedTree>&&)
{
}

RefPtr<WebCore::AXIsolatedTree> LoadingFrameLoaderClient::isolatedTree() const
{
    return nullptr;
}
#endif

void LoadingFrameLoaderClient::willCacheResponse(DocumentLoader*, ResourceLoaderIdentifier, NSCachedURLResponse *response, CompletionHandler<void(NSCachedURLResponse *)>&& completionHandler) const
{
    completionHandler(response);
}

#endif

void LoadingFrameLoaderClient::prefetchDNS(const String& hostname)
{
    // Apotheosis: real DNS prefetch (background getaddrinfo, warms the OS resolver
    // cache that libcurl hits next). Preconnect beyond DNS is not possible with libcurl
    // - see WebResourceLoadScheduler::preconnectTo().
    ::apotheosisPrefetchDNS(hostname);
}

RefPtr<HistoryItem> LoadingFrameLoaderClient::createHistoryItemTree(bool, BackForwardItemIdentifier) const
{
    return nullptr;
}

#if USE(QUICK_LOOK)

RefPtr<LegacyPreviewLoaderClient> LoadingFrameLoaderClient::createPreviewLoaderClient(const String&, const String&)
{
    return nullptr;
}

#endif

bool LoadingFrameLoaderClient::hasFrameSpecificStorageAccess()
{
    return false;
}

void LoadingFrameLoaderClient::revokeFrameSpecificStorageAccess()
{
}

void LoadingFrameLoaderClient::dispatchLoadEventToOwnerElementInAnotherProcess()
{
}

Ref<FrameNetworkingContext> LoadingFrameLoaderClient::createNetworkingContext()
{
    // cookie 持久化:返回带真 storageSession 的 networking context(HTTP Cookie/Set-Cookie 头经此到达 jar)。
    // frame 传 nullptr(同原 LoadingFrameNetworkingContext;networking context 只用 storageSession)。
    return WebCorePort::makeFrameNetworkingContext(nullptr);
}

void LoadingFrameLoaderClient::sendH2Ping(const URL& url, CompletionHandler<void(Expected<Seconds, ResourceError>&&)>&& completionHandler)
{
    ASSERT_NOT_REACHED();
    completionHandler(makeUnexpected(internalError(url)));
}

} // namespace WebCorePort
