// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/Peer.h"

#include "BanManager.h"
#include "crypto/BLAKE2.h"
#include "crypto/CryptoError.h"
#include "crypto/Hex.h"
#include "crypto/KeyUtils.h"
#include "crypto/Random.h"
#include "crypto/SHA.h"
#include "database/Database.h"
#include "herder/Herder.h"
#include "herder/TxSetFrame.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/FlowControl.h"
#include "overlay/OverlayManager.h"
#include "overlay/OverlayMetrics.h"
#include "overlay/PeerAuth.h"
#include "overlay/PeerManager.h"
#include "overlay/SurveyDataManager.h"
#include "overlay/SurveyManager.h"
#include "overlay/TxAdverts.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/ProtocolVersion.h"
#include "util/finally.h"

#include "medida/meter.h"
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

static std::string const AUTH_ACTION_QUEUE = "AUTH";
using namespace std;
using namespace soci;

static constexpr VirtualClock::time_point PING_NOT_SENT =
    VirtualClock::time_point::min();
static constexpr uint32_t QUERY_RESPONSE_MULTIPLIER = 5;

Peer::Peer(Application& app, PeerRole role)
    : mAppConnector(app.getAppConnector())
    , mNetworkID(app.getNetworkID())
    , mFlowControl(
          std::make_shared<FlowControl>(mAppConnector, useBackgroundThread()))
    , mLastRead(app.getClock().now())
    , mLastWrite(app.getClock().now())
    , mEnqueueTimeOfLastWrite(app.getClock().now())
    , mRole(role)
    , mOverlayMetrics(app.getOverlayManager().getOverlayMetrics())
    , mPeerMetrics(app.getClock().now())
    , mState(role == WE_CALLED_REMOTE ? CONNECTING : CONNECTED)
    , mRemoteOverlayMinVersion(0)
    , mRemoteOverlayVersion(0)
    , mCreationTime(app.getClock().now())
    , mRecurringTimer(app)
    , mDelayedExecutionTimer(app)
    , mTxAdverts(std::make_shared<TxAdverts>(app))
{
    releaseAssert(threadIsMain());
    mPingSentTime = PING_NOT_SENT;
    mLastPing = std::chrono::hours(24); // some default very high value
    auto bytes = randomBytes(mSendNonce.size());
    std::copy(bytes.begin(), bytes.end(), mSendNonce.begin());
}

CapacityTrackedMessage::CapacityTrackedMessage(std::weak_ptr<Peer> peer,
                                               StellarMessage const& msg)
    : mWeakPeer(peer), mMsg(msg)
{
    auto self = mWeakPeer.lock();
    if (!self)
    {
        throw std::runtime_error("Invalid peer");
    }
    self->beginMessageProcessing(mMsg);
    if (mMsg.type() == SCP_MESSAGE || mMsg.type() == TRANSACTION)
    {
        mMaybeHash = xdrBlake2(msg);
    }
}

std::optional<Hash>
CapacityTrackedMessage::maybeGetHash() const
{
    return mMaybeHash;
}

CapacityTrackedMessage::~CapacityTrackedMessage()
{
    auto self = mWeakPeer.lock();
    if (self)
    {
        self->endMessageProcessing(mMsg);
    }
}

StellarMessage const&
CapacityTrackedMessage::getMessage() const
{
    return mMsg;
}

void
Peer::sendHello()
{
    releaseAssert(threadIsMain());
    ZoneScoped;
    CLOG_DEBUG(Overlay, "Peer::sendHello to {}", toString());
    StellarMessage msg;
    msg.type(HELLO);
    Hello& elo = msg.hello();
    elo.ledgerVersion = mAppConnector.getConfig().LEDGER_PROTOCOL_VERSION;
    elo.overlayMinVersion =
        mAppConnector.getConfig().OVERLAY_PROTOCOL_MIN_VERSION;
    elo.overlayVersion = mAppConnector.getConfig().OVERLAY_PROTOCOL_VERSION;
    elo.versionStr = mAppConnector.getConfig().VERSION_STR;
    elo.networkID = mNetworkID;
    elo.listeningPort = mAppConnector.getConfig().PEER_PORT;
    elo.peerID = mAppConnector.getConfig().NODE_SEED.getPublicKey();
    elo.cert = this->getAuthCert();
    elo.nonce = mSendNonce;

    auto msgPtr = std::make_shared<StellarMessage const>(msg);
    sendMessage(msgPtr);
}

void
Peer::beginMessageProcessing(StellarMessage const& msg)
{
    releaseAssert(mFlowControl);
    auto success = mFlowControl->beginMessageProcessing(msg);
    if (!success)
    {
        drop("unexpected flood message, peer at capacity",
             Peer::DropDirection::WE_DROPPED_REMOTE);
    }
}

void
Peer::endMessageProcessing(StellarMessage const& msg)
{
    RECURSIVE_LOCK_GUARD(mStateMutex, guard);

    if (shouldAbort(guard))
    {
        return;
    }

    releaseAssert(mFlowControl);

    // We may release reading capacity, which gets taken by the background
    // thread immediately, so we can't assert `canRead` here
    auto res = mFlowControl->endMessageProcessing(msg);
    if (res.first > 0 || res.second > 0)
    {
        sendSendMore(static_cast<uint32>(res.first),
                     static_cast<uint32>(res.second));
    }

    // Now that we've released some capacity, maybe schedule more reads
    if (mFlowControl->stopThrottling())
    {
        maybeExecuteInBackground(
            "Peer::stopThrottling scheduleRead",
            [](std::shared_ptr<Peer> self) { self->scheduleRead(); });
    }
}

AuthCert
Peer::getAuthCert()
{
    releaseAssert(threadIsMain());
    return mAppConnector.getOverlayManager().getPeerAuth().getAuthCert();
}

std::chrono::seconds
Peer::getIOTimeout() const
{
    releaseAssert(threadIsMain());
    RECURSIVE_LOCK_GUARD(mStateMutex, guard);
    if (isAuthenticated(guard))
    {
        // Normally willing to wait 30s to hear anything
        // from an authenticated peer.
        return std::chrono::seconds(mAppConnector.getConfig().PEER_TIMEOUT);
    }
    else
    {
        // We give peers much less timing leeway while
        // performing handshake.
        return std::chrono::seconds(
            mAppConnector.getConfig().PEER_AUTHENTICATION_TIMEOUT);
    }
}

void
Peer::receivedBytes(size_t byteCount, bool gotFullMessage)
{
    mLastRead = mAppConnector.now();
    if (gotFullMessage)
    {
        mOverlayMetrics.mMessageRead.Mark();
        ++mPeerMetrics.mMessageRead;
    }
    mOverlayMetrics.mByteRead.Mark(byteCount);
    mPeerMetrics.mByteRead += byteCount;
}

void
Peer::startRecurrentTimer()
{
    releaseAssert(threadIsMain());
    RECURSIVE_LOCK_GUARD(mStateMutex, guard);

    constexpr std::chrono::seconds RECURRENT_TIMER_PERIOD(5);

    if (shouldAbort(guard))
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
Peer::initialize(PeerBareAddress const& address)
{
    releaseAssert(threadIsMain());
    mAddress = address;
    startRecurrentTimer();
}

void
Peer::shutdownAndRemovePeer(std::string const& reason,
                            DropDirection dropDirection)
{
    releaseAssert(threadIsMain());
    RECURSIVE_LOCK_GUARD(mStateMutex, guard);
    auto state = getState(guard);

    if (state != GOT_AUTH)
    {
        CLOG_DEBUG(Overlay,
                   "Dropping peer {} with state {}, role {}, reason {}",
                   toString(), format_as(state), format_as(mRole), reason);
    }
    else if (dropDirection == Peer::DropDirection::WE_DROPPED_REMOTE)
    {
        CLOG_INFO(Overlay, "Dropping peer {}, reason {}", toString(), reason);
    }
    else
    {
        CLOG_INFO(Overlay, "Peer {} dropped us, reason {}", toString(), reason);
    }
#ifdef BUILD_TESTS
    mDropReason = reason;
#endif

    // Set peer state to CLOSING to prevent any further processing
    setState(guard, CLOSING);

    // Remove peer from peer lists tracked by OverlayManager
    mAppConnector.getOverlayManager().removePeer(this);
}

void
Peer::recurrentTimerExpired(asio::error_code const& error)
{
    releaseAssert(threadIsMain());

    if (!error)
    {
        auto now = mAppConnector.now();
        auto timeout = getIOTimeout();
        auto stragglerTimeout = std::chrono::seconds(
            mAppConnector.getConfig().PEER_STRAGGLER_TIMEOUT);
        if (((now - mLastRead.load()) >= timeout) &&
            ((now - mLastWrite.load()) >= timeout))
        {
            mOverlayMetrics.mTimeoutIdle.Mark();
            drop("idle timeout", Peer::DropDirection::WE_DROPPED_REMOTE);
        }
        else if (mFlowControl && mFlowControl->noOutboundCapacityTimeout(
                                     now, Peer::PEER_SEND_MODE_IDLE_TIMEOUT))
        {
            drop("idle timeout (no new flood requests)",
                 Peer::DropDirection::WE_DROPPED_REMOTE);
        }
        else if (((now - mEnqueueTimeOfLastWrite.load()) >= stragglerTimeout))
        {
            mOverlayMetrics.mTimeoutStraggler.Mark();
            drop("straggling (cannot keep up)",
                 Peer::DropDirection::WE_DROPPED_REMOTE);
        }
        else
        {
            startRecurrentTimer();
        }
    }
}

void
Peer::startExecutionDelayedTimer(
    VirtualClock::duration d, std::function<void()> const& onSuccess,
    std::function<void(asio::error_code)> const& onFailure)
{
    releaseAssert(threadIsMain());
    mDelayedExecutionTimer.expires_from_now(d);
    mDelayedExecutionTimer.async_wait(onSuccess, onFailure);
}

Json::Value
Peer::getJsonInfo(bool compact) const
{
    releaseAssert(threadIsMain());
    Json::Value res;
    res["address"] = mAddress.toString();
    res["elapsed"] = (int)getLifeTime().count();
    res["latency"] = (int)getPing().count();
    res["ver"] = getRemoteVersion();
    res["olver"] = (int)getRemoteOverlayVersion();
    if (mFlowControl)
    {
        res["flow_control"] = mFlowControl->getFlowControlJsonInfo(compact);
    }
    if (!compact)
    {
        res["pull_mode"]["advert_delay"] = static_cast<Json::UInt64>(
            mPeerMetrics.mAdvertQueueDelay.GetSnapshot().get75thPercentile());
        res["pull_mode"]["pull_latency"] = static_cast<Json::UInt64>(
            mPeerMetrics.mPullLatency.GetSnapshot().get75thPercentile());
        res["pull_mode"]["demand_timeouts"] =
            static_cast<Json::UInt64>(mPeerMetrics.mDemandTimeouts);
        res["message_read"] =
            static_cast<Json::UInt64>(mPeerMetrics.mMessageRead);
        res["message_write"] =
            static_cast<Json::UInt64>(mPeerMetrics.mMessageWrite);
        res["byte_read"] = static_cast<Json::UInt64>(mPeerMetrics.mByteRead);
        res["byte_write"] = static_cast<Json::UInt64>(mPeerMetrics.mByteWrite);

        res["async_read"] = static_cast<Json::UInt64>(mPeerMetrics.mAsyncRead);
        res["async_write"] =
            static_cast<Json::UInt64>(mPeerMetrics.mAsyncWrite);

        res["message_drop"] =
            static_cast<Json::UInt64>(mPeerMetrics.mMessageDrop);

        res["message_delay_in_write_queue_p75"] = static_cast<Json::UInt64>(
            mPeerMetrics.mMessageDelayInWriteQueueTimer.GetSnapshot()
                .get75thPercentile());
        res["message_delay_in_async_write_p75"] = static_cast<Json::UInt64>(
            mPeerMetrics.mMessageDelayInAsyncWriteTimer.GetSnapshot()
                .get75thPercentile());

        res["unique_flood_message_recv"] =
            static_cast<Json::UInt64>(mPeerMetrics.mUniqueFloodMessageRecv);
        res["duplicate_flood_message_recv"] =
            static_cast<Json::UInt64>(mPeerMetrics.mDuplicateFloodMessageRecv);
        res["unique_fetch_message_recv"] =
            static_cast<Json::UInt64>(mPeerMetrics.mUniqueFetchMessageRecv);
        res["duplicate_fetch_message_recv"] =
            static_cast<Json::UInt64>(mPeerMetrics.mDuplicateFetchMessageRecv);
    }

    return res;
}

void
Peer::sendAuth()
{
    releaseAssert(threadIsMain());

    ZoneScoped;
    StellarMessage msg;
    msg.type(AUTH);
    msg.auth().flags = AUTH_MSG_FLAG_FLOW_CONTROL_BYTES_REQUESTED;
    auto msgPtr = std::make_shared<StellarMessage const>(msg);
    sendMessage(msgPtr);
}

std::string const&
Peer::toString()
{
    releaseAssert(threadIsMain());
    return mAddress.toString();
}

void
Peer::cancelTimers()
{
    releaseAssert(threadIsMain());
    mRecurringTimer.cancel();
    mDelayedExecutionTimer.cancel();
    if (mTxAdverts)
    {
        mTxAdverts->shutdown();
    }
}

void
Peer::clearBelow(uint32_t seq)
{
    releaseAssert(threadIsMain());
    if (mTxAdverts)
    {
        mTxAdverts->clearBelow(seq);
    }
}

void
Peer::connectHandler(asio::error_code const& error)
{
    RECURSIVE_LOCK_GUARD(mStateMutex, guard);
    if (error)
    {
        drop("unable to connect: " + error.message(),
             Peer::DropDirection::WE_DROPPED_REMOTE);
    }
    else
    {
        connected();
        setState(guard, CONNECTED);
        // Always send HELLO from main thread
        if (useBackgroundThread())
        {
            mAppConnector.postOnMainThread(
                [self = shared_from_this()]() { self->sendHello(); },
                "Peer::connectHandler sendHello");
        }
        else
        {
            sendHello();
        }
    }
}

void
Peer::maybeExecuteInBackground(std::string const& jobName,
                               std::function<void(std::shared_ptr<Peer>)> f)
{
    if (useBackgroundThread() && threadIsMain())
    {
        mAppConnector.postOnOverlayThread(
            [self = shared_from_this(), f]() { f(self); }, jobName);
    }
    else
    {
        // Execute the function directly if background processing is disabled or
        // we're already on the background thread.
        f(shared_from_this());
    }
}

void
Peer::sendDontHave(MessageType type, uint256 const& itemID)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    StellarMessage msg;
    msg.type(DONT_HAVE);
    msg.dontHave().reqHash = itemID;
    msg.dontHave().type = type;
    auto msgPtr = std::make_shared<StellarMessage const>(msg);
    sendMessage(msgPtr);
}

void
Peer::sendSCPQuorumSet(SCPQuorumSetPtr qSet)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    StellarMessage msg;
    msg.type(SCP_QUORUMSET);
    msg.qSet() = *qSet;
    auto msgPtr = std::make_shared<StellarMessage const>(msg);
    sendMessage(msgPtr);
}

void
Peer::sendGetTxSet(uint256 const& setID)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    StellarMessage newMsg;
    newMsg.type(GET_TX_SET);
    newMsg.txSetHash() = setID;

    auto msgPtr = std::make_shared<StellarMessage const>(newMsg);
    sendMessage(msgPtr);
}

void
Peer::sendGetQuorumSet(uint256 const& setID)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    StellarMessage newMsg;
    newMsg.type(GET_SCP_QUORUMSET);
    newMsg.qSetHash() = setID;

    auto msgPtr = std::make_shared<StellarMessage const>(newMsg);
    sendMessage(msgPtr);
}

void
Peer::sendGetScpState(uint32 ledgerSeq)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    StellarMessage newMsg;
    newMsg.type(GET_SCP_STATE);
    newMsg.getSCPLedgerSeq() = ledgerSeq;
    auto msgPtr = std::make_shared<StellarMessage const>(newMsg);
    sendMessage(msgPtr);
}

void
Peer::sendPeers()
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    StellarMessage newMsg;
    newMsg.type(PEERS);
    uint32 maxPeerCount = std::min<uint32>(50, newMsg.peers().max_size());

    // send top peers we know about
    auto peers =
        mAppConnector.getOverlayManager().getPeerManager().getPeersToSend(
            maxPeerCount, mAddress);
    releaseAssert(peers.size() <= maxPeerCount);

    if (!peers.empty())
    {
        newMsg.peers().reserve(peers.size());
        for (auto const& address : peers)
        {
            newMsg.peers().push_back(toXdr(address));
        }
        auto msgPtr = std::make_shared<StellarMessage const>(newMsg);
        sendMessage(msgPtr);
    }
}

void
Peer::sendError(ErrorCode error, std::string const& message)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    StellarMessage m;
    m.type(ERROR_MSG);
    m.error().code = error;
    m.error().msg = message;
    auto msgPtr = std::make_shared<StellarMessage const>(m);
    sendMessage(msgPtr);
}

void
Peer::sendErrorAndDrop(ErrorCode error, std::string const& message)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    sendError(error, message);
    drop(message, DropDirection::WE_DROPPED_REMOTE);
}

void
Peer::sendSendMore(uint32_t numMessages, uint32_t numBytes)
{
    ZoneScoped;

    auto m = std::make_shared<StellarMessage>();
    m->type(SEND_MORE_EXTENDED);
    m->sendMoreExtendedMessage().numMessages = numMessages;
    m->sendMoreExtendedMessage().numBytes = numBytes;
    sendMessage(m);
}

std::string
Peer::msgSummary(StellarMessage const& msg)
{
    releaseAssert(threadIsMain());

    switch (msg.type())
    {
    case ERROR_MSG:
        return "ERROR";
    case HELLO:
        return "HELLO";
    case AUTH:
        return "AUTH";
    case DONT_HAVE:
        return fmt::format(FMT_STRING("DONTHAVE {}:{}"), msg.dontHave().type,
                           hexAbbrev(msg.dontHave().reqHash));
    case PEERS:
        return fmt::format(FMT_STRING("PEERS {:d}"), msg.peers().size());

    case GET_TX_SET:
        return fmt::format(FMT_STRING("GETTXSET {}"),
                           hexAbbrev(msg.txSetHash()));
    case TX_SET:
    case GENERALIZED_TX_SET:
        return "TXSET";

    case TRANSACTION:
        return "TRANSACTION";

    case GET_SCP_QUORUMSET:
        return fmt::format(FMT_STRING("GET_SCP_QSET {}"),
                           hexAbbrev(msg.qSetHash()));
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
        return fmt::format(FMT_STRING("{} ({})"), t,
                           mAppConnector.getConfig().toShortString(
                               msg.envelope().statement.nodeID));
    }
    case GET_SCP_STATE:
        return fmt::format(FMT_STRING("GET_SCP_STATE {:d}"),
                           msg.getSCPLedgerSeq());

    case SURVEY_REQUEST:
    case SURVEY_RESPONSE:
    case TIME_SLICED_SURVEY_REQUEST:
    case TIME_SLICED_SURVEY_RESPONSE:
    case TIME_SLICED_SURVEY_START_COLLECTING:
    case TIME_SLICED_SURVEY_STOP_COLLECTING:
        return SurveyManager::getMsgSummary(msg);
    case SEND_MORE:
        return "SENDMORE";
    case SEND_MORE_EXTENDED:
        return "SENDMORE_EXTENDED";
    case FLOOD_ADVERT:
        return "FLODADVERT";
    case FLOOD_DEMAND:
        return "FLOODDEMAND";
    }
    return "UNKNOWN";
}

void
Peer::sendMessage(std::shared_ptr<StellarMessage const> msg, bool log)
{
    ZoneScoped;

    CLOG_TRACE(Overlay, "send: {} to : {}", msgSummary(*msg),
               mAppConnector.getConfig().toShortString(mPeerID));

    switch (msg->type())
    {
    case ERROR_MSG:
        mOverlayMetrics.mSendErrorMeter.Mark();
        break;
    case HELLO:
        mOverlayMetrics.mSendHelloMeter.Mark();
        break;
    case AUTH:
        mOverlayMetrics.mSendAuthMeter.Mark();
        break;
    case DONT_HAVE:
        mOverlayMetrics.mSendDontHaveMeter.Mark();
        break;
    case PEERS:
        mOverlayMetrics.mSendPeersMeter.Mark();
        break;
    case GET_TX_SET:
        mOverlayMetrics.mSendGetTxSetMeter.Mark();
        break;
    case TX_SET:
    case GENERALIZED_TX_SET:
        mOverlayMetrics.mSendTxSetMeter.Mark();
        break;
    case TRANSACTION:
        mOverlayMetrics.mSendTransactionMeter.Mark();
        break;
    case GET_SCP_QUORUMSET:
        mOverlayMetrics.mSendGetSCPQuorumSetMeter.Mark();
        break;
    case SCP_QUORUMSET:
        mOverlayMetrics.mSendSCPQuorumSetMeter.Mark();
        break;
    case SCP_MESSAGE:
        mOverlayMetrics.mSendSCPMessageSetMeter.Mark();
        break;
    case GET_SCP_STATE:
        mOverlayMetrics.mSendGetSCPStateMeter.Mark();
        break;
    case SURVEY_REQUEST:
    case TIME_SLICED_SURVEY_REQUEST:
        mOverlayMetrics.mSendSurveyRequestMeter.Mark();
        break;
    case SURVEY_RESPONSE:
    case TIME_SLICED_SURVEY_RESPONSE:
        mOverlayMetrics.mSendSurveyResponseMeter.Mark();
        break;
    case TIME_SLICED_SURVEY_START_COLLECTING:
        mOverlayMetrics.mSendStartSurveyCollectingMeter.Mark();
        break;
    case TIME_SLICED_SURVEY_STOP_COLLECTING:
        mOverlayMetrics.mSendStopSurveyCollectingMeter.Mark();
        break;
    case SEND_MORE:
    case SEND_MORE_EXTENDED:
        mOverlayMetrics.mSendSendMoreMeter.Mark();
        break;
    case FLOOD_ADVERT:
        mOverlayMetrics.mSendFloodAdvertMeter.Mark();
        break;
    case FLOOD_DEMAND:
        mOverlayMetrics.mSendFloodDemandMeter.Mark();
        break;
    };

    releaseAssert(mFlowControl);
    if (OverlayManager::isFloodMessage(*msg))
    {
        releaseAssert(threadIsMain());
        mFlowControl->addMsgAndMaybeTrimQueue(msg);
        maybeExecuteInBackground(
            "Peer::sendMessage maybeSendNextBatch",
            [](std::shared_ptr<Peer> self) {
                for (auto const& m : self->mFlowControl->getNextBatchToSend())
                {
                    self->sendAuthenticatedMessage(m.mMessage, m.mTimeEmplaced);
                }
            });
    }
    else
    {
        // Outgoing message is not flow-controlled, send it directly
        sendAuthenticatedMessage(msg);
    }
}

void
Peer::sendAuthenticatedMessage(
    std::shared_ptr<StellarMessage const> msg,
    std::optional<VirtualClock::time_point> timePlaced)
{
    {
        // No need to hold the lock for the duration of this function:
        // simply check if peer is shutting down, and if so, avoid putting
        // more work onto the queues. If peer shuts down _after_ we already
        // placed the message, any remaining messages will still go through
        // before we close the socket, so this should be harmless.
        RECURSIVE_LOCK_GUARD(mStateMutex, guard);
        if (shouldAbort(guard))
        {
            return;
        }
    }

    auto cb = [msg, timePlaced](std::shared_ptr<Peer> self) {
        // Construct an authenticated message and place it in the queue
        // _synchronously_ This is important because we assign auth sequence to
        // each message, which must be ordered
        AuthenticatedMessage amsg;
        self->mHmac.setAuthenticatedMessageBody(amsg, *msg);
        xdr::msg_ptr xdrBytes;
        {
            ZoneNamedN(xdrZone, "XDR serialize", true);
            xdrBytes = xdr::xdr_to_msg(amsg);
        }
        self->sendMessage(std::move(xdrBytes));
        if (timePlaced)
        {
            self->mFlowControl->updateMsgMetrics(msg, *timePlaced);
        }
    };

    // If we're already on the background thread (i.e. via flow control), move
    // msg to the queue right away
    maybeExecuteInBackground("sendAuthenticatedMessage", cb);
}

bool
Peer::isConnected(RecursiveLockGuard const& stateGuard) const
{
    return mState != CONNECTING && mState != CLOSING;
}

bool
Peer::isAuthenticated(RecursiveLockGuard const& stateGuard) const
{
    return mState == GOT_AUTH;
}

#ifdef BUILD_TESTS
bool
Peer::isAuthenticatedForTesting() const
{
    RECURSIVE_LOCK_GUARD(mStateMutex, guard);
    return isAuthenticated(guard);
}
bool
Peer::isConnectedForTesting() const
{
    RECURSIVE_LOCK_GUARD(mStateMutex, guard);
    return isConnected(guard);
}
bool
Peer::shouldAbortForTesting() const
{
    RECURSIVE_LOCK_GUARD(mStateMutex, guard);
    return shouldAbort(guard);
}
#endif

std::chrono::seconds
Peer::getLifeTime() const
{
    releaseAssert(threadIsMain());
    return std::chrono::duration_cast<std::chrono::seconds>(
        mAppConnector.now() - mCreationTime);
}

bool
Peer::shouldAbort(RecursiveLockGuard const& stateGuard) const
{
    return mState == CLOSING || mAppConnector.overlayShuttingDown();
}

bool
Peer::recvAuthenticatedMessage(AuthenticatedMessage&& msg)
{
    ZoneScoped;
    releaseAssert(!threadIsMain() || !useBackgroundThread());
    RECURSIVE_LOCK_GUARD(mStateMutex, guard);

    if (shouldAbort(guard))
    {
        return false;
    }

    std::string errorMsg;
    if (getState(guard) >= GOT_HELLO && msg.v0().message.type() != ERROR_MSG)
    {
        if (!mHmac.checkAuthenticatedMessage(msg, errorMsg))
        {
            if (!threadIsMain())
            {
                mAppConnector.postOnMainThread(
                    [self = shared_from_this(), errorMsg]() {
                        self->sendErrorAndDrop(ERR_AUTH, errorMsg);
                    },
                    "Peer::sendErrorAndDrop");
            }
            else
            {
                sendErrorAndDrop(ERR_AUTH, errorMsg);
            }
            return false;
        }
    }

    // NOTE: Additionally, we may use state snapshots to verify TRANSACTION type
    // messages in the background.

    // Start tracking capacity here, so read throttling is applied
    // appropriately. Flow control might not be started at that time
    auto msgTracker = std::make_shared<CapacityTrackedMessage>(
        shared_from_this(), msg.v0().message);

    std::string cat;
    Scheduler::ActionType type = Scheduler::ActionType::NORMAL_ACTION;

    switch (msgTracker->getMessage().type())
    {
    case HELLO:
    case AUTH:
        cat = AUTH_ACTION_QUEUE;
        break;
    // control messages
    case PEERS:
    case ERROR_MSG:
    case SEND_MORE:
    case SEND_MORE_EXTENDED:
        cat = "CTRL";
        break;
    // high volume flooding
    case TRANSACTION:
    case FLOOD_ADVERT:
    case FLOOD_DEMAND:
    {
        cat = "TX";
        type = Scheduler::ActionType::DROPPABLE_ACTION;
        break;
    }

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
    case GENERALIZED_TX_SET:
    case SCP_QUORUMSET:
    case SCP_MESSAGE:
        cat = "SCP";
        break;

    default:
        cat = "MISC";
    }

    // processing of incoming messages during authenticated must be in-order, so
    // while not authenticated, place all messages onto AUTH_ACTION_QUEUE
    // scheduler queue
    auto queueName = isAuthenticated(guard) ? cat : AUTH_ACTION_QUEUE;
    type = isAuthenticated(guard) ? type : Scheduler::ActionType::NORMAL_ACTION;

    // If a message is already scheduled, drop
    if (mAppConnector.checkScheduledAndCache(msgTracker))
    {
        return true;
    }

    // Verify SCP signatures when in the background
    if (useBackgroundThread() && msg.v0().message.type() == SCP_MESSAGE)
    {
        auto& envelope = msg.v0().message.envelope();
        PubKeyUtils::verifySig(envelope.statement.nodeID, envelope.signature,
                               xdr::xdr_to_opaque(mNetworkID, ENVELOPE_TYPE_SCP,
                                                  envelope.statement));
    }

    // Subtle: move `msgTracker` shared_ptr into the lambda, to ensure
    // its destructor is invoked from main thread only. Note that we can't use
    // unique_ptr here, because std::function requires its callable
    // to be copyable (C++23 fixes this with std::move_only_function, but we're
    // not there yet)
    mAppConnector.postOnMainThread(
        [self = shared_from_this(), t = std::move(msgTracker)]() {
            self->recvMessage(t);
        },
        std::move(queueName), type);

    // msgTracker should be null now
    releaseAssert(!msgTracker);
    return true;
}

void
Peer::recvMessage(std::shared_ptr<CapacityTrackedMessage> msgTracker)
{
    ZoneScoped;
    releaseAssert(threadIsMain());

    auto const& stellarMsg = msgTracker->getMessage();

    // No need to hold the lock for the whole duration of the function, just
    // need to check state for a potential early exit. If the peer gets dropped
    // after, we'd still process the message, but that's harmless.
    {
        RECURSIVE_LOCK_GUARD(mStateMutex, guard);
        if (shouldAbort(guard))
        {
            return;
        }
    }

    auto msgType = stellarMsg.type();
    bool ignoreIfOutOfSync = msgType == TRANSACTION ||
                             msgType == FLOOD_ADVERT || msgType == FLOOD_DEMAND;

    if (!mAppConnector.getLedgerManager().isSynced() && ignoreIfOutOfSync)
    {
        // For transactions, exit early during the state rebuild, as we
        // can't properly verify them
        return;
    }

    try
    {
        recvRawMessage(msgTracker);
    }
    catch (CryptoError const& e)
    {
        std::string err =
            fmt::format(FMT_STRING("Error RecvMessage T:{} msg:{} {} @{:d}"),
                        stellarMsg.type(), msgSummary(stellarMsg), toString(),
                        mAppConnector.getConfig().PEER_PORT);
        CLOG_ERROR(Overlay, "Dropping connection with {}: {}", err, e.what());
        drop("Bad crypto request", Peer::DropDirection::WE_DROPPED_REMOTE);
    }
}

void
Peer::recvSendMore(StellarMessage const& msg)
{
    releaseAssert(threadIsMain());
    releaseAssert(mFlowControl);
    mFlowControl->maybeReleaseCapacity(msg);
    maybeExecuteInBackground(
        "Peer::recvSendMore maybeSendNextBatch",
        [](std::shared_ptr<Peer> self) {
            for (auto const& m : self->mFlowControl->getNextBatchToSend())
            {
                self->sendAuthenticatedMessage(m.mMessage, m.mTimeEmplaced);
            }
        });
}

void
Peer::recvRawMessage(std::shared_ptr<CapacityTrackedMessage> msgTracker)
{
    ZoneScoped;
    releaseAssert(threadIsMain());

    auto const& stellarMsg = msgTracker->getMessage();
    auto peerStr = toString();
    ZoneText(peerStr.c_str(), peerStr.size());

    // No need to hold the lock for the whole duration of the function, just
    // need to check state for a potential early exit. If the peer gets dropped
    // after, we'd still process the message, but that's harmless.
    {
        RECURSIVE_LOCK_GUARD(mStateMutex, guard);
        if (shouldAbort(guard))
        {
            return;
        }

        if (!isAuthenticated(guard) && (stellarMsg.type() != HELLO) &&
            (stellarMsg.type() != AUTH) && (stellarMsg.type() != ERROR_MSG))
        {
            drop(fmt::format(
                     FMT_STRING("received {} before completed handshake"),
                     stellarMsg.type()),
                 Peer::DropDirection::WE_DROPPED_REMOTE);
            return;
        }

        if (stellarMsg.type() == PEERS && getRole() == REMOTE_CALLED_US)
        {
            drop(fmt::format("received {}", stellarMsg.type()),
                 Peer::DropDirection::WE_DROPPED_REMOTE);
            return;
        }

        releaseAssert(isAuthenticated(guard) || stellarMsg.type() == HELLO ||
                      stellarMsg.type() == AUTH ||
                      stellarMsg.type() == ERROR_MSG);
        mAppConnector.getOverlayManager().recordMessageMetric(
            stellarMsg, shared_from_this());
    }

    switch (stellarMsg.type())
    {
    case ERROR_MSG:
    {
        auto t = mOverlayMetrics.mRecvErrorTimer.TimeScope();
        recvError(stellarMsg);
    }
    break;

    case HELLO:
    {
        auto t = mOverlayMetrics.mRecvHelloTimer.TimeScope();
        this->recvHello(stellarMsg.hello());
    }
    break;

    case AUTH:
    {
        auto t = mOverlayMetrics.mRecvAuthTimer.TimeScope();
        this->recvAuth(stellarMsg);
    }
    break;

    case DONT_HAVE:
    {
        auto t = mOverlayMetrics.mRecvDontHaveTimer.TimeScope();
        recvDontHave(stellarMsg);
    }
    break;

    case PEERS:
    {
        auto t = mOverlayMetrics.mRecvPeersTimer.TimeScope();
        recvPeers(stellarMsg);
    }
    break;

    case SURVEY_REQUEST:
    case TIME_SLICED_SURVEY_REQUEST:
    {
        auto t = mOverlayMetrics.mRecvSurveyRequestTimer.TimeScope();
        recvSurveyRequestMessage(stellarMsg);
    }
    break;

    case SURVEY_RESPONSE:
    case TIME_SLICED_SURVEY_RESPONSE:
    {
        auto t = mOverlayMetrics.mRecvSurveyResponseTimer.TimeScope();
        recvSurveyResponseMessage(stellarMsg);
    }
    break;

    case TIME_SLICED_SURVEY_START_COLLECTING:
    {
        auto t = mOverlayMetrics.mRecvStartSurveyCollectingTimer.TimeScope();
        recvSurveyStartCollectingMessage(stellarMsg);
    }
    break;

    case TIME_SLICED_SURVEY_STOP_COLLECTING:
    {
        auto t = mOverlayMetrics.mRecvStopSurveyCollectingTimer.TimeScope();
        recvSurveyStopCollectingMessage(stellarMsg);
    }
    break;

    case GET_TX_SET:
    {
        auto t = mOverlayMetrics.mRecvGetTxSetTimer.TimeScope();
        recvGetTxSet(stellarMsg);
    }
    break;

    case TX_SET:
    {
        auto t = mOverlayMetrics.mRecvTxSetTimer.TimeScope();
        recvTxSet(stellarMsg);
    }
    break;

    case GENERALIZED_TX_SET:
    {
        auto t = mOverlayMetrics.mRecvTxSetTimer.TimeScope();
        recvGeneralizedTxSet(stellarMsg);
    }
    break;

    case TRANSACTION:
    {
        auto t = mOverlayMetrics.mRecvTransactionTimer.TimeScope();
        recvTransaction(*msgTracker);
    }
    break;

    case GET_SCP_QUORUMSET:
    {
        auto t = mOverlayMetrics.mRecvGetSCPQuorumSetTimer.TimeScope();
        recvGetSCPQuorumSet(stellarMsg);
    }
    break;

    case SCP_QUORUMSET:
    {
        auto t = mOverlayMetrics.mRecvSCPQuorumSetTimer.TimeScope();
        recvSCPQuorumSet(stellarMsg);
    }
    break;

    case SCP_MESSAGE:
    {
        auto t = mOverlayMetrics.mRecvSCPMessageTimer.TimeScope();
        recvSCPMessage(*msgTracker);
    }
    break;

    case GET_SCP_STATE:
    {
        auto t = mOverlayMetrics.mRecvGetSCPStateTimer.TimeScope();
        recvGetSCPState(stellarMsg);
    }
    break;
    case SEND_MORE:
    case SEND_MORE_EXTENDED:
    {
        std::string errorMsg;
        releaseAssert(mFlowControl);
        if (!mFlowControl->isSendMoreValid(stellarMsg, errorMsg))
        {
            drop(errorMsg, Peer::DropDirection::WE_DROPPED_REMOTE);
            return;
        }
        auto t = mOverlayMetrics.mRecvSendMoreTimer.TimeScope();
        recvSendMore(stellarMsg);
    }
    break;

    case FLOOD_ADVERT:
    {
        auto t = mOverlayMetrics.mRecvFloodAdvertTimer.TimeScope();
        recvFloodAdvert(stellarMsg);
    }
    break;

    case FLOOD_DEMAND:
    {
        auto t = mOverlayMetrics.mRecvFloodDemandTimer.TimeScope();
        recvFloodDemand(stellarMsg);
    }
    }
}

void
Peer::recvDontHave(StellarMessage const& msg)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    maybeProcessPingResponse(msg.dontHave().reqHash);

    mAppConnector.getHerder().peerDoesntHave(
        msg.dontHave().type, msg.dontHave().reqHash, shared_from_this());
}

bool
Peer::process(QueryInfo& queryInfo)
{
    auto const& cfg = mAppConnector.getConfig();
    std::chrono::seconds const QUERY_WINDOW =
        cfg.getExpectedLedgerCloseTime() * cfg.MAX_SLOTS_TO_REMEMBER;
    uint32_t const QUERIES_PER_WINDOW =
        QUERY_WINDOW.count() * QUERY_RESPONSE_MULTIPLIER;
    if (mAppConnector.now() - queryInfo.mLastTimeStamp >= QUERY_WINDOW)
    {
        queryInfo.mLastTimeStamp = mAppConnector.now();
        queryInfo.mNumQueries = 0;
    }
    return queryInfo.mNumQueries < QUERIES_PER_WINDOW;
}

void
Peer::recvGetTxSet(StellarMessage const& msg)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    if (!process(mTxSetQueryInfo))
    {
        return;
    }

    auto self = shared_from_this();
    if (auto txSet = mAppConnector.getHerder().getTxSet(msg.txSetHash()))
    {
        auto newMsg = std::make_shared<StellarMessage>();
        if (txSet->isGeneralizedTxSet())
        {
            newMsg->type(GENERALIZED_TX_SET);
            txSet->toXDR(newMsg->generalizedTxSet());
        }
        else
        {
            newMsg->type(TX_SET);
            txSet->toXDR(newMsg->txSet());
        }

        self->sendMessage(newMsg);
    }
    else
    {
        // Technically we don't exactly know what is the kind of the tx set
        // missing, however both TX_SET and GENERALIZED_TX_SET get the same
        // treatment when missing, so it should be ok to maybe send the
        // incorrect version during the upgrade.
        auto messageType =
            protocolVersionIsBefore(mAppConnector.getLedgerManager()
                                        .getLastClosedLedgerHeader()
                                        .header.ledgerVersion,
                                    SOROBAN_PROTOCOL_VERSION)
                ? TX_SET
                : GENERALIZED_TX_SET;
        sendDontHave(messageType, msg.txSetHash());
    }

    mTxSetQueryInfo.mNumQueries++;
}

void
Peer::recvTxSet(StellarMessage const& msg)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    auto frame = TxSetXDRFrame::makeFromWire(msg.txSet());
    mAppConnector.getHerder().recvTxSet(frame->getContentsHash(), frame);
}

void
Peer::recvGeneralizedTxSet(StellarMessage const& msg)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    auto frame = TxSetXDRFrame::makeFromWire(msg.generalizedTxSet());
    mAppConnector.getHerder().recvTxSet(frame->getContentsHash(), frame);
}

void
Peer::recvTransaction(CapacityTrackedMessage const& msg)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    releaseAssert(msg.maybeGetHash());
    mAppConnector.getOverlayManager().recvTransaction(
        msg.getMessage(), shared_from_this(), msg.maybeGetHash().value());
}

Hash
Peer::pingIDfromTimePoint(VirtualClock::time_point const& tp)
{
    releaseAssert(threadIsMain());
    auto sh = shortHash::xdrComputeHash(
        xdr::xdr_to_opaque(uint64_t(tp.time_since_epoch().count())));
    Hash res;
    releaseAssert(res.size() >= sizeof(sh));
    std::memcpy(res.data(), &sh, sizeof(sh));
    return res;
}

void
Peer::pingPeer()
{
    releaseAssert(threadIsMain());
    RECURSIVE_LOCK_GUARD(mStateMutex, guard);
    if (isAuthenticated(guard) && mPingSentTime == PING_NOT_SENT)
    {
        mPingSentTime = mAppConnector.now();
        auto h = pingIDfromTimePoint(mPingSentTime);
        sendGetQuorumSet(h);
    }
}

void
Peer::maybeProcessPingResponse(Hash const& id)
{
    releaseAssert(threadIsMain());
    if (mPingSentTime != PING_NOT_SENT)
    {
        auto h = pingIDfromTimePoint(mPingSentTime);
        if (h == id)
        {
            mLastPing = std::chrono::duration_cast<std::chrono::milliseconds>(
                mAppConnector.now() - mPingSentTime);
            mPingSentTime = PING_NOT_SENT;
            CLOG_DEBUG(Overlay, "Latency {}: {} ms", toString(),
                       mLastPing.count());
            mOverlayMetrics.mConnectionLatencyTimer.Update(mLastPing);
            mAppConnector.getOverlayManager().getSurveyManager().modifyPeerData(
                *this, [&](CollectingPeerData& peerData) {
                    peerData.mLatencyMsHistogram.Update(mLastPing.count());
                });
        }
    }
}

std::chrono::milliseconds
Peer::getPing() const
{
    releaseAssert(threadIsMain());
    return mLastPing;
}

bool
Peer::canRead() const
{
    releaseAssert(mFlowControl);
    return mFlowControl->canRead();
}

void
Peer::recvGetSCPQuorumSet(StellarMessage const& msg)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    if (!process(mQSetQueryInfo))
    {
        return;
    }

    SCPQuorumSetPtr qset = mAppConnector.getHerder().getQSet(msg.qSetHash());

    if (qset)
    {
        sendSCPQuorumSet(qset);
    }
    else
    {
        CLOG_TRACE(Overlay, "No quorum set: {}", hexAbbrev(msg.qSetHash()));
        sendDontHave(SCP_QUORUMSET, msg.qSetHash());
        // do we want to ask other people for it?
    }
    mQSetQueryInfo.mNumQueries++;
}
void
Peer::recvSCPQuorumSet(StellarMessage const& msg)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    Hash hash = xdrSha256(msg.qSet());
    maybeProcessPingResponse(hash);
    mAppConnector.getHerder().recvSCPQuorumSet(hash, msg.qSet());
}

void
Peer::recvSCPMessage(CapacityTrackedMessage const& msg)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    SCPEnvelope const& envelope = msg.getMessage().envelope();

    auto type = msg.getMessage().envelope().statement.pledges.type();
    auto t = (type == SCP_ST_PREPARE
                  ? mOverlayMetrics.mRecvSCPPrepareTimer.TimeScope()
                  : (type == SCP_ST_CONFIRM
                         ? mOverlayMetrics.mRecvSCPConfirmTimer.TimeScope()
                         : (type == SCP_ST_EXTERNALIZE
                                ? mOverlayMetrics.mRecvSCPExternalizeTimer
                                      .TimeScope()
                                : (mOverlayMetrics.mRecvSCPNominateTimer
                                       .TimeScope()))));
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
    releaseAssert(msg.maybeGetHash());
    mAppConnector.getOverlayManager().recvFloodedMsgID(
        msg.getMessage(), shared_from_this(), msg.maybeGetHash().value());

    auto res = mAppConnector.getHerder().recvSCPEnvelope(envelope);
    if (res == Herder::ENVELOPE_STATUS_DISCARDED)
    {
        // the message was discarded, remove it from the floodmap as well
        mAppConnector.getOverlayManager().forgetFloodedMsg(
            msg.maybeGetHash().value());
    }
}

void
Peer::recvGetSCPState(StellarMessage const& msg)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    uint32 seq = msg.getSCPLedgerSeq();
    mAppConnector.getHerder().sendSCPStateToPeer(seq, shared_from_this());
}

void
Peer::recvError(StellarMessage const& msg)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
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

    drop(fmt::format(FMT_STRING("{} ({})"), codeStr, msgStr),
         Peer::DropDirection::REMOTE_DROPPED_US);
}

void
Peer::updatePeerRecordAfterEcho()
{
    releaseAssert(threadIsMain());
    releaseAssert(!getAddress().isEmpty());

    PeerType type;
    if (mAppConnector.getOverlayManager().isPreferred(this))
    {
        type = PeerType::PREFERRED;
    }
    else if (mRole == WE_CALLED_REMOTE)
    {
        type = PeerType::OUTBOUND;
    }
    else
    {
        type = PeerType::INBOUND;
    }
    // Now that we've done authentication, we know whether this peer is
    // preferred or not
    mAppConnector.getOverlayManager().getPeerManager().update(
        getAddress(), type,
        /* preferredTypeKnown */ true);
}

void
Peer::updatePeerRecordAfterAuthentication()
{
    releaseAssert(threadIsMain());
    releaseAssert(!getAddress().isEmpty());

    if (mRole == WE_CALLED_REMOTE)
    {
        mAppConnector.getOverlayManager().getPeerManager().update(
            getAddress(), PeerManager::BackOffUpdate::RESET);
    }

    CLOG_DEBUG(Overlay, "successful handshake with {}@{}",
               mAppConnector.getConfig().toShortString(mPeerID), toString());
}

void
Peer::recvHello(Hello const& elo)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    RECURSIVE_LOCK_GUARD(mStateMutex, guard);

    if (getState(guard) >= GOT_HELLO)
    {
        drop("received unexpected HELLO",
             Peer::DropDirection::WE_DROPPED_REMOTE);
        return;
    }

    auto& peerAuth = mAppConnector.getOverlayManager().getPeerAuth();
    if (!peerAuth.verifyRemoteAuthCert(elo.peerID, elo.cert))
    {
        drop("failed to verify auth cert",
             Peer::DropDirection::WE_DROPPED_REMOTE);
        return;
    }

    if (mAppConnector.getBanManager().isBanned(elo.peerID))
    {
        drop("node is banned", Peer::DropDirection::WE_DROPPED_REMOTE);
        return;
    }

    mRemoteOverlayMinVersion = elo.overlayMinVersion;
    mRemoteOverlayVersion = elo.overlayVersion;
    mRemoteVersion = elo.versionStr;
    mPeerID = elo.peerID;
    mFlowControl->setPeerID(mPeerID);
    mRecvNonce = elo.nonce;
    mHmac.setSendMackey(peerAuth.getSendingMacKey(elo.cert.pubkey, mSendNonce,
                                                  mRecvNonce, mRole));
    mHmac.setRecvMackey(peerAuth.getReceivingMacKey(elo.cert.pubkey, mSendNonce,
                                                    mRecvNonce, mRole));

    setState(guard, GOT_HELLO);

    // mAddress is set in TCPPeer::initiate and TCPPeer::accept. It should
    // contain valid IP (but not necessarily port yet)
    auto ip = mAddress.getIP();
    if (ip.empty())
    {
        drop("failed to determine remote address",
             Peer::DropDirection::WE_DROPPED_REMOTE);
        return;
    }
    mAddress =
        PeerBareAddress{ip, static_cast<unsigned short>(elo.listeningPort)};

    CLOG_DEBUG(Overlay, "recvHello from {}", toString());

    if (mRole == REMOTE_CALLED_US)
    {
        // Send a HELLO back, even if it's going to be followed
        // immediately by ERROR, because ERROR is an authenticated
        // message type and the caller won't decode it right if
        // still waiting for an unauthenticated HELLO.
        sendHello();
    }

    if (mRemoteOverlayMinVersion > mRemoteOverlayVersion ||
        mRemoteOverlayVersion <
            mAppConnector.getConfig().OVERLAY_PROTOCOL_MIN_VERSION ||
        mRemoteOverlayMinVersion >
            mAppConnector.getConfig().OVERLAY_PROTOCOL_VERSION)
    {
        CLOG_DEBUG(Overlay, "Protocol = [{},{}] expected: [{},{}]",
                   mRemoteOverlayMinVersion, mRemoteOverlayVersion,
                   mAppConnector.getConfig().OVERLAY_PROTOCOL_MIN_VERSION,
                   mAppConnector.getConfig().OVERLAY_PROTOCOL_VERSION);
        sendErrorAndDrop(ERR_CONF, "wrong protocol version");
        return;
    }

    if (elo.peerID == mAppConnector.getConfig().NODE_SEED.getPublicKey())
    {
        sendErrorAndDrop(ERR_CONF, "connecting to self");
        return;
    }

    if (elo.networkID != mNetworkID)
    {
        CLOG_WARNING(Overlay, "Connection from peer with different NetworkID");
        CLOG_WARNING(Overlay, "Check your configuration file settings: "
                              "KNOWN_PEERS and PREFERRED_PEERS for peers "
                              "that are from other networks.");
        CLOG_DEBUG(Overlay, "NetworkID = {} expected: {}",
                   hexAbbrev(elo.networkID), hexAbbrev(mNetworkID));
        sendErrorAndDrop(ERR_CONF, "wrong network passphrase");
        return;
    }

    if (elo.listeningPort <= 0 || elo.listeningPort > UINT16_MAX || ip.empty())
    {
        sendErrorAndDrop(ERR_CONF, "bad address");
        return;
    }

    updatePeerRecordAfterEcho();

    auto const& authenticated =
        mAppConnector.getOverlayManager().getAuthenticatedPeers();
    auto authenticatedIt = authenticated.find(mPeerID);
    // no need to self-check here as this one cannot be in authenticated yet
    if (authenticatedIt != std::end(authenticated))
    {
        if (&(authenticatedIt->second->mPeerID) != &mPeerID)
        {
            sendErrorAndDrop(
                ERR_CONF, "already-connected peer: " +
                              mAppConnector.getConfig().toShortString(mPeerID));
            return;
        }
    }

    for (auto const& p : mAppConnector.getOverlayManager().getPendingPeers())
    {
        if (&(p->mPeerID) == &mPeerID)
        {
            continue;
        }
        if (p->getPeerID() == mPeerID)
        {
            sendErrorAndDrop(
                ERR_CONF, "already-connected peer: " +
                              mAppConnector.getConfig().toShortString(mPeerID));
            return;
        }
    }

    if (mRole == WE_CALLED_REMOTE)
    {
        sendAuth();
    }
}

void
Peer::setState(RecursiveLockGuard const& stateGuard, PeerState newState)
{
    mState = newState;
}

void
Peer::recvAuth(StellarMessage const& msg)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    RECURSIVE_LOCK_GUARD(mStateMutex, guard);

    if (getState(guard) != GOT_HELLO)
    {
        sendErrorAndDrop(ERR_MISC, "out-of-order AUTH message");
        return;
    }

    if (isAuthenticated(guard))
    {
        sendErrorAndDrop(ERR_MISC, "out-of-order AUTH message");
        return;
    }

    setState(guard, GOT_AUTH);

    if (mRole == REMOTE_CALLED_US)
    {
        sendAuth();
        sendPeers();
    }

    updatePeerRecordAfterAuthentication();

    auto self = shared_from_this();
    if (!mAppConnector.getOverlayManager().acceptAuthenticatedPeer(self))
    {
        sendErrorAndDrop(ERR_LOAD, "peer rejected");
        return;
    }

    if (msg.auth().flags != AUTH_MSG_FLAG_FLOW_CONTROL_BYTES_REQUESTED)
    {
        sendErrorAndDrop(ERR_CONF, "flow control bytes disabled");
        return;
    }

    uint32_t fcBytes =
        mAppConnector.getOverlayManager().getFlowControlBytesTotal();

    // Subtle: after successful auth, must send sendMore message first to
    // tell the other peer about the local node's reading capacity.
    sendSendMore(mAppConnector.getConfig().PEER_FLOOD_READING_CAPACITY,
                 fcBytes);

    auto weakSelf = std::weak_ptr<Peer>(shared_from_this());
    mTxAdverts->start([weakSelf](std::shared_ptr<StellarMessage const> msg) {
        auto self = weakSelf.lock();
        if (self)
        {
            self->sendMessage(msg);
        }
    });

    // Ask for SCP data _after_ the flow control message
    auto low = mAppConnector.getHerder().getMinLedgerSeqToAskPeers();
    sendGetScpState(low);
}

void
Peer::recvPeers(StellarMessage const& msg)
{
    ZoneScoped;
    releaseAssert(threadIsMain());

    if (mPeersReceived)
    {
        drop(fmt::format("too many msgs {}", msg.type()),
             Peer::DropDirection::WE_DROPPED_REMOTE);
        return;
    }
    mPeersReceived = true;

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

        releaseAssert(peer.ip.type() == IPv4);
        auto address = PeerBareAddress{peer};

        if (address.isPrivate())
        {
            CLOG_DEBUG(Overlay, "ignoring received private address {}",
                       address.toString());
        }
        else if (address ==
                 PeerBareAddress{getAddress().getIP(),
                                 mAppConnector.getConfig().PEER_PORT})
        {
            CLOG_DEBUG(Overlay, "ignoring received self-address {}",
                       address.toString());
        }
        else if (address.isLocalhost() &&
                 !mAppConnector.getConfig().ALLOW_LOCALHOST_FOR_TESTING)
        {
            CLOG_DEBUG(Overlay, "ignoring received localhost");
        }
        else
        {
            // don't use peer.numFailures here as we may have better luck
            // (and we don't want to poison our failure count)
            mAppConnector.getOverlayManager().getPeerManager().ensureExists(
                address);
        }
    }
}

void
Peer::recvSurveyRequestMessage(StellarMessage const& msg)
{
    ZoneScoped;
    releaseAssert(threadIsMain());

    mAppConnector.getOverlayManager().getSurveyManager().relayOrProcessRequest(
        msg, shared_from_this());
}

void
Peer::recvSurveyResponseMessage(StellarMessage const& msg)
{
    ZoneScoped;
    releaseAssert(threadIsMain());

    mAppConnector.getOverlayManager().getSurveyManager().relayOrProcessResponse(
        msg, shared_from_this());
}

void
Peer::recvSurveyStartCollectingMessage(StellarMessage const& msg)
{
    ZoneScoped;
    mAppConnector.getOverlayManager()
        .getSurveyManager()
        .relayStartSurveyCollecting(msg, shared_from_this());
}

void
Peer::recvSurveyStopCollectingMessage(StellarMessage const& msg)
{
    ZoneScoped;
    mAppConnector.getOverlayManager()
        .getSurveyManager()
        .relayStopSurveyCollecting(msg, shared_from_this());
}

void
Peer::recvFloodAdvert(StellarMessage const& msg)
{
    releaseAssert(threadIsMain());
    releaseAssert(mTxAdverts);
    auto seq = mAppConnector.getHerder().trackingConsensusLedgerIndex();
    mTxAdverts->queueIncomingAdvert(msg.floodAdvert().txHashes, seq);
}

void
Peer::recvFloodDemand(StellarMessage const& msg)
{
    releaseAssert(threadIsMain());
    // Pass the demand to OverlayManager for processing
    mAppConnector.getOverlayManager().recvTxDemand(msg.floodDemand(),
                                                   shared_from_this());
}

Peer::PeerMetrics::PeerMetrics(VirtualClock::time_point connectedTime)
    : mMessageRead(0)
    , mMessageWrite(0)
    , mByteRead(0)
    , mByteWrite(0)
    , mAsyncRead(0)
    , mAsyncWrite(0)
    , mMessageDrop(0)
    , mMessageDelayInWriteQueueTimer(medida::Timer(PEER_METRICS_DURATION_UNIT,
                                                   PEER_METRICS_RATE_UNIT,
                                                   PEER_METRICS_WINDOW_SIZE))
    , mMessageDelayInAsyncWriteTimer(medida::Timer(PEER_METRICS_DURATION_UNIT,
                                                   PEER_METRICS_RATE_UNIT,
                                                   PEER_METRICS_WINDOW_SIZE))
    , mAdvertQueueDelay(medida::Timer(PEER_METRICS_DURATION_UNIT,
                                      PEER_METRICS_RATE_UNIT,
                                      PEER_METRICS_WINDOW_SIZE))
    , mPullLatency(medida::Timer(PEER_METRICS_DURATION_UNIT,
                                 PEER_METRICS_RATE_UNIT,
                                 PEER_METRICS_WINDOW_SIZE))
    , mDemandTimeouts(0)
    , mUniqueFloodBytesRecv(0)
    , mDuplicateFloodBytesRecv(0)
    , mUniqueFetchBytesRecv(0)
    , mDuplicateFetchBytesRecv(0)
    , mUniqueFloodMessageRecv(0)
    , mDuplicateFloodMessageRecv(0)
    , mUniqueFetchMessageRecv(0)
    , mDuplicateFetchMessageRecv(0)
    , mTxHashReceived(0)
    , mConnectedTime(connectedTime)
    , mMessagesFulfilled(0)
    , mBannedMessageUnfulfilled(0)
    , mUnknownMessageUnfulfilled(0)
{
}

void
Peer::sendTxDemand(TxDemandVector&& demands)
{
    releaseAssert(threadIsMain());
    if (demands.size() > 0)
    {
        auto msg = std::make_shared<StellarMessage>();
        msg->type(FLOOD_DEMAND);
        msg->floodDemand().txHashes = std::move(demands);
        mOverlayMetrics.mMessagesDemanded.Mark(
            msg->floodDemand().txHashes.size());
        mAppConnector.postOnMainThread(
            [self = shared_from_this(), msg = std::move(msg)]() {
                self->sendMessage(msg);
            },
            "sendTxDemand");
        ++mPeerMetrics.mTxDemandSent;
    }
}

void
Peer::handleMaxTxSizeIncrease(uint32_t increase)
{
    releaseAssert(threadIsMain());
    if (increase > 0)
    {
        mFlowControl->handleTxSizeIncrease(increase);
        // Send an additional SEND_MORE to let the other peer know we have more
        // capacity available (and possibly unblock it)
        sendSendMore(0, increase);
    }
}

bool
Peer::sendAdvert(Hash const& hash)
{
    releaseAssert(threadIsMain());
    if (!mTxAdverts)
    {
        throw std::runtime_error("Pull mode is not set");
    }

    // No-op if peer already knows about the hash
    if (mTxAdverts->seenAdvert(hash))
    {
        return false;
    }

    // Otherwise, queue up an advert to broadcast to peer
    mTxAdverts->queueOutgoingAdvert(hash);
    return true;
}

void
Peer::retryAdvert(std::list<Hash>& hashes)
{
    releaseAssert(threadIsMain());
    if (!mTxAdverts)
    {
        throw std::runtime_error("Pull mode is not set");
    }
    mTxAdverts->retryIncomingAdvert(hashes);
}

bool
Peer::hasAdvert()
{
    releaseAssert(threadIsMain());
    if (!mTxAdverts)
    {
        throw std::runtime_error("Pull mode is not set");
    }
    return mTxAdverts->size() > 0;
}

std::pair<Hash, std::optional<VirtualClock::time_point>>
Peer::popAdvert()
{
    releaseAssert(threadIsMain());
    if (!mTxAdverts)
    {
        throw std::runtime_error("Pull mode is not set");
    }

    return mTxAdverts->popIncomingAdvert();
}

}
