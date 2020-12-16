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
#include "util/UnorderedSet.h"
#include <Tracy.hpp>
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
    , mValueSizeCache(TXSET_CACHE_SIZE + QSET_CACHE_SIZE)
    , mRebuildQuorum(true)
    , mQuorumTracker(mApp.getConfig().NODE_SEED.getPublicKey())
    , mProcessedCount(
          app.getMetrics().NewCounter({"scp", "pending", "processed"}))
    , mDiscardedCount(
          app.getMetrics().NewCounter({"scp", "pending", "discarded"}))
    , mFetchingCount(
          app.getMetrics().NewCounter({"scp", "pending", "fetching"}))
    , mReadyCount(app.getMetrics().NewCounter({"scp", "pending", "ready"}))
    , mFetchDuration(app.getMetrics().NewTimer({"scp", "fetch", "envelope"}))
    , mFetchTxSetTimer(app.getMetrics().NewTimer({"overlay", "fetch", "txset"}))
    , mFetchQsetTimer(app.getMetrics().NewTimer({"overlay", "fetch", "qset"}))
    , mCostPerSlot(app.getMetrics().NewHistogram({"scp", "cost", "per-slot"}))
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
        CLOG_INFO(Herder, "Unknown Type in peerDoesntHave: {}", type);
        break;
    }
}

SCPQuorumSetPtr
PendingEnvelopes::getKnownQSet(Hash const& hash, bool touch)
{
    SCPQuorumSetPtr res;
    auto it = mKnownQSets.find(hash);
    if (it != mKnownQSets.end())
    {
        res = it->second.lock();
    }

    // refresh the cache for this key
    if (res && touch)
    {
        mQsetCache.put(hash, res);
    }
    return res;
}

SCPQuorumSetPtr
PendingEnvelopes::putQSet(Hash const& qSetHash, SCPQuorumSet const& qSet)
{
    CLOG_TRACE(Herder, "Add SCPQSet {}", hexAbbrev(qSetHash));
    SCPQuorumSetPtr res;
    const char* errString = nullptr;
    assert(isQuorumSetSane(qSet, false, errString));
    res = getKnownQSet(qSetHash, true);
    if (!res)
    {
        res = std::make_shared<SCPQuorumSet>(qSet);
        mKnownQSets[qSetHash] = res;
        mQsetCache.put(qSetHash, res);
    }
    return res;
}

void
PendingEnvelopes::addSCPQuorumSet(Hash const& hash, SCPQuorumSet const& q)
{
    ZoneScoped;
    putQSet(hash, q);
    mQuorumSetFetcher.recv(hash, mFetchQsetTimer);
}

bool
PendingEnvelopes::recvSCPQuorumSet(Hash const& hash, SCPQuorumSet const& q)
{
    ZoneScoped;
    CLOG_TRACE(Herder, "Got SCPQSet {}", hexAbbrev(hash));

    auto lastSeenSlotIndex = mQuorumSetFetcher.getLastSeenSlotIndex(hash);
    if (lastSeenSlotIndex == 0)
    {
        return false;
    }

    const char* errString = nullptr;
    bool res = isQuorumSetSane(q, false, errString);
    if (res)
    {
        addSCPQuorumSet(hash, q);
    }
    else
    {
        discardSCPEnvelopesWithQSet(hash);
    }
    return res;
}

void
PendingEnvelopes::discardSCPEnvelopesWithQSet(Hash const& hash)
{
    ZoneScoped;
    CLOG_TRACE(Herder, "Discarding SCP Envelopes with SCPQSet {}",
               hexAbbrev(hash));

    auto envelopes = mQuorumSetFetcher.fetchingFor(hash);
    for (auto& envelope : envelopes)
        discardSCPEnvelope(envelope);
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
    TracyPlot("scp.pending.processed", processed);
    TracyPlot("scp.pending.fetching", fetching);
    mProcessedCount.set_count(processed);
    mDiscardedCount.set_count(discarded);
    mFetchingCount.set_count(fetching);
    mReadyCount.set_count(ready);
}

TxSetFramePtr
PendingEnvelopes::putTxSet(Hash const& hash, uint64 slot, TxSetFramePtr txset)
{
    auto res = getKnownTxSet(hash, slot, true);
    if (!res)
    {
        res = txset;
        mKnownTxSets[hash] = res;
        mTxSetCache.put(hash, std::make_pair(slot, res));
    }
    return res;
}

// tries to find a txset in memory, setting touch also touches the LRU,
// extending the lifetime of the result *and* updating the slot number
// to a greater value if needed
TxSetFramePtr
PendingEnvelopes::getKnownTxSet(Hash const& hash, uint64 slot, bool touch)
{
    // slot is only used when `touch` is set
    assert(touch || (slot == 0));
    TxSetFramePtr res;
    auto it = mKnownTxSets.find(hash);
    if (it != mKnownTxSets.end())
    {
        res = it->second.lock();
    }

    // refresh the cache for this key
    if (res && touch)
    {
        bool update = true;
        if (mTxSetCache.exists(hash))
        {
            auto& v = mTxSetCache.get(hash);
            update = (slot > v.first);
        }
        if (update)
        {
            mTxSetCache.put(hash, std::make_pair(slot, res));
        }
    }
    return res;
}

void
PendingEnvelopes::addTxSet(Hash const& hash, uint64 lastSeenSlotIndex,
                           TxSetFramePtr txset)
{
    ZoneScoped;
    CLOG_TRACE(Herder, "Add TxSet {}", hexAbbrev(hash));

    putTxSet(hash, lastSeenSlotIndex, txset);
    mTxSetFetcher.recv(hash, mFetchTxSetTimer);
}

bool
PendingEnvelopes::recvTxSet(Hash const& hash, TxSetFramePtr txset)
{
    ZoneScoped;
    CLOG_TRACE(Herder, "Got TxSet {}", hexAbbrev(hash));

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

static std::string
txSetsToStr(SCPEnvelope const& envelope)
{
    auto hashes = getTxSetHashes(envelope);
    UnorderedSet<Hash> hashesSet(hashes.begin(), hashes.end());
    std::string res = "[";
    for (auto const& s : hashesSet)
    {
        res += hexAbbrev(s);
        res += " ";
    }
    return res + "]";
}

// called from Peer and when an Item tracker completes
Herder::EnvelopeStatus
PendingEnvelopes::recvSCPEnvelope(SCPEnvelope const& envelope)
{
    ZoneScoped;
    auto const& nodeID = envelope.statement.nodeID;
    if (!isNodeDefinitelyInQuorum(nodeID))
    {
        CLOG_TRACE(Herder, "Dropping envelope from {} (not in quorum)",
                   mApp.getConfig().toShortString(nodeID));
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

        auto fetchIt = fetching.find(envelope);

        if (fetchIt == fetching.end())
        { // we aren't fetching this envelope
            if (processed.find(envelope) == processed.end())
            { // we haven't seen this envelope before
                // insert it into the fetching set
                fetchIt =
                    fetching.emplace(envelope, mApp.getClock().now()).first;
                startFetch(envelope);
                updateMetrics();
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
                mApp.getClock().now() - fetchIt->second;
            mFetchDuration.Update(durationNano);
            Hash h = Slot::getCompanionQuorumSetHashFromStatement(
                envelope.statement);
            if (Logging::logTrace("Perf"))
            {
                CLOG_TRACE(Perf,
                           "Herder fetched for envelope {} with txsets {} and "
                           "qset {} in {} seconds",
                           hexAbbrev(xdrSha256(envelope)),
                           txSetsToStr(envelope), hexAbbrev(h),
                           std::chrono::duration<double>(durationNano).count());
            }

            // move the item from fetching to processed
            processed.emplace(envelope);
            fetching.erase(fetchIt);

            envelopeReady(envelope);
            updateMetrics();
            return Herder::ENVELOPE_STATUS_READY;
        }
        else
        {
            // else just keep waiting for it to come in
            // and refresh fetchers as needed
            startFetch(envelope);
        }

        return Herder::ENVELOPE_STATUS_FETCHING;
    }
    catch (xdr::xdr_runtime_error& e)
    {
        CLOG_TRACE(Herder,
                   "PendingEnvelopes::recvSCPEnvelope got corrupt message: {}",
                   e.what());
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
    }
    catch (xdr::xdr_runtime_error& e)
    {
        CLOG_TRACE(
            Herder,
            "PendingEnvelopes::discardSCPEnvelope got corrupt message: {}",
            e.what());
    }
    updateMetrics();
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
    auto discarded = discardedSet.find(envelope);
    return discarded != discardedSet.end();
}

void
PendingEnvelopes::cleanKnownData()
{
    auto it = mKnownQSets.begin();
    while (it != mKnownQSets.end())
    {
        if (it->second.expired())
        {
            it = mKnownQSets.erase(it);
        }
        else
        {
            ++it;
        }
    }
    auto it2 = mKnownTxSets.begin();
    while (it2 != mKnownTxSets.end())
    {
        if (it2->second.expired())
        {
            it2 = mKnownTxSets.erase(it2);
        }
        else
        {
            ++it2;
        }
    }
}

#ifdef BUILD_TESTS
void
PendingEnvelopes::clearQSetCache()
{
    mQsetCache.clear();
    mKnownQSets.clear();
}
#endif

void
PendingEnvelopes::recordReceivedCost(SCPEnvelope const& env)
{
    ZoneScoped;

    if (!mQuorumTracker.isNodeDefinitelyInQuorum(env.statement.nodeID))
    {
        return;
    }

    // Record cost received from this validator
    size_t totalReceivedBytes = 0;
    totalReceivedBytes += xdr::xdr_argpack_size(env);

    for (auto const& v : getStellarValues(env.statement))
    {
        size_t txSetSize = 0;
        if (mValueSizeCache.exists(v.txSetHash))
        {
            txSetSize = mValueSizeCache.get(v.txSetHash);
        }
        else
        {
            auto txSetPtr = getTxSet(v.txSetHash);
            if (txSetPtr)
            {
                TransactionSet txSet;
                txSetPtr->toXDR(txSet);
                txSetSize = xdr::xdr_argpack_size(txSet);
                mValueSizeCache.put(v.txSetHash, txSetSize);
            }
        }

        totalReceivedBytes += txSetSize;
    }

    auto qSetHash = Slot::getCompanionQuorumSetHashFromStatement(env.statement);
    size_t qSetSize = 0;

    if (mValueSizeCache.exists(qSetHash))
    {
        qSetSize = mValueSizeCache.get(qSetHash);
    }
    else
    {
        auto qSetPtr = getQSet(qSetHash);
        if (qSetPtr)
        {
            qSetSize = xdr::xdr_argpack_size(*qSetPtr);
            mValueSizeCache.put(qSetHash, qSetSize);
        }
    }

    totalReceivedBytes += qSetSize;

    if (totalReceivedBytes > 0)
    {
        auto const& tracked =
            mQuorumTracker.findClosestValidators(env.statement.nodeID);
        auto& cost = mEnvelopes[env.statement.slotIndex].mReceivedCost;
        for (auto& t : tracked)
        {
            cost[t] += totalReceivedBytes;
        }
    }
}

void
PendingEnvelopes::envelopeReady(SCPEnvelope const& envelope)
{
    ZoneScoped;
    auto slot = envelope.statement.slotIndex;
    if (Logging::logTrace("Herder"))
    {
        CLOG_TRACE(Herder, "Envelope ready {} i:{} t:{}",
                   hexAbbrev(xdrSha256(envelope)), slot,
                   envelope.statement.pledges.type());
    }

    // envelope has been fetched completely, but SCP has not done
    // any validation on values yet. Regardless, record cost of this
    // envelope.
    recordReceivedCost(envelope);

    StellarMessage msg;
    msg.type(SCP_MESSAGE);
    msg.envelope() = envelope;
    mApp.getOverlayManager().broadcastMessage(msg);

    auto envW = mHerder.getHerderSCPDriver().wrapEnvelope(envelope);
    mEnvelopes[slot].mReadyEnvelopes.push_back(envW);
}

bool
PendingEnvelopes::isFullyFetched(SCPEnvelope const& envelope)
{
    if (!getKnownQSet(
            Slot::getCompanionQuorumSetHashFromStatement(envelope.statement),
            false))
    {
        return false;
    }

    auto txSetHashes = getTxSetHashes(envelope);
    return std::all_of(std::begin(txSetHashes), std::end(txSetHashes),
                       [&](Hash const& txSetHash) {
                           return getKnownTxSet(txSetHash, 0, false);
                       });
}

void
PendingEnvelopes::startFetch(SCPEnvelope const& envelope)
{
    ZoneScoped;
    Hash h = Slot::getCompanionQuorumSetHashFromStatement(envelope.statement);

    bool needSomething = false;
    if (!getKnownQSet(h, false))
    {
        mQuorumSetFetcher.fetch(h, envelope);
        needSomething = true;
    }

    for (auto const& h2 : getTxSetHashes(envelope))
    {
        if (!getKnownTxSet(h2, 0, false))
        {
            mTxSetFetcher.fetch(h2, envelope);
            needSomething = true;
        }
    }

    if (needSomething && Logging::logTrace("Herder"))
    {
        CLOG_TRACE(Herder, "StartFetch env {} i:{} t:{}",
                   hexAbbrev(xdrSha256(envelope)), envelope.statement.slotIndex,
                   envelope.statement.pledges.type());
    }
}

void
PendingEnvelopes::stopFetch(SCPEnvelope const& envelope)
{
    ZoneScoped;
    Hash h = Slot::getCompanionQuorumSetHashFromStatement(envelope.statement);
    mQuorumSetFetcher.stopFetch(h, envelope);

    for (auto const& h2 : getTxSetHashes(envelope))
    {
        mTxSetFetcher.stopFetch(h2, envelope);
    }

    if (Logging::logTrace("Herder"))
    {
        CLOG_TRACE(Herder, "StopFetch env {} i:{} t:{}",
                   hexAbbrev(xdrSha256(envelope)), envelope.statement.slotIndex,
                   envelope.statement.pledges.type());
    }
}

void
PendingEnvelopes::touchFetchCache(SCPEnvelope const& envelope)
{
    auto qsetHash =
        Slot::getCompanionQuorumSetHashFromStatement(envelope.statement);
    getKnownQSet(qsetHash, true);

    for (auto const& h : getTxSetHashes(envelope))
    {
        getKnownTxSet(h, envelope.statement.slotIndex, true);
    }
}

SCPEnvelopeWrapperPtr
PendingEnvelopes::pop(uint64 slotIndex)
{
    auto it = mEnvelopes.begin();
    while (it != mEnvelopes.end() && slotIndex >= it->first)
    {
        auto& v = it->second.mReadyEnvelopes;
        if (v.size() != 0)
        {
            auto ret = v.back();
            v.pop_back();

            updateMetrics();
            return ret;
        }
        it++;
    }
    return nullptr;
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
    stopAllBelow(slotIndex);

    // report only for the highest slot that we're purging
    reportCostOutliersForSlot(slotIndex - 1, true);

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

    cleanKnownData();
    updateMetrics();
}

void
PendingEnvelopes::stopAllBelow(uint64 slotIndex)
{
    // Before we purge a slot, check if any envelopes are still in
    // "fetching" mode and attempt to record cost
    for (auto it = mEnvelopes.begin();
         it != mEnvelopes.end() && it->first < slotIndex; it++)
    {
        auto& envs = it->second;
        for (auto const& env : envs.mFetchingEnvelopes)
        {
            recordReceivedCost(env.first);
        }
    }
    mTxSetFetcher.stopFetchingBelow(slotIndex);
    mQuorumSetFetcher.stopFetchingBelow(slotIndex);
}

void
PendingEnvelopes::forceRebuildQuorum()
{
    // force recomputing the transitive quorum
    mRebuildQuorum = true;
}

TxSetFramePtr
PendingEnvelopes::getTxSet(Hash const& hash)
{
    return getKnownTxSet(hash, 0, false);
}

SCPQuorumSetPtr
PendingEnvelopes::getQSet(Hash const& hash)
{
    auto qset = getKnownQSet(hash, false);
    if (qset)
    {
        return qset;
    }
    // if it was not known, see if we can find it somewhere else
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
        qset = putQSet(hash, *qset);
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
                    slot.append(scp.envToStr(e->getEnvelope()));
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

UnorderedMap<NodeID, size_t>
PendingEnvelopes::getCostPerValidator(uint64 slotIndex) const
{
    auto found = mEnvelopes.find(slotIndex);
    if (found != mEnvelopes.end())
    {
        return found->second.mReceivedCost;
    }
    return {};
}

static bool
shouldReportCostOutlier(double possibleOutlierCost, double expectedCost,
                        double ratioLimit)
{
    if (possibleOutlierCost <= 0 || expectedCost <= 0)
    {
        CLOG_ERROR(SCP, "Unexpected k-means value: must be positive");
        return false;
    }

    if (possibleOutlierCost / expectedCost > ratioLimit)
    {
        // If we're off by too much from the selected cluster, report the value
        return true;
    }
    return false;
}

void
PendingEnvelopes::reportCostOutliersForSlot(int64_t slotIndex,
                                            bool updateMetrics) const
{
    ZoneScoped;

    const uint32_t K_MEAN_NUM_CLUSTERS = 3;
    const double OUTLIER_COST_RATIO_LIMIT = 10;

    auto tracked = getCostPerValidator(slotIndex);
    if (tracked.empty())
    {
        return;
    }

    std::vector<double> myValidatorsTrackedCost;
    double totalCost = 0;

    for (auto const& t : tracked)
    {
        if (t.second > 0)
        {
            double cost = static_cast<double>(t.second);
            myValidatorsTrackedCost.push_back(cost);
            totalCost += cost;
        }
    }

    // Compare each node to other nodes we heard from for this slot
    // Note: do not include cost from self as it's much smaller and will
    // likely skew the data
    if (myValidatorsTrackedCost.size() > 1)
    {
        auto numClusters =
            std::min(static_cast<uint32_t>(myValidatorsTrackedCost.size()),
                     K_MEAN_NUM_CLUSTERS);
        auto clusters = k_means(myValidatorsTrackedCost, numClusters);

        if (clusters.empty() || *(clusters.begin()) <= 0)
        {
            CLOG_ERROR(SCP,
                       "Expected non-empty set of positive cluster centers");
        }
        else
        {
            Json::Value res;
            for (auto const& t : tracked)
            {
                auto clusterToCompare =
                    closest_cluster(static_cast<double>(t.second), clusters);
                auto const smallestCluster = *(clusters.begin());
                if (shouldReportCostOutlier(clusterToCompare, smallestCluster,
                                            OUTLIER_COST_RATIO_LIMIT))
                {
                    res[mApp.getConfig().toShortString(t.first)] =
                        static_cast<Json::UInt64>(t.second);
                }
            }

            Json::FastWriter fw;
            if (!res.empty())
            {
                CLOG_WARNING(SCP, "High validator costs for slot {}: {}",
                             slotIndex, fw.write(res));
            }
        }
    }

    if (updateMetrics && totalCost > 0)
    {
        mCostPerSlot.Update(static_cast<int64_t>(totalCost));
    }
}

Json::Value
PendingEnvelopes::getJsonValidatorCost(bool summary, bool fullKeys,
                                       uint64 index) const
{
    Json::Value res;

    auto computeTotalAndMaybeFillJson = [&](Json::Value& res, uint64 slot) {
        auto tracked = getCostPerValidator(slot);
        size_t total = 0;
        for (auto const& t : tracked)
        {
            if (!summary)
            {
                res[std::to_string(slot)]
                   [mApp.getConfig().toStrKey(t.first, fullKeys)] =
                       static_cast<Json::UInt64>(t.second);
            }
            total += t.second;
        }
        return total;
    };

    // Total for one or all slots
    size_t summaryTotal = 0;
    if (index == 0)
    {
        for (auto const& s : mEnvelopes)
        {
            auto slotTotal = computeTotalAndMaybeFillJson(res, s.first);
            summaryTotal += slotTotal;
        }
    }
    else
    {
        summaryTotal = computeTotalAndMaybeFillJson(res, index);
    }

    if (summary)
    {
        res = static_cast<Json::UInt64>(summaryTotal);
    }
    return res;
}
}
