// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/Peer.h"

#include "BanManager.h"
#include "crypto/CryptoError.h"
#include "crypto/Hex.h"
#include "crypto/Random.h"
#include "crypto/SHA.h"
#include "database/Database.h"
#include "herder/Herder.h"
#include "herder/TxSetFrame.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/OverlayManager.h"
#include "overlay/OverlayMetrics.h"
#include "overlay/PeerAuth.h"
#include "overlay/PeerManager.h"
#include "overlay/StellarXDR.h"
#include "overlay/SurveyManager.h"
#include "util/Decoder.h"
#include "util/Logging.h"
#include "util/XDROperators.h"

#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "medida/timer.h"
#include "xdrpp/marshal.h"
#include <fmt/format.h>

#include <Tracy.hpp>
#include <soci.h>
#include <time.h>

// LATER: need to add some way of docking peers that are misbehaving by sending
// you bad data

namespace stellar
{

using namespace std;
using namespace soci;

static constexpr VirtualClock::time_point PING_NOT_SENT =
    VirtualClock::time_point::min();

Peer::Peer(Application& app, PeerRole role)
    : mApp(app)
    , mRole(role)
    , mState(role == WE_CALLED_REMOTE ? CONNECTING : CONNECTED)
    , mRemoteOverlayMinVersion(0)
    , mRemoteOverlayVersion(0)
    , mCreationTime(app.getClock().now())
    , mRecurringTimer(app)
    , mLastRead(app.getClock().now())
    , mLastWrite(app.getClock().now())
    , mEnqueueTimeOfLastWrite(app.getClock().now())
    , mPeerMetrics(app.getClock().now())
{
    mPingSentTime = PING_NOT_SENT;
    mLastPing = std::chrono::hours(24); // some default very high value
    auto bytes = randomBytes(mSendNonce.size());
    std::copy(bytes.begin(), bytes.end(), mSendNonce.begin());
}

void
Peer::sendHello()
{
    ZoneScoped;
    CLOG_DEBUG(Overlay, "Peer::sendHello to {} @{}", toString(),
               mApp.getConfig().PEER_PORT);
    StellarMessage msg;
    msg.type(HELLO);
    Hello& elo = msg.hello();
    auto& cfg = mApp.getConfig();
    elo.ledgerVersion = cfg.LEDGER_PROTOCOL_VERSION;
    elo.overlayMinVersion = cfg.OVERLAY_PROTOCOL_MIN_VERSION;
    elo.overlayVersion = cfg.OVERLAY_PROTOCOL_VERSION;
    elo.versionStr = cfg.VERSION_STR;
    elo.networkID = mApp.getNetworkID();
    elo.listeningPort = cfg.PEER_PORT;
    elo.peerID = cfg.NODE_SEED.getPublicKey();
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

    mLastRead = mApp.getClock().now();
    if (gotFullMessage)
    {
        getOverlayMetrics().mMessageRead.Mark();
        ++mPeerMetrics.mMessageRead;
    }
    getOverlayMetrics().mByteRead.Mark(byteCount);
    mPeerMetrics.mByteRead += byteCount;
}

void
Peer::startRecurrentTimer()
{
    constexpr std::chrono::seconds RECURRENT_TIMER_PERIOD(5);

    if (shouldAbort())
    {
        return;
    }

    pingPeer();

    auto self = shared_from_this();
    mRecurringTimer.expires_from_now(RECURRENT_TIMER_PERIOD);
    mRecurringTimer.async_wait([self](asio::error_code const& error) {
        self->recurrentTimerExpired(error);
    });
}

void
Peer::recurrentTimerExpired(asio::error_code const& error)
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
        else if (((now - mEnqueueTimeOfLastWrite) >= stragglerTimeout))
        {
            getOverlayMetrics().mTimeoutStraggler.Mark();
            drop("straggling (cannot keep up)",
                 Peer::DropDirection::WE_DROPPED_REMOTE,
                 Peer::DropMode::IGNORE_WRITE_QUEUE);
        }
        else
        {
            startRecurrentTimer();
        }
    }
}

void
Peer::sendAuth()
{
    ZoneScoped;
    StellarMessage msg;
    msg.type(AUTH);
    sendMessage(msg);
}

std::string const&
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
        CLOG_DEBUG(Overlay, "Connected to {} @{}", toString(),
                   mApp.getConfig().PEER_PORT);
        connected();
        mState = CONNECTED;
        sendHello();
    }
}

void
Peer::sendDontHave(MessageType type, uint256 const& itemID)
{
    ZoneScoped;
    StellarMessage msg;
    msg.type(DONT_HAVE);
    msg.dontHave().reqHash = itemID;
    msg.dontHave().type = type;

    sendMessage(msg);
}

void
Peer::sendSCPQuorumSet(SCPQuorumSetPtr qSet)
{
    ZoneScoped;
    StellarMessage msg;
    msg.type(SCP_QUORUMSET);
    msg.qSet() = *qSet;

    sendMessage(msg);
}

void
Peer::sendGetTxSet(uint256 const& setID)
{
    ZoneScoped;
    StellarMessage newMsg;
    newMsg.type(GET_TX_SET);
    newMsg.txSetHash() = setID;

    sendMessage(newMsg);
}

void
Peer::sendGetQuorumSet(uint256 const& setID)
{
    ZoneScoped;
    StellarMessage newMsg;
    newMsg.type(GET_SCP_QUORUMSET);
    newMsg.qSetHash() = setID;

    sendMessage(newMsg);
}

void
Peer::sendGetPeers()
{
    ZoneScoped;
    StellarMessage newMsg;
    newMsg.type(GET_PEERS);

    sendMessage(newMsg);
}

void
Peer::sendGetScpState(uint32 ledgerSeq)
{
    ZoneScoped;
    StellarMessage newMsg;
    newMsg.type(GET_SCP_STATE);
    newMsg.getSCPLedgerSeq() = ledgerSeq;

    sendMessage(newMsg);
}

void
Peer::sendPeers()
{
    ZoneScoped;
    StellarMessage newMsg;
    newMsg.type(PEERS);
    uint32 maxPeerCount = std::min<uint32>(50, newMsg.peers().max_size());

    // send top peers we know about
    auto peers = mApp.getOverlayManager().getPeerManager().getPeersToSend(
        maxPeerCount, mAddress);
    assert(peers.size() <= maxPeerCount);

    if (!peers.empty())
    {
        newMsg.peers().reserve(peers.size());
        for (auto const& address : peers)
        {
            newMsg.peers().push_back(toXdr(address));
        }
        sendMessage(newMsg);
    }
}

void
Peer::sendError(ErrorCode error, std::string const& message)
{
    ZoneScoped;
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
    ZoneScoped;
    sendError(error, message);
    drop(message, DropDirection::WE_DROPPED_REMOTE, dropMode);
}

std::string
Peer::msgSummary(StellarMessage const& msg)
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
        return fmt::format("DONTHAVE {}:{}", msg.dontHave().type,
                           hexAbbrev(msg.dontHave().reqHash));
    case GET_PEERS:
        return "GETPEERS";
    case PEERS:
        return fmt::format("PEERS {}", msg.peers().size());

    case GET_TX_SET:
        return fmt::format("GETTXSET {}", hexAbbrev(msg.txSetHash()));
    case TX_SET:
        return "TXSET";

    case TRANSACTION:
        return "TRANSACTION";

    case GET_SCP_QUORUMSET:
        return fmt::format("GET_SCP_QSET {}", hexAbbrev(msg.qSetHash()));
    case SCP_QUORUMSET:
        return "SCP_QSET";
    case SCP_MESSAGE:
    {
        std::string t;
        switch (msg.envelope().statement.pledges.type())
        {
        case SCP_ST_PREPARE:
            t = "SCP::PREPARE";
            break;
        case SCP_ST_CONFIRM:
            t = "SCP::CONFIRM";
            break;
        case SCP_ST_EXTERNALIZE:
            t = "SCP::EXTERNALIZE";
            break;
        case SCP_ST_NOMINATE:
            t = "SCP::NOMINATE";
            break;
        default:
            t = "unknown";
        }
        return fmt::format(
            "{} ({})", t,
            mApp.getConfig().toShortString(msg.envelope().statement.nodeID));
    }
    case GET_SCP_STATE:
        return fmt::format("GET_SCP_STATE {}", msg.getSCPLedgerSeq());

    case SURVEY_REQUEST:
    case SURVEY_RESPONSE:
        return SurveyManager::getMsgSummary(msg);
    }
    return "UNKNOWN";
}

void
Peer::sendMessage(StellarMessage const& msg, bool log)
{
    ZoneScoped;
    //    if (log && Logging::logTrace("Overlay"))
    //    {
    CLOG_TRACE(Overlay, "send: {} to : {} @{}", msgSummary(msg),
               mApp.getConfig().toShortString(mPeerID),
               mApp.getConfig().PEER_PORT);
    //    }

    // There are really _two_ layers of queues, one in Scheduler for actions and
    // one in Peer (and its subclasses) for outgoing writes. We enforce a
    // similar load-shedding discipline here as in Scheduler: if there is more
    // than the scheduler latency-window worth of material in the write queue,
    // and we're being asked to add messages that are being generated _from_ a
    // droppable action, we drop the message rather than enqueue it. This avoids
    // growing our queues indefinitely.
    if (mApp.getClock().currentSchedulerActionType() ==
            Scheduler::ActionType::DROPPABLE_ACTION &&
        sendQueueIsOverloaded())
    {
        getOverlayMetrics().mMessageDrop.Mark();
        return;
    }

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
    case SURVEY_REQUEST:
        getOverlayMetrics().mSendSurveyRequestMeter.Mark();
        break;
    case SURVEY_RESPONSE:
        getOverlayMetrics().mSendSurveyResponseMeter.Mark();
        break;
    };

    AuthenticatedMessage amsg;
    amsg.v0().message = msg;
    if (msg.type() != HELLO && msg.type() != ERROR_MSG)
    {
        ZoneNamedN(hmacZone, "message HMAC", true);
        amsg.v0().sequence = mSendMacSeq;
        amsg.v0().mac =
            hmacSha256(mSendMacKey, xdr::xdr_to_opaque(mSendMacSeq, msg));
        ++mSendMacSeq;
    }
    xdr::msg_ptr xdrBytes;
    {
        ZoneNamedN(xdrZone, "XDR serialize", true);
        xdrBytes = xdr::xdr_to_msg(amsg);
    }
    this->sendMessage(std::move(xdrBytes));
}

void
Peer::recvMessage(xdr::msg_ptr const& msg)
{
    ZoneScoped;
    if (shouldAbort())
    {
        return;
    }

    try
    {
        ZoneNamedN(hmacZone, "message HMAC", true);
        AuthenticatedMessage am;
        {
            ZoneNamedN(xdrZone, "XDR deserialize", true);
            xdr::xdr_from_msg(msg, am);
        }
        recvMessage(am);
    }
    catch (xdr::xdr_runtime_error& e)
    {
        CLOG_ERROR(Overlay, "received corrupt xdr::msg_ptr {}", e.what());
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
    ZoneScoped;
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
    ZoneScoped;
    if (shouldAbort())
    {
        return;
    }

    char const* cat = nullptr;
    Scheduler::ActionType type = Scheduler::ActionType::NORMAL_ACTION;
    switch (stellarMsg.type())
    {
    // group messages used during handshake, process those synchronously
    case MessageType::HELLO:
    case MessageType::AUTH:
        Peer::recvRawMessage(stellarMsg);
        return;

    // control messages
    case GET_PEERS:
    case PEERS:
    case ERROR_MSG:
        cat = "CTRL";
        break;

    // high volume flooding
    case TRANSACTION:
        cat = "TX";
        type = Scheduler::ActionType::DROPPABLE_ACTION;
        break;

    // consensus, inbound
    case GET_TX_SET:
    case GET_SCP_QUORUMSET:
    case GET_SCP_STATE:
        cat = "SCPQ";
        type = Scheduler::ActionType::DROPPABLE_ACTION;
        break;

    // consensus, self
    case DONT_HAVE:
    case TX_SET:
    case SCP_QUORUMSET:
    case SCP_MESSAGE:
        cat = "SCP";
        break;

    default:
        cat = "MISC";
    }

    std::weak_ptr<Peer> weak(static_pointer_cast<Peer>(shared_from_this()));
    mApp.postOnMainThread(
        [weak, sm = StellarMessage(stellarMsg), mtype = stellarMsg.type(), cat,
         port = mApp.getConfig().PEER_PORT]() {
            auto self = weak.lock();
            if (self)
            {
                try
                {
                    self->recvRawMessage(sm);
                }
                catch (CryptoError const& e)
                {
                    std::string err =
                        fmt::format("Error RecvMessage T:{} cat:{} {} @{}",
                                    mtype, cat, self->toString(), port);
                    CLOG_ERROR(Overlay, "Dropping connection with {}: {}", err,
                               e.what());
                    self->drop("Bad crypto request",
                               Peer::DropDirection::WE_DROPPED_REMOTE,
                               Peer::DropMode::IGNORE_WRITE_QUEUE);
                }
            }
            else
            {
                CLOG_TRACE(Overlay, "Error RecvMessage T:{} cat:{}", mtype,
                           cat);
            }
        },
        fmt::format("{} recvMessage", cat), type);
}

void
Peer::recvRawMessage(StellarMessage const& stellarMsg)
{
    ZoneScoped;
    auto peerStr = toString();
    ZoneText(peerStr.c_str(), peerStr.size());

    if (shouldAbort())
    {
        return;
    }

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
    mApp.getOverlayManager().recordMessageMetric(stellarMsg,
                                                 shared_from_this());

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

    case SURVEY_REQUEST:
    {
        auto t = getOverlayMetrics().mRecvSurveyRequestTimer.TimeScope();
        recvSurveyRequestMessage(stellarMsg);
    }
    break;

    case SURVEY_RESPONSE:
    {
        auto t = getOverlayMetrics().mRecvSurveyResponseTimer.TimeScope();
        recvSurveyResponseMessage(stellarMsg);
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
    ZoneScoped;
    maybeProcessPingResponse(msg.dontHave().reqHash);

    mApp.getHerder().peerDoesntHave(msg.dontHave().type, msg.dontHave().reqHash,
                                    shared_from_this());
}

void
Peer::recvGetTxSet(StellarMessage const& msg)
{
    ZoneScoped;
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
    ZoneScoped;
    TxSetFrame frame(mApp.getNetworkID(), msg.txSet());
    mApp.getHerder().recvTxSet(frame.getContentsHash(), frame);
}

void
Peer::recvTransaction(StellarMessage const& msg)
{
    ZoneScoped;
    auto transaction = TransactionFrameBase::makeTransactionFromWire(
        mApp.getNetworkID(), msg.transaction());
    if (transaction)
    {
        // record that this peer sent us this transaction
        // add it to the floodmap so that this peer gets credit for it
        Hash msgID;
        mApp.getOverlayManager().recvFloodedMsgID(msg, shared_from_this(),
                                                  msgID);

        // add it to our current set
        // and make sure it is valid
        auto recvRes = mApp.getHerder().recvTransaction(transaction);

        if (!(recvRes == TransactionQueue::AddResult::ADD_STATUS_PENDING ||
              recvRes == TransactionQueue::AddResult::ADD_STATUS_DUPLICATE))
        {
            mApp.getOverlayManager().forgetFloodedMsg(msgID);
        }
    }
}

Hash
Peer::pingIDfromTimePoint(VirtualClock::time_point const& tp)
{
    auto sh = shortHash::xdrComputeHash(
        xdr::xdr_to_opaque(uint64_t(tp.time_since_epoch().count())));
    Hash res;
    assert(res.size() >= sizeof(sh));
    std::memcpy(res.data(), &sh, sizeof(sh));
    return res;
}

void
Peer::pingPeer()
{
    if (isAuthenticated() && mPingSentTime == PING_NOT_SENT)
    {
        mPingSentTime = mApp.getClock().now();
        auto h = pingIDfromTimePoint(mPingSentTime);
        sendGetQuorumSet(h);
    }
}

void
Peer::maybeProcessPingResponse(Hash const& id)
{
    if (mPingSentTime != PING_NOT_SENT)
    {
        auto h = pingIDfromTimePoint(mPingSentTime);
        if (h == id)
        {
            mLastPing = std::chrono::duration_cast<std::chrono::milliseconds>(
                mApp.getClock().now() - mPingSentTime);
            mPingSentTime = PING_NOT_SENT;
            CLOG_DEBUG(Overlay, "Latency {}: {} ms", toString(),
                       mLastPing.count());
            getOverlayMetrics().mConnectionLatencyTimer.Update(mLastPing);
        }
    }
}

std::chrono::milliseconds
Peer::getPing() const
{
    return mLastPing;
}

void
Peer::recvGetSCPQuorumSet(StellarMessage const& msg)
{
    ZoneScoped;
    maybeProcessPingResponse(msg.qSetHash());

    SCPQuorumSetPtr qset = mApp.getHerder().getQSet(msg.qSetHash());

    if (qset)
    {
        sendSCPQuorumSet(qset);
    }
    else
    {
        if (Logging::logTrace("Overlay"))
            CLOG_TRACE(Overlay, "No quorum set: {}", hexAbbrev(msg.qSetHash()));
        sendDontHave(SCP_QUORUMSET, msg.qSetHash());
        // do we want to ask other people for it?
    }
}
void
Peer::recvSCPQuorumSet(StellarMessage const& msg)
{
    ZoneScoped;
    Hash hash = xdrSha256(msg.qSet());
    mApp.getHerder().recvSCPQuorumSet(hash, msg.qSet());
}

void
Peer::recvSCPMessage(StellarMessage const& msg)
{
    ZoneScoped;
    SCPEnvelope const& envelope = msg.envelope();

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
    std::string codeStr;
    switch (type)
    {
    case SCP_ST_PREPARE:
        codeStr = "PREPARE";
        break;
    case SCP_ST_CONFIRM:
        codeStr = "CONFIRM";
        break;
    case SCP_ST_EXTERNALIZE:
        codeStr = "EXTERNALIZE";
        break;
    case SCP_ST_NOMINATE:
    default:
        codeStr = "NOMINATE";
        break;
    }
    ZoneText(codeStr.c_str(), codeStr.size());

    // add it to the floodmap so that this peer gets credit for it
    Hash msgID;
    mApp.getOverlayManager().recvFloodedMsgID(msg, shared_from_this(), msgID);

    auto res = mApp.getHerder().recvSCPEnvelope(envelope);
    if (res == Herder::ENVELOPE_STATUS_DISCARDED)
    {
        // the message was discarded, remove it from the floodmap as well
        mApp.getOverlayManager().forgetFloodedMsg(msgID);
    }
}

void
Peer::recvGetSCPState(StellarMessage const& msg)
{
    ZoneScoped;
    uint32 seq = msg.getSCPLedgerSeq();
    mApp.getHerder().sendSCPStateToPeer(seq, shared_from_this());
}

void
Peer::recvError(StellarMessage const& msg)
{
    ZoneScoped;
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

    std::string msgStr;
    msgStr.reserve(msg.error().msg.size());
    std::transform(msg.error().msg.begin(), msg.error().msg.end(),
                   std::back_inserter(msgStr),
                   [](char c) { return (isalnum(c) || c == ' ') ? c : '*'; });

    drop(fmt::format("{} ({})", codeStr, msgStr),
         Peer::DropDirection::REMOTE_DROPPED_US,
         Peer::DropMode::IGNORE_WRITE_QUEUE);
}

void
Peer::updatePeerRecordAfterEcho()
{
    assert(!getAddress().isEmpty());

    auto type = mApp.getOverlayManager().isPreferred(this)
                    ? PeerType::PREFERRED
                    : mRole == WE_CALLED_REMOTE ? PeerType::OUTBOUND
                                                : PeerType::INBOUND;
    // Now that we've done authentication, we know whether this peer is
    // preferred or not
    mApp.getOverlayManager().getPeerManager().update(
        getAddress(), type,
        /* preferredTypeKnown */ true);
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

    CLOG_DEBUG(Overlay, "successful handshake with {}@{}",
               mApp.getConfig().toShortString(mPeerID),
               getAddress().toString());
}

void
Peer::recvHello(Hello const& elo)
{
    ZoneScoped;
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
    if (ip.empty())
    {
        drop("failed to determine remote address",
             Peer::DropDirection::WE_DROPPED_REMOTE,
             Peer::DropMode::IGNORE_WRITE_QUEUE);
        return;
    }
    mAddress =
        PeerBareAddress{ip, static_cast<unsigned short>(elo.listeningPort)};

    CLOG_DEBUG(Overlay, "recvHello from {} @{}", toString(),
               mApp.getConfig().PEER_PORT);

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
        CLOG_DEBUG(Overlay, "Protocol = [{},{}] expected: [{},{}]",
                   mRemoteOverlayMinVersion, mRemoteOverlayVersion,
                   mApp.getConfig().OVERLAY_PROTOCOL_MIN_VERSION,
                   mApp.getConfig().OVERLAY_PROTOCOL_VERSION);
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
        CLOG_WARNING(Overlay, "Connection from peer with different NetworkID");
        CLOG_WARNING(Overlay, "Check your configuration file settings: "
                              "KNOWN_PEERS and PREFERRED_PEERS for peers "
                              "that are from other networks.");
        CLOG_DEBUG(Overlay, "NetworkID = {} expected: {}",
                   hexAbbrev(elo.networkID), hexAbbrev(mApp.getNetworkID()));
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
    ZoneScoped;
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

    auto low = mApp.getHerder().getMinLedgerSeqToAskPeers();
    sendGetScpState(low);
}

void
Peer::recvGetPeers(StellarMessage const& msg)
{
    ZoneScoped;
    sendPeers();
}

void
Peer::recvPeers(StellarMessage const& msg)
{
    ZoneScoped;
    for (auto const& peer : msg.peers())
    {
        if (peer.port == 0 || peer.port > UINT16_MAX)
        {
            CLOG_DEBUG(Overlay, "ignoring received peer with bad port {}",
                       peer.port);
            continue;
        }
        if (peer.ip.type() == IPv6)
        {
            CLOG_DEBUG(Overlay,
                       "ignoring received IPv6 address (not yet supported)");
            continue;
        }

        assert(peer.ip.type() == IPv4);
        auto address = PeerBareAddress{peer};

        if (address.isPrivate())
        {
            CLOG_DEBUG(Overlay, "ignoring received private address {}",
                       address.toString());
        }
        else if (address == PeerBareAddress{getAddress().getIP(),
                                            mApp.getConfig().PEER_PORT})
        {
            CLOG_DEBUG(Overlay, "ignoring received self-address {}",
                       address.toString());
        }
        else if (address.isLocalhost() &&
                 !mApp.getConfig().ALLOW_LOCALHOST_FOR_TESTING)
        {
            CLOG_DEBUG(Overlay, "ignoring received localhost");
        }
        else
        {
            // don't use peer.numFailures here as we may have better luck
            // (and we don't want to poison our failure count)
            mApp.getOverlayManager().getPeerManager().ensureExists(address);
        }
    }
}

void
Peer::recvSurveyRequestMessage(StellarMessage const& msg)
{
    ZoneScoped;
    mApp.getOverlayManager().getSurveyManager().relayOrProcessRequest(
        msg, shared_from_this());
}

void
Peer::recvSurveyResponseMessage(StellarMessage const& msg)
{
    ZoneScoped;
    mApp.getOverlayManager().getSurveyManager().relayOrProcessResponse(
        msg, shared_from_this());
}

Peer::PeerMetrics::PeerMetrics(VirtualClock::time_point connectedTime)
    : mMessageRead(0)
    , mMessageWrite(0)
    , mByteRead(0)
    , mByteWrite(0)
    , mUniqueFloodBytesRecv(0)
    , mDuplicateFloodBytesRecv(0)
    , mUniqueFetchBytesRecv(0)
    , mDuplicateFetchBytesRecv(0)
    , mUniqueFloodMessageRecv(0)
    , mDuplicateFloodMessageRecv(0)
    , mUniqueFetchMessageRecv(0)
    , mDuplicateFetchMessageRecv(0)
    , mConnectedTime(connectedTime)
{
}
}
