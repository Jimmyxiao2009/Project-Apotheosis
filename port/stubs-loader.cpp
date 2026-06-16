// stubs-loader.cpp — Phase 1b 接入 curl 网络后端后链接残留的 13 个符号。
//  - WebResourceLoadScheduler 的 10 个 xxxError 工厂(LoaderStrategy 纯虚,upstream 由嵌入层
//    实现;本端无嵌入层 → 返回通用 ResourceError)。
//  - CurlSSLHandle::platformInitialize(跨平台版被 OS(WINDOWS) 守掉,Win 版 CurlSSLHandleWin.cpp
//    已排除 → 空 no-op;TLS 根证书走打包的 cacert.pem + setCACertPath)。
//  - CryptoAlgorithmRSA_PSS::platformSign/Verify(WebCrypto 后端未编 → NotSupported)。
#include "config.h"

#include "WebResourceLoadScheduler.h"
#include "ResourceError.h"
#include "ResourceRequest.h"
#include "ResourceResponse.h"
#include "CurlSSLHandle.h"
#include "CryptoAlgorithmRSA_PSS.h"
#include "CryptoAlgorithmRsaPssParams.h"
#include "CryptoKeyRSA.h"
#include "ExceptionOr.h"
#include "ExceptionCode.h"
#include <wtf/Assertions.h>

using namespace WebCore;

// ---- WebResourceLoadScheduler 错误工厂(全局命名空间类)----
static ResourceError webKitStubError(const URL& url, ASCIILiteral desc)
{
    return ResourceError("WebKitErrorDomain"_s, 0, url, desc);
}
ResourceError WebResourceLoadScheduler::cancelledError(const ResourceRequest& r) const { return webKitStubError(r.url(), "cancelled"_s); }
ResourceError WebResourceLoadScheduler::blockedError(const ResourceRequest& r) const { return webKitStubError(r.url(), "blocked"_s); }
ResourceError WebResourceLoadScheduler::blockedByContentBlockerError(const ResourceRequest& r) const { return webKitStubError(r.url(), "blocked by content blocker"_s); }
ResourceError WebResourceLoadScheduler::cannotShowURLError(const ResourceRequest& r) const { return webKitStubError(r.url(), "cannot show URL"_s); }
ResourceError WebResourceLoadScheduler::interruptedForPolicyChangeError(const ResourceRequest& r) const { return webKitStubError(r.url(), "interrupted for policy change"_s); }
ResourceError WebResourceLoadScheduler::httpsUpgradeRedirectLoopError(const ResourceRequest& r) const { return webKitStubError(r.url(), "https upgrade redirect loop"_s); }
ResourceError WebResourceLoadScheduler::httpNavigationWithHTTPSOnlyError(const ResourceRequest& r) const { return webKitStubError(r.url(), "http navigation with https-only"_s); }
ResourceError WebResourceLoadScheduler::cannotShowMIMETypeError(const ResourceResponse& r) const { return webKitStubError(r.url(), "cannot show MIME type"_s); }
ResourceError WebResourceLoadScheduler::fileDoesNotExistError(const ResourceResponse& r) const { return webKitStubError(r.url(), "file does not exist"_s); }
ResourceError WebResourceLoadScheduler::pluginWillHandleLoadError(const ResourceResponse& r) const { return webKitStubError(r.url(), "plugin will handle load"_s); }

namespace WebCore {

// ---- CurlSSLHandle 平台初始化(Win 版已排除;空 no-op,CA 走 cacert.pem)----
void CurlSSLHandle::platformInitialize() { }

// ---- WebCrypto RSA-PSS(无后端)----
ExceptionOr<Vector<uint8_t>> CryptoAlgorithmRSA_PSS::platformSign(const CryptoAlgorithmRsaPssParams&, const CryptoKeyRSA&, const Vector<uint8_t>&)
{
    return Exception { ExceptionCode::NotSupportedError };
}
ExceptionOr<bool> CryptoAlgorithmRSA_PSS::platformVerify(const CryptoAlgorithmRsaPssParams&, const CryptoKeyRSA&, const Vector<uint8_t>&, const Vector<uint8_t>&)
{
    return Exception { ExceptionCode::NotSupportedError };
}

} // namespace WebCore
