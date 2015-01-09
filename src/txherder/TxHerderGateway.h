#ifndef __TXHERDERGATEWAY__
#define __TXHERDERGATEWAY__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <memory>
#include "generated/StellarXDR.h"
#include "fba/FBA.h"
/*
Public Interface to the TXHerder module

Responsible for collecting Txs and TxSets from the network and making sure Txs
aren't lost in ledger close


LATER: These gateway interface need cleaning up. We need to work out how to make
the bidirectional interfaces

*/
namespace stellar
{

class TxSetFrame;
typedef std::shared_ptr<TxSetFrame> TxSetFramePtr;

class TransactionFrame;
typedef std::shared_ptr<TransactionFrame> TransactionFramePtr;

class Peer;
typedef std::shared_ptr<Peer> PeerPtr;

class TxHerderGateway
{
  public:
    // called by FBA  LATER: shouldn't be here
    enum SlotComparisonType
    {
        SAME_SLOT,
        PAST_SLOT,
        FUTURE_SLOT,
        INCOMPATIBLIE_SLOT
    };
    enum BallotValidType
    {
        VALID_BALLOT,
        INVALID_BALLOT,
        FUTURE_BALLOT,
        UNKNOWN_VALIDITY
    };
    // make sure this set contains any super old TXs
    virtual BallotValidType
    isValidBallotValue(Ballot const& ballot) = 0;
    virtual SlotComparisonType
    compareSlot(SlotBallot const& ballot) = 0;

    // can start fetching this TxSet from the network if we don't know about it
    virtual TxSetFramePtr fetchTxSet(uint256 const& setHash,
                                         bool askNetwork) = 0;

    virtual void externalizeValue(const SlotBallot&) = 0;

    // called by Overlay
    // a Tx set comes in from the wire
    virtual void recvTransactionSet(TxSetFramePtr txSet) = 0;
    virtual void doesntHaveTxSet(uint256 const& setHash,
                                 PeerPtr peer) = 0;

    // we are learning about a new transaction
    // returns true if we should flood this tx
    virtual bool recvTransaction(TransactionFramePtr tx) = 0;

    // called by Ledger
    virtual void ledgerClosed(LedgerHeader& ledger) = 0;
};
}

#endif
