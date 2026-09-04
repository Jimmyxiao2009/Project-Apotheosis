/*
 * stubs-crypto.cpp — platform WebCrypto stubs for the Win10Mobile (ARM32 thumbv7 UWP)
 * WebCore render DLL.
 *
 * Apotheosis: WebCore now compiles its OpenSSL WebCrypto backend (PlatformWinUWP.cmake
 * includes platform/OpenSSL.cmake), so the former CryptoKey{EC,RSA} and
 * CryptoAlgorithm* platform entry points, CryptoAlgorithmRegistry::
 * platformRegisterAlgorithms and defaultWebCryptoMasterKey stubs
 * are gone - they collided (LNK2005) with WebCore.lib. Only PAL::CryptoDigest stays here.
 *
 * Policy:
 *   - PAL::CryptoDigest (create/addBytes/computeHash/dtor) is exercised by Subresource
 *     Integrity and other non-WebCrypto hashing paths, so it computes real digests
 *     through OpenSSL EVP (see below).
 */

#include "config.h"

#include <pal/crypto/CryptoDigest.h>

#include <wtf/Assertions.h>
#include <wtf/Vector.h>

// =====================================================================================
// PAL::CryptoDigest — 真实 SHA(走已为 TLS 链接的 OpenSSL libcrypto)。
// Apotheosis: 原先 computeHash() 返回全 0 假摘要 → Subresource Integrity(integrity=...)
// 等校验恒失败、资源被拦。改用 OpenSSL EVP 一次性摘要算真 SHA。
// 只声明所需 C 函数(不引 openssl 头,避免与 WebKit 自带 crypto 头/config 冲突)。
// =====================================================================================

#include <openssl/evp.h>   // libcrypto(已为 TLS 链接)的 EVP 摘要;头已在 vcpkg include 路径上

namespace PAL {

// 累积 addBytes 的输入,computeHash() 时一次性算摘要。
struct CryptoDigestContext {
    CryptoDigestHashFunction algorithm { CryptoDigestHashFunction::SHA_256 };
    Vector<uint8_t> data;
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

void CryptoDigest::addBytes(std::span<const uint8_t> bytes)
{
    if (m_context)
        m_context->data.append(bytes);
}

Vector<uint8_t> CryptoDigest::computeHash()
{
    const EVP_MD* md = EVP_sha256();
    if (m_context) {
        switch (m_context->algorithm) {
        case CryptoDigestHashFunction::SHA_1:              md = EVP_sha1();   break;
        case CryptoDigestHashFunction::DEPRECATED_SHA_224: md = EVP_sha224(); break;
        case CryptoDigestHashFunction::SHA_256:            md = EVP_sha256(); break;
        case CryptoDigestHashFunction::SHA_384:            md = EVP_sha384(); break;
        case CryptoDigestHashFunction::SHA_512:            md = EVP_sha512(); break;
        }
    }
    auto in = m_context ? m_context->data.span() : std::span<const uint8_t>();
    unsigned char hash[64]; // EVP_MAX_MD_SIZE
    unsigned int len = 0;
    Vector<uint8_t> result;
    if (EVP_Digest(in.data(), in.size(), hash, &len, md, nullptr) == 1)
        result.append(std::span<const uint8_t>(hash, len));
    return result;
}

} // namespace PAL

