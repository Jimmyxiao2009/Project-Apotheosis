/*
 * stubs-crypto.cpp — platform WebCrypto stubs for the Win10Mobile (ARM32 thumbv7 UWP)
 * WebCore render DLL.
 *
 * No WebCrypto backend (openssl/gcrypt/cocoa) is compiled for this port, so the
 * CryptoKey and CryptoAlgorithm platform* entry points, CryptoAlgorithmRegistry::
 * platformRegisterAlgorithms, PAL::CryptoDigest and defaultWebCryptoMasterKey are
 * provided here. Signatures are copied verbatim from the WebCore / PAL headers so the
 * mangled names match what WebCore.lib expects.
 *
 * Policy:
 *   - Actual crypto operations (sign/verify/encrypt/decrypt/derive/wrap/import/export
 *     /generatePair) are never reachable from the pure layout+paint render path, so they
 *     RELEASE_ASSERT_NOT_REACHED() and then return a default value to keep the compiler happy.
 *   - PAL::CryptoDigest (create/addBytes/computeHash/dtor) may be exercised by non-WebCrypto
 *     hashing paths, so it gets a runnable no-op/zero implementation instead of an assert:
 *     computeHash() returns a fixed-length all-zero digest sized by the requested algorithm.
 *   - defaultWebCryptoMasterKey() returns std::nullopt (no key material on this port).
 *   - platformRegisterAlgorithms() is a no-op (registry stays empty; SubtleCrypto unavailable).
 *
 * See port/undef-crypto.txt for the full symbol list this file covers.
 */

#include "config.h"

#include "CryptoAlgorithmAESCBC.h"
#include "CryptoAlgorithmAESCFB.h"
#include "CryptoAlgorithmAESCTR.h"
#include "CryptoAlgorithmAESGCM.h"
#include "CryptoAlgorithmAESKW.h"
#include "CryptoAlgorithmECDH.h"
#include "CryptoAlgorithmECDSA.h"
#include "CryptoAlgorithmHKDF.h"
#include "CryptoAlgorithmHMAC.h"
#include "CryptoAlgorithmPBKDF2.h"
#include "CryptoAlgorithmRSASSA_PKCS1_v1_5.h"
#include "CryptoAlgorithmRSA_OAEP.h"
#include "CryptoAlgorithmRegistry.h"
#include "CryptoKeyEC.h"
#include "CryptoKeyRSA.h"
#include "CryptoKeyRSAComponents.h"
#include "ExceptionOr.h"
#include "JsonWebKey.h"
#include "SerializedCryptoKeyWrap.h"

#include <pal/crypto/CryptoDigest.h>

#include <wtf/Assertions.h>
#include <wtf/Vector.h>

// =====================================================================================
// PAL::CryptoDigest — runnable no-op / zero implementation (may be hit by hashing paths)
// =====================================================================================

namespace PAL {

// Opaque per-digest context; we only need to remember the algorithm so computeHash() can
// return a correctly-sized all-zero digest. Matches the forward-declared
// PAL::CryptoDigestContext referenced by CryptoDigest::m_context.
struct CryptoDigestContext {
    CryptoDigestHashFunction algorithm { CryptoDigestHashFunction::SHA_256 };
};

CryptoDigest::CryptoDigest()
    : m_context(nullptr)
{
}

CryptoDigest::~CryptoDigest() = default;

std::unique_ptr<CryptoDigest> CryptoDigest::create(CryptoDigestHashFunction algorithm)
{
    auto digest = std::unique_ptr<CryptoDigest>(new CryptoDigest);
    digest->m_context = std::unique_ptr<CryptoDigestContext>(new CryptoDigestContext { algorithm });
    return digest;
}

void CryptoDigest::addBytes(std::span<const uint8_t>)
{
    // No-op: this port has no real digest backend.
}

Vector<uint8_t> CryptoDigest::computeHash()
{
    // Return a fixed-length all-zero digest sized for the requested hash function so callers
    // that only need a stable, correctly-sized buffer keep working.
    size_t length = 32; // SHA-256 default
    if (m_context) {
        switch (m_context->algorithm) {
        case CryptoDigestHashFunction::SHA_1:
            length = 20;
            break;
        case CryptoDigestHashFunction::DEPRECATED_SHA_224:
            length = 28;
            break;
        case CryptoDigestHashFunction::SHA_256:
            length = 32;
            break;
        case CryptoDigestHashFunction::SHA_384:
            length = 48;
            break;
        case CryptoDigestHashFunction::SHA_512:
            length = 64;
            break;
        }
    }
    Vector<uint8_t> result(length);
    result.fill(0, length);
    return result;
}

} // namespace PAL

// =====================================================================================
// WebCore WebCrypto backend stubs — not reachable from the render path
// =====================================================================================

namespace WebCore {

// --- defaultWebCryptoMasterKey: no key material on this port ---

std::optional<Vector<uint8_t>> defaultWebCryptoMasterKey()
{
    return std::nullopt;
}

// --- CryptoAlgorithmRegistry: no-op (registry stays empty) ---

void CryptoAlgorithmRegistry::platformRegisterAlgorithms()
{
}

// --- CryptoKeyEC platform operations ---

bool CryptoKeyEC::platformSupportedCurve(NamedCurve)
{
    RELEASE_ASSERT_NOT_REACHED();
    return false;
}

std::optional<CryptoKeyPair> CryptoKeyEC::platformGeneratePair(CryptoAlgorithmIdentifier, NamedCurve, bool, CryptoKeyUsageBitmap)
{
    RELEASE_ASSERT_NOT_REACHED();
    return std::nullopt;
}

RefPtr<CryptoKeyEC> CryptoKeyEC::platformImportRaw(CryptoAlgorithmIdentifier, NamedCurve, Vector<uint8_t>&&, bool, CryptoKeyUsageBitmap)
{
    RELEASE_ASSERT_NOT_REACHED();
    return nullptr;
}

RefPtr<CryptoKeyEC> CryptoKeyEC::platformImportJWKPublic(CryptoAlgorithmIdentifier, NamedCurve, Vector<uint8_t>&&, Vector<uint8_t>&&, bool, CryptoKeyUsageBitmap)
{
    RELEASE_ASSERT_NOT_REACHED();
    return nullptr;
}

RefPtr<CryptoKeyEC> CryptoKeyEC::platformImportJWKPrivate(CryptoAlgorithmIdentifier, NamedCurve, Vector<uint8_t>&&, Vector<uint8_t>&&, Vector<uint8_t>&&, bool, CryptoKeyUsageBitmap)
{
    RELEASE_ASSERT_NOT_REACHED();
    return nullptr;
}

RefPtr<CryptoKeyEC> CryptoKeyEC::platformImportSpki(CryptoAlgorithmIdentifier, NamedCurve, Vector<uint8_t>&&, bool, CryptoKeyUsageBitmap)
{
    RELEASE_ASSERT_NOT_REACHED();
    return nullptr;
}

RefPtr<CryptoKeyEC> CryptoKeyEC::platformImportPkcs8(CryptoAlgorithmIdentifier, NamedCurve, Vector<uint8_t>&&, bool, CryptoKeyUsageBitmap)
{
    RELEASE_ASSERT_NOT_REACHED();
    return nullptr;
}

Vector<uint8_t> CryptoKeyEC::platformExportRaw() const
{
    RELEASE_ASSERT_NOT_REACHED();
    return { };
}

bool CryptoKeyEC::platformAddFieldElements(JsonWebKey&) const
{
    RELEASE_ASSERT_NOT_REACHED();
    return false;
}

Vector<uint8_t> CryptoKeyEC::platformExportSpki() const
{
    RELEASE_ASSERT_NOT_REACHED();
    return { };
}

Vector<uint8_t> CryptoKeyEC::platformExportPkcs8() const
{
    RELEASE_ASSERT_NOT_REACHED();
    return { };
}

size_t CryptoKeyEC::keySizeInBits() const
{
    RELEASE_ASSERT_NOT_REACHED();
    return 0;
}

// --- CryptoKeyRSA: key class, create/import/export/generate ---

RefPtr<CryptoKeyRSA> CryptoKeyRSA::create(CryptoAlgorithmIdentifier, CryptoAlgorithmIdentifier, bool, const CryptoKeyRSAComponents&, bool, CryptoKeyUsageBitmap)
{
    RELEASE_ASSERT_NOT_REACHED();
    return nullptr;
}

bool CryptoKeyRSA::isRestrictedToHash(CryptoAlgorithmIdentifier&) const
{
    RELEASE_ASSERT_NOT_REACHED();
    return false;
}

size_t CryptoKeyRSA::keySizeInBits() const
{
    RELEASE_ASSERT_NOT_REACHED();
    return 0;
}

void CryptoKeyRSA::generatePair(CryptoAlgorithmIdentifier, CryptoAlgorithmIdentifier, bool, unsigned, const Vector<uint8_t>&, bool, CryptoKeyUsageBitmap, KeyPairCallback&&, VoidCallback&& failureCallback, ScriptExecutionContext*)
{
    RELEASE_ASSERT_NOT_REACHED();
    failureCallback();
}

RefPtr<CryptoKeyRSA> CryptoKeyRSA::importSpki(CryptoAlgorithmIdentifier, std::optional<CryptoAlgorithmIdentifier>, Vector<uint8_t>&&, bool, CryptoKeyUsageBitmap)
{
    RELEASE_ASSERT_NOT_REACHED();
    return nullptr;
}

RefPtr<CryptoKeyRSA> CryptoKeyRSA::importPkcs8(CryptoAlgorithmIdentifier, std::optional<CryptoAlgorithmIdentifier>, Vector<uint8_t>&&, bool, CryptoKeyUsageBitmap)
{
    RELEASE_ASSERT_NOT_REACHED();
    return nullptr;
}

ExceptionOr<Vector<uint8_t>> CryptoKeyRSA::exportSpki() const
{
    RELEASE_ASSERT_NOT_REACHED();
    return Exception { ExceptionCode::OperationError };
}

ExceptionOr<Vector<uint8_t>> CryptoKeyRSA::exportPkcs8() const
{
    RELEASE_ASSERT_NOT_REACHED();
    return Exception { ExceptionCode::OperationError };
}

std::unique_ptr<CryptoKeyRSAComponents> CryptoKeyRSA::exportData() const
{
    RELEASE_ASSERT_NOT_REACHED();
    return nullptr;
}

// --- CryptoAlgorithmAESCBC ---

ExceptionOr<Vector<uint8_t>> CryptoAlgorithmAESCBC::platformEncrypt(const CryptoAlgorithmAesCbcCfbParams&, const CryptoKeyAES&, const Vector<uint8_t>&, Padding)
{
    RELEASE_ASSERT_NOT_REACHED();
    return Exception { ExceptionCode::OperationError };
}

ExceptionOr<Vector<uint8_t>> CryptoAlgorithmAESCBC::platformDecrypt(const CryptoAlgorithmAesCbcCfbParams&, const CryptoKeyAES&, const Vector<uint8_t>&, Padding)
{
    RELEASE_ASSERT_NOT_REACHED();
    return Exception { ExceptionCode::OperationError };
}

// --- CryptoAlgorithmAESCFB ---

ExceptionOr<Vector<uint8_t>> CryptoAlgorithmAESCFB::platformEncrypt(const CryptoAlgorithmAesCbcCfbParams&, const CryptoKeyAES&, const Vector<uint8_t>&)
{
    RELEASE_ASSERT_NOT_REACHED();
    return Exception { ExceptionCode::OperationError };
}

ExceptionOr<Vector<uint8_t>> CryptoAlgorithmAESCFB::platformDecrypt(const CryptoAlgorithmAesCbcCfbParams&, const CryptoKeyAES&, const Vector<uint8_t>&)
{
    RELEASE_ASSERT_NOT_REACHED();
    return Exception { ExceptionCode::OperationError };
}

// --- CryptoAlgorithmAESCTR ---

ExceptionOr<Vector<uint8_t>> CryptoAlgorithmAESCTR::platformEncrypt(const CryptoAlgorithmAesCtrParams&, const CryptoKeyAES&, const Vector<uint8_t>&)
{
    RELEASE_ASSERT_NOT_REACHED();
    return Exception { ExceptionCode::OperationError };
}

ExceptionOr<Vector<uint8_t>> CryptoAlgorithmAESCTR::platformDecrypt(const CryptoAlgorithmAesCtrParams&, const CryptoKeyAES&, const Vector<uint8_t>&)
{
    RELEASE_ASSERT_NOT_REACHED();
    return Exception { ExceptionCode::OperationError };
}

// --- CryptoAlgorithmAESGCM ---

ExceptionOr<Vector<uint8_t>> CryptoAlgorithmAESGCM::platformEncrypt(const CryptoAlgorithmAesGcmParams&, const CryptoKeyAES&, const Vector<uint8_t>&)
{
    RELEASE_ASSERT_NOT_REACHED();
    return Exception { ExceptionCode::OperationError };
}

ExceptionOr<Vector<uint8_t>> CryptoAlgorithmAESGCM::platformDecrypt(const CryptoAlgorithmAesGcmParams&, const CryptoKeyAES&, const Vector<uint8_t>&)
{
    RELEASE_ASSERT_NOT_REACHED();
    return Exception { ExceptionCode::OperationError };
}

// --- CryptoAlgorithmAESKW ---

ExceptionOr<Vector<uint8_t>> CryptoAlgorithmAESKW::platformWrapKey(const CryptoKeyAES&, const Vector<uint8_t>&)
{
    RELEASE_ASSERT_NOT_REACHED();
    return Exception { ExceptionCode::OperationError };
}

ExceptionOr<Vector<uint8_t>> CryptoAlgorithmAESKW::platformUnwrapKey(const CryptoKeyAES&, const Vector<uint8_t>&)
{
    RELEASE_ASSERT_NOT_REACHED();
    return Exception { ExceptionCode::OperationError };
}

// --- CryptoAlgorithmECDH ---

std::optional<Vector<uint8_t>> CryptoAlgorithmECDH::platformDeriveBits(const CryptoKeyEC&, const CryptoKeyEC&)
{
    RELEASE_ASSERT_NOT_REACHED();
    return std::nullopt;
}

// --- CryptoAlgorithmECDSA ---

ExceptionOr<Vector<uint8_t>> CryptoAlgorithmECDSA::platformSign(const CryptoAlgorithmEcdsaParams&, const CryptoKeyEC&, const Vector<uint8_t>&)
{
    RELEASE_ASSERT_NOT_REACHED();
    return Exception { ExceptionCode::OperationError };
}

ExceptionOr<bool> CryptoAlgorithmECDSA::platformVerify(const CryptoAlgorithmEcdsaParams&, const CryptoKeyEC&, const Vector<uint8_t>&, const Vector<uint8_t>&)
{
    RELEASE_ASSERT_NOT_REACHED();
    return Exception { ExceptionCode::OperationError };
}

// --- CryptoAlgorithmHKDF ---

ExceptionOr<Vector<uint8_t>> CryptoAlgorithmHKDF::platformDeriveBits(const CryptoAlgorithmHkdfParams&, const CryptoKeyRaw&, size_t)
{
    RELEASE_ASSERT_NOT_REACHED();
    return Exception { ExceptionCode::OperationError };
}

// --- CryptoAlgorithmHMAC ---

ExceptionOr<Vector<uint8_t>> CryptoAlgorithmHMAC::platformSign(const CryptoKeyHMAC&, const Vector<uint8_t>&)
{
    RELEASE_ASSERT_NOT_REACHED();
    return Exception { ExceptionCode::OperationError };
}

ExceptionOr<bool> CryptoAlgorithmHMAC::platformVerify(const CryptoKeyHMAC&, const Vector<uint8_t>&, const Vector<uint8_t>&)
{
    RELEASE_ASSERT_NOT_REACHED();
    return Exception { ExceptionCode::OperationError };
}

// --- CryptoAlgorithmPBKDF2 ---

ExceptionOr<Vector<uint8_t>> CryptoAlgorithmPBKDF2::platformDeriveBits(const CryptoAlgorithmPbkdf2Params&, const CryptoKeyRaw&, size_t)
{
    RELEASE_ASSERT_NOT_REACHED();
    return Exception { ExceptionCode::OperationError };
}

// --- CryptoAlgorithmRSASSA_PKCS1_v1_5 ---

ExceptionOr<Vector<uint8_t>> CryptoAlgorithmRSASSA_PKCS1_v1_5::platformSign(const CryptoKeyRSA&, const Vector<uint8_t>&)
{
    RELEASE_ASSERT_NOT_REACHED();
    return Exception { ExceptionCode::OperationError };
}

ExceptionOr<bool> CryptoAlgorithmRSASSA_PKCS1_v1_5::platformVerify(const CryptoKeyRSA&, const Vector<uint8_t>&, const Vector<uint8_t>&)
{
    RELEASE_ASSERT_NOT_REACHED();
    return Exception { ExceptionCode::OperationError };
}

// --- CryptoAlgorithmRSA_OAEP ---

ExceptionOr<Vector<uint8_t>> CryptoAlgorithmRSA_OAEP::platformEncrypt(const CryptoAlgorithmRsaOaepParams&, const CryptoKeyRSA&, const Vector<uint8_t>&)
{
    RELEASE_ASSERT_NOT_REACHED();
    return Exception { ExceptionCode::OperationError };
}

ExceptionOr<Vector<uint8_t>> CryptoAlgorithmRSA_OAEP::platformDecrypt(const CryptoAlgorithmRsaOaepParams&, const CryptoKeyRSA&, const Vector<uint8_t>&)
{
    RELEASE_ASSERT_NOT_REACHED();
    return Exception { ExceptionCode::OperationError };
}

} // namespace WebCore
