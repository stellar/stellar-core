// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/test/LoopbackPeer.h"
#include "crypto/Random.h"
#include "main/Application.h"
#include "medida/meter.h"
#include "medida/timer.h"
#include "overlay/OverlayManager.h"
#include "overlay/OverlayMetrics.h"
#include "overlay/StellarXDR.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "xdrpp/marshal.h"
#include <Tracy.hpp>

namespace stellar
{

using namespace std;

///////////////////////////////////////////////////////////////////////
// LoopbackPeer
///////////////////////////////////////////////////////////////////////

LoopbackPeer::LoopbackPeer(Application& app, PeerRole role) : Peer(app, role)
{
    mFlowControl =
        std::make_shared<FlowControl>(mAppConnector, useBackgroundThread());
}

std::string
LoopbackPeer::getIP() const
{
    return "127.0.0.1";
}

std::pair<std::shared_ptr<LoopbackPeer>, std::shared_ptr<LoopbackPeer>>
LoopbackPeer::initiate(Application& app, Application& otherApp)
{
    auto peer = make_shared<LoopbackPeer>(app, Peer::WE_CALLED_REMOTE);
    auto otherPeer =
        make_shared<LoopbackPeer>(otherApp, Peer::REMOTE_CALLED_US);

    peer->mRemote = otherPeer;
    peer->mState = Peer::CONNECTED;

    otherPeer->mRemote = peer;
    otherPeer->mState = Peer::CONNECTED;

    peer->mAddress =
        PeerBareAddress(otherPeer->getIP(), otherPeer->getConfig().PEER_PORT);
    otherPeer->mAddress =
        PeerBareAddress{peer->getIP(), peer->getConfig().PEER_PORT};

    app.getOverlayManager().addOutboundConnection(peer);
    otherApp.getOverlayManager().maybeAddInboundConnection(otherPeer);
    // if connection was dropped during addPendingPeer, we don't want do call
    // connectHandler
    if (peer->mState != Peer::CONNECTED || otherPeer->mState != Peer::CONNECTED)
    {
        return std::pair(peer, otherPeer);
    }

    peer->startRecurrentTimer();
    otherPeer->startRecurrentTimer();

    std::weak_ptr<LoopbackPeer> init = peer;
    peer->mAppConnector.postOnMainThread(
        [init]() {
            auto inC = init.lock();
            if (inC)
            {
                inC->connectHandler(asio::error_code());
            }
        },
        "LoopbackPeer: connect");
    return std::pair(peer, otherPeer);
}

AuthCert
LoopbackPeer::getAuthCert()
{
    auto c = Peer::getAuthCert();
    if (mDamageCert)
    {
        c.expiration++;
    }
    return c;
}

void
LoopbackPeer::scheduleRead()
{
    processInQueue();
}

void
LoopbackPeer::sendMessage(xdr::msg_ptr&& msg)
{
    if (mRemote.expired())
    {
        drop("remote expired", Peer::DropDirection::WE_DROPPED_REMOTE);
        return;
    }

    // Damage authentication material.
    if (mDamageAuth)
    {
        mHmac.damageRecvMacKey();
    }

    TimestampedMessage tsm;
    tsm.mMessage = std::move(msg);
    tsm.mEnqueuedTime = mAppConnector.now();
    mOutQueue.emplace_back(std::move(tsm));
    // Possibly flush some queued messages if queue's full.
    while (mOutQueue.size() > mMaxQueueDepth && !mCorked)
    {
        // If our recipient is straggling, we will break off sending 75% of the
        // time even when we have more things to send, causing the outbound
        // queue to back up gradually.
        auto remote = mRemote.lock();
        if (remote && remote->getStraggling())
        {
            if (rand_flip() || rand_flip())
            {
                CLOG_DEBUG(
                    Overlay,
                    "Loopback send-to-straggler pausing, outbound queue at {}",
                    mOutQueue.size());
                break;
            }
            else
            {
                CLOG_DEBUG(
                    Overlay,
                    "Loopback send-to-straggler sending, outbound queue at {}",
                    mOutQueue.size());
            }
        }
        deliverOne();
    }
}

void
LoopbackPeer::drop(std::string const& reason, DropDirection direction)
{
    if (mState == CLOSING)
    {
        return;
    }

    if (mState != GOT_AUTH)
    {
        CLOG_DEBUG(Overlay, "LoopbackPeer::drop {} in state {} we called:{}",
                   toString(), format_as(mState), format_as(mRole));
    }
    else if (direction == Peer::DropDirection::WE_DROPPED_REMOTE)
    {
        CLOG_DEBUG(Overlay, "Dropping peer {}, reason {}", toString(), reason);
    }
    else
    {
        CLOG_DEBUG(Overlay, "peer {} dropped us, reason {}", toString(),
                   reason);
    }

    mDropReason = reason;
    mState = CLOSING;
    cancelTimers();
    mAppConnector.getOverlayManager().removePeer(this);

    auto remote = mRemote.lock();
    if (remote)
    {
        remote->mAppConnector.postOnMainThread(
            [remW = mRemote, reason, direction]() {
                auto remS = remW.lock();
                if (remS)
                {
                    remS->drop(reason,
                               direction ==
                                       Peer::DropDirection::WE_DROPPED_REMOTE
                                   ? Peer::DropDirection::REMOTE_DROPPED_US
                                   : Peer::DropDirection::WE_DROPPED_REMOTE);
                }
            },
            "LoopbackPeer: drop");
    }
}

static bool
damageMessage(stellar_default_random_engine& gen, xdr::msg_ptr& msg)
{
    size_t bitsFlipped = 0;
    char* d = msg->raw_data();
    char* e = msg->end();
    size_t sz = e - d;
    if (sz > 0)
    {
        auto dist = uniform_int_distribution<size_t>(0, sz - 1);
        auto byteDist = uniform_int_distribution<int>(0, 7);
        size_t nDamage = dist(gen);
        while (nDamage != 0)
        {
            --nDamage;
            auto pos = dist(gen);
            d[pos] ^= 1 << byteDist(gen);
            bitsFlipped++;
        }
    }
    return bitsFlipped != 0;
}

static Peer::TimestampedMessage
duplicateMessage(Peer::TimestampedMessage const& msg)
{
    xdr::msg_ptr m2 = xdr::message_t::alloc(msg.mMessage->size());
    memcpy(m2->raw_data(), msg.mMessage->raw_data(), msg.mMessage->raw_size());
    Peer::TimestampedMessage msg2;
    msg2.mEnqueuedTime = msg.mEnqueuedTime;
    msg2.mMessage = std::move(m2);
    return msg2;
}

void
LoopbackPeer::processInQueue()
{
    if (mFlowControl->maybeThrottleRead())
    {
        return;
    }

    if (!mInQueue.empty() && mState != CLOSING)
    {
        auto const& m = mInQueue.front();
        receivedBytes(m->size(), true);
        recvMessage(m);
        mInQueue.pop();

        if (!mInQueue.empty())
        {
            auto self = static_pointer_cast<LoopbackPeer>(shared_from_this());
            mAppConnector.postOnMainThread([self]() { self->processInQueue(); },
                                           "LoopbackPeer: processInQueue");
        }
    }
}

void
LoopbackPeer::recvMessage(xdr::msg_ptr const& msg)
{
    ZoneScoped;
    if (shouldAbortForTesting())
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
        recvAuthenticatedMessage(std::move(am));
    }
    catch (xdr::xdr_runtime_error& e)
    {
        CLOG_ERROR(Overlay, "received corrupt xdr::msg_ptr {}", e.what());
        drop("received corrupted message",
             Peer::DropDirection::WE_DROPPED_REMOTE);
        return;
    }
}

void
LoopbackPeer::recvMessage(std::shared_ptr<CapacityTrackedMessage> msgTracker)
{
    mAppConnector.postOnMainThread(
        [self = shared_from_this(), msgTracker]() {
            self->recvMessage(msgTracker);
        },
        "LoopbackPeer: processInQueue");
}

void
LoopbackPeer::deliverOne()
{
    if (mRemote.expired())
    {
        return;
    }

    if (!mOutQueue.empty() && !mCorked)
    {
        TimestampedMessage msg = std::move(mOutQueue.front());
        mOutQueue.pop_front();

        // Possibly duplicate the message and requeue it at the front.
        if (mDuplicateProb(gRandomEngine))
        {
            CLOG_INFO(Overlay, "LoopbackPeer duplicated message");
            mOutQueue.emplace_front(duplicateMessage(msg));
            mStats.messagesDuplicated++;
        }

        // Possibly requeue it at the back and return, reordering.
        if (mReorderProb(gRandomEngine) && mOutQueue.size() > 0)
        {
            CLOG_INFO(Overlay, "LoopbackPeer reordered message");
            mStats.messagesReordered++;
            mOutQueue.emplace_back(std::move(msg));
            return;
        }

        // Possibly flip some bits in the message.
        if (mDamageProb(gRandomEngine))
        {
            CLOG_INFO(Overlay, "LoopbackPeer damaged message");
            if (damageMessage(gRandomEngine, msg.mMessage))
                mStats.messagesDamaged++;
        }

        // Possibly just drop the message on the floor.
        if (mDropProb(gRandomEngine))
        {
            CLOG_INFO(Overlay, "LoopbackPeer dropped message");
            mStats.messagesDropped++;
            return;
        }

        size_t nBytes = msg.mMessage->raw_size();
        mStats.bytesDelivered += nBytes;

        mEnqueueTimeOfLastWrite = msg.mEnqueuedTime;

        // Pass ownership of a serialized XDR message buffer to a recvMesage
        // callback event against the remote Peer, posted on the remote
        // Peer's io_context.
        auto remote = mRemote.lock();
        if (remote)
        {
            // move msg to remote's in queue
            remote->mInQueue.emplace(std::move(msg.mMessage));
            remote->mAppConnector.postOnMainThread(
                [remW = mRemote]() {
                    auto remS = remW.lock();
                    if (remS)
                    {
                        remS->processInQueue();
                    }
                },
                "LoopbackPeer: processInQueue in deliverOne");
        }
        mLastWrite = mAppConnector.now();
        mOverlayMetrics.mMessageWrite.Mark();
        mOverlayMetrics.mByteWrite.Mark(nBytes);
        ++mPeerMetrics.mMessageWrite;
        mPeerMetrics.mByteWrite += nBytes;
    }
}

void
LoopbackPeer::deliverAll()
{
    while (!mOutQueue.empty() && !mCorked)
    {
        deliverOne();
    }
}

void
LoopbackPeer::dropAll()
{
    mOutQueue.clear();
}

size_t
LoopbackPeer::getBytesQueued() const
{
    size_t t = 0;
    for (auto const& m : mOutQueue)
    {
        t += m.mMessage->raw_size();
    }
    return t;
}

size_t
LoopbackPeer::getMessagesQueued() const
{
    return mOutQueue.size();
}

LoopbackPeer::Stats const&
LoopbackPeer::getStats() const
{
    return mStats;
}

bool
LoopbackPeer::getCorked() const
{
    return mCorked;
}

void
LoopbackPeer::setCorked(bool c)
{
    mCorked = c;
}

void
LoopbackPeer::clearInAndOutQueues()
{
    mOutQueue.clear();
    mInQueue = std::queue<xdr::msg_ptr>();
}

bool
LoopbackPeer::getStraggling() const
{
    return mStraggling;
}

void
LoopbackPeer::setStraggling(bool s)
{
    mStraggling = s;
}

size_t
LoopbackPeer::getMaxQueueDepth() const
{
    return mMaxQueueDepth;
}

void
LoopbackPeer::setMaxQueueDepth(size_t sz)
{
    mMaxQueueDepth = sz;
}

double
LoopbackPeer::getDamageProbability() const
{
    return mDamageProb.p();
}

static void
checkProbRange(double d)
{
    if (d < 0.0 || d > 1.0)
    {
        throw std::runtime_error("probability out of range");
    }
}

void
LoopbackPeer::setDamageProbability(double d)
{
    checkProbRange(d);
    mDamageProb = bernoulli_distribution(d);
}

double
LoopbackPeer::getDropProbability() const
{
    return mDropProb.p();
}

void
LoopbackPeer::setDamageCert(bool b)
{
    mDamageCert = b;
}

bool
LoopbackPeer::getDamageCert() const
{
    return mDamageCert;
}

void
LoopbackPeer::setDamageAuth(bool b)
{
    mDamageAuth = b;
}

bool
LoopbackPeer::getDamageAuth() const
{
    return mDamageAuth;
}

void
LoopbackPeer::setDropProbability(double d)
{
    checkProbRange(d);
    mDropProb = bernoulli_distribution(d);
}

double
LoopbackPeer::getDuplicateProbability() const
{
    return mDuplicateProb.p();
}

void
LoopbackPeer::setDuplicateProbability(double d)
{
    checkProbRange(d);
    mDuplicateProb = bernoulli_distribution(d);
}

double
LoopbackPeer::getReorderProbability() const
{
    return mReorderProb.p();
}

void
LoopbackPeer::setReorderProbability(double d)
{
    checkProbRange(d);
    mReorderProb = bernoulli_distribution(d);
}

LoopbackPeerConnection::LoopbackPeerConnection(Application& initiator,
                                               Application& acceptor)
{
    auto res = LoopbackPeer::initiate(initiator, acceptor);
    mInitiator = res.first;
    mAcceptor = res.second;
}

LoopbackPeerConnection::~LoopbackPeerConnection()
{
    // NB: Dropping the peer from one side will automatically drop the
    // other.
    mInitiator->drop("loopback destruction",
                     Peer::DropDirection::WE_DROPPED_REMOTE);
}

std::shared_ptr<LoopbackPeer>
LoopbackPeerConnection::getInitiator() const
{
    return mInitiator;
}

std::shared_ptr<LoopbackPeer>
LoopbackPeerConnection::getAcceptor() const
{
    return mAcceptor;
}

bool
LoopbackPeer::checkCapacity(std::shared_ptr<LoopbackPeer> otherPeer) const
{
    // Outbound capacity is equal to the config on the other node
    return otherPeer->getConfig().PEER_FLOOD_READING_CAPACITY ==
               getFlowControl()->getCapacity().getOutboundCapacity() &&
           otherPeer->mAppConnector.getOverlayManager()
                   .getFlowControlBytesTotal() ==
               getFlowControl()->getCapacityBytes().getOutboundCapacity();
}
}
