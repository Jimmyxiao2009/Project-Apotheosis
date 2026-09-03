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

// Apotheosis: buffered append to LocalState\console.txt, defined in WebCoreDriver.cpp (the
// path/ring/opt-in flag are statics of that TU — see the "JS console" block there). Called by
// PortChromeClient::addMessageToConsole() below. Plain C strings on purpose: keeps this header
// decoupled from the perf/crash-log internals. No-op when console logging is off.
void consoleLogAppend(const char* levelStr, const char* sourceID, unsigned lineNumber, const char* utf8Message);

class PortChromeClient final : public WebCore::ChromeClient {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(PortChromeClient);
public:
    PortChromeClient() = default;
    ~PortChromeClient() final = default;

    // ---- driver-facing accessors (the whole point of this subclass) ----
    WebCore::GraphicsLayer* rootLayer() const { return m_rootLayer; }
    bool takeNeedsPresent() { bool v = m_needsPresent; m_needsPresent = false; return v; }
    // Apotheosis (M4): non-consuming read, lets WebCoreLiveTick decide whether a tick may keep
    // the already-uploaded tiles (nothing asked for a rendering update since the last present).
    bool peekNeedsPresent() const { return m_needsPresent; }
    // Apotheosis (OFFTHREAD-RASTER-LOG.md §4.2): arm a follow-up present from the driver itself.
    // With threaded raster on, a tile replay that lands after the last composite of an operation
    // would otherwise sit in its buffer until something else dirties the page; the driver sets this
    // whenever replays are still in flight, so the next tick composites and uploads them.
    void setNeedsPresent() { m_needsPresent = true; }

    // ======================================================================
    // REAL accelerated-compositing behavior
    // ======================================================================

    // Pass nullptr as the GraphicsLayer to detach the root layer.
    void attachRootGraphicsLayer(WebCore::LocalFrame&, WebCore::GraphicsLayer* layer) final { m_rootLayer = layer; }
    void attachViewOverlayGraphicsLayer(WebCore::GraphicsLayer*) final { }
    void setNeedsOneShotDrawingSynchronization() final { m_needsPresent = true; }
    void triggerRenderingUpdate() final { m_needsPresent = true; }
    // Apotheosis (M4): Page::scheduleRenderingUpdateInternal() asks us first and only falls
    // back to RenderingUpdateScheduler (display link / timer -> triggerRenderingUpdate) when
    // we return false. Every repaint request goes through here - including the one an
    // asynchronous image decode raises via CachedImage::imageFrameAvailable ->
    // RenderImage::imageChanged -> GraphicsLayerTextureMapper::setNeedsDisplayInRect ->
    // notifyFlushRequired. Flag the present right away so the very next harness tick shows the
    // repaired tile instead of waiting for the scheduler's timer; keep returning false so the
    // scheduler still runs exactly as before.
    bool scheduleRenderingUpdate() final { m_needsPresent = true; return false; }

    // Use the default GraphicsLayerTextureMapper factory.
    WebCore::GraphicsLayerFactory* graphicsLayerFactory() const final { return nullptr; }

    bool allowsAcceleratedCompositing() const final { return true; }
    WebCore::ChromeClient::CompositingTriggerFlags allowedCompositingTriggers() const final
    {
        return static_cast<WebCore::ChromeClient::CompositingTriggerFlags>(WebCore::ChromeClient::AllTriggers);
    }
    // 注意:不要给主帧开 tiled backing(shouldUseTiledBackingForFrameView 保持默认 false)。真机实测开了之后
    //   主帧内容被路由进 "Page TiledBacking containment" 的 TileController 瓦片,而同步 GraphicsLayerTextureMapper
    //   合成路径根本不渲染 TileController → 内容照样全丢。根内容不画的真因是 RenderLayerBacking::paintsIntoWindow()
    //   对 TextureMapper 端没返回 false(已在引擎侧打补丁修正),与 tiled backing 无关。

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

    // Apotheosis: mirrors JS console output to LocalState\console.txt (see the "JS console"
    // block in WebCoreDriver.cpp, ahead of pumpLoop(), for the opt-in policy and ring buffer).
    // Out-of-line (PortChromeClient.cpp) because it needs the MessageLevel -> string mapping.
    void addMessageToConsole(JSC::MessageSource, JSC::MessageLevel, const String& message, unsigned lineNumber, unsigned columnNumber, const String& sourceID) final;

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

    void didFinishLoadingImageForElement(WebCore::HTMLImageElement&) final { m_needsPresent = true; }

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
