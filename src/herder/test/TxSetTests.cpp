// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/TxSetFrame.h"
#include "herder/test/TestTxSetUtils.h"
#include "ledger/LedgerManager.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "util/ProtocolVersion.h"

namespace stellar
{
namespace
{
using namespace txtest;

TEST_CASE("generalized tx set XDR validation", "[txset]")
{
    Config cfg(getTestConfig());
    cfg.LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(GENERALIZED_TX_SET_PROTOCOL_VERSION);
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(GENERALIZED_TX_SET_PROTOCOL_VERSION);
    VirtualClock clock;
    Application::pointer app = createTestApplication(clock, cfg);

    GeneralizedTransactionSet xdrTxSet(1);
    xdrTxSet.v1TxSet().previousLedgerHash =
        app->getLedgerManager().getLastClosedLedgerHeader().hash;

    SECTION("no phases")
    {
        auto txSet = TxSetFrame::makeFromWire(app->getNetworkID(), xdrTxSet);
        REQUIRE(!txSet->checkValidStructure());
    }
    SECTION("too many phases")
    {
        xdrTxSet.v1TxSet().phases.emplace_back();
        xdrTxSet.v1TxSet().phases.emplace_back();
        auto txSet = TxSetFrame::makeFromWire(app->getNetworkID(), xdrTxSet);
        REQUIRE(!txSet->checkValidStructure());
    }
    SECTION("incorrect base fee order")
    {
        xdrTxSet.v1TxSet().phases.emplace_back();
        SECTION("all components discounted")
        {

            xdrTxSet.v1TxSet().phases[0].v0Components().emplace_back(
                TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .baseFee.activate() = 1500;
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .txs.emplace_back();

            xdrTxSet.v1TxSet().phases[0].v0Components().emplace_back(
                TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .baseFee.activate() = 1400;
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .txs.emplace_back();

            xdrTxSet.v1TxSet().phases[0].v0Components().emplace_back(
                TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .baseFee.activate() = 1600;
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .txs.emplace_back();

            auto txSet =
                TxSetFrame::makeFromWire(app->getNetworkID(), xdrTxSet);
            REQUIRE(!txSet->checkValidStructure());
        }
        SECTION("non-discounted component out of place")
        {

            xdrTxSet.v1TxSet().phases[0].v0Components().emplace_back(
                TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .baseFee.activate() = 1500;
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .txs.emplace_back();

            xdrTxSet.v1TxSet().phases[0].v0Components().emplace_back(
                TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .baseFee.activate() = 1600;
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .txs.emplace_back();

            xdrTxSet.v1TxSet().phases[0].v0Components().emplace_back(
                TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);

            auto txSet =
                TxSetFrame::makeFromWire(app->getNetworkID(), xdrTxSet);
            REQUIRE(!txSet->checkValidStructure());
        }
        SECTION("with non-discounted component, discounted out of place")
        {
            xdrTxSet.v1TxSet().phases[0].v0Components().emplace_back(
                TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .txs.emplace_back();

            xdrTxSet.v1TxSet().phases[0].v0Components().emplace_back(
                TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .baseFee.activate() = 1500;
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .txs.emplace_back();

            xdrTxSet.v1TxSet().phases[0].v0Components().emplace_back(
                TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .baseFee.activate() = 1400;
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .txs.emplace_back();

            auto txSet =
                TxSetFrame::makeFromWire(app->getNetworkID(), xdrTxSet);
            REQUIRE(!txSet->checkValidStructure());
        }
    }
    SECTION("duplicate base fee")
    {
        xdrTxSet.v1TxSet().phases.emplace_back();
        SECTION("duplicate discounts")
        {
            xdrTxSet.v1TxSet().phases[0].v0Components().emplace_back(
                TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .txs.emplace_back();

            xdrTxSet.v1TxSet().phases[0].v0Components().emplace_back(
                TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .baseFee.activate() = 1500;
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .txs.emplace_back();

            xdrTxSet.v1TxSet().phases[0].v0Components().emplace_back(
                TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .baseFee.activate() = 1500;
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .txs.emplace_back();

            xdrTxSet.v1TxSet().phases[0].v0Components().emplace_back(
                TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .baseFee.activate() = 1600;
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .txs.emplace_back();

            auto txSet =
                TxSetFrame::makeFromWire(app->getNetworkID(), xdrTxSet);
            REQUIRE(!txSet->checkValidStructure());
        }
        SECTION("duplicate non-discounted components")
        {

            xdrTxSet.v1TxSet().phases[0].v0Components().emplace_back(
                TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .txs.emplace_back();

            xdrTxSet.v1TxSet().phases[0].v0Components().emplace_back(
                TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .txs.emplace_back();

            xdrTxSet.v1TxSet().phases[0].v0Components().emplace_back(
                TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .baseFee.activate() = 1500;
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .txs.emplace_back();

            auto txSet =
                TxSetFrame::makeFromWire(app->getNetworkID(), xdrTxSet);
            REQUIRE(!txSet->checkValidStructure());
        }
    }
    SECTION("empty component")
    {
        xdrTxSet.v1TxSet().phases.emplace_back();

        xdrTxSet.v1TxSet().phases[0].v0Components().emplace_back(
            TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);

        auto txSet = TxSetFrame::makeFromWire(app->getNetworkID(), xdrTxSet);
        REQUIRE(!txSet->checkValidStructure());
    }
    SECTION("valid XDR")
    {
        SECTION("no transactions")
        {
            xdrTxSet.v1TxSet().phases.emplace_back();
            auto txSet =
                TxSetFrame::makeFromWire(app->getNetworkID(), xdrTxSet);
            REQUIRE(txSet->checkValidStructure());
        }
        SECTION("single component")
        {
            xdrTxSet.v1TxSet().phases.emplace_back();
            xdrTxSet.v1TxSet().phases[0].v0Components().emplace_back(
                TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .txs.emplace_back();
            auto txSet =
                TxSetFrame::makeFromWire(app->getNetworkID(), xdrTxSet);
            REQUIRE(txSet->checkValidStructure());
        }
        SECTION("multiple components")
        {
            xdrTxSet.v1TxSet().phases.emplace_back();

            xdrTxSet.v1TxSet().phases[0].v0Components().emplace_back(
                TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .txs.emplace_back();

            xdrTxSet.v1TxSet().phases[0].v0Components().emplace_back(
                TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .baseFee.activate() = 1400;
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .txs.emplace_back();

            xdrTxSet.v1TxSet().phases[0].v0Components().emplace_back(
                TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .baseFee.activate() = 1500;
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .txs.emplace_back();

            xdrTxSet.v1TxSet().phases[0].v0Components().emplace_back(
                TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .baseFee.activate() = 1600;
            xdrTxSet.v1TxSet()
                .phases[0]
                .v0Components()
                .back()
                .txsMaybeDiscountedFee()
                .txs.emplace_back();

            auto txSet =
                TxSetFrame::makeFromWire(app->getNetworkID(), xdrTxSet);
            REQUIRE(txSet->checkValidStructure());
        }
    }
}

TEST_CASE("generalized tx set XDR conversion", "[txset]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(GENERALIZED_TX_SET_PROTOCOL_VERSION);
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(GENERALIZED_TX_SET_PROTOCOL_VERSION);
    Application::pointer app = createTestApplication(clock, cfg);
    auto root = TestAccount::createRoot(*app);
    auto createTxs = [&](int cnt, int fee) {
        std::vector<TransactionFrameBasePtr> txs;
        for (int i = 0; i < cnt; ++i)
        {
            txs.emplace_back(transactionFromOperations(
                *app, root.getSecretKey(), root.nextSequenceNumber(),
                {createAccount(getAccount(std::to_string(i)).getPublicKey(),
                               1)},
                fee));
        }
        return txs;
    };

    auto checkXdrRoundtrip = [&](GeneralizedTransactionSet const& txSetXdr) {
        auto frame = TxSetFrame::makeFromWire(app->getNetworkID(), txSetXdr);
        REQUIRE(frame->checkValid(*app, 0, 0));
        GeneralizedTransactionSet newXdr;
        frame->toXDR(newXdr);
        REQUIRE(newXdr == txSetXdr);
    };

    SECTION("empty set")
    {
        auto txSetFrame = testtxset::makeNonValidatedGeneralizedTxSet(
            {}, app->getNetworkID(),
            app->getLedgerManager().getLastClosedLedgerHeader().hash);

        GeneralizedTransactionSet txSetXdr;
        txSetFrame->toXDR(txSetXdr);
        REQUIRE(txSetXdr.v1TxSet().phases[0].v0Components().empty());
        checkXdrRoundtrip(txSetXdr);
    }
    SECTION("one discounted component set")
    {
        auto txSetFrame = testtxset::makeNonValidatedGeneralizedTxSet(
            {std::make_pair(1234LL, createTxs(5, 1234))}, app->getNetworkID(),
            app->getLedgerManager().getLastClosedLedgerHeader().hash);

        GeneralizedTransactionSet txSetXdr;
        txSetFrame->toXDR(txSetXdr);
        REQUIRE(txSetXdr.v1TxSet().phases[0].v0Components().size() == 1);
        REQUIRE(*txSetXdr.v1TxSet()
                     .phases[0]
                     .v0Components()[0]
                     .txsMaybeDiscountedFee()
                     .baseFee == 1234);
        REQUIRE(txSetXdr.v1TxSet()
                    .phases[0]
                    .v0Components()[0]
                    .txsMaybeDiscountedFee()
                    .txs.size() == 5);
        checkXdrRoundtrip(txSetXdr);
    }
    SECTION("one non-discounted component set")
    {
        auto txSetFrame = testtxset::makeNonValidatedGeneralizedTxSet(
            {std::make_pair(std::nullopt, createTxs(5, 4321))},
            app->getNetworkID(),
            app->getLedgerManager().getLastClosedLedgerHeader().hash);

        GeneralizedTransactionSet txSetXdr;
        txSetFrame->toXDR(txSetXdr);
        REQUIRE(txSetXdr.v1TxSet().phases[0].v0Components().size() == 1);
        REQUIRE(!txSetXdr.v1TxSet()
                     .phases[0]
                     .v0Components()[0]
                     .txsMaybeDiscountedFee()
                     .baseFee);
        REQUIRE(txSetXdr.v1TxSet()
                    .phases[0]
                    .v0Components()[0]
                    .txsMaybeDiscountedFee()
                    .txs.size() == 5);
        checkXdrRoundtrip(txSetXdr);
    }
    SECTION("multiple component sets")
    {
        auto txSetFrame = testtxset::makeNonValidatedGeneralizedTxSet(
            {std::make_pair(12345LL, createTxs(3, 12345)),
             std::make_pair(123LL, createTxs(1, 123)),
             std::make_pair(1234LL, createTxs(2, 1234)),
             std::make_pair(std::nullopt, createTxs(4, 4321))},
            app->getNetworkID(),
            app->getLedgerManager().getLastClosedLedgerHeader().hash);

        GeneralizedTransactionSet txSetXdr;
        txSetFrame->toXDR(txSetXdr);
        auto const& comps = txSetXdr.v1TxSet().phases[0].v0Components();
        REQUIRE(comps.size() == 4);
        REQUIRE(!comps[0].txsMaybeDiscountedFee().baseFee);
        REQUIRE(comps[0].txsMaybeDiscountedFee().txs.size() == 4);
        REQUIRE(*comps[1].txsMaybeDiscountedFee().baseFee == 123);
        REQUIRE(comps[1].txsMaybeDiscountedFee().txs.size() == 1);
        REQUIRE(*comps[2].txsMaybeDiscountedFee().baseFee == 1234);
        REQUIRE(comps[2].txsMaybeDiscountedFee().txs.size() == 2);
        REQUIRE(*comps[3].txsMaybeDiscountedFee().baseFee == 12345);
        REQUIRE(comps[3].txsMaybeDiscountedFee().txs.size() == 3);
        checkXdrRoundtrip(txSetXdr);
    }
    SECTION("built from transactions")
    {
        auto const& lclHeader =
            app->getLedgerManager().getLastClosedLedgerHeader();
        std::vector<TransactionFrameBasePtr> txs;
        for (int i = 0; i < 5; ++i)
        {
            txs.push_back(root.tx({createAccount(
                getAccount(std::to_string(i)).getPublicKey(), 1)}));
        }
        auto txSet = TxSetFrame::makeFromTransactions(txs, *app, 0, 0);

        GeneralizedTransactionSet txSetXdr;
        txSet->toXDR(txSetXdr);
        REQUIRE(txSetXdr.v1TxSet().phases[0].v0Components().size() == 1);
        REQUIRE(*txSetXdr.v1TxSet()
                     .phases[0]
                     .v0Components()[0]
                     .txsMaybeDiscountedFee()
                     .baseFee == lclHeader.header.baseFee);
        REQUIRE(txSetXdr.v1TxSet()
                    .phases[0]
                    .v0Components()[0]
                    .txsMaybeDiscountedFee()
                    .txs.size() == 5);
        checkXdrRoundtrip(txSetXdr);
    }
}

TEST_CASE("generalized tx set fees", "[txset]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(GENERALIZED_TX_SET_PROTOCOL_VERSION);
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(GENERALIZED_TX_SET_PROTOCOL_VERSION);
    Application::pointer app = createTestApplication(clock, cfg);
    auto root = TestAccount::createRoot(*app);
    int accountId = 1;

    auto createTx = [&](int opCnt, int fee) {
        std::vector<Operation> ops;
        for (int i = 0; i < opCnt; ++i)
        {
            ops.emplace_back(createAccount(
                getAccount(std::to_string(accountId++)).getPublicKey(), 1));
        }
        return transactionFromOperations(*app, root.getSecretKey(),
                                         root.nextSequenceNumber(), ops, fee);
    };

    SECTION("valid txset")
    {
        auto txSet = testtxset::makeNonValidatedGeneralizedTxSet(
            {std::make_pair(
                 500, std::vector<TransactionFrameBasePtr>{createTx(1, 1000),
                                                           createTx(3, 1500)}),
             std::make_pair(
                 1000, std::vector<TransactionFrameBasePtr>{createTx(4, 5000),
                                                            createTx(1, 1000),
                                                            createTx(5, 6000)}),
             std::make_pair(std::nullopt,
                            std::vector<TransactionFrameBasePtr>{
                                createTx(2, 10000), createTx(5, 100000)})},
            app->getNetworkID(),
            app->getLedgerManager().getLastClosedLedgerHeader().hash);

        REQUIRE(txSet->checkValid(*app, 0, 0));
        std::vector<std::optional<int64_t>> fees;
        for (auto const& tx : txSet->getTxs())
        {
            fees.push_back(txSet->getTxBaseFee(
                tx,
                app->getLedgerManager().getLastClosedLedgerHeader().header));
        }
        std::sort(fees.begin(), fees.end());
        REQUIRE(fees ==
                std::vector<std::optional<int64_t>>{
                    std::nullopt, std::nullopt, 500, 500, 1000, 1000, 1000});
    }

    SECTION("tx with too low discounted fee")
    {
        auto txSet = testtxset::makeNonValidatedGeneralizedTxSet(
            {std::make_pair(
                500, std::vector<TransactionFrameBasePtr>{createTx(2, 999)})},
            app->getNetworkID(),
            app->getLedgerManager().getLastClosedLedgerHeader().hash);

        REQUIRE(!txSet->checkValid(*app, 0, 0));
    }

    SECTION("tx with too low non-discounted fee")
    {
        auto txSet = testtxset::makeNonValidatedGeneralizedTxSet(
            {std::make_pair(
                std::nullopt,
                std::vector<TransactionFrameBasePtr>{createTx(2, 199)})},
            app->getNetworkID(),
            app->getLedgerManager().getLastClosedLedgerHeader().hash);

        REQUIRE(!txSet->checkValid(*app, 0, 0));
    }
}

} // namespace
} // namespace stellar
