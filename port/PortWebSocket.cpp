// PortWebSocket.cpp — see PortWebSocket.h.
//
// The wire half follows WebKit/NetworkProcess/curl/WebSocketTaskCurl.cpp and the
// client-facing half WebKit/WebProcess/Network/WebSocketChannel.cpp, minus the IPC and the
// NetworkSession that does not exist here. Cookies come from the driver's own process-wide
// curl NetworkStorageSession (PortNetworkStorageSession.cpp), i.e. the same jar the HTTP
// path uses, so a ws:// handshake carries the session cookies the page was loaded with.

#include "config.h"

#include "PortWebSocket.h"

#include "PortNetworkStorageSession.h"

#include <WebCore/Blob.h>
#include <WebCore/CertificateInfo.h>
#include <WebCore/CurlContext.h>
#include <WebCore/CurlStreamScheduler.h>
#include <WebCore/Document.h>
#include <WebCore/DocumentInlines.h>
#include <WebCore/NetworkSendQueue.h>
#include <WebCore/NetworkStorageSession.h>
#include <WebCore/SameSiteInfo.h>
#include <WebCore/ScriptExecutionContext.h>
#include <WebCore/SecurityOrigin.h>
#include <WebCore/SharedBuffer.h>
#include <WebCore/WebSocketChannelClient.h>
#include <WebCore/WebSocketHandshake.h>
#include <WebCore/WebTransportSession.h>
#include <JavaScriptCore/ArrayBuffer.h>
#include <JavaScriptCore/ConsoleTypes.h>
#include <wtf/CheckedArithmetic.h>
#include <wtf/MainThread.h>
#include <wtf/text/CString.h>
#include <wtf/text/MakeString.h>

namespace WebCorePort {

using namespace WebCore;

// ---------------------------------------------------------------------------------------
// PortSocketProvider
// ---------------------------------------------------------------------------------------

RefPtr<ThreadableWebSocketChannel> PortSocketProvider::createWebSocketChannel(Document& document, WebSocketChannelClient& client)
{
    return PortWebSocketChannel::create(document, client);
}

std::pair<RefPtr<WebTransportSession>, Ref<WebTransportSessionPromise>> PortSocketProvider::initializeWebTransportSession(ScriptExecutionContext&, WebTransportSessionClient&, const URL&, const WebTransportOptions&)
{
    return { nullptr, WebTransportSessionPromise::createAndReject() };
}

// ---------------------------------------------------------------------------------------
// PortWebSocketChannel
// ---------------------------------------------------------------------------------------

Ref<PortWebSocketChannel> PortWebSocketChannel::create(Document& document, WebSocketChannelClient& client)
{
    return adoptRef(*new PortWebSocketChannel(document, client));
}

Ref<NetworkSendQueue> PortWebSocketChannel::createMessageQueue(Document& document, PortWebSocketChannel& channel)
{
    // The queue exists for ordering: a Blob has to be read asynchronously, and anything
    // enqueued after it must not overtake it. Text and ArrayBuffer go straight through.
    return NetworkSendQueue::create(document, [weakChannel = WeakPtr { channel }](const CString& utf8String) {
        RefPtr channel = weakChannel.get();
        if (!channel)
            return;
        auto data = byteCast<uint8_t>(utf8String.span());
        if (!channel->sendFrame(WebSocketFrame::OpCodeText, data))
            channel->networkFailed("Failed to send WebSocket frame."_s);
        channel->decreaseBufferedAmount(utf8String.length());
    }, [weakChannel = WeakPtr { channel }](std::span<const uint8_t> span) {
        RefPtr channel = weakChannel.get();
        if (!channel)
            return;
        if (!channel->sendFrame(WebSocketFrame::OpCodeBinary, span))
            channel->networkFailed("Failed to send WebSocket frame."_s);
        channel->decreaseBufferedAmount(span.size());
    }, [weakChannel = WeakPtr { channel }](ExceptionCode exceptionCode) {
        RefPtr channel = weakChannel.get();
        if (!channel)
            return NetworkSendQueue::Continue::No;
        channel->fail(makeString("Failed to load Blob: exception code = "_s, static_cast<int>(exceptionCode)));
        return NetworkSendQueue::Continue::No;
    });
}

PortWebSocketChannel::PortWebSocketChannel(Document& document, WebSocketChannelClient& client)
    : m_document(document)
    , m_client(client)
    , m_messageQueue(createMessageQueue(document, *this))
    , m_inspector(document)
    , m_scheduler(CurlContext::singleton().streamScheduler())
{
}

PortWebSocketChannel::~PortWebSocketChannel()
{
    destructStream();
}

PortWebSocketChannel::ConnectStatus PortWebSocketChannel::connect(const URL& url, const String& protocol)
{
    ASSERT(isMainThread());

    RefPtr document = m_document.get();
    if (!document)
        return ConnectStatus::KO;

    // Same request the network process would have built: origin, user agent, SameSite,
    // no-cache, Sec-Fetch-*, plus the ws->wss upgrade a content rule list may have forced.
    auto request = webSocketConnectRequest(*document, url);
    if (!request)
        return ConnectStatus::KO;

    if (request->url() != url) {
        if (RefPtr client = m_client.get())
            client->didUpgradeURL();
    }

    m_inspector.didCreateWebSocket(request->url());
    m_url = request->url();
    m_protocol = protocol;
    m_request = request->isolatedCopy();

    m_streamID = m_scheduler.createStream(m_request.url(), *this, CurlStream::ServerTrustEvaluation::Enable, CurlStream::LocalhostAlias::Disable);
    if (isStreamInvalidated())
        return ConnectStatus::KO;

    m_handshakeRequest = ResourceRequest(m_request);
    m_inspector.willSendWebSocketHandshakeRequest(m_handshakeRequest);
    return ConnectStatus::OK;
}

String PortWebSocketChannel::subprotocol()
{
    return m_subprotocol.isNull() ? emptyString() : m_subprotocol;
}

String PortWebSocketChannel::extensions()
{
    return m_extensions.isNull() ? emptyString() : m_extensions;
}

bool PortWebSocketChannel::increaseBufferedAmount(size_t byteLength)
{
    if (!byteLength)
        return true;

    CheckedSize checkedNewBufferedAmount = m_bufferedAmount;
    checkedNewBufferedAmount += byteLength;
    if (checkedNewBufferedAmount.hasOverflowed()) [[unlikely]] {
        fail("Failed to send WebSocket frame: buffer has no more space"_s);
        return false;
    }

    m_bufferedAmount = checkedNewBufferedAmount;
    if (RefPtr client = m_client.get())
        client->didUpdateBufferedAmount(m_bufferedAmount);
    return true;
}

void PortWebSocketChannel::decreaseBufferedAmount(size_t byteLength)
{
    if (!byteLength || byteLength > m_bufferedAmount)
        return;

    m_bufferedAmount -= byteLength;
    if (RefPtr client = m_client.get())
        client->didUpdateBufferedAmount(m_bufferedAmount);
}

void PortWebSocketChannel::send(CString&& message)
{
    auto length = message.length();
    if (!increaseBufferedAmount(length))
        return;
    m_messageQueue->enqueue(WTF::move(message));
}

void PortWebSocketChannel::send(const JSC::ArrayBuffer& binaryData, unsigned byteOffset, unsigned byteLength)
{
    if (!increaseBufferedAmount(byteLength))
        return;
    m_messageQueue->enqueue(binaryData, byteOffset, byteLength);
}

void PortWebSocketChannel::send(Blob& blob)
{
    auto byteLength = blob.size();
    if (!byteLength)
        return send(JSC::ArrayBuffer::create(static_cast<size_t>(0), 1), 0, 0);

    if (!increaseBufferedAmount(byteLength))
        return;
    m_messageQueue->enqueue(blob);
}

void PortWebSocketChannel::close(int code, const String& reason)
{
    // Sending the closing handshake can fail, which closes the channel and can drop the
    // last reference to it.
    Ref protectedThis { *this };

    m_isClosing = true;
    if (RefPtr client = m_client.get())
        client->didStartClosingHandshake();

    WebSocketFrame closingFrame(WebSocketFrame::OpCodeClose, true, false, true);
    m_inspector.didSendWebSocketFrame(closingFrame);

    if (m_state == State::Closed)
        return;

    if (m_state == State::Connecting || m_state == State::Handshaking) {
        didCloseInternal(CloseEventCodeAbnormalClosure, { });
        return;
    }

    sendClosingHandshakeIfNeeded(code, reason);
}

void PortWebSocketChannel::fail(String&& reason)
{
    // The client may close the channel from inside didReceiveMessageError().
    Ref protectedThis { *this };

    logErrorMessage(reason);
    if (RefPtr client = m_client.get())
        client->didReceiveMessageError(String { reason });

    if (m_isClosing)
        return;

    if (m_state == State::Opened || m_state == State::Closing)
        sendClosingHandshakeIfNeeded(CloseEventCodeGoingAway, reason);
    didCloseInternal(CloseEventCodeAbnormalClosure, { });
}

void PortWebSocketChannel::disconnect()
{
    // Suppresses didClose(), per the ThreadableWebSocketChannel contract.
    m_client = nullptr;
    m_document = nullptr;
    m_messageQueue->clear();
    m_inspector.didCloseWebSocket();

    if (m_state == State::Opened || m_state == State::Closing)
        sendClosingHandshakeIfNeeded(CloseEventCodeGoingAway, { });
    m_state = State::Closed;
    destructStream();
}

void PortWebSocketChannel::logErrorMessage(const String& errorMessage)
{
    RefPtr document = m_document.get();
    if (!document)
        return;

    String consoleMessage;
    if (!m_url.isNull())
        consoleMessage = makeString("WebSocket connection to '"_s, m_url.string(), "' failed: "_s, errorMessage);
    else
        consoleMessage = makeString("WebSocket connection failed: "_s, errorMessage);
    document->addConsoleMessage(MessageSource::Network, MessageLevel::Error, consoleMessage);
}

// ---- CurlStream::Client -----------------------------------------------------------------

void PortWebSocketChannel::didOpen(CurlStreamID)
{
    if (m_state != State::Connecting)
        return;

    m_state = State::Handshaking;

    m_handshake = makeUnique<WebSocketHandshake>(m_request.url(), m_protocol, m_request.httpUserAgent(),
        m_request.httpHeaderField(HTTPHeaderName::Origin), m_request.allowCookies(), false);
    m_handshake->reset();
    m_handshake->addExtensionProcessor(m_deflateFramer.createExtensionProcessor());

    // WebSocketHandshake::clientHandshakeMessage() cannot add the Cookie header itself, so
    // it is spliced in ahead of the terminating CRLF exactly as WebSocketTaskCurl does.
    CString cookieHeader;
    if (m_request.allowCookies()) {
        auto& storageSession = defaultPortStorageSession();
        auto includeSecureCookies = m_request.url().protocolIs("wss"_s) ? IncludeSecureCookies::Yes : IncludeSecureCookies::No;
        auto cookieHeaderField = storageSession.cookieRequestHeaderFieldValue(m_request.firstPartyForCookies(),
            SameSiteInfo::create(m_request), m_request.url(), std::nullopt, std::nullopt, includeSecureCookies,
            ApplyTrackingPrevention::Yes, ShouldRelaxThirdPartyCookieBlocking::No, IsKnownCrossSiteTracker::No).first;
        if (!cookieHeaderField.isEmpty())
            cookieHeader = makeString("Cookie: "_s, cookieHeaderField, "\r\n"_s).utf8();
    }

    auto originalMessage = m_handshake->clientHandshakeMessage();
    auto handshakeMessageLength = originalMessage.length() + cookieHeader.length();
    auto handshakeMessage = makeUniqueArray<uint8_t>(handshakeMessageLength);

    memcpy(handshakeMessage.get(), originalMessage.data(), originalMessage.length());
    if (!cookieHeader.isNull() && cookieHeader.length()) {
        memcpy(handshakeMessage.get() + originalMessage.length() - 2, cookieHeader.data(), cookieHeader.length());
        memcpy(handshakeMessage.get() + handshakeMessageLength - 2, "\r\n", 2);
    }

    m_scheduler.send(m_streamID, WTF::move(handshakeMessage), handshakeMessageLength);
}

void PortWebSocketChannel::didReceiveData(CurlStreamID, const SharedBuffer& buffer)
{
    if (m_state == State::Connecting || m_state == State::Closed)
        return;

    Ref protectedThis { *this };

    if (!buffer.size()) {
        didCloseInternal(CloseEventCodeAbnormalClosure, { });
        return;
    }

    if (m_shouldDiscardReceivedData || m_receivedClosingHandshake)
        return;

    if (!appendReceivedBuffer(buffer)) {
        networkFailed("Ran out of memory while receiving WebSocket data."_s);
        return;
    }

    auto validateResult = validateOpeningHandshake();
    if (!validateResult.has_value()) {
        networkFailed(String(validateResult.error()));
        return;
    }
    if (!validateResult.value())
        return;

    auto frameResult = receiveFrames([this](WebSocketFrame::OpCode opCode, std::span<const uint8_t> data) {
        m_inspector.didReceiveWebSocketFrame(WebSocketChannelInspector::createFrame(data, opCode));

        switch (opCode) {
        case WebSocketFrame::OpCodeText: {
            String message = data.size() ? String::fromUTF8(data) : emptyString();
            if (message.isNull()) {
                networkFailed("Could not decode a text frame as UTF-8."_s);
                return;
            }
            if (m_isClosing)
                return;
            if (RefPtr client = m_client.get())
                client->didReceiveMessage(WTF::move(message));
            break;
        }

        case WebSocketFrame::OpCodeBinary:
            if (m_isClosing)
                return;
            if (RefPtr client = m_client.get())
                client->didReceiveBinaryData({ data });
            break;

        case WebSocketFrame::OpCodeClose:
            if (!data.size())
                m_closeEventCode = CloseEventCodeNoStatusRcvd;
            else if (data.size() == 1) {
                m_closeEventCode = CloseEventCodeAbnormalClosure;
                networkFailed("Received a broken close frame containing an invalid size body."_s);
                return;
            } else {
                auto highByte = static_cast<unsigned char>(data[0]);
                auto lowByte = static_cast<unsigned char>(data[1]);
                m_closeEventCode = highByte << 8 | lowByte;
                if (m_closeEventCode == CloseEventCodeNoStatusRcvd
                    || m_closeEventCode == CloseEventCodeAbnormalClosure
                    || m_closeEventCode == CloseEventCodeTLSHandshake) {
                    m_closeEventCode = CloseEventCodeAbnormalClosure;
                    networkFailed("Received a broken close frame containing a reserved status code."_s);
                    return;
                }
            }
            if (data.size() >= 3)
                m_closeEventReason = String::fromUTF8(data.subspan(2));
            else
                m_closeEventReason = emptyString();

            m_receivedClosingHandshake = true;
            sendClosingHandshakeIfNeeded(m_closeEventCode, m_closeEventReason);
            didCloseInternal(m_closeEventCode, m_closeEventReason);
            break;

        case WebSocketFrame::OpCodePing:
            if (!sendFrame(WebSocketFrame::OpCodePong, data))
                networkFailed("Failed to send WebSocket frame."_s);
            break;

        case WebSocketFrame::OpCodeContinuation:
        case WebSocketFrame::OpCodePong:
        case WebSocketFrame::OpCodeInvalid:
            break;
        }
    });

    if (frameResult)
        networkFailed(String(*frameResult));
}

void PortWebSocketChannel::didFail(CurlStreamID, CURLcode errorCode, CertificateInfo&&)
{
    // No NetworkSession here, so there is nobody to ask about an untrusted certificate:
    // a failed server-trust evaluation is simply a failed connection, which is also the
    // safe answer for a browser with no certificate UI.
    Ref protectedThis { *this };
    networkFailed(makeString("WebSocket network error: error code "_s, static_cast<uint32_t>(errorCode)));
}

// ---- wire ------------------------------------------------------------------------------

bool PortWebSocketChannel::appendReceivedBuffer(const SharedBuffer& buffer)
{
    size_t newBufferSize = m_receiveBuffer.size() + buffer.size();
    if (newBufferSize < m_receiveBuffer.size())
        return false;

    m_receiveBuffer.append(buffer.span());
    return true;
}

void PortWebSocketChannel::skipReceivedBuffer(size_t length)
{
    memmoveSpan(m_receiveBuffer.mutableSpan(), m_receiveBuffer.subspan(length));
    m_receiveBuffer.shrink(m_receiveBuffer.size() - length);
}

Expected<bool, String> PortWebSocketChannel::validateOpeningHandshake()
{
    if (m_didCompleteOpeningHandshake)
        return true;

    if (m_state != State::Handshaking || !m_handshake || m_handshake->mode() != WebSocketHandshake::Incomplete) {
        m_handshake = nullptr;
        return makeUnexpected("Unexpected handshaking condition"_s);
    }

    auto headerLength = m_handshake->readServerHandshake(m_receiveBuffer.span());
    if (headerLength <= 0)
        return false;

    skipReceivedBuffer(headerLength);

    if (m_handshake->mode() != WebSocketHandshake::Connected) {
        auto reason = m_handshake->failureReason();
        m_handshake = nullptr;
        return makeUnexpected(reason);
    }

    auto serverSetCookie = m_handshake->serverSetCookie();
    if (!serverSetCookie.isEmpty())
        defaultPortStorageSession().setCookiesFromHTTPResponse(m_request.firstPartyForCookies(), m_request.url(), serverSetCookie);

    m_state = State::Opened;
    m_didCompleteOpeningHandshake = true;

    m_subprotocol = m_handshake->serverWebSocketProtocol();
    m_extensions = m_handshake->acceptedExtensions();
    m_handshakeResponse = ResourceResponse(m_handshake->serverHandshakeResponse());
    m_inspector.didReceiveWebSocketHandshakeResponse(m_handshakeResponse);

    if (!m_isClosing) {
        if (RefPtr client = m_client.get())
            client->didConnect();
    }
    m_handshake = nullptr;
    return true;
}

std::optional<String> PortWebSocketChannel::receiveFrames(NOESCAPE const Function<void(WebSocketFrame::OpCode, std::span<const uint8_t>)>& callback)
{
    if (m_state != State::Opened && m_state != State::Closing)
        return std::nullopt;

    while (m_receiveBuffer.size() && !m_shouldDiscardReceivedData && !m_receivedClosingHandshake) {
        WebSocketFrame frame;
        const uint8_t* frameEnd;
        String errorString;
        auto parseResult = WebSocketFrame::parseFrame(m_receiveBuffer.mutableSpan(), frame, frameEnd, errorString);
        if (parseResult == WebSocketFrame::FrameIncomplete)
            return std::nullopt;
        if (parseResult == WebSocketFrame::FrameError)
            return errorString;

        auto inflateResult = m_deflateFramer.inflate(frame);
        if (!inflateResult->succeeded())
            return inflateResult->failureReason();

        if (auto validateResult = validateFrame(frame))
            return *validateResult;

        if (!frame.final || frame.opCode == WebSocketFrame::OpCodeContinuation) {
            if (frame.opCode != WebSocketFrame::OpCodeContinuation) {
                m_hasContinuousFrame = true;
                m_continuousFrameOpCode = frame.opCode;
            }

            m_continuousFrameData.append(frame.payload);

            if (frame.final) {
                callback(m_continuousFrameOpCode, m_continuousFrameData.span());
                m_hasContinuousFrame = false;
                m_continuousFrameData.clear();
            }
        } else
            callback(frame.opCode, frame.payload);

        if (!m_receiveBuffer.isEmpty())
            skipReceivedBuffer(frameEnd - m_receiveBuffer.begin());
    }

    return std::nullopt;
}

std::optional<String> PortWebSocketChannel::validateFrame(const WebSocketFrame& frame)
{
    if (WebSocketFrame::isReservedOpCode(frame.opCode))
        return makeString("Unrecognized frame opcode: "_s, static_cast<unsigned>(frame.opCode));

    if (frame.reserved2 || frame.reserved3)
        return makeString("One or more reserved bits are on: reserved2 = "_s, static_cast<unsigned>(frame.reserved2), ", reserved3 = "_s, static_cast<unsigned>(frame.reserved3));

    if (frame.masked)
        return "A server must not mask any frames that it sends to the client."_s;

    if (WebSocketFrame::isControlOpCode(frame.opCode) && !frame.final)
        return makeString("Received fragmented control frame: opcode = "_s, static_cast<unsigned>(frame.opCode));

    if (WebSocketFrame::isControlOpCode(frame.opCode) && WebSocketFrame::needsExtendedLengthField(frame.payload.size()))
        return makeString("Received control frame having too long payload: "_s, frame.payload.size(), " bytes"_s);

    if (m_hasContinuousFrame && frame.opCode != WebSocketFrame::OpCodeContinuation && !WebSocketFrame::isControlOpCode(frame.opCode))
        return "Received new data frame but previous continuous frame is unfinished."_s;

    if (!m_hasContinuousFrame && frame.opCode == WebSocketFrame::OpCodeContinuation)
        return "Received unexpected continuation frame."_s;

    return std::nullopt;
}

void PortWebSocketChannel::sendClosingHandshakeIfNeeded(int32_t code, const String& reason)
{
    if (m_didSendClosingHandshake)
        return;

    Vector<uint8_t> buf;
    if (!m_receivedClosingHandshake && code != CloseEventCodeNotSpecified) {
        buf.append(static_cast<uint8_t>(static_cast<unsigned short>(code) >> 8));
        buf.append(static_cast<uint8_t>(static_cast<unsigned short>(code)));
        buf.append(reason.utf8().span());
    }

    if (!sendFrame(WebSocketFrame::OpCodeClose, buf.span()))
        networkFailed("Failed to send WebSocket frame."_s);

    m_state = State::Closing;
    m_didSendClosingHandshake = true;
}

bool PortWebSocketChannel::sendFrame(WebSocketFrame::OpCode opCode, std::span<const uint8_t> data)
{
    if (m_didSendClosingHandshake)
        return true;
    if (isStreamInvalidated())
        return false;

    WebSocketFrame frame(opCode, true, false, true, data);
    m_inspector.didSendWebSocketFrame(frame);

    auto deflateResult = m_deflateFramer.deflate(frame);
    if (!deflateResult->succeeded()) {
        networkFailed(String(deflateResult->failureReason()));
        return false;
    }

    Vector<uint8_t> frameData;
    frame.makeFrameData(frameData);

    auto buffer = makeUniqueArray<uint8_t>(frameData.size());
    memcpySpan(unsafeMakeSpan(buffer.get(), frameData.size()), frameData.span());

    m_scheduler.send(m_streamID, WTF::move(buffer), frameData.size());
    return true;
}

void PortWebSocketChannel::networkFailed(String&& reason)
{
    if (m_reportedFailure)
        return;
    m_reportedFailure = true;

    // Hybi-10 7.1.7: once the connection has failed, stop handling incoming data.
    m_shouldDiscardReceivedData = true;
    m_receiveBuffer.clear();
    m_deflateFramer.didFail();
    m_hasContinuousFrame = false;
    m_continuousFrameData.clear();

    Ref protectedThis { *this };
    logErrorMessage(reason);
    if (RefPtr client = m_client.get())
        client->didReceiveMessageError(WTF::move(reason));

    didCloseInternal(CloseEventCodeAbnormalClosure, { });
}

void PortWebSocketChannel::didCloseInternal(int32_t code, const String& reason)
{
    destructStream();

    if (m_state == State::Closed)
        return;
    m_state = State::Closed;

    // Deferred like WebSocketTaskCurl::didClose(): this can run from inside the frame
    // callback of receiveFrames(), and WebSocket::didClose() drops the last reference to
    // the channel while that loop still owns `this`.
    callOnMainThread([protectedThis = Ref { *this }, code, reason = reason.isolatedCopy()] {
        RefPtr client = protectedThis->m_client.get();
        if (!client)
            return;

        bool receivedClosingHandshake = code != CloseEventCodeAbnormalClosure;
        if (receivedClosingHandshake)
            client->didStartClosingHandshake();

        client->didClose(protectedThis->m_bufferedAmount,
            (protectedThis->m_isClosing || receivedClosingHandshake) ? WebSocketChannelClient::ClosingHandshakeComplete : WebSocketChannelClient::ClosingHandshakeIncomplete,
            static_cast<unsigned short>(code), reason);
    });
}

void PortWebSocketChannel::destructStream()
{
    if (isStreamInvalidated())
        return;

    m_scheduler.destroyStream(m_streamID);
    m_streamID = invalidCurlStreamID;
}

} // namespace WebCorePort
