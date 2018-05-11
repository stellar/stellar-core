#pragma once

// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/Herder.h"
#include "overlay/FetchingEnvelopes.h"
#include "overlay/ReadyEnvelopes.h"
#include "xdr/Stellar-types.h"

/**
 * Container for envelopes that have not yet been processed by Herder. Each
 * envelope here can have on of four states: DISCARDED means that envelope
 * should not be processed for any reason, FETCHING means that some data for
 * this envelope is still not available on local node, READY means that it can
 * be processed by herder and PROCESSED that is has been processed already.
 */
namespace stellar
{

class PendingEnvelopes
{
  public:
    PendingEnvelopes(Application& app);
    ~PendingEnvelopes();

    /**
     * Checks if this envelope should be processed at all - if it is too old or
     * was discarded or processed already, it will be ignored. Otherwise it is
     * either put into FETCHING or READY state.
     */
    EnvelopeHandler::EnvelopeStatus handleEnvelope(Peer::pointer peer,
                                                   SCPEnvelope const& envelope);

    /**
     * After clearing internal quorum node cache it passes the call to
     * FetchingEnvelopes class. Returns list of envelopes that are now READY.
     */
    std::set<SCPEnvelope> handleQuorumSet(SCPQuorumSet const& qset,
                                          bool force = false);

    /**
     * It passes the call to FetchingEnvelopes class. Returns list of envelopes
     * that are now READY.
     */
    std::set<SCPEnvelope> handleTxSet(TxSetFramePtr txset, bool force = false);

    /**
     * Sets minimum value of envelope slot index that is acceptable.
     */
    void setMinimumSlotIndex(uint64_t slotIndex);

    /**
     * It passes the call to ReadyEnvelopes class.
     */
    bool pop(uint64_t slotIndex, SCPEnvelope& ret);

    /**
     * It passes the call to ReadyEnvelopes class.
     */
    std::vector<uint64_t> readySlots();

    Json::Value getJsonInfo(size_t limit);

  private:
    Application& mApp;
    FetchingEnvelopes mFetchingEnvelopes;
    ReadyEnvelopes mReadyEnvelopes;

    uint64_t mMinimumSlotIndex{0};

    void touchItemCache(SCPEnvelope const& envelope);
};
}
