#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "TxSetFrame.h"
#include "Upgrades.h"
#include "herder/QuorumTracker.h"
#include "herder/TransactionQueue.h"
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

    // timeout before triggering out of sync recovery
    static std::chrono::seconds const OUT_OF_SYNC_RECOVERY_TIMER;

    // Timeout before sending latest checkpoint ledger after sending current SCP
    // state
    static std::chrono::seconds const SEND_LATEST_CHECKPOINT_DELAY;

    // Maximum time slip between nodes.
    static std::chrono::seconds constexpr MAX_TIME_SLIP_SECONDS =
        std::chrono::seconds{60};

    // How many seconds of inactivity before evicting a node.
    static std::chrono::seconds const NODE_EXPIRATION_SECONDS;

    // How many ledger in the future we consider an envelope viable.
    static uint32 const LEDGER_VALIDITY_BRACKET;

    // Threshold used to filter out irrelevant events.
    static std::chrono::nanoseconds const TIMERS_THRESHOLD_NANOSEC;

    static std::unique_ptr<Herder> create(Application& app);

    // number of additional ledgers we retrieve from peers before our own lcl,
    // this is to help recover potential missing SCP messages for other nodes
    static uint32 const SCP_EXTRA_LOOKBACK_LEDGERS;

    static uint32 const FLOW_CONTROL_BYTES_EXTRA_BUFFER;

    static std::chrono::minutes const TX_SET_GC_DELAY;

    enum State
    {
        // Starting up, no state is known
        HERDER_BOOTING_STATE,
        // Fell out of sync, resyncing
        HERDER_SYNCING_STATE,
        // Fully in sync with the network
        HERDER_TRACKING_NETWORK_STATE,
        HERDER_NUM_STATE
    };

    enum EnvelopeStatus
    {
        // for some reason this envelope was discarded - either it was invalid,
        // used unsane qset or was coming from node that is not in quorum
        ENVELOPE_STATUS_DISCARDED = -100,
        // envelope was skipped as it's from this validator
        ENVELOPE_STATUS_SKIPPED_SELF = -10,
        // envelope was already processed
        ENVELOPE_STATUS_PROCESSED = -1,

        // envelope data is currently being fetched
        ENVELOPE_STATUS_FETCHING = 0,
        // current call to recvSCPEnvelope() was the first when the envelope
        // was fully fetched so it is ready for processing
        ENVELOPE_STATUS_READY = 1
    };

    virtual State getState() const = 0;
    virtual std::string getStateHuman(State st) const = 0;

    // Ensure any metrics that are "current state" gauge-like counters reflect
    // the current reality as best as possible.
    virtual void syncMetrics() = 0;

    virtual void bootstrap() = 0;
    virtual void shutdown() = 0;

    // restores Herder's state from disk
    virtual void start() = 0;

    virtual void lastClosedLedgerIncreased(bool latest,
                                           TxSetXDRFrameConstPtr txSet) = 0;

    // Setup Herder's state to fully participate in consensus
    virtual void setTrackingSCPState(uint64_t index, StellarValue const& value,
                                     bool isTrackingNetwork) = 0;

    virtual bool recvSCPQuorumSet(Hash const& hash,
                                  SCPQuorumSet const& qset) = 0;
    virtual bool recvTxSet(Hash const& hash, TxSetXDRFrameConstPtr txset) = 0;
    // We are learning about a new transaction.
    virtual TransactionQueue::AddResult
    recvTransaction(TransactionFrameBasePtr tx, bool submittedFromSelf) = 0;
    virtual void peerDoesntHave(stellar::MessageType type,
                                uint256 const& itemID, Peer::pointer peer) = 0;
    virtual TxSetXDRFrameConstPtr getTxSet(Hash const& hash) = 0;
    virtual SCPQuorumSetPtr getQSet(Hash const& qSetHash) = 0;

    // We are learning about a new envelope.
    virtual EnvelopeStatus recvSCPEnvelope(SCPEnvelope const& envelope) = 0;

    virtual bool isTracking() const = 0;

#ifdef BUILD_TESTS
    // We are learning about a new fully-fetched envelope.
    virtual EnvelopeStatus recvSCPEnvelope(SCPEnvelope const& envelope,
                                           const SCPQuorumSet& qset,
                                           TxSetXDRFrameConstPtr txset) = 0;
    virtual EnvelopeStatus recvSCPEnvelope(SCPEnvelope const& envelope,
                                           const SCPQuorumSet& qset,
                                           StellarMessage const& txset) = 0;

    virtual void
    externalizeValue(TxSetXDRFrameConstPtr txSet, uint32_t ledgerSeq,
                     uint64_t closeTime,
                     xdr::xvector<UpgradeType, 6> const& upgrades,
                     std::optional<SecretKey> skToSignValue = std::nullopt) = 0;

    virtual VirtualTimer const& getTriggerTimer() const = 0;
    virtual void setMaxClassicTxSize(uint32 bytes) = 0;
    virtual void setMaxTxSize(uint32 bytes) = 0;
    virtual void setFlowControlExtraBufferSize(uint32 bytes) = 0;

    virtual ClassicTransactionQueue& getTransactionQueue() = 0;
    virtual SorobanTransactionQueue& getSorobanTransactionQueue() = 0;

    virtual bool sourceAccountPending(AccountID const& accountID) const = 0;
#endif
    // a peer needs our SCP state
    virtual void sendSCPStateToPeer(uint32 ledgerSeq, Peer::pointer peer) = 0;

    virtual uint32_t trackingConsensusLedgerIndex() const = 0;
    virtual uint32_t getMaxClassicTxSize() const = 0;
    // Get maximum size of the whole transaction StellarMessage allowed by
    // overlay
    virtual uint32_t getMaxTxSize() const = 0;
    virtual uint32_t getFlowControlExtraBuffer() const = 0;

    // return the smallest ledger number we need messages for when asking peers
    virtual uint32 getMinLedgerSeqToAskPeers() const = 0;
    virtual uint32 getMinLedgerSeqToRemember() const = 0;

    virtual bool isNewerNominationOrBallotSt(SCPStatement const& oldSt,
                                             SCPStatement const& newSt) = 0;

    // Returns sequence number for most recent completed checkpoint that the
    // node knows about, as derived from
    // trackingConsensusLedgerIndex
    virtual uint32_t getMostRecentCheckpointSeq() = 0;

    virtual void triggerNextLedger(uint32_t ledgerSeqToTrigger,
                                   bool forceTrackingSCP) = 0;
    virtual void setInSyncAndTriggerNextLedger() = 0;

    // lookup a nodeID in config and in SCP messages
    virtual bool resolveNodeID(std::string const& s, PublicKey& retKey) = 0;

    // sets the upgrades that should be applied during consensus
    virtual void setUpgrades(Upgrades::UpgradeParameters const& upgrades) = 0;
    // gets the upgrades that are scheduled by this node
    virtual std::string getUpgradesJson() = 0;

    virtual void forceSCPStateIntoSyncWithLastClosedLedger() = 0;

    // helper function to craft an SCPValue
    virtual StellarValue
    makeStellarValue(Hash const& txSetHash, uint64_t closeTime,
                     xdr::xvector<UpgradeType, 6> const& upgrades,
                     SecretKey const& s) = 0;

    virtual ~Herder()
    {
    }

    virtual Json::Value getJsonInfo(size_t limit, bool fullKeys = false) = 0;
    virtual Json::Value getJsonQuorumInfo(NodeID const& id, bool summary,
                                          bool fullKeys, uint64 index) = 0;
    virtual Json::Value getJsonTransitiveQuorumInfo(NodeID const& id,
                                                    bool summary,
                                                    bool fullKeys) = 0;
    virtual QuorumTracker::QuorumMap const&
    getCurrentlyTrackedQuorum() const = 0;

    virtual size_t getMaxQueueSizeOps() const = 0;
    virtual size_t getMaxQueueSizeSorobanOps() const = 0;
    virtual void maybeHandleUpgrade() = 0;

    virtual bool isBannedTx(Hash const& hash) const = 0;
    virtual TransactionFrameBaseConstPtr getTx(Hash const& hash) const = 0;

    virtual void beginApply() = 0;
};
}
