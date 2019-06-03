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
#include <xdrpp/marshal.h>

using namespace std;

#define QSET_CACHE_SIZE 10000
#define TXSET_CACHE_SIZE 10000

namespace stellar
{

PendingEnvelopes::PendingEnvelopes(Application& app, HerderImpl& herder)
    : mApp(app)
    , mHerder(herder)
    , mQsetCache(QSET_CACHE_SIZE)
    , mTxSetFetcher(
          app, [](Peer::pointer peer, Hash hash) { peer->sendGetTxSet(hash); })
    , mQuorumSetFetcher(app, [](Peer::pointer peer,
                                Hash hash) { peer->sendGetQuorumSet(hash); })
    , mTxSetCache(TXSET_CACHE_SIZE)
    , mRebuildQuorum(true)
    , mQuorumTracker(mHerder.getSCP())
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
PendingEnvelopes::addSCPQuorumSet(Hash hash, const SCPQuorumSet& q)
{
    CLOG(TRACE, "Herder") << "Add SCPQSet " << hexAbbrev(hash);
    assert(isQuorumSetSane(q, false));

    auto qset = std::make_shared<SCPQuorumSet>(q);
    mQsetCache.put(hash, qset);

    mQuorumSetFetcher.recv(hash);
}

bool
PendingEnvelopes::recvSCPQuorumSet(Hash hash, const SCPQuorumSet& q)
{
    CLOG(TRACE, "Herder") << "Got SCPQSet " << hexAbbrev(hash);

    auto lastSeenSlotIndex = mQuorumSetFetcher.getLastSeenSlotIndex(hash);
    if (lastSeenSlotIndex <= 0)
    {
        return false;
    }

    if (isQuorumSetSane(q, false))
    {
        addSCPQuorumSet(hash, q);
        return true;
    }
    else
    {
        discardSCPEnvelopesWithQSet(hash);
        return false;
    }
}

void
PendingEnvelopes::discardSCPEnvelopesWithQSet(Hash hash)
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
PendingEnvelopes::addTxSet(Hash hash, uint64 lastSeenSlotIndex,
                           TxSetFramePtr txset)
{
    CLOG(TRACE, "Herder") << "Add TxSet " << hexAbbrev(hash);

    mTxSetCache.put(hash, std::make_pair(lastSeenSlotIndex, txset));
    mTxSetFetcher.recv(hash);
}

bool
PendingEnvelopes::recvTxSet(Hash hash, TxSetFramePtr txset)
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
    if (mRebuildQuorum)
    {
        rebuildQuorumTrackerState();
        mRebuildQuorum = false;
    }
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

        auto& set = mEnvelopes[envelope.statement.slotIndex].mFetchingEnvelopes;
        auto& processedList =
            mEnvelopes[envelope.statement.slotIndex].mProcessedEnvelopes;

        auto fetching = set.find(envelope);

        if (fetching == set.end())
        { // we aren't fetching this envelope
            if (find(processedList.begin(), processedList.end(), envelope) ==
                processedList.end())
            { // we haven't seen this envelope before
                // insert it into the fetching set
                fetching =
                    set.emplace(envelope, std::chrono::steady_clock::now())
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
            // move the item from fetching to processed
            processedList.emplace_back(fetching->first);
            std::chrono::nanoseconds durationNano =
                std::chrono::steady_clock::now() - fetching->second;
            mFetchDuration.Update(durationNano);
            CLOG(TRACE, "Perf")
                << "Herder fetched for "
                << hexAbbrev(sha256(xdr::xdr_to_opaque(envelope))) << " in "
                << std::chrono::duration<double>(durationNano).count()
                << " seconds";

            set.erase(fetching);
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
        if (isDiscarded(envelope))
        {
            return;
        }

        auto& discardedSet =
            mEnvelopes[envelope.statement.slotIndex].mDiscardedEnvelopes;
        discardedSet.insert(envelope);

        auto& fetchingSet =
            mEnvelopes[envelope.statement.slotIndex].mFetchingEnvelopes;
        fetchingSet.erase(envelope);

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
    if (!mQsetCache.exists(
            Slot::getCompanionQuorumSetHashFromStatement(envelope.statement)))
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

    if (!mQsetCache.exists(h))
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
    if (mQsetCache.exists(qsetHash))
    {
        // touch LRU
        mQsetCache.get(qsetHash);
    }

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
    if (mQsetCache.exists(hash))
    {
        return mQsetCache.get(hash);
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
        mQsetCache.put(hash, qset);
    }
    return qset;
}

Json::Value
PendingEnvelopes::getJsonInfo(size_t limit)
{
    Json::Value ret;

    updateMetrics();

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
                    slot.append(mHerder.getSCP().envToStr(kv.first));
                }
            }
            if (it->second.mReadyEnvelopes.size() != 0)
            {
                Json::Value& slot = ret[std::to_string(it->first)]["pending"];
                for (auto const& e : it->second.mReadyEnvelopes)
                {
                    slot.append(mHerder.getSCP().envToStr(e));
                }
            }
            it++;
        }
    }
    return ret;
}

void
PendingEnvelopes::rebuildQuorumTrackerState()
{
    // rebuild quorum information using data sources starting with the
    // freshest source
    mQuorumTracker.rebuild([&](NodeID const& id) -> SCPQuorumSetPtr {
        SCPQuorumSetPtr res;
        if (id == mHerder.getSCP().getLocalNodeID())
        {
            res = getQSet(mHerder.getSCP().getLocalNode()->getQuorumSetHash());
        }
        else
        {
            auto m = mHerder.getSCP().getLatestMessage(id);
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
}
