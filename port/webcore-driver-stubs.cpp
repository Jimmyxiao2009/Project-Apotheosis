// ============================================================================
// webcore-driver-stubs.cpp  —  link-time stubs for the WinUWP WebCore driver
// (EdgeHTML Reborn — ARM32 thumbv7 Win10-Mobile App Container)
//
// PURPOSE
//   WebCore.lib is a static archive built with USE(CURL)=OFF, USE(OPENSSL)=OFF,
//   no gcrypt/cocoa, USE(TEXTURE_MAPPER)=OFF, PlatformScreenWin.cpp dropped, etc.
//   The *platform-agnostic* .cpp's that call into those backends ARE compiled,
//   but the backend .cpp's are NOT — and (critically) the agnostic fallbacks in
//   files like ResourceHandle.cpp / PlatformScreen.cpp are themselves guarded by
//   `#if USE(CURL)||USE(SOUP)` / `#if PLATFORM(COCOA)||PLATFORM(GTK)||WPE`, so for
//   PORT=WinUWP they fall into a DEAD #if GAP: neither backend nor fallback exists.
//   This file fills that gap so the driver links.
//
// COMPILE (reuse harness-cmd.bat verbatim minus the PCH /FI and /Yu, minus
//   /showIncludes and the UnifiedSource path; same -I / -D / flags so ABI +
//   mangling + _HAS_EXCEPTIONS=0 match WebCore.lib exactly). Same TU must see
//   <WebCore/config.h> first so USE()/PLATFORM()/ENABLE() macros agree.
//
// POLICY PER STUB
//   * "can be hit, must behave"  -> real-ish default (return empty/false/no-op).
//   * "must never be hit in the layout+paint path" -> RELEASE_ASSERT_NOT_REACHED()
//     so a real call fails loudly instead of silently corrupting state.
//   * Symbols that only matter if a *feature* is exercised (crypto, sockets):
//     provide the single chokepoint stub (e.g. platformRegisterAlgorithms) and
//     let RELEASE_ASSERT guard the rest — they should be dead-stripped by /OPT:REF
//     unless actually referenced.
//
// METHODOLOGY: this is a STARTING set predicted from source inspection, NOT a
//   verified-complete list. The real list comes from the linker. See the
//   "LINK-DRIVEN WORKFLOW" note at the bottom: link once, read each
//   `unresolved external symbol`, undname it, add a stub here, repeat.
// ============================================================================

#include "config.h"

#include <wtf/Assertions.h>
#include <wtf/Forward.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

// ----------------------------------------------------------------------------
// 1. PLATFORM SCREEN  —  REQUIRED (definitely referenced by RenderView/FrameView)
//    PlatformScreen.cpp body is `#if COCOA||GTK||WPE` only; PlatformScreenWin.cpp
//    was dropped. RenderView::screenRect / MediaQueryEvaluator / FrameView call
//    screenRect/screenDepth/screenAvailableRect during the FIRST layout. So these
//    are NOT "if referenced" — the minimal path WILL pull them. Return a sane
//    fixed virtual screen (matches a 1080p phone; the driver can override the
//    FrameView size independently — these only feed `screen`/`window` CSS media).
// ----------------------------------------------------------------------------
#include "FloatRect.h"
#include "Widget.h"
#include "DestinationColorSpace.h"

namespace WebCore {

// Tune to the device. These are only used for `@media (device-width/height)`,
// window.screen.*, and color-gamut media queries.
static constexpr float kScreenW = 1080;
static constexpr float kScreenH = 1920;

int  screenDepth(Widget*)             { return 24; }
int  screenDepthPerComponent(Widget*) { return 8; }
bool screenIsMonochrome(Widget*)      { return false; }
bool screenHasInvertedColors()        { return false; }

FloatRect screenRect(Widget*)          { return FloatRect(0, 0, kScreenW, kScreenH); }
FloatRect screenAvailableRect(Widget*) { return FloatRect(0, 0, kScreenW, kScreenH); }

double screenDPI(PlatformDisplayID) { return 96.0; }

DestinationColorSpace screenColorSpace(Widget*) { return DestinationColorSpace::SRGB(); }

// The following are WEBCORE_EXPORT and may or may not be referenced; provide
// conservative defaults. If the linker says "already defined", delete the dup
// (means a kept *Win.cpp / generic file already supplies it) — see workflow note.
OptionSet<ContentsFormat> screenContentsFormats(Widget*) { return { ContentsFormat::RGBA8 }; }
bool screenSupportsExtendedColor(Widget*)               { return false; }
// screenSupportsHighDynamicRange is `constexpr {return false;}` in the header on
// non-COCOA/GTK — do NOT redefine it here (would be ODR/redefinition error).

} // namespace WebCore

// ----------------------------------------------------------------------------
// 2. CRYPTO  —  STUB-IF-REFERENCED (chokepoint only)
//    CryptoAlgorithmRegistry() ctor calls platformRegisterAlgorithms(), defined
//    ONLY in crypto/{openssl,gcrypt,cocoa}/CryptoAlgorithmRegistry*.cpp — none
//    compiled. registerAlgorithm() then pulls every CryptoAlgorithm*::create.
//    The pure layout+paint path NEVER constructs SubtleCrypto, so with /OPT:REF
//    the whole registry should dead-strip. BUT if any always-live global ctor in
//    a compiled TU touches the registry, you need this one no-op to satisfy the
//    link without dragging in OpenSSL. Leaving it empty = "no algorithms
//    registered" = SubtleCrypto throws NotSupportedError at runtime (correct for
//    a no-crypto build), instead of a link failure.
//
//    Per-key platform methods (CryptoKeyRSA::generatePair, ::importSpki, ...,
//    CryptoAlgorithm*::platformEncrypt/Decrypt/Sign/Verify/DeriveBits) live in
//    the same uncompiled backend dirs. They should ALSO dead-strip with the
//    registry. Only add explicit stubs for them if the linker actually names
//    them (it will tell you the exact mangled signature). Templates for the most
//    likely ones are provided commented-out below.
// ----------------------------------------------------------------------------
namespace WebCore {
class CryptoAlgorithmRegistry;
}
namespace WebCore {
// The real decl is a private member: void CryptoAlgorithmRegistry::platformRegisterAlgorithms();
// Define it out-of-line. Including the header keeps the mangling correct.
} // namespace WebCore

// NOTE: to define the member you must include the real header so the class +
// access + mangling match. Uncomment when/if the linker asks for it:
// #include "CryptoAlgorithmRegistry.h"
// namespace WebCore {
// void CryptoAlgorithmRegistry::platformRegisterAlgorithms() { /* register nothing */ }
// } // namespace WebCore

// Likely follow-on crypto unresolveds (uncomment + #include the header per item
// ONLY if named by the linker; signatures verified against 2.52.4 headers):
//
// #include "CryptoKeyRSA.h"
// namespace WebCore {
//   void CryptoKeyRSA::generatePair(CryptoAlgorithmIdentifier, CryptoAlgorithmIdentifier, bool, unsigned, const Vector<uint8_t>&, bool, CryptoKeyUsageBitmap, KeyPairCallback&&, VoidCallback&& failure, ScriptExecutionContext*) { failure(); }
//   RefPtr<CryptoKeyRSA> CryptoKeyRSA::importJwk(CryptoAlgorithmIdentifier, std::optional<CryptoAlgorithmIdentifier>, JsonWebKey&&, bool, CryptoKeyUsageBitmap) { return nullptr; }
//   RefPtr<CryptoKeyRSA> CryptoKeyRSA::importSpki(CryptoAlgorithmIdentifier, std::optional<CryptoAlgorithmIdentifier>, Vector<uint8_t>&&, bool, CryptoKeyUsageBitmap) { return nullptr; }
//   RefPtr<CryptoKeyRSA> CryptoKeyRSA::importPkcs8(CryptoAlgorithmIdentifier, std::optional<CryptoAlgorithmIdentifier>, Vector<uint8_t>&&, bool, CryptoKeyUsageBitmap) { return nullptr; }
//   ExceptionOr<Vector<uint8_t>> CryptoKeyRSA::exportSpki() const { return Exception { ExceptionCode::NotSupportedError }; }
//   ExceptionOr<Vector<uint8_t>> CryptoKeyRSA::exportPkcs8() const { return Exception { ExceptionCode::NotSupportedError }; }
//   JsonWebKey CryptoKeyRSA::exportJwk() const { RELEASE_ASSERT_NOT_REACHED(); }
//   auto CryptoKeyRSA::algorithm() const -> KeyAlgorithm { RELEASE_ASSERT_NOT_REACHED(); }
// } // namespace WebCore
// (CryptoKeyEC / CryptoKeyOKP have platformGeneratePair/platformImport*/platformExport*;
//  CryptoAlgorithm{AESCBC,AESCTR,AESGCM,ECDH,ECDSA,HKDF,HMAC,PBKDF2,RSA_OAEP,
//  RSASSA_PKCS1_v1_5,RSA_PSS}::platform{Encrypt,Decrypt,Sign,Verify,DeriveBits}
//  are all candidates. Stub each to `return Exception { ExceptionCode::NotSupportedError };`.)
//
// Also likely from crypto/SerializedCryptoKeyWrap.cpp (it's compiled) ->
//   wrapSerializedCryptoKey / unwrapSerializedCryptoKey / defaultWebCryptoMasterKey
//   resolve to platform/{cocoa,gtk,win}; the GTK one uses libgcrypt. If named:
// namespace WebCore {
//   std::optional<Vector<uint8_t>> wrapSerializedCryptoKey(const Vector<uint8_t>&, const Vector<uint8_t>&) { return std::nullopt; }
//   std::optional<Vector<uint8_t>> unwrapSerializedCryptoKey(const Vector<uint8_t>&, const Vector<uint8_t>&) { return std::nullopt; }
// }

// ----------------------------------------------------------------------------
// 3. NETWORK  —  STUB-IF-REFERENCED, but NetworkStorageSession ctor/dtor is the
//    landmine. With USE(CURL)=OFF, NetworkStorageSession's ctor, dtor, and ALL
//    cookie methods live only in network/curl/NetworkStorageSessionCurl.cpp
//    (not compiled). The agnostic NetworkStorageSession.cpp supplies only the
//    static members + tracking-prevention bits. If anything constructs a
//    NetworkStorageSession (CookieJar / Page setup can), you get a pile of
//    unresolveds. A pure string-load layout path can usually avoid it, but the
//    Document's cookie access (document.cookie, or even default Page wiring) may
//    not. Provide a minimal in-memory / no-op NetworkStorageSession if the
//    linker pulls it. Because the ctor signature is curl-specific
//    (PAL::SessionID, const String& alternativeServicesDirectory), match it.
//
//    ResourceHandle: its ~ResourceHandle/start/cancel/platform* fallbacks are
//    `#if USE(SOUP)||USE(CURL)` -> all unresolved here too. The minimal path
//    should not instantiate ResourceHandle at all (loads go through the
//    FrameLoaderClient/Empty clients). If it does, stub with ASSERT_NOT_REACHED.
//
//    Keep these COMMENTED until the linker names them — defining a class member
//    requires the exact header so vtable/mangling line up.
// ----------------------------------------------------------------------------
//
// #include "NetworkStorageSession.h"
// #include <pal/SessionID.h>
// namespace WebCore {
//   // Curl-shaped ctor/dtor (see network/curl/NetworkStorageSessionCurl.cpp):
//   NetworkStorageSession::NetworkStorageSession(PAL::SessionID sessionID, const String&)
//       : m_sessionID(sessionID) { }
//   NetworkStorageSession::~NetworkStorageSession() = default;
//   // Cookie methods: no jar -> empty results. Add each as the linker names it.
//   std::pair<String, bool> NetworkStorageSession::cookiesForDOM(const URL&, const SameSiteInfo&, const URL&, std::optional<FrameIdentifier>, std::optional<PageIdentifier>, IncludeSecureCookies, ApplyTrackingPrevention, ShouldRelaxThirdPartyCookieBlocking, IsKnownCrossSiteTracker) const { return { String(), false }; }
//   void NetworkStorageSession::setCookiesFromDOM(const URL&, const SameSiteInfo&, const URL&, std::optional<FrameIdentifier>, std::optional<PageIdentifier>, ApplyTrackingPrevention, RequiresScriptTrackingPrivacy, const String&, ShouldRelaxThirdPartyCookieBlocking, IsKnownCrossSiteTracker) const { }
//   bool NetworkStorageSession::getRawCookies(const URL&, const SameSiteInfo&, const URL&, std::optional<FrameIdentifier>, std::optional<PageIdentifier>, ApplyTrackingPrevention, ShouldRelaxThirdPartyCookieBlocking, Vector<Cookie>&) const { return false; }
//   std::pair<String, bool> NetworkStorageSession::cookieRequestHeaderFieldValue(const URL&, const SameSiteInfo&, const URL&, std::optional<FrameIdentifier>, std::optional<PageIdentifier>, IncludeSecureCookies, ApplyTrackingPrevention, ShouldRelaxThirdPartyCookieBlocking, IsKnownCrossSiteTracker) const { return { String(), false }; }
//   // ... add getCookies/setCookie/deleteCookie/getHostnamesWithCookies/etc. as named.
// } // namespace WebCore
//
// ResourceHandle (only if instantiated — should not be on the minimal path):
// #include "ResourceHandle.h"
// #include "ResourceHandleInternal.h"
// namespace WebCore {
//   ResourceHandleInternal::~ResourceHandleInternal() = default;
//   ResourceHandle::~ResourceHandle() { }
//   bool ResourceHandle::start() { RELEASE_ASSERT_NOT_REACHED(); return false; }
//   void ResourceHandle::cancel() { RELEASE_ASSERT_NOT_REACHED(); }
//   void ResourceHandle::platformSetDefersLoading(bool) { }
//   void ResourceHandle::platformLoadResourceSynchronously(NetworkingContext*, const ResourceRequest&, StoredCredentialsPolicy, SecurityOrigin*, ResourceError&, ResourceResponse&, Vector<uint8_t>&) { RELEASE_ASSERT_NOT_REACHED(); }
//   bool ResourceHandle::shouldUseCredentialStorage() { return false; }
// } // namespace WebCore
//
// SocketStreamHandle: only WebSocket needs it. Not on the layout path. If named,
// the impl is SocketStreamHandleImpl{Curl}.cpp -> stub the create + send/close.
//
// CredentialStorage: the agnostic CredentialStorage.cpp IS compiled and is
// self-contained (in-memory map) — should NOT need a stub. Do not pre-stub it.

// ----------------------------------------------------------------------------
// 4. PLATFORM MISC  —  evaluate case-by-case (most are NOT needed)
//    - SystemSettings / PlatformScreen color: covered above.
//    - Cursor / Pasteboard / DragData: ENABLE(DRAG_SUPPORT)=OFF,
//      ENABLE(CONTEXT_MENUS)=OFF, and the *Win.cpp files were dropped. A pure
//      paint path should not touch them. If EventHandler/Page wiring references
//      Cursor::Cursor or Pasteboard::createForCopyAndPaste, stub minimally.
//    - ScrollbarThemeWin: NOT used — USE(THEME_ADWAITA)=ON selects
//      ScrollbarThemeAdwaita (compiled). Do NOT stub ScrollbarThemeWin.
//    - RenderThemeWin: NOT used — Adwaita.cmake supplies RenderThemeAdwaita.
//    - MainThreadSharedTimerWin.cpp IS in the build (per draft cmake) but creates
//      an HWND_MESSAGE window (forbidden in App Container). That's a RUNTIME
//      problem, not a link stub — replace the file with a RunLoop-based timer.
//      (Listed here so it isn't mistaken for a missing symbol.)
//    - WebCoreInstanceHandle / WebCoreBundleWin / SearchPopupMenuDB: kept but
//      stubbed-behavior files; they LINK (defined), they just misbehave at
//      runtime in the sandbox. Not link stubs.
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------
// 5. PROCESS / SANDBOX / UA  —  mostly already provided
//    - UserAgentWin.cpp IS in the build -> standardUserAgent defined. No stub.
//    - ProcessWarming / RuntimeApplicationChecks: appear in a compiled unified
//      source (UnifiedSource-767013ce-6) -> defined. No stub expected.
//    - If the linker names a process-identity symbol, prefer wiring the real
//      WTF/PAL one over stubbing.
// ----------------------------------------------------------------------------

// ============================================================================
// LINK-DRIVEN WORKFLOW (the actual deliverable — this file is just the seed):
//
//   1) Link the driver + WTF/JSC/PAL/WebCore.lib with /OPT:REF /OPT:NOICF (as in
//      OptionsWinUWP.cmake). /OPT:REF dead-strips unreferenced archive members,
//      so the unresolved list reflects ONLY what the minimal path truly needs.
//   2) For each "LNK2019 unresolved external symbol <mangled>":
//        > undname <mangled>          (or llvm-undname) to get the C++ signature.
//      Decide the category:
//        - screen/render-critical  -> real default (already covered in §1).
//        - feature backend (crypto/socket/cookie) -> empty/Exception/no-op, and
//          ask: "is it on the layout+paint path?" If NO and it's still pulled,
//          something over-references it — consider cutting that caller instead.
//        - "should never run" -> RELEASE_ASSERT_NOT_REACHED().
//   3) Add the stub by #include-ing the real header (so class membership,
//      access level, calling convention, and _HAS_EXCEPTIONS=0 mangling match)
//      and defining out-of-line. Re-link. Repeat until clean.
//   4) Watch for LNK2005 "already defined": that means a backend file you
//      thought was excluded is actually compiled (or two stubs collide) — delete
//      the stub, don't fight the linker.
//   5) Keep RELEASE_ASSERT_NOT_REACHED() stubs in even after linking: if one
//      fires at runtime it pinpoints exactly which "shouldn't happen" path the
//      HTML actually exercised, telling you what to implement for real next.
//
// THINGS TO REAL-IMPLEMENT, NOT STUB (if the render needs them):
//   - Fonts: FreeType/Fontconfig/HarfBuzz backend is compiled & linked — never
//     stub FontPlatformData/FontCascade/GlyphPage. A blank render is almost
//     always a Fontconfig sandbox/cache issue (FONTCONFIG_PATH + writable cache
//     inside the App Container), NOT a missing symbol.
//   - Cairo image surface paint path: real (GraphicsContextCairo + Cairo.cmake).
//   - Image decoders (JPEG/PNG/WebP): real (ImageDecoders.cmake compiled).
//   - MainThreadSharedTimer: replace the HWND impl with a RunLoop timer — a
//     stub that no-ops the timer will hang layout (timers drive style/layout
//     flushing). This must WORK, not be stubbed.
// ============================================================================
