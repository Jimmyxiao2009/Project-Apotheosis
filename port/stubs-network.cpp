// ============================================================================
// stubs-network.cpp — link-time stubs for the "network" class of undefined
// platform symbols (EdgeHTML Reborn — ARM32 thumbv7 Win10-Mobile App Container).
//
// STATUS (Phase 1b, 2026-06-15 — curl backend turned ON):
//   This build is now USE(CURL)=1 / USE(OPENSSL)=1. The curl backend TUs under
//   platform/network/curl ARE compiled, so they now own the symbols that this
//   file used to stub. ALL of the former stubs here would LNK2005-collide with:
//     * ResourceHandle::*            -> revived in ResourceHandle.cpp
//                                       (#if USE(CURL) && WK_WINUWP curl bridge)
//     * SynchronousLoaderClient::*   -> SynchronousLoaderClient.cpp curl tail
//     * NetworkStorageSession cookies-> NetworkStorageSessionCurl.cpp
//     * ResourceError platform bits  -> ResourceErrorCurl.cpp
//     * ResourceResponse::platformSuggestedFilename -> ResourceResponseCurl.cpp
//     * CertificateInfo summary/isolatedCopy        -> CertificateInfoCurl.cpp
//   They have ALL been removed from this file.
//
// WHAT REMAINS:
//   Only the two NetworkStateNotifier platform hooks. The agnostic
//   NetworkStateNotifier.cpp IS compiled and owns singleton()/onLine()/
//   addListener()/updateState(); only these two platform members are missing
//   (normally NetworkStateNotifier{Win,GLib,Mac}.cpp, none of which is built for
//   this App-Container port). No real adapter polling -> pretend "online".
//   Pinning m_isOnLine prevents onLine()/updateState() from re-querying forever.
//
// PlatformScreen / DNSResolveQueue are owned by other stub files — untouched.
// ============================================================================

#include "config.h"

#include "NetworkStateNotifier.h"

namespace WebCore {

void NetworkStateNotifier::updateStateWithoutNotifying()
{
    m_isOnLine = true;
}

void NetworkStateNotifier::startObserving()
{
}

} // namespace WebCore
