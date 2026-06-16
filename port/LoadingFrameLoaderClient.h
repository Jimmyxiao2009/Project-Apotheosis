// ============================================================================
// LoadingFrameLoaderClient.h
//
// A non-Empty LocalFrameLoaderClient for the in-process WinUWP render driver.
//
// It is a verbatim copy of WebCore::EmptyFrameLoaderClient (the override set in
// Source/WebCore/loader/EmptyFrameLoaderClient.h, implementations in
// EmptyClients.cpp) with exactly these behavioral changes so that a *real*
// network navigation (DocumentLoader -> SubresourceLoader -> ResourceHandle ->
// curl) actually progresses to completion instead of stalling on policy checks:
//
//   - dispatchDecidePolicyForNavigationAction -> policyFunction(PolicyAction::Use)
//   - dispatchDecidePolicyForResponse         -> policyFunction(PolicyAction::Use)
//   - dispatchDecidePolicyForNewWindowAction  -> policyFunction(PolicyAction::Ignore)
//   - canHandleRequest()  -> true
//   - canShowMIMEType()   -> true   (so text/html etc. are treated as showable)
//   - canShowMIMETypeAsHTML() -> true
//   - createDocumentLoader() -> DocumentLoader::create (same as Empty)
//   - dispatchDidFinishLoad()/dispatchDidFailLoad()/dispatchDidFailProvisionalLoad()
//     set m_loadFinished / m_loadFailed so the driver can poll for completion.
//
// Everything else is the Empty no-op behavior. `final` has been dropped from the
// class and from every method so the driver can subclass/observe if needed.
//
// NOTE: isEmptyFrameLoaderClient() deliberately returns *false* here. Returning
// true would make WebCore treat the frame as a throwaway empty frame and short-
// circuit parts of the real load path.
// ============================================================================

#pragma once

#include <WebCore/LocalFrameLoaderClient.h>
#include <wtf/Function.h>
#include <wtf/Platform.h>

namespace WebCorePort {

class LoadingFrameLoaderClient : public WebCore::LocalFrameLoaderClient {
public:
    explicit LoadingFrameLoaderClient(WebCore::FrameLoader& frameLoader)
        : WebCore::LocalFrameLoaderClient(frameLoader)
    { }

    // ---- driver-pollable load state -------------------------------------
    bool loadFinished() const { return m_loadFinished; }
    bool loadFailed() const { return m_loadFailed; }
    void resetLoadState() { m_loadFinished = false; m_loadFailed = false; }

    // ---- optional completion callback -----------------------------------
    // Invoked at most once, on the main thread, from dispatchDidFinishLoad() /
    // dispatchDidFailLoad() / dispatchDidFailProvisionalLoad(). Lets the driver
    // pump RunLoop::run() and stop() on terminal state instead of busy-polling.
    void setLoadCompletionHandler(WTF::Function<void(bool failed)>&& handler) { m_loadCompletionHandler = WTF::move(handler); }

private:
    Ref<WebCore::DocumentLoader> createDocumentLoader(WebCore::ResourceRequest&&, WebCore::SubstituteData&&) override;

    bool hasWebView() const override;

    void makeRepresentation(WebCore::DocumentLoader*) override;

#if PLATFORM(IOS_FAMILY)
    bool forceLayoutOnRestoreFromBackForwardCache() override;
#endif

    void forceLayoutForNonHTML() override;

    void setCopiesOnScroll() override;

    void detachedFromParent2() override;
    void detachedFromParent3() override;

    void convertMainResourceLoadToDownload(WebCore::DocumentLoader*, const WebCore::ResourceRequest&, const WebCore::ResourceResponse&) override;

    void assignIdentifierToInitialRequest(WebCore::ResourceLoaderIdentifier, WebCore::DocumentLoader*, const WebCore::ResourceRequest&) override;
    bool shouldUseCredentialStorage(WebCore::DocumentLoader*, WebCore::ResourceLoaderIdentifier) override;
    void dispatchWillSendRequest(WebCore::DocumentLoader*, WebCore::ResourceLoaderIdentifier, WebCore::ResourceRequest&, const WebCore::ResourceResponse&) override;
    void dispatchDidReceiveAuthenticationChallenge(WebCore::DocumentLoader*, WebCore::ResourceLoaderIdentifier, const WebCore::AuthenticationChallenge&) override;
#if USE(PROTECTION_SPACE_AUTH_CALLBACK)
    bool canAuthenticateAgainstProtectionSpace(WebCore::DocumentLoader*, WebCore::ResourceLoaderIdentifier, const WebCore::ProtectionSpace&) override;
#endif

#if PLATFORM(IOS_FAMILY)
    RetainPtr<CFDictionaryRef> connectionProperties(WebCore::DocumentLoader*, WebCore::ResourceLoaderIdentifier) override;
#endif

    void dispatchDidReceiveResponse(WebCore::DocumentLoader*, WebCore::ResourceLoaderIdentifier, const WebCore::ResourceResponse&) override;
    void dispatchDidReceiveContentLength(WebCore::DocumentLoader*, WebCore::ResourceLoaderIdentifier, int) override;
    void dispatchDidFinishLoading(WebCore::DocumentLoader*, WebCore::ResourceLoaderIdentifier) override;
#if ENABLE(DATA_DETECTION)
    void dispatchDidFinishDataDetection(NSArray *) override;
#endif
    void dispatchDidFailLoading(WebCore::DocumentLoader*, WebCore::ResourceLoaderIdentifier, const WebCore::ResourceError&) override;
    bool dispatchDidLoadResourceFromMemoryCache(WebCore::DocumentLoader*, const WebCore::ResourceRequest&, const WebCore::ResourceResponse&, int) override;

    void dispatchDidDispatchOnloadEvents() override;
    void dispatchDidReceiveServerRedirectForProvisionalLoad() override;
    void dispatchDidCancelClientRedirect() override;
    void dispatchWillPerformClientRedirect(const URL&, double, WallTime, WebCore::LockBackForwardList) override;
    void dispatchDidChangeLocationWithinPage() override;
    void dispatchDidPushStateWithinPage() override;
    void dispatchDidReplaceStateWithinPage() override;
    void dispatchDidPopStateWithinPage() override;
    void dispatchWillClose() override;
    void dispatchDidStartProvisionalLoad() override;
    void dispatchDidReceiveTitle(const WebCore::StringWithDirection&) override;
    void dispatchDidCommitLoad(std::optional<WebCore::HasInsecureContent>, std::optional<WebCore::UsedLegacyTLS>, std::optional<WebCore::WasPrivateRelayed>) override;
    void dispatchDidFailProvisionalLoad(const WebCore::ResourceError&, WebCore::WillContinueLoading, WebCore::WillInternallyHandleFailure) override;
    void dispatchDidFailLoad(const WebCore::ResourceError&) override;
    void dispatchDidFinishDocumentLoad() override;
    void dispatchDidFinishLoad() override;
    void dispatchDidReachLayoutMilestone(OptionSet<WebCore::LayoutMilestone>) override;
    void dispatchDidReachVisuallyNonEmptyState() override;

    WebCore::LocalFrame* dispatchCreatePage(const WebCore::NavigationAction&, WebCore::NewFrameOpenerPolicy) override;
    void dispatchShow() override;

    void dispatchDecidePolicyForResponse(const WebCore::ResourceResponse&, const WebCore::ResourceRequest&, const String&, WebCore::FramePolicyFunction&&) override;
    void dispatchDecidePolicyForNewWindowAction(const WebCore::NavigationAction&, const WebCore::ResourceRequest&, WebCore::FormState*, const String&, std::optional<WebCore::HitTestResult>&&, WebCore::FramePolicyFunction&&) override;
    void dispatchDecidePolicyForNavigationAction(const WebCore::NavigationAction&, const WebCore::ResourceRequest&, const WebCore::ResourceResponse& redirectResponse, WebCore::FormState*, const String&, std::optional<WebCore::NavigationIdentifier>, std::optional<WebCore::HitTestResult>&&, bool, WebCore::NavigationUpgradeToHTTPSBehavior, WebCore::SandboxFlags, WebCore::PolicyDecisionMode, WebCore::FramePolicyFunction&&) override;
    void updateSandboxFlags(WebCore::SandboxFlags) override;
    void updateOpener(std::optional<WebCore::FrameIdentifier>) override;
    void setPrinting(bool, WebCore::FloatSize, WebCore::FloatSize, float, WebCore::AdjustViewSize) override;
    void cancelPolicyCheck() override;

    void dispatchUnableToImplementPolicy(const WebCore::ResourceError&) override;

    void dispatchWillSendSubmitEvent(Ref<WebCore::FormState>&&) override;
    void dispatchWillSubmitForm(WebCore::FormState&, URL&& requestURL, String&& method, CompletionHandler<void()>&&) override;

    void revertToProvisionalState(WebCore::DocumentLoader*) override;
    void setMainDocumentError(WebCore::DocumentLoader*, const WebCore::ResourceError&) override;

    void setMainFrameDocumentReady(bool) override;

    void startDownload(const WebCore::ResourceRequest&, const String&, WebCore::FromDownloadAttribute = WebCore::FromDownloadAttribute::No) override;

    void willChangeTitle(WebCore::DocumentLoader*) override;
    void didChangeTitle(WebCore::DocumentLoader*) override;

    void willReplaceMultipartContent() override;
    void didReplaceMultipartContent() override;

    void committedLoad(WebCore::DocumentLoader*, const WebCore::SharedBuffer&) override;
    void finishedLoading(WebCore::DocumentLoader*) override;

    void loadStorageAccessQuirksIfNeeded() override;

    bool shouldFallBack(const WebCore::ResourceError&) const override;

    bool canHandleRequest(const WebCore::ResourceRequest&) const override;
    bool canShowMIMEType(const String&) const override;
    bool canShowMIMETypeAsHTML(const String&) const override;
    bool representationExistsForURLScheme(StringView) const override;
    String generatedMIMETypeForURLScheme(StringView) const override;

    void frameLoadCompleted() override;
    void restoreViewState() override;
    void provisionalLoadStarted() override;
    void didFinishLoad() override;
    void prepareForDataSourceReplacement() override;

    void updateCachedDocumentLoader(WebCore::DocumentLoader&) override;
    void setTitle(const WebCore::StringWithDirection&, const URL&) override;

    String userAgent(const URL&) const override;

    void savePlatformDataToCachedFrame(WebCore::CachedFrame*) override;
    void transitionToCommittedFromCachedFrame(WebCore::CachedFrame*) override;
#if PLATFORM(IOS_FAMILY)
    void didRestoreFrameHierarchyForCachedFrame() override;
#endif
    void transitionToCommittedForNewPage(InitializingIframe) override;

    void didRestoreFromBackForwardCache() override;

    void updateGlobalHistory() override;
    void updateGlobalHistoryRedirectLinks() override;
    WebCore::ShouldGoToHistoryItem shouldGoToHistoryItem(WebCore::HistoryItem&, WebCore::IsSameDocumentNavigation, WebCore::ProcessSwapDisposition) const override;
    bool supportsAsyncShouldGoToHistoryItem() const override;
    void shouldGoToHistoryItemAsync(WebCore::HistoryItem&, CompletionHandler<void(WebCore::ShouldGoToHistoryItem)>&&) const override;

    void saveViewStateToItem(WebCore::HistoryItem&) override;
    bool canCachePage() const override;
    RefPtr<WebCore::LocalFrame> createFrame(const AtomString&, WebCore::HTMLFrameOwnerElement&) override;
    RefPtr<WebCore::Widget> createPlugin(WebCore::HTMLPlugInElement&, const URL&, const Vector<AtomString>&, const Vector<AtomString>&, const String&, bool) override;

    WebCore::ObjectContentType objectContentType(const URL&, const String&) override;
    AtomString overrideMediaType() const override;

    void redirectDataToPlugin(WebCore::Widget&) override;
    void dispatchDidClearWindowObjectInWorld(WebCore::DOMWrapperWorld&) override;

#if PLATFORM(COCOA)
    RemoteAXObjectRef accessibilityRemoteObject() override;
    IntPoint accessibilityRemoteFrameOffset() override;
#if ENABLE(ACCESSIBILITY_ISOLATED_TREE)
    void setIsolatedTree(Ref<WebCore::AXIsolatedTree>&&) override;
    RefPtr<WebCore::AXIsolatedTree> isolatedTree() const override;
#endif
    void willCacheResponse(WebCore::DocumentLoader*, WebCore::ResourceLoaderIdentifier, NSCachedURLResponse *, CompletionHandler<void(NSCachedURLResponse *)>&&) const override;
#endif

    Ref<WebCore::FrameNetworkingContext> createNetworkingContext() override;

    bool isEmptyFrameLoaderClient() const override;
    void prefetchDNS(const String&) override;
    void sendH2Ping(const URL&, CompletionHandler<void(Expected<Seconds, WebCore::ResourceError>&&)>&&) override;

#if USE(QUICK_LOOK)
    RefPtr<WebCore::LegacyPreviewLoaderClient> createPreviewLoaderClient(const String&, const String&) override;
#endif

    bool hasFrameSpecificStorageAccess() override;
    void revokeFrameSpecificStorageAccess() override;

    void dispatchLoadEventToOwnerElementInAnotherProcess() override;

    RefPtr<WebCore::HistoryItem> createHistoryItemTree(bool, WebCore::BackForwardItemIdentifier) const override;

    // ---- driver-pollable load state -------------------------------------
    void signalLoadComplete(bool failed);

    bool m_loadFinished { false };
    bool m_loadFailed { false };
    WTF::Function<void(bool failed)> m_loadCompletionHandler;
};

} // namespace WebCorePort
