// Copyright 2026 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// Tests of speculative ledger apply: the ledger apply thread executes the
// next ledger as soon as the node votes for a value for it, and the ledger
// is committed only if that value externalizes (see
// LedgerManager::startSpeculativeApply).

#include "crypto/SHA.h"
#include "herder/Herder.h"
#include "herder/HerderImpl.h"
#include "herder/LedgerCloseData.h"
#include "herder/test/TestTxSetUtils.h"
#include "ledger/LedgerHeaderUtils.h"
#include "ledger/LedgerManager.h"
#include "ledger/test/LedgerTestUtils.h"
#include "main/Application.h"
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
#include <fmt/format.h>
#include <set>

using namespace stellar;
using namespace stellar::txtest;

namespace
{
int64_t
applyCounter(Application& app, std::string const& name)
{
    return app.getMetrics().NewCounter({"ledger", "apply", name}).count();
}

struct SpeculationCounts
{
    int64_t mStarted;
    int64_t mHit;
    int64_t mMiss;
    bool
    operator==(SpeculationCounts const& other) const
    {
        return mStarted == other.mStarted && mHit == other.mHit &&
               mMiss == other.mMiss;
    }
};

SpeculationCounts
speculationCounts(Application& app)
{
    return SpeculationCounts{applyCounter(app, "speculation-started"),
                             applyCounter(app, "speculation-hit"),
                             applyCounter(app, "speculation-miss")};
}

Hash
emptyResultSetHash()
{
    return xdrSha256(TransactionResultSet{});
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
paymentWithSeq(Application& app, TestAccount& from, SequenceNumber seq)
{
    return transactionFromOperations(
        app, from.getSecretKey(), seq,
        {payment(app.getRoot()->getPublicKey(), 1)}, 100);
}

// A node whose ledgers apply on the ledger apply thread, with two funded
// accounts.
struct SpeculativeApplyFixture
{
    VirtualClock mClock;
    Config mCfg;
    Application::pointer mApp;
    std::optional<TestAccount> mA;
    std::optional<TestAccount> mB;

    explicit SpeculativeApplyFixture(int configNum = 0)
        : mCfg(getTestConfig(configNum, Config::TESTDB_BUCKET_DB_PERSISTENT))
    {
        mCfg.LEDGER_PROTOCOL_VERSION = static_cast<uint32_t>(
            APPLY_TIME_TX_SET_VALIDATION_PROTOCOL_VERSION);
        mCfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
            mCfg.LEDGER_PROTOCOL_VERSION;
        // The tests below drive speculation through the LedgerManager; keep
        // the Herder from starting its own on the ledgers they close.
        mCfg.EXPERIMENTAL_SPECULATIVE_LEDGER_APPLY = false;
        REQUIRE(mCfg.backgroundLedgerApply());
        mApp = createTestApplication(mClock, mCfg);
        auto root = mApp->getRoot();
        auto const balance =
            mApp->getLedgerManager().getLastMinBalance(2) + 10'000'000;
        mA.emplace(root->create("A", balance));
        mB.emplace(root->create("B", balance));
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
        auto& herder = static_cast<HerderImpl&>(mApp->getHerder());
        return herder.makeStellarValue(txSet->getContentsHash(),
                                       nextCloseTime(), upgrades,
                                       mCfg.NODE_SEED);
    }

    // Executes the next ledger with `txSet` on the apply thread without
    // closing it, and waits until it has been executed.
    void
    speculate(TxSetXDRFrameConstPtr const& txSet,
              xdr::xvector<UpgradeType, 6> const& upgrades = emptyUpgradeSteps)
    {
        lm().startSpeculativeApply(
            LedgerCloseData(nextSeq(), txSet, valueFor(txSet, upgrades)));
        REQUIRE(lm().isSpeculativelyApplying());
        while (lm().isSpeculativelyApplying())
        {
            mClock.crank(true);
        }
    }

    // Externalizes the value `speculate` used for `txSet`, which closes the
    // ledger.
    void
    externalize(
        TxSetXDRFrameConstPtr const& txSet,
        xdr::xvector<UpgradeType, 6> const& upgrades = emptyUpgradeSteps)
    {
        mApp->getHerder().externalizeValue(txSet, nextSeq(), nextCloseTime(),
                                           upgrades);
    }
};
}

TEST_CASE("speculative apply commits the executed ledger",
          "[ledger][speculative-apply]")
{
    SpeculativeApplyFixture f;
    auto& app = *f.mApp;
    auto& lm = f.lm();
    auto& a = *f.mA;
    auto const before = speculationCounts(app);
    auto const lclBefore = lm.getLastClosedLedgerHeader();
    auto const aSeq = a.getLastSequenceNumber();
    auto const aBalance = a.getBalance();

    auto txSet = makeUnvalidatedTxSet(app, {paymentWithSeq(app, a, aSeq + 1)});
    f.speculate(txSet);

    // Executed, but nothing of it is visible.
    REQUIRE(lm.getLastClosedLedgerHeader().hash == lclBefore.hash);
    REQUIRE(a.getLastSequenceNumber() == aSeq);
    REQUIRE(a.getBalance() == aBalance);
    REQUIRE(!lm.isApplying());
    REQUIRE(speculationCounts(app) ==
            SpeculationCounts{before.mStarted + 1, before.mHit, before.mMiss});

    // The same value externalizes: the ledger closes with what was executed.
    f.externalize(txSet);
    auto const& lcl = lm.getLastClosedLedgerHeader();
    REQUIRE(lcl.header.ledgerSeq == lclBefore.header.ledgerSeq + 1);
    REQUIRE(lcl.header.scpValue.txSetHash == txSet->getContentsHash());
    REQUIRE(lcl.header.txSetResultHash != emptyResultSetHash());
    REQUIRE(a.getLastSequenceNumber() == aSeq + 1);
    REQUIRE(a.getBalance() == aBalance - 100 - 1);
    REQUIRE(speculationCounts(app) == SpeculationCounts{before.mStarted + 1,
                                                        before.mHit + 1,
                                                        before.mMiss});

    // and the next ledger closes without any speculation
    auto next = makeUnvalidatedTxSet(app, {paymentWithSeq(app, a, aSeq + 2)});
    closeLedger(app, next);
    REQUIRE(a.getLastSequenceNumber() == aSeq + 2);
    REQUIRE(speculationCounts(app) == SpeculationCounts{before.mStarted + 1,
                                                        before.mHit + 1,
                                                        before.mMiss});
}

TEST_CASE("speculative apply is dropped when another value externalizes",
          "[ledger][speculative-apply]")
{
    SpeculativeApplyFixture f;
    auto& app = *f.mApp;
    auto& lm = f.lm();
    auto& a = *f.mA;
    auto& b = *f.mB;
    auto const before = speculationCounts(app);
    auto const aSeq = a.getLastSequenceNumber();
    auto const aBalance = a.getBalance();
    auto const bSeq = b.getLastSequenceNumber();

    auto speculated =
        makeUnvalidatedTxSet(app, {paymentWithSeq(app, a, aSeq + 1)});
    auto other = makeUnvalidatedTxSet(app, {paymentWithSeq(app, b, bSeq + 1)});
    REQUIRE(speculated->getContentsHash() != other->getContentsHash());

    SECTION("another transaction set")
    {
        f.speculate(speculated);
        f.externalize(other);
        // Only the externalized set was applied.
        REQUIRE(lm.getLastClosedLedgerHeader().header.scpValue.txSetHash ==
                other->getContentsHash());
        REQUIRE(a.getLastSequenceNumber() == aSeq);
        REQUIRE(a.getBalance() == aBalance);
        REQUIRE(b.getLastSequenceNumber() == bSeq + 1);
        REQUIRE(speculationCounts(app) == SpeculationCounts{before.mStarted + 1,
                                                            before.mHit,
                                                            before.mMiss + 1});
    }

    SECTION("the same transaction set with another close time")
    {
        f.speculate(speculated);
        auto const seq = f.nextSeq();
        auto const laterCloseTime =
            ConsensusTime::fromApplyTime(ApplyTime::fromTimePoint(
                lm.getLastClosedLedgerHeader().header.scpValue.closeTime + 5));
        app.getHerder().externalizeValue(speculated, seq, laterCloseTime,
                                         emptyUpgradeSteps);
        REQUIRE(lm.getLastClosedLedgerNum() == seq);
        REQUIRE(a.getLastSequenceNumber() == aSeq + 1);
        REQUIRE(speculationCounts(app) == SpeculationCounts{before.mStarted + 1,
                                                            before.mHit,
                                                            before.mMiss + 1});
    }

    SECTION("a newer speculation replaces it")
    {
        f.speculate(speculated);
        f.speculate(other);
        REQUIRE(speculationCounts(app) == SpeculationCounts{before.mStarted + 2,
                                                            before.mHit,
                                                            before.mMiss + 1});
        f.externalize(other);
        REQUIRE(a.getLastSequenceNumber() == aSeq);
        REQUIRE(b.getLastSequenceNumber() == bSeq + 1);
        REQUIRE(speculationCounts(app) == SpeculationCounts{before.mStarted + 2,
                                                            before.mHit + 1,
                                                            before.mMiss + 1});
    }

    SECTION("aborted explicitly")
    {
        f.speculate(speculated);
        lm.abortSpeculativeApply();
        REQUIRE(speculationCounts(app) == SpeculationCounts{before.mStarted + 1,
                                                            before.mHit,
                                                            before.mMiss + 1});
        // aborting again is a no-op
        lm.abortSpeculativeApply();
        // The ledger still applies normally afterwards, value included.
        f.externalize(speculated);
        REQUIRE(a.getLastSequenceNumber() == aSeq + 1);
        REQUIRE(speculationCounts(app) == SpeculationCounts{before.mStarted + 1,
                                                            before.mHit,
                                                            before.mMiss + 1});
    }

    SECTION("still held at shutdown")
    {
        f.speculate(speculated);
        // The application is destroyed with the execution in place.
    }
}

TEST_CASE("speculative apply of an invalid tx set closes an empty ledger",
          "[ledger][speculative-apply][apply-time-validation]")
{
    SpeculativeApplyFixture f;
    auto& app = *f.mApp;
    auto& lm = f.lm();
    auto& a = *f.mA;
    auto& b = *f.mB;
    auto const before = speculationCounts(app);
    auto const aSeq = a.getLastSequenceNumber();
    auto const aBalance = a.getBalance();
    auto const skippedBefore = applyCounter(app, "txset-skipped");

    auto good = paymentWithSeq(app, a, aSeq + 1);
    auto bad = paymentWithSeq(app, b, b.getLastSequenceNumber() + 10);
    auto invalid = makeUnvalidatedTxSet(app, {good, bad});

    f.speculate(invalid);
    f.externalize(invalid);

    auto const& lcl = lm.getLastClosedLedgerHeader();
    REQUIRE(lcl.header.scpValue.txSetHash == invalid->getContentsHash());
    REQUIRE(lcl.header.txSetResultHash == emptyResultSetHash());
    REQUIRE(a.getLastSequenceNumber() == aSeq);
    REQUIRE(a.getBalance() == aBalance);
    REQUIRE(applyCounter(app, "txset-skipped") == skippedBefore + 1);
    REQUIRE(speculationCounts(app) == SpeculationCounts{before.mStarted + 1,
                                                        before.mHit + 1,
                                                        before.mMiss});
}

// The ledgers a node closes must not depend on whether, or on what, it
// executed speculatively.
TEST_CASE("speculative apply closes the same ledgers as regular apply",
          "[ledger][speculative-apply]")
{
    enum class Mode
    {
        REGULAR,
        SPECULATE_EXTERNALIZED,
        SPECULATE_OTHER
    };
    auto run = [](Mode mode) {
        SpeculativeApplyFixture f;
        auto& app = *f.mApp;
        auto& a = *f.mA;
        auto& b = *f.mB;
        std::vector<Hash> hashes;
        for (int i = 0; i < 6; ++i)
        {
            auto const aSeq = a.getLastSequenceNumber();
            auto const bSeq = b.getLastSequenceNumber();
            std::vector<TransactionFrameBasePtr> txs{
                paymentWithSeq(app, a, aSeq + 1)};
            if (i % 3 == 1)
            {
                // an invalid set every third ledger
                txs.emplace_back(paymentWithSeq(app, b, bSeq + 10));
            }
            else
            {
                txs.emplace_back(paymentWithSeq(app, b, bSeq + 1));
            }
            auto txSet = makeUnvalidatedTxSet(app, txs);
            // Two of the values carry an upgrade, one of them with a set that
            // gets skipped: upgrades are applied when the ledger commits.
            xdr::xvector<UpgradeType, 6> upgrades;
            if (i == 1 || i == 3)
            {
                LedgerUpgrade upgrade(LEDGER_UPGRADE_MAX_TX_SET_SIZE);
                upgrade.newMaxTxSetSize() = 200 + i;
                upgrades.emplace_back(LedgerTestUtils::toUpgradeType(upgrade));
            }
            switch (mode)
            {
            case Mode::REGULAR:
                break;
            case Mode::SPECULATE_EXTERNALIZED:
                f.speculate(txSet, upgrades);
                break;
            case Mode::SPECULATE_OTHER:
                f.speculate(makeUnvalidatedTxSet(
                    app, {paymentWithSeq(app, b, bSeq + 1)}));
                break;
            }
            f.externalize(txSet, upgrades);
            auto const& lcl = f.lm().getLastClosedLedgerHeader();
            if (i == 1 || i == 3)
            {
                REQUIRE(lcl.header.maxTxSetSize ==
                        static_cast<uint32_t>(200 + i));
            }
            hashes.emplace_back(lcl.hash);
        }
        return hashes;
    };

    auto regular = run(Mode::REGULAR);
    REQUIRE(regular.size() == 6);
    REQUIRE(run(Mode::SPECULATE_EXTERNALIZED) == regular);
    REQUIRE(run(Mode::SPECULATE_OTHER) == regular);
}

// With real consensus every node executes the leader's value while the
// ballot protocol runs, and commits it when it externalizes.
TEST_CASE("network executes ledgers speculatively",
          "[ledger][speculative-apply][simulation]")
{
    auto runTest = [](bool speculate) {
        Hash networkID = sha256(getTestConfig().NETWORK_PASSPHRASE);
        auto simulation = Topologies::core(4, 0.75, networkID, [&](int i) {
            auto cfg = getTestConfig(i, Config::TESTDB_BUCKET_DB_PERSISTENT);
            cfg.ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING = true;
            cfg.EXPERIMENTAL_SPECULATIVE_LEDGER_APPLY = speculate;
            REQUIRE(cfg.backgroundLedgerApply());
            return cfg;
        });
        auto nodes = simulation->getNodes();

        // Every leader proposes one payment from the root account, so that
        // the ledgers being executed are not empty.
        for (auto const& node : nodes)
        {
            auto& app = *node;
            static_cast<HerderImpl&>(app.getHerder()).mProposedTxSetOverride =
                [&app](LedgerHeaderHistoryEntry const& lcl) {
                    auto root = app.getRoot();
                    auto tx = paymentWithSeq(app, *root,
                                             root->getLastSequenceNumber() + 1);
                    return testtxset::makeNonValidatedGeneralizedTxSet(
                               {{std::make_pair(
                                    std::nullopt,
                                    std::vector<TransactionFrameBasePtr>{tx})},
                                {}},
                               app, lcl.hash)
                        .first;
                };
        }
        simulation->startAllNodes();

        uint32_t const target = 20;
        simulation->crankUntil(
            [&]() { return simulation->haveAllExternalized(target, 1); },
            40 * simulation->getExpectedLedgerCloseTime(), false);
        REQUIRE(simulation->haveAllExternalized(target, 1));

        // All nodes closed the same ledgers, and the payments were applied.
        auto& lm0 = nodes[0]->getLedgerManager();
        auto const seq = lm0.getLastClosedLedgerNum();
        auto const expected = lm0.getLastClosedLedgerHeader();
        for (auto const& node : nodes)
        {
            auto& lm = node->getLedgerManager();
            simulation->crankUntil(
                [&]() { return lm.getLastClosedLedgerNum() >= seq; },
                10 * simulation->getExpectedLedgerCloseTime(), false);
            if (lm.getLastClosedLedgerNum() == seq)
            {
                REQUIRE(lm.getLastClosedLedgerHeader().hash == expected.hash);
            }
            auto const counts = speculationCounts(*node);
            CAPTURE(counts.mStarted, counts.mHit, counts.mMiss);
            if (speculate)
            {
                // Most ledgers close in the first round, on the value every
                // node voted for.
                REQUIRE(counts.mHit >= 10);
                REQUIRE(counts.mStarted >= counts.mHit + counts.mMiss);
            }
            else
            {
                REQUIRE(counts == SpeculationCounts{0, 0, 0});
            }
        }
        REQUIRE(nodes[0]->getRoot()->getLastSequenceNumber() >= 10);
    };

    SECTION("speculative apply on")
    {
        runTest(true);
    }
    SECTION("speculative apply off")
    {
        runTest(false);
    }
}
