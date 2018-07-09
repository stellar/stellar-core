#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "TxSetFrame.h"
#include "Upgrades.h"
#include "lib/json/json-forwards.h"
#include "overlay/Peer.h"
#include "overlay/StellarXDR.h"
#include "scp/SCP.h"
#include "util/Timer.h"
#include <functional>
#include <memory>
#include <string>

namespace stellar
{
class Application;
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

    enum TransactionSubmitStatus
    {
        TX_STATUS_PENDING = 0,
        TX_STATUS_DUPLICATE,
        TX_STATUS_ERROR,
        TX_STATUS_COUNT
    };

    static const char* TX_STATUS_STRING[TX_STATUS_COUNT];

    enum EnvelopeStatus
    {
        // for some reason this envelope was discarded - either is was invalid,
        // used unsane qset or was coming from node that is not in quorum
        ENVELOPE_STATUS_DISCARDED,
        // envelope data is currently being fetched
        ENVELOPE_STATUS_FETCHING,
        // current call to recvSCPEnvelope() was the first when the envelope
        // was fully fetched so it is ready for processing
        ENVELOPE_STATUS_READY,
        // envelope was already processed
        ENVELOPE_STATUS_PROCESSED,
    };

    virtual State getState() const = 0;
    virtual std::string getStateHuman() const = 0;

    // Ensure any metrics that are "current state" gauge-like counters reflect
    // the current reality as best as possible.
    virtual void syncMetrics() = 0;

    virtual void bootstrap() = 0;

    // restores Herder's state from disk
    virtual void restoreState() = 0;

    virtual bool recvSCPQuorumSet(Hash const& hash,
                                  SCPQuorumSet const& qset) = 0;
    virtual bool recvTxSet(Hash const& hash, TxSetFrame const& txset) = 0;
    // We are learning about a new transaction.
    virtual TransactionSubmitStatus recvTransaction(TransactionFramePtr tx) = 0;
    virtual void peerDoesntHave(stellar::MessageType type,
                                uint256 const& itemID, Peer::pointer peer) = 0;
    virtual TxSetFramePtr getTxSet(Hash const& hash) = 0;
    virtual SCPQuorumSetPtr getQSet(Hash const& qSetHash) = 0;

    // We are learning about a new envelope.
    virtual EnvelopeStatus recvSCPEnvelope(SCPEnvelope const& envelope) = 0;

    // We are learning about a new fully-fetched envelope.
    virtual EnvelopeStatus recvSCPEnvelope(SCPEnvelope const& envelope,
                                           const SCPQuorumSet& qset,
                                           TxSetFrame txset) = 0;

    // a peer needs our SCP state
    virtual void sendSCPStateToPeer(uint32 ledgerSeq, Peer::pointer peer) = 0;

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

    virtual ~Herder()
    {
    }

    virtual Json::Value getJsonInfo(size_t limit) = 0;
    virtual Json::Value getJsonQuorumInfo(NodeID const& id, bool summary,
                                          uint64 index = 0) = 0;
};
}
