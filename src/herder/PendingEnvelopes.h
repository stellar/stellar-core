#pragma once
#include <autocheck/function.hpp>
#include <queue>
#include <map>
#include <medida/medida.h>
#include <util/optional.h>
#include <set>
#include <xdr/Stellar-SCP.h>
#include "overlay/ItemFetcher.h"
#include "lib/json/json.h"
#include "lib/util/lrucache.hpp"
#include "crypto/SecretKey.h"

/*
SCP messages that you have received but are waiting to get the info of
before feeding into SCP
*/

namespace stellar
{

class HerderImpl;

class PendingEnvelopes
{
    Application& mApp;
    HerderImpl& mHerder;

    // ledger# and list of envelopes we have processed already
    std::map<uint64, std::vector<SCPEnvelope>> mProcessedEnvelopes;

    // ledger# and list of envelopes we are fetching right now
    std::map<uint64, std::set<SCPEnvelope>> mFetchingEnvelopes;

    // ledger# and list of envelopes that haven't been sent to SCP yet
    std::map<uint64, std::vector<SCPEnvelope>> mPendingEnvelopes;

    // all the quorum sets we have learned about
    cache::lru_cache<Hash, SCPQuorumSetPtr> mQsetCache;

    ItemFetcher mTxSetFetcher;
    ItemFetcher mQuorumSetFetcher;

    // all the txsets we have learned about per ledger#
    cache::lru_cache<Hash, TxSetFramePtr> mTxSetCache;

    // NodeIDs that are in quorum
    cache::lru_cache<NodeID, bool> mNodesInQuorum;

    medida::Counter& mPendingEnvelopesSize;

    // returns true if we think that the node is in quorum
    bool isNodeInQuorum(NodeID const& node);

  public:
    PendingEnvelopes(Application& app, HerderImpl& herder);
    ~PendingEnvelopes();

    void recvSCPEnvelope(SCPEnvelope const& envelope);
    void recvSCPQuorumSet(Hash hash, const SCPQuorumSet& qset);
    void recvTxSet(Hash hash, TxSetFramePtr txset);

    void peerDoesntHave(MessageType type, Hash const& itemID,
                        Peer::pointer peer);

    bool isFullyFetched(SCPEnvelope const& envelope);
    // returns true if already fetched
    bool startFetch(SCPEnvelope const& envelope);

    void envelopeReady(SCPEnvelope const& envelope);

    bool pop(uint64 slotIndex, SCPEnvelope& ret);

    void eraseBelow(uint64 slotIndex);

    void slotClosed(uint64 slotIndex);

    std::vector<uint64> readySlots();

    void dumpInfo(Json::Value& ret, size_t limit);

    TxSetFramePtr getTxSet(Hash const& hash);
    SCPQuorumSetPtr getQSet(Hash const& hash);
};
}
