#ifndef __HERDERGATEWAY__
#define __HERDERGATEWAY__

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include <memory>
#include "generated/StellarXDR.h"
#include "generated/FBAXDR.h"

namespace stellar
{
typedef std::shared_ptr<FBAQuorumSet> FBAQuorumSetPtr;

class TxSetFrame;
typedef std::shared_ptr<TxSetFrame> TxSetFramePtr;

class TransactionFrame;
typedef std::shared_ptr<TransactionFrame> TransactionFramePtr;

class Peer;
typedef std::shared_ptr<Peer> PeerPtr;

/*
 * Public Interface to the Herder module
 *
 * Drives the FBA consensus protocol, is responsible for collecting Txs and
 * TxSets from the network and making sure Txs aren't lost in ledger close
 *
 * LATER: These gateway interface need cleaning up. We need to work out how to
 * make the bidirectional interfaces
 */
class HerderGateway
{
  public:
    // Returns a TxSet or start fetching it from the network if we don't know
    // about it.
    virtual TxSetFramePtr fetchTxSet(uint256 const& setHash,
                                     bool askNetwork) = 0;
    virtual void recvTxSet(TxSetFramePtr txSet) = 0;
    virtual void doesntHaveTxSet(uint256 const& txSetHash, 
                                 PeerPtr peer) = 0;

    // Returns a QSet or start fetching it from the network if we don't know
    // about it.
    virtual FBAQuorumSetPtr fetchFBAQuorumSet(uint256 const& qSetHash,
                                              bool askNetwork) = 0;
    virtual void recvFBAQuorumSet(FBAQuorumSetPtr qSet) = 0;
    virtual void doesntHaveFBAQuorumSet(uint256 const& qSetHash, 
                                        PeerPtr peer) = 0;

    // We are learning about a new transaction. Returns true if we should flood
    // this tx.
    virtual bool recvTransaction(TransactionFramePtr tx) = 0;

    // We are learning about a new envelope. Returns wether the signature is
    // valid or not
    virtual bool recvFBAEnvelope(FBAEnvelope envelope) = 0;

    // Called by Ledger once the ledger closes.
    virtual void ledgerClosed(LedgerHeader& ledger) = 0;
};
}

#endif
