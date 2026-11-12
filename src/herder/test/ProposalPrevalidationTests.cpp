// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// Tests of the leader's head start on the next transaction set: the state a
// speculative execution leaves is handed to the Herder
// (LedgerManager::startSpeculativeApply), the node that expects to lead the
// next ledger validates its candidates against it in the background
// (HerderImpl::startProposalPrevalidation), and the build of the next set
// takes those results instead of validating again (TxValidationCache).

#include "crypto/SHA.h"
#include "herder/Herder.h"
#include "herder/HerderImpl.h"
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
    prepareLeadTime(std::chrono::milliseconds expectedCloseTime)
    {
        return HerderImpl::prepareTxSetLeadTime(expectedCloseTime);
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

    PrevalidationFixture()
        : mCfg(getTestConfig(0, Config::TESTDB_BUCKET_DB_PERSISTENT))
    {
        mCfg.LEDGER_PROTOCOL_VERSION = GATE_PROTOCOL;
        mCfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION = GATE_PROTOCOL;
        // The tests drive speculation themselves.
        mCfg.EXPERIMENTAL_SPECULATIVE_LEDGER_APPLY = false;
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
        herder().mGetTopTransactionsForTesting = [this](size_t) {
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
    REQUIRE(Access::prepareLeadTime(milliseconds(800)) == milliseconds(200));
    REQUIRE(Access::prepareLeadTime(milliseconds(1000)) == milliseconds(250));
    // capped: a long close time does not make transactions wait longer
    REQUIRE(Access::prepareLeadTime(milliseconds(5000)) == milliseconds(250));
    REQUIRE(Access::prepareLeadTime(milliseconds(0)) == milliseconds(0));
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
