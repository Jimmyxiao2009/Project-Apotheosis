// ============================================================================
// PortPlatformStrategies.cpp  (compiles in port/, links against WebCore.lib)
//
// Minimal PlatformStrategies for the in-process render path. createLoaderStrategy()
// returns the legacy WebResourceLoadScheduler (which drives ResourceHandle, which
// we patched to use curl). Everything else is the smallest thing that links.
//
// install once from WebCoreDriver init:  installPortPlatformStrategies();
//
// WebResourceLoadScheduler lives under Source/WebKitLegacy/WebCoreSupport/ and is
// compiled INTO WebCore.lib for this port (see PlatformWinUWP.cmake — Step 3 of
// the network-bridge integration adds it to WebCore_SOURCES).
//
// The PortBlobRegistry forwarder is a verbatim copy of WebKitLegacy's
// WebBlobRegistry (Source/WebKitLegacy/mac/WebCoreSupport/WebPlatformStrategies.mm):
// thin forwarders to an owned BlobRegistryImpl.
// ============================================================================

#include "config.h"

#include <WebCore/BlobRegistry.h>
#include <WebCore/BlobRegistryImpl.h>
#include <WebCore/LoaderStrategy.h>
#include <WebCore/MediaStrategy.h>
#include <WebCore/PlatformStrategies.h>
#include <wtf/NeverDestroyed.h>

// Legacy scheduler header (Source/WebKitLegacy/WebCoreSupport on the include path).
#include "WebResourceLoadScheduler.h"

using namespace WebCore;

namespace WebCorePort {

// --- BlobRegistry forwarder (copied from WebKitLegacy WebBlobRegistry) -------
class PortBlobRegistry final : public BlobRegistry {
public:
    PortBlobRegistry() = default;

private:
    void registerInternalFileBlobURL(const URL& url, Ref<BlobDataFileReference>&& reference, const String&, const String& contentType) final { m_blobRegistry.registerInternalFileBlobURL(url, WTF::move(reference), contentType); }
    void registerInternalBlobURL(const URL& url, Vector<BlobPart>&& parts, const String& contentType) final { m_blobRegistry.registerInternalBlobURL(url, WTF::move(parts), contentType); }
    void registerBlobURL(const URL& url, const URL& srcURL, const PolicyContainer& policyContainer, const std::optional<WebCore::SecurityOriginData>& topOrigin) final { m_blobRegistry.registerBlobURL(url, srcURL, policyContainer, topOrigin); }
    void registerInternalBlobURLOptionallyFileBacked(const URL& url, const URL& srcURL, RefPtr<BlobDataFileReference>&& reference, const String& contentType) final { m_blobRegistry.registerInternalBlobURLOptionallyFileBacked(url, srcURL, WTF::move(reference), contentType, { }); }
    void registerInternalBlobURLForSlice(const URL& url, const URL& srcURL, long long start, long long end, const String& contentType) final { m_blobRegistry.registerInternalBlobURLForSlice(url, srcURL, start, end, contentType); }
    void unregisterBlobURL(const URL& url, const std::optional<WebCore::SecurityOriginData>& topOrigin) final { m_blobRegistry.unregisterBlobURL(url, topOrigin); }
    String blobType(const URL& url) final { return m_blobRegistry.blobType(url); }
    unsigned long long blobSize(const URL& url) final { return m_blobRegistry.blobSize(url); }
    void writeBlobsToTemporaryFilesForIndexedDB(const Vector<String>& blobURLs, CompletionHandler<void(Vector<String>&& filePaths)>&& completionHandler) final { m_blobRegistry.writeBlobsToTemporaryFilesForIndexedDB(blobURLs, WTF::move(completionHandler)); }
    void registerBlobURLHandle(const URL& url, const std::optional<SecurityOriginData>& topOrigin) final { m_blobRegistry.registerBlobURLHandle(url, topOrigin); }
    void unregisterBlobURLHandle(const URL& url, const std::optional<SecurityOriginData>& topOrigin) final { m_blobRegistry.unregisterBlobURLHandle(url, topOrigin); }

    BlobRegistryImpl* blobRegistryImpl() final { return &m_blobRegistry; }

    BlobRegistryImpl m_blobRegistry;
};

class PortPlatformStrategies final : public PlatformStrategies {
public:
    PortPlatformStrategies() = default;

private:
    LoaderStrategy* createLoaderStrategy() final
    {
        // Drives ResourceLoader -> ResourceHandle -> (patched) CurlRequest.
        return new WebResourceLoadScheduler;
    }

    PasteboardStrategy* createPasteboardStrategy() final
    {
        // The render path never touches the pasteboard. pasteboardStrategy()
        // returns a CheckedPtr, so a null here is tolerated as long as nothing
        // dereferences it during a render (it does not).
        return nullptr;
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
