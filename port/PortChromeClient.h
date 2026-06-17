/*
 * PortChromeClient.h — headless WebKit 2.52.4 ChromeClient for the
 * Windows 10 Mobile ARM32 UWP GPU-compositing port (milestone M1).
 *
 * Subclasses WebCore::ChromeClient directly (NOT EmptyChromeClient, whose
 * compositing hooks are `final {}` no-ops and therefore un-overridable).
 * Every pure-virtual of ChromeClient is implemented as a no-op mirroring
 * EmptyChromeClient, EXCEPT the accelerated-compositing hooks, which capture
 * the root GraphicsLayer and the "needs present" flag for the GPU driver.
 */

#pragma once

#include <WebCore/ChromeClient.h>
#include <WebCore/FocusOptions.h>
#include <wtf/CompletionHandler.h>
#include <wtf/FastMalloc.h>

namespace WebCore {
class GraphicsLayer;
class GraphicsLayerFactory;
enum class BroadcastFocusedElement : bool;
struct FocusOptions;
}

namespace WebCorePort {

class PortChromeClient final : public WebCore::ChromeClient {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(PortChromeClient);
public:
    PortChromeClient() = default;
    ~PortChromeClient() final = default;

    // ---- driver-facing accessors (the whole point of this subclass) ----
    WebCore::GraphicsLayer* rootLayer() const { return m_rootLayer; }
    bool takeNeedsPresent() { bool v = m_needsPresent; m_needsPresent = false; return v; }

    // ======================================================================
    // REAL accelerated-compositing behavior
    // ======================================================================

    // Pass nullptr as the GraphicsLayer to detach the root layer.
    void attachRootGraphicsLayer(WebCore::LocalFrame&, WebCore::GraphicsLayer* layer) final { m_rootLayer = layer; }
    void attachViewOverlayGraphicsLayer(WebCore::GraphicsLayer*) final { }
    void setNeedsOneShotDrawingSynchronization() final { m_needsPresent = true; }
    void triggerRenderingUpdate() final { m_needsPresent = true; }

    // Use the default GraphicsLayerTextureMapper factory.
    WebCore::GraphicsLayerFactory* graphicsLayerFactory() const final { return nullptr; }

    bool allowsAcceleratedCompositing() const final { return true; }
    WebCore::ChromeClient::CompositingTriggerFlags allowedCompositingTriggers() const final
    {
        return static_cast<WebCore::ChromeClient::CompositingTriggerFlags>(WebCore::ChromeClient::AllTriggers);
    }

    // ======================================================================
    // No-op pure-virtuals (mirror of EmptyChromeClient)
    // ======================================================================

    void chromeDestroyed() final { }

    void setWindowRect(const WebCore::FloatRect&) final { }
    WebCore::FloatRect windowRect() const final { return WebCore::FloatRect(); }

    WebCore::FloatRect pageRect() const final { return WebCore::FloatRect(); }

    void focus() final { }
    void unfocus() final { }

    bool canTakeFocus(WebCore::FocusDirection) const final { return false; }
    void takeFocus(WebCore::FocusDirection) final { }

    void focusedElementChanged(WebCore::Element*, WebCore::LocalFrame*, WebCore::FocusOptions, WebCore::BroadcastFocusedElement) final { }
    void focusedFrameChanged(WebCore::Frame*) final { }

    RefPtr<WebCore::Page> createWindow(WebCore::LocalFrame&, const String&, const WebCore::WindowFeatures&, const WebCore::NavigationAction&) final { return nullptr; }
    void show() final { }

    bool canRunModal() const final { return false; }
    void runModal() final { }

    bool toolbarsVisible() const final { return false; }
    bool statusbarVisible() const final { return false; }
    bool scrollbarsVisible() const final { return false; }
    bool menubarVisible() const final { return false; }

    void setResizable(bool) final { }

    void addMessageToConsole(JSC::MessageSource, JSC::MessageLevel, const String&, unsigned, unsigned, const String&) final { }

    bool canRunBeforeUnloadConfirmPanel() final { return false; }
    bool runBeforeUnloadConfirmPanel(String&&, WebCore::LocalFrame&) final { return true; }

    void closeWindow() final { }

    void rootFrameAdded(const WebCore::LocalFrame&) final { }
    void rootFrameRemoved(const WebCore::LocalFrame&) final { }

    void runJavaScriptAlert(WebCore::LocalFrame&, const String&) final { }
    bool runJavaScriptConfirm(WebCore::LocalFrame&, const String&) final { return false; }
    bool runJavaScriptPrompt(WebCore::LocalFrame&, const String&, const String&, String&) final { return false; }

    bool selectItemWritingDirectionIsNatural() final { return false; }
    bool selectItemAlignmentFollowsMenuWritingDirection() final { return false; }
    RefPtr<WebCore::PopupMenu> createPopupMenu(WebCore::PopupMenuClient&) const final;
    RefPtr<WebCore::SearchPopupMenu> createSearchPopupMenu(WebCore::PopupMenuClient&) const final;

    WebCore::KeyboardUIMode keyboardUIMode() final { return WebCore::KeyboardAccessDefault; }

    bool hasAccessoryMousePointingDevice() const final { return false; }
    bool hoverSupportedByPrimaryPointingDevice() const final { return false; }
    bool hoverSupportedByAnyAvailablePointingDevice() const final { return false; }
    std::optional<WebCore::PointerCharacteristics> pointerCharacteristicsOfPrimaryPointingDevice() const final { return std::nullopt; }
    OptionSet<WebCore::PointerCharacteristics> pointerCharacteristicsOfAllAvailablePointingDevices() const final { return { }; }

    void invalidateRootView(const WebCore::IntRect&) final { }
    void invalidateContentsAndRootView(const WebCore::IntRect&) final { }
    void invalidateContentsForSlowScroll(const WebCore::IntRect&) final { }
    void scroll(const WebCore::IntSize&, const WebCore::IntRect&, const WebCore::IntRect&) final { }

    WebCore::IntPoint screenToRootView(const WebCore::IntPoint& p) const final { return p; }
    WebCore::IntPoint rootViewToScreen(const WebCore::IntPoint& p) const final { return p; }
    WebCore::IntRect rootViewToScreen(const WebCore::IntRect& r) const final { return r; }
    WebCore::IntPoint accessibilityScreenToRootView(const WebCore::IntPoint& p) const final { return p; }
    WebCore::IntRect rootViewToAccessibilityScreen(const WebCore::IntRect& r) const final { return r; }

    void didFinishLoadingImageForElement(WebCore::HTMLImageElement&) final { }

    PlatformPageClient platformPageClient() const final { return 0; }
    void contentsSizeChanged(WebCore::LocalFrame&, const WebCore::IntSize&) const final { }
    void intrinsicContentsSizeChanged(const WebCore::IntSize&) const final { }

    void mouseDidMoveOverElement(const WebCore::HitTestResult&, OptionSet<WebCore::PlatformEventModifier>, const String&, WebCore::TextDirection) final { }

    void print(WebCore::LocalFrame&, const WebCore::StringWithDirection&) final { }

    void exceededDatabaseQuota(WebCore::LocalFrame&, const String&, WebCore::DatabaseDetails) final { }

    void reachedMaxAppCacheSize(int64_t) final { }

    RefPtr<WebCore::ColorChooser> createColorChooser(WebCore::ColorChooserClient&, const WebCore::Color&) final;

    RefPtr<WebCore::DataListSuggestionPicker> createDataListSuggestionPicker(WebCore::DataListSuggestionsClient&) final;
    bool canShowDataListSuggestionLabels() const final { return false; }

    RefPtr<WebCore::DateTimeChooser> createDateTimeChooser(WebCore::DateTimeChooserClient&) final;

    void setTextIndicator(RefPtr<WebCore::TextIndicator>&&) const final;
    void updateTextIndicator(RefPtr<WebCore::TextIndicator>&&) const final;

    void runOpenPanel(WebCore::LocalFrame&, WebCore::FileChooser&) final;
    void showShareSheet(WebCore::ShareDataWithParsedURL&&, CompletionHandler<void(bool)>&&) final;
    void loadIconForFiles(const Vector<String>&, WebCore::FileIconLoader&) final { }

    void elementDidFocus(WebCore::Element&, const WebCore::FocusOptions&) final { }
    void elementDidBlur(WebCore::Element&) final { }

    void setCursor(const WebCore::Cursor&) final { }
    void setCursorHiddenUntilMouseMoves(bool) final { }

    void scrollContainingScrollViewsToRevealRect(const WebCore::IntRect&) const final { }
    void scrollMainFrameToRevealRect(const WebCore::IntRect&) const final { }

#if PLATFORM(WIN)
    void AXStartFrameLoad() final { }
    void AXFinishFrameLoad() final { }
#endif

    void wheelEventHandlersChanged(bool) final { }

    void didAssociateFormControls(const Vector<Ref<WebCore::Element>>&, WebCore::LocalFrame&) final { }
    bool shouldNotifyOnFormChanges() final { return false; }

    RefPtr<WebCore::Icon> createIconForFiles(const Vector<String>&) final;

    void requestCookieConsent(CompletionHandler<void(WebCore::CookieConsentDecisionResult)>&&) final;

private:
    WebCore::GraphicsLayer* m_rootLayer { nullptr };
    bool m_needsPresent { false };
};

} // namespace WebCorePort
