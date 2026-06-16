// ============================================================================
// webcoreloadurl-draft.cpp  —  fragment to be merged INTO WebCoreDriver.cpp
//
//   extern "C" int WebCoreLoadUrl(const char* url, int w, int h, uint8_t* outRGBA);
//
// Sibling of the existing WebCoreRenderHtml(): instead of feeding a local HTML
// string through the DocumentWriter, this drives a *real network load* of an
// http(s):// URL through the curl backend, pumps the WebKit main-thread run loop
// until the main frame finishes (or a 30 s watchdog fires), then reuses the
// exact same Cairo paint + RGBA swizzle tail as WebCoreRenderHtml().
//
// DEPENDENCY MAP (what must exist before this links):
//   [Core] installPortPlatformStrategies()  ......... draft-PortPlatformStrategies.cpp
//          - installs PlatformStrategies whose createLoaderStrategy() returns
//            WebResourceLoadScheduler (drives ResourceHandle -> CurlRequest).
//          - WITHOUT it, platformStrategies()->loaderStrategy() is null and
//            ResourceLoader::start() crashes. MUST be called once at init.
//   [Core] ResourceHandle.cpp curl patch  .......... draft-ResourceHandleCurl-patch.txt
//          - ResourceHandle::start()/cancel()/curlDid*  (the network body).
//   [Core] stubs-network.cpp  ...................... 24 ResourceHandle::* stubs
//          DELETED (else LNK2005 vs the revived ResourceHandle.cpp).
//   [A]    LoadingFrameLoaderClient  ................ Component A
//          - a concrete LocalFrameLoaderClient (replaces EmptyLocalFrameLoaderClient
//            for the main frame) that ACTUALLY creates DocumentLoaders / lets the
//            FrameLoader run a provisional load, and reports terminal load state.
//          - we only require from it the small "completion contract" below.
//
// COMPLETION CONTRACT required from Component A's LoadingFrameLoaderClient
// (keep this tiny so A and this fragment stay decoupled):
//
//     namespace WebCorePort {
//     struct LoadCompletion {
//         bool   finished = false;   // set true on terminal state
//         bool   failed   = false;   // true if the terminal state was a failure
//     };
//     class LoadingFrameLoaderClient final : public WebCore::LocalFrameLoaderClient {
//     public:
//         // called by the driver before load(); client invokes onDone() exactly
//         // once, on the main thread, from dispatchDidFinishLoad()/dispatchDidFailLoad()/
//         // dispatchDidFailProvisionalLoad().
//         void setLoadCompletionHandler(WTF::Function<void(bool failed)>&& onDone);
//         ...
//     };
//     }
//
// If A instead exposes a public `LoadCompletion& completion()` flag-poll surface,
// see the ALT (poll) variant at the bottom of this file.
// ============================================================================

// ----------------------------------------------------------------------------
// ADDITIONAL INCLUDES (append to the include block already at the top of
// WebCoreDriver.cpp; the ones it already has — Page/LocalFrame/LocalFrameView/
// Document/GraphicsContextCairo/Color/IntRect/IntSize/SharedBuffer/URL/etc — are
// NOT repeated here).
// ----------------------------------------------------------------------------
#include <wtf/RunLoop.h>                       // RunLoop::run / currentSingleton / Timer
#include <wtf/Seconds.h>                        // 30_s
#include <wtf/Function.h>                       // WTF::Function
#include <wtf/UniqueRef.h>                      // makeUniqueRefWithoutRefCountedCheck
#include <wtf/Variant.h>                        // WTF::Variant == std::variant on WK_WINUWP -> std::get OK
#include <WebCore/FrameLoader.h>                // FrameLoader::load(FrameLoadRequest&&)
#include <WebCore/FrameLoadRequest.h>           // FrameLoadRequest
#include <WebCore/ResourceRequest.h>            // ResourceRequest
#include <WebCore/SubstituteData.h>             // SubstituteData
#include <WebCore/LocalFrameLoaderClient.h>     // (base of LoadingFrameLoaderClient)

// Component A's header. Path TBD by A; placeholder:
#include "LoadingFrameLoaderClient.h"           // [A] WebCorePort::LoadingFrameLoaderClient

// Declared/defined by Core (draft-PortPlatformStrategies.cpp). Free function,
// idempotent, installs the PlatformStrategies singleton.
extern void installPortPlatformStrategies();     // [Core]

// ----------------------------------------------------------------------------
// The fragment below lives inside the SAME anonymous namespace as the existing
// driver helpers (so it sees `using namespace WebCore;` and the kErr* enum).
// Shown here re-opening that namespace for clarity.
// ----------------------------------------------------------------------------
namespace {

using namespace WebCore;

// New error codes (extend the existing kErr* enum in WebCoreDriver.cpp instead
// of redeclaring; listed here for reference):
//   kErrBadUrl       = -9,   // URL{url} parsed invalid
//   kErrLoadFailed   = -10,  // terminal load state was a failure
//   kErrLoadTimeout  = -11,  // 30 s watchdog fired before terminal state

// ensureWebCoreInitialized() already exists in WebCoreDriver.cpp. We need the
// platform strategies installed exactly once, alongside JSC/MainThread init.
// RECOMMENDED: add `installPortPlatformStrategies();` INSIDE the existing
// ensureWebCoreInitialized() lambda (after initializeCommonAtomStrings(),
// before populateJITOperations()), e.g.:
//
//     WebCore::initializeCommonAtomStrings();
//     installPortPlatformStrategies();          // <-- ADD [Core] dependency
//     WebCore::populateJITOperations();
//
// Keeping it there guarantees: (a) main thread + RunLoop::main exist first,
// (b) strategies installed before any Page/load, (c) once-only.

} // anonymous namespace


extern "C" {

// Load `url` over the network (curl backend), render the resulting page into a
// w*h RGBA8888 buffer. outRGBA must point to >= w*h*4 bytes. Returns 0 on success.
int WebCoreLoadUrl(const char* url, int w, int h, uint8_t* outRGBA)
{
    using namespace WebCore;

    if (!url || !outRGBA || w <= 0 || h <= 0)
        return /*kErrBadArgs*/ -1;

    // 1. process init (JSC/MainThread/AtomStrings) + installPortPlatformStrategies()
    //    NOTE: relies on the modification to ensureWebCoreInitialized() described
    //    above so the loader strategy is live before we issue a load.
    ensureWebCoreInitialized();

    // Parse + validate the URL up front so we fail fast on garbage input.
    URL parsedURL { String::fromUTF8(url) };
    if (!parsedURL.isValid())
        return /*kErrBadUrl*/ -9;

    // ---- 2. PageConfiguration with empty clients, then swizzle the main-frame
    //         loader-client factory to Component A's LoadingFrameLoaderClient. ----
    auto pageConfiguration = pageConfigurationWithEmptyClients(
        std::nullopt, PAL::SessionID::defaultSessionID());

    // Shared terminal-state signal. Captured by both the client completion handler
    // and the watchdog timer; both run on this (main) thread, so no locking needed.
    struct LoadState {
        bool finished = false;
        bool failed   = false;
        bool timedOut = false;
    } loadState;

    // The client signals completion here. It must call this exactly once, on the
    // main thread, from its dispatchDidFinishLoad()/dispatchDidFailLoad()/
    // dispatchDidFailProvisionalLoad(). We then stop the run loop to fall through
    // to layout+paint.
    auto onLoadDone = [&loadState](bool failed) {
        if (loadState.finished)
            return;
        loadState.finished = true;
        loadState.failed = failed;
        // Unblock RunLoop::run() below. stop() is safe to call on the current
        // (main) RunLoop from within a dispatched callback.
        RunLoop::currentSingleton().stop();
    };

    // Replace the EmptyLocalFrameLoaderClient factory with A's client.
    // mainFrameCreationParameters is a Variant; index 0 is
    // LocalMainFrameCreationParameters{ clientCreator, sandboxFlags, referrerPolicy }.
    // pageConfigurationWithEmptyClients always populates index 0, so we patch its
    // clientCreator in place (preserving sandboxFlags/referrerPolicy defaults).
    //
    // The clientCreator signature is:
    //   CompletionHandler<UniqueRef<LocalFrameLoaderClient>(LocalFrame&, FrameLoader&)>
    //
    // We capture onLoadDone by copy into the creator; it is invoked when the
    // FrameLoader constructs the main frame's client.
    //
    // NB: mirror EmptyClients.cpp:1254-1260 exactly:
    //   - construct with makeUniqueRefWithoutRefCountedCheck<>  (LocalFrameLoaderClient
    //     trips makeUniqueRef's RefCounted static_assert otherwise);
    //   - the in-tree empty client ctor takes ONLY FrameLoader& (the LocalFrame& arg
    //     is ignored by EmptyFrameLoaderClient). Component A SHOULD give
    //     LoadingFrameLoaderClient a ctor `LoadingFrameLoaderClient(FrameLoader&)`
    //     to match; if A needs the LocalFrame too, it can take (LocalFrame&, FrameLoader&)
    //     and we pass both — adjust the call below accordingly.
    {
        auto& params = std::get<PageConfiguration::LocalMainFrameCreationParameters>(
            pageConfiguration.mainFrameCreationParameters);
        params.clientCreator =
            CompletionHandler<UniqueRef<LocalFrameLoaderClient>(LocalFrame&, FrameLoader&)> {
            [onLoadDone](LocalFrame&, FrameLoader& frameLoader)
                -> UniqueRef<LocalFrameLoaderClient> {
                auto client = makeUniqueRefWithoutRefCountedCheck<WebCorePort::LoadingFrameLoaderClient>(frameLoader); // [A]
                client->setLoadCompletionHandler(WTF::Function<void(bool)> { onLoadDone });                            // [A] contract
                return client;
            } };
    }

    // ---- 3. Page ----
    Ref<Page> page = Page::create(WTF::move(pageConfiguration));

    // Network render: scripts still OFF for the first milestone (curl 200 GET ->
    // parse + layout + paint). Flip setScriptEnabled(true) once JSC-on-page is
    // validated. Compositing/media off (software paint, no <video>).
    page->settings().setScriptEnabled(false);
    page->settings().setAcceleratedCompositingEnabled(false);
    page->settings().setShouldAllowUserInstalledFonts(false);
#if ENABLE(VIDEO)
    page->settings().setMediaEnabled(false);
#endif
    // Subresources (CSS/img) are wanted for a real page; default loadsSubresources
    // is true. Leave it.

    // ---- 4. Main frame + view ----
    RefPtr<LocalFrame> localMainFrame = page->localMainFrame();
    if (!localMainFrame)
        return /*kErrNoMainFrame*/ -3;

    localMainFrame->setView(LocalFrameView::create(*localMainFrame));
    localMainFrame->init();   // creates initial empty document; FrameLoader ready

    RefPtr<LocalFrameView> view = localMainFrame->view();
    if (!view)
        return /*kErrNoView*/ -4;

    view->setTransparent(false);
    view->setBaseBackgroundColor(Color::white);
    view->setCanHaveScrollbars(true);   // a real page may scroll; harmless for paint
    view->resize(IntSize(w, h));        // establish viewport BEFORE load so layout
                                        // uses the right size as resources arrive.

    // ---- 5. Issue the network load ----
    // FrameLoadRequest(LocalFrame&, ResourceRequest&&, SubstituteData&&)
    // ctor exists (FrameLoadRequest.h:110). Empty SubstituteData => real fetch.
    ResourceRequest request { WTF::move(parsedURL) };
    FrameLoadRequest frameLoadRequest { *localMainFrame, WTF::move(request), SubstituteData { } };

    Ref<FrameLoader> loader = localMainFrame->loader();
    loader->load(WTF::move(frameLoadRequest));   // async: kicks off provisional load
                                                 // -> ResourceLoader -> ResourceHandle::start()
                                                 // -> CurlRequest (Core patch).

    // ---- 6. Pump the WebKit main-thread run loop until load completes ----
    // RunLoop::run() drains this thread's RunLoop (Drain mode) and only returns
    // when RunLoop::currentSingleton().stop() is called. We stop it from either
    // onLoadDone (client terminal state) or the watchdog timer. No busy-spin.
    //
    // Watchdog: 30 s one-shot. If the load wedges (DNS hang, TLS, missing redirect
    // support in the v1 curl bridge, App Container winsock capability missing),
    // we still return instead of hanging the caller forever.
    RunLoop::Timer watchdog(RunLoop::currentSingleton(), "WebCoreLoadUrl.watchdog"_s,
        [&loadState] {
            if (loadState.finished)
                return;
            loadState.finished = true;
            loadState.timedOut = true;
            RunLoop::currentSingleton().stop();
        });
    watchdog.startOneShot(30_s);

    // Guard against the (unlikely) case where the client already signaled before
    // we reached run() — only enter the loop if we are still waiting.
    if (!loadState.finished)
        RunLoop::run();

    watchdog.stop();

    if (loadState.timedOut)
        return /*kErrLoadTimeout*/ -11;
    if (loadState.failed)
        return /*kErrLoadFailed*/ -10;

    // ---- 7. Final layout ----
    // The document parsed as bytes arrived, but force a clean, complete style+layout
    // now (ignore-pending-stylesheets forces synchronous style resolution even if a
    // late <link> is still notionally pending — for a finished load they're all in).
    RefPtr<Document> document = localMainFrame->protectedDocument();
    if (!document)
        return /*kErrNoDocument*/ -6;
    document->updateLayoutIgnorePendingStylesheets();

    // ---- 8. Cairo paint + RGBA swizzle ----
    // IDENTICAL to WebCoreRenderHtml() steps 7-8. RECOMMENDED REFACTOR: extract
    // that tail of WebCoreRenderHtml() into a static helper and call it from both:
    //
    //     static int paintViewToRGBA(LocalFrameView& view, int w, int h, uint8_t* outRGBA);
    //
    // then here simply:
    //     return paintViewToRGBA(*view, w, h, outRGBA);
    //
    // Inlined copy shown for a standalone drop-in (delete if you do the refactor):
    {
        const IntSize size(w, h);

        cairo_surface_t* surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        if (!surface || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
            if (surface) cairo_surface_destroy(surface);
            return /*kErrCairoSurface*/ -7;
        }
        cairo_t* cr = cairo_create(surface);
        if (!cr || cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
            if (cr) cairo_destroy(cr);
            cairo_surface_destroy(surface);
            return /*kErrCairoContext*/ -8;
        }
        {
            GraphicsContextCairo context(adoptRef(cr));
            view->paint(context, IntRect(IntPoint(), size));
        }
        cairo_surface_flush(surface);

        const unsigned char* src = cairo_image_surface_get_data(surface);
        const int stride = cairo_image_surface_get_stride(surface);
        for (int y = 0; y < h; ++y) {
            const unsigned char* srow = src + static_cast<size_t>(y) * stride;
            uint8_t* drow = outRGBA + static_cast<size_t>(y) * w * 4;
            for (int x = 0; x < w; ++x) {
                const unsigned char b = srow[x * 4 + 0];
                const unsigned char g = srow[x * 4 + 1];
                const unsigned char r = srow[x * 4 + 2];
                const unsigned char a = srow[x * 4 + 3];
                if (a == 0 || a == 255) {
                    drow[x * 4 + 0] = r;
                    drow[x * 4 + 1] = g;
                    drow[x * 4 + 2] = b;
                    drow[x * 4 + 3] = a;
                } else {
                    drow[x * 4 + 0] = static_cast<uint8_t>((r * 255 + a / 2) / a);
                    drow[x * 4 + 1] = static_cast<uint8_t>((g * 255 + a / 2) / a);
                    drow[x * 4 + 2] = static_cast<uint8_t>((b * 255 + a / 2) / a);
                    drow[x * 4 + 3] = a;
                }
            }
        }
        cairo_surface_destroy(surface);
    }

    return /*kOK*/ 0;
}

} // extern "C"


// ============================================================================
// ALT (poll) VARIANT — use if Component A exposes a pollable flag instead of a
// completion callback. Replace the run-loop pump (step 6) with cycle-pumping:
//
//     // A exposes: bool LoadingFrameLoaderClient::isLoadFinished() const;
//     //            bool LoadingFrameLoaderClient::didLoadFail() const;
//     // and the driver kept a raw pointer to the client (captured out of the
//     // clientCreator into a local `LoadingFrameLoaderClient* clientPtr`).
//     MonotonicTime deadline = MonotonicTime::now() + 30_s;
//     while (!clientPtr->isLoadFinished()) {
//         if (MonotonicTime::now() >= deadline) { loadState.timedOut = true; break; }
//         RunLoop::cycle();              // one drain iteration (may block briefly)
//     }
//
// The callback variant above is preferred: cycle()-polling can spin hot when the
// run loop has no timers pending. Only fall back to this if A cannot easily call
// a completion handler.
// ============================================================================
