#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <memory>
#include <functional>
#include <string>
#include "generated/StellarXDR.h"
#include "scp/SCP.h"
#include "lib/json/json-forwards.h"
#include "util/Timer.h"

namespace stellar
{
typedef std::shared_ptr<SCPQuorumSet> SCPQuorumSetPtr;

class TxSetFrame;
typedef std::shared_ptr<TxSetFrame> TxSetFramePtr;

class TransactionFrame;
typedef std::shared_ptr<TransactionFrame> TransactionFramePtr;

class Application;
class Peer;
typedef std::shared_ptr<Peer> PeerPtr;

/*
 * Public Interface to the Herder module
 *
 * Drives the SCP consensus protocol, is responsible for collecting Txs and
 * TxSets from the network and making sure Txs aren't lost in ledger close
 *
 * LATER: These interfaces need cleaning up. We need to work out how to
 * make the bidirectional interfaces
 */
class Herder
{
  public:

    // Expected time between two ledger close.
    static std::chrono::seconds const EXP_LEDGER_TIMESPAN_SECONDS;
    
    // Maximum timeout for SCP consensus.
    static std::chrono::seconds const MAX_SCP_TIMEOUT_SECONDS;
    
    // timeout before considering the node out of sync
    static std::chrono::seconds const CONSENSUS_STUCK_TIMEOUT_SECONDS;

    // Maximum time slip between nodes.
    static std::chrono::seconds const MAX_TIME_SLIP_SECONDS;
    
    // How many seconds of inactivity before evicting a node.
    static std::chrono::seconds const NODE_EXPIRATION_SECONDS;
    
    // How many ledger in past/future we consider an envelope viable.
    static uint32 const LEDGER_VALIDITY_BRACKET;
    
    
    static std::unique_ptr<Herder> create(Application& app);

    enum State
    {
        HERDER_SYNCING_STATE,
        HERDER_TRACKING_STATE,
        HERDER_NUM_STATE
    };

    virtual State getState() const = 0;
    virtual std::string getStateHuman() const = 0;

    virtual void bootstrap() = 0;

    // Returns a TxSet or start fetching it from the network if we don't know
    // about it.
    virtual TxSetFramePtr fetchTxSet(uint256 const& setHash,
                                     bool askNetwork) = 0;
    virtual void recvTxSet(TxSetFramePtr txSet) = 0;
    virtual void doesntHaveTxSet(uint256 const& txSetHash, PeerPtr peer) = 0;

    // Returns a QSet or start fetching it from the network if we don't know
    // about it.
    virtual SCPQuorumSetPtr fetchSCPQuorumSet(uint256 const& qSetHash,
                                              bool askNetwork) = 0;
    virtual void recvSCPQuorumSet(SCPQuorumSetPtr qSet) = 0;
    virtual void doesntHaveSCPQuorumSet(uint256 const& qSetHash,
                                        PeerPtr peer) = 0;

    // We are learning about a new transaction. Returns true if we should flood
    // this tx.
    virtual bool recvTransaction(TransactionFramePtr tx) = 0;

    // We are learning about a new envelope. Callback called with whether the
    // envelope should be flooded or not.
    virtual void recvSCPEnvelope(SCPEnvelope envelope,
                                 std::function<void(SCP::EnvelopeState)> const&
                                     cb = [](SCP::EnvelopeState)
                                 {
    }) = 0;

    virtual void triggerNextLedger() = 0;
    virtual ~Herder() {}

    virtual void dumpInfo(Json::Value& ret) = 0;

};
}
