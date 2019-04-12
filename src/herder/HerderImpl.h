#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/Herder.h"
#include "herder/HerderSCPDriver.h"
#include "herder/PendingEnvelopes.h"
#include "herder/TransactionQueue.h"
#include "herder/Upgrades.h"
#include "util/Timer.h"
#include "util/XDROperators.h"
#include <deque>
#include <memory>
#include <unordered_map>
#include <vector>

namespace medida
{
class Meter;
class Counter;
class Timer;
}

namespace stellar
{
class Application;
class LedgerManager;
class HerderSCPDriver;

/*
 * Is in charge of receiving transactions from the network.
 */
class HerderImpl : public Herder
{
  public:
    HerderImpl(Application& app);
    ~HerderImpl();

    State getState() const override;
    std::string getStateHuman() const override;

    void syncMetrics() override;

    // Bootstraps the HerderImpl if we're creating a new Network
    void bootstrap() override;

    void restoreState() override;

    SCP& getSCP();
    HerderSCPDriver&
    getHerderSCPDriver()
    {
        return mHerderSCPDriver;
    }

    void valueExternalized(uint64 slotIndex, StellarValue const& value);
    void emitEnvelope(SCPEnvelope const& envelope);

    TransactionQueue::AddResult
    recvTransaction(TransactionFramePtr tx) override;

    EnvelopeStatus recvSCPEnvelope(SCPEnvelope const& envelope) override;
    EnvelopeStatus recvSCPEnvelope(SCPEnvelope const& envelope,
                                   const SCPQuorumSet& qset,
                                   TxSetFrame txset) override;

    void sendSCPStateToPeer(uint32 ledgerSeq, Peer::pointer peer) override;

    bool recvSCPQuorumSet(Hash const& hash, const SCPQuorumSet& qset) override;
    bool recvTxSet(Hash const& hash, const TxSetFrame& txset) override;
    void peerDoesntHave(MessageType type, uint256 const& itemID,
                        Peer::pointer peer) override;
    TxSetFramePtr getTxSet(Hash const& hash) override;
    SCPQuorumSetPtr getQSet(Hash const& qSetHash) override;

    void processSCPQueue();

    uint32_t getCurrentLedgerSeq() const override;

    SequenceNumber getMaxSeqInPendingTxs(AccountID const&) override;

    void triggerNextLedger(uint32_t ledgerSeqToTrigger) override;

    void setUpgrades(Upgrades::UpgradeParameters const& upgrades) override;
    std::string getUpgradesJson() override;

    bool resolveNodeID(std::string const& s, PublicKey& retKey) override;

    Json::Value getJsonInfo(size_t limit, bool fullKeys = false) override;
    Json::Value getJsonQuorumInfo(NodeID const& id, bool summary,
                                  bool fullKeys = false,
                                  uint64 index = 0) override;

#ifdef BUILD_TESTS
    // used for testing
    PendingEnvelopes& getPendingEnvelopes();
#endif

  private:
    void ledgerClosed();

    void startRebroadcastTimer();
    void rebroadcast();
    void broadcast(SCPEnvelope const& e);

    void processSCPQueueUpToIndex(uint64 slotIndex);

    TransactionQueue mTransactionQueue;

    void
    updateTransactionQueue(std::vector<TransactionFramePtr> const& applied);

    PendingEnvelopes mPendingEnvelopes;
    Upgrades mUpgrades;
    HerderSCPDriver mHerderSCPDriver;

    void herderOutOfSync();

    // attempt to retrieve additional SCP messages from peers
    void getMoreSCPState();

    // last slot that was persisted into the database
    // only keep track of the most recent slot
    uint64 mLastSlotSaved;

    // timer that detects that we're stuck on an SCP slot
    VirtualTimer mTrackingTimer;

    // saves the SCP messages that the instance sent out last
    void persistSCPState(uint64 slot);
    // restores SCP state based on the last messages saved on disk
    void restoreSCPState();

    // saves upgrade parameters
    void persistUpgrades();
    void restoreUpgrades();

    // called every time we get ledger externalized
    // ensures that if we don't hear from the network, we throw the herder into
    // indeterminate mode
    void trackingHeartBeat();

    VirtualTimer mTriggerTimer;

    VirtualTimer mRebroadcastTimer;

    Application& mApp;
    LedgerManager& mLedgerManager;

    struct SCPMetrics
    {
        medida::Meter& mLostSync;

        medida::Meter& mEnvelopeEmit;
        medida::Meter& mEnvelopeReceive;

        // Counters for things reached-through the
        // SCP maps: Slots and Nodes
        medida::Counter& mCumulativeStatements;

        SCPMetrics(Application& app);
    };

    SCPMetrics mSCPMetrics;
};
}
