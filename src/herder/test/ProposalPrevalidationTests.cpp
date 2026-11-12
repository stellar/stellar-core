// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// Tests of the leader's head start on the next transaction set: the state a
// speculative execution leaves is handed to the Herder
// (LedgerManager::startSpeculativeApply), the node that expects to lead the
// next ledger validates its candidates against it in the background
// (HerderImpl::startProposalPrevalidation), and the build of the next set
// takes those results instead of validating again (TxValidationCache).
//
// And of what the first in line does beyond that: it selects the next set on
// that state while the ledger is still in its ballot, completes it when the
// ledger has closed (HerderImpl::prepareEarlyTxSet), executes its own set as
// soon as it is prepared (HerderImpl::executePreparedTxSet), and checks the
// sets it builds in the background (TxSetSelfCheck).

#include "crypto/SHA.h"
#include "herder/Herder.h"
#include "herder/HerderImpl.h"
#include "herder/HerderUtils.h"
#include "herder/LeaderSchedule.h"
#include "herder/LedgerCloseData.h"
#include "herder/TxSetFrame.h"
#include "herder/test/TestTxSetUtils.h"
#include "ledger/ImmutableLedgerView.h"
#include "ledger/LedgerHeaderUtils.h"
#include "ledger/LedgerManager.h"
#include "ledger/test/LedgerTestUtils.h"
#include "main/Application.h"
#include "medida/meter.h"
#include "simulation/Simulation.h"
#include "simulation/Topologies.h"
#include "test/Catch2.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "util/MetricsRegistry.h"
#include "util/ProtocolVersion.h"
#include "util/XDROperators.h"
#include <chrono>
#include <fmt/format.h>
#include <set>
#include <thread>

using namespace stellar;
using namespace stellar::txtest;

namespace stellar
{
class ProposalPrevalidationTestAccess
{
  public:
    // What HerderImpl::maybeStartSpeculativeApply records when the node
    // votes for `value`.
    static void
    setVotedValue(HerderImpl& herder, uint64_t slot, StellarValue const& value)
    {
        herder.mSpeculativeApplyValue =
            std::make_pair(slot, Value(xdr::xdr_to_opaque(value)));
    }

    static void
    start(HerderImpl& herder, std::shared_ptr<ExecutedLedgerState const> state)
    {
        herder.startProposalPrevalidation(std::move(state));
    }

    static bool
    running(HerderImpl& herder)
    {
        return herder.mProposalPrevalidation != nullptr;
    }

    // Waits for the background tasks of the running pre-validation.
    static void
    waitUntilFinished(HerderImpl& herder)
    {
        auto job = herder.mProposalPrevalidation;
        REQUIRE(job);
        auto const deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (job->finishedAfterNs.load() < 0)
        {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    static std::pair<TxSetXDRFrameConstPtr, ApplicableTxSetFrameConstPtr>
    build(HerderImpl& herder, uint32_t ledgerSeq, ConsensusTime closeTime)
    {
        auto built = herder.buildTxSet(ledgerSeq, closeTime, false);
        return std::make_pair(built.txSet, std::move(built.applicableTxSet));
    }

    static size_t
    cachedResults(HerderImpl& herder)
    {
        return herder.mValidationCache.results.size();
    }

    static void
    clearCache(HerderImpl& herder)
    {
        herder.mValidationCache = HerderImpl::ValidationCache{};
    }

    static bool
    mayLeadAfter(HerderImpl& herder, uint64_t slot, StellarValue const& value,
                 bool appliedTransactions)
    {
        return herder.mayLeadAfter(slot, value, appliedTransactions);
    }

    static std::chrono::milliseconds
    prepareLeadTime(std::chrono::milliseconds expectedCloseTime,
                    std::chrono::milliseconds lastBuild)
    {
        return HerderImpl::prepareTxSetLeadTime(expectedCloseTime, lastBuild);
    }

    // What the node expects its next build to take, given what it knows.
    static std::chrono::milliseconds
    expectedBuild(HerderImpl& herder)
    {
        return herder.expectedTxSetBuildDuration();
    }

    static void
    setBuildHistory(HerderImpl& herder, std::chrono::milliseconds lastBuild,
                    std::chrono::nanoseconds costPerTx)
    {
        herder.mLastTxSetBuildDuration = lastBuild;
        herder.mTxSetBuildCostPerTx = costPerTx;
    }

    static void
    setLastClosedTxSetSize(HerderImpl& herder, size_t txs)
    {
        herder.mLastClosedTxSetSize = txs;
    }

    static size_t
    lastClosedTxSetSize(HerderImpl& herder)
    {
        return herder.mLastClosedTxSetSize;
    }

    // Whether the running pre-validation goes on to select a set.
    static bool
    selectsEarly(HerderImpl& herder)
    {
        return herder.mProposalPrevalidation &&
               herder.mProposalPrevalidation->selectEarly;
    }

    // Waits until the running pre-validation has selected its set, or has
    // given up on it.
    static void
    waitUntilSelected(HerderImpl& herder)
    {
        auto job = herder.mProposalPrevalidation;
        REQUIRE(job);
        REQUIRE(job->selectEarly);
        auto const deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!job->selectionDone.load())
        {
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    static bool
    hasEarlySet(HerderImpl& herder)
    {
        auto job = herder.mProposalPrevalidation;
        if (!job)
        {
            return false;
        }
        std::lock_guard<std::mutex> lock(job->mutex);
        return job->early != nullptr;
    }

    // Lets `f` change the selected set before the node looks at it.
    template <typename F>
    static void
    withEarlySet(HerderImpl& herder, F&& f)
    {
        auto job = herder.mProposalPrevalidation;
        REQUIRE(job);
        std::lock_guard<std::mutex> lock(job->mutex);
        REQUIRE(job->early);
        f(*job->early);
    }

    // The transactions the running job decoded as candidates of a set.
    static size_t
    pulledByJob(HerderImpl& herder)
    {
        REQUIRE(herder.mProposalPrevalidation);
        return herder.mProposalPrevalidation->pulled.size();
    }

    // The close time the running job's set is validated for, as an offset
    // from the executed ledger's.
    static uint64_t
    assumedCloseTimeOffset(HerderImpl& herder)
    {
        REQUIRE(herder.mProposalPrevalidation);
        return herder.mProposalPrevalidation->assumedCloseTimeOffset;
    }

    static bool
    prepareEarly(HerderImpl& herder, uint32_t ledgerSeq,
                 ConsensusTime closeTime)
    {
        return herder.prepareEarlyTxSet(ledgerSeq, closeTime);
    }

    // What the first-round leader does a lead time before the trigger.
    static void
    prepareByBuilding(HerderImpl& herder, uint32_t ledgerSeq,
                      ConsensusTime closeTime)
    {
        herder.prepareTxSet(ledgerSeq, closeTime);
    }

    static TxSetXDRFrameConstPtr
    prepared(HerderImpl& herder)
    {
        return herder.mPreparedTxSet ? herder.mPreparedTxSet->txSet : nullptr;
    }

    static std::vector<Hash>
    preparedInvalidTxs(HerderImpl& herder)
    {
        REQUIRE(herder.mPreparedTxSet);
        return herder.mPreparedTxSet->invalidTxHashes;
    }

    // What the node proposes when the trigger fires.
    static ValueWrapperPtr
    propose(HerderImpl& herder, uint32_t ledgerSeq)
    {
        return herder.makeProposalValue(ledgerSeq);
    }

    // The (slot, value) last handed to the apply thread by the herder.
    static std::optional<std::pair<uint64, Value>> const&
    speculativeValue(HerderImpl& herder)
    {
        return herder.mSpeculativeApplyValue;
    }

    // What the herder does when the node emits `envelope`.
    static void
    emitted(HerderImpl& herder, SCPEnvelope const& envelope)
    {
        herder.maybeStartSpeculativeApply(envelope);
    }

    static bool
    selfCheckHasFailed(HerderImpl& herder)
    {
        return herder.mTxSetSelfCheckFailed;
    }

    static bool
    buildMustStartNow(bool triggerDue, VirtualClock::time_point prepareAt,
                      VirtualClock::time_point now)
    {
        return HerderImpl::buildMustStartNow(triggerDue, prepareAt, now);
    }

    // What setupTriggerNextLedger does for the first-round leader once it
    // knows the trigger's time.
    static void
    schedulePreparation(HerderImpl& herder, uint32_t ledgerSeq,
                        VirtualClock::time_point triggerTime, bool triggerDue,
                        ConsensusTime minCloseTime)
    {
        herder.scheduleTxSetPreparation(
            ledgerSeq, triggerTime, triggerDue, herder.mApp.getClock().now(),
            herder.mLedgerManager.getExpectedLedgerCloseTime(), minCloseTime);
    }

    // Whether a build is scheduled for a lead time before the trigger.
    static bool
    buildScheduled(HerderImpl& herder)
    {
        return herder.mPrepareTxSetTimer.seq() != 0;
    }

    static void
    discardPrepared(HerderImpl& herder)
    {
        herder.discardPreparedTxSet();
    }

    static std::chrono::milliseconds
    prepareLeadTime(HerderImpl& herder)
    {
        return HerderImpl::prepareTxSetLeadTime(
            herder.mLedgerManager.getExpectedLedgerCloseTime(),
            herder.expectedTxSetBuildDuration());
    }

    static std::chrono::milliseconds
    minBuildSaving()
    {
        return HerderImpl::EARLY_TX_SET_MIN_BUILD_SAVING;
    }
};
}

namespace
{
using Access = ProposalPrevalidationTestAccess;

uint32_t const GATE_PROTOCOL =
    static_cast<uint32_t>(APPLY_TIME_TX_SET_VALIDATION_PROTOCOL_VERSION);

int64_t
meter(Application& app, std::string const& domain, std::string const& name,
      std::string const& unit)
{
    return static_cast<int64_t>(
        app.getMetrics().NewMeter({"herder", domain, name}, unit).count());
}

int64_t
applyCounter(Application& app, std::string const& name)
{
    return app.getMetrics().NewCounter({"ledger", "apply", name}).count();
}

struct CacheCounts
{
    int64_t mHit;
    int64_t mMiss;
};

CacheCounts
cacheCounts(Application& app)
{
    return CacheCounts{meter(app, "txset", "validation-cache-hit", "tx"),
                       meter(app, "txset", "validation-cache-miss", "tx")};
}

TxSetXDRFrameConstPtr
makeUnvalidatedTxSet(Application& app,
                     std::vector<TransactionFrameBasePtr> const& txs)
{
    auto res = testtxset::makeNonValidatedGeneralizedTxSet(
        {{std::make_pair(std::nullopt, txs)}, {}}, app,
        app.getLedgerManager().getLastClosedLedgerHeader().hash);
    REQUIRE(res.first);
    return res.first;
}

TransactionFrameBasePtr
paymentWithSeq(Application& app, TestAccount& from, SequenceNumber seq,
               int64_t amount = 1)
{
    return transactionFromOperations(
        app, from.getSecretKey(), seq,
        {payment(app.getRoot()->getPublicKey(), amount)}, 100);
}

std::set<Hash>
txHashes(ApplicableTxSetFrameConstPtr const& txSet)
{
    std::set<Hash> res;
    for (auto const& phase : txSet->getPhases())
    {
        for (auto const& tx : phase)
        {
            res.insert(tx->getFullHash());
        }
    }
    return res;
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

// A single validator (so it leads every ledger) whose ledgers apply on the
// ledger apply thread, with four funded accounts. Speculation and
// pre-validation are driven by the test, one step at a time.
struct PrevalidationFixture
{
    VirtualClock mClock;
    Config mCfg;
    Application::pointer mApp;
    std::optional<TestAccount> mA;
    std::optional<TestAccount> mB;
    std::optional<TestAccount> mC;
    std::optional<TestAccount> mD;
    std::optional<TestAccount> mE;
    // What the mempool holds; the Herder pulls it in this order.
    std::vector<TransactionFrameBasePtr> mMempool;
    // How often the Herder pulled it.
    size_t mPulls{0};

    // With `herderSpeculates` the Herder hands ledgers to the apply thread
    // itself, where it would in production; the tests still drive the
    // speculation of the ledger they close first.
    explicit PrevalidationFixture(
        bool herderSpeculates = false,
        std::optional<uint32_t> maxTxSetSize = std::nullopt)
        : mCfg(getTestConfig(0, Config::TESTDB_BUCKET_DB_PERSISTENT))
    {
        mCfg.LEDGER_PROTOCOL_VERSION = GATE_PROTOCOL;
        mCfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = GATE_PROTOCOL;
        if (maxTxSetSize)
        {
            mCfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = *maxTxSetSize;
        }
        // The tests drive speculation themselves.
        mCfg.EXPERIMENTAL_SPECULATIVE_LEDGER_APPLY = herderSpeculates;
        REQUIRE(mCfg.backgroundLedgerApply());
        mApp = createTestApplication(mClock, mCfg);
        auto root = mApp->getRoot();
        auto const balance =
            mApp->getLedgerManager().getLastMinBalance(2) + 10'000'000;
        mA.emplace(root->create("A", balance));
        mB.emplace(root->create("B", balance));
        mC.emplace(root->create("C", balance));
        mD.emplace(root->create("D", balance));
        mE.emplace(root->create("E", balance));
        // The sets of these tests hold a few transactions: use them even
        // though a build of that size would be quick.
        herder().mEarlyTxSetMinBuildSavingForTesting =
            std::chrono::milliseconds(0);
        herder().mGetTopTransactionsForTesting = [this](size_t) {
            ++mPulls;
            std::vector<TransactionEnvelope> envelopes;
            for (auto const& tx : mMempool)
            {
                envelopes.push_back(tx->getEnvelope());
            }
            return envelopes;
        };
    }

    ~PrevalidationFixture()
    {
        herder().mGetTopTransactionsForTesting = nullptr;
        herder().mBuiltTxSetOverrideForTesting = nullptr;
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

    // Speculates on `closing` as the next ledger, lets the node select the
    // set for the ledger after it, and closes the ledger with `closing`.
    void
    closeWithEarlySelection(TxSetXDRFrameConstPtr const& closing)
    {
        REQUIRE(speculate(closing, true));
        REQUIRE(Access::selectsEarly(herder()));
        Access::waitUntilFinished(herder());
        Access::waitUntilSelected(herder());
        REQUIRE(Access::hasEarlySet(herder()));
        externalize(closing);
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

    StellarValue
    valueFor(TxSetXDRFrameConstPtr const& txSet,
             xdr::xvector<UpgradeType, 6> const& upgrades = emptyUpgradeSteps)
    {
        return herder().makeStellarValue(txSet->getContentsHash(),
                                         nextCloseTime(), upgrades,
                                         mCfg.NODE_SEED);
    }

    // Executes the next ledger with `txSet` on the apply thread without
    // closing it, as a node does when it votes for the value, and returns
    // the state the execution left.
    std::shared_ptr<ExecutedLedgerState const>
    speculate(TxSetXDRFrameConstPtr const& txSet, bool prevalidate,
              xdr::xvector<UpgradeType, 6> const& upgrades = emptyUpgradeSteps)
    {
        auto const value = valueFor(txSet, upgrades);
        // By the time a node votes, SCP has asked for the slot's leaders,
        // which brings the tracked quorum (the node set) up to date.
        REQUIRE(herder().getHerderSCPDriver().getLeaderSchedule(nextSeq()) ==
                std::vector<NodeID>{mCfg.NODE_SEED.getPublicKey()});
        Access::setVotedValue(herder(), nextSeq(), value);
        std::shared_ptr<ExecutedLedgerState const> executed;
        bool called = false;
        lm().startSpeculativeApply(
            LedgerCloseData(nextSeq(), txSet, value),
            [&](std::shared_ptr<ExecutedLedgerState const> state) {
                called = true;
                executed = state;
                if (prevalidate)
                {
                    Access::start(herder(), state);
                }
            });
        while (lm().isSpeculativelyApplying())
        {
            mClock.crank(true);
        }
        REQUIRE(called);
        return executed;
    }

    // Externalizes the value `speculate` used for `txSet`, which closes the
    // ledger.
    void
    externalize(TxSetXDRFrameConstPtr const& txSet)
    {
        mApp->getHerder().externalizeValue(txSet, nextSeq(), nextCloseTime(),
                                           emptyUpgradeSteps);
    }

    std::set<Hash>
    build()
    {
        auto built = Access::build(herder(), nextSeq(), nextCloseTime());
        REQUIRE(built.second);
        return txHashes(built.second);
    }
};

AccountEntry
accountIn(AbstractLedgerView const& view, TestAccount& acc)
{
    auto entry = view.getAccount(acc.getPublicKey());
    REQUIRE(entry);
    return entry.current().data.account();
}
}

TEST_CASE("speculative apply hands over the state it executed",
          "[ledger][speculative-apply][prevalidation]")
{
    PrevalidationFixture f;
    auto& app = *f.mApp;
    auto& lm = f.lm();
    auto& a = *f.mA;
    auto& b = *f.mB;
    auto const aSeq = a.getLastSequenceNumber();
    auto const aBalance = a.getBalance();
    auto const bSeq = b.getLastSequenceNumber();
    auto const seq = f.nextSeq();
    auto const lclBefore = lm.getLastClosedLedgerHeader();

    SECTION("a ledger that applies")
    {
        auto tx = paymentWithSeq(app, a, aSeq + 1);
        auto txSet = makeUnvalidatedTxSet(app, {tx});
        auto const value = f.valueFor(txSet);
        auto state = f.speculate(txSet, false);
        REQUIRE(state);
        REQUIRE(state->mLedgerSeq == seq);
        REQUIRE(state->mValue == value);
        REQUIRE(!state->mTxSetSkipped);
        REQUIRE(state->mAppliedTxs == 1);
        REQUIRE(state->mTxHashes->size() == 1);
        REQUIRE(state->mTxHashes->count(tx->getFullHash()) == 1);
        REQUIRE(state->mHeader->ledgerSeq == seq);
        REQUIRE(state->mHeader->scpValue == value);
        REQUIRE(state->mHeader->previousLedgerHash == lclBefore.hash);

        // The view shows the ledger as executed: A paid and its sequence
        // number moved; B is read through from the previous ledger.
        auto view = state->makeView(app.getMetrics());
        REQUIRE(view->getLedgerHeader().current().ledgerSeq == seq);
        auto const executedA = accountIn(*view, a);
        REQUIRE(executedA.seqNum == aSeq + 1);
        REQUIRE(executedA.balance == aBalance - 100 - 1);
        REQUIRE(accountIn(*view, b).seqNum == bSeq);
        // ... while the last closed ledger is untouched.
        REQUIRE(lm.getLastClosedLedgerHeader().hash == lclBefore.hash);
        REQUIRE(accountIn(*lm.getLCLView(), a).seqNum == aSeq);

        // Once the value externalizes, the closed ledger holds what the
        // captured state said, and the state itself did not move.
        f.externalize(txSet);
        REQUIRE(lm.getLastClosedLedgerNum() == seq);
        auto const closedA = accountIn(*lm.getLCLView(), a);
        REQUIRE(closedA.seqNum == executedA.seqNum);
        REQUIRE(closedA.balance == executedA.balance);
        REQUIRE(accountIn(*view, a).seqNum == aSeq + 1);
        REQUIRE(applyCounter(app, "speculation-hit") == 1);
    }

    SECTION("an erased entry")
    {
        auto merge = transactionFromOperations(
            app, a.getSecretKey(), aSeq + 1,
            {accountMerge(app.getRoot()->getPublicKey())}, 100);
        auto state = f.speculate(makeUnvalidatedTxSet(app, {merge}), false);
        REQUIRE(state);
        REQUIRE(state->mAppliedTxs == 1);
        auto view = state->makeView(app.getMetrics());
        REQUIRE(!view->getAccount(a.getPublicKey()));
        REQUIRE(lm.getLCLView()->getAccount(a.getPublicKey()));
    }

    SECTION("a ledger whose transaction set is invalid")
    {
        auto good = paymentWithSeq(app, a, aSeq + 1);
        auto bad = paymentWithSeq(app, b, bSeq + 10);
        auto state = f.speculate(makeUnvalidatedTxSet(app, {good, bad}), false);
        REQUIRE(state);
        REQUIRE(state->mTxSetSkipped);
        REQUIRE(state->mAppliedTxs == 0);
        REQUIRE(state->mTxHashes->empty());
        auto view = state->makeView(app.getMetrics());
        REQUIRE(accountIn(*view, a).seqNum == aSeq);
        REQUIRE(accountIn(*view, a).balance == aBalance);
    }

    SECTION("a ledger that is not the next one is not executed")
    {
        auto txSet =
            makeUnvalidatedTxSet(app, {paymentWithSeq(app, a, aSeq + 1)});
        auto const value = f.valueFor(txSet);
        bool called = false;
        std::shared_ptr<ExecutedLedgerState const> state;
        lm.startSpeculativeApply(
            LedgerCloseData(seq + 1, txSet, value),
            [&](std::shared_ptr<ExecutedLedgerState const> s) {
                called = true;
                state = s;
            });
        while (lm.isSpeculativelyApplying())
        {
            f.mClock.crank(true);
        }
        REQUIRE(called);
        REQUIRE(!state);
    }
}

TEST_CASE("leader pre-validates the candidates of the next ledger",
          "[herder][txset][prevalidation]")
{
    PrevalidationFixture f;
    auto& app = *f.mApp;
    auto& herder = f.herder();
    auto& a = *f.mA;
    auto& b = *f.mB;
    auto& c = *f.mC;
    auto& d = *f.mD;
    auto const aSeq = a.getLastSequenceNumber();
    auto const bSeq = b.getLastSequenceNumber();
    auto const cSeq = c.getLastSequenceNumber();
    auto const dSeq = d.getLastSequenceNumber();

    // The ledger being closed (N) holds a payment of A and one of D.
    auto closingA = paymentWithSeq(app, a, aSeq + 1);
    auto closingD = paymentWithSeq(app, d, dSeq + 1);
    // Candidates for the ledger after it:
    //  * A's next payment: only valid once N has applied,
    //  * another payment of D with the sequence number N consumes: valid
    //    before N, not after,
    //  * B's payment: valid either way,
    //  * C's payment with a sequence number too far ahead: never valid.
    auto nextA = paymentWithSeq(app, a, aSeq + 2);
    auto staleD = paymentWithSeq(app, d, dSeq + 1, 2);
    auto plainB = paymentWithSeq(app, b, bSeq + 1);
    auto futureC = paymentWithSeq(app, c, cSeq + 5);
    REQUIRE(staleD->getFullHash() != closingD->getFullHash());

    auto closing = makeUnvalidatedTxSet(app, {closingA, closingD});
    // N's transactions are still in the mempool while N is in its ballot.
    f.mMempool = {closingA, closingD, nextA, staleD, plainB, futureC};
    auto const startedBefore =
        meter(app, "prevalidate", "started", "prevalidation");
    auto const usedBefore = meter(app, "prevalidate", "used", "prevalidation");
    auto const discardedBefore =
        meter(app, "prevalidate", "discarded", "prevalidation");

    auto state = f.speculate(closing, true);
    REQUIRE(state);
    REQUIRE(Access::running(herder));
    REQUIRE(meter(app, "prevalidate", "started", "prevalidation") ==
            startedBefore + 1);
    Access::waitUntilFinished(herder);

    SECTION("the build takes the results when the executed value closes")
    {
        f.externalize(closing);
        // The overlay drops a closed ledger's transactions.
        f.mMempool = {nextA, staleD, plainB, futureC};
        auto const before = cacheCounts(app);
        auto const built = f.build();
        REQUIRE(built == hashesOf({nextA, plainB}));
        auto const after = cacheCounts(app);
        // nothing was validated on the main thread
        REQUIRE(after.mHit - before.mHit == 4);
        REQUIRE(after.mMiss - before.mMiss == 0);
        REQUIRE(meter(app, "prevalidate", "used", "prevalidation") ==
                usedBefore + 1);
        REQUIRE(!Access::running(herder));

        // The same set comes out without the results...
        Access::clearCache(herder);
        auto const rebuiltBefore = cacheCounts(app);
        REQUIRE(f.build() == built);
        auto const rebuilt = cacheCounts(app);
        REQUIRE(rebuilt.mHit - rebuiltBefore.mHit == 0);
        REQUIRE(rebuilt.mMiss - rebuiltBefore.mMiss == 4);
        // ... and a later build on the same ledger only validates what is
        // new to it.
        auto lateB = paymentWithSeq(app, c, cSeq + 1);
        f.mMempool.push_back(lateB);
        auto const lateBefore = cacheCounts(app);
        // C now has two candidates; the lower sequence number is kept.
        REQUIRE(f.build() == hashesOf({nextA, plainB, lateB}));
        auto const late = cacheCounts(app);
        REQUIRE(late.mHit - lateBefore.mHit == 3);
        REQUIRE(late.mMiss - lateBefore.mMiss == 1);
    }

    SECTION("the results are dropped when another value closes the ledger")
    {
        // B's payment closes the ledger instead: A and D did not move.
        auto other = makeUnvalidatedTxSet(app, {plainB});
        f.externalize(other);
        f.mMempool = {closingA, closingD, staleD, futureC, plainB};
        auto const before = cacheCounts(app);
        // With the stale results, B's consumed payment would be proposed
        // again.
        auto const built = f.build();
        REQUIRE(built.size() == 2);
        REQUIRE(built.count(closingA->getFullHash()) == 1);
        REQUIRE(built.count(plainB->getFullHash()) == 0);
        REQUIRE(built.count(futureC->getFullHash()) == 0);
        auto const after = cacheCounts(app);
        REQUIRE(after.mHit - before.mHit == 0);
        REQUIRE(meter(app, "prevalidate", "used", "prevalidation") ==
                usedBefore);
        REQUIRE(meter(app, "prevalidate", "discarded", "prevalidation") ==
                discardedBefore + 1);
        REQUIRE(!Access::running(herder));
    }

    SECTION("the results are not kept past the ledger they are for")
    {
        f.externalize(closing);
        f.mMempool = {nextA, staleD, plainB, futureC};
        REQUIRE(f.build() == hashesOf({nextA, plainB}));
        REQUIRE(Access::cachedResults(herder) == 4);
        // the next ledger closes; a build on top of it starts from scratch
        f.externalize(makeUnvalidatedTxSet(app, {nextA}));
        f.mMempool = {plainB, futureC};
        auto const before = cacheCounts(app);
        REQUIRE(f.build() == hashesOf({plainB}));
        auto const after = cacheCounts(app);
        REQUIRE(after.mHit - before.mHit == 0);
        REQUIRE(after.mMiss - before.mMiss == 2);
        REQUIRE(Access::cachedResults(herder) == 2);
    }
}

TEST_CASE("pre-validation is not started when its results could not be used",
          "[herder][txset][prevalidation]")
{
    PrevalidationFixture f;
    auto& app = *f.mApp;
    auto& herder = f.herder();
    auto& a = *f.mA;
    auto& b = *f.mB;
    auto const aSeq = a.getLastSequenceNumber();
    auto const bSeq = b.getLastSequenceNumber();
    auto good = paymentWithSeq(app, a, aSeq + 1);
    f.mMempool = {good, paymentWithSeq(app, b, bSeq + 1)};

    SECTION("the executed set was invalid")
    {
        auto bad = paymentWithSeq(app, b, bSeq + 10);
        auto state = f.speculate(makeUnvalidatedTxSet(app, {good, bad}), true);
        REQUIRE(state);
        REQUIRE(state->mTxSetSkipped);
        REQUIRE(!Access::running(herder));
    }
    SECTION("the node votes for another value by the time it has executed")
    {
        auto txSet = makeUnvalidatedTxSet(app, {good});
        auto const value = f.valueFor(txSet);
        auto otherValue = value;
        otherValue.closeTime += 1;
        Access::setVotedValue(herder, f.nextSeq(), value);
        std::shared_ptr<ExecutedLedgerState const> executed;
        f.lm().startSpeculativeApply(
            LedgerCloseData(f.nextSeq(), txSet, value),
            [&](std::shared_ptr<ExecutedLedgerState const> state) {
                executed = state;
                Access::setVotedValue(herder, f.nextSeq(), otherValue);
                Access::start(herder, state);
            });
        while (f.lm().isSpeculativelyApplying())
        {
            f.mClock.crank(true);
        }
        REQUIRE(executed);
        REQUIRE(!Access::running(herder));
    }
    SECTION("the executed value carries upgrades")
    {
        // Upgrades are applied when the ledger commits, after the state
        // the candidates would be validated on.
        LedgerUpgrade upgrade(LEDGER_UPGRADE_BASE_FEE);
        upgrade.newBaseFee() = 200;
        xdr::xvector<UpgradeType, 6> upgrades;
        upgrades.emplace_back(LedgerTestUtils::toUpgradeType(upgrade));
        auto state =
            f.speculate(makeUnvalidatedTxSet(app, {good}), true, upgrades);
        REQUIRE(state);
        REQUIRE(!state->mValue.upgrades.empty());
        REQUIRE(!Access::running(herder));
    }
    SECTION("there is nothing in the mempool")
    {
        auto txSet = makeUnvalidatedTxSet(app, {good});
        f.mMempool.clear();
        REQUIRE(f.speculate(txSet, true));
        REQUIRE(!Access::running(herder));
    }
}

// What ledger N does to an account decides whether its next transaction is
// valid; the pre-validated results must say what validating on the closed
// ledger says, for every way N can touch a candidate's accounts.
TEST_CASE("builds with and without pre-validated results agree",
          "[herder][txset][prevalidation]")
{
    PrevalidationFixture f;
    auto& app = *f.mApp;
    auto& herder = f.herder();
    auto& a = *f.mA;
    auto& b = *f.mB;
    auto& c = *f.mC;
    auto& d = *f.mD;
    auto& e = *f.mE;
    auto const root = app.getRoot()->getPublicKey();
    auto const aSeq = a.getLastSequenceNumber();
    auto const bSeq = b.getLastSequenceNumber();
    auto const dSeq = d.getLastSequenceNumber();
    auto const eSeq = e.getLastSequenceNumber();

    // Ledger N: A pays; D pays away all it can spare; E merges into the root.
    auto closingA = paymentWithSeq(app, a, aSeq + 1);
    auto const spare =
        d.getBalance() - app.getLedgerManager().getLastMinBalance(0) - 100;
    REQUIRE(spare > 0);
    auto drainD = paymentWithSeq(app, d, dSeq + 1, spare);
    auto mergeE = transactionFromOperations(app, e.getSecretKey(), eSeq + 1,
                                            {accountMerge(root)}, 100);
    auto closing = makeUnvalidatedTxSet(app, {closingA, drainD, mergeE});

    // Candidates for N+1:
    //  * A's next two payments (chained): the first is valid after N, the
    //    second waits for it,
    //  * D's next payment: its sequence number is right after N, but D can
    //    no longer pay the fee,
    //  * E's next payment: E does not exist after N,
    //  * B's payment with C paying the fee (a fee bump): valid.
    auto nextA = paymentWithSeq(app, a, aSeq + 2);
    auto laterA = paymentWithSeq(app, a, aSeq + 3);
    auto brokeD = paymentWithSeq(app, d, dSeq + 2);
    auto goneE = paymentWithSeq(app, e, eSeq + 2);
    auto bumped = feeBump(app, c, paymentWithSeq(app, b, bSeq + 1), 1000);
    REQUIRE(!txValidityDependsOnCloseTime(bumped->getEnvelope()));

    f.mMempool = {closingA, drainD, mergeE, nextA,
                  laterA,   brokeD, goneE,  bumped};
    auto state = f.speculate(closing, true);
    REQUIRE(state);
    REQUIRE(state->mAppliedTxs == 3);
    REQUIRE(Access::running(herder));
    Access::waitUntilFinished(herder);
    f.externalize(closing);
    // N applied as the test assumes
    REQUIRE(!f.lm().getLCLView()->getAccount(e.getPublicKey()));
    REQUIRE(a.getLastSequenceNumber() == aSeq + 1);
    REQUIRE(d.getLastSequenceNumber() == dSeq + 1);

    f.mMempool = {nextA, laterA, brokeD, goneE, bumped};
    auto const before = cacheCounts(app);
    auto const withResults = f.build();
    auto const after = cacheCounts(app);
    // A's later payment is set aside before validation (one transaction per
    // source account); the other four come from the pre-validation.
    REQUIRE(after.mHit - before.mHit == 4);
    REQUIRE(after.mMiss - before.mMiss == 0);
    REQUIRE(withResults == hashesOf({nextA, bumped}));

    Access::clearCache(herder);
    auto const withoutBefore = cacheCounts(app);
    auto const withoutResults = f.build();
    auto const withoutAfter = cacheCounts(app);
    REQUIRE(withoutAfter.mHit - withoutBefore.mHit == 0);
    REQUIRE(withoutAfter.mMiss - withoutBefore.mMiss == 4);
    REQUIRE(withoutResults == withResults);
}

TEST_CASE("a running pre-validation does not outlive its use",
          "[herder][txset][prevalidation]")
{
    auto f = std::make_unique<PrevalidationFixture>();
    auto& app = *f->mApp;
    auto& herder = f->herder();
    auto& a = *f->mA;
    auto const aSeq = a.getLastSequenceNumber();
    auto closing =
        makeUnvalidatedTxSet(app, {paymentWithSeq(app, a, aSeq + 1)});
    // Enough candidates to keep the background tasks busy for a moment.
    auto next = paymentWithSeq(app, a, aSeq + 2);
    for (int i = 0; i < 400; ++i)
    {
        f->mMempool.push_back(next);
    }
    REQUIRE(f->speculate(closing, true));
    REQUIRE(Access::running(herder));

    SECTION("the application shuts down")
    {
        // Destroyed with the job possibly mid-flight: no hang, no crash.
        f.reset();
    }
    SECTION("the build takes what is there and does not wait")
    {
        f->externalize(closing);
        f->mMempool = {next};
        REQUIRE(f->build() == hashesOf({next}));
        REQUIRE(!Access::running(herder));
        auto const counts = cacheCounts(app);
        // whether or not the job had got to it, the candidate was validated
        // exactly once
        REQUIRE(counts.mHit + counts.mMiss >= 1);
    }
    SECTION("another ledger closes on top")
    {
        // Another leader's value closes the ledger and this node moves on
        // without ever building on it.
        f->externalize(closing);
        f->externalize(makeUnvalidatedTxSet(app, {next}));
        REQUIRE(!Access::running(herder));
        REQUIRE(Access::cachedResults(herder) == 0);
    }
}

TEST_CASE("transactions whose validity depends on the close time",
          "[herder][txset][prevalidation]")
{
    TransactionEnvelope v1(ENVELOPE_TYPE_TX);
    auto& cond = v1.v1().tx.cond;
    REQUIRE(!txValidityDependsOnCloseTime(v1));

    SECTION("time bounds")
    {
        cond.type(PRECOND_TIME);
        // present but unbounded
        REQUIRE(!txValidityDependsOnCloseTime(v1));
        cond.timeBounds().maxTime = 1000;
        REQUIRE(txValidityDependsOnCloseTime(v1));
        cond.timeBounds().maxTime = 0;
        cond.timeBounds().minTime = 5;
        REQUIRE(txValidityDependsOnCloseTime(v1));
    }
    SECTION("v2 preconditions")
    {
        cond.type(PRECOND_V2);
        // ledger bounds and sequence conditions depend on the ledger, not
        // on its close time
        cond.v2().ledgerBounds.activate().maxLedger = 100;
        cond.v2().minSeqNum.activate() = 7;
        cond.v2().minSeqLedgerGap = 3;
        REQUIRE(!txValidityDependsOnCloseTime(v1));
        SECTION("minimum sequence age")
        {
            cond.v2().minSeqAge = 10;
            REQUIRE(txValidityDependsOnCloseTime(v1));
        }
        SECTION("time bounds")
        {
            cond.v2().timeBounds.activate().maxTime = 99;
            REQUIRE(txValidityDependsOnCloseTime(v1));
        }
    }
    SECTION("v0 envelopes")
    {
        TransactionEnvelope v0(ENVELOPE_TYPE_TX_V0);
        REQUIRE(!txValidityDependsOnCloseTime(v0));
        v0.v0().tx.timeBounds.activate().maxTime = 1;
        REQUIRE(txValidityDependsOnCloseTime(v0));
    }
    SECTION("fee bumps follow their inner transaction")
    {
        TransactionEnvelope bump(ENVELOPE_TYPE_TX_FEE_BUMP);
        bump.feeBump().tx.innerTx.type(ENVELOPE_TYPE_TX);
        REQUIRE(!txValidityDependsOnCloseTime(bump));
        auto& inner = bump.feeBump().tx.innerTx.v1().tx.cond;
        inner.type(PRECOND_TIME);
        inner.timeBounds().maxTime = 1000;
        REQUIRE(txValidityDependsOnCloseTime(bump));
    }
}

TEST_CASE("time-bounded candidates are validated by the build itself",
          "[herder][txset][prevalidation]")
{
    PrevalidationFixture f;
    auto& app = *f.mApp;
    auto& herder = f.herder();
    auto& a = *f.mA;
    auto& b = *f.mB;
    auto const aSeq = a.getLastSequenceNumber();

    auto closing =
        makeUnvalidatedTxSet(app, {paymentWithSeq(app, a, aSeq + 1)});
    auto plain = paymentWithSeq(app, a, aSeq + 2);
    // valid for an hour from the last close
    PreconditionsV2 cond;
    cond.timeBounds.activate().maxTime =
        f.lm().getLastClosedLedgerHeader().header.scpValue.closeTime + 3600;
    auto bounded = transactionWithV2Precondition(app, b, 1, 100, cond);
    REQUIRE(txValidityDependsOnCloseTime(bounded->getEnvelope()));
    f.mMempool = {plain, bounded};

    REQUIRE(f.speculate(closing, true));
    Access::waitUntilFinished(herder);
    f.externalize(closing);

    auto const before = cacheCounts(app);
    REQUIRE(f.build() == hashesOf({plain, bounded}));
    auto const after = cacheCounts(app);
    REQUIRE(after.mHit - before.mHit == 1);
    REQUIRE(after.mMiss - before.mMiss == 1);
    // and its result is not kept for a build with another close time
    REQUIRE(Access::cachedResults(herder) == 1);
}

TEST_CASE("the first-round leader builds shortly before the trigger",
          "[herder][txset][prevalidation]")
{
    using namespace std::chrono;
    auto lead = [](int closeTimeMs, int lastBuildMs) {
        return Access::prepareLeadTime(milliseconds(closeTimeMs),
                                       milliseconds(lastBuildMs));
    };
    SECTION("before the node has built anything")
    {
        // a quarter of the target close time
        REQUIRE(lead(800, 0) == milliseconds(200));
        REQUIRE(lead(1000, 0) == milliseconds(250));
        // capped: a long close time does not make transactions wait longer
        REQUIRE(lead(5000, 0) == milliseconds(250));
        REQUIRE(lead(0, 0) == milliseconds(0));
    }
    SECTION("long enough for a build like the last one and its push")
    {
        // 180 ms + a quarter + 100 ms for the set to reach the peers
        REQUIRE(lead(800, 180) == milliseconds(325));
        REQUIRE(lead(1000, 240) == milliseconds(400));
        // a quick build does not shorten it
        REQUIRE(lead(800, 20) == milliseconds(200));
        REQUIRE(lead(5000, 20) == milliseconds(250));
        // on a long close time a slow build still gets its room
        REQUIRE(lead(5000, 600) == milliseconds(850));
    }
    SECTION("never more than half of the target close time")
    {
        REQUIRE(lead(800, 400) == milliseconds(400));
        REQUIRE(lead(800, 5000) == milliseconds(400));
        REQUIRE(lead(1000, 500) == milliseconds(500));
    }
}

// A node that starts a term has not built for a while: its last build says
// little about the next one. What the last closed ledger held does.
TEST_CASE("the lead time of the first build of a term follows the size of the "
          "last ledger",
          "[herder][txset][prevalidation]")
{
    using namespace std::chrono;
    PrevalidationFixture f;
    auto& app = *f.mApp;
    auto& herder = f.herder();
    auto& a = *f.mA;
    auto lead = [&](int closeTimeMs) {
        return Access::prepareLeadTime(milliseconds(closeTimeMs),
                                       Access::expectedBuild(herder));
    };

    SECTION("the herder learns the size of every ledger that closes")
    {
        auto const aSeq = a.getLastSequenceNumber();
        f.externalize(
            makeUnvalidatedTxSet(app, {paymentWithSeq(app, a, aSeq + 1)}));
        REQUIRE(Access::lastClosedTxSetSize(herder) == 1);
        f.externalize(makeUnvalidatedTxSet(app, {}));
        REQUIRE(Access::lastClosedTxSetSize(herder) == 0);
    }
    SECTION("a node that never built under load")
    {
        // its last build was of an empty set, long ago
        Access::setBuildHistory(herder, milliseconds(1), nanoseconds(0));
        Access::setLastClosedTxSetSize(herder, 0);
        REQUIRE(Access::expectedBuild(herder) == milliseconds(1));
        REQUIRE(lead(800) == milliseconds(200));
        // the network now closes ledgers of 9,300 transactions: at the
        // default cost per transaction that is a build of 372 ms, more than
        // the 200 ms a lead time sized for the old build would leave
        Access::setLastClosedTxSetSize(herder, 9300);
        REQUIRE(Access::expectedBuild(herder) == milliseconds(372));
        REQUIRE(lead(800) == milliseconds(400));
        // smaller ledgers, smaller head start
        Access::setLastClosedTxSetSize(herder, 2000);
        REQUIRE(Access::expectedBuild(herder) == milliseconds(80));
        REQUIRE(lead(800) == milliseconds(200));
        REQUIRE(lead(2000) == milliseconds(250));
    }
    SECTION("a node that has measured its own builds")
    {
        // 180 ms for 6,400 transactions: about 28 us each
        Access::setBuildHistory(herder, milliseconds(180), nanoseconds(28'125));
        Access::setLastClosedTxSetSize(herder, 6400);
        REQUIRE(Access::expectedBuild(herder) == milliseconds(180));
        REQUIRE(lead(800) == milliseconds(325));
        // ledgers grew since its last build: sized for them, at its own rate
        Access::setLastClosedTxSetSize(herder, 9600);
        REQUIRE(Access::expectedBuild(herder) == milliseconds(270));
        REQUIRE(lead(800) == milliseconds(400));
        // ledgers shrank: the last build still counts until the next one
        Access::setLastClosedTxSetSize(herder, 100);
        REQUIRE(Access::expectedBuild(herder) == milliseconds(180));
    }
    SECTION("a build records what it cost per transaction")
    {
        // a set too small to say anything leaves the rate alone
        Access::setBuildHistory(herder, milliseconds(0), nanoseconds(777));
        f.mMempool = {paymentWithSeq(app, a, a.getLastSequenceNumber() + 1)};
        REQUIRE(f.build().size() == 1);
        Access::setLastClosedTxSetSize(herder, 1'000'000);
        REQUIRE(Access::expectedBuild(herder) == milliseconds(777));
    }
}

// A network whose ledgers all carry a transaction: the node whose value
// closed a ledger leads the next one, for up to LEADER_TERM_LEDGERS ledgers,
// and leaders build from pre-validated candidates.
TEST_CASE("leaders keep their term and pre-validate in a network",
          "[herder][leader-schedule][leader-term][prevalidation][simulation]")
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
    auto nodeIDs = simulation->getNodeIDs();
    simulation->startAllNodes();

    // Every node's mempool holds the root account's next payments: one fits
    // each ledger (one transaction per source account), and the one after it
    // only becomes valid when that ledger has applied.
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
        static_cast<HerderImpl&>(app->getHerder())
            .mGetTopTransactionsForTesting = [&, app](size_t) {
            std::vector<TransactionEnvelope> top;
            auto const next = static_cast<size_t>(rootSeq(*app) - firstSeq);
            for (size_t i = next; i < next + 4 && i < payments.size(); ++i)
            {
                top.push_back(payments[i]);
            }
            return top;
        };
    }

    // Watch node 1 close ledgers.
    auto& lm = nodes[1]->getLedgerManager();
    auto prev = lm.getLastClosedLedgerHeader();
    uint32_t const target = prev.header.ledgerSeq + 40;
    Hash const emptyResults = xdrSha256(TransactionResultSet{});
    size_t withTransactions = 0;
    size_t insideTerm = 0;
    size_t incumbentLed = 0;
    size_t incumbentClosed = 0;
    std::set<NodeID> proposers;
    simulation->crankUntil(
        [&]() {
            auto const cur = lm.getLastClosedLedgerHeader();
            if (cur.header.ledgerSeq == prev.header.ledgerSeq)
            {
                return false;
            }
            REQUIRE(cur.header.ledgerSeq == prev.header.ledgerSeq + 1);
            auto const proposer =
                getLcValueSignature(cur.header.scpValue).nodeID;
            proposers.insert(proposer);
            if (cur.header.txSetResultHash != emptyResults)
            {
                ++withTransactions;
            }
            auto const schedule =
                computeNextLeaderSchedule(prev.header, nodeIDs, true);
            REQUIRE(schedule.size() == 4);
            // (the genesis ledger has no proposer)
            bool const prevApplied =
                prev.header.txSetResultHash != emptyResults &&
                prev.header.scpValue.ext.v() != STELLAR_VALUE_BASIC;
            if (cur.header.ledgerSeq % LEADER_TERM_LEDGERS != 0 && prevApplied)
            {
                // inside a term: the node that closed the previous ledger
                // leads this one
                ++insideTerm;
                auto const incumbent =
                    getLcValueSignature(prev.header.scpValue).nodeID;
                if (schedule[0] == incumbent)
                {
                    ++incumbentLed;
                }
                if (proposer == incumbent)
                {
                    ++incumbentClosed;
                }
            }
            else if (cur.header.ledgerSeq % LEADER_TERM_LEDGERS == 0 &&
                     prevApplied)
            {
                // a new term: the seeded order, known a ledger early
                REQUIRE(schedule ==
                        computeLeaderSchedule(prev.header.previousLedgerHash,
                                              nodeIDs));
            }
            // The node itself derives the next ledger's leaders from the
            // header it just closed.
            auto& driver = static_cast<HerderImpl&>(nodes[1]->getHerder())
                               .getHerderSCPDriver();
            REQUIRE(driver.getLeaderSchedule(cur.header.ledgerSeq + 1) ==
                    computeNextLeaderSchedule(cur.header, nodeIDs, true));
            prev = cur;
            return cur.header.ledgerSeq >= target;
        },
        60 * simulation->getExpectedLedgerCloseTime(), false);
    REQUIRE(prev.header.ledgerSeq >= target);
    auto const paid = rootSeq(*nodes[1]) - firstSeq;
    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(target, 2); },
        10 * simulation->getExpectedLedgerCloseTime(), false);

    int64_t timeouts = 0;
    int64_t prevalidationsUsed = 0;
    int64_t cacheHits = 0;
    int64_t speculationHits = 0;
    for (auto const& node : nodes)
    {
        auto& metrics = node->getMetrics();
        timeouts += static_cast<int64_t>(
            metrics.NewHistogram({"scp", "timeout", "leader"}).sum() +
            metrics.NewHistogram({"scp", "timeout", "prepare"}).sum());
        prevalidationsUsed +=
            meter(*node, "prevalidate", "used", "prevalidation");
        cacheHits += meter(*node, "txset", "validation-cache-hit", "tx");
        speculationHits += applyCounter(*node, "speculation-hit");
        // no leader proposed a set that apply found invalid
        REQUIRE(applyCounter(*node, "txset-skipped") == 0);
    }
    INFO(fmt::format("ledgers with txs {}, inside a term {}, incumbent led "
                     "{}, incumbent closed {}, timeouts {}, prevalidations "
                     "used {}, cache hits {}, speculation hits {}",
                     withTransactions, insideTerm, incumbentLed,
                     incumbentClosed, timeouts, prevalidationsUsed, cacheHits,
                     speculationHits));
    // Ledgers carried the payments, one each.
    REQUIRE(withTransactions >= 30);
    REQUIRE(paid == static_cast<int64_t>(withTransactions));
    // Inside a term the incumbent always leads round 1, and closes the
    // ledger unless a round timed out (e.g. on a busy test machine).
    REQUIRE(insideTerm >= 20);
    REQUIRE(incumbentLed == insideTerm);
    REQUIRE(static_cast<int64_t>(insideTerm - incumbentClosed) <= timeouts);
    REQUIRE(incumbentClosed * 4 >= insideTerm * 3);
    // Terms end: leadership still rotates.
    REQUIRE(proposers.size() >= 2);
    // Leaders executed their own ledgers speculatively and built the next
    // set from candidates validated on that state.
    REQUIRE(speculationHits > 0);
    REQUIRE(prevalidationsUsed > 0);
    REQUIRE(cacheHits > 0);

    for (auto const& node : nodes)
    {
        static_cast<HerderImpl&>(node->getHerder())
            .mGetTopTransactionsForTesting = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Early selection: the set for ledger N+1 is selected while ledger N is in
// its ballot and completed when N has closed.
// ---------------------------------------------------------------------------

namespace
{
int64_t
txsetMeter(Application& app, std::string const& name)
{
    return meter(app, "txset", name, "txset");
}

StellarValue
stellarValueOf(ValueWrapperPtr const& value)
{
    REQUIRE(value);
    StellarValue sv;
    REQUIRE(toStellarValue(value->getValue(), sv));
    return sv;
}
}

TEST_CASE("the set selected during the previous ballot is proposed unchanged",
          "[herder][txset][prevalidation][early-txset]")
{
    PrevalidationFixture f;
    auto& app = *f.mApp;
    auto& herder = f.herder();
    auto& lm = f.lm();
    auto& a = *f.mA;
    auto& b = *f.mB;
    auto& c = *f.mC;
    auto& d = *f.mD;
    auto const aSeq = a.getLastSequenceNumber();
    auto const bSeq = b.getLastSequenceNumber();
    auto const cSeq = c.getLastSequenceNumber();
    auto const dSeq = d.getLastSequenceNumber();

    // Ledger N and the candidates for N+1 of the pre-validation test above:
    // A's next payment is only valid once N has applied, D's other payment
    // only before, B's either way, C's never.
    auto closingA = paymentWithSeq(app, a, aSeq + 1);
    auto closingD = paymentWithSeq(app, d, dSeq + 1);
    auto nextA = paymentWithSeq(app, a, aSeq + 2);
    auto staleD = paymentWithSeq(app, d, dSeq + 1, 2);
    auto plainB = paymentWithSeq(app, b, bSeq + 1);
    auto futureC = paymentWithSeq(app, c, cSeq + 5);
    auto closing = makeUnvalidatedTxSet(app, {closingA, closingD});
    f.mMempool = {closingA, closingD, nextA, staleD, plainB, futureC};

    auto const selectedBefore = txsetMeter(app, "early-selected");
    auto const finalizedBefore = txsetMeter(app, "early-finalized");
    REQUIRE(f.speculate(closing, true));
    REQUIRE(Access::selectsEarly(herder));
    Access::waitUntilFinished(herder);
    Access::waitUntilSelected(herder);
    REQUIRE(Access::hasEarlySet(herder));
    REQUIRE(txsetMeter(app, "early-selected") == selectedBefore + 1);
    // N's own transactions, still in the mempool, are not candidates.
    REQUIRE(Access::pulledByJob(herder) == 4);
    // Nothing of this touched the last closed ledger or prepared a set.
    REQUIRE(!Access::prepared(herder));
    auto const pulls = f.mPulls;

    f.externalize(closing);
    auto const lcl = lm.getLastClosedLedgerHeader();
    // What reaches the mempool after the job's pull waits for a later set.
    auto lateC = paymentWithSeq(app, c, cSeq + 1);
    f.mMempool = {nextA, staleD, plainB, futureC, lateC};
    auto const closeTime = f.nextCloseTime();
    REQUIRE(Access::prepareEarly(herder, f.nextSeq(), closeTime));
    REQUIRE(txsetMeter(app, "early-finalized") == finalizedBefore + 1);

    auto prepared = Access::prepared(herder);
    REQUIRE(prepared);
    // It names the ledger that closed meanwhile as its parent...
    REQUIRE(prepared->previousLedgerHash() == lcl.hash);
    // ... holds what is valid after that ledger, and nothing of it ...
    auto applicable = prepared->prepareForApply(app, lcl.header);
    REQUIRE(applicable);
    REQUIRE(txHashes(applicable) == hashesOf({nextA, plainB}));
    // ... and is a valid set on top of it, transactions included.
    REQUIRE(applicable->checkValid(app, 1, 1));
    // D's stale payment leaves the mempool with the proposal; C's waits
    // behind its account's sequence number.
    REQUIRE(Access::preparedInvalidTxs(herder) ==
            std::vector<Hash>{staleD->getFullHash()});
    // The mempool was not pulled again, and the job's results are there for
    // a later build on this ledger.
    REQUIRE(f.mPulls == pulls);
    REQUIRE(!Access::running(herder));
    REQUIRE(Access::cachedResults(herder) == 4);

    // The trigger proposes that very set, for the close time it was
    // completed for, without building.
    auto const proposed = stellarValueOf(Access::propose(herder, f.nextSeq()));
    REQUIRE(proposed.txSetHash == prepared->getContentsHash());
    REQUIRE(getConsensusTime(proposed) == closeTime);
    REQUIRE(f.mPulls == pulls);

    // And the ledger applies it.
    auto const skipped = applyCounter(app, "txset-skipped");
    f.externalize(prepared);
    REQUIRE(applyCounter(app, "txset-skipped") == skipped);
    REQUIRE(a.getLastSequenceNumber() == aSeq + 2);
    REQUIRE(b.getLastSequenceNumber() == bSeq + 1);
    REQUIRE(c.getLastSequenceNumber() == cSeq);
}

// What ledger N does to a candidate's accounts decides whether the candidate
// can go into N+1. A set selected on N's executed state must hold what a build
// on the closed ledger N holds.
TEST_CASE("a set selected early holds what a build on the closed ledger does",
          "[herder][txset][prevalidation][early-txset]")
{
    PrevalidationFixture f;
    auto& app = *f.mApp;
    auto& herder = f.herder();
    auto& a = *f.mA;
    auto& b = *f.mB;
    auto& c = *f.mC;
    auto& d = *f.mD;
    auto& e = *f.mE;
    auto const root = app.getRoot()->getPublicKey();
    auto const aSeq = a.getLastSequenceNumber();
    auto const bSeq = b.getLastSequenceNumber();
    auto const dSeq = d.getLastSequenceNumber();
    auto const eSeq = e.getLastSequenceNumber();

    // Ledger N: A pays; D pays away all it can spare; E merges into the root.
    auto closingA = paymentWithSeq(app, a, aSeq + 1);
    auto const spare =
        d.getBalance() - app.getLedgerManager().getLastMinBalance(0) - 100;
    REQUIRE(spare > 0);
    auto drainD = paymentWithSeq(app, d, dSeq + 1, spare);
    auto mergeE = transactionFromOperations(app, e.getSecretKey(), eSeq + 1,
                                            {accountMerge(root)}, 100);
    auto closing = makeUnvalidatedTxSet(app, {closingA, drainD, mergeE});
    // Candidates for N+1: A's next two payments (chained), D's next payment
    // (D cannot pay the fee after N), E's (E is gone after N), and B's payment
    // with C paying the fee.
    auto nextA = paymentWithSeq(app, a, aSeq + 2);
    auto laterA = paymentWithSeq(app, a, aSeq + 3);
    auto brokeD = paymentWithSeq(app, d, dSeq + 2);
    auto goneE = paymentWithSeq(app, e, eSeq + 2);
    auto bumped = feeBump(app, c, paymentWithSeq(app, b, bSeq + 1), 1000);
    f.mMempool = {closingA, drainD, mergeE, nextA,
                  laterA,   brokeD, goneE,  bumped};

    f.closeWithEarlySelection(closing);
    auto const lcl = f.lm().getLastClosedLedgerHeader();
    REQUIRE(Access::prepareEarly(herder, f.nextSeq(), f.nextCloseTime()));
    auto prepared = Access::prepared(herder);
    REQUIRE(prepared);
    auto applicable = prepared->prepareForApply(app, lcl.header);
    REQUIRE(applicable);
    auto const early = txHashes(applicable);
    REQUIRE(early == hashesOf({nextA, bumped}));
    REQUIRE(applicable->checkValid(app, 1, 1));

    // The build on the closed ledger, validating everything itself.
    Access::clearCache(herder);
    f.mMempool = {nextA, laterA, brokeD, goneE, bumped};
    REQUIRE(f.build() == early);
}

TEST_CASE("a set selected early is not used when it does not hold",
          "[herder][txset][prevalidation][early-txset]")
{
    PrevalidationFixture f;
    auto& app = *f.mApp;
    auto& herder = f.herder();
    auto& a = *f.mA;
    auto& b = *f.mB;
    auto const aSeq = a.getLastSequenceNumber();
    auto const bSeq = b.getLastSequenceNumber();
    auto closingA = paymentWithSeq(app, a, aSeq + 1);
    auto nextA = paymentWithSeq(app, a, aSeq + 2);
    auto plainB = paymentWithSeq(app, b, bSeq + 1);
    auto closing = makeUnvalidatedTxSet(app, {closingA});
    f.mMempool = {closingA, nextA, plainB};
    auto const discardedBefore = txsetMeter(app, "early-discarded");
    auto const finalizedBefore = txsetMeter(app, "early-finalized");
    auto const seq = f.nextSeq();
    auto const closeTime = f.nextCloseTime();

    auto selectFor = [&](TxSetXDRFrameConstPtr const& txSet) {
        REQUIRE(f.speculate(txSet, true));
        Access::waitUntilFinished(herder);
        Access::waitUntilSelected(herder);
        REQUIRE(Access::hasEarlySet(herder));
    };

    SECTION("another set closes the ledger")
    {
        selectFor(closing);
        // B's payment closes the ledger instead: A did not move.
        f.externalize(makeUnvalidatedTxSet(app, {plainB}));
        REQUIRE(!Access::prepareEarly(herder, f.nextSeq(), f.nextCloseTime()));
        REQUIRE(!Access::prepared(herder));
        // The selected set held A's second payment and B's, neither of which
        // is valid on the ledger that closed.
        f.mMempool = {closingA, nextA, plainB};
        REQUIRE(f.build() == hashesOf({closingA}));
    }
    SECTION("the same set closes the ledger with another close time")
    {
        // Another value: not the ledger that was executed.
        selectFor(closing);
        auto later = ConsensusTime::fromApplyTime(ApplyTime::fromTimePoint(
            f.lm().getLastClosedLedgerHeader().header.scpValue.closeTime + 2));
        app.getHerder().externalizeValue(closing, seq, later,
                                         emptyUpgradeSteps);
        REQUIRE(!Access::prepareEarly(herder, f.nextSeq(), f.nextCloseTime()));
        REQUIRE(!Access::prepared(herder));
    }
    SECTION("the executed set was invalid")
    {
        // The ledger applies nothing, and its leader forfeits its term.
        auto bad = paymentWithSeq(app, b, bSeq + 10);
        auto invalid = makeUnvalidatedTxSet(app, {closingA, bad});
        auto state = f.speculate(invalid, true);
        REQUIRE(state);
        REQUIRE(state->mTxSetSkipped);
        REQUIRE(!Access::running(herder));
        f.externalize(invalid);
        REQUIRE(!Access::prepareEarly(herder, f.nextSeq(), f.nextCloseTime()));
        REQUIRE(!Access::prepared(herder));
    }
    SECTION("the executed value carries upgrades")
    {
        LedgerUpgrade upgrade(LEDGER_UPGRADE_BASE_FEE);
        upgrade.newBaseFee() = 200;
        xdr::xvector<UpgradeType, 6> upgrades;
        upgrades.emplace_back(LedgerTestUtils::toUpgradeType(upgrade));
        REQUIRE(f.speculate(closing, true, upgrades));
        REQUIRE(!Access::running(herder));
        app.getHerder().externalizeValue(closing, seq, closeTime, upgrades);
        REQUIRE(f.lm().getLastClosedLedgerHeader().header.baseFee == 200);
        REQUIRE(!Access::prepareEarly(herder, f.nextSeq(), f.nextCloseTime()));
        REQUIRE(!Access::prepared(herder));
    }
    SECTION("the limits of the closed ledger are not the ones it assumed")
    {
        selectFor(closing);
        Access::withEarlySet(herder, [](auto& early) {
            early.limits.mLclHeader.maxTxSetSize += 1;
        });
        f.externalize(closing);
        REQUIRE(!Access::prepareEarly(herder, f.nextSeq(), f.nextCloseTime()));
        REQUIRE(!Access::prepared(herder));
        REQUIRE(txsetMeter(app, "early-discarded") == discardedBefore + 1);
        // The job's validation results are still good for the build.
        f.mMempool = {nextA, plainB};
        auto const before = cacheCounts(app);
        REQUIRE(f.build() == hashesOf({nextA, plainB}));
        auto const after = cacheCounts(app);
        REQUIRE(after.mHit - before.mHit == 2);
        REQUIRE(after.mMiss - before.mMiss == 0);
    }
    SECTION("it is for the ledger after the executed one only")
    {
        selectFor(closing);
        f.externalize(closing);
        REQUIRE(
            !Access::prepareEarly(herder, f.nextSeq() + 1, f.nextCloseTime()));
        // Another ledger closes on top before the node gets to use it.
        f.externalize(makeUnvalidatedTxSet(app, {plainB}));
        REQUIRE(!Access::prepareEarly(herder, f.nextSeq(), f.nextCloseTime()));
        REQUIRE(!Access::prepared(herder));
    }
    SECTION("it can only be taken once")
    {
        selectFor(closing);
        f.externalize(closing);
        REQUIRE(Access::prepareEarly(herder, f.nextSeq(), f.nextCloseTime()));
        REQUIRE(!Access::prepareEarly(herder, f.nextSeq(), f.nextCloseTime()));
        REQUIRE(txsetMeter(app, "early-finalized") == finalizedBefore + 1);
    }
    SECTION("nothing is selected by a job that was stopped")
    {
        // Enough candidates to keep the background tasks busy for a moment.
        f.mMempool = {closingA};
        for (int i = 0; i < 400; ++i)
        {
            f.mMempool.push_back(nextA);
        }
        REQUIRE(f.speculate(closing, true));
        f.externalize(closing);
        // Whether or not the selection got done in time, the node either
        // has a valid prepared set or none.
        bool const used =
            Access::prepareEarly(herder, f.nextSeq(), f.nextCloseTime());
        REQUIRE(used == (Access::prepared(herder) != nullptr));
        f.mMempool = {nextA};
        REQUIRE(f.build() == hashesOf({nextA}));
        REQUIRE(!Access::running(herder));
    }
}

TEST_CASE("time-dependent transactions of a set selected early follow its "
          "close time",
          "[herder][txset][prevalidation][early-txset]")
{
    PrevalidationFixture f;
    auto& app = *f.mApp;
    auto& herder = f.herder();
    auto& a = *f.mA;
    auto& b = *f.mB;
    auto& c = *f.mC;
    auto const aSeq = a.getLastSequenceNumber();
    auto closing =
        makeUnvalidatedTxSet(app, {paymentWithSeq(app, a, aSeq + 1)});
    auto plain = paymentWithSeq(app, a, aSeq + 2);

    // The ledger being closed gets the last close time plus one second. The
    // set for the ledger after it is validated for the close time its
    // trigger is expected to have: some seconds later.
    auto const executedCloseTime =
        f.lm().getLastClosedLedgerHeader().header.scpValue.closeTime + 1;
    auto closeTimeAt = [&](uint64_t offset) {
        return ConsensusTime::fromApplyTime(
            ApplyTime::fromTimePoint(executedCloseTime + offset));
    };
    // B's payment is valid for an hour from now; C's only from an hour on.
    PreconditionsV2 untilLater;
    untilLater.timeBounds.activate().maxTime = executedCloseTime + 3600;
    auto bounded = transactionWithV2Precondition(app, b, 1, 100, untilLater);
    PreconditionsV2 notYet;
    notYet.timeBounds.activate().minTime = executedCloseTime + 3600;
    auto tooEarly = transactionWithV2Precondition(app, c, 1, 100, notYet);
    REQUIRE(txValidityDependsOnCloseTime(bounded->getEnvelope()));
    REQUIRE(txValidityDependsOnCloseTime(tooEarly->getEnvelope()));
    f.mMempool = {plain, bounded, tooEarly};

    REQUIRE(f.speculate(closing, true));
    Access::waitUntilFinished(herder);
    Access::waitUntilSelected(herder);
    REQUIRE(Access::hasEarlySet(herder));
    auto const assumed = Access::assumedCloseTimeOffset(herder);
    REQUIRE(assumed >= 1);
    REQUIRE(assumed < 3600);
    f.externalize(closing);
    auto const lcl = f.lm().getLastClosedLedgerHeader();
    auto const discardedBefore = txsetMeter(app, "early-discarded");

    auto preparedTxs = [&]() {
        auto prepared = Access::prepared(herder);
        REQUIRE(prepared);
        auto applicable = prepared->prepareForApply(app, lcl.header);
        REQUIRE(applicable);
        return txHashes(applicable);
    };

    SECTION("the close time it was validated for")
    {
        REQUIRE(
            Access::prepareEarly(herder, f.nextSeq(), closeTimeAt(assumed)));
        REQUIRE(preparedTxs() == hashesOf({plain, bounded}));
        // C's payment was not valid for that close time. It is not removed
        // from the mempool for it: the set could have got another one.
        REQUIRE(Access::preparedInvalidTxs(herder).empty());
    }
    SECTION("another close time at which they are still valid")
    {
        REQUIRE(Access::prepareEarly(herder, f.nextSeq(),
                                     closeTimeAt(assumed + 7)));
        REQUIRE(preparedTxs() == hashesOf({plain, bounded}));
        auto applicable =
            Access::prepared(herder)->prepareForApply(app, lcl.header);
        REQUIRE(applicable->checkValid(app, assumed + 7, assumed + 7));
    }
    SECTION("a close time at which one of them is not valid any more")
    {
        // The set is not used as it is ...
        REQUIRE(!Access::prepareEarly(herder, f.nextSeq(), closeTimeAt(3601)));
        REQUIRE(!Access::prepared(herder));
        REQUIRE(txsetMeter(app, "early-discarded") == discardedBefore + 1);
        // ... and the build for that close time leaves B's payment out and
        // takes C's.
        auto built = Access::build(herder, f.nextSeq(), closeTimeAt(3601));
        REQUIRE(built.second);
        REQUIRE(txHashes(built.second) == hashesOf({plain, tooEarly}));
    }
    // Results for a particular close time never reach the cache that later
    // builds on this ledger share.
    REQUIRE(Access::cachedResults(herder) <= 1);
}

// ---------------------------------------------------------------------------
// The leader executes its own set as soon as it is prepared.
// ---------------------------------------------------------------------------

TEST_CASE("the leader executes its prepared set before the trigger",
          "[herder][txset][speculative-apply][early-txset]")
{
    PrevalidationFixture f(/*herderSpeculates=*/true);
    auto& app = *f.mApp;
    auto& herder = f.herder();
    auto& lm = f.lm();
    auto& a = *f.mA;
    auto& b = *f.mB;
    auto const aSeq = a.getLastSequenceNumber();
    auto const bSeq = b.getLastSequenceNumber();
    auto closingA = paymentWithSeq(app, a, aSeq + 1);
    auto nextA = paymentWithSeq(app, a, aSeq + 2);
    auto plainB = paymentWithSeq(app, b, bSeq + 1);
    auto closing = makeUnvalidatedTxSet(app, {closingA});
    f.mMempool = {closingA, nextA, plainB};
    f.closeWithEarlySelection(closing);
    f.mMempool = {nextA, plainB};

    auto const seq = f.nextSeq();
    auto const closeTime = f.nextCloseTime();
    auto const started = applyCounter(app, "speculation-started");
    auto const hits = applyCounter(app, "speculation-hit");
    auto const misses = applyCounter(app, "speculation-miss");

    auto afterPreparing = [&]() {
        auto prepared = Access::prepared(herder);
        REQUIRE(prepared);
        // The ledger is with the apply thread already, although the node
        // has not proposed anything yet.
        REQUIRE(applyCounter(app, "speculation-started") == started + 1);
        while (lm.isSpeculativelyApplying())
        {
            f.mClock.crank(true);
        }
        REQUIRE(lm.getLastClosedLedgerNum() == seq - 1);
        // It is the very value the trigger proposes ...
        auto const proposal = Access::propose(herder, seq);
        REQUIRE(proposal);
        REQUIRE(Access::speculativeValue(herder));
        REQUIRE(Access::speculativeValue(herder)->first == seq);
        REQUIRE(Access::speculativeValue(herder)->second ==
                proposal->getValue());
        REQUIRE(stellarValueOf(proposal).txSetHash ==
                prepared->getContentsHash());
        // ... so the vote the node then emits does not start it again.
        SCPEnvelope vote;
        vote.statement.slotIndex = seq;
        vote.statement.pledges.type(SCP_ST_PREPARE);
        vote.statement.pledges.prepare().ballot.counter = 1;
        vote.statement.pledges.prepare().ballot.value = proposal->getValue();
        Access::emitted(herder, vote);
        REQUIRE(applyCounter(app, "speculation-started") == started + 1);

        SECTION("its value externalizes: the execution is committed")
        {
            f.externalize(prepared);
            REQUIRE(applyCounter(app, "speculation-hit") == hits + 1);
            REQUIRE(applyCounter(app, "speculation-miss") == misses);
            REQUIRE(a.getLastSequenceNumber() == aSeq + 2);
            REQUIRE(b.getLastSequenceNumber() == bSeq + 1);
        }
        SECTION("another value externalizes: the execution is dropped")
        {
            f.externalize(makeUnvalidatedTxSet(app, {plainB}));
            REQUIRE(applyCounter(app, "speculation-hit") == hits);
            REQUIRE(applyCounter(app, "speculation-miss") == misses + 1);
            REQUIRE(a.getLastSequenceNumber() == aSeq + 1);
            REQUIRE(b.getLastSequenceNumber() == bSeq + 1);
        }
        SECTION("the node votes for another value: that one is executed")
        {
            auto other = makeUnvalidatedTxSet(app, {plainB});
            herder.getPendingEnvelopes().putTxSet(other->getContentsHash(), seq,
                                                  other);
            auto const otherValue = f.valueFor(other);
            vote.statement.pledges.prepare().ballot.value =
                xdr::xdr_to_opaque(otherValue);
            Access::emitted(herder, vote);
            REQUIRE(applyCounter(app, "speculation-started") == started + 2);
            while (lm.isSpeculativelyApplying())
            {
                f.mClock.crank(true);
            }
            f.externalize(other);
            REQUIRE(applyCounter(app, "speculation-hit") == hits + 1);
            REQUIRE(a.getLastSequenceNumber() == aSeq + 1);
            REQUIRE(b.getLastSequenceNumber() == bSeq + 1);
        }
    };

    SECTION("a set selected early")
    {
        REQUIRE(Access::prepareEarly(herder, seq, closeTime));
        afterPreparing();
    }
    SECTION("a set built before the trigger")
    {
        Access::prepareByBuilding(herder, seq, closeTime);
        afterPreparing();
    }
}

TEST_CASE("a prepared set is not executed early when it cannot be",
          "[herder][txset][speculative-apply][early-txset]")
{
    SECTION("speculative apply is off")
    {
        PrevalidationFixture f(/*herderSpeculates=*/false);
        auto& app = *f.mApp;
        auto& a = *f.mA;
        f.mMempool = {paymentWithSeq(app, a, a.getLastSequenceNumber() + 1)};
        auto const started = applyCounter(app, "speculation-started");
        Access::prepareByBuilding(f.herder(), f.nextSeq(), f.nextCloseTime());
        REQUIRE(Access::prepared(f.herder()));
        REQUIRE(applyCounter(app, "speculation-started") == started);
        REQUIRE(!Access::speculativeValue(f.herder()));
    }
    SECTION("the node is about to propose another set")
    {
        PrevalidationFixture f(/*herderSpeculates=*/true);
        auto& app = *f.mApp;
        auto& a = *f.mA;
        f.mMempool = {paymentWithSeq(app, a, a.getLastSequenceNumber() + 1)};
        f.herder().mProposedTxSetOverride =
            [&](LedgerHeaderHistoryEntry const& lcl) {
                return TxSetXDRFrame::makeEmpty(lcl);
            };
        auto const started = applyCounter(app, "speculation-started");
        Access::prepareByBuilding(f.herder(), f.nextSeq(), f.nextCloseTime());
        REQUIRE(Access::prepared(f.herder()));
        REQUIRE(applyCounter(app, "speculation-started") == started);
        f.herder().mProposedTxSetOverride = nullptr;
    }
}

// ---------------------------------------------------------------------------
// The builder's self-check runs beside the use of the set.
// ---------------------------------------------------------------------------

TEST_CASE("the builder checks the set it built", "[herder][txset][early-txset]")
{
    PrevalidationFixture f;
    auto& app = *f.mApp;
    auto& a = *f.mA;
    auto& b = *f.mB;
    auto txA = paymentWithSeq(app, a, a.getLastSequenceNumber() + 1);
    auto txB = paymentWithSeq(app, b, b.getLastSequenceNumber() + 1);
    auto const lcl = f.lm().getLastClosedLedgerHeader();
    PerPhaseTransactionList phases{{txA, txB}, {}};
    PerPhaseTransactionList invalid(2);

    // Built without the check: the caller gets it to run.
    std::shared_ptr<TxSetSelfCheck const> check;
    bool handedOut = false;
    auto built = makeTxSetFromTransactions(
        phases, app, ApplyTimeOffset::fromSeconds(1), invalid, false, {}, false,
        [&](TxSetXDRFrameConstPtr const&) { handedOut = true; }, {}, nullptr,
        &check);
    REQUIRE(handedOut);
    REQUIRE(built.first);
    REQUIRE(built.second);
    REQUIRE(check);
    REQUIRE(check->getTxSet() == built.first);
    REQUIRE(built.second->getContentsHash() == built.first->getContentsHash());
    REQUIRE(txHashes(built.second) == hashesOf({txA, txB}));

    SECTION("passes on the set that was built")
    {
        REQUIRE(!check->run(app));
        // the same set as a build that checks before it returns
        auto checked = makeTxSetFromTransactions(
            phases, app, ApplyTimeOffset::fromSeconds(1), invalid);
        REQUIRE(checked.first->getContentsHash() ==
                built.first->getContentsHash());
    }
    SECTION("runs on another thread")
    {
        std::optional<std::string> failure{"not run"};
        std::thread t([&]() { failure = check->run(app); });
        t.join();
        REQUIRE(!failure);
    }
    SECTION("a set that lost a transaction")
    {
        auto broken =
            check->withTxSetForTesting(makeUnvalidatedTxSet(app, {txA}));
        auto const failure = broken->run(app);
        REQUIRE(failure);
        REQUIRE(failure->find("shape is mismatched") != std::string::npos);
    }
    SECTION("a set for another ledger")
    {
        // The same transactions on top of the ledger before. (Checked off
        // the main thread, as the node does: on the main thread reading
        // such a set back is an assertion failure.)
        auto xdr = built.first->getGeneralizedXDR();
        xdr.v1TxSet().previousLedgerHash = lcl.header.previousLedgerHash;
        auto other = check->withTxSetForTesting(
            TxSetXDRFrame::makeFromWire(std::move(xdr)));
        std::optional<std::string> failure;
        std::thread t([&]() { failure = other->run(app); });
        t.join();
        REQUIRE(failure);
        REQUIRE(failure->find("PREVIOUS_LEDGER_HASH_MISMATCH") !=
                std::string::npos);
    }
    SECTION("a set with a base fee below the ledger's")
    {
        auto cheap = testtxset::makeNonValidatedGeneralizedTxSet(
            {{std::make_pair(std::make_optional<int64_t>(1),
                             std::vector<TransactionFrameBasePtr>{txA, txB})},
             {}},
            app, lcl.hash);
        REQUIRE(cheap.first);
        REQUIRE(check->withTxSetForTesting(cheap.first)->run(app));
    }
}

TEST_CASE("a set that fails its self-check after it was handed out",
          "[herder][txset][early-txset]")
{
    PrevalidationFixture f;
    auto& app = *f.mApp;
    auto& herder = f.herder();
    auto& a = *f.mA;
    auto& b = *f.mB;
    auto& c = *f.mC;
    auto const aSeq = a.getLastSequenceNumber();
    auto const cSeq = c.getLastSequenceNumber();
    auto closingA = paymentWithSeq(app, a, aSeq + 1);
    auto nextA = paymentWithSeq(app, a, aSeq + 2);
    auto plainB = paymentWithSeq(app, b, b.getLastSequenceNumber() + 1);
    auto closing = makeUnvalidatedTxSet(app, {closingA});
    f.mMempool = {closingA, nextA, plainB};
    f.closeWithEarlySelection(closing);
    f.mMempool = {nextA, plainB};
    auto const seq = f.nextSeq();
    auto const closeTime = f.nextCloseTime();
    auto const failedBefore = txsetMeter(app, "self-check-failed");
    auto const passedBefore = txsetMeter(app, "self-check-passed");

    // What the node "builds": a set that is not what it selected. The one
    // transaction it holds is not valid either (a sequence number too far
    // ahead), so ledger apply refuses it.
    auto broken = makeUnvalidatedTxSet(app, {paymentWithSeq(app, c, cSeq + 5)});
    auto buildBroken = [&]() {
        herder.mBuiltTxSetOverrideForTesting =
            [&](TxSetXDRFrameConstPtr const&) { return broken; };
    };
    auto failed = [&]() {
        return txsetMeter(app, "self-check-failed") == failedBefore + 1;
    };

    SECTION("a set that passes stays prepared")
    {
        REQUIRE(Access::prepareEarly(herder, seq, closeTime));
        auto prepared = Access::prepared(herder);
        f.crankUntil([&]() {
            return txsetMeter(app, "self-check-passed") == passedBefore + 1;
        });
        REQUIRE(Access::prepared(herder) == prepared);
        REQUIRE(!Access::selfCheckHasFailed(herder));
        REQUIRE(txsetMeter(app, "self-check-failed") == failedBefore);
    }
    SECTION("selected early, caught before the proposal")
    {
        buildBroken();
        REQUIRE(Access::prepareEarly(herder, seq, closeTime));
        // The set is out (prepared, pushed) before the check has run.
        REQUIRE(Access::prepared(herder) == broken);
        herder.mBuiltTxSetOverrideForTesting = nullptr;
        f.crankUntil(failed);
        // Dropped: the trigger builds another one, checked before use.
        REQUIRE(!Access::prepared(herder));
        REQUIRE(Access::selfCheckHasFailed(herder));
        auto const pulls = f.mPulls;
        auto const proposed = stellarValueOf(Access::propose(herder, seq));
        REQUIRE(f.mPulls == pulls + 1);
        REQUIRE(proposed.txSetHash != broken->getContentsHash());
        auto txSet = herder.getTxSet(proposed.txSetHash);
        auto const* frame = std::get_if<TxSetXDRFrameConstPtr>(&txSet);
        REQUIRE(frame);
        REQUIRE(*frame);
        auto applicable = (*frame)->prepareForApply(
            app, f.lm().getLastClosedLedgerHeader().header);
        REQUIRE(applicable);
        REQUIRE(txHashes(applicable) == hashesOf({nextA, plainB}));
        // That build ran its check before returning: none in the
        // background.
        f.crankFor(std::chrono::milliseconds(50));
        REQUIRE(txsetMeter(app, "self-check-passed") == passedBefore);
    }
    SECTION("built before the trigger, caught before the proposal")
    {
        buildBroken();
        Access::prepareByBuilding(herder, seq, closeTime);
        REQUIRE(Access::prepared(herder) == broken);
        herder.mBuiltTxSetOverrideForTesting = nullptr;
        f.crankUntil(failed);
        REQUIRE(!Access::prepared(herder));
        REQUIRE(Access::selfCheckHasFailed(herder));
    }
    SECTION("caught after the proposal")
    {
        buildBroken();
        REQUIRE(Access::prepareEarly(herder, seq, closeTime));
        // The trigger fires before the check is back: the set is proposed.
        auto const proposed = stellarValueOf(Access::propose(herder, seq));
        REQUIRE(proposed.txSetHash == broken->getContentsHash());
        herder.mBuiltTxSetOverrideForTesting = nullptr;
        f.crankUntil(failed);
        REQUIRE(Access::selfCheckHasFailed(herder));
        // It cannot be recalled. Every node validates it when it applies
        // the ledger, this one included: the ledger closes without it.
        auto const skipped = applyCounter(app, "txset-skipped");
        f.externalize(broken);
        REQUIRE(applyCounter(app, "txset-skipped") == skipped + 1);
        REQUIRE(c.getLastSequenceNumber() == cSeq);
        // From now on a set selected early is checked before it is used:
        // a broken one is dropped there.
        auto nextNextA = paymentWithSeq(app, a, aSeq + 3);
        f.mMempool = {nextA, nextNextA, plainB};
        f.closeWithEarlySelection(makeUnvalidatedTxSet(app, {nextA}));
        broken = makeUnvalidatedTxSet(app, {paymentWithSeq(app, c, cSeq + 5)});
        buildBroken();
        auto const discarded = txsetMeter(app, "early-discarded");
        REQUIRE(!Access::prepareEarly(herder, f.nextSeq(), f.nextCloseTime()));
        REQUIRE(!Access::prepared(herder));
        REQUIRE(txsetMeter(app, "early-discarded") == discarded + 1);
        REQUIRE(txsetMeter(app, "self-check-failed") == failedBefore + 2);
        herder.mBuiltTxSetOverrideForTesting = nullptr;
        // The build that takes its place checks its own result.
        f.mMempool = {nextNextA, plainB};
        REQUIRE(f.build() == hashesOf({nextNextA, plainB}));
    }
}

// A network in which every build has to start when the previous ledger
// closes (the lead time is as long as the target), so that leaders use the
// sets they selected during the previous ballot whenever they have one, and
// in which every ledger carries a payment that is only valid once the
// previous one has applied.
TEST_CASE("leaders propose sets selected during the previous ballot in a "
          "network",
          "[herder][leader-schedule][leader-term][prevalidation][early-txset]"
          "[simulation]")
{
    Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
    auto simulation = Topologies::core(4, 0.75, networkID, [](int i) {
        auto cfg = getTestConfig(i, Config::TESTDB_BUCKET_DB_PERSISTENT);
        cfg.LEDGER_PROTOCOL_VERSION = GATE_PROTOCOL;
        cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = GATE_PROTOCOL;
        cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
        // Leaves the background selection the time a real ballot does: the
        // commit of every ledger takes a moment of real time.
        cfg.ARTIFICIALLY_DELAY_LEDGER_CLOSE_FOR_TESTING =
            std::chrono::milliseconds(30);
        return cfg;
    });
    auto nodes = simulation->getNodes();
    auto nodeIDs = simulation->getNodeIDs();
    simulation->startAllNodes();

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
        herder.mPrepareLeadTimeForTesting = std::chrono::seconds(3600);
        herder.mEarlyTxSetMinBuildSavingForTesting =
            std::chrono::milliseconds(0);
        // The mempool as the overlay keeps it: a closed ledger's payments
        // are gone, the next few are there. While a ledger is in its
        // ballot its own payment is still the first one.
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
    auto prev = lm.getLastClosedLedgerHeader();
    uint32_t const target = prev.header.ledgerSeq + 40;
    Hash const emptyResults = xdrSha256(TransactionResultSet{});
    size_t withTransactions = 0;
    size_t termStartsWithTransactions = 0;
    simulation->crankUntil(
        [&]() {
            auto const cur = lm.getLastClosedLedgerHeader();
            if (cur.header.ledgerSeq == prev.header.ledgerSeq)
            {
                return false;
            }
            REQUIRE(cur.header.ledgerSeq == prev.header.ledgerSeq + 1);
            if (cur.header.txSetResultHash != emptyResults)
            {
                ++withTransactions;
                if (cur.header.ledgerSeq % LEADER_TERM_LEDGERS == 0)
                {
                    ++termStartsWithTransactions;
                }
            }
            prev = cur;
            return cur.header.ledgerSeq >= target;
        },
        60 * simulation->getExpectedLedgerCloseTime(), false);
    REQUIRE(prev.header.ledgerSeq >= target);
    auto const paid = rootSeq(*nodes[1]) - firstSeq;
    simulation->crankUntil(
        [&]() { return simulation->haveAllExternalized(target, 2); },
        10 * simulation->getExpectedLedgerCloseTime(), false);

    int64_t selected = 0;
    int64_t finalized = 0;
    int64_t notReady = 0;
    int64_t discarded = 0;
    int64_t checksPassed = 0;
    int64_t speculationHits = 0;
    int64_t speculationMisses = 0;
    int64_t timeouts = 0;
    for (auto const& node : nodes)
    {
        selected += txsetMeter(*node, "early-selected");
        finalized += txsetMeter(*node, "early-finalized");
        notReady += txsetMeter(*node, "early-not-ready");
        discarded += txsetMeter(*node, "early-discarded");
        checksPassed += txsetMeter(*node, "self-check-passed");
        speculationHits += applyCounter(*node, "speculation-hit");
        speculationMisses += applyCounter(*node, "speculation-miss");
        timeouts += static_cast<int64_t>(
            node->getMetrics()
                .NewHistogram({"scp", "timeout", "leader"})
                .sum() +
            node->getMetrics()
                .NewHistogram({"scp", "timeout", "prepare"})
                .sum());
        // No leader proposed a set that apply found invalid, and no set
        // failed its builder's check.
        REQUIRE(applyCounter(*node, "txset-skipped") == 0);
        REQUIRE(txsetMeter(*node, "self-check-failed") == 0);
    }
    INFO(fmt::format("ledgers with txs {} (term starts {}), sets selected "
                     "early {}, used {}, not ready {}, discarded {}, checks "
                     "passed {}, speculation hits {} misses {}, timeouts {}",
                     withTransactions, termStartsWithTransactions, selected,
                     finalized, notReady, discarded, checksPassed,
                     speculationHits, speculationMisses, timeouts));
    // Every ledger that carried transactions carried exactly the one
    // payment that was valid on top of the ledger before it: a set selected
    // on the executed state of the previous ledger is a set for the closed
    // one. That includes the first ledger of each term, whose leader
    // executed the previous ledger as a follower.
    REQUIRE(withTransactions >= 30);
    REQUIRE(paid == static_cast<int64_t>(withTransactions));
    REQUIRE(termStartsWithTransactions >= 4);
    // Leaders did use sets they had selected early, and none had to be
    // thrown away: nothing changes limits or close-time validity here.
    REQUIRE(finalized >= 10);
    REQUIRE(finalized <= selected);
    REQUIRE(discarded == 0);
    REQUIRE(checksPassed > 0);
    // Leaders executed their own sets ahead of their proposal, and those
    // executions were committed.
    REQUIRE(speculationHits > 0);

    for (auto const& node : nodes)
    {
        auto& herder = static_cast<HerderImpl&>(node->getHerder());
        herder.mGetTopTransactionsForTesting = nullptr;
        herder.mPrepareLeadTimeForTesting.reset();
        herder.mEarlyTxSetMinBuildSavingForTesting.reset();
    }
}

// ---------------------------------------------------------------------------
// More of the early selection: capacity, the Soroban phase, one account in
// both phases, limits, and when it is used.
// ---------------------------------------------------------------------------

TEST_CASE("a set selected early is surge priced and validated past the "
          "pre-validation window",
          "[herder][txset][prevalidation][early-txset]")
{
    // Room for two operations per ledger: a build validates the first three
    // candidates (5/4 of the capacity) and goes on only if some are invalid.
    PrevalidationFixture f(/*herderSpeculates=*/false, /*maxTxSetSize=*/2);
    auto& app = *f.mApp;
    auto& herder = f.herder();
    auto& a = *f.mA;
    auto& b = *f.mB;
    auto& c = *f.mC;
    auto& d = *f.mD;
    auto& e = *f.mE;
    REQUIRE(f.lm().getLastMaxTxSetSizeOps() == 2);
    auto const root = app.getRoot()->getPublicKey();
    auto pay = [&](TestAccount& from, int64_t seqDelta, uint32_t fee) {
        return transactionFromOperations(
            app, from.getSecretKey(), from.getLastSequenceNumber() + seqDelta,
            {payment(root, 1)}, fee);
    };
    auto closingA = pay(a, 1, 100);
    auto closing = makeUnvalidatedTxSet(app, {closingA});
    // After the closing ledger's own transaction, the mempool holds B's
    // payment with a sequence number too far ahead, then three valid
    // payments with different fees. The pre-validation window ends before
    // the last (and best paying) one.
    auto futureB = pay(b, 5, 500);
    auto payC = pay(c, 1, 300);
    auto payD = pay(d, 1, 200);
    auto payE = pay(e, 1, 400);
    f.mMempool = {closingA, futureB, payC, payD, payE};
    auto const validatedBefore =
        meter(app, "prevalidate", "txs", "transaction");

    f.closeWithEarlySelection(closing);
    REQUIRE(Access::prepareEarly(herder, f.nextSeq(), f.nextCloseTime()));
    // The tasks validated the window; E's payment was validated by the
    // selection itself, when B's turned out invalid.
    REQUIRE(meter(app, "prevalidate", "txs", "transaction") ==
            validatedBefore + 3);
    auto prepared = Access::prepared(herder);
    REQUIRE(prepared);
    auto const lcl = f.lm().getLastClosedLedgerHeader();
    auto applicable = prepared->prepareForApply(app, lcl.header);
    REQUIRE(applicable);
    // The two best paying ones fit.
    auto const early = txHashes(applicable);
    REQUIRE(early == hashesOf({payC, payE}));
    REQUIRE(applicable->checkValid(app, 1, 1));
    // B's payment waits behind its account's sequence number.
    REQUIRE(Access::preparedInvalidTxs(herder).empty());

    // The same set as a build on the closed ledger selects.
    Access::clearCache(herder);
    f.mMempool = {futureB, payC, payD, payE};
    REQUIRE(f.build() == early);
}

TEST_CASE("a set selected early can hold Soroban transactions",
          "[herder][txset][prevalidation][early-txset][soroban]")
{
    PrevalidationFixture f;
    auto& app = *f.mApp;
    auto& herder = f.herder();
    auto& a = *f.mA;
    auto& b = *f.mB;
    auto const aSeq = a.getLastSequenceNumber();
    auto uploader = app.getRoot()->create(
        "S", app.getLedgerManager().getLastMinBalance(2) + 1'000'000'000);
    auto const sSeq = uploader.getLastSequenceNumber();
    SorobanResources resources;
    resources.instructions = 1'000'000;
    resources.diskReadBytes = 1000;
    resources.writeBytes = 1000;
    auto upload = createUploadWasmTx(app, uploader, 100,
                                     DEFAULT_TEST_RESOURCE_FEE, resources);
    REQUIRE(upload->isSoroban());
    auto closingA = paymentWithSeq(app, a, aSeq + 1);
    auto plainB = paymentWithSeq(app, b, b.getLastSequenceNumber() + 1);
    auto closing = makeUnvalidatedTxSet(app, {closingA});
    f.mMempool = {closingA, upload, plainB};

    f.closeWithEarlySelection(closing);
    REQUIRE(Access::prepareEarly(herder, f.nextSeq(), f.nextCloseTime()));
    auto prepared = Access::prepared(herder);
    REQUIRE(prepared);
    auto const lcl = f.lm().getLastClosedLedgerHeader();
    auto applicable = prepared->prepareForApply(app, lcl.header);
    REQUIRE(applicable);
    REQUIRE(applicable->sizeTx(TxSetPhase::CLASSIC) == 1);
    REQUIRE(applicable->sizeTx(TxSetPhase::SOROBAN) == 1);
    auto const early = txHashes(applicable);
    REQUIRE(early == hashesOf({upload, plainB}));
    REQUIRE(applicable->checkValid(app, 1, 1));

    // The build on the closed ledger agrees ...
    Access::clearCache(herder);
    f.mMempool = {upload, plainB};
    REQUIRE(f.build() == early);
    // ... and the ledger applies the set.
    auto const skipped = applyCounter(app, "txset-skipped");
    f.externalize(prepared);
    REQUIRE(applyCounter(app, "txset-skipped") == skipped);
    REQUIRE(uploader.getLastSequenceNumber() == sSeq + 1);
}

// A set holds one transaction per source account, across its phases. The
// mempool can hold an account's classic and Soroban transaction side by side
// (two nodes accepted one each).
TEST_CASE("candidates of one account in both phases",
          "[herder][txset][prevalidation][early-txset][soroban]")
{
    PrevalidationFixture f;
    auto& app = *f.mApp;
    auto& herder = f.herder();
    auto& a = *f.mA;
    auto& b = *f.mB;
    auto const aSeq = a.getLastSequenceNumber();
    auto both = app.getRoot()->create(
        "S", app.getLedgerManager().getLastMinBalance(2) + 1'000'000'000);
    auto const sSeq = both.getLastSequenceNumber();
    SorobanResources resources;
    resources.instructions = 1'000'000;
    resources.diskReadBytes = 1000;
    resources.writeBytes = 1000;
    auto plainB = paymentWithSeq(app, b, b.getLastSequenceNumber() + 1);
    auto classic = paymentWithSeq(app, both, sSeq + 1);
    auto soroban =
        createUploadWasmTx(app, both, 100, DEFAULT_TEST_RESOURCE_FEE, resources,
                           std::nullopt, 0, std::nullopt, sSeq + 1);
    auto laterClassic = paymentWithSeq(app, both, sSeq + 2);
    REQUIRE(soroban->getSeqNum() == classic->getSeqNum());

    SECTION("a build keeps one of them")
    {
        SECTION("the same sequence number: the classic one")
        {
            f.mMempool = {soroban, classic, plainB};
            REQUIRE(f.build() == hashesOf({classic, plainB}));
        }
        SECTION("the Soroban one comes first in the account's sequence")
        {
            f.mMempool = {laterClassic, soroban, plainB};
            REQUIRE(f.build() == hashesOf({soroban, plainB}));
        }
        SECTION("the classic one comes first in the account's sequence")
        {
            auto laterSoroban = createUploadWasmTx(
                app, both, 100, DEFAULT_TEST_RESOURCE_FEE, resources,
                std::nullopt, 0, std::nullopt, sSeq + 2);
            f.mMempool = {laterSoroban, classic, plainB};
            REQUIRE(f.build() == hashesOf({classic, plainB}));
        }
    }
    SECTION("so does a set selected early")
    {
        auto closingA = paymentWithSeq(app, a, aSeq + 1);
        f.mMempool = {closingA, soroban, classic, plainB};
        f.closeWithEarlySelection(makeUnvalidatedTxSet(app, {closingA}));
        REQUIRE(Access::prepareEarly(herder, f.nextSeq(), f.nextCloseTime()));
        auto applicable = Access::prepared(herder)->prepareForApply(
            app, f.lm().getLastClosedLedgerHeader().header);
        REQUIRE(applicable);
        REQUIRE(txHashes(applicable) == hashesOf({classic, plainB}));
        REQUIRE(applicable->checkValid(app, 1, 1));
    }
}

TEST_CASE("the limits a tx set is selected within",
          "[herder][txset][early-txset]")
{
    PrevalidationFixture f;
    auto& app = *f.mApp;
    auto const limits = TxSetBuildLimits::forLCL(app);
    REQUIRE(limits.mSorobanConfig);
    REQUIRE(limits.sameAs(limits));
    REQUIRE(limits.maxTxSetSizeOps() == f.lm().getLastMaxTxSetSizeOps());
    REQUIRE(limits.mLclHeader == f.lm().getLastClosedLedgerHeader().header);

    auto differs = [&](TxSetBuildLimits const& other) {
        return !limits.sameAs(other) && !other.sameAs(limits);
    };
    auto withHeader = [&](std::function<void(LedgerHeader&)> change) {
        auto other = limits;
        change(other.mLclHeader);
        return other;
    };
    // What selection reads of the header ...
    REQUIRE(differs(withHeader([](LedgerHeader& h) { h.ledgerVersion -= 1; })));
    REQUIRE(differs(withHeader([](LedgerHeader& h) { h.baseFee += 1; })));
    REQUIRE(differs(withHeader([](LedgerHeader& h) { h.baseReserve += 1; })));
    REQUIRE(differs(withHeader([](LedgerHeader& h) { h.maxTxSetSize += 1; })));
    // ... and what it does not: every ledger changes these.
    REQUIRE(limits.sameAs(withHeader([](LedgerHeader& h) {
        h.ledgerSeq += 1;
        h.feePool += 100;
        h.idPool += 1;
        h.scpValue.closeTime += 1;
        h.previousLedgerHash = sha256("another ledger");
        h.bucketListHash = sha256("another bucket list");
        h.txSetResultHash = sha256("other results");
    })));

    // The Soroban limits of the ledger.
    auto withoutSoroban = limits;
    withoutSoroban.mSorobanConfig = nullptr;
    REQUIRE(differs(withoutSoroban));
    auto withSoroban = [&](SorobanNetworkConfig& config,
                           std::function<void(SorobanNetworkConfig&)> change) {
        config = *limits.mSorobanConfig;
        change(config);
        auto other = limits;
        other.mSorobanConfig = &config;
        return other;
    };
    SorobanNetworkConfig config = *limits.mSorobanConfig;
    REQUIRE(limits.sameAs(withSoroban(config, [](SorobanNetworkConfig&) {})));
    REQUIRE(differs(withSoroban(
        config, [](SorobanNetworkConfig& c) { c.mLedgerMaxTxCount += 1; })));
    REQUIRE(differs(withSoroban(config, [](SorobanNetworkConfig& c) {
        c.mLedgerMaxInstructions += 1;
    })));
    REQUIRE(differs(withSoroban(config, [](SorobanNetworkConfig& c) {
        c.mLedgerMaxDependentTxClusters += 1;
    })));
    REQUIRE(differs(withSoroban(config, [](SorobanNetworkConfig& c) {
        c.mLedgerMaxTransactionsSizeBytes += 1;
    })));
    REQUIRE(differs(withSoroban(config, [](SorobanNetworkConfig& c) {
        c.mLedgerMaxDiskReadEntries += 1;
    })));
}

TEST_CASE("a set selected early replaces only a build that has to start at "
          "once",
          "[herder][txset][early-txset]")
{
    VirtualClock clock;
    auto const now = clock.now();
    auto const ms = [](int n) { return std::chrono::milliseconds(n); };
    // The trigger is due when the previous ledger closes.
    REQUIRE(Access::buildMustStartNow(true, now, now));
    REQUIRE(Access::buildMustStartNow(true, now + ms(100), now));
    // The trigger is ahead, but less than a build's lead time away.
    REQUIRE(Access::buildMustStartNow(false, now, now));
    REQUIRE(Access::buildMustStartNow(false, now - ms(1), now));
    // There is time for a build before the trigger: it holds what arrives
    // in the meantime, and is preferred.
    REQUIRE(!Access::buildMustStartNow(false, now + ms(1), now));
    REQUIRE(!Access::buildMustStartNow(false, now + ms(500), now));
}

TEST_CASE("the upgrades due are part of the value the leader executes early",
          "[herder][txset][speculative-apply][early-txset]")
{
    PrevalidationFixture f(/*herderSpeculates=*/true);
    auto& app = *f.mApp;
    auto& herder = f.herder();
    auto& a = *f.mA;
    f.mMempool = {paymentWithSeq(app, a, a.getLastSequenceNumber() + 1)};
    Upgrades::UpgradeParameters params;
    params.mUpgradeTime = VirtualClock::system_time_point{};
    params.mBaseFee = 250;
    herder.setUpgrades(params);
    auto const seq = f.nextSeq();
    auto const started = applyCounter(app, "speculation-started");

    Access::prepareByBuilding(herder, seq, f.nextCloseTime());
    REQUIRE(Access::prepared(herder));
    REQUIRE(applyCounter(app, "speculation-started") == started + 1);
    while (f.lm().isSpeculativelyApplying())
    {
        f.mClock.crank(true);
    }
    auto const proposal = Access::propose(herder, seq);
    auto const proposed = stellarValueOf(proposal);
    REQUIRE(proposed.upgrades.size() == 1);
    REQUIRE(Access::speculativeValue(herder));
    REQUIRE(Access::speculativeValue(herder)->second == proposal->getValue());
}

// Executing a ledger starts with the full validation of its set: the same
// check every other node runs when it applies the ledger. When the leader's
// own execution of its prepared set is done before the trigger, an invalid
// set is not proposed.
TEST_CASE("a prepared set that the leader finds invalid when executing it "
          "is not proposed",
          "[herder][txset][speculative-apply][early-txset]")
{
    PrevalidationFixture f(/*herderSpeculates=*/true);
    auto& app = *f.mApp;
    auto& herder = f.herder();
    auto& a = *f.mA;
    auto& b = *f.mB;
    auto& c = *f.mC;
    auto nextA = paymentWithSeq(app, a, a.getLastSequenceNumber() + 1);
    auto plainB = paymentWithSeq(app, b, b.getLastSequenceNumber() + 1);
    f.mMempool = {nextA, plainB};
    auto const seq = f.nextSeq();
    auto const failedBefore = txsetMeter(app, "self-check-failed");
    auto const passedBefore = txsetMeter(app, "self-check-passed");
    auto const skippedBefore = applyCounter(app, "txset-skipped");

    // What the node "builds" has the shape of its selection (two classic
    // transactions) and passes the structural check, but one of its
    // transactions is not valid: only validating the transactions shows it.
    auto invalid = makeUnvalidatedTxSet(
        app, {nextA, paymentWithSeq(app, c, c.getLastSequenceNumber() + 5)});
    herder.mBuiltTxSetOverrideForTesting = [&](TxSetXDRFrameConstPtr const&) {
        return invalid;
    };
    Access::prepareByBuilding(herder, seq, f.nextCloseTime());
    herder.mBuiltTxSetOverrideForTesting = nullptr;
    REQUIRE(Access::prepared(herder) == invalid);

    f.crankUntil([&]() {
        return txsetMeter(app, "self-check-failed") == failedBefore + 1 &&
               txsetMeter(app, "self-check-passed") == passedBefore + 1;
    });
    // The apply thread skipped the set, and the herder dropped it.
    REQUIRE(applyCounter(app, "txset-skipped") == skippedBefore + 1);
    REQUIRE(!Access::prepared(herder));
    REQUIRE(Access::selfCheckHasFailed(herder));
    REQUIRE(f.lm().getLastClosedLedgerNum() == seq - 1);
    // The trigger builds a set of its own.
    auto const proposed = stellarValueOf(Access::propose(herder, seq));
    REQUIRE(proposed.txSetHash != invalid->getContentsHash());
    auto txSet = herder.getTxSet(proposed.txSetHash);
    auto const* frame = std::get_if<TxSetXDRFrameConstPtr>(&txSet);
    REQUIRE(frame);
    REQUIRE(*frame);
    auto applicable = (*frame)->prepareForApply(
        app, f.lm().getLastClosedLedgerHeader().header);
    REQUIRE(applicable);
    REQUIRE(txHashes(applicable) == hashesOf({nextA, plainB}));
}

TEST_CASE("the application shuts down while a set is being checked or "
          "selected",
          "[herder][txset][prevalidation][early-txset]")
{
    auto f = std::make_unique<PrevalidationFixture>();
    auto& app = *f->mApp;
    auto& herder = f->herder();
    auto& a = *f->mA;
    auto const aSeq = a.getLastSequenceNumber();
    auto closingA = paymentWithSeq(app, a, aSeq + 1);
    auto nextA = paymentWithSeq(app, a, aSeq + 2);

    SECTION("the self-check of a set built before the trigger")
    {
        f->mMempool = {closingA};
        Access::prepareByBuilding(herder, f->nextSeq(), f->nextCloseTime());
        REQUIRE(Access::prepared(herder));
        f.reset();
    }
    SECTION("the selection")
    {
        f->mMempool = {closingA};
        for (int i = 0; i < 400; ++i)
        {
            f->mMempool.push_back(nextA);
        }
        REQUIRE(f->speculate(makeUnvalidatedTxSet(app, {closingA}), true));
        REQUIRE(Access::selectsEarly(herder));
        Access::waitUntilFinished(herder);
        // The validation is done; the selection may still be running.
        f.reset();
    }
    SECTION("the self-check of a set selected early")
    {
        f->mMempool = {closingA, nextA};
        f->closeWithEarlySelection(makeUnvalidatedTxSet(app, {closingA}));
        REQUIRE(Access::prepareEarly(herder, f->nextSeq(), f->nextCloseTime()));
        f.reset();
    }
}

// ---------------------------------------------------------------------------
// When a set selected early is used.
// ---------------------------------------------------------------------------

TEST_CASE("a set selected early is used only if a build of its size takes "
          "time",
          "[herder][txset][prevalidation][early-txset]")
{
    PrevalidationFixture f;
    auto& app = *f.mApp;
    auto& herder = f.herder();
    auto& a = *f.mA;
    auto& b = *f.mB;
    auto const aSeq = a.getLastSequenceNumber();
    auto closingA = paymentWithSeq(app, a, aSeq + 1);
    auto nextA = paymentWithSeq(app, a, aSeq + 2);
    auto plainB = paymentWithSeq(app, b, b.getLastSequenceNumber() + 1);
    auto closing = makeUnvalidatedTxSet(app, {closingA});
    auto const smallBefore = txsetMeter(app, "early-small");
    auto const discardedBefore = txsetMeter(app, "early-discarded");
    // The rule as the node applies it, not the tests' override.
    herder.mEarlyTxSetMinBuildSavingForTesting.reset();
    REQUIRE(Access::minBuildSaving() > std::chrono::milliseconds(0));

    SECTION("an empty selection is never proposed")
    {
        // All the mempool held when the previous ledger was executed were
        // that ledger's own transactions.
        f.mMempool = {closingA};
        herder.mEarlyTxSetMinBuildSavingForTesting =
            std::chrono::milliseconds(0);
        f.closeWithEarlySelection(closing);
        REQUIRE(!Access::prepareEarly(herder, f.nextSeq(), f.nextCloseTime()));
        REQUIRE(!Access::prepared(herder));
        REQUIRE(txsetMeter(app, "early-small") == smallBefore + 1);
        // What arrived during the ballot goes into the build.
        f.mMempool = {nextA, plainB};
        REQUIRE(f.build() == hashesOf({nextA, plainB}));
    }
    SECTION("a selection of a few transactions is left to a build")
    {
        f.mMempool = {closingA, nextA, plainB};
        f.closeWithEarlySelection(closing);
        // A build of two transactions costs next to nothing at this node's
        // rate (or at the rate assumed before it has built anything).
        Access::setBuildHistory(herder, std::chrono::milliseconds(1),
                                std::chrono::microseconds(40));
        REQUIRE(!Access::prepareEarly(herder, f.nextSeq(), f.nextCloseTime()));
        REQUIRE(!Access::prepared(herder));
        REQUIRE(txsetMeter(app, "early-small") == smallBefore + 1);
        REQUIRE(txsetMeter(app, "early-discarded") == discardedBefore + 1);
        // The build still has the job's validation results.
        f.mMempool = {nextA, plainB};
        auto const before = cacheCounts(app);
        REQUIRE(f.build() == hashesOf({nextA, plainB}));
        auto const after = cacheCounts(app);
        REQUIRE(after.mHit - before.mHit == 2);
    }
    SECTION("a selection whose build would take time is used")
    {
        f.mMempool = {closingA, nextA, plainB};
        f.closeWithEarlySelection(closing);
        // At 10 ms per transaction, two transactions are worth it ...
        Access::setBuildHistory(herder, std::chrono::milliseconds(1),
                                Access::minBuildSaving() / 2);
        REQUIRE(Access::prepareEarly(herder, f.nextSeq(), f.nextCloseTime()));
        REQUIRE(Access::prepared(herder));
        REQUIRE(txsetMeter(app, "early-small") == smallBefore);
    }
    SECTION("just below the threshold it is not")
    {
        f.mMempool = {closingA, nextA, plainB};
        f.closeWithEarlySelection(closing);
        Access::setBuildHistory(herder, std::chrono::milliseconds(1),
                                Access::minBuildSaving() / 2 -
                                    std::chrono::milliseconds(1));
        REQUIRE(!Access::prepareEarly(herder, f.nextSeq(), f.nextCloseTime()));
        REQUIRE(txsetMeter(app, "early-small") == smallBefore + 1);
    }
}

// HerderImpl::scheduleTxSetPreparation is what setupTriggerNextLedger does
// for the first-round leader once the previous ledger has closed and the next
// trigger's time is known.
TEST_CASE("how the leader gets its set ready after the previous ledger closed",
          "[herder][txset][prevalidation][early-txset]")
{
    PrevalidationFixture f;
    auto& app = *f.mApp;
    auto& herder = f.herder();
    auto& a = *f.mA;
    auto& b = *f.mB;
    auto const aSeq = a.getLastSequenceNumber();
    auto closingA = paymentWithSeq(app, a, aSeq + 1);
    auto nextA = paymentWithSeq(app, a, aSeq + 2);
    auto plainB = paymentWithSeq(app, b, b.getLastSequenceNumber() + 1);
    auto closing = makeUnvalidatedTxSet(app, {closingA});
    f.mMempool = {closingA, nextA, plainB};
    auto const finalizedBefore = txsetMeter(app, "early-finalized");
    auto const lead = Access::prepareLeadTime(herder);
    REQUIRE(lead > std::chrono::milliseconds(0));
    auto now = [&]() { return f.mClock.now(); };

    SECTION("with a set selected during the previous ballot")
    {
        f.closeWithEarlySelection(closing);
        auto const pulls = f.mPulls;
        SECTION("the trigger is already due: the set is completed at once")
        {
            Access::schedulePreparation(herder, f.nextSeq(), now(), true,
                                        f.nextCloseTime());
            REQUIRE(Access::prepared(herder));
            REQUIRE(!Access::buildScheduled(herder));
            REQUIRE(txsetMeter(app, "early-finalized") == finalizedBefore + 1);
            REQUIRE(f.mPulls == pulls);
        }
        SECTION("the trigger is less than a lead time away: the same")
        {
            Access::schedulePreparation(herder, f.nextSeq(), now() + lead / 2,
                                        false, f.nextCloseTime());
            REQUIRE(Access::prepared(herder));
            REQUIRE(!Access::buildScheduled(herder));
            REQUIRE(txsetMeter(app, "early-finalized") == finalizedBefore + 1);
        }
        SECTION("there is time for a build before the trigger: it is "
                "scheduled, a lead time ahead")
        {
            Access::schedulePreparation(herder, f.nextSeq(),
                                        now() + lead + std::chrono::seconds(1),
                                        false, f.nextCloseTime());
            REQUIRE(!Access::prepared(herder));
            REQUIRE(Access::buildScheduled(herder));
            REQUIRE(txsetMeter(app, "early-finalized") == finalizedBefore);
            // Nothing was built yet.
            REQUIRE(f.mPulls == pulls);
        }
        Access::discardPrepared(herder);
    }
    SECTION("without one")
    {
        f.externalize(closing);
        SECTION("a due trigger builds by itself: nothing is prepared or "
                "scheduled")
        {
            Access::schedulePreparation(herder, f.nextSeq(), now(), true,
                                        f.nextCloseTime());
            REQUIRE(!Access::prepared(herder));
            REQUIRE(!Access::buildScheduled(herder));
        }
        SECTION("a trigger ahead: a build is scheduled")
        {
            Access::schedulePreparation(herder, f.nextSeq(), now() + lead / 2,
                                        false, f.nextCloseTime());
            REQUIRE(!Access::prepared(herder));
            REQUIRE(Access::buildScheduled(herder));
        }
        Access::discardPrepared(herder);
    }
    SECTION("the selection is not usable: a build is scheduled as before")
    {
        // (the ledger closed with another set)
        REQUIRE(f.speculate(closing, true));
        Access::waitUntilFinished(herder);
        Access::waitUntilSelected(herder);
        f.externalize(makeUnvalidatedTxSet(app, {plainB}));
        Access::schedulePreparation(herder, f.nextSeq(), now() + lead / 2,
                                    false, f.nextCloseTime());
        REQUIRE(!Access::prepared(herder));
        REQUIRE(Access::buildScheduled(herder));
        Access::discardPrepared(herder);
    }
}

// The verdict of the node's own execution is about a set this node built,
// for the close time it built it for.
TEST_CASE("the verdict of the leader on its own set",
          "[herder][txset][speculative-apply][early-txset]")
{
    PrevalidationFixture f(/*herderSpeculates=*/true);
    auto& app = *f.mApp;
    auto& herder = f.herder();
    auto& a = *f.mA;
    auto& b = *f.mB;
    auto& c = *f.mC;
    auto const aSeq = a.getLastSequenceNumber();
    auto const failedBefore = txsetMeter(app, "self-check-failed");
    auto const skippedBefore = applyCounter(app, "txset-skipped");
    auto vote = [&](uint32_t seq, StellarValue const& value) {
        SCPEnvelope envelope;
        envelope.statement.slotIndex = seq;
        envelope.statement.pledges.type(SCP_ST_PREPARE);
        envelope.statement.pledges.prepare().ballot.counter = 1;
        envelope.statement.pledges.prepare().ballot.value =
            xdr::xdr_to_opaque(value);
        Access::emitted(herder, envelope);
    };
    auto executed = [&]() {
        while (f.lm().isSpeculativelyApplying())
        {
            f.mClock.crank(true);
        }
        f.crankFor(std::chrono::milliseconds(20));
    };

    SECTION("a set selected early that is invalid is dropped before the "
            "proposal")
    {
        auto closingA = paymentWithSeq(app, a, aSeq + 1);
        auto nextA = paymentWithSeq(app, a, aSeq + 2);
        auto plainB = paymentWithSeq(app, b, b.getLastSequenceNumber() + 1);
        f.mMempool = {closingA, nextA, plainB};
        f.closeWithEarlySelection(makeUnvalidatedTxSet(app, {closingA}));
        // Same shape as the selection, one transaction not valid.
        auto invalid = makeUnvalidatedTxSet(
            app,
            {nextA, paymentWithSeq(app, c, c.getLastSequenceNumber() + 5)});
        herder.mBuiltTxSetOverrideForTesting =
            [&](TxSetXDRFrameConstPtr const&) { return invalid; };
        REQUIRE(Access::prepareEarly(herder, f.nextSeq(), f.nextCloseTime()));
        herder.mBuiltTxSetOverrideForTesting = nullptr;
        REQUIRE(Access::prepared(herder) == invalid);
        f.crankUntil([&]() {
            return txsetMeter(app, "self-check-failed") == failedBefore + 1;
        });
        REQUIRE(applyCounter(app, "txset-skipped") == skippedBefore + 1);
        REQUIRE(!Access::prepared(herder));
        REQUIRE(Access::selfCheckHasFailed(herder));
    }
    SECTION("a verdict that comes after the proposal is recorded")
    {
        auto nextA = paymentWithSeq(app, a, aSeq + 1);
        auto plainB = paymentWithSeq(app, b, b.getLastSequenceNumber() + 1);
        f.mMempool = {nextA, plainB};
        auto invalid = makeUnvalidatedTxSet(
            app,
            {nextA, paymentWithSeq(app, c, c.getLastSequenceNumber() + 5)});
        herder.mBuiltTxSetOverrideForTesting =
            [&](TxSetXDRFrameConstPtr const&) { return invalid; };
        auto const seq = f.nextSeq();
        Access::prepareByBuilding(herder, seq, f.nextCloseTime());
        herder.mBuiltTxSetOverrideForTesting = nullptr;
        // The trigger fires before the execution has got anywhere.
        auto const proposed = stellarValueOf(Access::propose(herder, seq));
        REQUIRE(proposed.txSetHash == invalid->getContentsHash());
        f.crankUntil([&]() {
            return txsetMeter(app, "self-check-failed") == failedBefore + 1;
        });
        REQUIRE(Access::selfCheckHasFailed(herder));
        // The ledger closes without applying the set, here as everywhere.
        f.externalize(invalid);
        REQUIRE(f.lm().getLastClosedLedgerNum() == seq);
        REQUIRE(a.getLastSequenceNumber() == aSeq);
    }
    SECTION("an invalid set of another node says nothing about this node")
    {
        auto bad = paymentWithSeq(app, c, c.getLastSequenceNumber() + 5);
        auto foreign = makeUnvalidatedTxSet(app, {bad});
        auto const seq = f.nextSeq();
        herder.getPendingEnvelopes().putTxSet(foreign->getContentsHash(), seq,
                                              foreign);
        vote(seq, f.valueFor(foreign));
        executed();
        REQUIRE(applyCounter(app, "txset-skipped") == skippedBefore + 1);
        REQUIRE(txsetMeter(app, "self-check-failed") == failedBefore);
        REQUIRE(!Access::selfCheckHasFailed(herder));
    }
    SECTION("nor does this node's set under a close time it was not built "
            "for")
    {
        // Valid for the close time the node built it for; another leader
        // puts it forward for a later one, at which its time-bounded
        // transaction has expired.
        auto const builtFor = f.nextCloseTime();
        PreconditionsV2 cond;
        cond.timeBounds.activate().maxTime = builtFor.toApplyTime().timePoint();
        auto bounded = transactionWithV2Precondition(app, b, 1, 100, cond);
        f.mMempool = {bounded};
        auto const seq = f.nextSeq();
        Access::prepareByBuilding(herder, seq, builtFor);
        auto own = Access::prepared(herder);
        REQUIRE(own);
        REQUIRE(own->sizeTxTotal() == 1);
        executed();
        REQUIRE(applyCounter(app, "txset-skipped") == skippedBefore);
        herder.getPendingEnvelopes().putTxSet(own->getContentsHash(), seq, own);
        auto const later = ConsensusTime::fromApplyTime(
            ApplyTime::fromTimePoint(builtFor.toApplyTime().timePoint() + 5));
        vote(seq, herder.makeStellarValue(own->getContentsHash(), later,
                                          emptyUpgradeSteps, f.mCfg.NODE_SEED));
        executed();
        REQUIRE(applyCounter(app, "txset-skipped") == skippedBefore + 1);
        REQUIRE(txsetMeter(app, "self-check-failed") == failedBefore);
        REQUIRE(!Access::selfCheckHasFailed(herder));
        REQUIRE(Access::prepared(herder) == own);
    }
}

// What the selection itself can get wrong is checked before the set leaves
// the builder, also when the check of the written set comes later.
TEST_CASE("a build checks its selection before the set leaves it",
          "[herder][txset][early-txset][soroban]")
{
    PrevalidationFixture f;
    auto& app = *f.mApp;
    auto both = app.getRoot()->create(
        "S", app.getLedgerManager().getLastMinBalance(2) + 1'000'000'000);
    auto const sSeq = both.getLastSequenceNumber();
    SorobanResources resources;
    resources.instructions = 1'000'000;
    resources.diskReadBytes = 1000;
    resources.writeBytes = 1000;
    TransactionFrameBasePtr classic = paymentWithSeq(app, both, sSeq + 1);
    TransactionFrameBasePtr soroban =
        createUploadWasmTx(app, both, 100, DEFAULT_TEST_RESOURCE_FEE, resources,
                           std::nullopt, 0, std::nullopt, sSeq + 1);
    // Candidates as the herder must not hand them over: one account in both
    // phases.
    PerPhaseTransactionList phases{{classic}, {soroban}};
    PerPhaseTransactionList invalid(2);
    bool handedOut = false;
    auto onReady = [&](TxSetXDRFrameConstPtr const&) { handedOut = true; };

    SECTION("with the check of the written set deferred")
    {
        std::shared_ptr<TxSetSelfCheck const> check;
        REQUIRE_THROWS_WITH(
            makeTxSetFromTransactions(
                phases, app, ApplyTimeOffset::fromSeconds(1), invalid, false,
                {}, false, onReady, {}, nullptr, &check),
            Catch::Matchers::Contains("MULTIPLE_TXS_PER_SOURCE_ACCOUNT"));
        // Nothing was handed to the overlay or the peers.
        REQUIRE(!handedOut);
        REQUIRE(!check);
    }
    SECTION("with the check before returning, as always")
    {
        REQUIRE_THROWS_WITH(
            makeTxSetFromTransactions(phases, app,
                                      ApplyTimeOffset::fromSeconds(1), invalid,
                                      false, {}, false, onReady),
            Catch::Matchers::Contains("MULTIPLE_TXS_PER_SOURCE_ACCOUNT"));
    }
}
