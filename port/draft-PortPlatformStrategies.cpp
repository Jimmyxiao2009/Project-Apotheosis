// ============================================================================
// draft-PortPlatformStrategies.cpp  (compiles in port/, links against WebCore.lib)
//
// Minimal PlatformStrategies for the in-process render path. createLoaderStrategy()
// returns the legacy WebResourceLoadScheduler (which drives ResourceHandle, which
// we patched to use curl). Everything else is the smallest thing that links.
//
// install once from WebCoreDriver init:  installPortPlatformStrategies();
//
// OPEN QUESTION (verify at build): is WebResourceLoadScheduler compiled into
//   WebCore.lib, or only WebKitLegacy? It lives under Source/WebKitLegacy/
//   WebCoreSupport/. If WebKitLegacy is NOT in our build, EITHER (a) add
//   WebResourceLoadScheduler.cpp to the WinUWP WebCore source list, OR
//   (b) write a tiny LoaderStrategy here. See report decision tree.
// ============================================================================

#include "config.h"

#include <WebCore/BlobRegistryImpl.h>
#include <WebCore/LoaderStrategy.h>
#include <WebCore/MediaStrategy.h>
#include <WebCore/PlatformStrategies.h>
#include <wtf/NeverDestroyed.h>

// If reusing the legacy scheduler:
#include "WebResourceLoadScheduler.h"   // from Source/WebKitLegacy/WebCoreSupport

using namespace WebCore;

namespace WebCorePort {

class PortPlatformStrategies final : public PlatformStrategies {
public:
    PortPlatformStrategies() = default;

private:
    LoaderStrategy* createLoaderStrategy() final
    {
        // Drives ResourceHandle -> (patched) CurlRequest.
        return new WebResourceLoadScheduler;
    }

    PasteboardStrategy* createPasteboardStrategy() final
    {
        // Render path never touches the pasteboard. nullptr is acceptable only
        // if pasteboardStrategy() is never called; otherwise provide a no-op.
        // Safer: return a trivial subclass. Left null here, mark uncertain.
        return nullptr; // TODO: confirm no call path hits this during render.
    }

    MediaStrategy* createMediaStrategy() final
    {
        class PortMediaStrategy final : public MediaStrategy {
            bool enableWebMMediaPlayer() const final { return false; }
        };
        return new PortMediaStrategy;
    }

    BlobRegistry* createBlobRegistry() final
    {
        // Needed for blob: URLs and for SubresourceLoader bookkeeping in some paths.
        class PortBlobRegistry final : public BlobRegistry {
            // ... thin forwarders to m_impl, identical to WebKitLegacy's
            //     WebBlobRegistry. Omitted here for brevity; copy that class.
            BlobRegistryImpl* blobRegistryImpl() final { return &m_impl; }
            // (all other pure virtuals forward to m_impl)
            BlobRegistryImpl m_impl;
        };
        return new PortBlobRegistry;
    }
};

} // namespace WebCorePort

void installPortPlatformStrategies()
{
    static WTF::NeverDestroyed<WebCorePort::PortPlatformStrategies> strategies;
    if (!WebCore::hasPlatformStrategies())
        WebCore::setPlatformStrategies(&strategies.get());
}
