#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/TxSetFrame.h"
#include "herder/Upgrades.h"
#include "overlay/StellarXDR.h"
#include "scp/SCP.h"
#include "transport/EnvelopeHandler.h"
#include "transport/Peer.h"
#include "transport/TransactionHandler.h"
#include "util/Timer.h"

#include <functional>
#include <lib/json/json-forwards.h>
#include <memory>
#include <string>

namespace stellar
{
class Application;
class ItemKey;
class Peer;
class XDROutputFileStream;

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

    // How many ledger in the future we consider an envelope viable.
    static uint32 const LEDGER_VALIDITY_BRACKET;

    // How many ledgers in the past we keep track of
    static uint32 const MAX_SLOTS_TO_REMEMBER;

    // Threshold used to filter out irrelevant events.
    static std::chrono::nanoseconds const TIMERS_THRESHOLD_NANOSEC;

    static std::unique_ptr<Herder> create(Application& app);

    enum State
    {
        HERDER_SYNCING_STATE,
        HERDER_TRACKING_STATE,
        HERDER_NUM_STATE
    };

    virtual State getState() const = 0;
    virtual std::string getStateHuman() const = 0;

    // Ensure any metrics that are "current state" gauge-like counters reflect
    // the current reality as best as possible.
    virtual void syncMetrics() = 0;

    virtual void bootstrap() = 0;

    // restores Herder's state from disk
    virtual void restoreState() = 0;

    // We are learning about a new transaction.
    virtual TransactionHandler::TransactionStatus
    recvTransaction(Peer::pointer peer, TransactionFramePtr tx) = 0;

    virtual bool isValidEnvelope(SCPEnvelope const& envelope) = 0;

    // return current SCP state
    virtual std::vector<SCPEnvelope> getSCPState(uint32 ledgerSeq) = 0;

    // new envelopes are ready for processing
    virtual void processSCPQueue() = 0;

    // returns the latest known ledger seq using consensus information
    // and local state
    virtual uint32_t getCurrentLedgerSeq() const = 0;

    // Return the maximum sequence number for any tx (or 0 if none) from a given
    // sender in the pending or recent tx sets.
    virtual SequenceNumber getMaxSeqInPendingTxs(AccountID const&) = 0;

    virtual void triggerNextLedger(uint32_t ledgerSeqToTrigger) = 0;

    // lookup a nodeID in config and in SCP messages
    virtual bool resolveNodeID(std::string const& s, PublicKey& retKey) = 0;

    // sets the upgrades that should be applied during consensus
    virtual void setUpgrades(Upgrades::UpgradeParameters const& upgrades) = 0;
    // gets the upgrades that are scheduled by this node
    virtual std::string getUpgradesJson() = 0;

    virtual SCP& getSCP() = 0;

    virtual ~Herder()
    {
    }

    virtual Json::Value getJsonInfo(size_t limit) = 0;
    virtual Json::Value getJsonQuorumInfo(NodeID const& id, bool summary,
                                          uint64 index = 0) = 0;
};
}
