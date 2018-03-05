// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transport/Peer.h"
#include "crypto/Hex.h"
#include "crypto/Random.h"
#include "crypto/SHA.h"
#include "database/Database.h"
#include "item/ItemKey.h"
#include "main/Application.h"
#include "transport/EnvelopeHandler.h"
#include "transport/LoadManager.h"
#include "transport/MessageHandler.h"
#include "transport/PeerAuth.h"
#include "transport/PeerRecord.h"
#include "transport/TransactionHandler.h"
#include "util/Logging.h"
#include "util/XDROperators.h"

#include <medida/meter.h>
#include <medida/metrics_registry.h>
#include <medida/timer.h>
#include <xdrpp/marshal.h>

// LATER: need to add some way of docking peers that are misbehaving by sending
// you bad data

namespace stellar
{

medida::Meter&
Peer::getByteReadMeter(Application& app)
{
    return app.getMetrics().NewMeter({"overlay", "byte", "read"}, "byte");
}

medida::Meter&
Peer::getByteWriteMeter(Application& app)
{
    return app.getMetrics().NewMeter({"overlay", "byte", "write"}, "byte");
}

Peer::Peer(Application& app, PeerRole role)
    : mApp(app)
    , mRole(role)
    , mState(role == WE_CALLED_REMOTE ? CONNECTING : CONNECTED)
    , mRemoteOverlayVersion(0)
    , mIdleTimer(app)
    , mLastRead(app.getClock().now())
    , mLastWrite(app.getClock().now())

    , mMessageRead(
          app.getMetrics().NewMeter({"overlay", "message", "read"}, "message"))
    , mMessageWrite(
          app.getMetrics().NewMeter({"overlay", "message", "write"}, "message"))
    , mByteRead(getByteReadMeter(app))
    , mByteWrite(getByteWriteMeter(app))
    , mErrorRead(
          app.getMetrics().NewMeter({"overlay", "error", "read"}, "error"))
    , mErrorWrite(
          app.getMetrics().NewMeter({"overlay", "error", "write"}, "error"))
    , mTimeoutIdle(
          app.getMetrics().NewMeter({"overlay", "timeout", "idle"}, "timeout"))

    , mRecvErrorTimer(app.getMetrics().NewTimer({"overlay", "recv", "error"}))
    , mRecvHelloTimer(app.getMetrics().NewTimer({"overlay", "recv", "hello"}))
    , mRecvAuthTimer(app.getMetrics().NewTimer({"overlay", "recv", "auth"}))
    , mRecvDontHaveTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "dont-have"}))
    , mRecvGetPeersTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "get-peers"}))
    , mRecvPeersTimer(app.getMetrics().NewTimer({"overlay", "recv", "peers"}))
    , mRecvGetTxSetTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "get-txset"}))
    , mRecvTxSetTimer(app.getMetrics().NewTimer({"overlay", "recv", "txset"}))
    , mRecvTransactionTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "transaction"}))
    , mRecvGetSCPQuorumSetTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "get-scp-qset"}))
    , mRecvSCPQuorumSetTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "scp-qset"}))
    , mRecvSCPMessageTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "scp-message"}))
    , mRecvGetSCPStateTimer(
          app.getMetrics().NewTimer({"overlay", "recv", "get-scp-state"}))

    , mSendErrorMeter(
          app.getMetrics().NewMeter({"overlay", "send", "error"}, "message"))
    , mSendHelloMeter(
          app.getMetrics().NewMeter({"overlay", "send", "hello"}, "message"))
    , mSendAuthMeter(
          app.getMetrics().NewMeter({"overlay", "send", "auth"}, "message"))
    , mSendDontHaveMeter(app.getMetrics().NewMeter(
          {"overlay", "send", "dont-have"}, "message"))
    , mSendGetPeersMeter(app.getMetrics().NewMeter(
          {"overlay", "send", "get-peers"}, "message"))
    , mSendPeersMeter(
          app.getMetrics().NewMeter({"overlay", "send", "peers"}, "message"))
    , mSendGetTxSetMeter(app.getMetrics().NewMeter(
          {"overlay", "send", "get-txset"}, "message"))
    , mSendTransactionMeter(app.getMetrics().NewMeter(
          {"overlay", "send", "transaction"}, "message"))
    , mSendTxSetMeter(
          app.getMetrics().NewMeter({"overlay", "send", "txset"}, "message"))
    , mSendGetSCPQuorumSetMeter(app.getMetrics().NewMeter(
          {"overlay", "send", "get-scp-qset"}, "message"))
    , mSendSCPQuorumSetMeter(
          app.getMetrics().NewMeter({"overlay", "send", "scp-qset"}, "message"))
    , mSendSCPMessageSetMeter(app.getMetrics().NewMeter(
          {"overlay", "send", "scp-message"}, "message"))
    , mSendGetSCPStateMeter(app.getMetrics().NewMeter(
          {"overlay", "send", "get-scp-state"}, "message"))
    , mDropInConnectHandlerMeter(app.getMetrics().NewMeter(
          {"overlay", "drop", "connect-handler"}, "drop"))
    , mDropInRecvMessageDecodeMeter(app.getMetrics().NewMeter(
          {"overlay", "drop", "recv-message-decode"}, "drop"))
    , mDropInRecvMessageSeqMeter(app.getMetrics().NewMeter(
          {"overlay", "drop", "recv-message-seq"}, "drop"))
    , mDropInRecvMessageMacMeter(app.getMetrics().NewMeter(
          {"overlay", "drop", "recv-message-mac"}, "drop"))
    , mDropInRecvMessageUnauthMeter(app.getMetrics().NewMeter(
          {"overlay", "drop", "recv-message-unauth"}, "drop"))
    , mDropInRecvHelloUnexpectedMeter(app.getMetrics().NewMeter(
          {"overlay", "drop", "recv-hello-unexpected"}, "drop"))
    , mDropInRecvHelloVersionMeter(app.getMetrics().NewMeter(
          {"overlay", "drop", "recv-hello-version"}, "drop"))
    , mDropInRecvHelloAddressMeter(app.getMetrics().NewMeter(
          {"overlay", "drop", "recv-hello-address"}, "drop"))
    , mDropInRecvAuthUnexpectedMeter(app.getMetrics().NewMeter(
          {"overlay", "drop", "recv-auth-unexpected"}, "drop"))
    , mDropInRecvAuthRejectMeter(app.getMetrics().NewMeter(
          {"overlay", "drop", "recv-auth-reject"}, "drop"))
    , mDropInRecvAuthInvalidPeerMeter(app.getMetrics().NewMeter(
          {"overlay", "drop", "recv-auth-invalid-peer"}, "drop"))
    , mDropInRecvErrorMeter(
          app.getMetrics().NewMeter({"overlay", "drop", "recv-error"}, "drop"))
{
    auto bytes = randomBytes(mSendNonce.size());
    std::copy(bytes.begin(), bytes.end(), mSendNonce.begin());
}

void
Peer::sendHello()
{
    CLOG(DEBUG, "Overlay") << "Peer::sendHello to " << toString();
    StellarMessage msg;
    msg.type(HELLO);
    Hello& elo = msg.hello();
    elo.ledgerVersion = mApp.getConfig().LEDGER_PROTOCOL_VERSION;
    elo.overlayMinVersion = mApp.getConfig().OVERLAY_PROTOCOL_MIN_VERSION;
    elo.overlayVersion = mApp.getConfig().OVERLAY_PROTOCOL_VERSION;
    elo.versionStr = mApp.getConfig().VERSION_STR;
    elo.networkID = mApp.getNetworkID();
    elo.listeningPort = mApp.getConfig().PEER_PORT;
    elo.peerID = mApp.getConfig().NODE_SEED.getPublicKey();
    elo.cert = getAuthCert();
    elo.nonce = mSendNonce;
    sendMessage(msg);
}

AuthCert
Peer::getAuthCert()
{
    return mApp.getPeerAuth().getAuthCert();
}

size_t
Peer::getIOTimeoutSeconds() const
{
    if (isAuthenticated())
    {
        // Normally willing to wait 30s to hear anything
        // from an authenticated peer.
        return mApp.getConfig().PEER_TIMEOUT;
    }
    else
    {
        // We give peers much less timing leeway while
        // performing handshake.
        return mApp.getConfig().PEER_AUTHENTICATION_TIMEOUT;
    }
}

void
Peer::receivedBytes(size_t byteCount, bool gotFullMessage)
{
    if (shouldAbort())
    {
        return;
    }

    LoadManager::PeerContext loadCtx(mApp, mPeerID);
    mLastRead = mApp.getClock().now();
    if (gotFullMessage)
        mMessageRead.Mark();
    mByteRead.Mark(byteCount);
}

void
Peer::startIdleTimer()
{
    if (shouldAbort())
    {
        return;
    }

    auto self = shared_from_this();
    mIdleTimer.expires_from_now(std::chrono::seconds(getIOTimeoutSeconds()));
    mIdleTimer.async_wait([self](asio::error_code const& error) {
        self->idleTimerExpired(error);
    });
}

void
Peer::idleTimerExpired(asio::error_code const& error)
{
    if (!error)
    {
        auto now = mApp.getClock().now();
        auto timeout = std::chrono::seconds(getIOTimeoutSeconds());
        if (((now - mLastRead) >= timeout) && ((now - mLastWrite) >= timeout))
        {
            CLOG(WARNING, "Overlay") << "idle timeout";
            mTimeoutIdle.Mark();
            drop();
        }
        else
        {
            startIdleTimer();
        }
    }
}

void
Peer::sendAuth()
{
    StellarMessage msg;
    msg.type(AUTH);
    sendMessage(msg);
}

std::string
Peer::toString()
{
    return mAddress.toString();
}

void
Peer::drop(ErrorCode err, std::string const& msg)
{
    StellarMessage m;
    m.type(ERROR_MSG);
    m.error().code = err;
    m.error().msg = msg;
    sendMessage(m);
    // note: this used to be a post which caused delays in stopping
    // to process read messages.
    // this will try to send all data from send queue if the error is ERR_LOAD
    // - it sends list of peers
    drop(err != ERR_LOAD);
}

void
Peer::connectHandler(asio::error_code const& error)
{
    if (error)
    {
        CLOG(WARNING, "Overlay")
            << " connectHandler error: " << error.message();
        mDropInConnectHandlerMeter.Mark();
        drop();
    }
    else
    {
        CLOG(DEBUG, "Overlay") << "connected " << toString();
        connected();
        mState = CONNECTED;
        sendHello();
    }
}

void
Peer::sendDontHave(MessageType type, uint256 const& itemID)
{
    StellarMessage msg;
    msg.type(DONT_HAVE);
    msg.dontHave().reqHash = itemID;
    msg.dontHave().type = type;

    sendMessage(msg);
}

void
Peer::sendSCPQuorumSet(SCPQuorumSetPtr qSet)
{
    StellarMessage msg;
    msg.type(SCP_QUORUMSET);
    msg.qSet() = *qSet;

    sendMessage(msg);
}
void
Peer::sendGetTxSet(uint256 const& setID)
{
    StellarMessage newMsg;
    newMsg.type(GET_TX_SET);
    newMsg.txSetHash() = setID;

    sendMessage(newMsg);
}

void
Peer::sendGetQuorumSet(uint256 const& setID)
{
    if (Logging::logTrace("Overlay"))
        CLOG(TRACE, "Overlay") << "Get quorum set: " << hexAbbrev(setID);

    StellarMessage newMsg;
    newMsg.type(GET_SCP_QUORUMSET);
    newMsg.qSetHash() = setID;

    sendMessage(newMsg);
}

void
Peer::sendGetPeers()
{
    CLOG(TRACE, "Overlay") << "Get peers";

    StellarMessage newMsg;
    newMsg.type(GET_PEERS);

    sendMessage(newMsg);
}

void
Peer::sendGetScpState(uint32 ledgerSeq)
{
    CLOG(TRACE, "Overlay") << "Get SCP State for " << ledgerSeq;

    StellarMessage newMsg;
    newMsg.type(GET_SCP_STATE);
    newMsg.getSCPLedgerSeq() = ledgerSeq;

    sendMessage(newMsg);
}

void
Peer::sendPeers(std::vector<PeerRecord> const& peers)
{
    StellarMessage newMsg;
    newMsg.type(PEERS);
    newMsg.peers().reserve(peers.size());

    for (auto const& pr : peers)
    {
        PeerAddress pa;
        pr.toXdr(pa);
        newMsg.peers().push_back(pa);
    }
    sendMessage(newMsg);
}

static std::string
msgSummary(StellarMessage const& msg)
{
    switch (msg.type())
    {
    case ERROR_MSG:
        return "ERROR";
    case HELLO:
        return "HELLO";
    case AUTH:
        return "AUTH";
    case DONT_HAVE:
        return "DONTHAVE";
    case GET_PEERS:
        return "GETPEERS";
    case PEERS:
        return "PEERS";

    case GET_TX_SET:
        return "GETTXSET";
    case TX_SET:
        return "TXSET";

    case TRANSACTION:
        return "TRANSACTION";

    case GET_SCP_QUORUMSET:
        return "GET_SCP_QSET";
    case SCP_QUORUMSET:
        return "SCP_QSET";
    case SCP_MESSAGE:
        switch (msg.envelope().statement.pledges.type())
        {
        case SCP_ST_PREPARE:
            return "SCP::PREPARE";
        case SCP_ST_CONFIRM:
            return "SCP::CONFIRM";
        case SCP_ST_EXTERNALIZE:
            return "SCP::EXTERNALIZE";
        case SCP_ST_NOMINATE:
            return "SCP::NOMINATE";
        }
    case GET_SCP_STATE:
        return "GET_SCP_STATE";
    }
    return "UNKNOWN";
}

void
Peer::sendMessage(StellarMessage const& msg)
{
    if (Logging::logTrace("Overlay"))
        CLOG(TRACE, "Overlay")
            << "("
            << mApp.getConfig().toShortString(
                   mApp.getConfig().NODE_SEED.getPublicKey())
            << ") send: " << msgSummary(msg)
            << " to : " << mApp.getConfig().toShortString(mPeerID);

    switch (msg.type())
    {
    case ERROR_MSG:
        mSendErrorMeter.Mark();
        break;
    case HELLO:
        mSendHelloMeter.Mark();
        break;
    case AUTH:
        mSendAuthMeter.Mark();
        break;
    case DONT_HAVE:
        mSendDontHaveMeter.Mark();
        break;
    case GET_PEERS:
        mSendGetPeersMeter.Mark();
        break;
    case PEERS:
        mSendPeersMeter.Mark();
        break;
    case GET_TX_SET:
        mSendGetTxSetMeter.Mark();
        break;
    case TX_SET:
        mSendTxSetMeter.Mark();
        break;
    case TRANSACTION:
        mSendTransactionMeter.Mark();
        break;
    case GET_SCP_QUORUMSET:
        mSendGetSCPQuorumSetMeter.Mark();
        break;
    case SCP_QUORUMSET:
        mSendSCPQuorumSetMeter.Mark();
        break;
    case SCP_MESSAGE:
        mSendSCPMessageSetMeter.Mark();
        break;
    case GET_SCP_STATE:
        mSendGetSCPStateMeter.Mark();
        break;
    };

    AuthenticatedMessage amsg;
    amsg.v0().message = msg;
    if (msg.type() != HELLO && msg.type() != ERROR_MSG)
    {
        amsg.v0().sequence = mSendMacSeq;
        amsg.v0().mac =
            hmacSha256(mSendMacKey, xdr::xdr_to_opaque(mSendMacSeq, msg));
        ++mSendMacSeq;
    }
    xdr::msg_ptr xdrBytes(xdr::xdr_to_msg(amsg));
    sendMessage(std::move(xdrBytes));
}

void
Peer::recvMessage(xdr::msg_ptr const& msg)
{
    if (shouldAbort())
    {
        return;
    }

    LoadManager::PeerContext loadCtx(mApp, mPeerID);

    CLOG(TRACE, "Overlay") << "received xdr::msg_ptr";
    try
    {
        AuthenticatedMessage am;
        xdr::xdr_from_msg(msg, am);
        recvMessage(am);
    }
    catch (xdr::xdr_runtime_error& e)
    {
        CLOG(ERROR, "Overlay") << "received corrupt xdr::msg_ptr " << e.what();
        mDropInRecvMessageDecodeMeter.Mark();
        drop();
        return;
    }
}

bool
Peer::isConnected() const
{
    return mState != CONNECTING && mState != CLOSING;
}

bool
Peer::isAuthenticated() const
{
    return mState == GOT_AUTH;
}

bool
Peer::shouldAbort() const
{
    return (mState == CLOSING) ||
           mApp.getMessageHandler().shouldAbort(shared_from_this());
}

void
Peer::recvMessage(AuthenticatedMessage const& msg)
{
    if (shouldAbort())
    {
        return;
    }

    if (mState >= GOT_HELLO && msg.v0().message.type() != ERROR_MSG)
    {
        if (msg.v0().sequence != mRecvMacSeq)
        {
            CLOG(ERROR, "Overlay") << "Unexpected message-auth sequence";
            mDropInRecvMessageSeqMeter.Mark();
            ++mRecvMacSeq;
            drop(ERR_AUTH, "unexpected auth sequence");
            return;
        }

        if (!hmacSha256Verify(
                msg.v0().mac, mRecvMacKey,
                xdr::xdr_to_opaque(msg.v0().sequence, msg.v0().message)))
        {
            CLOG(ERROR, "Overlay") << "Message-auth check failed";
            mDropInRecvMessageMacMeter.Mark();
            ++mRecvMacSeq;
            drop(ERR_AUTH, "unexpected MAC");
            return;
        }
        ++mRecvMacSeq;
    }
    recvMessage(msg.v0().message);
}

void
Peer::recvMessage(StellarMessage const& stellarMsg)
{
    if (shouldAbort())
    {
        return;
    }

    if (Logging::logTrace("Overlay"))
        CLOG(TRACE, "Overlay")
            << "("
            << mApp.getConfig().toShortString(
                   mApp.getConfig().NODE_SEED.getPublicKey())
            << ") recv: " << msgSummary(stellarMsg)
            << " from:" << mApp.getConfig().toShortString(mPeerID);

    if (!isAuthenticated() && (stellarMsg.type() != HELLO) &&
        (stellarMsg.type() != AUTH) && (stellarMsg.type() != ERROR_MSG))
    {
        CLOG(WARNING, "Overlay")
            << "recv: " << stellarMsg.type() << " before completed handshake";
        mDropInRecvMessageUnauthMeter.Mark();
        drop();
        return;
    }

    assert(isAuthenticated() || stellarMsg.type() == HELLO ||
           stellarMsg.type() == AUTH || stellarMsg.type() == ERROR_MSG);

    auto self = shared_from_this();
    switch (stellarMsg.type())
    {
    case ERROR_MSG:
    {
        auto t = mRecvErrorTimer.TimeScope();
        recvError(stellarMsg);
    }
    break;

    case HELLO:
    {
        auto t = mRecvHelloTimer.TimeScope();
        recvHello(stellarMsg.hello());
    }
    break;

    case AUTH:
    {
        auto t = mRecvAuthTimer.TimeScope();
        recvAuth(stellarMsg);
    }
    break;

    case DONT_HAVE:
    {
        auto t = mRecvDontHaveTimer.TimeScope();
        recvDontHave(stellarMsg);
    }
    break;

    case GET_PEERS:
    {
        auto t = mRecvGetPeersTimer.TimeScope();
        mApp.getMessageHandler().getPeers(self);
    }
    break;

    case PEERS:
    {
        auto t = mRecvPeersTimer.TimeScope();
        mApp.getMessageHandler().peers(self, stellarMsg.peers());
    }
    break;

    case GET_TX_SET:
    {
        auto t = mRecvGetTxSetTimer.TimeScope();
        mApp.getMessageHandler().getTxSet(self, stellarMsg.txSetHash());
    }
    break;

    case TX_SET:
    {
        auto t = mRecvTxSetTimer.TimeScope();
        mApp.getMessageHandler().txSet(self, stellarMsg.txSet());
    }
    break;

    case TRANSACTION:
    {
        auto t = mRecvTransactionTimer.TimeScope();
        mApp.getTransactionHandler().transaction(self,
                                                 stellarMsg.transaction());
    }
    break;

    case GET_SCP_QUORUMSET:
    {
        auto t = mRecvGetSCPQuorumSetTimer.TimeScope();
        mApp.getMessageHandler().getQuorumSet(self, stellarMsg.qSetHash());
    }
    break;

    case SCP_QUORUMSET:
    {
        auto t = mRecvSCPQuorumSetTimer.TimeScope();
        mApp.getMessageHandler().quorumSet(self, stellarMsg.qSet());
    }
    break;

    case SCP_MESSAGE:
    {
        auto t = mRecvSCPMessageTimer.TimeScope();
        mApp.getEnvelopeHandler().envelope(self, stellarMsg.envelope());
    }
    break;

    case GET_SCP_STATE:
    {
        auto t = mRecvGetSCPStateTimer.TimeScope();
        mApp.getMessageHandler().getSCPState(self,
                                             stellarMsg.getSCPLedgerSeq());
    }
    break;
    }
}

void
Peer::recvDontHave(StellarMessage const& msg)
{
    ItemType itemType;
    switch (msg.dontHave().type)
    {
    case TX_SET:
    {
        itemType = ItemType::TX_SET;
        break;
    }
    case SCP_QUORUMSET:
    {
        itemType = ItemType::QUORUM_SET;
        break;
    }
    default:
    {
        assert(false);
    }
    }

    mApp.getMessageHandler().doesNotHave(
        shared_from_this(), ItemKey{itemType, msg.dontHave().reqHash});
}

void
Peer::recvError(StellarMessage const& msg)
{
    std::string codeStr = "UNKNOWN";
    switch (msg.error().code)
    {
    case ERR_MISC:
        codeStr = "ERR_MISC";
        break;
    case ERR_DATA:
        codeStr = "ERR_DATA";
        break;
    case ERR_CONF:
        codeStr = "ERR_CONF";
        break;
    case ERR_AUTH:
        codeStr = "ERR_AUTH";
        break;
    case ERR_LOAD:
        codeStr = "ERR_LOAD";
        break;
    default:
        break;
    }
    CLOG(WARNING, "Overlay")
        << "Received error (" << codeStr << "): " << msg.error().msg;
    mDropInRecvErrorMeter.Mark();
    drop();
}

void
Peer::recvHello(Hello const& elo)
{
    if (mState >= GOT_HELLO)
    {
        CLOG(ERROR, "Overlay") << "received unexpected HELLO";
        mDropInRecvHelloUnexpectedMeter.Mark();
        drop();
        return;
    }

    if (elo.overlayMinVersion > elo.overlayVersion ||
        elo.overlayVersion < mApp.getConfig().OVERLAY_PROTOCOL_MIN_VERSION ||
        elo.overlayMinVersion > mApp.getConfig().OVERLAY_PROTOCOL_VERSION)
    {
        CLOG(ERROR, "Overlay") << "connection from peer with incompatible "
                                  "overlay protocol version";
        CLOG(DEBUG, "Overlay")
            << "Protocol = [" << elo.overlayMinVersion << ","
            << elo.overlayVersion << "] expected: ["
            << mApp.getConfig().OVERLAY_PROTOCOL_VERSION << ","
            << mApp.getConfig().OVERLAY_PROTOCOL_VERSION << "]";
        mDropInRecvHelloVersionMeter.Mark();
        drop(ERR_CONF, "wrong protocol version");
        return;
    }

    auto error = mApp.getMessageHandler().acceptHello(shared_from_this(), elo);
    if (!error.first)
    {
        if (error.second.empty())
        {
            drop();
        }
        else
        {
            drop(ERR_CONF, error.second);
        }
        return;
    }

    mAddress = makeAddress(elo.listeningPort);
    if (mAddress.isEmpty())
    {
        CLOG(WARNING, "Overlay") << "bad address in recvHello";
        mDropInRecvHelloAddressMeter.Mark();
        drop(ERR_CONF, "bad address");
        return;
    }

    mRemoteOverlayMinVersion = elo.overlayMinVersion;
    mRemoteOverlayVersion = elo.overlayVersion;
    mRemoteVersion = elo.versionStr;
    mPeerID = elo.peerID;
    mRecvNonce = elo.nonce;
    mSendMacSeq = 0;
    mRecvMacSeq = 0;

    auto& peerAuth = mApp.getPeerAuth();
    mSendMacKey = peerAuth.getSendingMacKey(elo.cert.pubkey, mSendNonce,
                                            mRecvNonce, mRole);
    mRecvMacKey = peerAuth.getReceivingMacKey(elo.cert.pubkey, mSendNonce,
                                              mRecvNonce, mRole);

    mState = GOT_HELLO;
    CLOG(DEBUG, "Overlay") << "recvHello from " << toString();

    switch (mRole)
    {
    case REMOTE_CALLED_US:
        sendHello();
        break;
    case WE_CALLED_REMOTE:
        sendAuth();
        break;
    }
}

void
Peer::recvAuth(StellarMessage const& msg)
{
    if (mState != GOT_HELLO)
    {
        CLOG(INFO, "Overlay") << "Unexpected AUTH message before HELLO";
        mDropInRecvAuthUnexpectedMeter.Mark();
        drop(ERR_MISC, "out-of-order AUTH message");
        return;
    }

    if (isAuthenticated())
    {
        CLOG(INFO, "Overlay") << "Unexpected AUTH message";
        mDropInRecvAuthUnexpectedMeter.Mark();
        drop(ERR_MISC, "out-of-order AUTH message");
        return;
    }

    mState = GOT_AUTH;

    if (mRole == REMOTE_CALLED_US)
    {
        sendAuth();
        mApp.getMessageHandler().getPeers(shared_from_this());
    }

    if (getAddress().isEmpty())
    {
        CLOG(ERROR, "Overlay")
            << "unable to handshake with " << getAddress().toString();
        mDropInRecvAuthInvalidPeerMeter.Mark();
        drop();
        return;
    }

    auto self = shared_from_this();
    if (!mApp.getMessageHandler().acceptAuthenticated(self))
    {
        CLOG(WARNING, "Overlay") << "New peer rejected, all slots taken";
        mDropInRecvAuthRejectMeter.Mark();
        drop(ERR_LOAD, "peer rejected");
    }
}
}
