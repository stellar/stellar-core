// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/Floodgate.h"
#include "crypto/BLAKE2.h"
#include "crypto/Hex.h"
#include "herder/Herder.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "medida/counter.h"
#include "medida/metrics_registry.h"
#include "overlay/OverlayManager.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/XDROperators.h"
#include "xdrpp/marshal.h"
#include <Tracy.hpp>
#include <fmt/chrono.h>
#include <fmt/format.h>

namespace stellar
{

constexpr std::chrono::seconds PENDING_DEMAND_TIMEOUT{1};
constexpr size_t PENDING_DEMAND_LIMIT{1000000};

Floodgate::FloodRecord::FloodRecord(StellarMessage const& msg, uint32_t ledger,
                                    uint64_t keyedShortHash,
                                    uint64_t unkeyedShortHash)
    : mLedgerSeq(ledger)
    , mMessage(msg)
    , mKeyedShortHash(keyedShortHash)
    , mUnkeyedShortHash(unkeyedShortHash)
{
}

Floodgate::Floodgate(Application& app)
    : mApp(app)
    , mFloodMapSize(
          app.getMetrics().NewCounter({"overlay", "memory", "flood-known"}))
    , mPendingDemandsSize(
          app.getMetrics().NewCounter({"overlay", "memory", "pending-demands"}))
    , mSendFromBroadcast(app.getMetrics().NewMeter(
          {"overlay", "flood", "broadcast"}, "message"))
    , mMessagesAdvertized(app.getMetrics().NewMeter(
          {"overlay", "flood", "advertized"}, "message"))
    , mMessagesDemanded(app.getMetrics().NewMeter(
          {"overlay", "flood", "demanded"}, "message"))
    , mMessagesFulfilled(app.getMetrics().NewMeter(
          {"overlay", "flood", "fulfilled"}, "message"))
    , mShuttingDown(false)
{
    mId = KeyUtils::toShortString(mApp.getConfig().NODE_SEED.getPublicKey());
}

// remove old flood records
void
Floodgate::clearBelow(uint32_t maxLedger)
{
    ZoneScoped;
    for (auto it = mFloodMap.cbegin(); it != mFloodMap.cend();)
    {
        if (it->second->mLedgerSeq < maxLedger)
        {
            auto record = it->second;
            mPendingDemands.erase(record->mKeyedShortHash);
            mPendingDemands.erase(record->mUnkeyedShortHash);
            mShortHashFloodMap.erase(record->mKeyedShortHash);
            mShortHashFloodMap.erase(record->mUnkeyedShortHash);
            it = mFloodMap.erase(it);
        }
        else
        {
            ++it;
        }
    }

    auto now = mApp.getClock().now();
    mPendingDemandCount.clear();
    for (auto it = mPendingDemands.cbegin(); it != mPendingDemands.cend();)
    {
        // Every pending demand older than 1 ledger is erased. This is narrower
        // than `maxLedger` because demands should really be fulfilled
        // immediately -- much less than 5 seconds. We just use this
        // `clearBelow` call as an opportunity to GC obsolete demands too.
        if (now - it->second.second > Herder::EXP_LEDGER_TIMESPAN_SECONDS)
        {
            it = mPendingDemands.erase(it);
        }
        else
        {
            mPendingDemandCount[it->second.first]++;
            ++it;
        }
    }
    mFloodMapSize.set_count(mFloodMap.size());
    mPendingDemandsSize.set_count(mPendingDemands.size());
}

std::pair<std::map<Hash, Floodgate::FloodRecord::pointer>::iterator, bool>
Floodgate::insert(StellarMessage const& msg, bool force)
{
    Hash index = xdrBlake2(msg);
    auto seq = mApp.getHerder().trackingConsensusLedgerIndex();
    auto iter = mFloodMap.find(index);
    if (iter != mFloodMap.end())
    {
        if (force)
        {
            // "Force" means "clear the mPeersTold and reset seq" when there's
            // an existing entry.
            iter->second->mPeersTold.clear();
            iter->second->mLedgerSeq = seq;
        }
        return std::make_pair(iter, false);
    }

    Hash LCLHash = mApp.getLedgerManager().getLastClosedLedgerHeader().hash;
    Hash zeroHash;
    uint64_t keyedShortHash =
        shortHash::xdrComputeKeyedHash(msg, ByteSlice(LCLHash.data(), 16));
    uint64_t unkeyedShortHash =
        shortHash::xdrComputeKeyedHash(msg, ByteSlice(zeroHash.data(), 16));

    removeAnyPendingDemand(keyedShortHash);
    removeAnyPendingDemand(unkeyedShortHash);

    FloodRecord::pointer rec = std::make_shared<FloodRecord>(
        msg, seq, keyedShortHash, unkeyedShortHash);
    mShortHashFloodMap.emplace(keyedShortHash, rec);
    mShortHashFloodMap.emplace(unkeyedShortHash, rec);
    auto ret = mFloodMap.emplace(index, rec);
    releaseAssert(ret.second);
    mFloodMapSize.set_count(mFloodMap.size());
    TracyPlot("overlay.memory.flood-known",
              static_cast<int64_t>(mFloodMap.size()));
    return ret;
}

bool
Floodgate::addRecord(StellarMessage const& msg, Peer::pointer peer, Hash& index)
{
    ZoneScoped;
    if (mShuttingDown)
    {
        index = xdrBlake2(msg);
        return false;
    }
    auto pair = insert(msg);
    index = pair.first->first;
    FloodRecord::pointer record = pair.first->second;
    if (peer)
    {
        record->mPeersTold.insert(peer->toString());
    }
    return pair.second;
}

void
Floodgate::removeAnyPendingDemand(uint64_t hash)
{
    auto i = mPendingDemands.find(hash);
    if (i != mPendingDemands.end())
    {
        auto c = mPendingDemandCount.find(i->second.first);
        releaseAssert(c->second != 0);
        c->second -= 1;
        mPendingDemands.erase(i);
        mPendingDemandsSize.set_count(mPendingDemands.size());
    }
}

void
Floodgate::addPendingDemand(uint64_t hash, std::string const& peer)
{
    auto i = mPendingDemands.find(hash);
    releaseAssert(i == mPendingDemands.end());
    auto now = mApp.getClock().now();
    mPendingDemands.emplace(hash, std::make_pair(peer, now));
    mPendingDemandCount[peer]++;
    mPendingDemandsSize.set_count(mPendingDemands.size());
}

bool
Floodgate::shouldFloodLazily(StellarMessage const& msg, Peer::pointer peer)
{
    if (peer->supportsAdverts())
    {
        auto const& cfg = mApp.getConfig();
        if (msg.type() == TRANSACTION)
        {
            return rand_flip(cfg.FLOOD_TX_LAZY_PROBABILITY);
        }
        else if (msg.type() == SCP_MESSAGE)
        {
            return rand_flip(cfg.FLOOD_SCP_LAZY_PROBABILITY);
        }
    }
    return false;
}

// send message to anyone you haven't gotten it from
bool
Floodgate::broadcast(StellarMessage const& msg, bool force)
{
    ZoneScoped;
    if (mShuttingDown)
    {
        return false;
    }

    auto pair = insert(msg, force);
    Hash const& index = pair.first->first;
    FloodRecord::pointer record = pair.first->second;

    CLOG_TRACE(Overlay, "broadcast {}", hexAbbrev(index));

    // Send (or at least advertize) it to people that haven't sent it to us
    // and/or we haven't sent it to in full. We _might_ advertize the same
    // message to the same peer twice if we somehow receive it twice and decide
    // to call broadcast() twice before sending it to them; but we should only
    // really be broadcast()'ing new messages anyway in our caller.
    auto& peersTold = record->mPeersTold;

    // make a copy, in case peers gets modified
    auto peers = mApp.getOverlayManager().getAuthenticatedPeers();

    bool broadcasted = false;
    auto smsg = std::make_shared<StellarMessage const>(msg);
    for (auto& peer : peers)
    {
        releaseAssert(peer.second->isAuthenticated());
        if (peersTold.insert(peer.second->toString()).second)
        {
            if (shouldFloodLazily(msg, peer.second))
            {
                uint64_t shortHash =
                    (mApp.getState() == Application::State::APP_SYNCED_STATE
                         ? record->mKeyedShortHash
                         : record->mUnkeyedShortHash);
                CLOG_TRACE(Overlay, "{} advertizing {} to {}", mId, shortHash,
                           KeyUtils::toShortString(peer.second->getPeerID()));
                mMessagesAdvertized.Mark();
                peer.second->advertizeMessage(shortHash);
            }
            else
            {
                mSendFromBroadcast.Mark();
                std::weak_ptr<Peer> weak(
                    std::static_pointer_cast<Peer>(peer.second));
                mApp.postOnMainThread(
                    [smsg, weak, log = !broadcasted]() {
                        auto strong = weak.lock();
                        if (strong)
                        {
                            strong->sendMessage(smsg, log);
                        }
                    },
                    fmt::format(FMT_STRING("broadcast to {}"),
                                peer.second->toString()));
            }
            broadcasted = true;
        }
    }
    CLOG_TRACE(Overlay, "broadcast {} told {}", hexAbbrev(index),
               peersTold.size());
    return broadcasted;
}

void
Floodgate::demandMissing(FloodAdvert const& adv, Peer::pointer fromPeer)
{
    auto msg = std::make_shared<StellarMessage>();
    msg->type(FLOOD_DEMAND);
    FloodDemand& demand = msg->floodDemand();
    auto now = mApp.getClock().now();
    std::string peerID = fromPeer->toString();
    for (uint64_t h : adv.hashes)
    {
        auto i = mShortHashFloodMap.find(h);
        if (i != mShortHashFloodMap.end())
        {
            // We already got a full message for h; record that fromPeer also
            // has the message, to inhibit advertizing or sending to fromPeer.
            i->second->mPeersTold.insert(fromPeer->toString());
            CLOG_TRACE(Overlay,
                       "{} already have full message for {} advertied by {}",
                       mId, h, KeyUtils::toShortString(fromPeer->getPeerID()));
        }
        else
        {
            auto existingDemand = mPendingDemands.find(h);
            if (existingDemand != mPendingDemands.end())
            {
                CLOG_TRACE(Overlay, "{} already demanded {} advertized by {}",
                           mId, h,
                           KeyUtils::toShortString(fromPeer->getPeerID()));
                std::chrono::milliseconds dur =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - existingDemand->second.second);
                if (dur < PENDING_DEMAND_TIMEOUT)
                {
                    // We don't have this message but we did already ask for it
                    // from someone else, more recently than
                    // PENDING_DEMAND_TIMEOUT, so we'll avoid asking for it here
                    // and wait a bit, hoping the peer we demanded it from
                    // fulfills the demand.
                    continue;
                }
                else
                {
                    bool lastDemandWasThisPeer =
                        existingDemand->second.first == peerID;

                    // We don't have the message, we did demand it it, but the
                    // peer we demanded it from hasn't fulfilled the demand
                    // within a timeout; we'll forget the existing demand now,
                    // in any case.
                    CLOG_TRACE(Overlay,
                               "demand for {} from {} expired after {}", h,
                               existingDemand->second.first, dur);

                    // NB: this call invalidates the iterator `existingDemand`.
                    removeAnyPendingDemand(h);

                    if (lastDemandWasThisPeer)
                    {
                        // This peer didn't answer our last demand. We'll avoid
                        // demanding from them just now, to let someone else
                        // have a chance to advertize and have us demand from
                        // them instead. If nobody does, we'll demand again from
                        // this peer on its next advertisement.
                        CLOG_TRACE(Overlay,
                                   "leaving opening for someone other than {} "
                                   "to advertize {}",
                                   peerID, h);
                        continue;
                    }
                }
            }

            // We don't have this message in full and haven't demanded it yet
            // from anyone who advertized it; ask now and leave a record that
            // we've done so to avoid demanding it from others.
            mMessagesDemanded.Mark();
            if (mPendingDemandCount[peerID] < PENDING_DEMAND_LIMIT)
            {
                CLOG_TRACE(Overlay, "{} demanding {} from {}", mId, h,
                           KeyUtils::toShortString(fromPeer->getPeerID()));
                addPendingDemand(h, fromPeer->toString());
            }
            else
            {
                CLOG_TRACE(Overlay,
                           "{} not demanding {} from {} -- already have {} "
                           "demands pending",
                           mId, h,
                           KeyUtils::toShortString(fromPeer->getPeerID()),
                           mPendingDemandCount[peerID]);
            }
            demand.hashes.emplace_back(h);
        }
    }
    fromPeer->sendMessage(msg);
}

void
Floodgate::fulfillDemand(FloodDemand const& dmd, Peer::pointer fromPeer)
{
    for (uint64_t h : dmd.hashes)
    {
        auto i = mShortHashFloodMap.find(h);
        if (i == mShortHashFloodMap.end())
        {
            CLOG_TRACE(Overlay,
                       "can't fulfill demand for {} demanded by {} -- don't "
                       "know of message",
                       mId, h, KeyUtils::toShortString(fromPeer->getPeerID()));
        }
        else
        {
            CLOG_TRACE(Overlay, "{} fulfilling demand for {} demanded by {}",
                       mId, h, KeyUtils::toShortString(fromPeer->getPeerID()));
            mMessagesFulfilled.Mark();
            i->second->mPeersTold.insert(fromPeer->toString());
            auto smsg = std::make_shared<StellarMessage>(i->second->mMessage);
            fromPeer->sendMessage(smsg);
        }
    }
}

std::set<Peer::pointer>
Floodgate::getPeersKnows(Hash const& h)
{
    std::set<Peer::pointer> res;
    auto record = mFloodMap.find(h);
    if (record != mFloodMap.end())
    {
        auto& ids = record->second->mPeersTold;
        auto const& peers = mApp.getOverlayManager().getAuthenticatedPeers();
        for (auto& p : peers)
        {
            if (ids.find(p.second->toString()) != ids.end())
            {
                res.insert(p.second);
            }
        }
    }
    return res;
}

void
Floodgate::shutdown()
{
    mShuttingDown = true;
    mFloodMap.clear();
    mShortHashFloodMap.clear();
    mPendingDemands.clear();
}

void
Floodgate::forgetRecord(Hash const& h)
{
    CLOG_TRACE(Overlay, "{} forgetting {}", mId, hexAbbrev(h));
    auto i = mFloodMap.find(h);
    if (i != mFloodMap.end())
    {
        auto record = i->second;
        mShortHashFloodMap.erase(record->mKeyedShortHash);
        mShortHashFloodMap.erase(record->mUnkeyedShortHash);
        mPendingDemands.erase(record->mKeyedShortHash);
        mPendingDemands.erase(record->mUnkeyedShortHash);
        mPendingDemandsSize.set_count(mPendingDemands.size());
        mFloodMap.erase(i);
        mFloodMapSize.set_count(mFloodMap.size());
    }
}

void
Floodgate::updateRecord(StellarMessage const& oldMsg,
                        StellarMessage const& newMsg)
{
    ZoneScoped;
    Hash oldHash = xdrBlake2(oldMsg);
    Hash newHash = xdrBlake2(newMsg);

    auto oldIter = mFloodMap.find(oldHash);
    if (oldIter != mFloodMap.end())
    {
        auto record = oldIter->second;
        record->mMessage = newMsg;

        mFloodMap.erase(oldIter);
        mFloodMap.emplace(newHash, record);
    }
}
}
