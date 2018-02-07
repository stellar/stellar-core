#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "PendingEnvelopes.h"
#include "herder/Herder.h"
#include "herder/HerderSCPDriver.h"
#include "herder/Upgrades.h"
#include "util/Timer.h"
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

using xdr::operator<;
using xdr::operator==;

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

    TransactionSubmitStatus recvTransaction(TransactionFramePtr tx) override;

    EnvelopeStatus recvSCPEnvelope(SCPEnvelope const& envelope) override;
    EnvelopeStatus recvSCPEnvelope(SCPEnvelope const& envelope,
                                   const SCPQuorumSet& qset,
                                   TxSetFrame txset) override;

    void sendSCPStateToPeer(uint32 ledgerSeq, PeerPtr peer) override;

    bool recvSCPQuorumSet(Hash const& hash, const SCPQuorumSet& qset) override;
    bool recvTxSet(Hash const& hash, const TxSetFrame& txset) override;
    void peerDoesntHave(MessageType type, uint256 const& itemID,
                        PeerPtr peer) override;
    TxSetFramePtr getTxSet(Hash const& hash) override;
    SCPQuorumSetPtr getQSet(Hash const& qSetHash) override;

    void processSCPQueue();

    uint32_t getCurrentLedgerSeq() const override;

    SequenceNumber getMaxSeqInPendingTxs(AccountID const&) override;

    void triggerNextLedger(uint32_t ledgerSeqToTrigger) override;

    void setUpgrades(Upgrades::UpgradeParameters const& upgrades) override;
    std::string getUpgradesJson() override;

    bool resolveNodeID(std::string const& s, PublicKey& retKey) override;

    void dumpInfo(Json::Value& ret, size_t limit) override;
    void dumpQuorumInfo(Json::Value& ret, NodeID const& id, bool summary,
                        uint64 index) override;

    struct TxMap
    {
        SequenceNumber mMaxSeq{0};
        int64_t mTotalFees{0};
        std::unordered_map<Hash, TransactionFramePtr> mTransactions;
        void addTx(TransactionFramePtr);
        void recalculate();
    };
    typedef std::unordered_map<AccountID, std::shared_ptr<TxMap>> AccountTxMap;

  private:
    void ledgerClosed();
    void removeReceivedTxs(std::vector<TransactionFramePtr> const& txs);

    void startRebroadcastTimer();
    void rebroadcast();
    void broadcast(SCPEnvelope const& e);

    void updateSCPCounters();

    void processSCPQueueUpToIndex(uint64 slotIndex);

    // 0- tx we got during ledger close
    // 1- one ledger ago. rebroadcast
    // 2- two ledgers ago. rebroadcast
    // ...
    std::deque<AccountTxMap> mPendingTransactions;

    void
    updatePendingTransactions(std::vector<TransactionFramePtr> const& applied);

    PendingEnvelopes mPendingEnvelopes;
    Upgrades mUpgrades;
    HerderSCPDriver mHerderSCPDriver;

    void herderOutOfSync();

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

    VirtualClock::time_point mLastTrigger;
    VirtualTimer mTriggerTimer;

    VirtualTimer mRebroadcastTimer;

    Application& mApp;
    LedgerManager& mLedgerManager;

    struct SCPMetrics
    {
        medida::Meter& mLostSync;

        medida::Meter& mBallotExpire;

        medida::Meter& mEnvelopeEmit;
        medida::Meter& mEnvelopeReceive;

        // Counters for stuff in parent class (SCP)
        // that we monitor on a best-effort basis from
        // here.
        medida::Counter& mKnownSlotsSize;

        // Counters for things reached-through the
        // SCP maps: Slots and Nodes
        medida::Counter& mCumulativeStatements;

        // Pending tx buffer sizes
        medida::Counter& mHerderPendingTxs0;
        medida::Counter& mHerderPendingTxs1;
        medida::Counter& mHerderPendingTxs2;
        medida::Counter& mHerderPendingTxs3;

        SCPMetrics(Application& app);
    };

    SCPMetrics mSCPMetrics;
};
}
