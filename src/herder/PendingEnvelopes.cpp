#include "PendingEnvelopes.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "herder/HerderImpl.h"
#include "herder/HerderPersistence.h"
#include "herder/HerderUtils.h"
#include "herder/TxSetFrame.h"
#include "main/Application.h"
#include "main/Config.h"
#include "overlay/OverlayManager.h"
#include "scp/QuorumSetUtils.h"
#include "scp/Slot.h"
#include "util/Logging.h"
#include <unordered_set>
#include <xdrpp/marshal.h>

using namespace std;

#define TXSET_CACHE_SIZE 10000

namespace stellar
{

PendingEnvelopes::PendingEnvelopes(Application& app, HerderImpl& herder)
    : mApp(app)
    , mHerder(herder)
    , mTxSetFetcher(
          app, [](Peer::pointer peer, Hash hash) { peer->sendGetTxSet(hash); })
    , mQuorumSetFetcher(app, [](Peer::pointer peer,
                                Hash hash) { peer->sendGetQuorumSet(hash); })
    , mTxSetCache(TXSET_CACHE_SIZE)
    , mRebuildQuorum(true)
    , mQuorumTracker(mHerder.getSCP())
    , mQSetGCThreshold(0)
    , mProcessedCount(
          app.getMetrics().NewCounter({"scp", "pending", "processed"}))
    , mDiscardedCount(
          app.getMetrics().NewCounter({"scp", "pending", "discarded"}))
    , mFetchingCount(
          app.getMetrics().NewCounter({"scp", "pending", "fetching"}))
    , mReadyCount(app.getMetrics().NewCounter({"scp", "pending", "ready"}))
    , mFetchDuration(app.getMetrics().NewTimer({"scp", "fetch", "duration"}))
{
}

PendingEnvelopes::~PendingEnvelopes()
{
}

void
PendingEnvelopes::peerDoesntHave(MessageType type, Hash const& itemID,
                                 Peer::pointer peer)
{
    switch (type)
    {
    case TX_SET:
        mTxSetFetcher.doesntHave(itemID, peer);
        break;
    case SCP_QUORUMSET:
        mQuorumSetFetcher.doesntHave(itemID, peer);
        break;
    default:
        CLOG(INFO, "Herder") << "Unknown Type in peerDoesntHave: " << type;
        break;
    }
}

void
PendingEnvelopes::addSCPQuorumSet(Hash const& hash, SCPQuorumSet const& q)
{
    CLOG(TRACE, "Herder") << "Add SCPQSet " << hexAbbrev(hash);
    assert(isQuorumSetSane(q, false));

    auto qset = std::make_shared<SCPQuorumSet>(q);
    mKnownQSet.emplace(hash, qset);

    mQuorumSetFetcher.recv(hash);
}

bool
PendingEnvelopes::recvSCPQuorumSet(Hash const& hash, SCPQuorumSet const& q)
{
    CLOG(TRACE, "Herder") << "Got SCPQSet " << hexAbbrev(hash);

    auto lastSeenSlotIndex = mQuorumSetFetcher.getLastSeenSlotIndex(hash);
    if (lastSeenSlotIndex == 0)
    {
        return false;
    }

    bool res;

    if (isQuorumSetSane(q, false))
    {
        addSCPQuorumSet(hash, q);
        res = true;
    }
    else
    {
        discardSCPEnvelopesWithQSet(hash);
        res = false;
    }

    // trigger cleanup of quorum sets if we have too many of them
    if (mKnownQSet.size() > mQSetGCThreshold)
    {
        DropUnrefencedQsets();
        mQSetGCThreshold = mKnownQSet.size() * 2;
    }

    return res;
}

void
PendingEnvelopes::discardSCPEnvelopesWithQSet(Hash const& hash)
{
    CLOG(TRACE, "Herder") << "Discarding SCP Envelopes with SCPQSet "
                          << hexAbbrev(hash);

    auto envelopes = mQuorumSetFetcher.fetchingFor(hash);
    for (auto& envelope : envelopes)
        discardSCPEnvelope(envelope);

    updateMetrics();
}

void
PendingEnvelopes::updateMetrics()
{
    int64 processed = 0;
    int64 discarded = 0;
    int64 fetching = 0;
    int64 ready = 0;

    for (auto const& s : mEnvelopes)
    {
        auto& v = s.second;
        processed += v.mProcessedEnvelopes.size();
        discarded += v.mDiscardedEnvelopes.size();
        fetching += v.mFetchingEnvelopes.size();
        ready += v.mReadyEnvelopes.size();
    }
    mProcessedCount.set_count(processed);
    mDiscardedCount.set_count(discarded);
    mFetchingCount.set_count(fetching);
    mReadyCount.set_count(ready);
}

void
PendingEnvelopes::addTxSet(Hash const& hash, uint64 lastSeenSlotIndex,
                           TxSetFramePtr txset)
{
    CLOG(TRACE, "Herder") << "Add TxSet " << hexAbbrev(hash);

    mTxSetCache.put(hash, std::make_pair(lastSeenSlotIndex, txset));
    mTxSetFetcher.recv(hash);
}

bool
PendingEnvelopes::recvTxSet(Hash const& hash, TxSetFramePtr txset)
{
    CLOG(TRACE, "Herder") << "Got TxSet " << hexAbbrev(hash);

    auto lastSeenSlotIndex = mTxSetFetcher.getLastSeenSlotIndex(hash);
    if (lastSeenSlotIndex == 0)
    {
        return false;
    }

    addTxSet(hash, lastSeenSlotIndex, txset);
    return true;
}

bool
PendingEnvelopes::isNodeDefinitelyInQuorum(NodeID const& node)
{
    rebuildQuorumTrackerState(false);
    return mQuorumTracker.isNodeDefinitelyInQuorum(node);
}

// called from Peer and when an Item tracker completes
Herder::EnvelopeStatus
PendingEnvelopes::recvSCPEnvelope(SCPEnvelope const& envelope)
{
    auto const& nodeID = envelope.statement.nodeID;
    if (!isNodeDefinitelyInQuorum(nodeID))
    {
        CLOG(DEBUG, "Herder")
            << "Dropping envelope from "
            << mApp.getConfig().toShortString(nodeID) << " (not in quorum)";
        return Herder::ENVELOPE_STATUS_DISCARDED;
    }

    // did we discard this envelope?
    // do we already have this envelope?
    // do we have the qset
    // do we have the txset

    try
    {
        if (isDiscarded(envelope))
        {
            return Herder::ENVELOPE_STATUS_DISCARDED;
        }

        touchFetchCache(envelope);

        auto& envs = mEnvelopes[envelope.statement.slotIndex];
        auto& fetching = envs.mFetchingEnvelopes;
        auto& processed = envs.mProcessedEnvelopes;

        auto fetchit = fetching.find(envelope);

        if (fetchit == fetching.end())
        { // we aren't fetching this envelope
            if (processed.find(envelope) == processed.end())
            { // we haven't seen this envelope before
                // insert it into the fetching set
                fetchit =
                    fetching.emplace(envelope, std::chrono::steady_clock::now())
                        .first;
                startFetch(envelope);
            }
            else
            {
                // we already have this one
                return Herder::ENVELOPE_STATUS_PROCESSED;
            }
        }

        // we are fetching this envelope
        // check if we are done fetching it
        if (isFullyFetched(envelope))
        {
            std::chrono::nanoseconds durationNano =
                std::chrono::steady_clock::now() - fetchit->second;
            mFetchDuration.Update(durationNano);
            CLOG(TRACE, "Perf")
                << "Herder fetched for "
                << hexAbbrev(sha256(xdr::xdr_to_opaque(envelope))) << " in "
                << std::chrono::duration<double>(durationNano).count()
                << " seconds";

            // move the item from fetching to processed
            processed.emplace(envelope);
            fetching.erase(fetchit);

            envelopeReady(envelope);
            updateMetrics();
            return Herder::ENVELOPE_STATUS_READY;
        } // else just keep waiting for it to come in

        updateMetrics();
        return Herder::ENVELOPE_STATUS_FETCHING;
    }
    catch (xdr::xdr_runtime_error& e)
    {
        CLOG(TRACE, "Herder")
            << "PendingEnvelopes::recvSCPEnvelope got corrupt message: "
            << e.what();
        return Herder::ENVELOPE_STATUS_DISCARDED;
    }
}

void
PendingEnvelopes::discardSCPEnvelope(SCPEnvelope const& envelope)
{
    try
    {
        auto& envs = mEnvelopes[envelope.statement.slotIndex];
        auto& discardedSet = envs.mDiscardedEnvelopes;
        auto r = discardedSet.insert(envelope);

        if (!r.second)
        {
            return;
        }

        envs.mFetchingEnvelopes.erase(envelope);

        stopFetch(envelope);
        updateMetrics();
    }
    catch (xdr::xdr_runtime_error& e)
    {
        CLOG(TRACE, "Herder")
            << "PendingEnvelopes::discardSCPEnvelope got corrupt message: "
            << e.what();
    }
}

bool
PendingEnvelopes::isDiscarded(SCPEnvelope const& envelope) const
{
    auto envelopes = mEnvelopes.find(envelope.statement.slotIndex);
    if (envelopes == mEnvelopes.end())
    {
        return false;
    }

    auto& discardedSet = envelopes->second.mDiscardedEnvelopes;
    auto discarded =
        std::find(std::begin(discardedSet), std::end(discardedSet), envelope);
    return discarded != discardedSet.end();
}

void
PendingEnvelopes::envelopeReady(SCPEnvelope const& envelope)
{
    CLOG(TRACE, "Herder") << "Envelope ready i:" << envelope.statement.slotIndex
                          << " t:" << envelope.statement.pledges.type();

    StellarMessage msg;
    msg.type(SCP_MESSAGE);
    msg.envelope() = envelope;
    mApp.getOverlayManager().broadcastMessage(msg);

    mEnvelopes[envelope.statement.slotIndex].mReadyEnvelopes.push_back(
        envelope);
}

bool
PendingEnvelopes::isFullyFetched(SCPEnvelope const& envelope)
{
    if (mKnownQSet.find(Slot::getCompanionQuorumSetHashFromStatement(
            envelope.statement)) == mKnownQSet.end())
        return false;

    auto txSetHashes = getTxSetHashes(envelope);
    return std::all_of(std::begin(txSetHashes), std::end(txSetHashes),
                       [this](Hash const& txSetHash) {
                           return mTxSetCache.exists(txSetHash);
                       });
}

void
PendingEnvelopes::startFetch(SCPEnvelope const& envelope)
{
    Hash h = Slot::getCompanionQuorumSetHashFromStatement(envelope.statement);

    if (mKnownQSet.find(h) == mKnownQSet.end())
    {
        mQuorumSetFetcher.fetch(h, envelope);
    }

    for (auto const& h2 : getTxSetHashes(envelope))
    {
        if (!mTxSetCache.exists(h2))
        {
            mTxSetFetcher.fetch(h2, envelope);
        }
    }

    CLOG(TRACE, "Herder") << "StartFetch i:" << envelope.statement.slotIndex
                          << " t:" << envelope.statement.pledges.type();
}

void
PendingEnvelopes::stopFetch(SCPEnvelope const& envelope)
{
    Hash h = Slot::getCompanionQuorumSetHashFromStatement(envelope.statement);
    mQuorumSetFetcher.stopFetch(h, envelope);

    for (auto const& h2 : getTxSetHashes(envelope))
    {
        mTxSetFetcher.stopFetch(h2, envelope);
    }

    CLOG(TRACE, "Herder") << "StopFetch i:" << envelope.statement.slotIndex
                          << " t:" << envelope.statement.pledges.type();
}

void
PendingEnvelopes::touchFetchCache(SCPEnvelope const& envelope)
{
    auto qsetHash =
        Slot::getCompanionQuorumSetHashFromStatement(envelope.statement);

    for (auto const& h : getTxSetHashes(envelope))
    {
        if (mTxSetCache.exists(h))
        {
            auto& item = mTxSetCache.get(h);
            item.first = std::max(item.first, envelope.statement.slotIndex);
        }
    }
}

bool
PendingEnvelopes::pop(uint64 slotIndex, SCPEnvelope& ret)
{
    auto it = mEnvelopes.begin();
    while (it != mEnvelopes.end() && slotIndex >= it->first)
    {
        auto& v = it->second.mReadyEnvelopes;
        if (v.size() != 0)
        {
            ret = v.back();
            v.pop_back();

            return true;
        }
        it++;
    }
    return false;
}

vector<uint64>
PendingEnvelopes::readySlots()
{
    vector<uint64> result;
    for (auto const& entry : mEnvelopes)
    {
        if (!entry.second.mReadyEnvelopes.empty())
            result.push_back(entry.first);
    }
    return result;
}

void
PendingEnvelopes::eraseBelow(uint64 slotIndex)
{
    for (auto iter = mEnvelopes.begin(); iter != mEnvelopes.end();)
    {
        if (iter->first < slotIndex)
        {
            iter = mEnvelopes.erase(iter);
        }
        else
            break;
    }

    // 0 is special mark for data that we do not know the slot index
    // it is used for state loaded from database
    mTxSetCache.erase_if([&](TxSetFramCacheItem const& i) {
        return i.first != 0 && i.first < slotIndex;
    });

    updateMetrics();
}

void
PendingEnvelopes::slotClosed(uint64 slotIndex)
{
    // force recomputing the transitive quorum
    mRebuildQuorum = true;

    // stop processing envelopes & downloads for the slot falling off the
    // window
    if (slotIndex > Herder::MAX_SLOTS_TO_REMEMBER)
    {
        slotIndex -= Herder::MAX_SLOTS_TO_REMEMBER;

        mEnvelopes.erase(slotIndex);

        mTxSetFetcher.stopFetchingBelow(slotIndex + 1);
        mQuorumSetFetcher.stopFetchingBelow(slotIndex + 1);

        mTxSetCache.erase_if(
            [&](TxSetFramCacheItem const& i) { return i.first == slotIndex; });
    }

    updateMetrics();
}

TxSetFramePtr
PendingEnvelopes::getTxSet(Hash const& hash)
{
    if (mTxSetCache.exists(hash))
    {
        return mTxSetCache.get(hash).second;
    }

    return TxSetFramePtr();
}

SCPQuorumSetPtr
PendingEnvelopes::getQSet(Hash const& hash)
{
    auto qSetCacheIt = mKnownQSet.find(hash);
    if (qSetCacheIt != mKnownQSet.end())
    {
        return qSetCacheIt->second;
    }
    SCPQuorumSetPtr qset;
    auto& scp = mHerder.getSCP();
    if (hash == scp.getLocalNode()->getQuorumSetHash())
    {
        qset = make_shared<SCPQuorumSet>(scp.getLocalQuorumSet());
    }
    else
    {
        auto& db = mApp.getDatabase();
        qset = HerderPersistence::getQuorumSet(db, db.getSession(), hash);
    }
    if (qset)
    {
        mKnownQSet.emplace(hash, qset);
    }
    return qset;
}

Json::Value
PendingEnvelopes::getJsonInfo(size_t limit)
{
    Json::Value ret;

    updateMetrics();

    auto& scp = mHerder.getSCP();
    {
        auto it = mEnvelopes.rbegin();
        size_t l = limit;
        while (it != mEnvelopes.rend() && l-- != 0)
        {
            if (it->second.mFetchingEnvelopes.size() != 0)
            {
                Json::Value& slot = ret[std::to_string(it->first)]["fetching"];
                for (auto const& kv : it->second.mFetchingEnvelopes)
                {
                    slot.append(scp.envToStr(kv.first));
                }
            }
            if (it->second.mReadyEnvelopes.size() != 0)
            {
                Json::Value& slot = ret[std::to_string(it->first)]["pending"];
                for (auto const& e : it->second.mReadyEnvelopes)
                {
                    slot.append(scp.envToStr(e));
                }
            }
            it++;
        }
    }
    return ret;
}

void
PendingEnvelopes::rebuildQuorumTrackerState(bool force)
{
    if (!force && !mRebuildQuorum)
    {
        return;
    }
    mRebuildQuorum = false;
    auto& scp = mHerder.getSCP();
    // rebuild quorum information using data sources starting with the
    // freshest source
    mQuorumTracker.rebuild([&](NodeID const& id) -> SCPQuorumSetPtr {
        SCPQuorumSetPtr res;
        if (id == scp.getLocalNodeID())
        {
            res = getQSet(scp.getLocalNode()->getQuorumSetHash());
        }
        else
        {
            auto m = scp.getLatestMessage(id);
            if (m != nullptr)
            {
                auto h =
                    Slot::getCompanionQuorumSetHashFromStatement(m->statement);
                res = getQSet(h);
            }
            if (res == nullptr)
            {
                // see if we had some information for that node
                auto& db = mApp.getDatabase();
                auto h = HerderPersistence::getNodeQuorumSet(
                    db, db.getSession(), id);
                if (h)
                {
                    res = getQSet(*h);
                }
            }
        }
        return res;
    });
}

QuorumTracker::QuorumMap const&
PendingEnvelopes::getCurrentlyTrackedQuorum() const
{
    return mQuorumTracker.getQuorum();
}

void
PendingEnvelopes::envelopeProcessed(SCPEnvelope const& env)
{
    auto const& st = env.statement;
    auto const& id = st.nodeID;

    auto h = Slot::getCompanionQuorumSetHashFromStatement(st);

    SCPQuorumSetPtr qset = getQSet(h);
    if (!mQuorumTracker.expand(id, qset))
    {
        // could not expand quorum, queue up a rebuild
        mRebuildQuorum = true;
    }
}

void
PendingEnvelopes::DropUnrefencedQsets()
{
    std::unordered_map<Hash, SCPQuorumSetPtr> qsets;

    auto addQset = [&](SCPEnvelope const& e) {
        auto r = qsets.emplace(
            Slot::getCompanionQuorumSetHashFromStatement(e.statement),
            SCPQuorumSetPtr());
        if (r.second)
        {
            // if we didn't know of this hash, also set the QSet
            r.first->second = getQSet(r.first->first);
        }
    };

    auto& scp = mHerder.getSCP();

    // computes the qsets referenced
    // by all slots
    auto itt = scp.descSlots();
    for (; itt.first != itt.second; ++itt.first)
    {
        auto slot = *itt.first;
        scp.processCurrentState(slot,
                                [&](SCPEnvelope const& e) {
                                    addQset(e);
                                    return true;
                                },
                                true);
    }
    // add qsets referenced by quorum
    rebuildQuorumTrackerState(false);
    for (auto const& q : mQuorumTracker.getQuorum())
    {
        if (q.second)
        {
            auto qHash = sha256(xdr::xdr_to_opaque(*q.second));
            qsets.emplace(qHash, q.second);
        }
    }
    // now, compute qsets referenced by pending envelopes
    // NB: skip "processed" as "ready" is the subset of processed
    // that is still owned by this class
    for (auto const& ee : mEnvelopes)
    {
        for (auto const& e : ee.second.mFetchingEnvelopes)
        {
            addQset(e.first);
        }
        for (auto const& e : ee.second.mReadyEnvelopes)
        {
            addQset(e);
        }
    }

    // transform into map of known quorum sets (not just referenced)
    for (auto it = qsets.begin(); it != qsets.end();)
    {
        if (it->second == nullptr)
        {
            it = qsets.erase(it);
        }
        else
        {
            ++it;
        }
    }

    if (Logging::logDebug("Herder"))
    {
        for (auto const& qs : mKnownQSet)
        {
            if (qsets.find(qs.first) == qsets.end())
            {
                CLOG(DEBUG, "Herder")
                    << "Dropping QSet " << hexAbbrev(qs.first);
            }
        }
    }

    mKnownQSet = std::move(qsets);
}

bool
PendingEnvelopes::isReady(SCPEnvelope const& e) const
{
    auto it = mEnvelopes.find(e.statement.slotIndex);
    if (it == mEnvelopes.end())
    {
        return false;
    }
    auto& se = it->second;
    return std::find(se.mReadyEnvelopes.begin(), se.mReadyEnvelopes.end(), e) !=
           se.mReadyEnvelopes.end();
}

bool
PendingEnvelopes::isProcessed(SCPEnvelope const& e) const
{
    auto it = mEnvelopes.find(e.statement.slotIndex);
    if (it == mEnvelopes.end())
    {
        return false;
    }
    auto& se = it->second;
    return se.mProcessedEnvelopes.find(e) != se.mProcessedEnvelopes.end();
}
}
