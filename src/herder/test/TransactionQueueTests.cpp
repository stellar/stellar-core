// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "herder/Herder.h"
#include "herder/HerderImpl.h"
#include "herder/SurgePricingUtils.h"
#include "herder/TransactionQueue.h"
#include "herder/TxQueueLimiter.h"
#include "herder/TxSetFrame.h"
#include "herder/TxSetUtils.h"
#include "ledger/LedgerHashUtils.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/MutableTransactionResult.h"
#include "transactions/SignatureUtils.h"
#include "transactions/TransactionBridge.h"
#include "transactions/TransactionUtils.h"
#include "transactions/test/SorobanTxTestUtils.h"
#include "util/Timer.h"
#include "util/numeric128.h"
#include "xdr/Stellar-transaction.h"

#include "xdrpp/autocheck.h"

#include <chrono>
#include <fmt/chrono.h>
#include <lib/catch.hpp>
#include <numeric>

using namespace stellar;
using namespace stellar::txtest;

namespace
{
TransactionTestFramePtr
transaction(Application& app, TestAccount& account, int64_t sequenceDelta,
            int64_t amount, uint32_t fee, int nbOps = 1, bool isSoroban = false)
{
    if (!isSoroban)
    {
        std::vector<Operation> ops;
        for (int i = 0; i < nbOps; ++i)
        {
            ops.emplace_back(payment(account.getPublicKey(), amount));
        }
        return transactionFromOperations(
            app, account, account.getLastSequenceNumber() + sequenceDelta, ops,
            fee);
    }
    else
    {
        REQUIRE(nbOps == 1);
        SorobanResources resources;
        resources.instructions = 1;
        return createUploadWasmTx(app, account, fee, DEFAULT_TEST_RESOURCE_FEE,
                                  resources, std::nullopt, 0, std::nullopt,
                                  account.getLastSequenceNumber() +
                                      sequenceDelta);
    }
}

TransactionTestFramePtr
invalidTransaction(Application& app, TestAccount& account, int sequenceDelta)
{
    return transactionFromOperations(
        app, account, account.getLastSequenceNumber() + sequenceDelta,
        {payment(account.getPublicKey(), -1)});
}

class TransactionQueueTest
{
  public:
    struct TransactionQueueState
    {
        struct AccountState
        {
            AccountID mAccountID;
            uint32 mAge;
            std::vector<TransactionFrameBasePtr> mAccountTransactions;
        };

        struct BannedState
        {
            std::vector<TransactionFrameBasePtr> mBanned0;
            std::vector<TransactionFrameBasePtr> mBanned1;
        };

        std::vector<AccountState> mAccountStates;
        BannedState mBannedState;
    };

    explicit TransactionQueueTest(TransactionQueue& queue)
        : mTransactionQueue(queue)
    {
    }

    TransactionQueue::AddResult
    add(TransactionFrameBasePtr const& tx,
        TransactionQueue::AddResultCode expected)
    {
        auto res = mTransactionQueue.tryAdd(tx, false);
        REQUIRE(res.code == expected);
        return res;
    }

    TransactionQueue&
    getTransactionQueue()
    {
        return mTransactionQueue;
    }

    void
    removeApplied(std::vector<TransactionFrameBasePtr> const& toRemove,
                  bool noChangeExpected = false)
    {
        auto size = mTransactionQueue.getTransactions({}).size();
        mTransactionQueue.removeApplied(toRemove);

        if (noChangeExpected)
        {
            REQUIRE(size == mTransactionQueue.getTransactions({}).size());
        }
        else
        {
            REQUIRE(size - toRemove.size() >=
                    mTransactionQueue.getTransactions({}).size());
        }

        // Everything that got removed should have age=0
        for (auto const& tx : toRemove)
        {
            auto txInfo = mTransactionQueue.getAccountTransactionQueueInfo(
                tx->getSourceID());
            REQUIRE(txInfo.mAge == 0);
        }
    }

    void
    ban(std::vector<TransactionFrameBasePtr> const& toRemove)
    {
        auto txsBefore = mTransactionQueue.getTransactions({});
        // count the number of transactions from `toRemove` already included
        auto inPoolCount = std::count_if(
            toRemove.begin(), toRemove.end(),
            [&](TransactionFrameBasePtr const& tx) {
                auto const& txs = txsBefore;
                return std::any_of(txs.begin(), txs.end(),
                                   [&](TransactionFrameBasePtr const& tx2) {
                                       return tx2->getFullHash() ==
                                              tx->getFullHash();
                                   });
            });
        mTransactionQueue.ban(toRemove);
        auto txsAfter = mTransactionQueue.getTransactions({});
        REQUIRE(txsBefore.size() - inPoolCount >= txsAfter.size());
    }

    void
    shift()
    {
        mTransactionQueue.shift();
    }

    void
    check(const TransactionQueueState& state)
    {
        std::map<AccountID, int64_t> expectedFees;
        for (auto const& accountState : state.mAccountStates)
        {
            for (auto const& tx : accountState.mAccountTransactions)
            {
                auto& fee = expectedFees[tx->getFeeSourceID()];
                if (INT64_MAX - fee > tx->getFullFee())
                {
                    fee += tx->getFullFee();
                }
                else
                {
                    fee = INT64_MAX;
                }
            }
        }

        std::map<AccountID, int64_t> fees;
        auto queueTxs = mTransactionQueue.getTransactions({});
        for (auto const& tx : queueTxs)
        {
            auto& fee = fees[tx->getFeeSourceID()];
            if (INT64_MAX - fee > tx->getFullFee())
            {
                fee += tx->getFullFee();
            }
            else
            {
                fee = INT64_MAX;
            }
        }

        REQUIRE(fees == expectedFees);

        TxFrameList expectedTxs;
        size_t totOps = 0;
        for (auto const& accountState : state.mAccountStates)
        {
            auto& txs = accountState.mAccountTransactions;
            auto seqNum = txs.empty() ? 0 : txs.back()->getSeqNum();
            auto accountTransactionQueueInfo =
                mTransactionQueue.getAccountTransactionQueueInfo(
                    accountState.mAccountID);
            REQUIRE(accountTransactionQueueInfo.mTotalFees ==
                    expectedFees[accountState.mAccountID]);
            auto queueSeqNum =
                accountTransactionQueueInfo.mTransaction
                    ? accountTransactionQueueInfo.mTransaction->mTx->getSeqNum()
                    : 0;
            totOps += accountTransactionQueueInfo.mTransaction
                          ? accountTransactionQueueInfo.mTransaction->mTx
                                ->getNumOperations()
                          : 0;
            REQUIRE(queueSeqNum == seqNum);
            REQUIRE(accountTransactionQueueInfo.mAge == accountState.mAge);

            expectedTxs.insert(expectedTxs.end(),
                               accountState.mAccountTransactions.begin(),
                               accountState.mAccountTransactions.end());
        }

        REQUIRE(totOps == mTransactionQueue.getQueueSizeOps());

        REQUIRE_THAT(queueTxs, Catch::Matchers::UnorderedEquals(expectedTxs));
        REQUIRE(state.mBannedState.mBanned0.size() ==
                mTransactionQueue.countBanned(0));
        REQUIRE(state.mBannedState.mBanned1.size() ==
                mTransactionQueue.countBanned(1));
        for (auto const& tx : state.mBannedState.mBanned0)
        {
            REQUIRE(mTransactionQueue.isBanned(tx->getFullHash()));
        }
        for (auto const& tx : state.mBannedState.mBanned1)
        {
            REQUIRE(mTransactionQueue.isBanned(tx->getFullHash()));
        }
    }

  private:
    TransactionQueue& mTransactionQueue;
};
}

TEST_CASE("TransactionQueue complex scenarios", "[herder][transactionqueue]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 4;
    cfg.FLOOD_TX_PERIOD_MS = 100;
    auto app = createTestApplication(clock, cfg);
    auto queue = ClassicTransactionQueue{*app, 4, 2, 2};
    auto const minBalance2 = app->getLedgerManager().getLastMinBalance(2);

    auto root = TestAccount::createRoot(*app);
    auto account1 = root.create("a1", minBalance2);
    auto account2 = root.create("a2", minBalance2);
    auto account3 = root.create("a3", minBalance2);

    auto txSeqA1T0 = transaction(*app, account1, 0, 1, 200);
    auto txSeqA1T1 = transaction(*app, account1, 1, 1, 200);
    auto txSeqA1T2 = transaction(*app, account1, 2, 1, 400, 2);
    auto txSeqA1T1V2 = transaction(*app, account1, 1, 2, 200);
    auto txSeqA1T2V2 = transaction(*app, account1, 2, 2, 200);
    auto txSeqA1T3 = transaction(*app, account1, 3, 1, 200);
    auto txSeqA1T4 = transaction(*app, account1, 4, 1, 200);
    auto txSeqA2T1 = transaction(*app, account2, 1, 1, 200);
    auto txSeqA2T2 = transaction(*app, account2, 2, 1, 200);
    auto txSeqA3T1 = transaction(*app, account3, 1, 1, 100);

    SECTION("multiple good sequence numbers, with four shifts")
    {
        TransactionQueueTest test{queue};
        test.add(txSeqA1T1,
                 TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}});
        test.add(txSeqA1T2,
                 TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}});

        test.shift();
        test.check({{{account1, 1, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 2, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 3, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1}, {account2}}, {{txSeqA1T1}}});
        test.shift();
        test.check({{{account1}, {account2}}, {{}, {txSeqA1T1}}});
        test.shift();
        test.check({{{account1}, {account2}}});
    }

    SECTION("multiple good sequence numbers, with replace")
    {
        TransactionQueueTest test{queue};
        test.add(txSeqA1T1,
                 TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 1, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 2, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 3, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1}, {account2}}, {{txSeqA1T1}}});
        // Transactions are banned
        test.add(txSeqA1T1,
                 TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
        test.check({{{account1}, {account2}}, {{txSeqA1T1}}});

        // Can't add txSeqA1T2V2 before txSeqA1T1V2
        test.add(txSeqA1T2V2,
                 TransactionQueue::AddResultCode::ADD_STATUS_ERROR);
        test.check({{{account1}, {account2}}, {{txSeqA1T1}}});

        // Adding txSeqA1T1V2 with the same seqnum as txSeqA1T1 ("replace")
        test.add(txSeqA1T1V2,
                 TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1V2}}, {account2}}, {{txSeqA1T1}}});

        // Can't add txSeqA1T1 or txSeqA1T2, still banned
        test.add(txSeqA1T1,
                 TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
        test.check({{{account1, 0, {txSeqA1T1V2}}, {account2}}, {{txSeqA1T1}}});
        test.add(txSeqA1T2,
                 TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
        test.check({{{account1, 0, {txSeqA1T1V2}}, {account2}}, {{txSeqA1T1}}});
    }

    SECTION("multiple good sequence numbers, with shifts between")
    {
        TransactionQueueTest test{queue};
        test.add(txSeqA1T1,
                 TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 1, {txSeqA1T1}}, {account2}}});
        test.add(txSeqA1T2,
                 TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
        test.check({{{account1, 1, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 2, {txSeqA1T1}}, {account2}}});
        test.add(txSeqA1T3,
                 TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
        test.check({{{account1, 2, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 3, {txSeqA1T1}}, {account2}}});
        test.add(txSeqA1T4,
                 TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
        test.check({{{account1, 3, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1}, {account2}}, {{txSeqA1T1}}});
        test.shift();
        test.check({{{account1}, {account2}}, {{}, {txSeqA1T1}}});
        test.shift();
        test.check({{{account1}, {account2}}});
    }

    SECTION(
        "multiple good sequence numbers, different accounts, with four shifts")
    {
        TransactionQueueTest test{queue};
        test.add(txSeqA1T1,
                 TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}});
        test.add(txSeqA2T1,
                 TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2, 0, {txSeqA2T1}}}});
        test.add(txSeqA1T2,
                 TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2, 0, {txSeqA2T1}}}});
        test.add(txSeqA2T2,
                 TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2, 0, {txSeqA2T1}}}});
        test.shift();
        test.check({{{account1, 1, {txSeqA1T1}}, {account2, 1, {txSeqA2T1}}}});
        test.shift();
        test.check({{{account1, 2, {txSeqA1T1}}, {account2, 2, {txSeqA2T1}}}});
        test.shift();
        test.check({{{account1, 3, {txSeqA1T1}}, {account2, 3, {txSeqA2T1}}}});
        test.shift();
        // Everything should be banned now
        test.check({{{account1}, {account2}}, {{txSeqA1T1, txSeqA2T1}}});
    }

    SECTION("multiple good sequence numbers, different accounts, with shifts "
            "between")
    {
        TransactionQueueTest test{queue};
        test.add(txSeqA1T1,
                 TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 1, {txSeqA1T1}}, {account2}}});
        test.add(txSeqA2T1,
                 TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
        test.check({{{account1, 1, {txSeqA1T1}}, {account2, 0, {txSeqA2T1}}}});
        test.shift();
        test.check({{{account1, 2, {txSeqA1T1}}, {account2, 1, {txSeqA2T1}}}});
        test.add(txSeqA1T2,
                 TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
        test.check({{{account1, 2, {txSeqA1T1}}, {account2, 1, {txSeqA2T1}}}});
        test.shift();
        test.check({{{account1, 3, {txSeqA1T1}}, {account2, 2, {txSeqA2T1}}}});
        test.add(txSeqA2T2,
                 TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
        test.check({{{account1, 3, {txSeqA1T1}}, {account2, 2, {txSeqA2T1}}}});
        test.shift();
        test.check({{{account1}, {account2, 3, {txSeqA2T1}}}, {{txSeqA1T1}}});
        test.shift();
        test.check({{{account1}, {account2}}, {{txSeqA2T1}, {txSeqA1T1}}});
        test.shift();
        test.check({{{account1}, {account2}}, {{}, {txSeqA2T1}}});
        test.shift();
        test.check({{{account1}, {account2}}});
    }

    SECTION("multiple good sequence numbers, different accounts, with remove")
    {
        TransactionQueueTest test{queue};
        SECTION("with shift and remove")
        {
            test.add(txSeqA1T1,
                     TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
            test.check({{{account1, 0, {txSeqA1T1}}, {account2}}});
            test.add(txSeqA2T1,
                     TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
            test.check(
                {{{account1, 0, {txSeqA1T1}}, {account2, 0, {txSeqA2T1}}}});
            test.add(
                txSeqA1T2,
                TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
            test.check(
                {{{account1, 0, {txSeqA1T1}}, {account2, 0, {txSeqA2T1}}}});
            test.add(
                txSeqA2T2,
                TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
            test.check(
                {{{account1, 0, {txSeqA1T1}}, {account2, 0, {txSeqA2T1}}}});
            test.shift();
            test.check(
                {{{account1, 1, {txSeqA1T1}}, {account2, 1, {txSeqA2T1}}}});
            test.removeApplied({txSeqA1T1, txSeqA2T1});
            test.check({{{account1, 0, {}}, {account2}},
                        {{txSeqA1T1, txSeqA2T1}, {}}});
        }
        SECTION("with remove")
        {
            test.add(txSeqA1T1,
                     TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
            test.check({{{account1, 0, {txSeqA1T1}}, {account2}}});
            test.add(txSeqA2T1,
                     TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
            test.check(
                {{{account1, 0, {txSeqA1T1}}, {account2, 0, {txSeqA2T1}}}});
            test.add(
                txSeqA2T2,
                TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
            test.check(
                {{{account1, 0, {txSeqA1T1}}, {account2, 0, {txSeqA2T1}}}});
            test.removeApplied({txSeqA2T1});
            test.check({{{account1, 0, {txSeqA1T1}}, {account2, 0, {}}},
                        {{txSeqA2T1}, {}}});
            test.removeApplied({txSeqA1T1});
            test.check(
                {{{account1}, {account2}}, {{txSeqA2T1, txSeqA1T1}, {}}});
        }
    }

    SECTION("multiple good sequence numbers, different accounts, with ban")
    {
        TransactionQueueTest test{queue};
        test.add(txSeqA1T1,
                 TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
        test.add(txSeqA2T1,
                 TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
        test.add(txSeqA1T2,
                 TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
        test.add(txSeqA2T2,
                 TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
        test.shift();
        test.ban({txSeqA1T1, txSeqA2T2, txSeqA3T1});
        test.check({{{account1}, {account2, 1, {txSeqA2T1}}},
                    {{txSeqA1T1, txSeqA2T2, txSeqA3T1}}});
        test.add(txSeqA1T1,
                 TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
        test.check({{{account1}, {account2, 1, {txSeqA2T1}}},
                    {{txSeqA1T1, txSeqA2T2, txSeqA3T1}}});

        // still banned when we shift
        test.shift();
        test.check({{{account1}, {account2, 2, {txSeqA2T1}}},
                    {{}, {txSeqA1T1, txSeqA2T2, txSeqA3T1}}});
        test.add(txSeqA1T1,
                 TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
        test.add(txSeqA3T1,
                 TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
        // not banned anymore
        test.shift();
        test.add(txSeqA1T1,
                 TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
        test.add(txSeqA3T1,
                 TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}},
                     {account2, 3, {txSeqA2T1}},
                     {account3, 0, {txSeqA3T1}}}});
    }
}

void
testTransactionQueueBasicScenarios()
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 4;
    cfg.FLOOD_TX_PERIOD_MS = 100;
    auto app = createTestApplication(clock, cfg);
    auto queue = ClassicTransactionQueue{*app, 4, 2, 2};
    auto const minBalance2 = app->getLedgerManager().getLastMinBalance(2);

    auto root = TestAccount::createRoot(*app);
    auto account1 = root.create("a1", minBalance2);
    auto account2 = root.create("a2", minBalance2);
    auto account3 = root.create("a3", minBalance2);

    auto txSeqA1T0 = transaction(*app, account1, 0, 1, 200);
    auto txSeqA1T1 = transaction(*app, account1, 1, 1, 200);
    auto txSeqA1T2 = transaction(*app, account1, 2, 1, 400, 2);
    auto txSeqA1T1V2 = transaction(*app, account1, 1, 2, 200);
    auto txSeqA1T2V2 = transaction(*app, account1, 2, 2, 200);
    auto txSeqA1T3 = transaction(*app, account1, 3, 1, 200);
    auto txSeqA1T4 = transaction(*app, account1, 4, 1, 200);
    auto txSeqA2T1 = transaction(*app, account2, 1, 1, 200);
    auto txSeqA2T2 = transaction(*app, account2, 2, 1, 200);
    auto txSeqA3T1 = transaction(*app, account3, 1, 1, 100);

    SECTION("simple sequence")
    {
        TransactionQueueTest test{queue};

        CLOG_INFO(Tx, "Adding first transaction");
        // adding first tx
        // too small seqnum
        test.add(txSeqA1T0, TransactionQueue::AddResultCode::ADD_STATUS_ERROR);
        test.check({{{account1}, {account2}}, {}});
        // too big seqnum
        test.add(txSeqA1T2, TransactionQueue::AddResultCode::ADD_STATUS_ERROR);
        test.check({{{account1}, {account2}}, {}});

        test.add(txSeqA1T1,
                 TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}, {}});

        CLOG_INFO(Tx, "Adding second transaction");
        // adding second tx
        TransactionQueueTest::TransactionQueueState state;
        test.add(txSeqA1T2,
                 TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
        state = {{{account1, 0, {txSeqA1T1}}, {account2}}, {}};
        test.check(state);

        CLOG_INFO(Tx, "Adding third transaction");
        // adding third tx
        // duplicates
        test.add(txSeqA1T1,
                 TransactionQueue::AddResultCode::ADD_STATUS_DUPLICATE);
        test.check(state);

        test.add(txSeqA1T2,
                 TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
        test.check(state);

        // Tx is rejected due to limit or bad seqnum
        // too low
        test.add(txSeqA1T0, TransactionQueue::AddResultCode::ADD_STATUS_ERROR);
        test.check(state);
        // too high
        test.add(txSeqA1T4,
                 TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
        test.check(state);
        // just right
        test.add(txSeqA1T3,
                 TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
        test.check(state);
    }

    SECTION("good sequence number, same twice with shift")
    {
        TransactionQueueTest test{queue};
        test.add(txSeqA1T1,
                 TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}, {}});
        test.shift();
        test.check({{{account1, 1, {txSeqA1T1}}, {account2}}, {}});
        test.add(txSeqA1T1,
                 TransactionQueue::AddResultCode::ADD_STATUS_DUPLICATE);
        test.check({{{account1, 1, {txSeqA1T1}}, {account2}}, {}});
    }

    SECTION("good then big sequence number, with shift")
    {
        TransactionQueueTest test{queue};
        test.add(txSeqA1T1,
                 TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}, {}});
        test.shift();
        test.check({{{account1, 1, {txSeqA1T1}}, {account2}}, {}});
        auto status =
            TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER;

        test.add(txSeqA1T3, status);
        test.check({{{account1, 1, {txSeqA1T1}}, {account2}}, {}});
    }

    SECTION("good then good sequence number, with shift")
    {
        TransactionQueueTest test{queue};
        test.add(txSeqA1T1,
                 TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
        TransactionQueueTest::TransactionQueueState state = {
            {{account1, 0, {txSeqA1T1}}, {account2}}, {}};
        test.check(state);
        test.shift();
        state.mAccountStates[0].mAge += 1;
        test.check(state);
        test.add(txSeqA1T2,
                 TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
        test.check(state);
    }

    SECTION("good sequence number, same twice with double shift")
    {
        TransactionQueueTest test{queue};
        test.add(txSeqA1T1,
                 TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}, {}});
        test.shift();
        test.check({{{account1, 1, {txSeqA1T1}}, {account2}}, {}});
        test.shift();
        test.check({{{account1, 2, {txSeqA1T1}}, {account2}}, {}});
        test.add(txSeqA1T1,
                 TransactionQueue::AddResultCode::ADD_STATUS_DUPLICATE);
        test.check({{{account1, 2, {txSeqA1T1}}, {account2}}, {}});
    }

    SECTION("good then big sequence number, with double shift")
    {
        TransactionQueueTest test{queue};
        test.add(txSeqA1T1,
                 TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 1, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 2, {txSeqA1T1}}, {account2}}});
        auto status =
            TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER;
        test.add(txSeqA1T3, status);
        test.check({{{account1, 2, {txSeqA1T1}}, {account2}}});
    }

    SECTION("good then good sequence number, with double shift")
    {
        TransactionQueueTest test{queue};
        test.add(txSeqA1T1,
                 TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 1, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 2, {txSeqA1T1}}, {account2}}});
        test.add(txSeqA1T2,
                 TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
        test.check({{{account1, 2, {txSeqA1T1}}, {account2}}});
    }

    SECTION("good sequence number, same twice with four shifts, then two more")
    {
        TransactionQueueTest test{queue};
        test.add(txSeqA1T1,
                 TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 1, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 2, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 3, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1}, {account2}}, {{txSeqA1T1}}});
        test.add(txSeqA1T1,
                 TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
        test.check({{{account1}, {account2}}, {{txSeqA1T1}}});
        test.shift();
        test.check({{{account1}, {account2}}, {{}, {txSeqA1T1}}});
        test.shift();
        test.check({{{account1}, {account2}}});
        test.add(txSeqA1T1,
                 TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}});
    }

    SECTION("good then big sequence number, with four shifts")
    {
        TransactionQueueTest test{queue};
        test.add(txSeqA1T1,
                 TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 1, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 2, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 3, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1}, {account2}}, {{txSeqA1T1}}});
        test.add(txSeqA1T3, TransactionQueue::AddResultCode::ADD_STATUS_ERROR);
        test.check({{{account1}, {account2}}, {{txSeqA1T1}}});
    }

    SECTION("good then small sequence number, with four shifts")
    {
        TransactionQueueTest test{queue};
        test.add(txSeqA1T1,
                 TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 1, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 2, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 3, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1}, {account2}}, {{txSeqA1T1}}});
        test.add(txSeqA1T0, TransactionQueue::AddResultCode::ADD_STATUS_ERROR);
        test.check({{{account1}, {account2}}, {{txSeqA1T1}}});
    }

    SECTION("invalid transaction")
    {
        TransactionQueueTest test{queue};
        test.add(invalidTransaction(*app, account1, 1),
                 TransactionQueue::AddResultCode::ADD_STATUS_ERROR);
        test.check({{{account1}, {account2}}});
    }
}

TEST_CASE("TransactionQueue base", "[herder][transactionqueue]")
{
    testTransactionQueueBasicScenarios();
}

TEST_CASE("TransactionQueue hitting the rate limit",
          "[herder][transactionqueue]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 4;
    cfg.FLOOD_TX_PERIOD_MS = 100;
    auto app = createTestApplication(clock, cfg);
    auto queue = ClassicTransactionQueue{*app, 4, 2, 2};
    auto const minBalance2 = app->getLedgerManager().getLastMinBalance(2);

    auto root = TestAccount::createRoot(*app);
    auto account1 = root.create("a1", minBalance2);
    auto account2 = root.create("a2", minBalance2);
    auto account3 = root.create("a3", minBalance2);
    auto account4 = root.create("a4", minBalance2);
    auto account5 = root.create("a5", minBalance2);
    auto account6 = root.create("a6", minBalance2);

    TransactionQueueTest testQueue{queue};
    std::vector<TransactionFrameBasePtr> txs;
    auto addTx = [&](TransactionFrameBasePtr tx) {
        txs.push_back(tx);
        testQueue.add(tx, TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
    };
    // Fill the queue/limiter with 8 ops (2 * 4) - any further ops should result
    // in eviction (limit is 2 * 4=TESTING_UPGRADE_MAX_TX_SET_SIZE).
    addTx(transaction(*app, account1, 1, 1, 200 * 1, 1));
    addTx(transaction(*app, account2, 1, 1, 400 * 2, 2));
    addTx(transaction(*app, account3, 1, 1, 100 * 1, 1));
    addTx(transaction(*app, account4, 1, 1, 300 * 4, 4));

    SECTION("cannot add low fee tx")
    {
        auto tx = transaction(*app, account5, 1, 1, 300 * 3, 3);
        auto addResult = testQueue.add(
            tx, TransactionQueue::AddResultCode::ADD_STATUS_ERROR);
        REQUIRE(addResult.txResult->getResultCode() == txINSUFFICIENT_FEE);
        REQUIRE(addResult.txResult->getResult().feeCharged == 300 * 3 + 1);
    }
    SECTION("cannot add - tx outside of limits")
    {
        auto tx = transaction(*app, account5, 1, 1, 100 * 1'000, 100);
        auto addResult = testQueue.add(
            tx, TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
        REQUIRE(!addResult.txResult);
    }
    SECTION("add high fee tx with eviction")
    {
        auto tx = transaction(*app, account5, 1, 1, 300 * 3 + 1, 3);
        testQueue.add(tx, TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
        // Evict txs from `account1`, `account3` and `account4`
        testQueue.check(
            {{{account1}, {account2, 0, {txs[1]}}, {account5, 0, {tx}}},
             {{txs[0], txs[2], txs[3]}, {}}});

        SECTION("then cannot add tx with lower fee than evicted")
        {
            auto nextTx = transaction(*app, account6, 1, 1, 300, 1);
            auto addResult = testQueue.add(
                nextTx, TransactionQueue::AddResultCode::ADD_STATUS_ERROR);
            REQUIRE(addResult.txResult->getResultCode() == txINSUFFICIENT_FEE);
            REQUIRE(addResult.txResult->getResult().feeCharged == 301);
        }
        SECTION("then add tx with higher fee than evicted")
        {
            // The last evicted fee rate we accounted for was 200 (tx with fee
            // rate 400 is evicted due to seq num and is not accounted for).
            auto nextTx = transaction(*app, account6, 1, 1, 301, 1);
            testQueue.add(nextTx,
                          TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
            testQueue.check({{{account1},
                              {account2, 0, {txs[1]}},
                              {account3},
                              {account4},
                              {account5, 0, {tx}},
                              {account6, 0, {nextTx}}},
                             {{txs[0], txs[2], txs[3]}, {}}});
        }
    }
}

TEST_CASE("TransactionQueue with PreconditionsV2", "[herder][transactionqueue]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 4;
    cfg.FLOOD_TX_PERIOD_MS = 100;
    auto app = createTestApplication(clock, cfg);
    auto queue = ClassicTransactionQueue{*app, 4, 2, 2};
    auto const minBalance2 = app->getLedgerManager().getLastMinBalance(2);

    auto root = TestAccount::createRoot(*app);
    auto account1 = root.create("a1", minBalance2);
    auto account2 = root.create("a2", minBalance2);

    // use bumpSequence to update account1's seqLedger
    account1.bumpSequence(1);

    auto txSeqA1S1 = transaction(*app, account1, 1, 1, 200);
    auto txSeqA1S2 = transaction(*app, account1, 2, 1, 200);
    auto txSeqA1S6 = transaction(*app, account1, 6, 1, 200);

    PreconditionsV2 condMinSeqNum;
    condMinSeqNum.minSeqNum.activate() = 2;

    auto txSeqA1S5MinSeqNum =
        transactionWithV2Precondition(*app, account1, 5, 200, condMinSeqNum);

    auto txSeqA1S4MinSeqNum =
        transactionWithV2Precondition(*app, account1, 4, 200, condMinSeqNum);

    auto txSeqA1S8MinSeqNum =
        transactionWithV2Precondition(*app, account1, 8, 200, condMinSeqNum);

    PreconditionsV2 condMinSeqAge;
    condMinSeqAge.minSeqAge = 1;
    auto txSeqA1S3MinSeqAge =
        transactionWithV2Precondition(*app, account1, 3, 200, condMinSeqAge);

    PreconditionsV2 condMinSeqLedgerGap;
    condMinSeqLedgerGap.minSeqLedgerGap = 1;
    auto txSeqA1S3MinSeqLedgerGap = transactionWithV2Precondition(
        *app, account1, 3, 200, condMinSeqLedgerGap);

    SECTION("fee bump new tx with minSeqNum past lastSeq")
    {
        PreconditionsV2 cond;
        cond.minSeqNum.activate() = account1.getLastSequenceNumber() + 2;
        auto tx = transactionWithV2Precondition(*app, account1, 5, 200, cond);

        TransactionQueueTest test{queue};
        test.add(tx, TransactionQueue::AddResultCode::ADD_STATUS_ERROR);
    }
    SECTION("fee bump only existing tx")
    {
        PreconditionsV2 cond;
        cond.minSeqNum.activate() = 2;
        auto tx = transactionWithV2Precondition(*app, account1, 5, 200, cond);

        TransactionQueueTest test{queue};
        test.add(tx, TransactionQueue::AddResultCode::ADD_STATUS_PENDING);

        auto fb = feeBump(*app, account1, tx, 4000);
        test.add(fb, TransactionQueue::AddResultCode::ADD_STATUS_PENDING);

        test.check({{{account1, 0, {fb}}, {account2}}, {}});
    }
    SECTION("fee bump existing tx and add minSeqNum")
    {
        TransactionQueueTest test{queue};
        test.add(txSeqA1S1,
                 TransactionQueue::AddResultCode::ADD_STATUS_PENDING);

        PreconditionsV2 cond;
        cond.minSeqNum.activate() = 2;

        auto tx = transactionWithV2Precondition(*app, account1, 1, 200, cond);
        auto fb = feeBump(*app, account1, tx, 4000);
        test.add(fb, TransactionQueue::AddResultCode::ADD_STATUS_PENDING);

        test.check({{{account1, 0, {fb}}, {account2}}, {}});
    }
    SECTION("fee bump existing tx and remove minSeqNum")
    {
        TransactionQueueTest test{queue};

        PreconditionsV2 cond;
        cond.minSeqNum.activate() = 2;

        auto tx = transactionWithV2Precondition(*app, account1, 1, 200, cond);
        test.add(tx, TransactionQueue::AddResultCode::ADD_STATUS_PENDING);

        auto fb = feeBump(*app, account1, txSeqA1S1, 4000);
        test.add(fb, TransactionQueue::AddResultCode::ADD_STATUS_PENDING);

        test.check({{{account1, 0, {fb}}, {account2}}, {}});
    }
    SECTION("extra signer")
    {
        TransactionQueueTest test{queue};

        SignerKey a2;
        a2.type(SIGNER_KEY_TYPE_ED25519);
        a2.ed25519() = account2.getPublicKey().ed25519();

        PreconditionsV2 cond;
        cond.extraSigners.emplace_back(a2);

        SECTION("one signer")
        {
            auto tx =
                transactionWithV2Precondition(*app, account1, 1, 200, cond);
            test.add(tx, TransactionQueue::AddResultCode::ADD_STATUS_ERROR);

            tx->addSignature(account2.getSecretKey());
            test.add(tx, TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
        }

        SECTION("two signers")
        {
            SignerKey rootKey;
            rootKey.type(SIGNER_KEY_TYPE_ED25519);
            rootKey.ed25519() = root.getPublicKey().ed25519();

            cond.extraSigners.emplace_back(rootKey);
            auto tx =
                transactionWithV2Precondition(*app, account1, 1, 200, cond);

            // no signature
            test.add(tx, TransactionQueue::AddResultCode::ADD_STATUS_ERROR);

            SECTION("first signature missing")
            {
                tx->addSignature(root.getSecretKey());
                test.add(tx, TransactionQueue::AddResultCode::ADD_STATUS_ERROR);

                tx->addSignature(account2.getSecretKey());
                test.add(tx,
                         TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
            }

            SECTION("second signature missing")
            {
                tx->addSignature(account2.getSecretKey());
                test.add(tx, TransactionQueue::AddResultCode::ADD_STATUS_ERROR);

                tx->addSignature(root.getSecretKey());
                test.add(tx,
                         TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
            }
        }
    }
    SECTION("remove invalid ledger bound after close")
    {
        auto lclNum = app->getLedgerManager().getLastClosedLedgerNum();
        LedgerBounds bounds;
        bounds.minLedger = 0;
        bounds.maxLedger = lclNum + 2;

        PreconditionsV2 cond;
        cond.ledgerBounds.activate() = bounds;

        auto tx = transactionWithV2Precondition(*app, account1, 1, 200, cond);

        auto& herder = static_cast<HerderImpl&>(app->getHerder());
        auto& tq = herder.getTransactionQueue();

        REQUIRE(herder.recvTransaction(tx, false).code ==
                TransactionQueue::AddResultCode::ADD_STATUS_PENDING);

        REQUIRE(tq.getTransactions({}).size() == 1);
        closeLedger(*app);
        REQUIRE(tq.getTransactions({}).size() == 0);
        REQUIRE(tq.isBanned(tx->getFullHash()));
    }
    SECTION("gap valid due to minSeqNum")
    {
        TransactionQueueTest test{queue};
        // Ledger state is at seqnum 1
        closeLedger(*app, {txSeqA1S1});

        {
            // Try tx with a minSeqNum that's not low enough
            PreconditionsV2 cond;
            cond.minSeqNum.activate() = account1.getLastSequenceNumber() + 2;
            auto tx =
                transactionWithV2Precondition(*app, account1, 5, 200, cond);

            test.add(tx, TransactionQueue::AddResultCode::ADD_STATUS_ERROR);
        }

        test.add(txSeqA1S5MinSeqNum,
                 TransactionQueue::AddResultCode::ADD_STATUS_PENDING);

        // make sure duplicates are identified correctly
        test.add(txSeqA1S5MinSeqNum,
                 TransactionQueue::AddResultCode::ADD_STATUS_DUPLICATE);

        // try to fill in gap with a tx
        test.add(txSeqA1S2, TransactionQueue::AddResultCode::ADD_STATUS_ERROR);

        // try to fill in gap with a minSeqNum tx
        test.add(txSeqA1S4MinSeqNum,
                 TransactionQueue::AddResultCode::ADD_STATUS_ERROR);

        test.check({{{account1, 0, {txSeqA1S5MinSeqNum}}, {account2}}, {}});

        // fee bump the existing minSeqNum tx
        auto fb = feeBump(*app, account1, txSeqA1S5MinSeqNum, 4000);
        test.add(fb, TransactionQueue::AddResultCode::ADD_STATUS_PENDING);

        test.check({{{account1, 0, {fb}}, {account2}}, {}});

        // fee bump a new minSeqNum tx fails due to account limit
        auto fb2 = feeBump(*app, account1, txSeqA1S8MinSeqNum, 400);
        test.add(fb2,
                 TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);

        test.check({{{account1, 0, {fb}}, {account2}}, {}});
    }
}

class SorobanLimitingLaneConfigForTesting : public SurgePricingLaneConfig
{
  public:
    // Index of the DEX limited lane.
    static constexpr size_t LARGE_SOROBAN_LANE = 1;

    SorobanLimitingLaneConfigForTesting(Resource sorobanGenericLimit,
                                        std::optional<Resource> sorobanLimit)
    {
        mLaneOpsLimits.push_back(sorobanGenericLimit);
        if (sorobanLimit)
        {
            mLaneOpsLimits.push_back(*sorobanLimit);
        }
    }

    size_t
    getLane(TransactionFrameBase const& tx) const override
    {
        bool limitedLane = tx.getEnvelope().v1().tx.memo.type() == MEMO_TEXT &&
                           tx.getEnvelope().v1().tx.memo.text() == "limit";
        if (mLaneOpsLimits.size() >
                SorobanLimitingLaneConfigForTesting::LARGE_SOROBAN_LANE &&
            limitedLane)
        {
            return SorobanLimitingLaneConfigForTesting::LARGE_SOROBAN_LANE;
        }
        else
        {
            return SurgePricingPriorityQueue::GENERIC_LANE;
        }
    }
    std::vector<Resource> const&
    getLaneLimits() const override
    {
        return mLaneOpsLimits;
    }
    virtual void
    updateGenericLaneLimit(Resource const& limit) override
    {
        mLaneOpsLimits[0] = limit;
    }
    virtual Resource
    getTxResources(TransactionFrameBase const& tx) override
    {
        releaseAssert(tx.isSoroban());
        return tx.getResources(false);
    }

  private:
    std::vector<Resource> mLaneOpsLimits;
};

TEST_CASE("Soroban TransactionQueue pre-protocol-20",
          "[soroban][transactionqueue]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.TESTING_UPGRADE_LEDGER_PROTOCOL_VERSION =
        static_cast<uint32_t>(SOROBAN_PROTOCOL_VERSION) - 1;

    auto app = createTestApplication(clock, cfg);
    auto root = TestAccount::createRoot(*app);

    SorobanResources resources;
    resources.instructions = 2'000'000;
    resources.readBytes = 2000;
    resources.writeBytes = 1000;

    auto tx = createUploadWasmTx(*app, root, 10'000'000,
                                 DEFAULT_TEST_RESOURCE_FEE, resources);

    // Soroban tx is not supported
    REQUIRE(app->getHerder().recvTransaction(tx, false).code ==
            TransactionQueue::AddResultCode::ADD_STATUS_ERROR);
    REQUIRE(app->getHerder().getTx(tx->getFullHash()) == nullptr);
}

TEST_CASE("Soroban tx and memos", "[soroban][transactionqueue]")
{
    Config cfg = getTestConfig();
    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);

    const int64_t startingBalance =
        app->getLedgerManager().getLastMinBalance(50);

    auto root = TestAccount::createRoot(*app);
    auto a1 = root.create("A", startingBalance);

    auto wasm = rust_bridge::get_test_wasm_add_i32();
    auto resources = defaultUploadWasmResourcesWithoutFootprint(
        wasm, getLclProtocolVersion(*app));
    resources.instructions = 0;

    Operation uploadOp;
    uploadOp.body.type(INVOKE_HOST_FUNCTION);
    auto& uploadHF = uploadOp.body.invokeHostFunctionOp().hostFunction;
    uploadHF.type(HOST_FUNCTION_TYPE_UPLOAD_CONTRACT_WASM);
    uploadHF.wasm().assign(wasm.data.begin(), wasm.data.end());

    SorobanAuthorizationEntry sae;
    SorobanCredentials sc(SOROBAN_CREDENTIALS_ADDRESS);
    sae.credentials = sc;

    if (resources.footprint.readWrite.empty())
    {
        resources.footprint.readWrite = {
            contractCodeKey(sha256(uploadHF.wasm()))};
    }
    auto uploadResourceFee =
        sorobanResourceFee(*app, resources, 1000 + wasm.data.size(), 40) +
        DEFAULT_TEST_RESOURCE_FEE;

    SECTION("source auth with memo")
    {
        auto txWithMemo = sorobanTransactionFrameFromOpsWithTotalFee(
            app->getNetworkID(), a1, {uploadOp}, {}, resources,
            uploadResourceFee + 100, uploadResourceFee, "memo");

        REQUIRE(app->getHerder().recvTransaction(txWithMemo, false).code ==
                TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
    }

    SECTION("non-source auth tx with memo")
    {
        SorobanAuthorizationEntry sae2;
        SorobanCredentials sourcec(SOROBAN_CREDENTIALS_SOURCE_ACCOUNT);
        sae2.credentials = sourcec;

        uploadOp.body.invokeHostFunctionOp().auth.emplace_back(sae2);
        uploadOp.body.invokeHostFunctionOp().auth.emplace_back(sae);

        auto txWithMemo = sorobanTransactionFrameFromOpsWithTotalFee(
            app->getNetworkID(), a1, {uploadOp}, {}, resources,
            uploadResourceFee + 100, uploadResourceFee, "memo");

        REQUIRE(app->getHerder().recvTransaction(txWithMemo, false).code ==
                TransactionQueue::AddResultCode::ADD_STATUS_ERROR);
    }

    SECTION("non-source auth tx with muxed tx source")
    {
        uploadOp.body.invokeHostFunctionOp().auth.emplace_back(sae);

        auto txWithMuxedTxSource = sorobanTransactionFrameFromOpsWithTotalFee(
            app->getNetworkID(), a1, {uploadOp}, {}, resources,
            uploadResourceFee + 100, uploadResourceFee, std::nullopt,
            1 /*muxedData*/);

        REQUIRE(
            app->getHerder().recvTransaction(txWithMuxedTxSource, false).code ==
            TransactionQueue::AddResultCode::ADD_STATUS_ERROR);
    }

    SECTION("non-source auth tx with muxed op source")
    {
        uploadOp.body.invokeHostFunctionOp().auth.emplace_back(sae);

        MuxedAccount muxedAccount(CryptoKeyType::KEY_TYPE_MUXED_ED25519);
        muxedAccount.med25519().ed25519 = a1.getPublicKey().ed25519();
        muxedAccount.med25519().id = 1;
        uploadOp.sourceAccount.activate() = muxedAccount;
        auto txWithMuxedOpSource = sorobanTransactionFrameFromOpsWithTotalFee(
            app->getNetworkID(), a1, {uploadOp}, {}, resources,
            uploadResourceFee + 100, uploadResourceFee);

        REQUIRE(
            app->getHerder().recvTransaction(txWithMuxedOpSource, false).code ==
            TransactionQueue::AddResultCode::ADD_STATUS_ERROR);
    }
}

TEST_CASE("Soroban TransactionQueue limits",
          "[herder][transactionqueue][soroban]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 4;

    cfg.FLOOD_TX_PERIOD_MS = 100;
    auto app = createTestApplication(clock, cfg);
    overrideSorobanNetworkConfigForTest(*app);
    modifySorobanNetworkConfig(*app, [](SorobanNetworkConfig& cfg) {
        cfg.mLedgerMaxTxCount = 4;
        // Restrict instructions to only allow 1 max instructions tx.
        cfg.mLedgerMaxInstructions = cfg.mTxMaxInstructions;
    });
    auto const minBalance2 = app->getLedgerManager().getLastMinBalance(2);
    auto root = TestAccount::createRoot(*app);
    auto account1 = root.create("a1", minBalance2);
    auto account2 = root.create("a2", minBalance2);

    SorobanNetworkConfig conf =
        app->getLedgerManager().getSorobanNetworkConfigReadOnly();

    SorobanResources resources;
    resources.instructions = 2'000'000;
    resources.readBytes = 2000;
    resources.writeBytes = 1000;

    int const resourceFee = 2'000'000;
    int initialInclusionFee = 100'000;

    auto resAdjusted = resources;
    resAdjusted.instructions = static_cast<uint32>(conf.txMaxInstructions());

    auto tx = createUploadWasmTx(*app, root, initialInclusionFee, resourceFee,
                                 resAdjusted);

    REQUIRE(app->getHerder().recvTransaction(tx, false).code ==
            TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
    REQUIRE(app->getHerder().getTx(tx->getFullHash()) != nullptr);

    SECTION("malformed tx")
    {
        TransactionFrameBasePtr badTx;
        SECTION("missing extension")
        {
            Operation op0;
            op0.body.type(INVOKE_HOST_FUNCTION);
            auto& ihf0 = op0.body.invokeHostFunctionOp().hostFunction;
            ihf0.type(HOST_FUNCTION_TYPE_CREATE_CONTRACT);

            badTx =
                transactionFrameFromOps(app->getNetworkID(), root, {op0}, {});
        }
        SECTION("negative inclusion fee")
        {
            badTx =
                feeBump(*app, root, tx, tx->declaredSorobanResourceFee() - 1,
                        /* useInclusionAsFullFee */ true);

            REQUIRE(badTx->getFullFee() < badTx->declaredSorobanResourceFee());
        }
        SECTION("negative fee-bump full fee")
        {
            int64_t fee = 0;
            SECTION("basic")
            {
                fee = -1;
            }
            SECTION("int64 limit")
            {
                fee = INT64_MIN;
            }
            badTx = feeBump(*app, root, tx, fee,
                            /* useInclusionAsFullFee */ true);

            REQUIRE(badTx->getFullFee() < 0);
        }
        SECTION("negative declared resource fee")
        {
            int64_t resFee = 0;
            SECTION("basic")
            {
                resFee = -1;
            }
            SECTION("int64 limit")
            {
                resFee = INT64_MIN;
            }
            auto wasmTx = createUploadWasmTx(*app, account1, 0, 0, resAdjusted);
            txbridge::setSorobanFees(wasmTx, UINT32_MAX, resFee);
            txbridge::getSignatures(wasmTx).clear();
            wasmTx->addSignature(account1.getSecretKey());
            badTx = wasmTx;
            REQUIRE_THROWS_AS(badTx->getInclusionFee(), std::runtime_error);
        }
        // Gracefully handle malformed transaction
        auto addResult = app->getHerder().recvTransaction(badTx, false);
        REQUIRE(addResult.code ==
                TransactionQueue::AddResultCode::ADD_STATUS_ERROR);
        REQUIRE(addResult.txResult);
        REQUIRE(addResult.txResult->getResultCode() == txMALFORMED);
    }
    SECTION("source account limit, soroban and classic tx queue")
    {
        auto classic = transaction(*app, account1, 1, 100, 100);
        auto soroban = createUploadWasmTx(*app, account1, initialInclusionFee,
                                          resourceFee, resAdjusted);

        auto checkLimitEnforced = [&](auto const& pendingTx,
                                      auto const& rejectedTx,
                                      TransactionQueue& txQueue) {
            REQUIRE(app->getHerder().recvTransaction(pendingTx, false).code ==
                    TransactionQueue::AddResultCode::ADD_STATUS_PENDING);

            // Can't submit tx due to source account limit
            REQUIRE(
                app->getHerder().recvTransaction(rejectedTx, false).code ==
                TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);

            // ban existing tx
            txQueue.ban({pendingTx});
            REQUIRE(app->getHerder().getTx(pendingTx->getFullHash()) ==
                    nullptr);
            REQUIRE(app->getHerder().isBannedTx(pendingTx->getFullHash()));

            // Now can submit classic txs
            REQUIRE(app->getHerder().recvTransaction(rejectedTx, false).code ==
                    TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
        };
        SECTION("classic is rejected when soroban is pending")
        {
            auto& queue = app->getHerder().getSorobanTransactionQueue();
            SECTION("no fee bump")
            {
                checkLimitEnforced(soroban, classic, queue);
            }
            SECTION("fee bump")
            {
                SECTION("fee bump soroban")
                {
                    auto fb = feeBump(*app, account1, soroban,
                                      initialInclusionFee * 10);
                    checkLimitEnforced(fb, classic, queue);
                }
                SECTION("classic fee bump")
                {
                    auto fb = feeBump(*app, account1, classic, 100 * 10);
                    checkLimitEnforced(soroban, fb, queue);
                }
            }
        }
        SECTION("soroban is rejected when classic is pending")
        {
            auto& queue = app->getHerder().getTransactionQueue();
            SECTION("no fee bump")
            {
                checkLimitEnforced(classic, soroban, queue);
            }
            SECTION("fee bump")
            {
                SECTION("fee bump soroban")
                {
                    auto fb = feeBump(*app, account1, soroban,
                                      initialInclusionFee * 10);
                    checkLimitEnforced(classic, fb, queue);
                }
                SECTION("classic fee bump")
                {
                    auto fb = feeBump(*app, account1, classic, 100 * 10);
                    checkLimitEnforced(fb, soroban, queue);
                }
            }
        }
    }
    SECTION("tx does not fit")
    {
        SECTION("reject")
        {
            // New Soroban tx fits within limits, but now there's no space
            auto txNew = createUploadWasmTx(*app, account1, initialInclusionFee,
                                            resourceFee, resources);

            REQUIRE(app->getHerder().recvTransaction(txNew, false).code ==
                    TransactionQueue::AddResultCode::ADD_STATUS_PENDING);

            SECTION("insufficient fee")
            {
                TransactionFrameBasePtr tx2;
                auto innerTx =
                    createUploadWasmTx(*app, account2, initialInclusionFee,
                                       resourceFee, resAdjusted);
                auto expectedFeeCharged = 0;
                SECTION("regular tx")
                {
                    tx2 = innerTx;
                    expectedFeeCharged = initialInclusionFee + resourceFee + 1;
                }
                SECTION("fee-bump")
                {
                    tx2 = feeBump(*app, account2, innerTx,
                                  initialInclusionFee * 2);
                    expectedFeeCharged =
                        initialInclusionFee * 2 + resourceFee + 1;
                }

                // Same per-op fee, no eviction
                auto addResult = app->getHerder().recvTransaction(tx2, false);
                REQUIRE(addResult.code ==
                        TransactionQueue::AddResultCode::ADD_STATUS_ERROR);
                REQUIRE(!app->getHerder().isBannedTx(tx->getFullHash()));

                REQUIRE(addResult.txResult);
                REQUIRE(addResult.txResult->getResultCode() ==
                        TransactionResultCode::txINSUFFICIENT_FEE);
                REQUIRE(addResult.txResult->getResult().feeCharged ==
                        expectedFeeCharged);
            }
            SECTION("insufficient account balance")
            {
                // Not enough balance to cover full fee
                auto tx2 =
                    createUploadWasmTx(*app, account2, initialInclusionFee,
                                       minBalance2, resources);

                auto addResult = app->getHerder().recvTransaction(tx2, false);
                REQUIRE(addResult.code ==
                        TransactionQueue::AddResultCode::ADD_STATUS_ERROR);
                REQUIRE(!app->getHerder().isBannedTx(tx->getFullHash()));
                REQUIRE(addResult.txResult);
                REQUIRE(addResult.txResult->getResultCode() ==
                        TransactionResultCode::txINSUFFICIENT_BALANCE);
            }
            SECTION("invalid resources")
            {
                // Instruction count over max
                resources.instructions =
                    static_cast<uint32>(conf.txMaxInstructions() + 1);

                TransactionFrameBasePtr tx2;
                SECTION("different source account")
                {
                    // Double the fee
                    tx2 = createUploadWasmTx(*app, account2,
                                             initialInclusionFee * 2,
                                             resourceFee, resources);
                }
                SECTION("invalid resources kick in before source account limit")
                {
                    tx2 =
                        createUploadWasmTx(*app, account1, initialInclusionFee,
                                           resourceFee, resources);
                }

                auto addResult = app->getHerder().recvTransaction(tx2, false);
                REQUIRE(addResult.code ==
                        TransactionQueue::AddResultCode::ADD_STATUS_ERROR);
                REQUIRE(!app->getHerder().isBannedTx(tx->getFullHash()));
                REQUIRE(addResult.txResult);
                REQUIRE(addResult.txResult->getResultCode() ==
                        TransactionResultCode::txSOROBAN_INVALID);
            }
            SECTION("too many ops")
            {
                auto tx2 = createUploadWasmTx(
                    *app, account2, initialInclusionFee * 2, resourceFee,
                    resources, std::nullopt, /* addInvalidOps */ 1);

                auto addResult = app->getHerder().recvTransaction(tx2, false);
                REQUIRE(addResult.code ==
                        TransactionQueue::AddResultCode::ADD_STATUS_ERROR);
                REQUIRE(!app->getHerder().isBannedTx(tx->getFullHash()));
                REQUIRE(addResult.txResult);
                REQUIRE(addResult.txResult->getResultCode() ==
                        TransactionResultCode::txMALFORMED);
            }
        }
        SECTION("accept but evict first tx")
        {
            // Add two more txs that will cause instructions to go over limit;
            // evict the first tx (lowest fee)
            resources.instructions =
                static_cast<uint32>(conf.txMaxInstructions());

            auto tx2 =
                createUploadWasmTx(*app, account1, initialInclusionFee + 1,
                                   resourceFee, resources);

            TransactionFrameBasePtr tx3;
            auto innerTx =
                createUploadWasmTx(*app, account2, initialInclusionFee + 1,
                                   resourceFee, resources);
            SECTION("regular tx")
            {
                tx3 = innerTx;
            }
            SECTION("fee-bump")
            {
                // Fee bump must pay double inclusion fee
                tx3 = feeBump(*app, account2, innerTx,
                              2 * (initialInclusionFee + 1));
            }

            auto res = app->getHerder().recvTransaction(tx2, false);
            REQUIRE(res.code ==
                    TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
            auto res2 = app->getHerder().recvTransaction(tx3, false);
            REQUIRE(res2.code ==
                    TransactionQueue::AddResultCode::ADD_STATUS_PENDING);

            // Evicted and banned the first tx
            REQUIRE(app->getHerder().getTx(tx->getFullHash()) == nullptr);
            REQUIRE(app->getHerder().isBannedTx(tx->getFullHash()));
            // Second tx is still in the queue
            REQUIRE(app->getHerder().getTx(tx2->getFullHash()) != nullptr);
        }
    }
    SECTION("limited lane eviction")
    {
        Resource limits = app->getLedgerManager().maxLedgerResources(true);

        // Setup limits: generic fits 1 ledger worth of resources, while limited
        // lane fits 1/4 ledger
        Resource limitedLane(
            bigDivideOrThrow(limits, 1, 4, Rounding::ROUND_UP));
        auto config = std::make_shared<SorobanLimitingLaneConfigForTesting>(
            limits, limitedLane);
        auto queue = std::make_unique<SurgePricingPriorityQueue>(
            /* isHighestPriority */ false, config, 1);

        std::vector<std::pair<TransactionFrameBasePtr, bool>> toEvict;

        // Generic tx, takes 1/2 of instruction limits
        resources.instructions =
            static_cast<uint32>(conf.ledgerMaxInstructions() / 2);
        tx = createUploadWasmTx(*app, root, initialInclusionFee, resourceFee,
                                resources);

        SECTION("generic fits")
        {
            REQUIRE(
                queue->canFitWithEviction(*tx, std::nullopt, toEvict).first);
            REQUIRE(toEvict.empty());
        }
        SECTION("limited too big")
        {
            // Fits into generic, but doesn't fit into limited
            resources.instructions =
                static_cast<uint32>(conf.txMaxInstructions() / 2);
            auto tx2 = createUploadWasmTx(
                *app, account1, initialInclusionFee, resourceFee, resources,
                std::make_optional<std::string>("limit"));

            REQUIRE(config->getLane(*tx2) ==
                    SorobanLimitingLaneConfigForTesting::LARGE_SOROBAN_LANE);

            REQUIRE(
                !queue->canFitWithEviction(*tx2, std::nullopt, toEvict).first);
            REQUIRE(toEvict.empty());
        }
        SECTION("limited fits")
        {
            // Fits into limited
            resources.instructions =
                static_cast<uint32>(conf.txMaxInstructions() / 8);
            auto txNew = createUploadWasmTx(
                *app, account1, initialInclusionFee * 2, resourceFee, resources,
                std::make_optional<std::string>("limit"));

            REQUIRE(config->getLane(*txNew) ==
                    SorobanLimitingLaneConfigForTesting::LARGE_SOROBAN_LANE);

            REQUIRE(
                queue->canFitWithEviction(*txNew, std::nullopt, toEvict).first);
            REQUIRE(toEvict.empty());

            SECTION("limited evicts")
            {
                // Add 2 generic transactions to reach generic limit
                queue->add(tx);
                resources.instructions =
                    static_cast<uint32>(conf.ledgerMaxInstructions() / 2);
                // The fee is slightly higher so this transactions is more
                // favorable during evictions
                auto secondGeneric =
                    createUploadWasmTx(*app, account2, initialInclusionFee + 10,
                                       resourceFee, resources);

                REQUIRE(queue
                            ->canFitWithEviction(*secondGeneric, std::nullopt,
                                                 toEvict)
                            .first);
                REQUIRE(toEvict.empty());
                queue->add(secondGeneric);

                SECTION("limited evicts generic")
                {
                    // Fit within limited lane
                    REQUIRE(
                        queue->canFitWithEviction(*txNew, std::nullopt, toEvict)
                            .first);
                    REQUIRE(toEvict.size() == 1);
                    REQUIRE(toEvict[0].first == tx);
                }
                SECTION("evict due to lane limit")
                {
                    // Add another limited tx, so that generic and limited are
                    // both at max
                    resources.writeBytes = conf.txMaxWriteBytes() / 4;
                    resources.instructions = 0;
                    auto tx2 = createUploadWasmTx(
                        *app, account1, initialInclusionFee * 2, resourceFee,
                        resources, std::make_optional<std::string>("limit"));

                    REQUIRE(
                        queue->canFitWithEviction(*tx2, std::nullopt, toEvict)
                            .first);
                    queue->add(tx2);

                    // Add, new tx with max limited lane resources, set a high
                    // fee
                    resources.instructions =
                        static_cast<uint32>(conf.txMaxInstructions() / 4);
                    resources.instructions =
                        static_cast<uint32>(conf.txMaxWriteBytes() / 4);
                    auto tx3 = createUploadWasmTx(
                        *app, account2, initialInclusionFee * 3, resourceFee,
                        resources, std::make_optional<std::string>("limit"));

                    REQUIRE(
                        queue->canFitWithEviction(*tx3, std::nullopt, toEvict)
                            .first);

                    // Should evict generic _and_ limited tx
                    REQUIRE(toEvict.size() == 2);
                    REQUIRE(toEvict[0].first == tx);
                    REQUIRE(!toEvict[0].second);
                    REQUIRE(toEvict[1].first == tx2);
                    REQUIRE(toEvict[1].second);
                }
            }
        }
    }
}

TEST_CASE("TransactionQueue limits", "[herder][transactionqueue]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 6;
    cfg.FLOOD_TX_PERIOD_MS = 100;
    auto app = createTestApplication(clock, cfg);
    auto const minBalance2 = app->getLedgerManager().getLastMinBalance(2);

    auto root = TestAccount::createRoot(*app);
    auto account1 = root.create("a1", minBalance2);
    auto account2 = root.create("a2", minBalance2);
    auto account3 = root.create("a3", minBalance2);
    auto account4 = root.create("a4", minBalance2);
    auto account5 = root.create("a5", minBalance2);
    auto account6 = root.create("a6", minBalance2);
    auto account7 = root.create("a7", minBalance2);

    TxQueueLimiter limiter(1, *app, false);

    struct SetupElement
    {
        TestAccount& account;
        SequenceNumber startSeq;
        std::vector<std::pair<int, int>> opsFeeVec;
    };
    std::vector<TransactionFrameBasePtr> txs;

    TransactionFrameBasePtr noTx;

    auto setup = [&](std::vector<SetupElement> elems) {
        for (auto& e : elems)
        {
            SequenceNumber seq = e.startSeq;
            for (auto opsFee : e.opsFeeVec)
            {
                auto tx = transaction(*app, e.account, seq++, 1, opsFee.second,
                                      opsFee.first);
                std::vector<std::pair<TransactionFrameBasePtr, bool>>
                    txsToEvict;
                bool can = limiter.canAddTx(tx, noTx, txsToEvict).first;
                REQUIRE(can);
                REQUIRE(txsToEvict.empty());
                limiter.addTransaction(tx);
                txs.emplace_back(tx);
            }
        }
        REQUIRE(limiter.size() == 5);
    };
    // act \ base fee   400 300 200  100
    //  1                1   0    0   0
    //  2                1   0    0   0
    //  3                0   1    1   0
    //  4                0   0    1   0
    //  5                0   0    0   1
    // total             2   1    1   1 --> 5 (free = 1)
    setup({{account1, 1, {{1, 400}}},
           {account2, 1, {{1, 400}}},
           {account3, 1, {{1, 300}}},
           {account4, 1, {{1, 200}}},
           {account5, 1, {{1, 100}}}});
    auto checkAndAddTx = [&](bool expected, TestAccount& account, int ops,
                             int fee, int64 expFeeOnFailed,
                             int expEvictedOpsOnSuccess) {
        auto tx = transaction(*app, account, 1000, 1, fee, ops);
        std::vector<std::pair<TransactionFrameBasePtr, bool>> txsToEvict;
        auto can = limiter.canAddTx(tx, noTx, txsToEvict);
        REQUIRE(expected == can.first);
        if (can.first)
        {
            int evictedOps = 0;
            limiter.evictTransactions(
                txsToEvict, *tx, [&](TransactionFrameBasePtr const& evict) {
                    // can't evict cheaper transactions
                    auto cmp3 = feeRate3WayCompare(
                        evict->getInclusionFee(), evict->getNumOperations(),
                        tx->getInclusionFee(), tx->getNumOperations());
                    REQUIRE(cmp3 < 0);
                    // can't evict self
                    bool same = evict->getSourceID() == tx->getSourceID();
                    REQUIRE(!same);
                    evictedOps += evict->getNumOperations();
                    limiter.removeTransaction(evict);
                });
            REQUIRE(evictedOps == expEvictedOpsOnSuccess);
            limiter.addTransaction(tx);
            limiter.removeTransaction(tx);
        }
        else
        {
            REQUIRE(can.second == expFeeOnFailed);
        }
    };
    // check that `account`
    // can add ops operations,
    // but not add ops+1 at the same basefee
    // that would require evicting a transaction with basefee
    auto checkTxBoundary = [&](TestAccount& account, int ops, int bfee,
                               int expEvicted) {
        auto txFee1 = bfee * (ops + 1);
        checkAndAddTx(false, account, ops + 1, txFee1, txFee1 + 1, 0);
        checkAndAddTx(true, account, ops, bfee * ops, 0, expEvicted);
    };

    // Check that 1 operation transaction with `minFee` cannot be added to
    // the limiter, but with `minFee + 1` can be added. Use for checking
    // that fee threshold is applied even when there is enough space in
    // the limiter, but some transactions were evicted before.
    auto checkMinFeeToFitWithNoEvict = [&](uint32_t minFee) {
        std::vector<std::pair<TransactionFrameBasePtr, bool>> txsToEvict;
        // 0 fee is a special case as transaction shouldn't have 0 fee.
        // Hence we only check that fee of 1 allows transaction to be added.
        if (minFee == 0)
        {
            REQUIRE(limiter
                        .canAddTx(transaction(*app, account1, 1000, 1, 1), noTx,
                                  txsToEvict)
                        .first);
            return;
        }
        auto feeTx = transaction(*app, account1, 1000, 1, minFee);
        auto [canAdd, feeNeeded] = limiter.canAddTx(feeTx, noTx, txsToEvict);
        REQUIRE(canAdd == false);
        REQUIRE(feeNeeded == minFee + 1);

        auto increasedFeeTx = transaction(*app, account1, 1000, 1, minFee + 1);
        REQUIRE(limiter.canAddTx(increasedFeeTx, noTx, txsToEvict).first);
    };

    SECTION("evict nothing")
    {
        checkTxBoundary(account6, 1, 100, 0);
        // can't evict transaction with the same base fee
        checkAndAddTx(false, account7, 2, 100 * 2, 2 * 100 + 1, 0);
    }
    SECTION("evict 100s")
    {
        checkTxBoundary(account6, 2, 200, 1);
    }
    SECTION("evict 100s and 200s")
    {
        checkTxBoundary(account6, 3, 300, 2);
        checkMinFeeToFitWithNoEvict(200);
        SECTION("and add empty tx")
        {
            // Empty transaction can be added from the limiter standpoint
            // (as it contains 0 ops and cannot exceed the operation limits)
            // and hence should be rejected by the validation logic.
            checkAndAddTx(true, account7, 0, 100, 0, 0);
        }
    }
    SECTION("evict 100s and 200s, can't evict self")
    {
        checkAndAddTx(false, account4, 3, 3 * 300, 0, 0);
    }
    SECTION("evict all")
    {
        checkAndAddTx(true, account6, 6, 6 * 500, 0, 5);
        checkMinFeeToFitWithNoEvict(400);
        limiter.resetEvictionState();
        checkMinFeeToFitWithNoEvict(0);
    }
    SECTION("enforce limit")
    {
        checkMinFeeToFitWithNoEvict(0);
        checkAndAddTx(true, account6, 2, 2 * 200, 0, 1);
        // at this point as a transaction of base fee 100 was evicted
        // no transactions of base fee 100 can be accepted
        checkMinFeeToFitWithNoEvict(100);
        // evict some more (300s)
        checkAndAddTx(true, account7, 4, 300 * 4 + 1, 0, 2);
        checkMinFeeToFitWithNoEvict(300);

        // now, reset the min fee requirement
        limiter.resetEvictionState();
        checkMinFeeToFitWithNoEvict(0);
    }
}

TEST_CASE("TransactionQueue limiter with DEX separation",
          "[herder][transactionqueue]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 3;
    cfg.FLOOD_TX_PERIOD_MS = 100;
    cfg.MAX_DEX_TX_OPERATIONS_IN_TX_SET = 1;
    auto app = createTestApplication(clock, cfg);
    auto const minBalance2 = app->getLedgerManager().getLastMinBalance(2);

    auto root = TestAccount::createRoot(*app);
    auto account1 = root.create("a1", minBalance2);
    auto account2 = root.create("a2", minBalance2);
    auto account3 = root.create("a3", minBalance2);
    auto account4 = root.create("a4", minBalance2);
    auto account5 = root.create("a5", minBalance2);
    auto account6 = root.create("a6", minBalance2);

    // 3 * 3 = 9 operations limit, 3 * 1 = 3 DEX operations limit.
    TxQueueLimiter limiter(3, *app, false);

    std::vector<TransactionFrameBasePtr> txs;

    TransactionFrameBasePtr noTx;

    auto checkAndAddTx = [&](TestAccount& account, bool isDex, int ops,
                             uint32 fee, bool expected, int64 expFeeOnFailed,
                             int expEvictedOpsOnSuccess) {
        TransactionFrameBasePtr tx;
        if (isDex)
        {
            tx = createSimpleDexTx(*app, account, ops, fee);
        }
        else
        {
            tx = transaction(*app, account, 1, 1, fee, ops);
        }
        std::vector<std::pair<TransactionFrameBasePtr, bool>> txsToEvict;
        auto can = limiter.canAddTx(tx, noTx, txsToEvict);
        REQUIRE(can.first == expected);
        if (can.first)
        {
            int evictedOps = 0;
            limiter.evictTransactions(
                txsToEvict, *tx, [&](TransactionFrameBasePtr const& evict) {
                    // can't evict cheaper transactions (
                    // evict.bid/evict.ops < tx->bid/tx->ops)
                    REQUIRE(bigMultiply(evict->getInclusionFee(),
                                        tx->getNumOperations()) <
                            bigMultiply(tx->getInclusionFee(),
                                        evict->getNumOperations()));
                    // can't evict self
                    bool same = evict->getSourceID() == tx->getSourceID();
                    REQUIRE(!same);
                    evictedOps += evict->getNumOperations();
                    limiter.removeTransaction(evict);
                });
            REQUIRE(evictedOps == expEvictedOpsOnSuccess);
            limiter.addTransaction(tx);
        }
        else
        {
            REQUIRE(can.second == expFeeOnFailed);
        }
    };

    auto checkAndAddWithIncreasedBid = [&](TestAccount& account, bool isDex,
                                           uint32 ops, int opBid,
                                           int expectedEvicted) {
        checkAndAddTx(account, isDex, ops, ops * opBid, false, opBid * ops + 1,
                      0);
        checkAndAddTx(account, isDex, ops, ops * opBid + 1, true, 0,
                      expectedEvicted);
    };

    SECTION("non-DEX transactions only")
    {
        // Fill capacity of 9 ops
        checkAndAddTx(account1, false, 1, 100, true, 0, 0);
        checkAndAddTx(account2, false, 5, 300 * 5, true, 0, 0);
        checkAndAddTx(account3, false, 1, 400, true, 0, 0);
        checkAndAddTx(account4, false, 2, 200 * 2, true, 0, 0);

        // Cannot add transactions anymore without eviction.
        checkAndAddTx(account5, false, 1, 100, false, 101, 0);
        // Evict transactions with high enough bid.
        checkAndAddTx(account5, false, 2, 2 * 200 + 1, true, 0, 3);
    }
    SECTION("DEX transactions only")
    {
        // Fill DEX capacity of 3 ops
        checkAndAddTx(account1, true, 1, 100, true, 0, 0);
        checkAndAddTx(account2, true, 2, 200, true, 0, 0);

        // Cannot add DEX transactions anymore without eviction.
        checkAndAddTx(account3, true, 1, 100, false, 101, 0);
        // Evict DEX transactions with high enough bid.
        checkAndAddTx(account3, true, 3, 3 * 200 + 1, true, 0, 3);
    }
    SECTION("DEX and non-DEX transactions")
    {
        // 3 DEX ops (bid 200)
        checkAndAddTx(account1, true, 3, 200 * 3, true, 0, 0);

        // 1 non-DEX op (bid 100) - fits
        checkAndAddTx(account2, false, 1, 100, true, 0, 0);
        // 1 DEX op (bid 100) - doesn't fit
        checkAndAddTx(account2, true, 1, 100, false, 201, 0);

        // 7 non-DEX ops (bid 200/op + 1) - evict all DEX and non-DEX txs.
        checkAndAddTx(account3, false, 7, 200 * 7 + 1, true, 0, 4);

        // 1 DEX op - while it fits, 200 bid is not enough (as we evicted tx
        // with 200 DEX bid).
        checkAndAddWithIncreasedBid(account4, true, 1, 200, 0);

        // 1 non-DEX op - while it fits, 200 bid is not enough (as we evicted
        // DEX tx with 200 bid before reaching the DEX ops limit).
        checkAndAddWithIncreasedBid(account5, false, 1, 200, 0);
    }

    SECTION("DEX and non-DEX transactions with DEX limit reached")
    {
        // 2 DEX ops (bid 200/op)
        checkAndAddTx(account1, true, 2, 200 * 2, true, 0, 0);

        // 3 non-DEX ops (bid 100/op) - fits
        checkAndAddTx(account2, false, 3, 100 * 3, true, 0, 0);
        // 2 DEX ops (bid 300/op) - fits and evicts the previous DEX tx
        checkAndAddTx(account3, true, 2, 300 * 2, true, 0, 2);

        // 5 non-DEX ops (bid 250/op) - evict non-DEX tx.
        checkAndAddTx(account4, false, 5, 250 * 5, true, 0, 3);

        // 1 DEX op - while it fits, 200 bid is not enough (as we evicted tx
        // with 200 DEX bid).
        checkAndAddWithIncreasedBid(account5, true, 1, 200, 0);

        // 1 non-DEX op - while it fits, 100 bid is not enough (as we evicted
        // non-DEX tx with bid 100, but DEX tx was evicted due to DEX limit).
        checkAndAddWithIncreasedBid(account1, false, 1, 100, 0);
    }
    SECTION("DEX evicts non-DEX if DEX lane has not enough ops to evict")
    {
        // 8 non-DEX ops (bid 100/op) - fits
        checkAndAddTx(account1, false, 8, 100 * 8, true, 0, 0);
        // 1 DEX op (bid 200/op) - fits
        checkAndAddTx(account2, true, 1, 200 * 1, true, 0, 0);
        // 3 DEX ops with high fee (bid 10000/op) - fits by evicting 9 ops from
        // both lanes
        checkAndAddTx(account3, true, 3, 10000 * 3, true, 0, 9);
    }
    SECTION("non-DEX transactions evict DEX transactions")
    {
        // Add 9 ops (2 + 1 DEX, 3 + 2 + 1 non-DEX)
        checkAndAddTx(account1, true, 2, 100 * 2, true, 0, 0);
        checkAndAddTx(account2, false, 3, 200 * 3, true, 0, 0);
        checkAndAddTx(account3, true, 1, 300, true, 0, 0);
        checkAndAddTx(account4, false, 2, 400 * 2, true, 0, 0);
        checkAndAddTx(account5, false, 1, 500, true, 0, 0);

        // Evict 2 DEX ops and 3 non-DEX ops.
        checkAndAddWithIncreasedBid(account6, false, 5, 200, 5);
    }

    SECTION("DEX transactions evict non-DEX transactions in DEX slots")
    {
        SECTION("evict only due to global limit")
        {
            // 1 DEX op + 8 non-DEX ops (2 ops in DEX slots).
            checkAndAddTx(account1, true, 1, 200, true, 0, 0);
            checkAndAddTx(account2, false, 6, 400 * 6, true, 0, 0);
            checkAndAddTx(account3, false, 1, 100, true, 0, 0);
            checkAndAddTx(account4, false, 1, 300, true, 0, 0);

            // Evict 1 DEX op and 100/300 non-DEX ops (bids strictly increase)
            checkAndAddWithIncreasedBid(account5, true, 3, 300, 3);
        }
        SECTION("evict due to both global and DEX limits")
        {
            // 2 DEX ops + 7 non-DEX ops (1 op in DEX slots).
            checkAndAddTx(account1, true, 2, 200 * 2, true, 0, 0);
            checkAndAddTx(account2, false, 5, 400 * 6, true, 0, 0);
            checkAndAddTx(account3, false, 1, 100, true, 0, 0);
            checkAndAddTx(account4, false, 1, 150, true, 0, 0);

            SECTION("fill all DEX slots")
            {
                // Evict 2 DEX ops and bid 100 non-DEX op (skip non-DEX 150 bid)
                checkAndAddWithIncreasedBid(account5, true, 3, 200, 3);
            }
            SECTION("fill part of DEX slots")
            {
                // Evict 2 DEX ops and bid 100 non-DEX op (skip non-DEX 150 bid)
                checkAndAddWithIncreasedBid(account5, true, 2, 200, 3);

                SECTION("and add non-DEX tx")
                {
                    // Add a fitting non-DEX tx with at least 100 + 1 bid to
                    // beat the evicted non-DEX tx.
                    checkAndAddWithIncreasedBid(account6, false, 1, 100, 0);
                }
                SECTION("and add DEX tx")
                {
                    // Add a fitting non-DEX tx with at least 200 + 1 bid to
                    // beat the evicted DEX tx.
                    checkAndAddWithIncreasedBid(account6, true, 1, 200, 0);
                }
            }
        }
    }

    SECTION("cannot evict transactions from the same account")
    {
        checkAndAddTx(account1, true, 3, 200 * 3, true, 0, 0);
        checkAndAddTx(account2, false, 6, 100 * 6, true, 0, 0);

        // Even though these transactions have high enough bid, they cannot
        // evict transactions from the same account.
        checkAndAddTx(account1, true, 3, 300 * 3, false, 0, 0);
        checkAndAddTx(account2, false, 4, 300 * 4, false, 0, 0);

        SECTION("but evict DEX transaction from a different account")
        {
            checkAndAddTx(account5, true, 3, 300 * 3, true, 0, 3);
        }
        SECTION("but evict non-DEX transaction from a different account")
        {
            checkAndAddTx(account5, false, 4, 300 * 4, true, 0, 6);
        }
    }

    SECTION("cannot add transaction with more ops than limit")
    {
        SECTION("global limit")
        {
            checkAndAddTx(account1, false, 10, 200 * 10, false, 0, 0);
        }
        SECTION("DEX limit")
        {
            checkAndAddTx(account1, true, 4, 200 * 4, false, 0, 0);
        }
    }
}

TEST_CASE("transaction queue starting sequence boundary",
          "[herder][transactionqueue]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto const minBalance2 = app->getLedgerManager().getLastMinBalance(2);

    auto root = TestAccount::createRoot(*app);
    auto acc1 = root.create("a1", minBalance2);

    closeLedger(*app);
    closeLedger(*app);

    auto nextLedgerSeq = app->getLedgerManager().getLastClosedLedgerNum();

    SECTION("check a single transaction")
    {
        int64_t startingSeq = static_cast<int64_t>(nextLedgerSeq) << 32;
        REQUIRE(acc1.loadSequenceNumber() < startingSeq);
        acc1.bumpSequence(startingSeq - 1);
        REQUIRE(acc1.loadSequenceNumber() == startingSeq - 1);

        ClassicTransactionQueue tq(*app, 4, 10, 4);
        REQUIRE(tq.tryAdd(transaction(*app, acc1, 1, 1, 100), false).code ==
                TransactionQueue::AddResultCode::ADD_STATUS_PENDING);

        auto checkTxSet = [&](uint32_t ledgerSeq) {
            auto lcl = app->getLedgerManager().getLastClosedLedgerHeader();
            lcl.header.ledgerSeq = ledgerSeq;
            return !tq.getTransactions(lcl.header).empty();
        };

        REQUIRE(checkTxSet(2));
        REQUIRE(!checkTxSet(3));
        REQUIRE(checkTxSet(4));
    }
}

TEST_CASE("transaction queue with fee-bump", "[herder][transactionqueue]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.FLOOD_TX_PERIOD_MS = 100;
    // With this setting, max tx queue size will be 14
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 7;
    auto app = createTestApplication(clock, cfg);
    auto const minBalance0 = app->getLedgerManager().getLastMinBalance(0);
    auto const minBalance2 = app->getLedgerManager().getLastMinBalance(2) +
                             DEFAULT_TEST_RESOURCE_FEE;

    auto root = TestAccount::createRoot(*app);
    auto account1 = root.create("a1", minBalance2);
    auto account2 = root.create("a2", minBalance2);
    auto account3 = root.create("a3", minBalance2);

    overrideSorobanNetworkConfigForTest(*app);

    auto testFeeBump = [&](TransactionQueue& queue, bool isSoroban) {
        SECTION("1 fee bump, fee source same as source")
        {
            TransactionQueueTest test{queue};
            auto tx = transaction(*app, account1, 1, 1, 100, 1, isSoroban);
            auto fb = feeBump(*app, account1, tx, 200);
            test.add(fb, TransactionQueue::AddResultCode::ADD_STATUS_PENDING);

            for (uint32 i = 0; i <= 3; ++i)
            {
                test.check({{{account1, i, {fb}}, {account2}, {account3}}, {}});
                test.shift();
            }
            test.check({{{account1}, {account2}, {account3}}, {{fb}}});
        }

        SECTION("1 fee bump, fee source distinct from source")
        {
            TransactionQueueTest test{queue};
            auto tx = transaction(*app, account1, 1, 1, 100, 1, isSoroban);
            auto fb = feeBump(*app, account2, tx, 200);
            test.add(fb, TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
            test.check({{{account1, 0, {fb}}, {account2, 0}}, {}});

            for (uint32 i = 1; i <= 3; ++i)
            {
                test.shift();
                test.check(
                    {{{account1, i, {fb}}, {account2, 0}, {account3}}, {}});
            }
            test.shift();
            test.check({{{account1}, {account2}, {account3}}, {{fb}}});
        }
        SECTION("different ops")
        {
            if (!isSoroban)
            {
                TransactionQueueTest test{queue};
                auto tx = transaction(*app, account1, 1, 1, 100, /* nbOps */ 1,
                                      isSoroban);
                auto txMultiOps = transaction(*app, account1, 1, 1, 10 * 100,
                                              /* nbOps */ 10, isSoroban);
                TransactionFrameBasePtr fb;

                SECTION("more ops")
                {
                    // Set fee=150*10*10, such that feePerOp is higher than tx's
                    // fee (150 > 100)
                    fb = feeBump(*app, account1, txMultiOps, 15000);
                    test.add(
                        tx,
                        TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
                    test.add(
                        fb,
                        TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
                }
                SECTION("less ops")
                {
                    fb = feeBump(*app, account1, tx, 2000);
                    test.add(
                        txMultiOps,
                        TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
                    test.add(
                        fb,
                        TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
                }
                test.check({{{account1, 0, {fb}}, {account2}, {account3}}, {}});
            }
        }
        SECTION("fee bump at limit")
        {
            if (!isSoroban)
            {
                TransactionQueueTest test{queue};
                REQUIRE(queue.getMaxQueueSizeOps() == 14);
                // Tx1 will put tx queue at limit
                auto tx1 = transaction(*app, account1, 1, 1, 14 * 100,
                                       /* nbOps */ 14, isSoroban);
                auto tx2 = transaction(*app, account1, 1, 1, 10 * 100,
                                       /* nbOps */ 10, isSoroban);
                auto fb = feeBump(*app, account1, tx2, 14 * 100 * 10);
                test.add(tx1,
                         TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
                // Allow tx discount to kick in, and fee bump replace the
                // original tx
                test.add(fb,
                         TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
                test.check({{{account1, 0, {fb}}, {account2}, {account3}}, {}});
            }
        }
        SECTION("2 fee bumps with same fee source but different source, "
                "fee source distinct from source")
        {
            TransactionQueueTest test{queue};
            auto tx1 = transaction(*app, account1, 1, 1, 100, 1, isSoroban);
            auto fb1 = feeBump(*app, account3, tx1, 200);
            test.add(fb1, TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
            test.check({{{account1, 0, {fb1}}, {account2}, {account3, 0}}, {}});

            test.shift();
            test.check({{{account1, 1, {fb1}}, {account2}, {account3, 0}}, {}});

            auto tx2 = transaction(*app, account2, 1, 1, 100, 1, isSoroban);
            auto fb2 = feeBump(*app, account3, tx2, 200);
            test.add(fb2, TransactionQueue::AddResultCode::ADD_STATUS_PENDING);

            for (uint32 i = 1; i <= 3; ++i)
            {
                test.check({{{account1, i, {fb1}},
                             {account2, i - 1, {fb2}},
                             {account3, 0}},
                            {}});
                test.shift();
            }
            test.check(
                {{{account1}, {account2, 3, {fb2}}, {account3, 0}}, {{fb1}}});
            test.shift();
            test.check({{{account1}, {account2}, {account3}}, {{fb2}, {fb1}}});
        }

        SECTION("1 fee bump and 1 transaction with same fee source, "
                "fee source distinct from source, fee bump first")
        {
            TransactionQueueTest test{queue};
            auto tx1 = transaction(*app, account1, 1, 1, 100, 1, isSoroban);
            auto fb1 = feeBump(*app, account3, tx1, 200);
            test.add(fb1, TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
            test.check({{{account1, 0, {fb1}}, {account2}, {account3, 0}}, {}});

            test.shift();
            test.check({{{account1, 1, {fb1}}, {account2}, {account3, 0}}, {}});

            auto tx2 = transaction(*app, account3, 1, 1, 100, 1, isSoroban);
            test.add(tx2, TransactionQueue::AddResultCode::ADD_STATUS_PENDING);

            for (uint32 i = 1; i <= 3; ++i)
            {
                test.check({{{account1, i, {fb1}},
                             {account2},
                             {account3, i - 1, {tx2}}},
                            {}});
                test.shift();
            }
            test.check(
                {{{account1}, {account2}, {account3, 3, {tx2}}}, {{fb1}}});
            test.shift();
            test.check({{{account1}, {account2}, {account3}}, {{tx2}, {fb1}}});
        }

        SECTION("1 fee bump and 1 transaction with same fee source, "
                "fee source distinct from source, fee bump second")
        {
            TransactionQueueTest test{queue};
            auto tx1 = transaction(*app, account3, 1, 1, 100, 1, isSoroban);
            test.add(tx1, TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
            test.check({{{account1}, {account2}, {account3, 0, {tx1}}}, {}});

            test.shift();
            test.check({{{account1}, {account2}, {account3, 1, {tx1}}}, {}});

            auto tx2 = transaction(*app, account1, 1, 1, 100, 1, isSoroban);
            auto fb2 = feeBump(*app, account3, tx2, 200);
            test.add(fb2, TransactionQueue::AddResultCode::ADD_STATUS_PENDING);

            for (uint32 i = 1; i <= 3; ++i)
            {
                test.check({{{account1, i - 1, {fb2}},
                             {account2},
                             {account3, i, {tx1}}},
                            {}});
                test.shift();
            }
            test.check(
                {{{account1, 3, {fb2}}, {account2}, {account3, 0}}, {{tx1}}});
            test.shift();
            test.check({{{account1}, {account2}, {account3}}, {{fb2}, {tx1}}});
        }

        SECTION("two fee bumps with same fee source and source, fee source "
                "same as source")
        {
            TransactionQueueTest test{queue};
            auto tx1 = transaction(*app, account1, 1, 1, 100, 1, isSoroban);
            auto fb1 = feeBump(*app, account1, tx1, 200);
            test.add(fb1, TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
            test.check({{{account1, 0, {fb1}}, {account2}, {account3}}, {}});

            auto tx2 = transaction(*app, account1, 2, 1, 100, 1, isSoroban);
            auto fb2 = feeBump(*app, account1, tx2, 200);

            // New fee-bump transaction can't replace the old one
            test.add(
                fb2,
                TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
            test.check({{{account1, 0, {fb1}}, {account2}, {account3}}, {}});
        }

        SECTION("ban first of two fee bumps with same fee source and source, "
                "fee source distinct from source")
        {
            TransactionQueueTest test{queue};
            auto tx1 = transaction(*app, account1, 1, 1, 100, 1, isSoroban);
            auto fb1 = feeBump(*app, account3, tx1, 200);
            test.add(fb1, TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
            test.check({{{account1, 0, {fb1}}, {account2}, {account3, 0}}, {}});

            auto tx2 = transaction(*app, account1, 2, 1, 100, 1, isSoroban);
            auto fb2 = feeBump(*app, account3, tx2, 200);

            // New fee-bump transaction can't replace the old one
            test.add(
                fb2,
                TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
            test.check({{{account1, 0, {fb1}}, {account2}, {account3}}, {}});
        }

        SECTION("add transaction, fee source has insufficient balance due to "
                "fee bumps")
        {
            TransactionQueueTest test{queue};
            auto tx1 = transaction(*app, account1, 1, 1, 100, 1, isSoroban);
            uint32_t discount = 0;
            auto newInclusionToPay = 200;
            if (isSoroban)
            {
                // In case of Soroban, provide additional discount to test the
                // case where inclusion fee is less than balance, but total fee
                // is not.
                discount += newInclusionToPay;
            }
            // Available balance after fb1 is 1
            auto fb1 = feeBump(*app, account3, tx1,
                               minBalance2 - minBalance0 - 1 - discount,
                               /* useInclusionAsFullFee */ true);
            test.add(fb1, TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
            test.check({{{account1, 0, {fb1}}, {account2}, {account3, 0}}, {}});
            if (isSoroban)
            {
                REQUIRE(account3.getAvailableBalance() >= newInclusionToPay);
            }

            SECTION("transaction")
            {
                // NB: source account limit does not apply here; fb1 has
                // account1 as source account (account3 is just a fee source)
                auto tx2 = transaction(*app, account3, 1, 1, newInclusionToPay,
                                       1, isSoroban);
                auto addResult = test.add(
                    tx2, TransactionQueue::AddResultCode::ADD_STATUS_ERROR);
                REQUIRE(addResult.txResult->getResultCode() ==
                        txINSUFFICIENT_BALANCE);
                test.check(
                    {{{account1, 0, {fb1}}, {account2}, {account3, 0}}, {}});
            }

            SECTION("fee bump with fee source same as source")
            {
                auto tx2 = transaction(*app, account3, 1, 1, 100, 1, isSoroban);
                auto fb2 = feeBump(*app, account3, tx2, newInclusionToPay);
                auto addResult = test.add(
                    fb2, TransactionQueue::AddResultCode::ADD_STATUS_ERROR);
                REQUIRE(addResult.txResult->getResultCode() ==
                        txINSUFFICIENT_BALANCE);
                test.check(
                    {{{account1, 0, {fb1}}, {account2}, {account3, 0}}, {}});
            }

            SECTION("fee bump with fee source distinct from source")
            {
                auto tx2 = transaction(*app, account2, 1, 1, 100, 1, isSoroban);
                auto fb2 = feeBump(*app, account3, tx2, newInclusionToPay);
                REQUIRE(account3.getAvailableBalance() >= fb2->getFullFee());
                auto addResult = test.add(
                    fb2, TransactionQueue::AddResultCode::ADD_STATUS_ERROR);
                REQUIRE(addResult.txResult->getResultCode() ==
                        txINSUFFICIENT_BALANCE);
                test.check(
                    {{{account1, 0, {fb1}}, {account2}, {account3, 0}}, {}});
            }
            SECTION("replace by fee valid, check balance")
            {
                SECTION("balance sufficient with oldTx discount")
                {
                    // Top off account3 balance to be able to pay for fb2
                    // (assuming discount from fb1)
                    root.pay(account3, (9 * fb1->getInclusionFee() - 1));
                    auto tx2 =
                        transaction(*app, account1, 1, 1, 100, 1, isSoroban);
                    auto fb2 = feeBump(*app, account3, tx2,
                                       fb1->getInclusionFee() * 10);

                    test.add(
                        fb2,
                        TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
                    test.check(
                        {{{account1, 0, {fb2}}, {account2}, {account3, 0}},
                         {}});
                }
                SECTION("balance insufficient")
                {
                    // valid replace-by-fee, but not enough funds to pay for fb2
                    auto tx2 =
                        transaction(*app, account1, 1, 1, 100, 1, isSoroban);
                    TransactionFrameBasePtr fb2;
                    SECTION("min replace-by-fee threshold")
                    {
                        fb2 = feeBump(*app, account3, tx2,
                                      fb1->getInclusionFee() * 10);
                    }
                    SECTION("maximum fee")
                    {
                        fb2 = feeBump(*app, account3, tx2, INT64_MAX,
                                      /* useInclusionAsFullFee */ true);
                    }

                    auto addResult = test.add(
                        fb2, TransactionQueue::AddResultCode::ADD_STATUS_ERROR);

                    REQUIRE(addResult.txResult->getResultCode() ==
                            txINSUFFICIENT_BALANCE);
                    test.check(
                        {{{account1, 0, {fb1}}, {account2}, {account3, 0}},
                         {}});
                }
            }
        }
        SECTION("transaction or fee bump duplicates fee bump")
        {
            TransactionQueueTest test{queue};
            auto tx1 = transaction(*app, account1, 1, 1, 100, 1, isSoroban);
            auto discount = tx1->getFullFee() - tx1->getInclusionFee();
            auto fb1 = feeBump(*app, account3, tx1,
                               minBalance2 - minBalance0 - 1ll - discount);
            test.add(fb1, TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
            test.check({{{account1, 0, {fb1}}, {account2}, {account3, 0}}, {}});
            test.add(fb1,
                     TransactionQueue::AddResultCode::ADD_STATUS_DUPLICATE);
            test.check({{{account1, 0, {fb1}}, {account2}, {account3, 0}}, {}});
            test.add(tx1,
                     TransactionQueue::AddResultCode::ADD_STATUS_DUPLICATE);
            test.check({{{account1, 0, {fb1}}, {account2}, {account3, 0}}, {}});
        }
    };

    SECTION("classic")
    {
        auto queue = ClassicTransactionQueue{*app, 4, 2, 2};
        testFeeBump(queue, /* isSoroban */ false);
    }
    SECTION("soroban")
    {
        auto queue = SorobanTransactionQueue{*app, 4, 2, 2};
        testFeeBump(queue, /* isSoroban */ true);
    }
}

TEST_CASE("replace by fee", "[herder][transactionqueue]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.FLOOD_TX_PERIOD_MS = 100;
    auto app = createTestApplication(clock, cfg);
    auto const minBalance2 = app->getLedgerManager().getLastMinBalance(2);

    auto root = TestAccount::createRoot(*app);
    auto account1 = root.create("a1", minBalance2);
    auto account2 = root.create("a2", minBalance2);

    auto setupTransactions = [&](TransactionQueueTest& test, bool isSoroban) {
        std::vector<TransactionFrameBasePtr> txs;
        txs.emplace_back(transaction(*app, account1, 1, 1, 200, 1, isSoroban));
        test.add(txs.back(),
                 TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
        return txs;
    };

    auto setupFeeBumps = [&](TransactionQueueTest& test, TestAccount& feeSource,
                             bool isSoroban) {
        std::vector<TransactionFrameBasePtr> txs;
        auto tx = transaction(*app, account1, 1, 1, 100, 1, isSoroban);
        auto fb = feeBump(*app, feeSource, tx, 400);
        txs.emplace_back(fb);
        test.add(txs.back(),
                 TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
        return txs;
    };

    auto submitTransactions = [&](TransactionQueueTest& test,
                                  std::vector<TransactionFrameBasePtr> txs,
                                  bool isSoroban) {
        SECTION("lower fee")
        {
            test.add(
                transaction(*app, account1, 1, 1, 199, 1, isSoroban),
                TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
            test.check({{{account1, 0, txs}, {account2}}, {}});
        }

        SECTION("higher fee below threshold")
        {
            test.add(
                transaction(*app, account1, 1, 1, 1999, 1, isSoroban),
                TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
            test.check({{{account1, 0, txs}, {account2}}, {}});
        }

        SECTION("higher fee at threshold")
        {
            test.add(
                transaction(*app, account1, 1, 1, 2000, 1, isSoroban),
                TransactionQueue::AddResultCode::ADD_STATUS_TRY_AGAIN_LATER);
            test.check({{{account1, 0, txs}, {account2}}, {}});
        }
    };

    auto submitFeeBumps = [&](TransactionQueueTest& test,
                              std::vector<TransactionFrameBasePtr> txs,
                              bool isSoroban) {
        SECTION("lower fee")
        {
            std::vector<TestAccount> accounts{account1, account2};
            for (auto& feeSource : accounts)
            {
                auto tx = transaction(*app, account1, 1, 1, 100, 1, isSoroban);
                auto fb = feeBump(*app, feeSource, tx, 399);
                auto addResult = test.add(
                    fb, TransactionQueue::AddResultCode::ADD_STATUS_ERROR);
                auto& txResult = addResult.txResult;
                REQUIRE(txResult->getResultCode() == txINSUFFICIENT_FEE);
                REQUIRE(txResult->getResult().feeCharged ==
                        4000 + (tx->getFullFee() - tx->getInclusionFee()));
                test.check({{{account1, 0, txs}, {account2}}, {}});
            }
        }

        SECTION("higher fee below threshold")
        {
            std::vector<TestAccount> accounts{account1, account2};
            for (auto& feeSource : accounts)
            {
                auto tx = transaction(*app, account1, 1, 1, 100, 1, isSoroban);
                auto fb = feeBump(*app, feeSource, tx, 3999);
                auto addResult = test.add(
                    fb, TransactionQueue::AddResultCode::ADD_STATUS_ERROR);
                auto& txResult = addResult.txResult;
                REQUIRE(txResult->getResultCode() == txINSUFFICIENT_FEE);
                REQUIRE(txResult->getResult().feeCharged ==
                        4000 + (tx->getFullFee() - tx->getInclusionFee()));
                test.check({{{account1, 0, txs}, {account2}}, {}});
            }
        }

        SECTION("higher fee at threshold")
        {
            auto checkPos = [&](TestAccount& source) {
                auto tx = transaction(*app, account1, 1, 1, 100, 1, isSoroban);
                auto fb = feeBump(*app, source, tx, 4000);
                txs[0] = fb;
                test.add(fb,
                         TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
                test.check({{{account1, 0, txs}, {account2}}, {}});
            };
            SECTION("transaction from same source account")
            {
                checkPos(account1);
            }
            SECTION("transaction from different source account")
            {
                checkPos(account2);
            }
        }
    };

    auto testReplaceByFee = [&](TransactionQueue& queue, bool isSoroban) {
        SECTION("replace transaction with transaction")
        {
            TransactionQueueTest test{queue};
            auto txs = setupTransactions(test, isSoroban);
            submitTransactions(test, txs, isSoroban);
        }

        SECTION("replace transaction with fee-bump")
        {
            TransactionQueueTest test{queue};
            auto txs = setupTransactions(test, isSoroban);
            submitFeeBumps(test, txs, isSoroban);
        }

        SECTION("replace fee-bump having same source and fee-source with "
                "transaction")
        {
            TransactionQueueTest test{queue};
            auto txs = setupFeeBumps(test, account1, isSoroban);
            submitTransactions(test, txs, isSoroban);
        }

        SECTION("replace fee-bump having different source and fee-source with "
                "transaction")
        {
            TransactionQueueTest test{queue};
            auto txs = setupFeeBumps(test, account2, isSoroban);
            submitTransactions(test, txs, isSoroban);
        }

        SECTION(
            "replace fee-bump having same source and fee-source with fee-bump")
        {
            TransactionQueueTest test{queue};
            auto txs = setupFeeBumps(test, account1, isSoroban);
            submitFeeBumps(test, txs, isSoroban);
        }

        SECTION("replace fee-bump having different source and fee-source with "
                "fee-bump")
        {
            TransactionQueueTest test{queue};
            auto txs = setupFeeBumps(test, account2, isSoroban);
            submitFeeBumps(test, txs, isSoroban);
        }
    };

    SECTION("classic")
    {
        auto queue = ClassicTransactionQueue{*app, 4, 2, 2};
        testReplaceByFee(queue, /* isSoroban */ false);
    }
    SECTION("soroban")
    {
        auto queue = SorobanTransactionQueue{*app, 4, 2, 2};
        testReplaceByFee(queue, /* isSoroban */ true);
    }
}

TEST_CASE("remove applied", "[herder][transactionqueue]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    auto app = createTestApplication(clock, cfg);

    auto& lm = app->getLedgerManager();
    auto& herder = static_cast<HerderImpl&>(app->getHerder());
    auto& tq = herder.getTransactionQueue();

    auto root = TestAccount::createRoot(*app);
    auto acc = root.create("A", lm.getLastMinBalance(2));
    auto acc2 = root.create("B", lm.getLastMinBalance(2));
    auto acc3 = root.create("C", lm.getLastMinBalance(2));

    auto tx1a = root.tx({payment(root, 1)});
    root.loadSequenceNumber();
    auto tx1b = root.tx({payment(root, 2)});
    auto tx2 = acc.tx({payment(root, 1)});
    auto tx3 = acc2.tx({payment(root, 1)});
    auto tx4 = acc3.tx({payment(root, 1)});

    herder.recvTransaction(tx1a, false);
    herder.recvTransaction(tx2, false);
    herder.recvTransaction(tx3, false);

    {
        auto const& lcl = lm.getLastClosedLedgerHeader();
        auto ledgerSeq = lcl.header.ledgerSeq + 1;

        root.loadSequenceNumber();
        auto [txSet, _] = makeTxSetFromTransactions({tx1b, tx2}, *app, 0, 0);
        herder.getPendingEnvelopes().putTxSet(txSet->getContentsHash(),
                                              ledgerSeq, txSet);

        auto lastCloseTime = lcl.header.scpValue.closeTime;
        StellarValue sv = herder.makeStellarValue(
            txSet->getContentsHash(), lastCloseTime, emptyUpgradeSteps,
            app->getConfig().NODE_SEED);
        herder.getHerderSCPDriver().valueExternalized(ledgerSeq,
                                                      xdr::xdr_to_opaque(sv));
    }

    REQUIRE(tq.getTransactions({}).size() == 1);
    REQUIRE(herder.recvTransaction(tx4, false).code ==
            TransactionQueue::AddResultCode::ADD_STATUS_PENDING);
    REQUIRE(tq.getTransactions({}).size() == 2);
}

static UnorderedSet<AssetPair, AssetPairHash>
apVecToSet(std::vector<AssetPair> const& v)
{
    UnorderedSet<AssetPair, AssetPairHash> ret;
    for (auto const& a : v)
    {
        ret.emplace(a);
    }
    return ret;
}

TEST_CASE("arbitrage tx identification",
          "[herder][transactionqueue][arbitrage]")
{
    SecretKey aliceSec = txtest::getAccount("alice");
    SecretKey bobSec = txtest::getAccount("bob");
    SecretKey carolSec = txtest::getAccount("carol");

    PublicKey alicePub = aliceSec.getPublicKey();
    PublicKey bobPub = bobSec.getPublicKey();
    PublicKey carolPub = carolSec.getPublicKey();

    Asset xlm = txtest::makeNativeAsset();
    Asset usd = txtest::makeAsset(aliceSec, "USD");
    Asset eur = txtest::makeAsset(bobSec, "EUR");
    Asset cny = txtest::makeAsset(carolSec, "CNY");
    Asset gbp = txtest::makeAsset(carolSec, "GBP");
    Asset inr = txtest::makeAsset(carolSec, "INR");
    Asset mxn = txtest::makeAsset(carolSec, "MXN");
    Asset chf = txtest::makeAsset(carolSec, "CHF");
    Asset jpy = txtest::makeAsset(carolSec, "JPY");

    TransactionEnvelope tx1, tx2, tx3, tx4, tx5, tx6, tx7;
    tx1.type(ENVELOPE_TYPE_TX);
    tx2.type(ENVELOPE_TYPE_TX);
    tx3.type(ENVELOPE_TYPE_TX);
    tx4.type(ENVELOPE_TYPE_TX);
    tx5.type(ENVELOPE_TYPE_TX);
    tx6.type(ENVELOPE_TYPE_TX);
    tx7.type(ENVELOPE_TYPE_TX);

    // Tx1 is a one-op XLM->USD->XLM loop.
    tx1.v1().tx.operations.emplace_back(
        txtest::pathPayment(bobPub, xlm, 100, xlm, 100, {usd}));

    // Tx2 is a two-op contiguous XLM->USD->EUR and EUR->CNY->XLM loop.
    tx2.v1().tx.operations.emplace_back(
        txtest::pathPayment(bobPub, xlm, 100, eur, 100, {usd}));
    tx2.v1().tx.operations.emplace_back(
        txtest::pathPayment(bobPub, eur, 100, xlm, 100, {cny}));

    // Tx3 is a 4-op discontiguous loop: XLM->USD->CNY, GBP->INR->MXN,
    // CNY->EUR->GBP, MXN->CHF->XLM.
    tx3.v1().tx.operations.emplace_back(
        txtest::pathPayment(bobPub, xlm, 100, cny, 100, {usd}));
    tx3.v1().tx.operations.emplace_back(
        txtest::pathPayment(bobPub, gbp, 100, mxn, 100, {inr}));
    tx3.v1().tx.operations.emplace_back(
        txtest::pathPayment(bobPub, cny, 100, gbp, 100, {eur}));
    tx3.v1().tx.operations.emplace_back(
        txtest::pathPayment(bobPub, mxn, 100, xlm, 100, {chf}));

    // Tx4 is the same as Tx3 but the cycle is broken.
    tx4.v1().tx.operations.emplace_back(
        txtest::pathPayment(bobPub, xlm, 100, cny, 100, {usd}));
    tx4.v1().tx.operations.emplace_back(
        txtest::pathPayment(bobPub, gbp, 100, mxn, 100, {inr}));
    tx4.v1().tx.operations.emplace_back(
        txtest::pathPayment(bobPub, cny, 100, jpy, 100, {eur}));
    tx4.v1().tx.operations.emplace_back(
        txtest::pathPayment(bobPub, mxn, 100, xlm, 100, {chf}));

    // Tx5 is a two-op contiguous USD->EUR->CNY->MXN and
    // MXN->JPY->INR->USD loop.
    tx5.v1().tx.operations.emplace_back(
        txtest::pathPayment(bobPub, usd, 100, mxn, 100, {eur, cny}));
    tx5.v1().tx.operations.emplace_back(
        txtest::pathPayment(bobPub, mxn, 100, usd, 100, {jpy, inr}));

    // Tx6 is a four-op pair of loops, formed discontiguously:
    // XLM->USD->CNY, GBP->INR->MXN, CNY->EUR->XLM, MXN->CHF->GBP;
    // We want to identify _both_ loops.
    tx6.v1().tx.operations.emplace_back(
        txtest::pathPayment(bobPub, xlm, 100, cny, 100, {usd}));
    tx6.v1().tx.operations.emplace_back(
        txtest::pathPayment(bobPub, gbp, 100, mxn, 100, {inr}));
    tx6.v1().tx.operations.emplace_back(
        txtest::pathPayment(bobPub, cny, 100, xlm, 100, {eur}));
    tx6.v1().tx.operations.emplace_back(
        txtest::pathPayment(bobPub, mxn, 100, gbp, 100, {chf}));

    // Tx7 is a non-cycle that has 2 paths from the same source
    // to the same destination.
    tx7.v1().tx.operations.emplace_back(
        txtest::pathPayment(bobPub, usd, 100, mxn, 100, {eur, cny}));
    tx7.v1().tx.operations.emplace_back(
        txtest::pathPayment(bobPub, usd, 100, mxn, 100, {jpy, inr}));

    auto tx1f = std::make_shared<TransactionFrame>(Hash(), tx1);
    auto tx2f = std::make_shared<TransactionFrame>(Hash(), tx2);
    auto tx3f = std::make_shared<TransactionFrame>(Hash(), tx3);
    auto tx4f = std::make_shared<TransactionFrame>(Hash(), tx4);
    auto tx5f = std::make_shared<TransactionFrame>(Hash(), tx5);
    auto tx6f = std::make_shared<TransactionFrame>(Hash(), tx6);
    auto tx7f = std::make_shared<TransactionFrame>(Hash(), tx7);

    LOG_TRACE(DEFAULT_LOG, "Tx1 - 1 op / 3 asset contiguous loop");
    REQUIRE(
        apVecToSet(
            TransactionQueue::findAllAssetPairsInvolvedInPaymentLoops(tx1f)) ==
        UnorderedSet<AssetPair, AssetPairHash>{{xlm, usd}, {usd, xlm}});

    LOG_TRACE(DEFAULT_LOG, "Tx2 - 2 op / 4 asset contiguous loop");
    REQUIRE(
        apVecToSet(TransactionQueue::findAllAssetPairsInvolvedInPaymentLoops(
            tx2f)) == UnorderedSet<AssetPair, AssetPairHash>{
                          {xlm, usd}, {usd, eur}, {eur, cny}, {cny, xlm}});

    LOG_TRACE(DEFAULT_LOG, "Tx3 - 4 op / 8 asset discontiguous loop");
    REQUIRE(
        apVecToSet(TransactionQueue::findAllAssetPairsInvolvedInPaymentLoops(
            tx3f)) == UnorderedSet<AssetPair, AssetPairHash>{{xlm, usd},
                                                             {usd, cny},
                                                             {cny, eur},
                                                             {eur, gbp},
                                                             {gbp, inr},
                                                             {inr, mxn},
                                                             {mxn, chf},
                                                             {chf, xlm}});

    LOG_TRACE(DEFAULT_LOG, "Tx4 - 4 op / 8 asset non-loop");
    REQUIRE(
        apVecToSet(TransactionQueue::findAllAssetPairsInvolvedInPaymentLoops(
            tx4f)) == UnorderedSet<AssetPair, AssetPairHash>{});

    LOG_TRACE(DEFAULT_LOG, "Tx5 - 2 op / 6 asset contiguous loop");
    REQUIRE(
        apVecToSet(TransactionQueue::findAllAssetPairsInvolvedInPaymentLoops(
            tx5f)) == UnorderedSet<AssetPair, AssetPairHash>{{usd, eur},
                                                             {eur, cny},
                                                             {cny, mxn},
                                                             {mxn, jpy},
                                                             {jpy, inr},
                                                             {inr, usd}});

    LOG_TRACE(DEFAULT_LOG, "Tx6 - 4 op / 8 asset dual discontiguous loop");
    REQUIRE(
        apVecToSet(TransactionQueue::findAllAssetPairsInvolvedInPaymentLoops(
            tx6f)) == UnorderedSet<AssetPair, AssetPairHash>{{xlm, usd},
                                                             {usd, cny},
                                                             {cny, eur},
                                                             {eur, xlm},
                                                             {gbp, inr},
                                                             {inr, mxn},
                                                             {mxn, chf},
                                                             {chf, gbp}});

    LOG_TRACE(DEFAULT_LOG, "Tx7 - 2 op / 6 asset non-loop");
    REQUIRE(
        apVecToSet(TransactionQueue::findAllAssetPairsInvolvedInPaymentLoops(
            tx7f)) == UnorderedSet<AssetPair, AssetPairHash>{});
}

TEST_CASE("arbitrage tx identification benchmark",
          "[herder][transactionqueue][arbitrage][bench][!hide]")
{
    // This test generates a tx with a single 600-step-long discontiguous loop
    // formed from 100 7-step ops with 100 overlapping endpoints (forcing the
    // use of the SCC checker) and then benchmarks how long it takes to check it
    // for payment loops 100 times, giving a rough idea of how much time the
    // arb-loop checker might take in the worst case in the middle of the
    // txqueue flood loop.
    SecretKey bobSec = txtest::getAccount("bob");
    PublicKey bobPub = bobSec.getPublicKey();
    Asset xlm = txtest::makeNativeAsset();

    TransactionEnvelope tx1;
    tx1.type(ENVELOPE_TYPE_TX);

    Asset prev = xlm;
    for (size_t i = 0; i < MAX_OPS_PER_TX / 2; ++i)
    {
        SecretKey carolSec = txtest::getAccount(fmt::format("carol{}", i));
        Asset aaa = txtest::makeAsset(carolSec, "AAA");
        Asset bbb = txtest::makeAsset(carolSec, "BBB");
        Asset ccc = txtest::makeAsset(carolSec, "CCC");
        Asset ddd = txtest::makeAsset(carolSec, "DDD");
        Asset eee = txtest::makeAsset(carolSec, "EEE");
        Asset fff = txtest::makeAsset(carolSec, "FFF");
        Asset ggg = txtest::makeAsset(carolSec, "GGG");
        Asset hhh = txtest::makeAsset(carolSec, "HHH");
        Asset iii = txtest::makeAsset(carolSec, "III");
        Asset jjj = txtest::makeAsset(carolSec, "JJJ");
        Asset kkk = txtest::makeAsset(carolSec, "KKK");
        Asset lll = txtest::makeAsset(carolSec, "LLL");
        if (i == MAX_OPS_PER_TX / 2 - 1)
        {
            lll = xlm;
        }
        tx1.v1().tx.operations.emplace_back(txtest::pathPayment(
            bobPub, fff, 100, lll, 100, {ggg, hhh, iii, jjj, kkk}));
        tx1.v1().tx.operations.emplace_back(txtest::pathPayment(
            bobPub, prev, 100, fff, 100, {aaa, bbb, ccc, ddd, eee}));
        prev = lll;
    }

    auto tx1f = std::make_shared<TransactionFrame>(Hash(), tx1);

    namespace ch = std::chrono;
    using clock = ch::high_resolution_clock;
    using usec = ch::microseconds;
    auto start = clock::now();
    for (size_t i = 0; i < 100; ++i)
    {
        TransactionQueue::findAllAssetPairsInvolvedInPaymentLoops(tx1f);
    }
    auto end = clock::now();
    LOG_INFO(DEFAULT_LOG, "executed 100 loop-checks of 600-op tx loop in {}",
             ch::duration_cast<ch::milliseconds>(end - start));
}
