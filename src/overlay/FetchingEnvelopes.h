#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/EnvelopeItemMap.h"
#include "overlay/NodesInQuorum.h"
#include "xdr/Stellar-SCP.h"

#include <map>
#include <set>

namespace stellar
{

/**
 * Container for envelopes that are currently being fetched. Also keeps track of
 * envelopes that are discarded and should be ignored. Only allows envelopes
 * above certaing slot index that can be set by setMinimumSlotIndex.
 */
class FetchingEnvelopes
{
  public:
    explicit FetchingEnvelopes(Application& app);
    ~FetchingEnvelopes();

    /**
     * Checks if all data for this envelope is available. If so, returns true.
     * If not, adds it to internal list of envelopes and start fetching data for
     * it. Peer argument is used to mark peers that should be first asked for
     * missing data.
     *
     * If envelope is too old, returns false.
     */
    bool handleEnvelope(Peer::pointer peer, SCPEnvelope const& envelope);

    /**
     * Handles quorum set. If force argument is false, it ignores any quorum
     * sets that are not currently being fetched.
     *
     * If given quorum set is not sane it discards any envelope that is fetching
     * it (possibly also discarding transactions sets requested by those
     * envelopes). Otherwise given quorum set is added to cache and any envelope
     * that has all of its data fetched now is returned.
     */
    std::set<SCPEnvelope> handleQuorumSet(SCPQuorumSet const& qset,
                                          bool force = false);

    /**
     * Handles transaction set. If force argument is false, it ignores any
     * transaction sets that are not currently being fetched.
     *
     * Transcation set is added to cache and any envelope that has all of its
     * data fetched now is returned.
     */
    std::set<SCPEnvelope> handleTxSet(TxSetFramePtr txset, bool force = false);

    /**
     * Check if given envelope is discarded. Envelopes are discarded if they are
     * too old or if they require quorum sets that are not sane.
     */
    bool isDiscarded(SCPEnvelope const& envelope);

    /**
     * Sets minimum value of envelope slot index that is acceptable. All
     * fetching envelopes with smaller slot indexes are removed and will no
     * longer be accepted.
     */
    void setMinimumSlotIndex(uint64_t slotIndex);

    void dumpInfo(Json::Value& ret, size_t limit);

  private:
    Application& mApp;
    NodesInQuorum mNodesInQuorum;

    struct SlotFetchingEnvelopes
    {
        std::set<SCPEnvelope> mDiscardedEnvelopes;
        std::set<SCPEnvelope> mFetchingEnvelopes;
    };

    std::map<uint64_t, SlotFetchingEnvelopes> mEnvelopes;

    EnvelopeItemMap mEnvelopeItemMap;
    uint64_t mMinimumSlotIndex{0};

    std::vector<ItemKey> itemsToFetch(SCPEnvelope const& envelope) const;

    void discardEnvelope(SCPEnvelope const& envelope);
    void discardEnvelopesWithItem(ItemKey itemKey);
    std::set<SCPEnvelope> processReadyItems(ItemKey itemKey);

    void startFetching(Peer::pointer peer, SCPEnvelope const& envelope,
                       std::vector<ItemKey> const& items);
};
}
