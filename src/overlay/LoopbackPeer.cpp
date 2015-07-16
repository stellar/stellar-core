// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/LoopbackPeer.h"
#include "util/Logging.h"
#include "main/Application.h"
#include "overlay/StellarXDR.h"
#include "xdrpp/marshal.h"
#include "overlay/OverlayManager.h"

namespace stellar
{

using namespace std;

///////////////////////////////////////////////////////////////////////
// LoopbackPeer
///////////////////////////////////////////////////////////////////////

LoopbackPeer::LoopbackPeer(Application& app, PeerRole role)
    : Peer(app, role), mRemote(nullptr)
{
}

void
LoopbackPeer::sendMessage(xdr::msg_ptr&& msg)
{
    // CLOG(TRACE, "Overlay") << "LoopbackPeer queueing message";
    mQueue.emplace_back(std::move(msg));
    // Possibly flush some queued messages if queue's full.
    while (mQueue.size() > mMaxQueueDepth && !mCorked)
    {
        deliverOne();
    }
}

std::string
LoopbackPeer::getIP()
{
    return "127.0.0.1";
}

void
LoopbackPeer::drop()
{
    if (mState == CLOSING)
    {
        return;
    }
    mState = CLOSING;
    auto self = shared_from_this();
    mApp.getClock().getIOService().post(
        [self]()
        {
            self->getApp().getOverlayManager().dropPeer(self);
        });
    if (mRemote)
    {
        auto remote = mRemote;
        mRemote->getApp().getClock().getIOService().post(
            [remote]()
            {
                remote->getApp().getOverlayManager().dropPeer(remote);
                remote->mRemote = nullptr;
            });
        mRemote = nullptr;
    }
}

bool
LoopbackPeer::recvHello(StellarMessage const& msg)
{
    if (!Peer::recvHello(msg))
        return false;

    if (mRole == INITIATOR)
    { // this guy called us
        sendHello();
    }
    return true;
}

static bool
damageMessage(default_random_engine& gen, xdr::msg_ptr& msg)
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

static xdr::msg_ptr
duplicateMessage(xdr::msg_ptr const& msg)
{
    xdr::msg_ptr msg2 = xdr::message_t::alloc(msg->size());
    memcpy(msg2->raw_data(), msg->raw_data(), msg->raw_size());
    return std::move(msg2);
}

void
LoopbackPeer::deliverOne()
{
    // CLOG(TRACE, "Overlay") << "LoopbackPeer attempting to deliver message";
    if (!mRemote)
    {
        throw std::runtime_error("LoopbackPeer missing target");
    }

    if (!mQueue.empty() && !mCorked)
    {
        xdr::msg_ptr msg = std::move(mQueue.front());
        mQueue.pop_front();

        // CLOG(TRACE, "Overlay") << "LoopbackPeer dequeued message";

        // Possibly duplicate the message and requeue it at the front.
        if (mDuplicateProb(mGenerator))
        {
            CLOG(INFO, "Overlay") << "LoopbackPeer duplicated message";
            mQueue.emplace_front(std::move(duplicateMessage(msg)));
            mStats.messagesDuplicated++;
        }

        // Possibly requeue it at the back and return, reordering.
        if (mReorderProb(mGenerator) && mQueue.size() > 0)
        {
            CLOG(INFO, "Overlay") << "LoopbackPeer reordered message";
            mStats.messagesReordered++;
            mQueue.emplace_back(std::move(msg));
            return;
        }

        // Possibly flip some bits in the message.
        if (mDamageProb(mGenerator))
        {
            CLOG(INFO, "Overlay") << "LoopbackPeer damaged message";
            if (damageMessage(mGenerator, msg))
                mStats.messagesDamaged++;
        }

        // Possibly just drop the message on the floor.
        if (mDropProb(mGenerator))
        {
            CLOG(INFO, "Overlay") << "LoopbackPeer dropped message";
            mStats.messagesDropped++;
            return;
        }

        mStats.bytesDelivered += msg->raw_size();

        // Pass ownership of a serialized XDR message buffer to a recvMesage
        // callback event against the remote Peer, posted on the remote
        // Peer's io_service.
        auto remote = mRemote;
        auto m = std::make_shared<xdr::msg_ptr>(std::move(msg));
        remote->getApp().getClock().getIOService().post(
            [remote, m]()
            {
                remote->recvMessage(std::move(*m));
            });

        // CLOG(TRACE, "Overlay") << "LoopbackPeer posted message to remote";
    }
}

void
LoopbackPeer::deliverAll()
{
    while (!mQueue.empty() && !mCorked)
    {
        deliverOne();
    }
}

void
LoopbackPeer::dropAll()
{
    mQueue.clear();
}

size_t
LoopbackPeer::getBytesQueued() const
{
    size_t t = 0;
    for (auto const& m : mQueue)
    {
        t += m->raw_size();
    }
    return t;
}

size_t
LoopbackPeer::getMessagesQueued() const
{
    return mQueue.size();
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
    : mInitiator(make_shared<LoopbackPeer>(initiator, Peer::INITIATOR))
    , mAcceptor(make_shared<LoopbackPeer>(acceptor, Peer::ACCEPTOR))
{
    mInitiator->mRemote = mAcceptor;
    mInitiator->mState = Peer::CONNECTED;

    mAcceptor->mRemote = mInitiator;
    mAcceptor->mState = Peer::CONNECTED;

    initiator.getOverlayManager().addConnectedPeer(mInitiator);
    acceptor.getOverlayManager().addConnectedPeer(mAcceptor);

    mAcceptor->connectHandler(asio::error_code());
}

LoopbackPeerConnection::~LoopbackPeerConnection()
{
    // NB: Dropping the peer from one side will automatically drop the
    // other.
    mInitiator->drop();
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
}
