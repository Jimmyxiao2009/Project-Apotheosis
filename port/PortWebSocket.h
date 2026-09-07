// PortWebSocket.h — WebSocket support for the WinUWP driver.
//
// Apotheosis (2026-09-07): the driver builds its Page from
// pageConfigurationWithEmptyClients(), whose SocketProvider is EmptyClients.cpp's
// EmptySocketProvider — createWebSocketChannel() returns nullptr. WebSocket.cpp:288 then
// hits `RELEASE_ASSERT(m_channel)` ("Every ScriptExecutionContext should have a
// SocketProvider") and the app dies with SIGABRT on the first `new WebSocket(...)`.
// Device (0.1.9.23, build-driver\logs\20260907-114911\crash.txt): mapy.com aborts every
// time, and any SPA with a live connection would do the same.
//
// This port has no NetworkProcess, so there is no WebSocketTask to talk to over IPC.
// Everything the protocol needs is already inside WebCore, though: CurlStreamScheduler
// (a worker thread that owns raw CURL stream handles and calls its clients back on the
// main thread), WebSocketHandshake, WebSocketFrame and WebSocketDeflateFramer. So
// PortWebSocketChannel is the two upstream halves fused into one main-thread object:
//   - the ThreadableWebSocketChannel surface of WebKit/WebProcess/Network/WebSocketChannel
//     (connect/send/close/fail/disconnect, NetworkSendQueue for ordering, bufferedAmount,
//     the inspector hooks), and
//   - the wire half of WebKit/NetworkProcess/curl/WebSocketTaskCurl (opening handshake
//     with cookies, frame parse/validate/inflate, close handshake, ping/pong).
// Both are followed closely on purpose: this is protocol code, and the upstream versions
// are the tested ones.
//
// Threading: everything here is main (engine) thread. CurlStreamScheduler does its I/O on
// its own worker thread and hands results back via callOnMainThread(), which the driver's
// pump and live tick both service.

#pragma once

#include <WebCore/CurlStream.h>
#include <WebCore/SocketProvider.h>
#include <WebCore/ThreadableWebSocketChannel.h>
#include <WebCore/WebSocketChannelInspector.h>
#include <WebCore/WebSocketDeflateFramer.h>
#include <WebCore/WebSocketFrame.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/ResourceResponse.h>
#include <wtf/Expected.h>
#include <wtf/RefCounted.h>
#include <wtf/WeakPtr.h>

namespace WebCore {
class CurlStreamScheduler;
class Document;
class NetworkSendQueue;
class SharedBuffer;
class WebSocketChannelClient;
class WebSocketHandshake;
class WeakPtrImplWithEventTargetData;
}

namespace WebCorePort {

class PortWebSocketChannel final
    : public WebCore::ThreadableWebSocketChannel
    , public WebCore::CurlStream::Client
    , public CanMakeWeakPtr<PortWebSocketChannel>
    , public RefCounted<PortWebSocketChannel> {
public:
    static Ref<PortWebSocketChannel> create(WebCore::Document&, WebCore::WebSocketChannelClient&);
    ~PortWebSocketChannel();

    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

private:
    PortWebSocketChannel(WebCore::Document&, WebCore::WebSocketChannelClient&);

    enum class State : uint8_t { Connecting, Handshaking, Opened, Closing, Closed };

    // ThreadableWebSocketChannel
    ConnectStatus connect(const URL&, const String& protocol) final;
    String subprotocol() final;
    String extensions() final;
    void send(CString&&) final;
    void send(const JSC::ArrayBuffer&, unsigned byteOffset, unsigned byteLength) final;
    void send(WebCore::Blob&) final;
    void close(int code, const String& reason) final;
    void fail(String&& reason) final;
    void disconnect() final;
    void suspend() final { }
    void resume() final { }
    const WebCore::WebSocketChannelInspector* channelInspector() const final { return &m_inspector; }
    WebCore::WebSocketChannelIdentifier progressIdentifier() const final { return m_inspector.progressIdentifier(); }
    bool hasCreatedHandshake() const final { return !m_url.isNull(); }
    bool isConnected() const final { return !m_handshakeResponse.isNull(); }
    WebCore::ResourceRequest clientHandshakeRequest(const CookieGetter&) const final { return m_handshakeRequest; }
    const WebCore::ResourceResponse& serverHandshakeResponse() const final { return m_handshakeResponse; }

    // CurlStream::Client (all delivered on the main thread by CurlStreamScheduler)
    void didOpen(WebCore::CurlStreamID) final;
    void didSendData(WebCore::CurlStreamID, size_t) final { }
    void didReceiveData(WebCore::CurlStreamID, const WebCore::SharedBuffer&) final;
    void didFail(WebCore::CurlStreamID, CURLcode, WebCore::CertificateInfo&&) final;

    static Ref<WebCore::NetworkSendQueue> createMessageQueue(WebCore::Document&, PortWebSocketChannel&);

    bool appendReceivedBuffer(const WebCore::SharedBuffer&);
    void skipReceivedBuffer(size_t);
    Expected<bool, String> validateOpeningHandshake();
    std::optional<String> receiveFrames(NOESCAPE const Function<void(WebCore::WebSocketFrame::OpCode, std::span<const uint8_t>)>&);
    std::optional<String> validateFrame(const WebCore::WebSocketFrame&);
    bool sendFrame(WebCore::WebSocketFrame::OpCode, std::span<const uint8_t>);
    void sendClosingHandshakeIfNeeded(int32_t, const String& reason);

    void networkFailed(String&& reason);
    void didCloseInternal(int32_t code, const String& reason);
    void logErrorMessage(const String&);
    bool increaseBufferedAmount(size_t);
    void decreaseBufferedAmount(size_t);

    bool isStreamInvalidated() const { return m_streamID == WebCore::invalidCurlStreamID; }
    void destructStream();

    WeakPtr<WebCore::Document, WebCore::WeakPtrImplWithEventTargetData> m_document;
    ThreadSafeWeakPtr<WebCore::WebSocketChannelClient> m_client;
    const Ref<WebCore::NetworkSendQueue> m_messageQueue;
    WebCore::WebSocketChannelInspector m_inspector;

    WebCore::CurlStreamScheduler& m_scheduler;
    WebCore::CurlStreamID m_streamID { WebCore::invalidCurlStreamID };

    URL m_url;
    String m_protocol;
    String m_subprotocol;
    String m_extensions;
    WebCore::ResourceRequest m_request;
    WebCore::ResourceRequest m_handshakeRequest;
    WebCore::ResourceResponse m_handshakeResponse;

    State m_state { State::Connecting };
    size_t m_bufferedAmount { 0 };
    bool m_isClosing { false };

    std::unique_ptr<WebCore::WebSocketHandshake> m_handshake;
    WebCore::WebSocketDeflateFramer m_deflateFramer;
    bool m_didCompleteOpeningHandshake { false };

    bool m_shouldDiscardReceivedData { false };
    Vector<uint8_t> m_receiveBuffer;

    bool m_hasContinuousFrame { false };
    WebCore::WebSocketFrame::OpCode m_continuousFrameOpCode { WebCore::WebSocketFrame::OpCode::OpCodeInvalid };
    Vector<uint8_t> m_continuousFrameData;

    bool m_receivedClosingHandshake { false };
    int32_t m_closeEventCode { WebCore::ThreadableWebSocketChannel::CloseEventCode::CloseEventCodeNotSpecified };
    String m_closeEventReason;
    bool m_didSendClosingHandshake { false };
    bool m_reportedFailure { false };
};

// The provider the driver hands to PageConfiguration in place of EmptySocketProvider.
// WebTransport stays unimplemented (it needs HTTP/3, which this port has no transport for);
// initializeWebTransportSession() rejects, which is what every non-Cocoa port does today.
class PortSocketProvider final : public WebCore::SocketProvider {
public:
    static Ref<PortSocketProvider> create() { return adoptRef(*new PortSocketProvider); }

private:
    RefPtr<WebCore::ThreadableWebSocketChannel> createWebSocketChannel(WebCore::Document&, WebCore::WebSocketChannelClient&) final;
    std::pair<RefPtr<WebCore::WebTransportSession>, Ref<WebCore::WebTransportSessionPromise>> initializeWebTransportSession(WebCore::ScriptExecutionContext&, WebCore::WebTransportSessionClient&, const URL&, const WebCore::WebTransportOptions&) final;
};

} // namespace WebCorePort
