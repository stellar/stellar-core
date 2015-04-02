#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <vector>
#include "herder/Herder.h"
#include "overlay/ItemFetcher.h"
#include "scp/SCP.h"
#include "util/Timer.h"

// Expected time between two ledger close.
#define EXP_LEDGER_TIMESPAN_SECONDS 4

// Maximum timeout for SCP consensus.
#define MAX_SCP_TIMEOUT_SECONDS 240

// Maximum time slip between nodes.
#define MAX_TIME_SLIP_SECONDS 60

// How many seconds of inactivity before evicting a node.
#define NODE_EXPIRATION_SECONDS 240

// How many ledger in past/future we consider an envelope viable.
#define LEDGER_VALIDITY_BRACKET 1000

namespace medida
{
class Meter;
class Counter;
}

namespace stellar
{
class Application;

using xdr::operator<;
using xdr::operator==;

/*
 * Drives the SCP protocol (is an SCP::Client). It is also in charge of
 * receiving transactions from the network.
 */
class HerderImpl : public Herder, public SCP
{
  public:
    HerderImpl(Application& app);
    ~HerderImpl();

    // Bootstraps the HerderImpl if we're creating a new Network
    void bootstrap() override;

    // SCP methods
    void validateValue(uint64 const& slotIndex, uint256 const& nodeID,
                       Value const& value,
                       std::function<void(bool)> const& cb) override;
    int compareValues(uint64 const& slotIndex, uint32 const& ballotCounter,
                      Value const& v1, Value const& v2) override;

    void validateBallot(uint64 const& slotIndex, uint256 const& nodeID,
                        SCPBallot const& ballot,
                        std::function<void(bool)> const& cb) override;

    void ballotDidHearFromQuorum(uint64 const& slotIndex,
                                 SCPBallot const& ballot) override;
    void valueExternalized(uint64 const& slotIndex,
                           Value const& value) override;

    void nodeTouched(uint256 const& nodeID) override;

    void retrieveQuorumSet(
        uint256 const& nodeID, Hash const& qSetHash,
        std::function<void(SCPQuorumSet const&)> const& cb) override;
    void emitEnvelope(SCPEnvelope const& envelope) override;

    // Extra SCP methods overridden solely to increment metrics.
    void ballotDidPrepare(uint64 const& slotIndex,
                          SCPBallot const& ballot) override;
    void ballotDidPrepared(uint64 const& slotIndex,
                           SCPBallot const& ballot) override;
    void ballotDidCommit(uint64 const& slotIndex,
                         SCPBallot const& ballot) override;
    void ballotDidCommitted(uint64 const& slotIndex,
                            SCPBallot const& ballot) override;
    void envelopeSigned() override;
    void envelopeVerified(bool) override;

    // Herder methods
    TxSetFramePtr fetchTxSet(uint256 const& txSetHash,
                             bool askNetwork) override;
    void recvTxSet(TxSetFramePtr txSet) override;
    void doesntHaveTxSet(uint256 const& txSethash, PeerPtr peer) override;

    SCPQuorumSetPtr fetchSCPQuorumSet(uint256 const& qSetHash,
                                      bool askNetwork) override;
    void recvSCPQuorumSet(SCPQuorumSetPtr qSet) override;
    void doesntHaveSCPQuorumSet(uint256 const& qSetHash, PeerPtr peer) override;

    // returns whether the transaction should be flooded
    bool recvTransaction(TransactionFramePtr tx) override;

    void recvSCPEnvelope(SCPEnvelope envelope,
                         std::function<void(EnvelopeState)> const& cb = [](bool)
                         {
                         }) override;

    void ledgerClosed(LedgerHeaderHistoryEntry const& ledger) override;

    void triggerNextLedger() override;

    void dumpInfo(Json::Value& ret) override;

  private:
    void removeReceivedTx(TransactionFramePtr tx);
    void expireBallot(uint64 const& slotIndex, SCPBallot const& ballot);

    void startRebroadcastTimer();
    void rebroadcast();

    // StellarBallot internal signature/verification
    void signStellarBallot(StellarBallot& b);
    bool verifyStellarBallot(StellarBallot const& b);

    void updateSCPCounters();

    // 0- tx we got during ledger close
    // 1- one ledger ago. rebroadcast
    // 2- two ledgers ago.
    std::vector<std::vector<TransactionFramePtr>> mReceivedTransactions;

    // Time of last access to a node, used to evict unused nodes.
    std::map<uint256, VirtualClock::time_point> mNodeLastAccess;

    // need to keep the old tx sets around for at least one Consensus round in
    // case some stragglers are still need the old txsets in order to close
    std::array<TxSetFetcher, 2> mTxSetFetcher;
    int mCurrentTxSetFetcher;
    std::map<Hash, std::vector<std::function<void(TxSetFramePtr)>>>
        mTxSetFetches;

    SCPQSetFetcher mSCPQSetFetcher;
    std::map<Hash, std::vector<std::function<void(SCPQuorumSetPtr)>>>
        mSCPQSetFetches;

    std::map<
        uint64,
        std::vector<std::pair<SCPEnvelope, std::function<void(EnvelopeState)>>>>
        mFutureEnvelopes;

    std::map<SCPBallot,
             std::map<uint256, std::vector<std::shared_ptr<VirtualTimer>>>>
        mBallotValidationTimers;

    LedgerHeaderHistoryEntry mLastClosedLedger;

    VirtualClock::time_point mLastTrigger;
    VirtualTimer mTriggerTimer;

    VirtualTimer mBumpTimer;
    VirtualTimer mRebroadcastTimer;
    Value mCurrentValue;
    StellarMessage mLastSentMessage;

    Application& mApp;

    medida::Meter& mValueValid;
    medida::Meter& mValueInvalid;
    medida::Meter& mValuePrepare;
    medida::Meter& mValueExternalize;

    medida::Meter& mBallotValid;
    medida::Meter& mBallotInvalid;
    medida::Meter& mBallotPrepare;
    medida::Meter& mBallotPrepared;
    medida::Meter& mBallotCommit;
    medida::Meter& mBallotCommitted;
    medida::Meter& mBallotSign;
    medida::Meter& mBallotValidSig;
    medida::Meter& mBallotInvalidSig;
    medida::Meter& mBallotExpire;

    medida::Meter& mQuorumHeard;
    medida::Meter& mQsetRetrieve;

    medida::Meter& mEnvelopeEmit;
    medida::Meter& mEnvelopeReceive;
    medida::Meter& mEnvelopeSign;
    medida::Meter& mEnvelopeValidSig;
    medida::Meter& mEnvelopeInvalidSig;

    medida::Counter& mNodeLastAccessSize;
    medida::Counter& mSCPQSetFetchesSize;
    medida::Counter& mFutureEnvelopesSize;
    medida::Counter& mBallotValidationTimersSize;

    // Counters for stuff in parent class (SCP)
    // that we monitor on a best-effort basis from
    // here.
    medida::Counter& mKnownNodesSize;
    medida::Counter& mKnownSlotsSize;

    // Counters for things reached-through the
    // SCP maps: Slots and Nodes
    medida::Counter& mCumulativeStatements;
    medida::Counter& mCumulativeCachedQuorumSets;
};
}
