// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/Peer.h"

#include "BanManager.h"
#include "crypto/Hex.h"
#include "crypto/Random.h"
#include "crypto/SHA.h"
#include "database/Database.h"
#include "herder/Herder.h"
#include "herder/TxSetFrame.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/LoadManager.h"
#include "overlay/OverlayManager.h"
#include "overlay/OverlayMetrics.h"
#include "overlay/PeerAuth.h"
#include "overlay/PeerManager.h"
#include "overlay/StellarXDR.h"
#include "util/Logging.h"
#include "util/XDROperators.h"

#include "lib/util/format.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "medida/timer.h"
#include "xdrpp/marshal.h"

#include <soci.h>
#include <time.h>

// LATER: need to add some way of docking peers that are misbehaving by sending
// you bad data

namespace stellar
{

using namespace std;
using namespace soci;

Peer::Peer(Application& app, PeerRole role)
    : mApp(app)
    , mRole(role)
    , mState(role == WE_CALLED_REMOTE ? CONNECTING : CONNECTED)
    , mRemoteOverlayMinVersion(0)
    , mRemoteOverlayVersion(0)
    , mCreationTime(app.getClock().now())
    , mIdleTimer(app)
    , mLastRead(app.getClock().now())
    , mLastWrite(app.getClock().now())
    , mLastEmpty(app.getClock().now())
{
    auto bytes = randomBytes(mSendNonce.size());
    std::copy(bytes.begin(), bytes.end(), mSendNonce.begin());
}

void
Peer::sendHello()
{
    CLOG(DEBUG, "Overlay") << "Peer::sendHello to " << toString() << " @"
                           << mApp.getConfig().PEER_PORT;
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
    elo.cert = this->getAuthCert();
    elo.nonce = mSendNonce;
    sendMessage(msg);
}

AuthCert
Peer::getAuthCert()
{
    return mApp.getOverlayManager().getPeerAuth().getAuthCert();
}

OverlayMetrics&
Peer::getOverlayMetrics()
{
    return mApp.getOverlayManager().getOverlayMetrics();
}

std::chrono::seconds
Peer::getIOTimeout() const
{
    if (isAuthenticated())
    {
        // Normally willing to wait 30s to hear anything
        // from an authenticated peer.
        return std::chrono::seconds(mApp.getConfig().PEER_TIMEOUT);
    }
    else
    {
        // We give peers much less timing leeway while
        // performing handshake.
        return std::chrono::seconds(
            mApp.getConfig().PEER_AUTHENTICATION_TIMEOUT);
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
        getOverlayMetrics().mMessageRead.Mark();
    getOverlayMetrics().mByteRead.Mark(byteCount);
}

void
Peer::startIdleTimer()
{
    if (shouldAbort())
    {
        return;
    }

    auto self = shared_from_this();
    mIdleTimer.expires_from_now(getIOTimeout());
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
        auto timeout = getIOTimeout();
        auto stragglerTimeout =
            std::chrono::seconds(mApp.getConfig().PEER_STRAGGLER_TIMEOUT);
        if (((now - mLastRead) >= timeout) && ((now - mLastWrite) >= timeout))
        {
            getOverlayMetrics().mTimeoutIdle.Mark();
            drop("idle timeout", Peer::DropDirection::WE_DROPPED_REMOTE,
                 Peer::DropMode::IGNORE_WRITE_QUEUE);
        }
        else if (((now - mLastEmpty) >= stragglerTimeout))
        {
            getOverlayMetrics().mTimeoutStraggler.Mark();
            drop("straggling (cannot keep up)",
                 Peer::DropDirection::WE_DROPPED_REMOTE,
                 Peer::DropMode::IGNORE_WRITE_QUEUE);
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
Peer::connectHandler(asio::error_code const& error)
{
    if (error)
    {
        drop("unable to connect: " + error.message(),
             Peer::DropDirection::WE_DROPPED_REMOTE,
             Peer::DropMode::IGNORE_WRITE_QUEUE);
    }
    else
    {
        CLOG(DEBUG, "Overlay") << "Connected to " << toString() << " @"
                               << mApp.getConfig().PEER_PORT;
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
        CLOG(TRACE, "Overlay") << "Get quorum set: " << hexAbbrev(setID) << " @"
                               << mApp.getConfig().PEER_PORT;

    StellarMessage newMsg;
    newMsg.type(GET_SCP_QUORUMSET);
    newMsg.qSetHash() = setID;

    sendMessage(newMsg);
}

void
Peer::sendGetPeers()
{
    CLOG(TRACE, "Overlay") << "Get peers @" << mApp.getConfig().PEER_PORT;

    StellarMessage newMsg;
    newMsg.type(GET_PEERS);

    sendMessage(newMsg);
}

void
Peer::sendGetScpState(uint32 ledgerSeq)
{
    CLOG(TRACE, "Overlay") << "Get SCP State for " << ledgerSeq << " @"
                           << mApp.getConfig().PEER_PORT;

    StellarMessage newMsg;
    newMsg.type(GET_SCP_STATE);
    newMsg.getSCPLedgerSeq() = ledgerSeq;

    sendMessage(newMsg);
}

void
Peer::sendPeers()
{
    StellarMessage newMsg;
    newMsg.type(PEERS);
    uint32 maxPeerCount = std::min<uint32>(50, newMsg.peers().max_size());

    // send top peers we know about
    auto peers = mApp.getOverlayManager().getPeerManager().getPeersToSend(
        maxPeerCount, mAddress);
    assert(peers.size() <= maxPeerCount);

    newMsg.peers().reserve(peers.size());
    for (auto const& address : peers)
    {
        newMsg.peers().push_back(toXdr(address));
    }
    sendMessage(newMsg);
}

void
Peer::sendError(ErrorCode error, std::string const& message)
{
    StellarMessage m;
    m.type(ERROR_MSG);
    m.error().code = error;
    m.error().msg = message;
    sendMessage(m);
}

void
Peer::sendErrorAndDrop(ErrorCode error, std::string const& message,
                       DropMode dropMode)
{
    sendError(error, message);
    drop(message, DropDirection::WE_DROPPED_REMOTE, dropMode);
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
            << "send: " << msgSummary(msg)
            << " to : " << mApp.getConfig().toShortString(mPeerID) << " @"
            << mApp.getConfig().PEER_PORT;

    switch (msg.type())
    {
    case ERROR_MSG:
        getOverlayMetrics().mSendErrorMeter.Mark();
        break;
    case HELLO:
        getOverlayMetrics().mSendHelloMeter.Mark();
        break;
    case AUTH:
        getOverlayMetrics().mSendAuthMeter.Mark();
        break;
    case DONT_HAVE:
        getOverlayMetrics().mSendDontHaveMeter.Mark();
        break;
    case GET_PEERS:
        getOverlayMetrics().mSendGetPeersMeter.Mark();
        break;
    case PEERS:
        getOverlayMetrics().mSendPeersMeter.Mark();
        break;
    case GET_TX_SET:
        getOverlayMetrics().mSendGetTxSetMeter.Mark();
        break;
    case TX_SET:
        getOverlayMetrics().mSendTxSetMeter.Mark();
        break;
    case TRANSACTION:
        getOverlayMetrics().mSendTransactionMeter.Mark();
        break;
    case GET_SCP_QUORUMSET:
        getOverlayMetrics().mSendGetSCPQuorumSetMeter.Mark();
        break;
    case SCP_QUORUMSET:
        getOverlayMetrics().mSendSCPQuorumSetMeter.Mark();
        break;
    case SCP_MESSAGE:
        getOverlayMetrics().mSendSCPMessageSetMeter.Mark();
        break;
    case GET_SCP_STATE:
        getOverlayMetrics().mSendGetSCPStateMeter.Mark();
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
    this->sendMessage(std::move(xdrBytes));
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
        drop("received corrupted message",
             Peer::DropDirection::WE_DROPPED_REMOTE,
             Peer::DropMode::IGNORE_WRITE_QUEUE);
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

std::chrono::seconds
Peer::getLifeTime() const
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        mApp.getClock().now() - mCreationTime);
}

bool
Peer::shouldAbort() const
{
    return (mState == CLOSING) || mApp.getOverlayManager().isShuttingDown();
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
            ++mRecvMacSeq;
            sendErrorAndDrop(ERR_AUTH, "unexpected auth sequence",
                             DropMode::IGNORE_WRITE_QUEUE);
            return;
        }

        if (!hmacSha256Verify(
                msg.v0().mac, mRecvMacKey,
                xdr::xdr_to_opaque(msg.v0().sequence, msg.v0().message)))
        {
            ++mRecvMacSeq;
            sendErrorAndDrop(ERR_AUTH, "unexpected MAC",
                             DropMode::IGNORE_WRITE_QUEUE);
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
            << "recv: " << msgSummary(stellarMsg)
            << " from:" << mApp.getConfig().toShortString(mPeerID) << " @"
            << mApp.getConfig().PEER_PORT;

    if (!isAuthenticated() && (stellarMsg.type() != HELLO) &&
        (stellarMsg.type() != AUTH) && (stellarMsg.type() != ERROR_MSG))
    {
        drop(fmt::format("received {} before completed handshake",
                         stellarMsg.type()),
             Peer::DropDirection::WE_DROPPED_REMOTE,
             Peer::DropMode::IGNORE_WRITE_QUEUE);
        return;
    }

    assert(isAuthenticated() || stellarMsg.type() == HELLO ||
           stellarMsg.type() == AUTH || stellarMsg.type() == ERROR_MSG);
    mApp.getOverlayManager().recordDuplicateMessageMetric(stellarMsg);

    switch (stellarMsg.type())
    {
    case ERROR_MSG:
    {
        auto t = getOverlayMetrics().mRecvErrorTimer.TimeScope();
        recvError(stellarMsg);
    }
    break;

    case HELLO:
    {
        auto t = getOverlayMetrics().mRecvHelloTimer.TimeScope();
        this->recvHello(stellarMsg.hello());
    }
    break;

    case AUTH:
    {
        auto t = getOverlayMetrics().mRecvAuthTimer.TimeScope();
        this->recvAuth(stellarMsg);
    }
    break;

    case DONT_HAVE:
    {
        auto t = getOverlayMetrics().mRecvDontHaveTimer.TimeScope();
        recvDontHave(stellarMsg);
    }
    break;

    case GET_PEERS:
    {
        auto t = getOverlayMetrics().mRecvGetPeersTimer.TimeScope();
        recvGetPeers(stellarMsg);
    }
    break;

    case PEERS:
    {
        auto t = getOverlayMetrics().mRecvPeersTimer.TimeScope();
        recvPeers(stellarMsg);
    }
    break;

    case GET_TX_SET:
    {
        auto t = getOverlayMetrics().mRecvGetTxSetTimer.TimeScope();
        recvGetTxSet(stellarMsg);
    }
    break;

    case TX_SET:
    {
        auto t = getOverlayMetrics().mRecvTxSetTimer.TimeScope();
        recvTxSet(stellarMsg);
    }
    break;

    case TRANSACTION:
    {
        auto t = getOverlayMetrics().mRecvTransactionTimer.TimeScope();
        recvTransaction(stellarMsg);
    }
    break;

    case GET_SCP_QUORUMSET:
    {
        auto t = getOverlayMetrics().mRecvGetSCPQuorumSetTimer.TimeScope();
        recvGetSCPQuorumSet(stellarMsg);
    }
    break;

    case SCP_QUORUMSET:
    {
        auto t = getOverlayMetrics().mRecvSCPQuorumSetTimer.TimeScope();
        recvSCPQuorumSet(stellarMsg);
    }
    break;

    case SCP_MESSAGE:
    {
        auto t = getOverlayMetrics().mRecvSCPMessageTimer.TimeScope();
        recvSCPMessage(stellarMsg);
    }
    break;

    case GET_SCP_STATE:
    {
        auto t = getOverlayMetrics().mRecvGetSCPStateTimer.TimeScope();
        recvGetSCPState(stellarMsg);
    }
    break;
    }
}

void
Peer::recvDontHave(StellarMessage const& msg)
{
    mApp.getHerder().peerDoesntHave(msg.dontHave().type, msg.dontHave().reqHash,
                                    shared_from_this());
}

void
Peer::recvGetTxSet(StellarMessage const& msg)
{
    auto self = shared_from_this();
    if (auto txSet = mApp.getHerder().getTxSet(msg.txSetHash()))
    {
        StellarMessage newMsg;
        newMsg.type(TX_SET);
        txSet->toXDR(newMsg.txSet());

        self->sendMessage(newMsg);
    }
    else
    {
        sendDontHave(TX_SET, msg.txSetHash());
    }
}

void
Peer::recvTxSet(StellarMessage const& msg)
{
    TxSetFrame frame(mApp.getNetworkID(), msg.txSet());
    mApp.getHerder().recvTxSet(frame.getContentsHash(), frame);
}

void
Peer::recvTransaction(StellarMessage const& msg)
{
    TransactionFramePtr transaction = TransactionFrame::makeTransactionFromWire(
        mApp.getNetworkID(), msg.transaction());
    if (transaction)
    {
        // add it to our current set
        // and make sure it is valid
        auto recvRes = mApp.getHerder().recvTransaction(transaction);

        if (recvRes == TransactionQueue::AddResult::ADD_STATUS_PENDING ||
            recvRes == TransactionQueue::AddResult::ADD_STATUS_DUPLICATE)
        {
            // record that this peer sent us this transaction
            mApp.getOverlayManager().recvFloodedMsg(msg, shared_from_this());

            if (recvRes == TransactionQueue::AddResult::ADD_STATUS_PENDING)
            {
                // if it's a new transaction, broadcast it
                mApp.getOverlayManager().broadcastMessage(msg);
            }
        }
    }
}

void
Peer::recvGetSCPQuorumSet(StellarMessage const& msg)
{
    SCPQuorumSetPtr qset = mApp.getHerder().getQSet(msg.qSetHash());

    if (qset)
    {
        sendSCPQuorumSet(qset);
    }
    else
    {
        if (Logging::logTrace("Overlay"))
            CLOG(TRACE, "Overlay")
                << "No quorum set: " << hexAbbrev(msg.qSetHash());
        sendDontHave(SCP_QUORUMSET, msg.qSetHash());
        // do we want to ask other people for it?
    }
}
void
Peer::recvSCPQuorumSet(StellarMessage const& msg)
{
    Hash hash = sha256(xdr::xdr_to_opaque(msg.qSet()));
    mApp.getHerder().recvSCPQuorumSet(hash, msg.qSet());
}

void
Peer::recvSCPMessage(StellarMessage const& msg)
{
    SCPEnvelope const& envelope = msg.envelope();
    if (Logging::logTrace("Overlay"))
        CLOG(TRACE, "Overlay")
            << "recvSCPMessage node: "
            << mApp.getConfig().toShortString(msg.envelope().statement.nodeID)
            << " @" << mApp.getConfig().PEER_PORT;

    auto type = msg.envelope().statement.pledges.type();
    auto t = (type == SCP_ST_PREPARE
                  ? getOverlayMetrics().mRecvSCPPrepareTimer.TimeScope()
                  : (type == SCP_ST_CONFIRM
                         ? getOverlayMetrics().mRecvSCPConfirmTimer.TimeScope()
                         : (type == SCP_ST_EXTERNALIZE
                                ? getOverlayMetrics()
                                      .mRecvSCPExternalizeTimer.TimeScope()
                                : (getOverlayMetrics()
                                       .mRecvSCPNominateTimer.TimeScope()))));

    auto res = mApp.getHerder().recvSCPEnvelope(envelope);
    if (res != Herder::ENVELOPE_STATUS_DISCARDED)
    {
        mApp.getOverlayManager().recvFloodedMsg(msg, shared_from_this());
    }
}

void
Peer::recvGetSCPState(StellarMessage const& msg)
{
    uint32 seq = msg.getSCPLedgerSeq();
    CLOG(TRACE, "Overlay") << "get SCP State " << seq << " @"
                           << mApp.getConfig().PEER_PORT;
    mApp.getHerder().sendSCPStateToPeer(seq, shared_from_this());
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
    drop(fmt::format("{} ({})", codeStr, std::string{msg.error().msg}),
         Peer::DropDirection::REMOTE_DROPPED_US,
         Peer::DropMode::IGNORE_WRITE_QUEUE);
}

void
Peer::updatePeerRecordAfterEcho()
{
    assert(!getAddress().isEmpty());

    auto type = mApp.getOverlayManager().isPreferred(this)
                    ? PeerManager::TypeUpdate::SET_PREFERRED
                    : mRole == WE_CALLED_REMOTE
                          ? PeerManager::TypeUpdate::SET_OUTBOUND
                          : PeerManager::TypeUpdate::REMOVE_PREFERRED;
    mApp.getOverlayManager().getPeerManager().update(getAddress(), type);
}

void
Peer::updatePeerRecordAfterAuthentication()
{
    assert(!getAddress().isEmpty());

    if (mRole == WE_CALLED_REMOTE)
    {
        mApp.getOverlayManager().getPeerManager().update(
            getAddress(), PeerManager::BackOffUpdate::RESET);
    }

    CLOG(DEBUG, "Overlay") << "successful handshake with "
                           << mApp.getConfig().toShortString(mPeerID) << "@"
                           << getAddress().toString();
}

void
Peer::recvHello(Hello const& elo)
{
    if (mState >= GOT_HELLO)
    {
        drop("received unexpected HELLO",
             Peer::DropDirection::WE_DROPPED_REMOTE,
             Peer::DropMode::IGNORE_WRITE_QUEUE);
        return;
    }

    auto& peerAuth = mApp.getOverlayManager().getPeerAuth();
    if (!peerAuth.verifyRemoteAuthCert(elo.peerID, elo.cert))
    {
        drop("failed to verify auth cert",
             Peer::DropDirection::WE_DROPPED_REMOTE,
             Peer::DropMode::IGNORE_WRITE_QUEUE);
        return;
    }

    if (mApp.getBanManager().isBanned(elo.peerID))
    {
        drop("node is banned", Peer::DropDirection::WE_DROPPED_REMOTE,
             Peer::DropMode::IGNORE_WRITE_QUEUE);
        return;
    }

    mRemoteOverlayMinVersion = elo.overlayMinVersion;
    mRemoteOverlayVersion = elo.overlayVersion;
    mRemoteVersion = elo.versionStr;
    mPeerID = elo.peerID;
    mRecvNonce = elo.nonce;
    mSendMacSeq = 0;
    mRecvMacSeq = 0;
    mSendMacKey = peerAuth.getSendingMacKey(elo.cert.pubkey, mSendNonce,
                                            mRecvNonce, mRole);
    mRecvMacKey = peerAuth.getReceivingMacKey(elo.cert.pubkey, mSendNonce,
                                              mRecvNonce, mRole);

    mState = GOT_HELLO;

    auto ip = getIP();
    mAddress =
        PeerBareAddress{ip, static_cast<unsigned short>(elo.listeningPort)};

    CLOG(DEBUG, "Overlay") << "recvHello from " << toString() << " @"
                           << mApp.getConfig().PEER_PORT;

    auto dropMode = Peer::DropMode::IGNORE_WRITE_QUEUE;
    if (mRole == REMOTE_CALLED_US)
    {
        // Send a HELLO back, even if it's going to be followed
        // immediately by ERROR, because ERROR is an authenticated
        // message type and the caller won't decode it right if
        // still waiting for an unauthenticated HELLO.
        sendHello();
        dropMode = Peer::DropMode::FLUSH_WRITE_QUEUE;
    }

    if (mRemoteOverlayMinVersion > mRemoteOverlayVersion ||
        mRemoteOverlayVersion < mApp.getConfig().OVERLAY_PROTOCOL_MIN_VERSION ||
        mRemoteOverlayMinVersion > mApp.getConfig().OVERLAY_PROTOCOL_VERSION)
    {
        CLOG(DEBUG, "Overlay")
            << "Protocol = [" << mRemoteOverlayMinVersion << ","
            << mRemoteOverlayVersion << "] expected: ["
            << mApp.getConfig().OVERLAY_PROTOCOL_VERSION << ","
            << mApp.getConfig().OVERLAY_PROTOCOL_VERSION << "]";
        sendErrorAndDrop(ERR_CONF, "wrong protocol version", dropMode);
        return;
    }

    if (elo.peerID == mApp.getConfig().NODE_SEED.getPublicKey())
    {
        sendErrorAndDrop(ERR_CONF, "connecting to self", dropMode);
        return;
    }

    if (elo.networkID != mApp.getNetworkID())
    {
        CLOG(WARNING, "Overlay")
            << "Connection from peer with different NetworkID";
        CLOG(WARNING, "Overlay") << "Check your configuration file settings: "
                                    "KNOWN_PEERS and PREFERRED_PEERS for peers "
                                    "that are from other networks.";
        CLOG(DEBUG, "Overlay")
            << "NetworkID = " << hexAbbrev(elo.networkID)
            << " expected: " << hexAbbrev(mApp.getNetworkID());
        sendErrorAndDrop(ERR_CONF, "wrong network passphrase", dropMode);
        return;
    }

    if (elo.listeningPort <= 0 || elo.listeningPort > UINT16_MAX || ip.empty())
    {
        sendErrorAndDrop(ERR_CONF, "bad address",
                         Peer::DropMode::IGNORE_WRITE_QUEUE);
        return;
    }

    updatePeerRecordAfterEcho();

    auto const& authenticated =
        mApp.getOverlayManager().getAuthenticatedPeers();
    auto authenticatedIt = authenticated.find(mPeerID);
    // no need to self-check here as this one cannot be in authenticated yet
    if (authenticatedIt != std::end(authenticated))
    {
        if (&(authenticatedIt->second->mPeerID) != &mPeerID)
        {
            sendErrorAndDrop(ERR_CONF,
                             "already-connected peer: " +
                                 mApp.getConfig().toShortString(mPeerID),
                             dropMode);
            return;
        }
    }

    for (auto const& p : mApp.getOverlayManager().getPendingPeers())
    {
        if (&(p->mPeerID) == &mPeerID)
        {
            continue;
        }
        if (p->getPeerID() == mPeerID)
        {
            sendErrorAndDrop(ERR_CONF,
                             "already-connected peer: " +
                                 mApp.getConfig().toShortString(mPeerID),
                             dropMode);
            return;
        }
    }

    if (mRole == WE_CALLED_REMOTE)
    {
        sendAuth();
    }
}

void
Peer::recvAuth(StellarMessage const& msg)
{
    if (mState != GOT_HELLO)
    {
        sendErrorAndDrop(ERR_MISC, "out-of-order AUTH message",
                         DropMode::IGNORE_WRITE_QUEUE);
        return;
    }

    if (isAuthenticated())
    {
        sendErrorAndDrop(ERR_MISC, "out-of-order AUTH message",
                         DropMode::IGNORE_WRITE_QUEUE);
        return;
    }

    mState = GOT_AUTH;

    if (mRole == REMOTE_CALLED_US)
    {
        sendAuth();
        sendPeers();
    }

    updatePeerRecordAfterAuthentication();

    auto self = shared_from_this();
    if (!mApp.getOverlayManager().acceptAuthenticatedPeer(self))
    {
        sendErrorAndDrop(ERR_LOAD, "peer rejected",
                         Peer::DropMode::FLUSH_WRITE_QUEUE);
        return;
    }

    // ask for SCP state if not synced
    // this requests data for slots lcl-window ... latest consensus (if
    // possible) we need to ask for older slots in case other peers need
    // messages we didn't need ourself
    auto low = mApp.getLedgerManager().getLastClosedLedgerNum() + 1;
    if (low > Herder::MAX_SLOTS_TO_REMEMBER)
    {
        low -= Herder::MAX_SLOTS_TO_REMEMBER;
    }
    else
    {
        low = 1;
    }
    sendGetScpState(low);
}

void
Peer::recvGetPeers(StellarMessage const& msg)
{
    sendPeers();
}

void
Peer::recvPeers(StellarMessage const& msg)
{
    for (auto const& peer : msg.peers())
    {
        if (peer.port == 0 || peer.port > UINT16_MAX)
        {
            CLOG(DEBUG, "Overlay")
                << "ignoring received peer with bad port " << peer.port;
            continue;
        }
        if (peer.ip.type() == IPv6)
        {
            CLOG(DEBUG, "Overlay") << "ignoring received IPv6 address"
                                   << " (not yet supported)";
            continue;
        }

        assert(peer.ip.type() == IPv4);
        auto address = PeerBareAddress{peer};

        if (address.isPrivate())
        {
            CLOG(DEBUG, "Overlay")
                << "ignoring received private address " << address.toString();
        }
        else if (address == PeerBareAddress{getAddress().getIP(),
                                            mApp.getConfig().PEER_PORT})
        {
            CLOG(DEBUG, "Overlay")
                << "ignoring received self-address " << address.toString();
        }
        else if (address.isLocalhost() &&
                 !mApp.getConfig().ALLOW_LOCALHOST_FOR_TESTING)
        {
            CLOG(DEBUG, "Overlay") << "ignoring received localhost";
        }
        else
        {
            // don't use peer.numFailures here as we may have better luck
            // (and we don't want to poison our failure count)
            mApp.getOverlayManager().getPeerManager().ensureExists(address);
        }
    }
}
}
