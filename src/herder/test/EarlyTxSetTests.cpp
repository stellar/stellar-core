// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// Tests of the selection of the next transaction set before the current
// ledger has closed (herder/EarlyTxSetSelection.h), of its completion on the
// closed ledger (TxSetBuilder::completeSelection, HerderImpl::buildTxSet), and
// of the pull of a build (capacity plus a margin, refilled when validation
// leaves the set short).
//
// Two properties are what these tests are for:
//  a. whatever the overlap between ledger N and the candidates of N+1, the
//     set a node proposes for N+1 passes the validation every node applies
//     to it, against N as it really closed;
//  b. the set is as full as the mempool allows.

#include "crypto/SHA.h"
#include "herder/EarlyTxSetSelection.h"
#include "herder/Herder.h"
#include "herder/HerderImpl.h"
#include "herder/HerderUtils.h"
#include "herder/LedgerCloseData.h"
#include "herder/ParallelTxSetBuilder.h"
#include "herder/TxSetFrame.h"
#include "herder/Upgrades.h"
#include "herder/test/TestTxSetUtils.h"
#include "ledger/ImmutableLedgerView.h"
#include "ledger/LedgerManager.h"
#include "ledger/test/LedgerTestUtils.h"
#include "main/Application.h"
#include "medida/histogram.h"
#include "medida/meter.h"
#include "simulation/ApplyLoad.h"
#include "simulation/Simulation.h"
#include "simulation/Topologies.h"
#include "test/Catch2.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/SignatureUtils.h"
#include "transactions/TransactionUtils.h"
#include "util/BatchExecutor.h"
#include "util/Math.h"
#include "util/MetricsRegistry.h"
#include "util/ProtocolVersion.h"
#include "util/XDROperators.h"
#include <chrono>
#include <fmt/format.h>
#include <iostream>
#include <map>
#include <numeric>
#include <random>
#include <set>
#include <thread>

using namespace stellar;
using namespace stellar::txtest;

namespace stellar
{
class EarlyTxSetTestAccess
{
  public:
    // What the herder does with a statement this node emits.
    static void
    voted(HerderImpl& herder, SCPEnvelope const& envelope)
    {
        herder.maybeScheduleEarlyTxSetSelection(envelope);
    }

    static bool
    hasPlan(HerderImpl& herder)
    {
        return herder.mEarlyTxSetPlan.has_value();
    }

    static bool
    planned(HerderImpl& herder)
    {
        return herder.mEarlyTxSetPlan && herder.mEarlyTxSetPlan->planned;
    }

    static std::shared_ptr<EarlyTxSetJob>
    job(HerderImpl& herder)
    {
        return herder.mEarlyTxSet;
    }

    static auto
    build(HerderImpl& herder, uint32_t ledgerSeq, ConsensusTime closeTime)
    {
        return herder.buildTxSet(ledgerSeq, closeTime, false);
    }

    static size_t
    pullCount(HerderImpl& herder)
    {
        return herder.txSetPullCount();
    }

    static void
    setStartDelay(HerderImpl& herder,
                  std::optional<std::chrono::milliseconds> delay)
    {
        herder.mEarlyTxSetStartDelayForTesting = delay;
    }

    static std::chrono::milliseconds
    startDelay(HerderImpl& herder)
    {
        return herder.earlyTxSetStartDelay();
    }

    static void
    setTimings(HerderImpl& herder, std::chrono::milliseconds voteToBuild,
               std::chrono::milliseconds lastVoteToBuild,
               std::chrono::milliseconds job, std::chrono::milliseconds lastJob)
    {
        herder.mVoteToBuild = voteToBuild;
        herder.mLastVoteToBuild = lastVoteToBuild;
        herder.mEarlyTxSetJobDuration = job;
        herder.mLastEarlyTxSetJobDuration = lastJob;
    }

    // What the node takes a selection job to last (running average, last).
    static std::pair<std::chrono::milliseconds, std::chrono::milliseconds>
    jobDuration(HerderImpl& herder)
    {
        return {herder.mEarlyTxSetJobDuration,
                herder.mLastEarlyTxSetJobDuration};
    }

    static void
    skipFinalPass(HerderImpl& herder, bool skip)
    {
        herder.mSkipEarlyTxSetFinalPassForTesting = skip;
    }

    static void
    hold(HerderImpl& herder, std::shared_ptr<std::atomic<bool>> hold)
    {
        herder.mEarlyTxSetHoldForTesting = std::move(hold);
    }

    static bool
    selfCheckLatched(HerderImpl& herder)
    {
        return herder.mTxSetSelfCheckFailed;
    }
};
}

namespace
{
using Access = EarlyTxSetTestAccess;

uint32_t const GATE_PROTOCOL =
    static_cast<uint32_t>(APPLY_TIME_TX_SET_VALIDATION_PROTOCOL_VERSION);

int64_t
txsetMeter(Application& app, std::string const& name,
           std::string const& unit = "txset")
{
    return static_cast<int64_t>(
        app.getMetrics().NewMeter({"herder", "txset", name}, unit).count());
}

int64_t
applyCounter(Application& app, std::string const& name)
{
    return app.getMetrics().NewCounter({"ledger", "apply", name}).count();
}

std::set<Hash>
hashesOf(std::vector<TransactionFrameBasePtr> const& txs)
{
    std::set<Hash> res;
    for (auto const& tx : txs)
    {
        res.insert(tx->getFullHash());
    }
    return res;
}

std::set<Hash>
hashesOf(ApplicableTxSetFrame const& txSet)
{
    std::set<Hash> res;
    for (auto const& phase : txSet.getPhases())
    {
        for (auto const& tx : phase)
        {
            res.insert(tx->getFullHash());
        }
    }
    return res;
}

// What a node got when it asked for its next set.
struct Built
{
    TxSetXDRFrameConstPtr mTxSet;
    ApplicableTxSetFrameConstPtr mApplicable;
    // The set is a completed early selection, not a build.
    bool mEarly{false};
    std::vector<Hash> mInvalid;

    std::set<Hash>
    hashes() const
    {
        return hashesOf(*mApplicable);
    }

    bool
    holds(TransactionFrameBasePtr const& tx) const
    {
        return hashes().count(tx->getFullHash()) != 0;
    }
};

// A single validator (so it leads every ledger) with funded accounts. Its
// mempool is `mMempool`, pulled in that order; the test plays the network:
// it has the node vote for a set, closes the ledger with a set, and asks the
// node for its next set.
struct EarlyFixture
{
    VirtualClock mClock;
    Config mCfg;
    Application::pointer mApp;
    std::vector<TestAccount> mAccounts;
    std::vector<TransactionFrameBasePtr> mMempool;
    // What each pull of the mempool asked for.
    std::vector<size_t> mPulls;

    explicit EarlyFixture(size_t accounts = 8,
                          std::optional<uint32_t> maxTxSetSize = std::nullopt,
                          std::function<void(Config&)> tweak = nullptr)
        : mCfg(getTestConfig(0, Config::TESTDB_BUCKET_DB_PERSISTENT))
    {
        mCfg.LEDGER_PROTOCOL_VERSION = GATE_PROTOCOL;
        mCfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = GATE_PROTOCOL;
        if (maxTxSetSize)
        {
            mCfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = *maxTxSetSize;
        }
        // The tests close ledgers themselves.
        mCfg.EXPERIMENTAL_SPECULATIVE_LEDGER_APPLY = false;
        if (tweak)
        {
            tweak(mCfg);
        }
        REQUIRE(mCfg.backgroundLedgerApply());
        mApp = createTestApplication(mClock, mCfg);
        auto root = mApp->getRoot();
        auto const balance =
            mApp->getLedgerManager().getLastMinBalance(2) + 1'000'000'000;
        for (size_t i = 0; i < accounts; ++i)
        {
            mAccounts.emplace_back(
                root->create(fmt::format("account-{}", i), balance));
        }
        // An early selection starts as soon as the node has voted.
        Access::setStartDelay(herder(), std::chrono::milliseconds(0));
        herder().mGetTopTransactionsForTesting = [this](size_t count) {
            mPulls.push_back(count);
            std::vector<TransactionEnvelope> envelopes;
            for (size_t i = 0; i < mMempool.size() && i < count; ++i)
            {
                envelopes.push_back(mMempool[i]->getEnvelope());
            }
            return envelopes;
        };
    }

    ~EarlyFixture()
    {
        herder().mGetTopTransactionsForTesting = nullptr;
    }

    HerderImpl&
    herder()
    {
        return static_cast<HerderImpl&>(mApp->getHerder());
    }

    LedgerManager&
    lm()
    {
        return mApp->getLedgerManager();
    }

    TestAccount&
    acc(size_t i)
    {
        return mAccounts.at(i);
    }

    uint32_t
    nextSeq()
    {
        return lm().getLastClosedLedgerNum() + 1;
    }

    ConsensusTime
    nextCloseTime()
    {
        return ConsensusTime::fromApplyTime(ApplyTime::fromTimePoint(
            lm().getLastClosedLedgerHeader().header.scpValue.closeTime + 1));
    }

    // The sequence number and balance of an account on the LCL.
    SequenceNumber
    seqOf(TestAccount& account)
    {
        auto entry = lm().getLCLView()->getAccount(account.getPublicKey());
        REQUIRE(entry);
        return entry.current().data.account().seqNum;
    }

    bool
    exists(TestAccount& account)
    {
        return static_cast<bool>(
            lm().getLCLView()->getAccount(account.getPublicKey()));
    }

    // A payment of `account` to the root account with the `n`-th sequence
    // number after the account's current one.
    TransactionFrameBasePtr
    pay(TestAccount& account, int n, int64_t amount = 1, uint32_t fee = 100)
    {
        return transactionFromOperations(
            *mApp, account.getSecretKey(), seqOf(account) + n,
            {payment(mApp->getRoot()->getPublicKey(), amount)}, fee);
    }

    TransactionFrameBasePtr
    tx(TestAccount& account, int n, std::vector<Operation> const& ops,
       uint32_t fee = 100)
    {
        return transactionFromOperations(
            *mApp, account.getSecretKey(), seqOf(account) + n, ops,
            fee * static_cast<uint32_t>(ops.size()));
    }

    // A Soroban transaction of `account` that conflicts with no other
    // (`seed` picks the entry it writes).
    TransactionFrameBasePtr
    upload(TestAccount& account, int n, uint64_t seed,
           uint32_t inclusionFee = 100, uint32_t instructions = 1'000'000)
    {
        SorobanResources resources;
        resources.instructions = instructions;
        resources.diskReadBytes = 1000;
        resources.writeBytes = 2000;
        return createUploadWasmTx(*mApp, account, inclusionFee,
                                  DEFAULT_TEST_RESOURCE_FEE, resources,
                                  std::nullopt, 0, std::nullopt,
                                  seqOf(account) + n, std::make_optional(seed));
    }

    // A set of classic transactions on top of the LCL, not validated.
    TxSetXDRFrameConstPtr
    setOf(std::vector<TransactionFrameBasePtr> const& txs)
    {
        auto res = testtxset::makeNonValidatedGeneralizedTxSet(
            {{std::make_pair(std::nullopt, txs)}, {}}, *mApp,
            lm().getLastClosedLedgerHeader().hash);
        REQUIRE(res.first);
        return res.first;
    }

    StellarValue
    valueFor(TxSetXDRFrameConstPtr const& txSet,
             xdr::xvector<UpgradeType, 6> const& upgrades = emptyUpgradeSteps,
             std::optional<ConsensusTime> closeTime = std::nullopt)
    {
        return herder().makeStellarValue(
            txSet->getContentsHash(), closeTime ? *closeTime : nextCloseTime(),
            upgrades, mCfg.NODE_SEED);
    }

    // Cranks until `done`, giving background threads time to post to the
    // main thread.
    template <typename F>
    void
    crankUntil(F&& done)
    {
        auto const deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!done())
        {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            mClock.crank(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void
    crankFor(std::chrono::milliseconds duration)
    {
        auto const deadline = std::chrono::steady_clock::now() + duration;
        while (std::chrono::steady_clock::now() < deadline)
        {
            mClock.crank(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // The node emits a vote for `closing` as the next ledger.
    void
    vote(TxSetXDRFrameConstPtr const& closing,
         xdr::xvector<UpgradeType, 6> const& upgrades = emptyUpgradeSteps)
    {
        // By the time a node votes, SCP has asked for the slot's leaders,
        // which brings the tracked quorum (the node set) up to date.
        REQUIRE(herder().getHerderSCPDriver().getLeaderSchedule(nextSeq()) ==
                std::vector<NodeID>{mCfg.NODE_SEED.getPublicKey()});
        herder().getPendingEnvelopes().putTxSet(closing->getContentsHash(),
                                                nextSeq(), closing);
        SCPEnvelope envelope;
        envelope.statement.slotIndex = nextSeq();
        envelope.statement.pledges.type(SCP_ST_PREPARE);
        envelope.statement.pledges.prepare().ballot.counter = 1;
        envelope.statement.pledges.prepare().ballot.value =
            xdr::xdr_to_opaque(valueFor(closing, upgrades));
        Access::voted(herder(), envelope);
    }

    // Waits until the selection the vote planned has started.
    std::shared_ptr<EarlyTxSetJob>
    waitStarted()
    {
        REQUIRE(Access::planned(herder()));
        crankUntil([&]() { return Access::job(herder()) != nullptr; });
        return Access::job(herder());
    }

    // Waits until it has finished; returns whether it selected a set.
    bool
    waitSelected()
    {
        auto job = waitStarted();
        crankUntil([&]() { return job->mDone.load(); });
        std::lock_guard<std::mutex> lock(job->mMutex);
        return job->mSelection != nullptr;
    }

    // The node votes for `closing` and selects the set of the ledger after.
    void
    voteAndSelect(TxSetXDRFrameConstPtr const& closing)
    {
        vote(closing);
        REQUIRE(waitSelected());
    }

    // The next ledger closes with `closing`. The overlay drops a closed
    // set's transactions from the mempool; with `keepInMempool` it has not
    // got to it yet.
    void
    close(TxSetXDRFrameConstPtr const& closing, bool keepInMempool = false,
          xdr::xvector<UpgradeType, 6> const& upgrades = emptyUpgradeSteps,
          std::optional<ConsensusTime> closeTime = std::nullopt)
    {
        auto const seq = nextSeq();
        mApp->getHerder().externalizeValue(
            closing, seq, closeTime ? *closeTime : nextCloseTime(), upgrades);
        REQUIRE(lm().getLastClosedLedgerNum() == seq);
        if (!keepInMempool)
        {
            std::set<Hash> closed;
            for (auto const& hash : closing->getTransactionFullHashes())
            {
                closed.insert(hash);
            }
            std::vector<TransactionFrameBasePtr> left;
            for (auto const& tx : mMempool)
            {
                if (closed.count(tx->getFullHash()) == 0)
                {
                    left.push_back(tx);
                }
            }
            mMempool = std::move(left);
        }
    }

    // The node's set for the next ledger.
    Built
    build()
    {
        auto const completed = txsetMeter(*mApp, "early-completed");
        auto prepared = Access::build(herder(), nextSeq(), nextCloseTime());
        Built built;
        built.mTxSet = prepared.txSet;
        built.mApplicable = std::move(prepared.applicableTxSet);
        built.mInvalid = prepared.invalidTxHashes;
        built.mEarly = txsetMeter(*mApp, "early-completed") == completed + 1;
        REQUIRE(built.mTxSet);
        REQUIRE(built.mApplicable);
        return built;
    }

    // Property a: the set passes the validation every node applies to a
    // set, in full (structure and every transaction), on the LCL.
    TxSetValidationResult
    validate(Built const& built)
    {
        // As a node that receives the set: read from its bytes. (build()
        // asks for a close time one second after the LCL's.)
        auto received = TxSetXDRFrame::makeFromWire(
            GeneralizedTransactionSet(built.mTxSet->getGeneralizedXDR()));
        REQUIRE(received->getContentsHash() == built.mTxSet->getContentsHash());
        auto applicable = received->prepareForApply(
            *mApp, lm().getLastClosedLedgerHeader().header);
        REQUIRE(applicable);
        REQUIRE(hashesOf(*applicable) == built.hashes());
        auto const result = applicable->checkValidWithResult(*mApp, 1, 1);
        // The set the node keeps says the same.
        REQUIRE(built.mApplicable->checkValidWithResult(*mApp, 1, 1) == result);
        return result;
    }

    // The set a build from scratch selects out of `txs` on the LCL.
    Built
    buildFrom(std::vector<TransactionFrameBasePtr> const& txs)
    {
        auto mempool = mMempool;
        mMempool = txs;
        auto built = build();
        mMempool = std::move(mempool);
        REQUIRE(!built.mEarly);
        return built;
    }

    // Closes the next ledger with a set this node proposed and checks that
    // it was applied: the set was not skipped as invalid, and every one of
    // its transactions consumed its sequence number.
    void
    closeAndCheckApplied(Built const& built)
    {
        REQUIRE(validate(built) == TxSetValidationResult::VALID);
        auto const skipped = applyCounter(*mApp, "txset-skipped");
        std::vector<std::pair<AccountID, SequenceNumber>> expected;
        for (auto const& phase : built.mApplicable->getPhases())
        {
            for (auto const& tx : phase)
            {
                expected.emplace_back(tx->getSourceID(), tx->getSeqNum());
            }
        }
        close(built.mTxSet);
        REQUIRE(applyCounter(*mApp, "txset-skipped") == skipped);
        auto view = lm().getLCLView();
        for (auto const& [account, seq] : expected)
        {
            auto entry = view->getAccount(account);
            // (an account that merged itself away is gone)
            if (entry)
            {
                REQUIRE(entry.current().data.account().seqNum >= seq);
            }
        }
    }
};

}

// ---------------------------------------------------------------------------
// The mechanism.
// ---------------------------------------------------------------------------

TEST_CASE("the next tx set is selected while the current ledger is in its "
          "ballot",
          "[herder][txset][early-txset]")
{
    EarlyFixture f;
    auto& app = *f.mApp;
    auto& herder = f.herder();
    auto closingA = f.pay(f.acc(0), 1);
    auto nextA = f.pay(f.acc(0), 2);
    auto plainB = f.pay(f.acc(1), 1);
    auto closing = f.setOf({closingA});
    // The closing ledger's transaction is still in the mempool.
    f.mMempool = {closingA, nextA, plainB};

    auto const planned = txsetMeter(app, "early-planned");
    auto const started = txsetMeter(app, "early-started");
    f.vote(closing);
    REQUIRE(Access::planned(herder));
    REQUIRE(txsetMeter(app, "early-planned") == planned + 1);
    // Nothing is pulled before the selection starts.
    REQUIRE(f.mPulls.empty());
    REQUIRE(f.waitSelected());
    REQUIRE(txsetMeter(app, "early-started") == started + 1);
    // One pull: the capacity of a set plus the margin, plus the closing
    // ledger's transactions.
    REQUIRE(f.mPulls == std::vector<size_t>{Access::pullCount(herder) + 1});

    {
        auto job = Access::job(herder);
        std::lock_guard<std::mutex> lock(job->mMutex);
        auto const& selection = *job->mSelection;
        REQUIRE(selection.mLedgerSeq == f.nextSeq() + 1);
        std::set<Hash> selected;
        for (auto const& phase : selection.mPhases)
        {
            for (auto const& tx : phase)
            {
                selected.insert(tx->getFullHash());
            }
        }
        // A's next payment is only valid once the closing ledger has
        // applied: the selection is made on what that ledger will do.
        REQUIRE(selected == hashesOf({nextA, plainB}));
        REQUIRE(selection.mLeftovers.empty());
    }

    SECTION("the ledger closes with the value: the selection is completed")
    {
        f.close(closing);
        f.mPulls.clear();
        auto const built = f.build();
        REQUIRE(built.mEarly);
        REQUIRE(built.hashes() == hashesOf({nextA, plainB}));
        REQUIRE(f.validate(built) == TxSetValidationResult::VALID);
        REQUIRE(built.mTxSet->previousLedgerHash() ==
                f.lm().getLastClosedLedgerHeader().hash);
        REQUIRE(built.mInvalid.empty());
        // The set had room: the mempool was asked once for what arrived
        // since (nothing).
        REQUIRE(f.mPulls == std::vector<size_t>{Access::pullCount(herder)});
        // The same set a build from scratch selects.
        REQUIRE(f.buildFrom({nextA, plainB}).hashes() == built.hashes());
        f.closeAndCheckApplied(built);
        // The plan is used up.
        REQUIRE(!Access::hasPlan(herder));
        REQUIRE(!Access::job(herder));
    }
    SECTION("a second build on the same ledger is a build")
    {
        f.close(closing);
        REQUIRE(f.build().mEarly);
        auto const again = f.build();
        REQUIRE(!again.mEarly);
        REQUIRE(again.hashes() == hashesOf({nextA, plainB}));
    }
    SECTION("statements for the same value change nothing")
    {
        auto job = Access::job(herder);
        f.vote(closing);
        REQUIRE(Access::job(herder) == job);
        REQUIRE(txsetMeter(app, "early-planned") == planned + 1);
    }
}

TEST_CASE("when a tx set is selected early", "[herder][txset][early-txset]")
{
    SECTION("not when the setting is off")
    {
        EarlyFixture f(2, std::nullopt, [](Config& cfg) {
            cfg.EXPERIMENTAL_EARLY_TX_SET_SELECTION = false;
        });
        auto closingA = f.pay(f.acc(0), 1);
        auto nextA = f.pay(f.acc(0), 2);
        f.mMempool = {closingA, nextA};
        auto closing = f.setOf({closingA});
        f.vote(closing);
        REQUIRE(!Access::hasPlan(f.herder()));
        f.close(closing);
        auto const built = f.build();
        REQUIRE(!built.mEarly);
        REQUIRE(built.hashes() == hashesOf({nextA}));
    }
    SECTION("not for a value with upgrades")
    {
        EarlyFixture f(2);
        auto closingA = f.pay(f.acc(0), 1);
        auto nextA = f.pay(f.acc(0), 2);
        f.mMempool = {closingA, nextA};
        auto closing = f.setOf({closingA});
        xdr::xvector<UpgradeType, 6> upgrades;
        LedgerUpgrade upgrade(LEDGER_UPGRADE_BASE_FEE);
        upgrade.newBaseFee() = 150;
        upgrades.emplace_back(LedgerTestUtils::toUpgradeType(upgrade));
        f.vote(closing, upgrades);
        // Looked at, and nothing planned.
        REQUIRE(Access::hasPlan(f.herder()));
        REQUIRE(!Access::planned(f.herder()));
        f.crankFor(std::chrono::milliseconds(20));
        REQUIRE(!Access::job(f.herder()));
        REQUIRE(f.mPulls.empty());
    }
    SECTION("not for an empty set")
    {
        EarlyFixture f(2);
        f.mMempool = {f.pay(f.acc(0), 1)};
        auto empty =
            TxSetXDRFrame::makeEmpty(f.lm().getLastClosedLedgerHeader());
        f.vote(empty);
        REQUIRE(!Access::planned(f.herder()));
    }
    SECTION("not for a ledger that is not the next one")
    {
        EarlyFixture f(2);
        auto closingA = f.pay(f.acc(0), 1);
        auto closing = f.setOf({closingA});
        SCPEnvelope envelope;
        envelope.statement.slotIndex = f.nextSeq() + 1;
        envelope.statement.pledges.type(SCP_ST_PREPARE);
        envelope.statement.pledges.prepare().ballot.counter = 1;
        envelope.statement.pledges.prepare().ballot.value =
            xdr::xdr_to_opaque(f.valueFor(closing));
        Access::voted(f.herder(), envelope);
        REQUIRE(!Access::hasPlan(f.herder()));
    }
    SECTION("the start is timed to end shortly before the set is needed")
    {
        using std::chrono::milliseconds;
        EarlyFixture f(1);
        auto& herder = f.herder();
        Access::setStartDelay(herder, std::nullopt);
        // Nothing measured: at once.
        Access::setTimings(herder, milliseconds(0), milliseconds(0),
                           milliseconds(0), milliseconds(0));
        REQUIRE(Access::startDelay(herder) == milliseconds(0));
        Access::setTimings(herder, milliseconds(800), milliseconds(800),
                           milliseconds(0), milliseconds(0));
        REQUIRE(Access::startDelay(herder) == milliseconds(0));
        // 800 ms from the vote to the build, a selection takes 200 ms:
        // 800 - 200 - 50 - 30.
        Access::setTimings(herder, milliseconds(800), milliseconds(800),
                           milliseconds(200), milliseconds(200));
        REQUIRE(Access::startDelay(herder) == milliseconds(520));
        // The earlier of the two build times, the longer of the two jobs.
        Access::setTimings(herder, milliseconds(800), milliseconds(700),
                           milliseconds(200), milliseconds(240));
        REQUIRE(Access::startDelay(herder) ==
                milliseconds(700 - 240 - 60 - 30));
        // Never negative.
        Access::setTimings(herder, milliseconds(100), milliseconds(100),
                           milliseconds(200), milliseconds(200));
        REQUIRE(Access::startDelay(herder) == milliseconds(0));
    }
    SECTION("a selection that has not started when the ledger closes")
    {
        EarlyFixture f(2);
        auto& app = *f.mApp;
        auto closingA = f.pay(f.acc(0), 1);
        auto nextA = f.pay(f.acc(0), 2);
        f.mMempool = {closingA, nextA};
        auto closing = f.setOf({closingA});
        // The start is due long after the close.
        Access::setStartDelay(f.herder(), std::chrono::milliseconds(60'000));
        f.vote(closing);
        REQUIRE(Access::planned(f.herder()));
        f.close(closing);
        auto const notStarted = txsetMeter(app, "early-not-started");
        auto const built = f.build();
        REQUIRE(!built.mEarly);
        REQUIRE(txsetMeter(app, "early-not-started") == notStarted + 1);
        REQUIRE(built.hashes() == hashesOf({nextA}));
        REQUIRE(f.validate(built) == TxSetValidationResult::VALID);
    }
}

// ---------------------------------------------------------------------------
// Property a: no invalid set, whatever the overlap between N and N+1.
// ---------------------------------------------------------------------------

TEST_CASE("a set selected early is valid on the closed ledger whatever the "
          "previous set held",
          "[herder][txset][early-txset]")
{
    EarlyFixture f(8);
    auto& herder = f.herder();
    auto& a = f.acc(0);
    auto& b = f.acc(1);
    auto& c = f.acc(2);
    auto& d = f.acc(3);

    SECTION("the candidates are exactly the previous set's transactions")
    {
        std::vector<TransactionFrameBasePtr> txs{f.pay(a, 1), f.pay(b, 1),
                                                 f.pay(c, 1)};
        auto closing = f.setOf(txs);
        f.mMempool = txs;
        f.voteAndSelect(closing);
        SECTION("the overlay has dropped them at the close")
        {
            f.close(closing);
        }
        SECTION("the overlay still holds them at the close")
        {
            f.close(closing, /*keepInMempool=*/true);
        }
        auto const built = f.build();
        REQUIRE(built.mEarly);
        REQUIRE(built.hashes().empty());
        REQUIRE(f.validate(built) == TxSetValidationResult::VALID);
    }
    SECTION("the same accounts with their next sequence numbers")
    {
        std::vector<TransactionFrameBasePtr> first{f.pay(a, 1), f.pay(b, 1),
                                                   f.pay(c, 1)};
        std::vector<TransactionFrameBasePtr> second{f.pay(a, 2), f.pay(b, 2),
                                                    f.pay(c, 2)};
        auto closing = f.setOf(first);
        f.mMempool = first;
        f.mMempool.insert(f.mMempool.end(), second.begin(), second.end());
        f.voteAndSelect(closing);
        f.close(closing, /*keepInMempool=*/true);
        auto const built = f.build();
        REQUIRE(built.mEarly);
        REQUIRE(built.hashes() == hashesOf(second));
        f.closeAndCheckApplied(built);
        REQUIRE(f.seqOf(a) == second[0]->getSeqNum());
    }
    SECTION("a transaction of the previous set fails when it is applied")
    {
        // The payment is more than the account has: the transaction is
        // valid, fails, and consumes its sequence number all the same.
        auto failing = f.pay(a, 1, a.getBalance() * 2);
        auto nextA = f.pay(a, 2);
        auto closing = f.setOf({failing});
        f.mMempool = {failing, nextA};
        f.voteAndSelect(closing);
        auto const balance = a.getBalance();
        f.close(closing);
        // It failed: only the fee is gone.
        REQUIRE(a.getBalance() == balance - 100);
        auto const built = f.build();
        REQUIRE(built.mEarly);
        REQUIRE(built.hashes() == hashesOf({nextA}));
        f.closeAndCheckApplied(built);
    }
    SECTION("another transaction with a sequence number the previous set "
            "used")
    {
        auto closingA = f.pay(a, 1, 1);
        auto sameSeq = f.pay(a, 1, 2);
        auto plainB = f.pay(b, 1);
        auto closing = f.setOf({closingA});
        f.mMempool = {closingA, sameSeq, plainB};
        f.voteAndSelect(closing);
        f.close(closing);
        auto const built = f.build();
        REQUIRE(built.mEarly);
        REQUIRE(built.hashes() == hashesOf({plainB}));
        REQUIRE(f.validate(built) == TxSetValidationResult::VALID);
    }
    SECTION("the previous set drains an account")
    {
        // Everything but the reserve goes: A cannot pay another fee. The
        // selection cannot know (it predicts fees, not payments).
        auto const spare = a.getAvailableBalance() - 100;
        auto drain = f.pay(a, 1, spare);
        auto nextA = f.pay(a, 2);
        auto plainB = f.pay(b, 1);
        auto closing = f.setOf({drain});
        f.mMempool = {drain, nextA, plainB};
        f.voteAndSelect(closing);
        {
            auto job = Access::job(herder);
            std::lock_guard<std::mutex> lock(job->mMutex);
            REQUIRE(job->mSelection->mSelected == 2);
        }
        f.close(closing);
        REQUIRE(a.getAvailableBalance() == 0);
        SECTION("the validity pass removes its next transaction")
        {
            auto const removed = txsetMeter(*f.mApp, "early-removed", "tx");
            auto const built = f.build();
            REQUIRE(built.mEarly);
            REQUIRE(built.hashes() == hashesOf({plainB}));
            REQUIRE(txsetMeter(*f.mApp, "early-removed", "tx") == removed + 1);
            // It could not pay its fee: out of the mempool.
            REQUIRE(built.mInvalid == std::vector<Hash>{nextA->getFullHash()});
            f.closeAndCheckApplied(built);
        }
        SECTION("without the validity pass the set would be invalid")
        {
            Access::skipFinalPass(herder, true);
            auto const built = f.build();
            Access::skipFinalPass(herder, false);
            REQUIRE(built.mEarly);
            REQUIRE(built.holds(nextA));
            REQUIRE(f.validate(built) != TxSetValidationResult::VALID);
        }
    }
    SECTION("the previous set merges an account away")
    {
        auto merge = f.tx(a, 1, {accountMerge(b.getPublicKey())});
        auto nextA = f.pay(a, 2);
        auto plainC = f.pay(c, 1);
        auto closing = f.setOf({merge});
        f.mMempool = {merge, nextA, plainC};
        f.voteAndSelect(closing);
        f.close(closing);
        REQUIRE(!f.exists(a));
        SECTION("the validity pass removes its next transaction")
        {
            auto const built = f.build();
            REQUIRE(built.mEarly);
            REQUIRE(built.hashes() == hashesOf({plainC}));
            f.closeAndCheckApplied(built);
        }
        SECTION("without the validity pass the set would be invalid")
        {
            Access::skipFinalPass(herder, true);
            auto const built = f.build();
            Access::skipFinalPass(herder, false);
            REQUIRE(built.holds(nextA));
            REQUIRE(f.validate(built) != TxSetValidationResult::VALID);
        }
    }
    SECTION("the previous set changes an account's signers")
    {
        // The master key loses its weight to another key: what A signed
        // with it no longer counts.
        auto other = SecretKey::pseudoRandomForTesting();
        auto rekey = f.tx(a, 1,
                          {setOptions(setSigner(makeSigner(other, 1))),
                           setOptions(setMasterWeight(0))});
        auto nextA = f.pay(a, 2);
        auto plainC = f.pay(c, 1);
        auto closing = f.setOf({rekey});
        f.mMempool = {rekey, nextA, plainC};
        f.voteAndSelect(closing);
        f.close(closing);
        REQUIRE(f.seqOf(a) == rekey->getSeqNum());
        SECTION("the validity pass removes what the old key signed")
        {
            auto const built = f.build();
            REQUIRE(built.mEarly);
            REQUIRE(built.hashes() == hashesOf({plainC}));
            f.closeAndCheckApplied(built);
        }
        SECTION("without the validity pass the set would be invalid")
        {
            Access::skipFinalPass(herder, true);
            auto const built = f.build();
            Access::skipFinalPass(herder, false);
            REQUIRE(built.holds(nextA));
            REQUIRE(f.validate(built) != TxSetValidationResult::VALID);
        }
    }
    SECTION("fee bumps")
    {
        // D pays for A's transactions. In the previous set it also sends
        // away all it has but one more fee bump's worth.
        auto innerClosing = f.pay(a, 1);
        auto bumpClosing = feeBump(*f.mApp, d, innerClosing, 200);
        auto const fees = bumpClosing->getFullFee();
        auto drainD = f.pay(d, 1, d.getAvailableBalance() - 100 - 2 * fees);
        auto closing = f.setOf({bumpClosing, drainD});
        // Two fee bumps by D for the next ledger: it can pay for one.
        auto bumpNextA = feeBump(*f.mApp, d, f.pay(a, 2), 200);
        auto bumpB = feeBump(*f.mApp, d, f.pay(b, 1), 200);
        auto plainC = f.pay(c, 1);
        f.mMempool = {bumpClosing, drainD, bumpNextA, bumpB, plainC};
        f.voteAndSelect(closing);
        f.close(closing);
        REQUIRE(f.seqOf(a) == innerClosing->getSeqNum());
        REQUIRE(d.getAvailableBalance() == fees);
        auto const built = f.build();
        REQUIRE(built.mEarly);
        REQUIRE(built.holds(plainC));
        // One of D's two, whichever comes first, and not the other.
        REQUIRE(built.holds(bumpNextA));
        REQUIRE(!built.holds(bumpB));
        f.closeAndCheckApplied(built);
    }
    SECTION("the previous set's own transactions were not applied")
    {
        // The set this node voted for is not valid (a sequence number too
        // far ahead): the ledger closes with it and applies nothing. What
        // the selection predicted of it is wrong for every account.
        auto closingA = f.pay(a, 1);
        auto bad = f.pay(c, 5);
        auto nextA = f.pay(a, 2);
        auto plainB = f.pay(b, 1);
        auto closing = f.setOf({closingA, bad});
        f.mMempool = {closingA, bad, nextA, plainB};
        f.voteAndSelect(closing);
        auto const skipped = applyCounter(*f.mApp, "txset-skipped");
        auto const seqA = f.seqOf(a);
        f.close(closing);
        REQUIRE(applyCounter(*f.mApp, "txset-skipped") == skipped + 1);
        REQUIRE(f.seqOf(a) == seqA);
        auto const built = f.build();
        REQUIRE(built.mEarly);
        // A's second payment waits for its first.
        REQUIRE(built.hashes() == hashesOf({plainB}));
        f.closeAndCheckApplied(built);
    }
}

TEST_CASE("a set selected early is valid for any overlap with the previous "
          "set",
          "[herder][txset][early-txset]")
{
    // Per account, one of: nothing in the previous set; its next transaction
    // in the previous set; and as candidates any of: the transaction after
    // that, another one with the sequence number the previous set uses, one
    // far ahead. In random order, for a random share of the accounts.
    size_t const ACCOUNTS = 24;
    EarlyFixture f(ACCOUNTS);
    auto const seed = GENERATE(1u, 2u, 3u, 4u, 5u, 6u);
    auto const overlapPercent = GENERATE(0, 30, 70, 100);
    CAPTURE(seed, overlapPercent);
    std::mt19937 gen(seed * 1000 + static_cast<unsigned>(overlapPercent));
    auto chance = [&](int percent) {
        return stellar::uniform_int_distribution<int>(0, 99)(gen) < percent;
    };

    std::vector<TransactionFrameBasePtr> previous;
    std::vector<TransactionFrameBasePtr> candidates;
    // What must be in the next set, and what must not.
    std::vector<TransactionFrameBasePtr> valid;
    std::vector<TransactionFrameBasePtr> invalid;
    for (size_t i = 0; i < ACCOUNTS; ++i)
    {
        auto& account = f.acc(i);
        bool const inPrevious = chance(overlapPercent);
        if (inPrevious)
        {
            previous.push_back(f.pay(account, 1, 1));
            if (chance(60))
            {
                valid.push_back(f.pay(account, 2));
                candidates.push_back(valid.back());
            }
            if (chance(30))
            {
                invalid.push_back(f.pay(account, 1, 7));
                candidates.push_back(invalid.back());
            }
        }
        else if (chance(70))
        {
            valid.push_back(f.pay(account, 1));
            candidates.push_back(valid.back());
        }
        if (chance(20))
        {
            invalid.push_back(f.pay(account, 9));
            candidates.push_back(invalid.back());
        }
    }
    if (previous.empty())
    {
        // (an empty set plans no selection)
        return;
    }
    f.mMempool = previous;
    f.mMempool.insert(f.mMempool.end(), candidates.begin(), candidates.end());
    stellar::shuffle(f.mMempool.begin(), f.mMempool.end(), gen);

    auto closing = f.setOf(previous);
    f.voteAndSelect(closing);
    f.close(closing, /*keepInMempool=*/chance(50));
    auto const built = f.build();
    REQUIRE(built.mEarly);
    REQUIRE(f.validate(built) == TxSetValidationResult::VALID);
    auto const hashes = built.hashes();
    for (auto const& tx : previous)
    {
        REQUIRE(hashes.count(tx->getFullHash()) == 0);
    }
    for (auto const& tx : invalid)
    {
        REQUIRE(hashes.count(tx->getFullHash()) == 0);
    }
    // Nothing is left behind that a build would take: it is the set a build
    // from scratch selects on the closed ledger once the closed set's
    // transactions have left the mempool. (A candidate that reuses a
    // sequence number shadows the account's next one in both: one
    // transaction per account, the lowest sequence number first.)
    std::vector<TransactionFrameBasePtr> stillPending;
    {
        auto const closed = hashesOf(previous);
        for (auto const& tx : f.mMempool)
        {
            if (closed.count(tx->getFullHash()) == 0)
            {
                stillPending.push_back(tx);
            }
        }
    }
    REQUIRE(hashes == f.buildFrom(stillPending).hashes());
    std::set<Hash> validHashes = hashesOf(valid);
    for (auto const& hash : hashes)
    {
        REQUIRE(validHashes.count(hash) == 1);
    }
    f.closeAndCheckApplied(built);
}

TEST_CASE("a set selected early is not used when the ledger closes "
          "otherwise",
          "[herder][txset][early-txset]")
{
    EarlyFixture f(4);
    auto& app = *f.mApp;
    auto& herder = f.herder();
    auto& a = f.acc(0);
    auto& b = f.acc(1);
    auto closingA = f.pay(a, 1);
    auto nextA = f.pay(a, 2);
    auto plainB = f.pay(b, 1);
    auto closing = f.setOf({closingA});
    f.mMempool = {closingA, nextA, plainB};
    f.voteAndSelect(closing);
    auto const discarded = txsetMeter(app, "early-discarded");

    SECTION("another set closes the ledger")
    {
        f.close(f.setOf({plainB}));
        REQUIRE(txsetMeter(app, "early-discarded") == discarded + 1);
        REQUIRE(!Access::hasPlan(herder));
        auto const built = f.build();
        REQUIRE(!built.mEarly);
        // A's first payment is still to come.
        REQUIRE(built.hashes() == hashesOf({closingA}));
        f.closeAndCheckApplied(built);
    }
    SECTION("the same set closes the ledger with another close time")
    {
        auto const later =
            ConsensusTime::fromApplyTime(ApplyTime::fromTimePoint(
                f.lm().getLastClosedLedgerHeader().header.scpValue.closeTime +
                3));
        f.close(closing, false, emptyUpgradeSteps, later);
        REQUIRE(txsetMeter(app, "early-discarded") == discarded + 1);
        auto const built = f.build();
        REQUIRE(!built.mEarly);
        REQUIRE(built.hashes() == hashesOf({nextA, plainB}));
    }
    SECTION("the ledger closes with an upgrade that lowers the limits")
    {
        xdr::xvector<UpgradeType, 6> upgrades;
        LedgerUpgrade upgrade(LEDGER_UPGRADE_MAX_TX_SET_SIZE);
        upgrade.newMaxTxSetSize() = 1;
        upgrades.emplace_back(LedgerTestUtils::toUpgradeType(upgrade));
        f.close(closing, false, upgrades);
        REQUIRE(f.lm().getLastClosedLedgerHeader().header.maxTxSetSize == 1);
        REQUIRE(txsetMeter(app, "early-discarded") == discarded + 1);
        auto const built = f.build();
        REQUIRE(!built.mEarly);
        // The selection held two transactions; the ledger takes one.
        REQUIRE(built.hashes().size() == 1);
        REQUIRE(f.validate(built) == TxSetValidationResult::VALID);
        f.closeAndCheckApplied(built);
    }
    SECTION("the limits of the closed ledger are not the ones it assumed")
    {
        // Cannot happen without an upgrade; checked all the same.
        {
            auto job = Access::job(herder);
            std::lock_guard<std::mutex> lock(job->mMutex);
            job->mSelection->mLimits.mLclHeader.maxTxSetSize += 1;
        }
        f.close(closing);
        auto const built = f.build();
        REQUIRE(!built.mEarly);
        REQUIRE(txsetMeter(app, "early-discarded") == discarded + 1);
        REQUIRE(built.hashes() == hashesOf({nextA, plainB}));
    }
    SECTION("the node votes for another value before the ledger closes")
    {
        // The round's leader timed out; the next one proposes another set.
        auto first = Access::job(herder);
        auto other = f.setOf({plainB});
        f.vote(other);
        // The first selection is abandoned ...
        REQUIRE(first->mStop.load());
        REQUIRE(Access::job(herder) != first);
        // ... and one for the new value made.
        REQUIRE(f.waitSelected());
        SECTION("the ledger closes with the new value")
        {
            f.close(other);
            auto const built = f.build();
            REQUIRE(built.mEarly);
            REQUIRE(built.hashes() == hashesOf({closingA}));
            f.closeAndCheckApplied(built);
        }
        SECTION("the ledger closes with the first value after all")
        {
            f.close(closing);
            REQUIRE(txsetMeter(app, "early-discarded") == discarded + 1);
            auto const built = f.build();
            REQUIRE(!built.mEarly);
            REQUIRE(built.hashes() == hashesOf({nextA, plainB}));
            f.closeAndCheckApplied(built);
        }
    }
}

TEST_CASE("a selection that is still running when it is needed",
          "[herder][txset][early-txset]")
{
    EarlyFixture f(4);
    auto& app = *f.mApp;
    auto& herder = f.herder();
    auto& a = f.acc(0);
    auto& b = f.acc(1);
    auto closingA = f.pay(a, 1);
    auto nextA = f.pay(a, 2);
    auto afterNextA = f.pay(a, 3);
    auto plainB = f.pay(b, 1);
    auto closing = f.setOf({closingA});
    f.mMempool = {closingA, nextA, plainB};

    // The job's tasks wait until the test lets them go.
    auto hold = std::make_shared<std::atomic<bool>>(true);
    Access::hold(herder, hold);
    f.vote(closing);
    auto first = f.waitStarted();
    REQUIRE(!first->mDone.load());
    REQUIRE(Access::jobDuration(herder).first == std::chrono::milliseconds(0));
    f.close(closing);
    f.crankFor(std::chrono::milliseconds(20));
    auto const notReady = txsetMeter(app, "early-not-ready");
    auto const built = f.build();
    // The ledger gets a set built the usual way; nobody waits for the job.
    REQUIRE(!built.mEarly);
    REQUIRE(txsetMeter(app, "early-not-ready") == notReady + 1);
    // The job had 20 ms and more and was not done: the node now takes a
    // selection to last half as long again, so the next one starts earlier.
    REQUIRE(Access::jobDuration(herder).first >= std::chrono::milliseconds(30));
    REQUIRE(Access::jobDuration(herder).second >=
            std::chrono::milliseconds(30));
    REQUIRE(built.hashes() == hashesOf({nextA, plainB}));
    REQUIRE(first->mStop.load());
    REQUIRE(!Access::job(herder));

    SECTION("it finishes after the build and hands nothing over")
    {
        hold->store(false);
        f.crankUntil([&]() { return first->mDone.load(); });
        std::lock_guard<std::mutex> lock(first->mMutex);
        REQUIRE(!first->mSelection);
    }
    SECTION("it is still running when the next selection starts")
    {
        // The next ledger is in its ballot, with this node's set.
        f.mMempool = {nextA, plainB, afterNextA};
        Access::hold(herder, nullptr);
        f.vote(built.mTxSet);
        REQUIRE(f.waitSelected());
        auto second = Access::job(herder);
        REQUIRE(second != first);
        // Only now does the first job get to run.
        hold->store(false);
        f.crankUntil([&]() { return first->mDone.load(); });
        {
            std::lock_guard<std::mutex> lock(first->mMutex);
            REQUIRE(!first->mSelection);
        }
        REQUIRE(Access::job(herder) == second);
        f.closeAndCheckApplied(built);
        auto const next = f.build();
        REQUIRE(next.mEarly);
        REQUIRE(next.hashes() == hashesOf({afterNextA}));
        f.closeAndCheckApplied(next);
    }
    hold->store(false);
}

TEST_CASE("the application shuts down while a selection runs",
          "[herder][txset][early-txset]")
{
    auto f = std::make_unique<EarlyFixture>(4);
    auto closingA = f->pay(f->acc(0), 1);
    f->mMempool = {closingA, f->pay(f->acc(0), 2), f->pay(f->acc(1), 1)};
    auto hold = std::make_shared<std::atomic<bool>>(true);
    Access::hold(f->herder(), hold);
    f->vote(f->setOf({closingA}));
    auto job = f->waitStarted();
    REQUIRE(!job->mDone.load());
    // The tasks are let go while the application is going down: they find
    // the job stopped, or finish it; either way nothing is left running
    // when the application is gone.
    std::thread release([hold]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        hold->store(false);
    });
    f.reset();
    release.join();
    REQUIRE(job->mDone.load());
}

TEST_CASE("candidates whose validity depends on the close time in a set "
          "selected early",
          "[herder][txset][early-txset]")
{
    EarlyFixture f(4);
    auto& app = *f.mApp;
    auto& a = f.acc(0);
    auto& b = f.acc(1);
    auto& c = f.acc(2);
    auto closingA = f.pay(a, 1);
    auto closing = f.setOf({closingA});
    auto const lclCloseTime =
        f.lm().getLastClosedLedgerHeader().header.scpValue.closeTime;
    auto timeBounded = [&](TestAccount& account, TimePoint maxTime) {
        PreconditionsV2 cond;
        cond.timeBounds.activate().minTime = 0;
        cond.timeBounds.activate().maxTime = maxTime;
        return TransactionFrameBasePtr(transactionFromOperationsV1(
            app, account.getSecretKey(), f.seqOf(account) + 1,
            {payment(app.getRoot()->getPublicKey(), 1)}, 100, cond));
    };
    // The closing ledger gets close time +1 and the next +2 (seconds after
    // the LCL's). B's transaction expires before the next ledger, C's lasts.
    auto expiring = timeBounded(b, lclCloseTime + 1);
    auto lasting = timeBounded(c, lclCloseTime + 1000);
    REQUIRE(txValidityDependsOnCloseTime(expiring->getEnvelope()));
    f.mMempool = {closingA, expiring, lasting};
    f.voteAndSelect(closing);
    f.close(closing);
    auto const built = f.build();
    REQUIRE(built.mEarly);
    // Whatever close time the selection assumed, the completed set is
    // validated for the one it gets.
    REQUIRE(built.hashes() == hashesOf({lasting}));
    REQUIRE(f.validate(built) == TxSetValidationResult::VALID);
    f.closeAndCheckApplied(built);
}

// ---------------------------------------------------------------------------
// Property b: full sets.
// ---------------------------------------------------------------------------

TEST_CASE("a set selected early is topped up when the ledger has closed",
          "[herder][txset][early-txset]")
{
    EarlyFixture f(10, 6);
    auto& app = *f.mApp;
    auto& herder = f.herder();
    REQUIRE(f.lm().getLastMaxTxSetSizeOps() == 6);
    auto closingA = f.pay(f.acc(0), 1);
    auto closing = f.setOf({closingA});
    std::vector<TransactionFrameBasePtr> early{f.pay(f.acc(1), 1),
                                               f.pay(f.acc(2), 1)};
    f.mMempool = {closingA, early[0], early[1]};
    f.voteAndSelect(closing);
    auto const added = txsetMeter(app, "early-added", "tx");

    SECTION("with what arrived after the selection's pull")
    {
        std::vector<TransactionFrameBasePtr> late{
            f.pay(f.acc(3), 1), f.pay(f.acc(4), 1), f.pay(f.acc(0), 2)};
        f.mMempool.insert(f.mMempool.end(), late.begin(), late.end());
        f.close(closing);
        f.mPulls.clear();
        auto const built = f.build();
        REQUIRE(built.mEarly);
        auto all = early;
        all.insert(all.end(), late.begin(), late.end());
        REQUIRE(built.hashes() == hashesOf(all));
        REQUIRE(txsetMeter(app, "early-added", "tx") == added + 3);
        REQUIRE(f.mPulls == std::vector<size_t>{Access::pullCount(herder)});
        f.closeAndCheckApplied(built);
    }
    SECTION("up to its capacity, and no further")
    {
        std::vector<TransactionFrameBasePtr> late;
        for (size_t i = 3; i < 10; ++i)
        {
            late.push_back(f.pay(f.acc(i), 1));
        }
        f.mMempool.insert(f.mMempool.end(), late.begin(), late.end());
        f.close(closing);
        auto const built = f.build();
        REQUIRE(built.mEarly);
        REQUIRE(built.hashes().size() == 6);
        REQUIRE(built.holds(early[0]));
        REQUIRE(built.holds(early[1]));
        REQUIRE(f.validate(built) == TxSetValidationResult::VALID);
        f.closeAndCheckApplied(built);
    }
    SECTION("a new transaction of an account that is in the set already "
            "waits")
    {
        auto second = f.pay(f.acc(1), 2);
        auto plain = f.pay(f.acc(5), 1);
        f.mMempool.push_back(second);
        f.mMempool.push_back(plain);
        f.close(closing);
        auto const built = f.build();
        REQUIRE(built.mEarly);
        REQUIRE(built.hashes() == hashesOf({early[0], early[1], plain}));
        f.closeAndCheckApplied(built);
    }
    SECTION("a full phase asks for nothing more")
    {
        // Another selection, with more candidates than the set takes.
        f.close(closing);
        f.closeAndCheckApplied(f.build());
        auto closingB = f.pay(f.acc(1), 1);
        auto closing2 = f.setOf({closingB});
        f.mMempool = {closingB};
        for (size_t i = 2; i < 10; ++i)
        {
            f.mMempool.push_back(f.pay(f.acc(i), 1));
        }
        f.voteAndSelect(closing2);
        f.close(closing2);
        f.mPulls.clear();
        auto const added = txsetMeter(app, "early-added", "tx");
        auto const built = f.build();
        REQUIRE(built.mEarly);
        REQUIRE(built.hashes().size() == 6);
        REQUIRE(txsetMeter(app, "early-added", "tx") == added);
        // One look at the mempool's top, for the Soroban phase, which has
        // room and never had a candidate: nothing there for it, so no
        // deeper pull.
        REQUIRE(f.mPulls == std::vector<size_t>{Access::pullCount(herder)});
        f.closeAndCheckApplied(built);
    }
}

TEST_CASE("a set selected early is refilled when transactions of it turn "
          "out invalid",
          "[herder][txset][early-txset]")
{
    // A set takes 10 transactions; the mempool holds 40 candidates beside
    // the closing ledger's. The closing ledger drains the accounts of some
    // of them, which only the closed ledger shows.
    size_t const CAPACITY = 10;
    size_t const CANDIDATES = 40;
    EarlyFixture f(CANDIDATES + 1, CAPACITY);
    auto& app = *f.mApp;
    auto& herder = f.herder();
    auto const drainedPercent = GENERATE(0, 10, 30, 60, 100);
    CAPTURE(drainedPercent);

    // Candidate i comes from account i; the closing set drains the first
    // `drained` of those accounts.
    size_t const drained = CANDIDATES * drainedPercent / 100;
    std::vector<TransactionFrameBasePtr> previous;
    std::vector<TransactionFrameBasePtr> candidates;
    for (size_t i = 0; i < CANDIDATES; ++i)
    {
        auto& account = f.acc(i);
        if (i < drained)
        {
            previous.push_back(
                f.pay(account, 1, account.getAvailableBalance() - 100));
            candidates.push_back(f.pay(account, 2));
        }
        else
        {
            candidates.push_back(f.pay(account, 1));
        }
    }
    if (previous.empty())
    {
        previous.push_back(f.pay(f.acc(CANDIDATES), 1));
    }
    f.mMempool = previous;
    f.mMempool.insert(f.mMempool.end(), candidates.begin(), candidates.end());
    auto closing = f.setOf(previous);
    f.voteAndSelect(closing);
    f.close(closing);

    auto const built = f.build();
    REQUIRE(built.mEarly);
    REQUIRE(f.validate(built) == TxSetValidationResult::VALID);
    auto const hashes = built.hashes();
    for (size_t i = 0; i < drained; ++i)
    {
        REQUIRE(hashes.count(candidates[i]->getFullHash()) == 0);
    }
    // Full whenever enough valid candidates exist.
    size_t const validCandidates = CANDIDATES - drained;
    REQUIRE(hashes.size() == std::min(CAPACITY, validCandidates));
    auto& fill =
        app.getMetrics().NewHistogram({"herder", "txset", "fill-classic"});
    REQUIRE(fill.count() >= 1);
    f.closeAndCheckApplied(built);
}

TEST_CASE("a build pulls the capacity of a set plus a margin and refills",
          "[herder][txset][early-txset]")
{
    size_t const CAPACITY = 20;
    auto const marginPercent = GENERATE(0u, 10u, 50u);
    CAPTURE(marginPercent);
    size_t const ACCOUNTS = 120;
    EarlyFixture f(ACCOUNTS, CAPACITY, [&](Config& cfg) {
        cfg.EXPERIMENTAL_TX_SET_PULL_MARGIN_PERCENT = marginPercent;
        // Builds only.
        cfg.EXPERIMENTAL_EARLY_TX_SET_SELECTION = false;
    });
    auto& app = *f.mApp;
    auto& herder = f.herder();
    // The capacity of both phases together, and the margin on top.
    size_t const total =
        CAPACITY +
        f.lm().getLastClosedSorobanNetworkConfig().ledgerMaxTxCount();
    size_t const pull = total + (total * marginPercent + 99) / 100;
    REQUIRE(Access::pullCount(herder) == pull);

    SECTION("one pull when the mempool holds less than it asks for")
    {
        for (size_t i = 0; i < 30; ++i)
        {
            f.mMempool.push_back(f.pay(f.acc(i), 1));
        }
        auto const built = f.build();
        REQUIRE(f.mPulls == std::vector<size_t>{pull});
        REQUIRE(built.hashes().size() == CAPACITY);
    }
    SECTION("candidates lost to the one-per-account rule are refilled too")
    {
        // The top of the mempool is one account's transactions with one
        // sequence number, each valid by itself; a set takes one of them.
        for (size_t i = 0; i < pull; ++i)
        {
            f.mMempool.push_back(
                f.pay(f.acc(0), 1, static_cast<int64_t>(i + 1)));
        }
        for (size_t i = 1; i < 60; ++i)
        {
            f.mMempool.push_back(f.pay(f.acc(i), 1));
        }
        auto const built = f.build();
        REQUIRE(f.mPulls.size() == 2);
        REQUIRE(built.hashes().size() == CAPACITY);
        REQUIRE(f.validate(built) == TxSetValidationResult::VALID);
    }
    SECTION("X% of the candidates are invalid")
    {
        // The mempool holds more than three pulls can reach; every k-th
        // candidate is invalid (a sequence number of the past).
        auto const invalidPercent = GENERATE(0, 5, 20, 50, 90);
        CAPTURE(invalidPercent);
        auto const refills = txsetMeter(app, "refill-pull", "pull");
        std::mt19937 gen(42);
        size_t valid = 0;
        std::vector<TransactionFrameBasePtr> spent;
        // One spent sequence number per account to make invalid ones from.
        for (size_t i = 0; i < ACCOUNTS; ++i)
        {
            bool const bad = stellar::uniform_int_distribution<int>(0, 99)(
                                 gen) < invalidPercent;
            if (bad)
            {
                f.mMempool.push_back(f.pay(f.acc(i), 0));
            }
            else
            {
                f.mMempool.push_back(f.pay(f.acc(i), 1));
                ++valid;
            }
        }
        auto const built = f.build();
        REQUIRE(f.validate(built) == TxSetValidationResult::VALID);
        // Count the valid candidates within reach of the pulls made.
        size_t const reached = std::min<size_t>(f.mPulls.back(), ACCOUNTS);
        size_t validReached = 0;
        for (size_t i = 0; i < reached; ++i)
        {
            if (f.mMempool[i]->getSeqNum() == f.seqOf(f.acc(i)) + 1)
            {
                ++validReached;
            }
        }
        REQUIRE(built.hashes().size() ==
                std::min<size_t>(CAPACITY, validReached));
        // Each pull asks for twice the one before.
        for (size_t i = 0; i < f.mPulls.size(); ++i)
        {
            REQUIRE(f.mPulls[i] == pull << i);
        }
        REQUIRE(f.mPulls.size() <= 3);
        REQUIRE(txsetMeter(app, "refill-pull", "pull") ==
                refills + static_cast<int64_t>(f.mPulls.size()) - 1);
        // The set is full whenever the mempool holds enough valid
        // transactions within three pulls.
        if (valid >= CAPACITY && ACCOUNTS <= (pull << 2))
        {
            REQUIRE(built.hashes().size() == CAPACITY);
        }
    }
}

TEST_CASE("a phase that lost candidates is refilled from below a mempool top "
          "of the other phase",
          "[herder][txset][early-txset]")
{
    // The mempool has one fee order for both phases. A phase that lost
    // candidates looks deeper also when the top holds little of its kind:
    // what it needs may lie below the other phase's transactions.
    size_t const CAPACITY = 10;
    size_t const SOROBAN_CAPACITY = 10;
    SECTION("a completion")
    {
        EarlyFixture f(60, CAPACITY);
        auto& app = *f.mApp;
        auto& herder = f.herder();
        overrideSorobanNetworkConfigForTest(app);
        modifySorobanNetworkConfig(app, [&](SorobanNetworkConfig& cfg) {
            cfg.mLedgerMaxTxCount = SOROBAN_CAPACITY;
        });
        size_t const pull = Access::pullCount(herder);
        REQUIRE(pull >= CAPACITY + SOROBAN_CAPACITY);
        // The closing set drains accounts 0 and 1. The selection holds
        // their next payments and two more: four classic candidates, all
        // there are.
        std::vector<TransactionFrameBasePtr> previous;
        std::vector<TransactionFrameBasePtr> candidates;
        for (size_t i = 0; i < 2; ++i)
        {
            auto& account = f.acc(i);
            previous.push_back(
                f.pay(account, 1, account.getAvailableBalance() - 100));
            candidates.push_back(f.pay(account, 2));
        }
        candidates.push_back(f.pay(f.acc(2), 1));
        candidates.push_back(f.pay(f.acc(3), 1));
        f.mMempool = previous;
        f.mMempool.insert(f.mMempool.end(), candidates.begin(),
                          candidates.end());
        auto closing = f.setOf(previous);
        f.voteAndSelect(closing);
        f.close(closing);
        // During the ballot: more Soroban transactions than a pull holds,
        // and below them classic ones.
        for (size_t i = 0; i < pull + 4; ++i)
        {
            f.mMempool.push_back(f.upload(f.acc(4 + i), 1, 100 + i));
        }
        std::vector<TransactionFrameBasePtr> below;
        for (size_t i = 0; i < 8; ++i)
        {
            below.push_back(f.pay(f.acc(4 + pull + 4 + i), 1));
        }
        f.mMempool.insert(f.mMempool.end(), below.begin(), below.end());
        auto const refills = txsetMeter(app, "refill-pull", "pull");
        auto const built = f.build();
        REQUIRE(built.mEarly);
        REQUIRE(f.validate(built) == TxSetValidationResult::VALID);
        REQUIRE(!built.holds(candidates[0]));
        REQUIRE(!built.holds(candidates[1]));
        REQUIRE(built.holds(candidates[2]));
        REQUIRE(built.holds(candidates[3]));
        // The two it lost are made up for from below.
        REQUIRE(txsetMeter(app, "refill-pull", "pull") > refills);
        REQUIRE(built.mApplicable->sizeTx(TxSetPhase::CLASSIC) > 2);
        REQUIRE(built.mApplicable->sizeTx(TxSetPhase::SOROBAN) ==
                SOROBAN_CAPACITY);
        f.closeAndCheckApplied(built);
    }
    SECTION("a build")
    {
        EarlyFixture f(120, CAPACITY, [&](Config& cfg) {
            cfg.EXPERIMENTAL_EARLY_TX_SET_SELECTION = false;
        });
        auto& app = *f.mApp;
        auto& herder = f.herder();
        overrideSorobanNetworkConfigForTest(app);
        modifySorobanNetworkConfig(app, [&](SorobanNetworkConfig& cfg) {
            cfg.mLedgerMaxTxCount = SOROBAN_CAPACITY;
        });
        size_t const pull = Access::pullCount(herder);
        // The top: six classic candidates, four of them with a spent
        // sequence number, then Soroban transactions to the end of the
        // pull. Below: valid classic ones.
        for (size_t i = 0; i < 6; ++i)
        {
            f.mMempool.push_back(f.pay(f.acc(i), i < 4 ? 0 : 1));
        }
        for (size_t i = 6; i < pull; ++i)
        {
            f.mMempool.push_back(f.upload(f.acc(i), 1, 100 + i));
        }
        for (size_t i = 0; i < 12; ++i)
        {
            f.mMempool.push_back(f.pay(f.acc(pull + i), 1));
        }
        auto const built = f.build();
        REQUIRE(f.validate(built) == TxSetValidationResult::VALID);
        REQUIRE(f.mPulls.size() == 2);
        REQUIRE(built.mApplicable->sizeTx(TxSetPhase::CLASSIC) == CAPACITY);
        REQUIRE(built.mApplicable->sizeTx(TxSetPhase::SOROBAN) ==
                SOROBAN_CAPACITY);
    }
}

TEST_CASE("what the top-up may not add to a set selected early",
          "[herder][txset][early-txset]")
{
    EarlyFixture f(8);
    auto& app = *f.mApp;
    auto& a = f.acc(0);
    auto& b = f.acc(1);
    auto& c = f.acc(2);
    auto& d = f.acc(3);
    auto closingA = f.pay(a, 1);
    auto closing = f.setOf({closingA});

    SECTION("a transaction its fee source cannot pay for on top of the "
            "selected ones")
    {
        // D pays the fees of other accounts' transactions. The closing
        // ledger leaves it with what two fee bumps cost (the selection
        // cannot know: it predicts fees, not payments). One bump by D is
        // selected; two more arrive later. D can pay for one of them.
        auto bumpB = feeBump(app, d, f.pay(b, 1), 200);
        auto const fees = bumpB->getFullFee();
        auto drainD = f.pay(d, 1, d.getAvailableBalance() - 100 - 2 * fees);
        auto closing2 = f.setOf({closingA, drainD});
        auto bumpLate1 = feeBump(app, d, f.pay(c, 1), 200);
        auto bumpLate2 = feeBump(app, d, f.pay(f.acc(4), 1), 200);
        auto plain = f.pay(f.acc(5), 1);
        f.mMempool = {closingA, drainD, bumpB};
        f.voteAndSelect(closing2);
        f.close(closing2);
        REQUIRE(d.getAvailableBalance() == 2 * fees);
        f.mMempool.push_back(bumpLate1);
        f.mMempool.push_back(bumpLate2);
        f.mMempool.push_back(plain);
        auto const built = f.build();
        REQUIRE(built.mEarly);
        REQUIRE(built.holds(bumpB));
        REQUIRE(built.holds(plain));
        // The selected bump counts against D's balance when the late ones
        // are validated: one more fits, the next does not.
        REQUIRE(built.holds(bumpLate1));
        REQUIRE(!built.holds(bumpLate2));
        REQUIRE(f.validate(built) == TxSetValidationResult::VALID);
        f.closeAndCheckApplied(built);
    }
    SECTION("a second transaction of an account that has one in the set")
    {
        // Another transaction of B with the sequence number of the selected
        // one: each is valid on the closed ledger, together they are not a
        // set.
        auto selectedB = f.pay(b, 1, 1);
        auto otherB = f.pay(b, 1, 2);
        auto plain = f.pay(c, 1);
        f.mMempool = {closingA, selectedB};
        f.voteAndSelect(closing);
        f.close(closing);
        f.mMempool.push_back(otherB);
        f.mMempool.push_back(plain);
        auto const built = f.build();
        REQUIRE(built.mEarly);
        REQUIRE(built.hashes() == hashesOf({selectedB, plain}));
        REQUIRE(f.validate(built) == TxSetValidationResult::VALID);
        REQUIRE(!Access::selfCheckLatched(f.herder()));
        f.closeAndCheckApplied(built);
    }
}

TEST_CASE("the base fee of a set selected early follows what was left out",
          "[herder][txset][early-txset]")
{
    // A set takes 4 transactions. Fees: 500, 400, 300, 200 selected; 150
    // left over.
    EarlyFixture f(8, 4);
    auto& app = *f.mApp;
    auto closingA = f.pay(f.acc(0), 1);
    auto closing = f.setOf({closingA});
    auto t500 = f.pay(f.acc(1), 1, 1, 500);
    auto t400 = f.pay(f.acc(2), 1, 1, 400);
    auto t300 = f.pay(f.acc(3), 1, 1, 300);
    auto t150 = f.pay(f.acc(5), 1, 1, 150);
    auto baseFeeOf = [&](Built const& built,
                         TransactionFrameBasePtr const& tx) {
        for (auto const& candidate :
             built.mApplicable->getPhase(TxSetPhase::CLASSIC))
        {
            if (candidate->getFullHash() == tx->getFullHash())
            {
                auto fee = built.mApplicable->getTxBaseFee(candidate);
                REQUIRE(fee);
                return *fee;
            }
        }
        REQUIRE(false);
        return int64_t(0);
    };
    auto const ledgerBaseFee =
        f.lm().getLastClosedLedgerHeader().header.baseFee;

    SECTION("untouched: the lowest fee selected, because one was left out")
    {
        auto t200 = f.pay(f.acc(4), 1, 1, 200);
        f.mMempool = {closingA, t500, t400, t300, t200, t150};
        f.voteAndSelect(closing);
        f.close(closing);
        auto const built = f.build();
        REQUIRE(built.mEarly);
        REQUIRE(built.hashes() == hashesOf({t500, t400, t300, t200}));
        REQUIRE(baseFeeOf(built, t500) == 200);
        REQUIRE(f.validate(built) == TxSetValidationResult::VALID);
    }
    SECTION("one removed and replaced by the left-over: its fee, nothing "
            "is left out any more")
    {
        // The account of the 200 transaction is drained by the closing
        // ledger.
        auto& drained = f.acc(4);
        auto drain = f.pay(drained, 1, drained.getAvailableBalance() - 100);
        auto t200 = f.pay(drained, 2, 1, 200);
        auto closing2 = f.setOf({drain});
        f.mMempool = {drain, t500, t400, t300, t200, t150};
        f.voteAndSelect(closing2);
        f.close(closing2);
        auto const built = f.build();
        REQUIRE(built.mEarly);
        REQUIRE(built.hashes() == hashesOf({t500, t400, t300, t150}));
        // Everything valid is in: nobody competed for the room.
        REQUIRE(baseFeeOf(built, t500) == ledgerBaseFee);
        REQUIRE(f.validate(built) == TxSetValidationResult::VALID);
        // The same as a build from scratch.
        auto const fresh = f.buildFrom({t500, t400, t300, t200, t150});
        REQUIRE(fresh.hashes() == built.hashes());
        REQUIRE(baseFeeOf(fresh, t500) == ledgerBaseFee);
        f.closeAndCheckApplied(built);
    }
    SECTION("more arrive than fit: the lowest fee that got in")
    {
        f.mMempool = {closingA, t500, t400};
        f.voteAndSelect(closing);
        f.close(closing);
        auto t350 = f.pay(f.acc(4), 1, 1, 350);
        auto t250 = f.pay(f.acc(6), 1, 1, 250);
        f.mMempool = {t500, t400, t350, t300, t250, t150};
        auto const built = f.build();
        REQUIRE(built.mEarly);
        REQUIRE(built.hashes() == hashesOf({t500, t400, t350, t300}));
        REQUIRE(baseFeeOf(built, t500) == 300);
        REQUIRE(f.validate(built) == TxSetValidationResult::VALID);
        f.closeAndCheckApplied(built);
    }
}

// ---------------------------------------------------------------------------
// Soroban: the parallel phase is extended in place.
// ---------------------------------------------------------------------------

TEST_CASE("a parallel Soroban phase is extended without selecting it again",
          "[herder][txset][early-txset][soroban]")
{
    EarlyFixture f(12);
    auto& app = *f.mApp;
    // Limits under which the transactions of this test are valid.
    overrideSorobanNetworkConfigForTest(app);
    auto const lcl = f.lm().getLastClosedLedgerHeader();
    auto const version = lcl.header.ledgerVersion;
    SorobanNetworkConfig config = f.lm().getLastClosedSorobanNetworkConfig();
    // No limit but the ones each section sets.
    auto laneLimit = config.maxLedgerResources();
    for (auto type :
         {Resource::Type::OPERATIONS, Resource::Type::INSTRUCTIONS,
          Resource::Type::TX_BYTE_SIZE, Resource::Type::DISK_READ_BYTES,
          Resource::Type::WRITE_BYTES, Resource::Type::READ_LEDGER_ENTRIES,
          Resource::Type::WRITE_LEDGER_ENTRIES})
    {
        laneLimit.setVal(type, std::numeric_limits<int64_t>::max() / 4);
    }

    // The stages an extender made, as a set: validated like any set.
    auto validity = [&](TxStageFrameList const& stages,
                        SorobanNetworkConfig const& cfg) {
        auto set = testtxset::makeNonValidatedGeneralizedTxSet(
            {}, std::make_optional<int64_t>(lcl.header.baseFee), stages, app,
            lcl.hash);
        REQUIRE(set.second);
        TxSetValidationContext ctx = TxSetValidationContext::forLCL(app);
        ctx.mSorobanConfig = &cfg;
        return set.second->checkValidWithResult(app, ctx, 0, 0);
    };
    auto count = [](TxStageFrameList const& stages) {
        size_t n = 0;
        for (auto const& stage : stages)
        {
            for (auto const& cluster : stage)
            {
                n += cluster.size();
            }
        }
        return n;
    };

    SECTION("independent transactions spread over the clusters")
    {
        config.mLedgerMaxDependentTxClusters = 4;
        config.mLedgerMaxInstructions = 3'000'000;
        ParallelPhaseExtender extender({}, config, laneLimit, version);
        // 4 clusters of 3,000,000 instructions take 12 transactions of
        // 1,000,000.
        for (size_t i = 0; i < 12; ++i)
        {
            REQUIRE(extender.tryAdd(f.upload(f.acc(i), 1, 100 + i)));
        }
        REQUIRE(extender.sizeTx() == 12);
        REQUIRE(extender.getStages().size() == 1);
        REQUIRE(extender.getStages()[0].size() == 4);
        for (auto const& cluster : extender.getStages()[0])
        {
            REQUIRE(cluster.size() == 3);
        }
        REQUIRE(validity(extender.getStages(), config) ==
                TxSetValidationResult::VALID);
        // The thirteenth does not fit the instruction limit.
        REQUIRE(!extender.tryAdd(f.upload(f.acc(0), 2, 999)));
        REQUIRE(extender.sizeTx() == 12);
    }
    SECTION("a transaction joins the cluster it conflicts with")
    {
        config.mLedgerMaxDependentTxClusters = 4;
        config.mLedgerMaxInstructions = 10'000'000;
        ParallelPhaseExtender extender({}, config, laneLimit, version);
        // Same seed: the same entry written.
        REQUIRE(extender.tryAdd(f.upload(f.acc(0), 1, 7)));
        REQUIRE(extender.tryAdd(f.upload(f.acc(1), 1, 8)));
        REQUIRE(extender.tryAdd(f.upload(f.acc(2), 1, 7)));
        auto const& stage = extender.getStages().at(0);
        REQUIRE(stage.size() == 2);
        REQUIRE(stage[0].size() == 2);
        REQUIRE(stage[1].size() == 1);
        REQUIRE(validity(extender.getStages(), config) ==
                TxSetValidationResult::VALID);
    }
    SECTION("a transaction that would join two clusters is left out")
    {
        config.mLedgerMaxDependentTxClusters = 4;
        config.mLedgerMaxInstructions = 10'000'000;
        auto first = f.upload(f.acc(0), 1, 7);
        auto second = f.upload(f.acc(1), 1, 8);
        // Writes what the two others write.
        SorobanResources resources;
        resources.instructions = 1'000'000;
        resources.diskReadBytes = 1000;
        resources.writeBytes = 4000;
        resources.footprint.readWrite =
            first->sorobanResources().footprint.readWrite;
        resources.footprint.readWrite.push_back(
            second->sorobanResources().footprint.readWrite.at(0));
        auto both = createUploadWasmTx(app, f.acc(2), 100,
                                       DEFAULT_TEST_RESOURCE_FEE, resources);
        ParallelPhaseExtender extender({}, config, laneLimit, version);
        REQUIRE(extender.tryAdd(first));
        REQUIRE(extender.tryAdd(second));
        REQUIRE(!extender.tryAdd(both));
        REQUIRE(extender.sizeTx() == 2);
        // Had it been added to either cluster, the phase would not be
        // valid.
        auto broken = extender.getStages();
        broken[0][0].push_back(both);
        REQUIRE(validity(broken, config) ==
                TxSetValidationResult::TX_ORDERING_INVALID);
    }
    SECTION("a reader conflicts with the writer, not with other readers")
    {
        config.mLedgerMaxDependentTxClusters = 4;
        config.mLedgerMaxInstructions = 10'000'000;
        auto writer = f.upload(f.acc(0), 1, 7);
        auto const key = writer->sorobanResources().footprint.readWrite.at(0);
        auto reader = [&](TestAccount& account, uint64_t seed) {
            SorobanResources resources;
            resources.instructions = 1'000'000;
            resources.diskReadBytes = 1000;
            resources.writeBytes = 2000;
            auto own = f.upload(account, 1, seed);
            resources.footprint.readWrite =
                own->sorobanResources().footprint.readWrite;
            resources.footprint.readOnly = {key};
            return createUploadWasmTx(app, account, 100,
                                      DEFAULT_TEST_RESOURCE_FEE, resources,
                                      std::nullopt, 0, std::nullopt,
                                      std::nullopt, std::make_optional(seed));
        };
        SECTION("readers first")
        {
            ParallelPhaseExtender extender({}, config, laneLimit, version);
            REQUIRE(extender.tryAdd(reader(f.acc(1), 11)));
            REQUIRE(extender.tryAdd(reader(f.acc(2), 12)));
            // Two readers, two clusters: the writer would join both.
            REQUIRE(extender.getStages().at(0).size() == 2);
            REQUIRE(!extender.tryAdd(writer));
            REQUIRE(validity(extender.getStages(), config) ==
                    TxSetValidationResult::VALID);
        }
        SECTION("the writer first")
        {
            ParallelPhaseExtender extender({}, config, laneLimit, version);
            REQUIRE(extender.tryAdd(writer));
            REQUIRE(extender.tryAdd(reader(f.acc(1), 11)));
            REQUIRE(extender.tryAdd(reader(f.acc(2), 12)));
            // Both readers are with the writer.
            REQUIRE(extender.getStages().at(0).size() == 1);
            REQUIRE(extender.getStages()[0][0].size() == 3);
            REQUIRE(validity(extender.getStages(), config) ==
                    TxSetValidationResult::VALID);
        }
    }
    SECTION("the lane's limits hold")
    {
        config.mLedgerMaxDependentTxClusters = 4;
        config.mLedgerMaxInstructions = 100'000'000;
        auto limit = laneLimit;
        // Three transactions.
        limit.setVal(Resource::Type::OPERATIONS, 3);
        ParallelPhaseExtender extender({}, config, limit, version);
        REQUIRE(extender.tryAdd(f.upload(f.acc(0), 1, 1)));
        REQUIRE(extender.tryAdd(f.upload(f.acc(1), 1, 2)));
        REQUIRE(extender.tryAdd(f.upload(f.acc(2), 1, 3)));
        REQUIRE(!extender.tryAdd(f.upload(f.acc(3), 1, 4)));
        REQUIRE(count(extender.getStages()) == 3);
    }
    SECTION("a transaction joins the cluster of an existing phase it "
            "conflicts with")
    {
        config.mLedgerMaxDependentTxClusters = 4;
        config.mLedgerMaxInstructions = 10'000'000;
        // One stage, two clusters, as a selection made them.
        TxStageFrameList stages{
            {{f.upload(f.acc(0), 1, 1)}, {f.upload(f.acc(1), 1, 2)}}};
        ParallelPhaseExtender extender(stages, config, laneLimit, version);
        // Writes what the second cluster writes.
        REQUIRE(extender.tryAdd(f.upload(f.acc(2), 1, 2)));
        REQUIRE(extender.getStages().at(0).size() == 2);
        REQUIRE(extender.getStages()[0][0].size() == 1);
        REQUIRE(extender.getStages()[0][1].size() == 2);
        REQUIRE(validity(extender.getStages(), config) ==
                TxSetValidationResult::VALID);
        // In the other cluster the phase would not be valid.
        auto broken = stages;
        broken[0][0].push_back(f.upload(f.acc(2), 1, 2));
        REQUIRE(validity(broken, config) ==
                TxSetValidationResult::TX_ORDERING_INVALID);
    }
    SECTION("stages that exist are extended, and stay within the limit")
    {
        config.mLedgerMaxDependentTxClusters = 2;
        config.mLedgerMaxInstructions = 4'000'000;
        // Two stages of one cluster each: 2,000,000 + 1,000,000 sequential
        // instructions of 4,000,000.
        TxStageFrameList stages{
            {{f.upload(f.acc(0), 1, 1), f.upload(f.acc(1), 1, 2)}},
            {{f.upload(f.acc(2), 1, 3)}}};
        ParallelPhaseExtender extender(stages, config, laneLimit, version);
        REQUIRE(extender.sizeTx() == 3);
        // A new cluster beside the first stage's: no more sequential
        // instructions.
        REQUIRE(extender.tryAdd(f.upload(f.acc(3), 1, 4)));
        REQUIRE(extender.tryAdd(f.upload(f.acc(4), 1, 5)));
        REQUIRE(extender.getStages()[0].size() == 2);
        // The first stage is at 2,000,000 in both clusters: the next goes
        // where a stage can still grow.
        size_t added = 0;
        for (size_t i = 5; i < 12; ++i)
        {
            if (extender.tryAdd(f.upload(f.acc(i), 1, 10 + i)))
            {
                ++added;
            }
        }
        REQUIRE(validity(extender.getStages(), config) ==
                TxSetValidationResult::VALID);
        // 4,000,000 sequential instructions in two clusters per stage:
        // 8 transactions at most.
        REQUIRE(extender.sizeTx() == 8);
        REQUIRE(added == 3);
    }
}

TEST_CASE("a set selected early holds Soroban transactions and is topped up "
          "with them",
          "[herder][txset][early-txset][soroban]")
{
    EarlyFixture f(14);
    auto& app = *f.mApp;
    auto& herder = f.herder();
    overrideSorobanNetworkConfigForTest(app);
    modifySorobanNetworkConfig(
        app, [](SorobanNetworkConfig& cfg) { cfg.mLedgerMaxTxCount = 8; });
    REQUIRE(f.lm().getLastClosedSorobanNetworkConfig().ledgerMaxTxCount() == 8);
    auto closingA = f.pay(f.acc(0), 1);
    auto closing = f.setOf({closingA});
    std::vector<TransactionFrameBasePtr> early{f.upload(f.acc(1), 1, 1),
                                               f.upload(f.acc(2), 1, 2),
                                               f.upload(f.acc(3), 1, 3)};
    auto classic = f.pay(f.acc(4), 1);
    f.mMempool = {closingA, early[0], classic, early[1], early[2]};
    f.voteAndSelect(closing);

    SECTION("candidates that arrive later are added to the phase")
    {
        std::vector<TransactionFrameBasePtr> late{f.upload(f.acc(5), 1, 5),
                                                  f.upload(f.acc(6), 1, 6)};
        f.mMempool.insert(f.mMempool.end(), late.begin(), late.end());
        f.close(closing);
        auto const built = f.build();
        REQUIRE(built.mEarly);
        auto all = early;
        all.insert(all.end(), late.begin(), late.end());
        all.push_back(classic);
        REQUIRE(built.hashes() == hashesOf(all));
        REQUIRE(built.mApplicable->sizeTx(TxSetPhase::SOROBAN) == 5);
        f.closeAndCheckApplied(built);
    }
    SECTION("one that conflicts with a selected one joins its cluster")
    {
        // Writes the entry the first selected transaction writes.
        auto conflicting = f.upload(f.acc(5), 1, 1);
        f.mMempool.push_back(conflicting);
        f.close(closing);
        auto const built = f.build();
        REQUIRE(built.mEarly);
        REQUIRE(built.holds(conflicting));
        REQUIRE(built.mApplicable->sizeTx(TxSetPhase::SOROBAN) == 4);
        REQUIRE(!Access::selfCheckLatched(herder));
        f.closeAndCheckApplied(built);
    }
    SECTION("up to the phase's capacity")
    {
        for (size_t i = 5; i < 14; ++i)
        {
            f.mMempool.push_back(f.upload(f.acc(i), 1, 10 + i));
        }
        f.close(closing);
        auto const built = f.build();
        REQUIRE(built.mEarly);
        REQUIRE(built.mApplicable->sizeTx(TxSetPhase::SOROBAN) == 8);
        REQUIRE(built.holds(classic));
        f.closeAndCheckApplied(built);
    }
    SECTION("one that the closed ledger made invalid is replaced")
    {
        // Account 1 is drained by the closing ledger... which is not what
        // closes: another set does, with the same account's payment.
        f.close(closing);
        auto const built = f.build();
        REQUIRE(built.mEarly);
        f.closeAndCheckApplied(built);
        // Next round: the closing set drains account 5, whose Soroban
        // transaction the selection picks.
        auto& drainedAccount = f.acc(5);
        auto drain = f.pay(drainedAccount, 1,
                           drainedAccount.getAvailableBalance() - 100);
        auto closing2 = f.setOf({drain});
        auto doomed = f.upload(drainedAccount, 2, 50);
        std::vector<TransactionFrameBasePtr> others;
        for (size_t i = 6; i < 14; ++i)
        {
            others.push_back(f.upload(f.acc(i), 1, 60 + i));
        }
        // The selection is full with `doomed` and seven others; one more
        // is left over.
        f.mMempool = {drain, doomed};
        f.mMempool.insert(f.mMempool.end(), others.begin(), others.end());
        f.voteAndSelect(closing2);
        {
            auto job = Access::job(herder);
            std::lock_guard<std::mutex> lock(job->mMutex);
            REQUIRE(job->mSelection->mSelected == 8);
            REQUIRE(job->mSelection->mLeftovers.size() == 1);
        }
        f.close(closing2);
        f.mPulls.clear();
        auto const next = f.build();
        REQUIRE(next.mEarly);
        REQUIRE(!next.holds(doomed));
        // Full again, from what the selection had left over. The classic
        // phase, which never had a candidate, looks at the mempool's top
        // once.
        REQUIRE(next.mApplicable->sizeTx(TxSetPhase::SOROBAN) == 8);
        REQUIRE(next.hashes() == hashesOf(others));
        REQUIRE(f.mPulls == std::vector<size_t>{Access::pullCount(herder)});
        f.closeAndCheckApplied(next);
    }
}

TEST_CASE("a phase the selection could not fill is filled at the close",
          "[herder][txset][early-txset][soroban]")
{
    EarlyFixture f(14);
    auto& app = *f.mApp;
    auto& herder = f.herder();
    overrideSorobanNetworkConfigForTest(app);
    auto closingA = f.pay(f.acc(0), 1);
    auto closing = f.setOf({closingA});

    SECTION("a phase without a single candidate when the selection was made")
    {
        modifySorobanNetworkConfig(
            app, [](SorobanNetworkConfig& cfg) { cfg.mLedgerMaxTxCount = 8; });
        closingA = f.pay(f.acc(0), 1);
        closing = f.setOf({closingA});
        // Classic candidates only.
        auto classic = f.pay(f.acc(1), 1);
        f.mMempool = {closingA, classic};
        f.voteAndSelect(closing);
        f.close(closing);
        // Soroban transactions arrive during the ballot.
        std::vector<TransactionFrameBasePtr> late{f.upload(f.acc(2), 1, 2),
                                                  f.upload(f.acc(3), 1, 3)};
        f.mMempool.insert(f.mMempool.end(), late.begin(), late.end());
        auto const built = f.build();
        REQUIRE(built.mEarly);
        REQUIRE(built.hashes() == hashesOf({classic, late[0], late[1]}));
        f.closeAndCheckApplied(built);
    }
    SECTION("a selection that left out a transaction for its cluster's "
            "limit")
    {
        // Two clusters of 10,000,000 instructions. Three transactions of
        // 5,000,000 on one entry: two fill a cluster, the third has
        // nowhere to go, while the other cluster is empty.
        modifySorobanNetworkConfig(app, [](SorobanNetworkConfig& cfg) {
            cfg.mLedgerMaxTxCount = 8;
            cfg.mLedgerMaxDependentTxClusters = 2;
            cfg.mTxMaxInstructions = 10'000'000;
            cfg.mLedgerMaxInstructions = 10'000'000;
        });
        closingA = f.pay(f.acc(0), 1);
        closing = f.setOf({closingA});
        std::vector<TransactionFrameBasePtr> hot{
            f.upload(f.acc(1), 1, 7, 100, 5'000'000),
            f.upload(f.acc(2), 1, 7, 100, 5'000'000),
            f.upload(f.acc(3), 1, 7, 100, 5'000'000)};
        f.mMempool = {closingA, hot[0], hot[1], hot[2]};
        f.voteAndSelect(closing);
        {
            auto job = Access::job(herder);
            std::lock_guard<std::mutex> lock(job->mMutex);
            REQUIRE(job->mSelection->mSelected == 2);
            REQUIRE(job->mSelection->mHadTxNotFittingLane.at(1).at(0));
        }
        f.close(closing);
        // Two that write other entries arrive: they have room.
        std::vector<TransactionFrameBasePtr> late{
            f.upload(f.acc(4), 1, 8, 100, 5'000'000),
            f.upload(f.acc(5), 1, 9, 100, 5'000'000)};
        f.mMempool.insert(f.mMempool.end(), late.begin(), late.end());
        auto const built = f.build();
        REQUIRE(built.mEarly);
        REQUIRE(built.holds(late[0]));
        REQUIRE(built.holds(late[1]));
        REQUIRE(built.mApplicable->sizeTx(TxSetPhase::SOROBAN) == 4);
        REQUIRE(f.validate(built) == TxSetValidationResult::VALID);
        f.closeAndCheckApplied(built);
    }
    SECTION("many left-overs with nowhere to go do not close the phase")
    {
        // As above, with ten transactions on the one entry: eight valid
        // left-overs that fit nowhere. The phase is not full for that: two
        // that write other entries still have the second cluster.
        modifySorobanNetworkConfig(app, [](SorobanNetworkConfig& cfg) {
            cfg.mLedgerMaxTxCount = 16;
            cfg.mLedgerMaxDependentTxClusters = 2;
            cfg.mTxMaxInstructions = 10'000'000;
            cfg.mLedgerMaxInstructions = 10'000'000;
        });
        closingA = f.pay(f.acc(0), 1);
        closing = f.setOf({closingA});
        f.mMempool = {closingA};
        for (size_t i = 1; i <= 10; ++i)
        {
            f.mMempool.push_back(f.upload(f.acc(i), 1, 7, 100, 5'000'000));
        }
        f.voteAndSelect(closing);
        {
            auto job = Access::job(herder);
            std::lock_guard<std::mutex> lock(job->mMutex);
            REQUIRE(job->mSelection->mSelected == 2);
        }
        f.close(closing);
        std::vector<TransactionFrameBasePtr> late{
            f.upload(f.acc(11), 1, 8, 100, 5'000'000),
            f.upload(f.acc(12), 1, 9, 100, 5'000'000)};
        f.mMempool.insert(f.mMempool.end(), late.begin(), late.end());
        auto const built = f.build();
        REQUIRE(built.mEarly);
        REQUIRE(built.holds(late[0]));
        REQUIRE(built.holds(late[1]));
        REQUIRE(built.mApplicable->sizeTx(TxSetPhase::SOROBAN) == 4);
        REQUIRE(f.validate(built) == TxSetValidationResult::VALID);
        f.closeAndCheckApplied(built);
    }
}

// ---------------------------------------------------------------------------
// The prediction.
// ---------------------------------------------------------------------------

TEST_CASE("what the transaction set of a ledger predicts of the ledger",
          "[herder][txset][early-txset]")
{
    EarlyFixture f(4);
    auto& app = *f.mApp;
    auto& a = f.acc(0);
    auto& b = f.acc(1);
    auto& c = f.acc(2);
    auto payA = f.pay(a, 1, 5, 300);
    auto bump = feeBump(app, c, f.pay(b, 1), 500);
    PredictedAccountChanges changes;
    predictAccountChanges(*payA, changes);
    predictAccountChanges(*bump, changes);
    REQUIRE(changes.size() == 3);
    // A: its sequence number and its fee.
    REQUIRE(changes[a.getPublicKey()].mSeqNum == payA->getSeqNum());
    REQUIRE(changes[a.getPublicKey()].mFees == 300);
    // B: the sequence number of the inner transaction, no fee.
    REQUIRE(changes[b.getPublicKey()].mSeqNum == f.seqOf(b) + 1);
    REQUIRE(changes[b.getPublicKey()].mFees == 0);
    // C: the fee bump's fee, no sequence number.
    REQUIRE(changes[c.getPublicKey()].mSeqNum == 0);
    REQUIRE(changes[c.getPublicKey()].mFees == bump->getFullFee());

    auto const& lcl = f.lm().getLastClosedLedgerHeader();
    auto header = std::make_shared<LedgerHeader>(lcl.header);
    header->ledgerSeq += 1;
    auto view = makePredictedLedgerView(
        f.lm().getLastClosedLedgerState(), header,
        std::make_shared<PredictedAccountChanges const>(changes),
        app.getMetrics());
    REQUIRE(view->getLedgerHeader().current().ledgerSeq ==
            lcl.header.ledgerSeq + 1);
    auto lclView = f.lm().getLCLView();
    auto account = [&](AbstractLedgerView const& v, TestAccount& acc) {
        auto entry = v.getAccount(acc.getPublicKey());
        REQUIRE(entry);
        return entry.current().data.account();
    };
    REQUIRE(account(*view, a).seqNum == payA->getSeqNum());
    REQUIRE(account(*view, a).balance == account(*lclView, a).balance - 300);
    REQUIRE(account(*view, b).seqNum == account(*lclView, b).seqNum + 1);
    REQUIRE(account(*view, b).balance == account(*lclView, b).balance);
    REQUIRE(account(*view, c).seqNum == account(*lclView, c).seqNum);
    REQUIRE(account(*view, c).balance ==
            account(*lclView, c).balance - bump->getFullFee());
    // An account the set does not name is the last closed ledger's.
    REQUIRE(account(*view, f.acc(3)) == account(*lclView, f.acc(3)));
    // An account that does not exist does not appear.
    REQUIRE(
        !view->getAccount(SecretKey::pseudoRandomForTesting().getPublicKey()));
    // Nothing is predicted about other entries.
    LedgerKey code(CONTRACT_CODE);
    REQUIRE_THROWS_AS(view->load(code), std::logic_error);
}

// ---------------------------------------------------------------------------
// In a network.
// ---------------------------------------------------------------------------

TEST_CASE("leaders select their next set early in a network",
          "[herder][txset][early-txset][simulation]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation = Topologies::core(4, 0.75, networkID, [](int i) {
        auto cfg = getTestConfig(i, Config::TESTDB_BUCKET_DB_PERSISTENT);
        cfg.LEDGER_PROTOCOL_VERSION = GATE_PROTOCOL;
        cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = GATE_PROTOCOL;
        cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
        return cfg;
    });
    auto nodes = simulation->getNodes();
    simulation->startAllNodes();

    // Every node's mempool holds the root account's next payments: one fits
    // each ledger (one transaction per source account), and the one after it
    // only becomes valid when that ledger has applied. A leader that selects
    // early gets the next one right only by predicting what the ledger in
    // its ballot does.
    auto const rootKey = nodes[0]->getRoot()->getSecretKey();
    auto const rootID = rootKey.getPublicKey();
    auto const firstSeq = nodes[0]->getRoot()->getLastSequenceNumber();
    std::vector<TransactionEnvelope> payments;
    for (int i = 1; i <= 400; ++i)
    {
        payments.push_back(transactionFromOperations(*nodes[0], rootKey,
                                                     firstSeq + i,
                                                     {payment(rootID, 1)}, 100)
                               ->getEnvelope());
    }
    auto rootSeq = [&](Application& app) {
        auto root = app.getLedgerManager().getLCLView()->getAccount(rootID);
        REQUIRE(root);
        return root.current().data.account().seqNum;
    };
    for (auto const& node : nodes)
    {
        auto* app = node.get();
        auto& herder = static_cast<HerderImpl&>(app->getHerder());
        // Virtual time runs ahead of the job's threads: start at the vote.
        Access::setStartDelay(herder, std::chrono::milliseconds(0));
        herder.mGetTopTransactionsForTesting = [&, app](size_t) {
            std::vector<TransactionEnvelope> top;
            auto const next = static_cast<size_t>(rootSeq(*app) - firstSeq);
            for (size_t i = next; i < next + 4 && i < payments.size(); ++i)
            {
                top.push_back(payments[i]);
            }
            return top;
        };
    }

    auto& lm = nodes[1]->getLedgerManager();
    uint32_t const target = lm.getLastClosedLedgerNum() + 40;
    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(target, 2); },
        80 * simulation->getExpectedLedgerCloseTime(), false);

    int64_t planned = 0;
    int64_t completed = 0;
    int64_t notReady = 0;
    int64_t failed = 0;
    int64_t selfCheckFailed = 0;
    for (auto const& node : nodes)
    {
        planned += txsetMeter(*node, "early-planned");
        completed += txsetMeter(*node, "early-completed");
        notReady += txsetMeter(*node, "early-not-ready") +
                    txsetMeter(*node, "early-not-started");
        failed += txsetMeter(*node, "early-failed");
        selfCheckFailed += txsetMeter(*node, "self-check-failed");
        // No leader proposed a set that apply found invalid.
        REQUIRE(applyCounter(*node, "txset-skipped") == 0);
    }
    INFO(fmt::format("planned {}, completed {}, not ready {}", planned,
                     completed, notReady));
    REQUIRE(failed == 0);
    REQUIRE(selfCheckFailed == 0);
    REQUIRE(planned > 0);
    // How often the selection is ready depends on the machine (the jobs run
    // in real time, the network in virtual time).
    REQUIRE(completed + notReady > 0);
    // The ledgers carried the payments.
    REQUIRE(rootSeq(*nodes[1]) - firstSeq >= 20);
    for (auto const& node : nodes)
    {
        static_cast<HerderImpl&>(node->getHerder())
            .mGetTopTransactionsForTesting = nullptr;
    }
}

// What the leader's path to its next set costs at perfnet's scale, with real
// SAC payments (never validated by Core before: the Rust overlay admitted
// them): a build as it is now, and the completion of a set that was selected
// while the previous ledger was in its ballot, with a share of the candidates
// arriving only after the selection's pull.
// Env: BENCH_CAP (transactions per set, default 8000), BENCH_LATE_PERCENT
// (share of a ledger's candidates that arrive after the selection's pull,
// default 30), BENCH_BACKLOG (1: the mempool holds twice the capacity at the
// build, as in a backlog; default 0), BENCH_REPS (default 2), BENCH_TASKS
// (validation tasks, default 16), BENCH_CLUSTERS (default 16).
TEST_CASE("early tx set completion costs", "[herder][.][benchmark]")
{
    auto envInt = [](char const* name, uint32_t dflt) {
        auto v = std::getenv(name);
        return v ? static_cast<uint32_t>(std::stoul(v)) : dflt;
    };
    uint32_t const cap = envInt("BENCH_CAP", 8000);
    uint32_t const latePercent = envInt("BENCH_LATE_PERCENT", 30);
    bool const backlog = envInt("BENCH_BACKLOG", 0) != 0;
    uint32_t const reps = envInt("BENCH_REPS", 2);
    uint32_t const tasks = envInt("BENCH_TASKS", 16);
    uint32_t const clusters = envInt("BENCH_CLUSTERS", 16);
    // Per repetition: a closing ledger and the next one's candidates (twice
    // as many in a backlog), at 80% of the cap as on perfnet.
    uint32_t const perLedger = cap * 8 / 10;
    uint32_t const nextCount = backlog ? 2 * cap : perLedger;
    uint32_t const perRep = perLedger + nextCount;
    uint32_t const candidates = (reps + 1) * perRep;

    auto cfg = getTestConfig(0, Config::TESTDB_BUCKET_DB_PERSISTENT);
    cfg.APPLY_LOAD_MODE = ApplyLoadMode::BENCHMARK_MODEL_TX;
    cfg.APPLY_LOAD_MODEL_TX = ApplyLoadModelTx::SAC;
    cfg.USE_CONFIG_FOR_GENESIS = true;
    cfg.LEDGER_PROTOCOL_VERSION = Config::CURRENT_LEDGER_PROTOCOL_VERSION;
    cfg.IGNORE_MESSAGE_LIMITS_FOR_TESTING = true;
    cfg.APPLY_LOAD_MAX_SOROBAN_TX_COUNT = cap;
    cfg.APPLY_LOAD_BATCH_SAC_COUNT = 1;
    cfg.APPLY_LOAD_LEDGER_MAX_DEPENDENT_TX_CLUSTERS = clusters;
    cfg.APPLY_LOAD_CLASSIC_TXS_PER_LEDGER = 0;
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 100;
    // ApplyLoad makes one account per candidate it is configured for.
    cfg.SOROBAN_TRANSACTION_QUEUE_SIZE_MULTIPLIER = candidates / cap + 1;
    cfg.GENESIS_TEST_ACCOUNT_COUNT =
        cap * cfg.SOROBAN_TRANSACTION_QUEUE_SIZE_MULTIPLIER + 10;
    cfg.EXPERIMENTAL_SPECULATIVE_LEDGER_APPLY = false;
    cfg.DISABLE_TX_META_FOR_TESTING = true;
    cfg.METADATA_DEBUG_LEDGERS = 0;
    cfg.WORKER_THREADS = static_cast<int>(tasks) + 2;
    VirtualClock clock(VirtualClock::REAL_TIME);
    auto app = createTestApplication(clock, cfg);
    auto& herder = static_cast<HerderImpl&>(app->getHerder());
    auto& lm = app->getLedgerManager();
    auto& metrics = app->getMetrics();
    Access::setStartDelay(herder, std::chrono::milliseconds(0));

    std::vector<TransactionEnvelope> envelopes;
    {
        ApplyLoad al(*app);
        envelopes = al.generateSacPaymentEnvelopesForTesting(candidates);
    }
    REQUIRE(envelopes.size() == candidates);
    app->getBatchExecutor().setPreferredTaskCountForTesting(tasks);
    REQUIRE(lm.getLastClosedSorobanNetworkConfig().ledgerMaxTxCount() == cap);

    // The mempool: a range of the envelopes, handed over as the overlay
    // encodes a pull ([count:4]([len:4][xdr])*), encoded ahead of time.
    std::vector<xdr::opaque_vec<>> encoded;
    encoded.reserve(envelopes.size());
    for (auto const& env : envelopes)
    {
        encoded.emplace_back(xdr::xdr_to_opaque(env));
    }
    std::vector<size_t> mempool;
    herder.mGetTopTransactionsPayloadForTesting = [&](size_t count) {
        uint32_t const n =
            static_cast<uint32_t>(std::min(count, mempool.size()));
        std::vector<uint8_t> payload(4);
        std::memcpy(payload.data(), &n, 4);
        for (uint32_t i = 0; i < n; ++i)
        {
            auto const& bytes = encoded[mempool[i]];
            uint32_t const len = static_cast<uint32_t>(bytes.size());
            auto const at = payload.size();
            payload.resize(at + 4 + len);
            std::memcpy(payload.data() + at, &len, 4);
            std::memcpy(payload.data() + at + 4, bytes.data(), len);
        }
        return payload;
    };
    auto range = [&](size_t from, size_t count) {
        std::vector<size_t> res(count);
        std::iota(res.begin(), res.end(), from);
        return res;
    };
    auto nextCloseTime = [&]() {
        auto const& lcl = lm.getLastClosedLedgerHeader();
        return getConsensusTime(lcl.header.scpValue)
            .next(lcl.header.ledgerVersion);
    };
    auto crankUntil = [&](std::function<bool()> done) {
        auto const deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(120);
        while (!done())
        {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            clock.crank(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    std::vector<std::string> const stages = {
        "build-pull",    "build-frames",          "build-refill-check",
        "build-trim",    "build-select",          "early-final-pass",
        "early-top-up",  "build-selection-check", "build-xdr",
        "build-handoff", "early-complete",        "build",
        "early-pull",    "early-select",          "early-job"};
    auto clearStages = [&]() {
        for (auto const& stage : stages)
        {
            metrics.NewTimer({"herder", "txset", stage}).Clear();
        }
    };
    std::map<std::string, std::map<std::string, std::vector<double>>> samples;
    auto record = [&](std::string const& what) {
        for (auto const& stage : stages)
        {
            auto& timer = metrics.NewTimer({"herder", "txset", stage});
            if (timer.count() > 0)
            {
                samples[what][stage].push_back(timer.sum());
            }
        }
    };

    std::cout << "=== leader's next set: cap " << cap << ", " << perLedger
              << " per ledger, " << (backlog ? "backlog, " : "") << latePercent
              << "% arriving after the selection's pull, " << tasks
              << " validation tasks, " << clusters
              << " clusters ===" << std::endl;
    size_t included = 0;
    size_t builtSize = 0;
    for (uint32_t rep = 0; rep < reps; ++rep)
    {
        size_t const base = rep * perRep;
        auto const closingTxs = range(base, perLedger);
        auto const nextTxs = range(base + perLedger, nextCount);
        size_t const earlyCount = nextCount * (100 - latePercent) / 100;

        // The closing ledger's set, built from the mempool: a plain build.
        // In a backlog the mempool holds twice the cap behind it.
        mempool = closingTxs;
        if (backlog)
        {
            auto more =
                range(base + perLedger, std::min<size_t>(nextCount, 2 * cap));
            mempool.insert(mempool.end(), more.begin(), more.end());
        }
        PubKeyUtils::clearVerifySigCache();
        clearStages();
        auto closing = Access::build(herder, lm.getLastClosedLedgerNum() + 1,
                                     nextCloseTime());
        record("1. build, cold signature cache");
        clearStages();
        auto closing2 = Access::build(herder, lm.getLastClosedLedgerNum() + 1,
                                      nextCloseTime());
        record("2. build, warm signature cache");
        builtSize = closing.txSet->sizeTxTotal();

        // The node votes for it; its early selection for the ledger after
        // sees the closing ledger's transactions and the early arrivals.
        mempool = closingTxs;
        mempool.insert(mempool.end(), nextTxs.begin(),
                       nextTxs.begin() + earlyCount);
        auto const seq = lm.getLastClosedLedgerNum() + 1;
        auto const closeTime = nextCloseTime();
        herder.getPendingEnvelopes().putTxSet(closing.txSet->getContentsHash(),
                                              seq, closing.txSet);
        SCPEnvelope envelope;
        envelope.statement.slotIndex = seq;
        envelope.statement.pledges.type(SCP_ST_PREPARE);
        envelope.statement.pledges.prepare().ballot.counter = 1;
        envelope.statement.pledges.prepare().ballot.value = xdr::xdr_to_opaque(
            herder.makeStellarValue(closing.txSet->getContentsHash(), closeTime,
                                    emptyUpgradeSteps, cfg.NODE_SEED));
        clearStages();
        Access::voted(herder, envelope);
        REQUIRE(Access::planned(herder));
        crankUntil([&]() { return Access::job(herder) != nullptr; });
        auto job = Access::job(herder);
        crankUntil([&]() { return job->mDone.load(); });
        samples["3. early selection (background, during the ballot)"]
               ["job, wall time"]
                   .push_back(static_cast<double>(job->mFinishedAfterNs) / 1e6);
        record("3. early selection (background, during the ballot)");

        // The ledger closes; the rest of the candidates has arrived.
        app->getHerder().externalizeValue(closing.txSet, seq, closeTime,
                                          emptyUpgradeSteps);
        REQUIRE(lm.getLastClosedLedgerNum() == seq);
        mempool = nextTxs;
        clearStages();
        auto const completedBefore = txsetMeter(*app, "early-completed");
        auto next = Access::build(herder, lm.getLastClosedLedgerNum() + 1,
                                  nextCloseTime());
        REQUIRE(txsetMeter(*app, "early-completed") == completedBefore + 1);
        record("4. completion at the close");
        included = next.txSet->sizeTxTotal();
        REQUIRE(next.applicableTxSet->checkValid(*app, 1, 1));
        REQUIRE(included == std::min<size_t>(cap, nextCount));
    }
    std::cout << "plain build: " << builtSize
              << " transactions; completed set: " << included << " transactions"
              << std::endl;
    for (auto& [what, byStage] : samples)
    {
        std::cout << what << std::endl;
        for (auto& [stage, v] : byStage)
        {
            std::sort(v.begin(), v.end());
            std::cout << "  " << stage << ": median " << v[v.size() / 2]
                      << " ms (min " << v.front() << ", max " << v.back()
                      << ", n=" << v.size() << ")" << std::endl;
        }
    }
    herder.mGetTopTransactionsPayloadForTesting = nullptr;
}
