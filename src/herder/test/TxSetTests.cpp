// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/TxSetFrame.h"
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

// TEST_CASE("generalized tx set XDR validation", "[txset]")
//{
//     SECTION("wrong txset version")
//     {
//         GeneralizedTransactionSet txSet;
//         REQUIRE(!validateTxSetXDRStructure(txSet));
//     }
//     SECTION("no phases")
//     {
//         GeneralizedTransactionSet txSet(1);
//         REQUIRE(!validateTxSetXDRStructure(txSet));
//     }
//     SECTION("wrong phase version")
//     {
//         GeneralizedTransactionSet txSet(1);
//         txSet.v1TxSet().phases.emplace_back(1);
//         REQUIRE(!validateTxSetXDRStructure(txSet));
//     }
//     SECTION("too many phases")
//     {
//         GeneralizedTransactionSet txSet(1);
//         txSet.v1TxSet().phases.emplace_back();
//         txSet.v1TxSet().phases.emplace_back();
//         REQUIRE(!validateTxSetXDRStructure(txSet));
//     }
//     SECTION("too many BID_IS_FEE components")
//     {
//         GeneralizedTransactionSet txSet(1);
//         txSet.v1TxSet().phases.emplace_back();
//         txSet.v1TxSet().phases[0].v0Components().emplace_back(
//             TXSET_COMP_TXS_BID_IS_FEE);
//         txSet.v1TxSet().phases[0].v0Components().emplace_back(
//             TXSET_COMP_TXS_BID_IS_FEE);
//         REQUIRE(!validateTxSetXDRStructure(txSet));
//     }
//     SECTION("valid XDR")
//     {
//         GeneralizedTransactionSet txSet(1);
//         txSet.v1TxSet().phases.emplace_back();
//         txSet.v1TxSet().phases[0].v0Components().emplace_back(
//             TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
//         txSet.v1TxSet().phases[0].v0Components().emplace_back(
//             TXSET_COMP_TXS_BID_IS_FEE);
//         txSet.v1TxSet().phases[0].v0Components().emplace_back(
//             TXSET_COMP_TXS_MAYBE_DISCOUNTED_FEE);
//         REQUIRE(validateTxSetXDRStructure(txSet));
//     }
// }

// TEST_CASE("generalized tx set XDR conversion", "[txset]")
//{
//    if (protocolVersionIsBefore(Config::CURRENT_LEDGER_PROTOCOL_VERSION,
//                                GENERALIZED_TX_SET_PROTOCOL_VERSION))
//    {
//        return;
//    }
//    VirtualClock clock;
//    auto cfg = getTestConfig();
//    cfg.LEDGER_PROTOCOL_VERSION =
//        static_cast<uint32_t>(GENERALIZED_TX_SET_PROTOCOL_VERSION);
//    Application::pointer app = createTestApplication(clock, cfg);
//    auto root = TestAccount::createRoot(*app);
//    auto createTxs = [&root](int cnt)
//    {
//        std::vector<TransactionFrameBasePtr> txs;
//        for (int i = 0; i < cnt; ++i)
//        {
//            txs.emplace_back(root.tx({createAccount(
//                getAccount(std::to_string(i)).getPublicKey(), 1)}));
//        }
//        return txs;
//    };
//
//    SECTION("empty set")
//    {
//        auto txSetFrame = createGeneralizedTxSet({}, {}, *app);
//        GeneralizedTransactionSet txSetXdr;
//        txSetFrame->toXDR(txSetXdr);
//        REQUIRE(validateTxSetXDRStructure(txSetXdr));
//        REQUIRE(txSetXdr.v1TxSet().phases[0].v0Components().empty());
//    }
//    SECTION("one discounted component set")
//    {
//        auto txSetFrame = createGeneralizedTxSet(
//            {std::make_pair(1234LL, createTxs(5))}, {}, *app);
//        GeneralizedTransactionSet txSetXdr;
//        txSetFrame->toXDR(txSetXdr);
//        REQUIRE(validateTxSetXDRStructure(txSetXdr));
//        REQUIRE(txSetXdr.v1TxSet().phases[0].v0Components().size() == 1);
//        REQUIRE(txSetXdr.v1TxSet()
//                    .phases[0]
//                    .v0Components()[0]
//                    .txsMaybeDiscountedFee()
//                    .baseFee == 1234);
//        REQUIRE(txSetXdr.v1TxSet()
//                    .phases[0]
//                    .v0Components()[0]
//                    .txsMaybeDiscountedFee()
//                    .txs.size() == 5);
//    }
//    SECTION("one non-discounted component set")
//    {
//        auto txSetFrame = createGeneralizedTxSet({}, createTxs(5), *app);
//        GeneralizedTransactionSet txSetXdr;
//        txSetFrame->toXDR(txSetXdr);
//        REQUIRE(validateTxSetXDRStructure(txSetXdr));
//        REQUIRE(txSetXdr.v1TxSet().phases[0].v0Components().size() == 1);
//        REQUIRE(txSetXdr.v1TxSet()
//                    .phases[0]
//                    .v0Components()[0]
//                    .txsBidIsFee()
//                    .size() == 5);
//    }
//    SECTION("multiple component sets")
//    {
//        auto txSetFrame =
//            createGeneralizedTxSet({std::make_pair(12345LL, createTxs(3)),
//                                    std::make_pair(123LL, createTxs(1)),
//                                    std::make_pair(1234LL, createTxs(2))},
//                                   createTxs(10), *app);
//        GeneralizedTransactionSet txSetXdr;
//        txSetFrame->toXDR(txSetXdr);
//        REQUIRE(validateTxSetXDRStructure(txSetXdr));
//        auto const& comps = txSetXdr.v1TxSet().phases[0].v0Components();
//        REQUIRE(comps.size() == 4);
//        REQUIRE(comps[0].txsBidIsFee().size() == 10);
//        REQUIRE(comps[1].txsMaybeDiscountedFee().baseFee == 123);
//        REQUIRE(comps[1].txsMaybeDiscountedFee().txs.size() == 1);
//        REQUIRE(comps[2].txsMaybeDiscountedFee().baseFee == 1234);
//        REQUIRE(comps[2].txsMaybeDiscountedFee().txs.size() == 2);
//        REQUIRE(comps[3].txsMaybeDiscountedFee().baseFee == 12345);
//        REQUIRE(comps[3].txsMaybeDiscountedFee().txs.size() == 3);
//    }
//    /*SECTION("built by adding transactions")
//    {
//        auto const& lclHeader =
//            app->getLedgerManager().getLastClosedLedgerHeader();
//        auto txSet = TxSetFrame::makeFromTransactions(
//            lclHeader.hash, lclHeader.header.ledgerVersion);
//        for (int i = 0; i < 5; ++i)
//        {
//            txSet->add(root.tx({createAccount(
//                getAccount(std::to_string(i)).getPublicKey(), 1)}));
//        }
//        txSet->computeTxFees(lclHeader.header);
//        txSet->sortForHash();
//        GeneralizedTransactionSet txSetXdr;
//        txSet->toXDR(txSetXdr);
//        REQUIRE(txSetXdr.v1TxSet().phases[0].v0Components().size() == 1);
//        REQUIRE(*txSetXdr.v1TxSet()
//                     .phases[0]
//                     .v0Components()[0]
//                     .txsMaybeDiscountedFee()
//                     .baseFee == lclHeader.header.baseFee);
//        REQUIRE(*txSetXdr.v1TxSet()
//                     .phases[0]
//                     .v0Components()[0]
//                     .txsMaybeDiscountedFee()
//                     .txs.size() == 5);
//    }*/
//}

// TEST_CASE("generalized tx set fees", "[txset]")
//{
//    VirtualClock clock;
//    auto cfg = getTestConfig();
//    cfg.LEDGER_PROTOCOL_VERSION =
//        static_cast<uint32_t>(GENERALIZED_TX_SET_PROTOCOL_VERSION);
//    Application::pointer app = createTestApplication(clock, cfg);
//    auto root = TestAccount::createRoot(*app);
//    int accountId = 1;
//
//    auto createTx = [&](int opCnt, int fee)
//    {
//        std::vector<Operation> ops;
//        for (int i = 0; i < opCnt; ++i)
//        {
//            ops.emplace_back(createAccount(
//                getAccount(std::to_string(accountId++)).getPublicKey(), 1));
//        }
//        return transactionFromOperations(*app, root.getSecretKey(),
//                                         root.nextSequenceNumber(), ops, fee);
//    };
//
//    SECTION("valid txset")
//    {
//        std::vector<TransactionFrameBasePtr> discounted500Txs = {
//            createTx(1, 1000), createTx(3, 1500)};
//        std::vector<TransactionFrameBasePtr> discounted1000Txs = {
//            createTx(4, 5000), createTx(1, 1000), createTx(5, 6000)};
//        std::vector<TransactionFrameBasePtr> noDiscountTxs = {
//            createTx(2, 10000), createTx(5, 100000)};
//
//        auto txSet =
//            createGeneralizedTxSet({std::make_pair(500, discounted500Txs),
//                                    std::make_pair(1000, discounted1000Txs)},
//                                   noDiscountTxs, *app);
//
//        REQUIRE(txSet->checkValid(*app, 0, 0));
//        std::vector<std::optional<int64_t>> fees;
//        for (auto const& tx : txSet->mTransactions)
//        {
//            fees.push_back(txSet->getTxBaseFee(tx));
//        }
//        std::sort(fees.begin(), fees.end());
//        REQUIRE(fees ==
//                std::vector<std::optional<int64_t>>{
//                    std::nullopt, std::nullopt, 500, 500, 1000, 1000, 1000});
//    }
//
//    SECTION("tx with too low discounted fee")
//    {
//        std::vector<TransactionFrameBasePtr> txs = {createTx(2, 999)};
//        auto txSet =
//            createGeneralizedTxSet({std::make_pair(500, txs)}, {}, *app);
//
//        REQUIRE(!txSet->checkValid(*app, 0, 0));
//    }
//
//    SECTION("tx with too low non-discounted fee")
//    {
//        std::vector<TransactionFrameBasePtr> txs = {createTx(2, 199)};
//        auto txSet = createGeneralizedTxSet({}, txs, *app);
//
//        REQUIRE(!txSet->checkValid(*app, 0, 0));
//    }
//}

} // namespace
} // namespace stellar
