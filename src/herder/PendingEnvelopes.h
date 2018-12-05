#pragma once
#include "crypto/SecretKey.h"
#include "herder/Herder.h"
#include "lib/json/json.h"
#include "lib/util/lrucache.hpp"
#include "overlay/ItemFetcher.h"
#include <autocheck/function.hpp>
#include <map>
#include <medida/medida.h>
#include <queue>
#include <set>
#include <util/optional.h>

/*
SCP messages that you have received but are waiting to get the info of
before feeding into SCP
*/

namespace stellar
{

class HerderImpl;

struct SlotEnvelopes
{
    // list of envelopes we have processed already
    std::vector<SCPEnvelope> mProcessedEnvelopes;
    // list of envelopes we have discarded already
    std::set<SCPEnvelope> mDiscardedEnvelopes;
    // list of envelopes we are fetching right now
    std::set<SCPEnvelope> mFetchingEnvelopes;
    // list of ready envelopes that haven't been sent to SCP yet
    std::vector<SCPEnvelope> mReadyEnvelopes;
};

class PendingEnvelopes
{
    Application& mApp;
    HerderImpl& mHerder;

    // ledger# and list of envelopes in various states
    std::map<uint64, SlotEnvelopes> mEnvelopes;

    // all the quorum sets we have learned about
    cache::lru_cache<Hash, SCPQuorumSetPtr> mQsetCache;

    ItemFetcher mTxSetFetcher;
    ItemFetcher mQuorumSetFetcher;

    using TxSetFramCacheItem = std::pair<uint64, TxSetFramePtr>;
    // all the txsets we have learned about per ledger#
    cache::lru_cache<Hash, TxSetFramCacheItem> mTxSetCache;

    // NodeIDs that are in quorum
    cache::lru_cache<NodeID, bool> mNodesInQuorum;

    medida::Counter& mProcessedCount;
    medida::Counter& mDiscardedCount;
    medida::Counter& mFetchingCount;
    medida::Counter& mReadyCount;

    // returns true if we think that the node is in quorum
    bool isNodeInQuorum(NodeID const& node);

    // discards all SCP envelopes thats use QSet with given hash,
    // as it is not sane QSet
    void discardSCPEnvelopesWithQSet(Hash hash);

    void updateMetrics();

  public:
    PendingEnvelopes(Application& app, HerderImpl& herder);
    ~PendingEnvelopes();

    /**
     * Process received @p envelope.
     *
     * Return status of received envelope.
     */
    Herder::EnvelopeStatus recvSCPEnvelope(SCPEnvelope const& envelope);

    /**
     * Add @p qset identified by @p hash to local cache. Notifies
     * @see ItemFetcher about that event - it may cause calls to Herder's
     * recvSCPEnvelope which in turn may cause calls to @see recvSCPEnvelope
     * in PendingEnvelopes.
     */
    void addSCPQuorumSet(Hash hash, const SCPQuorumSet& qset);

    /**
     * Check if @p qset identified by @p hash was requested before from peers.
     * If not, ignores that @p qset. If it was requested, calls
     * @see addSCPQuorumSet.
     *
     * Return true if SCPQuorumSet is sane and useful (was asked for).
     */
    bool recvSCPQuorumSet(Hash hash, const SCPQuorumSet& qset);

    /**
     * Add @p txset identified by @p hash to local cache. Notifies
     * @see ItemFetcher about that event - it may cause calls to Herder's
     * recvSCPEnvelope which in turn may cause calls to @see recvSCPEnvelope
     * in PendingEnvelopes.
     */
    void addTxSet(Hash hash, uint64 lastSeenSlotIndex, TxSetFramePtr txset);

    /**
     * Check if @p txset identified by @p hash was requested before from peers.
     * If not, ignores that @p txset. If it was requested, calls
     * @see addTxSet.
     *
     * Return true if TxSet useful (was asked for).
     */
    bool recvTxSet(Hash hash, TxSetFramePtr txset);
    void discardSCPEnvelope(SCPEnvelope const& envelope);

    void peerDoesntHave(MessageType type, Hash const& itemID,
                        Peer::pointer peer);

    bool isDiscarded(SCPEnvelope const& envelope) const;
    bool isFullyFetched(SCPEnvelope const& envelope);
    void startFetch(SCPEnvelope const& envelope);
    void stopFetch(SCPEnvelope const& envelope);
    void touchFetchCache(SCPEnvelope const& envelope);

    void envelopeReady(SCPEnvelope const& envelope);

    bool pop(uint64 slotIndex, SCPEnvelope& ret);

    void eraseBelow(uint64 slotIndex);

    void slotClosed(uint64 slotIndex);

    std::vector<uint64> readySlots();

    Json::Value getJsonInfo(size_t limit);

    TxSetFramePtr getTxSet(Hash const& hash);
    SCPQuorumSetPtr getQSet(Hash const& hash);
};
}
