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
#include "transactions/SignatureUtils.h"
#include "transactions/TransactionUtils.h"
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

TransactionFrameBasePtr
transaction(Application& app, TestAccount& account, int64_t sequenceDelta,
            int64_t amount, uint32_t fee, int nbOps = 1)
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

TransactionFramePtr
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

    explicit TransactionQueueTest(Application& app)
        : mTransactionQueue{app, 4, 2, 2}
    {
    }

    void
    add(TransactionFrameBasePtr const& tx,
        TransactionQueue::AddResult AddResult)
    {
        REQUIRE(mTransactionQueue.tryAdd(tx, false) == AddResult);
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
                if (INT64_MAX - fee > tx->getFeeBid())
                {
                    fee += tx->getFeeBid();
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
            if (INT64_MAX - fee > tx->getFeeBid())
            {
                fee += tx->getFeeBid();
            }
            else
            {
                fee = INT64_MAX;
            }
        }

        REQUIRE(fees == expectedFees);

        TxSetFrame::Transactions expectedTxs;
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
            REQUIRE(accountTransactionQueueInfo.mMaxSeq == seqNum);
            REQUIRE(accountTransactionQueueInfo.mAge == accountState.mAge);
            REQUIRE(accountTransactionQueueInfo.mBroadcastQueueOps ==
                    accountTransactionQueueInfo.mQueueSizeOps);
            totOps += accountTransactionQueueInfo.mQueueSizeOps;

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
    TransactionQueue mTransactionQueue;
};
}

TEST_CASE("TransactionQueue base", "[herder][transactionqueue]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 4;
    cfg.FLOOD_TX_PERIOD_MS = 100;
    auto app = createTestApplication(clock, cfg);
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
        TransactionQueueTest test{*app};

        // adding first tx
        // too small seqnum
        test.add(txSeqA1T0, TransactionQueue::AddResult::ADD_STATUS_ERROR);
        test.check({{{account1}, {account2}}, {}});
        // too big seqnum
        test.add(txSeqA1T2, TransactionQueue::AddResult::ADD_STATUS_ERROR);
        test.check({{{account1}, {account2}}, {}});

        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}, {}});

        // adding second tx
        test.add(txSeqA1T2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1, txSeqA1T2}}, {account2}}, {}});

        // adding third tx
        // duplicates
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_DUPLICATE);
        test.check({{{account1, 0, {txSeqA1T1, txSeqA1T2}}, {account2}}, {}});
        test.add(txSeqA1T2, TransactionQueue::AddResult::ADD_STATUS_DUPLICATE);
        test.check({{{account1, 0, {txSeqA1T1, txSeqA1T2}}, {account2}}, {}});
        // too low
        test.add(txSeqA1T0, TransactionQueue::AddResult::ADD_STATUS_ERROR);
        test.check({{{account1, 0, {txSeqA1T1, txSeqA1T2}}, {account2}}, {}});
        // too high
        test.add(txSeqA1T4, TransactionQueue::AddResult::ADD_STATUS_ERROR);
        test.check({{{account1, 0, {txSeqA1T1, txSeqA1T2}}, {account2}}, {}});
        // just right
        test.add(txSeqA1T3, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check(
            {{{account1, 0, {txSeqA1T1, txSeqA1T2, txSeqA1T3}}, {account2}},
             {}});
    }

    SECTION("good sequence number, same twice with shift")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}, {}});
        test.shift();
        test.check({{{account1, 1, {txSeqA1T1}}, {account2}}, {}});
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_DUPLICATE);
        test.check({{{account1, 1, {txSeqA1T1}}, {account2}}, {}});
    }

    SECTION("good then big sequence number, with shift")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}, {}});
        test.shift();
        test.check({{{account1, 1, {txSeqA1T1}}, {account2}}, {}});
        test.add(txSeqA1T3, TransactionQueue::AddResult::ADD_STATUS_ERROR);
        test.check({{{account1, 1, {txSeqA1T1}}, {account2}}, {}});
    }

    SECTION("good then good sequence number, with shift")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}, {}});
        test.shift();
        test.check({{{account1, 1, {txSeqA1T1}}, {account2}}, {}});
        test.add(txSeqA1T2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 1, {txSeqA1T1, txSeqA1T2}}, {account2}}, {}});
    }

    SECTION("good sequence number, same twice with double shift")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}, {}});
        test.shift();
        test.check({{{account1, 1, {txSeqA1T1}}, {account2}}, {}});
        test.shift();
        test.check({{{account1, 2, {txSeqA1T1}}, {account2}}, {}});
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_DUPLICATE);
        test.check({{{account1, 2, {txSeqA1T1}}, {account2}}, {}});
    }

    SECTION("good then big sequence number, with double shift")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 1, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 2, {txSeqA1T1}}, {account2}}});
        test.add(txSeqA1T3, TransactionQueue::AddResult::ADD_STATUS_ERROR);
        test.check({{{account1, 2, {txSeqA1T1}}, {account2}}});
    }

    SECTION("good then good sequence number, with double shift")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 1, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 2, {txSeqA1T1}}, {account2}}});
        test.add(txSeqA1T2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 2, {txSeqA1T1, txSeqA1T2}}, {account2}}});
    }

    SECTION("good sequence number, same twice with four shifts, then two more")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
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
                 TransactionQueue::AddResult::ADD_STATUS_TRY_AGAIN_LATER);
        test.check({{{account1}, {account2}}, {{txSeqA1T1}}});
        test.shift();
        test.check({{{account1}, {account2}}, {{}, {txSeqA1T1}}});
        test.shift();
        test.check({{{account1}, {account2}}});
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}});
    }

    SECTION("good then big sequence number, with four shifts")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 1, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 2, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 3, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1}, {account2}}, {{txSeqA1T1}}});
        test.add(txSeqA1T3, TransactionQueue::AddResult::ADD_STATUS_ERROR);
        test.check({{{account1}, {account2}}, {{txSeqA1T1}}});
    }

    SECTION("good then small sequence number, with four shifts")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 1, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 2, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 3, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1}, {account2}}, {{txSeqA1T1}}});
        test.add(txSeqA1T0, TransactionQueue::AddResult::ADD_STATUS_ERROR);
        test.check({{{account1}, {account2}}, {{txSeqA1T1}}});
    }

    SECTION("invalid transaction")
    {
        TransactionQueueTest test{*app};
        test.add(invalidTransaction(*app, account1, 1),
                 TransactionQueue::AddResult::ADD_STATUS_ERROR);
        test.check({{{account1}, {account2}}});
    }

    SECTION("multiple good sequence numbers, with four shifts")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}});
        test.add(txSeqA1T2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1, txSeqA1T2}}, {account2}}});
        test.add(txSeqA1T3, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check(
            {{{account1, 0, {txSeqA1T1, txSeqA1T2, txSeqA1T3}}, {account2}}});
        test.add(txSeqA1T4, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check(
            {{{account1, 0, {txSeqA1T1, txSeqA1T2, txSeqA1T3, txSeqA1T4}},
              {account2}}});
        test.shift();
        test.check(
            {{{account1, 1, {txSeqA1T1, txSeqA1T2, txSeqA1T3, txSeqA1T4}},
              {account2}}});
        test.shift();
        test.check(
            {{{account1, 2, {txSeqA1T1, txSeqA1T2, txSeqA1T3, txSeqA1T4}},
              {account2}}});
        test.shift();
        test.check(
            {{{account1, 3, {txSeqA1T1, txSeqA1T2, txSeqA1T3, txSeqA1T4}},
              {account2}}});
        test.shift();
        test.check({{{account1}, {account2}},
                    {{txSeqA1T1, txSeqA1T2, txSeqA1T3, txSeqA1T4}}});
        test.shift();
        test.check({{{account1}, {account2}},
                    {{}, {txSeqA1T1, txSeqA1T2, txSeqA1T3, txSeqA1T4}}});
        test.shift();
        test.check({{{account1}, {account2}}});
    }

    SECTION("multiple good sequence numbers, with replace")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}});
        test.add(txSeqA1T2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1, txSeqA1T2}}, {account2}}});
        test.shift();
        test.check({{{account1, 1, {txSeqA1T1, txSeqA1T2}}, {account2}}});
        test.shift();
        test.check({{{account1, 2, {txSeqA1T1, txSeqA1T2}}, {account2}}});
        test.shift();
        test.check({{{account1, 3, {txSeqA1T1, txSeqA1T2}}, {account2}}});
        test.shift();
        test.check({{{account1}, {account2}}, {{txSeqA1T1, txSeqA1T2}}});
        test.add(txSeqA1T1,
                 TransactionQueue::AddResult::ADD_STATUS_TRY_AGAIN_LATER);
        test.check({{{account1}, {account2}}, {{txSeqA1T1, txSeqA1T2}}});
        test.add(txSeqA1T2,
                 TransactionQueue::AddResult::ADD_STATUS_TRY_AGAIN_LATER);
        test.check({{{account1}, {account2}}, {{txSeqA1T1, txSeqA1T2}}});
        test.add(txSeqA1T2V2, TransactionQueue::AddResult::ADD_STATUS_ERROR);
        test.check({{{account1}, {account2}}, {{txSeqA1T1, txSeqA1T2}}});
        test.add(txSeqA1T1V2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1V2}}, {account2}},
                    {{txSeqA1T1, txSeqA1T2}}});
        test.add(txSeqA1T1,
                 TransactionQueue::AddResult::ADD_STATUS_TRY_AGAIN_LATER);
        test.check({{{account1, 0, {txSeqA1T1V2}}, {account2}},
                    {{txSeqA1T1, txSeqA1T2}}});
        test.add(txSeqA1T2,
                 TransactionQueue::AddResult::ADD_STATUS_TRY_AGAIN_LATER);
        test.check({{{account1, 0, {txSeqA1T1V2}}, {account2}},
                    {{txSeqA1T1, txSeqA1T2}}});
        test.add(txSeqA1T2V2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1V2, txSeqA1T2V2}}, {account2}},
                    {{txSeqA1T1, txSeqA1T2}}});
    }

    SECTION("multiple good sequence numbers, with shifts between")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 1, {txSeqA1T1}}, {account2}}});
        test.add(txSeqA1T2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 1, {txSeqA1T1, txSeqA1T2}}, {account2}}});
        test.shift();
        test.check({{{account1, 2, {txSeqA1T1, txSeqA1T2}}, {account2}}});
        test.add(txSeqA1T3, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check(
            {{{account1, 2, {txSeqA1T1, txSeqA1T2, txSeqA1T3}}, {account2}}});
        test.shift();
        test.check(
            {{{account1, 3, {txSeqA1T1, txSeqA1T2, txSeqA1T3}}, {account2}}});
        test.add(txSeqA1T4, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check(
            {{{account1, 3, {txSeqA1T1, txSeqA1T2, txSeqA1T3, txSeqA1T4}},
              {account2}}});
        test.shift();
        test.check({{{account1}, {account2}},
                    {{txSeqA1T1, txSeqA1T2, txSeqA1T3, txSeqA1T4}}});
        test.shift();
        test.check({{{account1}, {account2}},
                    {{}, {txSeqA1T1, txSeqA1T2, txSeqA1T3, txSeqA1T4}}});
        test.shift();
        test.check({{{account1}, {account2}}});
    }

    SECTION(
        "multiple good sequence numbers, different accounts, with four shifts")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}});
        test.add(txSeqA2T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2, 0, {txSeqA2T1}}}});
        test.add(txSeqA1T2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1, txSeqA1T2}},
                     {account2, 0, {txSeqA2T1}}}});
        test.add(txSeqA2T2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1, txSeqA1T2}},
                     {account2, 0, {txSeqA2T1, txSeqA2T2}}}});
        test.shift();
        test.check({{{account1, 1, {txSeqA1T1, txSeqA1T2}},
                     {account2, 1, {txSeqA2T1, txSeqA2T2}}}});
        test.shift();
        test.check({{{account1, 2, {txSeqA1T1, txSeqA1T2}},
                     {account2, 2, {txSeqA2T1, txSeqA2T2}}}});
        test.shift();
        test.check({{{account1, 3, {txSeqA1T1, txSeqA1T2}},
                     {account2, 3, {txSeqA2T1, txSeqA2T2}}}});
        test.shift();
        test.check({{{account1}, {account2}},
                    {{txSeqA1T1, txSeqA2T1, txSeqA1T2, txSeqA2T2}}});
    }

    SECTION("multiple good sequence numbers, different accounts, with shifts "
            "between")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}});
        test.shift();
        test.check({{{account1, 1, {txSeqA1T1}}, {account2}}});
        test.add(txSeqA2T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 1, {txSeqA1T1}}, {account2, 0, {txSeqA2T1}}}});
        test.shift();
        test.check({{{account1, 2, {txSeqA1T1}}, {account2, 1, {txSeqA2T1}}}});
        test.add(txSeqA1T2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 2, {txSeqA1T1, txSeqA1T2}},
                     {account2, 1, {txSeqA2T1}}}});
        test.shift();
        test.check({{{account1, 3, {txSeqA1T1, txSeqA1T2}},
                     {account2, 2, {txSeqA2T1}}}});
        test.add(txSeqA2T2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 3, {txSeqA1T1, txSeqA1T2}},
                     {account2, 2, {txSeqA2T1, txSeqA2T2}}}});
        test.shift();
        test.check({{{account1}, {account2, 3, {txSeqA2T1, txSeqA2T2}}},
                    {{txSeqA1T1, txSeqA1T2}}});
        test.shift();
        test.check({{{account1}, {account2}},
                    {{txSeqA2T1, txSeqA2T2}, {txSeqA1T1, txSeqA1T2}}});
        test.shift();
        test.check({{{account1}, {account2}}, {{}, {txSeqA2T1, txSeqA2T2}}});
        test.shift();
        test.check({{{account1}, {account2}}});
    }

    SECTION("multiple good sequence numbers, different accounts, with remove")
    {
        TransactionQueueTest test{*app};
        SECTION("with shift and remove")
        {
            test.add(txSeqA1T1,
                     TransactionQueue::AddResult::ADD_STATUS_PENDING);
            test.check({{{account1, 0, {txSeqA1T1}}, {account2}}});
            test.add(txSeqA2T1,
                     TransactionQueue::AddResult::ADD_STATUS_PENDING);
            test.check(
                {{{account1, 0, {txSeqA1T1}}, {account2, 0, {txSeqA2T1}}}});
            test.add(txSeqA1T2,
                     TransactionQueue::AddResult::ADD_STATUS_PENDING);
            test.check({{{account1, 0, {txSeqA1T1, txSeqA1T2}},
                         {account2, 0, {txSeqA2T1}}}});
            test.add(txSeqA2T2,
                     TransactionQueue::AddResult::ADD_STATUS_PENDING);
            test.check({{{account1, 0, {txSeqA1T1, txSeqA1T2}},
                         {account2, 0, {txSeqA2T1, txSeqA2T2}}}});
            test.shift();
            test.check({{{account1, 1, {txSeqA1T1, txSeqA1T2}},
                         {account2, 1, {txSeqA2T1, txSeqA2T2}}}});
            test.removeApplied({txSeqA1T1, txSeqA2T2});
            test.check({{{account1, 0, {txSeqA1T2}}, {account2}},
                        {{txSeqA1T1, txSeqA2T2}, {}}});
            test.removeApplied({txSeqA1T2});
            test.check({{{account1}, {account2}},
                        {{txSeqA1T1, txSeqA2T2, txSeqA1T2}, {}}});
        }
        SECTION("with remove")
        {
            test.add(txSeqA1T1,
                     TransactionQueue::AddResult::ADD_STATUS_PENDING);
            test.check({{{account1, 0, {txSeqA1T1}}, {account2}}});
            test.add(txSeqA2T1,
                     TransactionQueue::AddResult::ADD_STATUS_PENDING);
            test.check(
                {{{account1, 0, {txSeqA1T1}}, {account2, 0, {txSeqA2T1}}}});
            test.add(txSeqA2T2,
                     TransactionQueue::AddResult::ADD_STATUS_PENDING);
            test.check({{{account1, 0, {txSeqA1T1}},
                         {account2, 0, {txSeqA2T1, txSeqA2T2}}}});
            test.removeApplied({txSeqA2T1});
            test.check(
                {{{account1, 0, {txSeqA1T1}}, {account2, 0, {txSeqA2T2}}},
                 {{txSeqA2T1}, {}}});
            test.removeApplied({txSeqA2T2});
            test.check({{{account1, 0, {txSeqA1T1}}, {account2}},
                        {{txSeqA2T1, txSeqA2T2}, {}}});
            test.removeApplied({txSeqA1T1});
            test.check({{{account1}, {account2}},
                        {{txSeqA2T1, txSeqA2T2, txSeqA1T1}, {}}});
        }
    }

    SECTION("multiple good sequence numbers, different accounts, with ban")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.add(txSeqA2T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.add(txSeqA1T2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.add(txSeqA2T2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.shift();
        test.ban({txSeqA1T1, txSeqA2T2, txSeqA3T1});
        test.check({{{account1}, {account2, 1, {txSeqA2T1}}},
                    {{txSeqA1T1, txSeqA1T2, txSeqA2T2, txSeqA3T1}}});
        test.add(txSeqA1T1,
                 TransactionQueue::AddResult::ADD_STATUS_TRY_AGAIN_LATER);
        test.check({{{account1}, {account2, 1, {txSeqA2T1}}},
                    {{txSeqA1T1, txSeqA1T2, txSeqA2T2, txSeqA3T1}}});

        // still banned when we shift
        test.shift();
        test.check({{{account1}, {account2, 2, {txSeqA2T1}}},
                    {{}, {txSeqA1T1, txSeqA1T2, txSeqA2T2, txSeqA3T1}}});
        test.add(txSeqA1T1,
                 TransactionQueue::AddResult::ADD_STATUS_TRY_AGAIN_LATER);
        test.add(txSeqA3T1,
                 TransactionQueue::AddResult::ADD_STATUS_TRY_AGAIN_LATER);
        // not banned anymore
        test.shift();
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.add(txSeqA3T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}},
                     {account2, 3, {txSeqA2T1}},
                     {account3, 0, {txSeqA3T1}}}});
    }
}

TEST_CASE("TransactionQueue hitting the rate limit",
          "[herder][transactionqueue]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 4;
    cfg.FLOOD_TX_PERIOD_MS = 100;
    auto app = createTestApplication(clock, cfg);
    auto const minBalance2 = app->getLedgerManager().getLastMinBalance(2);

    auto root = TestAccount::createRoot(*app);
    auto account1 = root.create("a1", minBalance2);
    auto account2 = root.create("a2", minBalance2);
    auto account3 = root.create("a3", minBalance2);

    TransactionQueueTest testQueue{*app};
    std::vector<TransactionFrameBasePtr> txs;
    auto addTx = [&](TransactionFrameBasePtr tx) {
        txs.push_back(tx);
        testQueue.add(tx, TransactionQueue::AddResult::ADD_STATUS_PENDING);
    };
    // Fill the queue/limiter with 8 ops (2 * 4) - any further ops should result
    // in eviction (limit is 2 * 4=TESTING_UPGRADE_MAX_TX_SET_SIZE).
    addTx(transaction(*app, account1, 1, 1, 200 * 1, 1));
    addTx(transaction(*app, account1, 2, 1, 400 * 2, 2));
    addTx(transaction(*app, account1, 3, 1, 100 * 1, 1));
    addTx(transaction(*app, account2, 1, 1, 300 * 4, 4));

    SECTION("cannot add low fee tx")
    {
        auto tx = transaction(*app, account3, 1, 1, 300 * 3, 3);
        testQueue.add(tx, TransactionQueue::AddResult::ADD_STATUS_ERROR);
        REQUIRE(tx->getResult().result.code() == txINSUFFICIENT_FEE);
        REQUIRE(tx->getResult().feeCharged == 300 * 3 + 1);
    }
    SECTION("add high fee tx with eviction")
    {
        auto tx = transaction(*app, account3, 1, 1, 300 * 3 + 1, 3);
        testQueue.add(tx, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        // Evict all txs from `account1` as `tx[2]` can't be applied
        // after `tx[1]` is evicted.
        testQueue.check(
            {{{account1}, {account2, 0, {txs[3]}}, {account3, 0, {tx}}},
             {{txs[0], txs[1], txs[2]}, {}}});

        SECTION("then cannot add tx with lower fee than evicted")
        {
            auto nextTx = transaction(*app, account3, 2, 1, 200, 1);
            testQueue.add(nextTx,
                          TransactionQueue::AddResult::ADD_STATUS_ERROR);
            REQUIRE(nextTx->getResult().result.code() == txINSUFFICIENT_FEE);
            REQUIRE(nextTx->getResult().feeCharged == 201);
        }
        SECTION("then add tx with higher fee than evicted")
        {
            // The last evicted fee rate we accounted for was 200 (tx with fee
            // rate 400 is evicted due to seq num and is not accounted for).
            auto nextTx = transaction(*app, account3, 2, 1, 201, 1);
            testQueue.add(nextTx,
                          TransactionQueue::AddResult::ADD_STATUS_PENDING);
            testQueue.check({{{account1},
                              {account2, 0, {txs[3]}},
                              {account3, 0, {tx, nextTx}}},
                             {{txs[0], txs[1], txs[2]}, {}}});
        }
    }
}

TEST_CASE_VERSIONS("TransactionQueue with PreconditionsV2",
                   "[herder][transactionqueue]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 4;
    cfg.FLOOD_TX_PERIOD_MS = 100;
    auto app = createTestApplication(clock, cfg);
    auto const minBalance2 = app->getLedgerManager().getLastMinBalance(2);

    for_versions_from(19, *app, [&] {
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

        auto txSeqA1S5MinSeqNum = transactionWithV2Precondition(
            *app, account1, 5, 200, condMinSeqNum);

        auto txSeqA1S4MinSeqNum = transactionWithV2Precondition(
            *app, account1, 4, 200, condMinSeqNum);

        auto txSeqA1S8MinSeqNum = transactionWithV2Precondition(
            *app, account1, 8, 200, condMinSeqNum);

        PreconditionsV2 condMinSeqAge;
        condMinSeqAge.minSeqAge = 1;
        auto txSeqA1S3MinSeqAge = transactionWithV2Precondition(
            *app, account1, 3, 200, condMinSeqAge);

        PreconditionsV2 condMinSeqLedgerGap;
        condMinSeqLedgerGap.minSeqLedgerGap = 1;
        auto txSeqA1S3MinSeqLedgerGap = transactionWithV2Precondition(
            *app, account1, 3, 200, condMinSeqLedgerGap);

        SECTION("gap valid due to minSeqNum")
        {
            TransactionQueueTest test{*app};
            test.add(txSeqA1S1,
                     TransactionQueue::AddResult::ADD_STATUS_PENDING);

            {
                // Try tx with a minSeqNum that's not low enough
                PreconditionsV2 cond;
                cond.minSeqNum.activate() =
                    account1.getLastSequenceNumber() + 2;
                auto tx =
                    transactionWithV2Precondition(*app, account1, 5, 200, cond);

                test.add(tx, TransactionQueue::AddResult::ADD_STATUS_ERROR);
            }

            test.add(txSeqA1S5MinSeqNum,
                     TransactionQueue::AddResult::ADD_STATUS_PENDING);
            test.add(txSeqA1S6,
                     TransactionQueue::AddResult::ADD_STATUS_PENDING);

            // make sure duplicates are identified correctly
            test.add(txSeqA1S1,
                     TransactionQueue::AddResult::ADD_STATUS_DUPLICATE);
            test.add(txSeqA1S5MinSeqNum,
                     TransactionQueue::AddResult::ADD_STATUS_DUPLICATE);
            test.add(txSeqA1S6,
                     TransactionQueue::AddResult::ADD_STATUS_DUPLICATE);

            // try to fill in gap with a tx
            test.add(txSeqA1S2, TransactionQueue::AddResult::ADD_STATUS_ERROR);

            // try to fill in gap with a minSeqNum tx
            test.add(txSeqA1S4MinSeqNum,
                     TransactionQueue::AddResult::ADD_STATUS_ERROR);

            test.check(
                {{{account1, 0, {txSeqA1S1, txSeqA1S5MinSeqNum, txSeqA1S6}},
                  {account2}},
                 {}});

            // fee bump the existing minSeqNum tx
            auto fb = feeBump(*app, account1, txSeqA1S5MinSeqNum, 4000);
            test.add(fb, TransactionQueue::AddResult::ADD_STATUS_PENDING);

            test.check(
                {{{account1, 0, {txSeqA1S1, fb, txSeqA1S6}}, {account2}}, {}});

            // fee bump a new minSeqNum tx
            auto fb2 = feeBump(*app, account1, txSeqA1S8MinSeqNum, 400);
            test.add(fb2, TransactionQueue::AddResult::ADD_STATUS_PENDING);

            test.check(
                {{{account1, 0, {txSeqA1S1, fb, txSeqA1S6, fb2}}, {account2}},
                 {}});

            SECTION("removeApplied")
            {
                // seqNum=2 and below should be removed here
                test.removeApplied({txSeqA1S2});
                test.check({{{account1, 0, {fb, txSeqA1S6, fb2}}, {account2}},
                            {{txSeqA1S2}, {}}});

                // seqNum=4. No change
                test.removeApplied({txSeqA1S4MinSeqNum}, true);
                test.check({{{account1, 0, {fb, txSeqA1S6, fb2}}, {account2}},
                            {{txSeqA1S2, txSeqA1S4MinSeqNum}, {}}});

                // seqNum=5 and below should be removed here
                test.removeApplied({fb});
                test.check({{{account1, 0, {txSeqA1S6, fb2}}, {account2}},
                            {{txSeqA1S2, txSeqA1S4MinSeqNum, fb}, {}}});

                SECTION("removeApplied last tx")
                {
                    // seqNum=8 and below should be removed here
                    test.removeApplied({fb2});
                    test.check(
                        {{{account1, 0, {}}, {account2}},
                         {{txSeqA1S2, txSeqA1S4MinSeqNum, fb, fb2}, {}}});
                }
                SECTION("removeApplied past last tx")
                {
                    // seqNum=9 and below should be removed here
                    auto txSeqA1S9 = transaction(*app, account1, 9, 1, 200);
                    test.removeApplied({txSeqA1S9});
                    test.check(
                        {{{account1, 0, {}}, {account2}},
                         {{txSeqA1S2, txSeqA1S4MinSeqNum, fb, txSeqA1S9}, {}}});
                }
            }
            SECTION("ban")
            {
                SECTION("ban first tx")
                {
                    test.ban({txSeqA1S1});
                    test.check({{{account1, 0, {}}, {account2}},
                                {{txSeqA1S1, fb, txSeqA1S6, fb2}}});
                }
                SECTION("ban missing tx")
                {
                    test.ban({txSeqA1S2});
                    // no queue change
                    test.check({{{account1, 0, {txSeqA1S1, fb, txSeqA1S6, fb2}},
                                 {account2}},
                                {{txSeqA1S2}}});
                }
                SECTION("ban existing tx with larger seqnum first, missing tx "
                        "second")
                {
                    test.ban({fb, txSeqA1S2});
                    test.check({{{account1, 0, {txSeqA1S1}}, {account2}},
                                {{txSeqA1S2, fb, txSeqA1S6, fb2}}});
                }
            }
        }
        SECTION("fee bump new tx with minSeqNum past lastSeq")
        {
            PreconditionsV2 cond;
            cond.minSeqNum.activate() = account1.getLastSequenceNumber() + 2;
            auto tx =
                transactionWithV2Precondition(*app, account1, 5, 200, cond);

            TransactionQueueTest test{*app};
            test.add(tx, TransactionQueue::AddResult::ADD_STATUS_ERROR);
        }
        SECTION("fee bump only existing tx")
        {
            PreconditionsV2 cond;
            cond.minSeqNum.activate() = 2;
            auto tx =
                transactionWithV2Precondition(*app, account1, 5, 200, cond);

            TransactionQueueTest test{*app};
            test.add(tx, TransactionQueue::AddResult::ADD_STATUS_PENDING);

            auto fb = feeBump(*app, account1, tx, 4000);
            test.add(fb, TransactionQueue::AddResult::ADD_STATUS_PENDING);

            test.check({{{account1, 0, {fb}}, {account2}}, {}});
        }
        SECTION("fee bump existing tx and add minSeqNum")
        {
            TransactionQueueTest test{*app};
            test.add(txSeqA1S1,
                     TransactionQueue::AddResult::ADD_STATUS_PENDING);

            PreconditionsV2 cond;
            cond.minSeqNum.activate() = 2;

            auto tx =
                transactionWithV2Precondition(*app, account1, 1, 200, cond);
            auto fb = feeBump(*app, account1, tx, 4000);
            test.add(fb, TransactionQueue::AddResult::ADD_STATUS_PENDING);

            test.check({{{account1, 0, {fb}}, {account2}}, {}});
        }
        SECTION("fee bump existing tx and remove minSeqNum")
        {
            TransactionQueueTest test{*app};

            PreconditionsV2 cond;
            cond.minSeqNum.activate() = 2;

            auto tx =
                transactionWithV2Precondition(*app, account1, 1, 200, cond);
            test.add(tx, TransactionQueue::AddResult::ADD_STATUS_PENDING);

            auto fb = feeBump(*app, account1, txSeqA1S1, 4000);
            test.add(fb, TransactionQueue::AddResult::ADD_STATUS_PENDING);

            test.check({{{account1, 0, {fb}}, {account2}}, {}});
        }
        SECTION("Try invalidating preconditions with fee bump")
        {
            TransactionQueueTest test{*app};
            test.add(txSeqA1S1,
                     TransactionQueue::AddResult::ADD_STATUS_PENDING);
            test.add(txSeqA1S5MinSeqNum,
                     TransactionQueue::AddResult::ADD_STATUS_PENDING);

            // try removing minSeqNum from second tx
            {
                auto txS5 = transaction(*app, account1, 5, 1, 200);
                auto fb = feeBump(*app, account1, txS5, 4000);
                test.add(fb, TransactionQueue::AddResult::ADD_STATUS_ERROR);
            }

            // add minSeqLedgerGap to second tx
            {
                PreconditionsV2 cond;
                cond.minSeqNum.activate() = 2;
                cond.minSeqLedgerGap = 1;

                auto tx =
                    transactionWithV2Precondition(*app, account1, 5, 200, cond);

                auto fb = feeBump(*app, account1, tx, 4000);
                test.add(
                    fb,
                    TransactionQueue::AddResult::ADD_STATUS_TRY_AGAIN_LATER);
            }

            // add minSeqAge to second tx
            {
                PreconditionsV2 cond;
                cond.minSeqNum.activate() = 2;
                cond.minSeqAge = 1;

                auto tx =
                    transactionWithV2Precondition(*app, account1, 5, 200, cond);

                auto fb = feeBump(*app, account1, tx, 4000);
                test.add(
                    fb,
                    TransactionQueue::AddResult::ADD_STATUS_TRY_AGAIN_LATER);
            }

            test.check(
                {{{account1, 0, {txSeqA1S1, txSeqA1S5MinSeqNum}}, {account2}},
                 {}});
        }
        SECTION("remove unnecessary minSeqNum with feeBump")
        {
            TransactionQueueTest test{*app};

            test.add(txSeqA1S1,
                     TransactionQueue::AddResult::ADD_STATUS_PENDING);

            auto txSeqA1S2MinSeqNum = transactionWithV2Precondition(
                *app, account1, 2, 200, condMinSeqNum);
            test.add(txSeqA1S2MinSeqNum,
                     TransactionQueue::AddResult::ADD_STATUS_PENDING);

            auto fb = feeBump(*app, account1, txSeqA1S2, 4000);
            test.add(fb, TransactionQueue::AddResult::ADD_STATUS_PENDING);

            test.check({{{account1, 0, {txSeqA1S1, fb}}, {account2}}, {}});
        }
        SECTION("fee bump existing tx and add all preconditions")
        {
            // move lcl forward
            closeLedgerOn(*app, 1, 1, 2022);
            TransactionQueueTest test{*app};
            test.add(txSeqA1S1,
                     TransactionQueue::AddResult::ADD_STATUS_PENDING);
            test.add(txSeqA1S5MinSeqNum,
                     TransactionQueue::AddResult::ADD_STATUS_PENDING);

            PreconditionsV2 cond;
            cond.minSeqAge = 1;
            cond.minSeqLedgerGap = 1;
            cond.minSeqNum.activate() = 1;

            auto lclNum = app->getLedgerManager().getLastClosedLedgerNum();
            LedgerBounds bounds;
            bounds.minLedger = lclNum + 1;
            bounds.maxLedger = lclNum + 2;
            cond.ledgerBounds.activate() = bounds;

            auto tx =
                transactionWithV2Precondition(*app, account1, 1, 200, cond);

            auto fb = feeBump(*app, account1, tx, 4000);
            test.add(fb, TransactionQueue::AddResult::ADD_STATUS_PENDING);

            test.check(
                {{{account1, 0, {fb, txSeqA1S5MinSeqNum}}, {account2}}, {}});
        }
        SECTION("minSeqAge failed due to lower seqNum in queue")
        {
            TransactionQueueTest test{*app};
            test.add(txSeqA1S1,
                     TransactionQueue::AddResult::ADD_STATUS_PENDING);
            test.add(txSeqA1S2,
                     TransactionQueue::AddResult::ADD_STATUS_PENDING);
            test.add(txSeqA1S3MinSeqAge,
                     TransactionQueue::AddResult::ADD_STATUS_TRY_AGAIN_LATER);

            // submit as fee bump
            auto fb = feeBump(*app, account1, txSeqA1S3MinSeqAge, 4000);
            test.add(fb,
                     TransactionQueue::AddResult::ADD_STATUS_TRY_AGAIN_LATER);

            test.check(
                {{{account1, 0, {txSeqA1S1, txSeqA1S2}}, {account2}}, {}});
        }
        SECTION("minSeqLedgerGap failed due to lower seqNum in queue")
        {
            TransactionQueueTest test{*app};
            test.add(txSeqA1S1,
                     TransactionQueue::AddResult::ADD_STATUS_PENDING);
            test.add(txSeqA1S2,
                     TransactionQueue::AddResult::ADD_STATUS_PENDING);
            test.add(txSeqA1S3MinSeqLedgerGap,
                     TransactionQueue::AddResult::ADD_STATUS_TRY_AGAIN_LATER);

            // submit as fee bump
            auto fb = feeBump(*app, account1, txSeqA1S3MinSeqLedgerGap, 4000);
            test.add(fb,
                     TransactionQueue::AddResult::ADD_STATUS_TRY_AGAIN_LATER);

            test.check(
                {{{account1, 0, {txSeqA1S1, txSeqA1S2}}, {account2}}, {}});
        }
        SECTION("minSeqLedgerGap uses next ledgerSeq for validation")
        {
            TransactionQueueTest test{*app};
            test.add(txSeqA1S1,
                     TransactionQueue::AddResult::ADD_STATUS_PENDING);
            test.add(txSeqA1S2,
                     TransactionQueue::AddResult::ADD_STATUS_PENDING);
            test.add(txSeqA1S3MinSeqLedgerGap,
                     TransactionQueue::AddResult::ADD_STATUS_TRY_AGAIN_LATER);
        }
        SECTION("first tx has minSeqAge set")
        {
            auto lastCloseTime = app->getLedgerManager()
                                     .getLastClosedLedgerHeader()
                                     .header.scpValue.closeTime;

            auto nextCloseTime = lastCloseTime + 100;
            auto lclNum = app->getLedgerManager().getLastClosedLedgerNum();

            PreconditionsV2 cond;
            cond.minSeqAge = 100;
            auto txPass =
                transactionWithV2Precondition(*app, account1, 1, 200, cond);

            ++cond.minSeqAge;
            auto txFail =
                transactionWithV2Precondition(*app, account1, 1, 200, cond);

            closeLedgerOn(*app, lclNum + 1, nextCloseTime);

            TransactionQueueTest test{*app};
            test.add(txFail, TransactionQueue::AddResult::ADD_STATUS_ERROR);
            test.add(txPass, TransactionQueue::AddResult::ADD_STATUS_PENDING);
            test.add(txSeqA1S2,
                     TransactionQueue::AddResult::ADD_STATUS_PENDING);
            test.check({{{account1, 0, {txPass, txSeqA1S2}}, {account2}}, {}});
        }
        SECTION("first tx has minSeqLedgerGap set")
        {
            auto lastCloseTime = app->getLedgerManager()
                                     .getLastClosedLedgerHeader()
                                     .header.scpValue.closeTime;

            auto lclNum = app->getLedgerManager().getLastClosedLedgerNum();

            PreconditionsV2 cond;
            cond.minSeqLedgerGap = 3;
            auto txPass =
                transactionWithV2Precondition(*app, account1, 1, 200, cond);

            ++cond.minSeqLedgerGap;
            auto txFail =
                transactionWithV2Precondition(*app, account1, 1, 200, cond);

            closeLedgerOn(*app, lclNum + 1, lastCloseTime);
            closeLedgerOn(*app, lclNum + 2, lastCloseTime);

            TransactionQueueTest test{*app};
            test.add(txFail, TransactionQueue::AddResult::ADD_STATUS_ERROR);
            test.add(txPass, TransactionQueue::AddResult::ADD_STATUS_PENDING);
            test.add(txSeqA1S2,
                     TransactionQueue::AddResult::ADD_STATUS_PENDING);
            test.check({{{account1, 0, {txPass, txSeqA1S2}}, {account2}}, {}});
        }
        SECTION("extra signer")
        {
            TransactionQueueTest test{*app};

            SignerKey a2;
            a2.type(SIGNER_KEY_TYPE_ED25519);
            a2.ed25519() = account2.getPublicKey().ed25519();

            PreconditionsV2 cond;
            cond.extraSigners.emplace_back(a2);

            SECTION("one signer")
            {
                auto tx =
                    transactionWithV2Precondition(*app, account1, 1, 200, cond);
                test.add(tx, TransactionQueue::AddResult::ADD_STATUS_ERROR);

                tx->addSignature(account2.getSecretKey());
                test.add(tx, TransactionQueue::AddResult::ADD_STATUS_PENDING);
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
                test.add(tx, TransactionQueue::AddResult::ADD_STATUS_ERROR);

                SECTION("first signature missing")
                {
                    tx->addSignature(root.getSecretKey());
                    test.add(tx, TransactionQueue::AddResult::ADD_STATUS_ERROR);

                    tx->addSignature(account2.getSecretKey());
                    test.add(tx,
                             TransactionQueue::AddResult::ADD_STATUS_PENDING);
                }

                SECTION("second signature missing")
                {
                    tx->addSignature(account2.getSecretKey());
                    test.add(tx, TransactionQueue::AddResult::ADD_STATUS_ERROR);

                    tx->addSignature(root.getSecretKey());
                    test.add(tx,
                             TransactionQueue::AddResult::ADD_STATUS_PENDING);
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

            auto tx =
                transactionWithV2Precondition(*app, account1, 1, 200, cond);

            auto& herder = static_cast<HerderImpl&>(app->getHerder());
            auto& tq = herder.getTransactionQueue();

            REQUIRE(herder.recvTransaction(tx, false) ==
                    TransactionQueue::AddResult::ADD_STATUS_PENDING);

            REQUIRE(tq.getTransactions({}).size() == 1);
            closeLedger(*app);
            REQUIRE(tq.getTransactions({}).size() == 0);
            REQUIRE(tq.isBanned(tx->getFullHash()));
        }
    });
}

TEST_CASE("TransactionQueue limits", "[herder][transactionqueue]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 4;
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

    SECTION("simple limit")
    {
        auto simpleLimitTest = [&](std::function<uint32(int)> fee,
                                   bool belowFee) {
            TransactionQueueTest test{*app};
            auto minFee = fee(1);
            auto txSeqA3T1 = transaction(*app, account3, 1, minFee, 100);

            test.add(txSeqA3T1,
                     TransactionQueue::AddResult::ADD_STATUS_PENDING);
            std::vector<TransactionFrameBasePtr> a3Txs({txSeqA3T1});
            std::vector<TransactionFrameBasePtr> banned;

            for (int i = 2; i <= 10; i++)
            {
                auto txFee = fee(i);
                if (i == 10)
                {
                    txFee *= 100;
                }
                auto txSeqA3Ti = transaction(*app, account3, i, 1, txFee);
                if (i <= 8)
                {
                    test.add(txSeqA3Ti,
                             TransactionQueue::AddResult::ADD_STATUS_PENDING);
                    a3Txs.emplace_back(txSeqA3Ti);
                    minFee = std::min(minFee, txFee);
                }
                else
                {
                    if (i == 9 && belowFee)
                    {
                        // below fee requirement
                        test.add(txSeqA3Ti,
                                 TransactionQueue::AddResult::ADD_STATUS_ERROR);
                        REQUIRE(txSeqA3Ti->getResult().feeCharged ==
                                (minFee + 1));
                    }
                    else // if (i == 10)
                    {
                        // would need to kick out own transaction
                        test.add(txSeqA3Ti, TransactionQueue::AddResult::
                                                ADD_STATUS_TRY_AGAIN_LATER);
                    }
                    banned.emplace_back(txSeqA3Ti);
                    test.check({{{account3, 0, a3Txs}}, {banned}});
                }
            }
        };
        SECTION("constant fee")
        {
            simpleLimitTest([](int) { return 100; }, true);
        }
        SECTION("fee increasing")
        {
            simpleLimitTest([](int i) { return 100 * i; }, false);
        }
        SECTION("fee decreasing")
        {
            simpleLimitTest([](int i) { return 100 * (100 - i); }, false);
        }
    }
    SECTION("multi accounts limits")
    {
        TxQueueLimiter limiter(3, *app);

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
                    auto tx = transaction(*app, e.account, seq++, 1,
                                          opsFee.second, opsFee.first);
                    std::vector<std::pair<TxStackPtr, bool>> txsToEvict;
                    bool can = limiter.canAddTx(tx, noTx, txsToEvict).first;
                    REQUIRE(can);
                    REQUIRE(txsToEvict.empty());
                    limiter.addTransaction(tx);
                    txs.emplace_back(tx);
                }
            }
            REQUIRE(limiter.size() == 11);
        };
        // act \ base fee   400 300 200  100
        //  1                2   1    0   0
        //  2                3   1    0   0
        //  3                0   1    1   0
        //  4                0   0    1   0
        //  5                0   0    0   1
        // total             5   3    2   1 --> 11 (free = 1)
        setup({{account1, 1, {{1, 400}, {1, 300}, {1, 400}}},
               {account2, 1, {{1, 400}, {1, 300}, {2, 400}}},
               {account3, 1, {{1, 300}, {1, 200}}},
               {account4, 1, {{1, 200}}},
               {account5, 1, {{1, 100}}}});
        auto checkAndAddTx = [&](bool expected, TestAccount& account, int ops,
                                 int fee, int64 expFeeOnFailed,
                                 int expEvictedOpsOnSuccess) {
            auto tx = transaction(*app, account, 1000, 1, fee, ops);
            std::vector<std::pair<TxStackPtr, bool>> txsToEvict;
            auto can = limiter.canAddTx(tx, noTx, txsToEvict);
            REQUIRE(expected == can.first);
            if (can.first)
            {
                int evictedOps = 0;
                limiter.evictTransactions(
                    txsToEvict, *tx, [&](TransactionFrameBasePtr const& evict) {
                        // can't evict cheaper transactions
                        auto cmp3 = feeRate3WayCompare(
                            evict->getFeeBid(), evict->getNumOperations(),
                            tx->getFeeBid(), tx->getNumOperations());
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
            std::vector<std::pair<TxStackPtr, bool>> txsToEvict;
            // 0 fee is a special case as transaction shouldn't have 0 fee.
            // Hence we only check that fee of 1 allows transaction to be added.
            if (minFee == 0)
            {
                REQUIRE(limiter
                            .canAddTx(transaction(*app, account1, 1000, 1, 1),
                                      noTx, txsToEvict)
                            .first);
                return;
            }
            auto feeTx = transaction(*app, account1, 1000, 1, minFee);
            auto [canAdd, feeNeeded] =
                limiter.canAddTx(feeTx, noTx, txsToEvict);
            REQUIRE(canAdd == false);
            REQUIRE(feeNeeded == minFee + 1);

            auto increasedFeeTx =
                transaction(*app, account1, 1000, 1, minFee + 1);
            REQUIRE(limiter.canAddTx(increasedFeeTx, noTx, txsToEvict).first);
        };

        SECTION("evict nothing")
        {
            checkTxBoundary(account1, 1, 100, 0);
            // can't evict transaction with the same base fee
            checkAndAddTx(false, account1, 2, 100 * 2, 2 * 100 + 1, 0);
        }
        SECTION("evict 100s")
        {
            checkTxBoundary(account1, 2, 200, 1);
        }
        SECTION("evict 100s and 200s")
        {
            checkTxBoundary(account6, 6, 300, 5);
            checkMinFeeToFitWithNoEvict(200);
            SECTION("and add empty tx")
            {
                // Empty transaction can be added from the limiter standpoint
                // (as it contains 0 ops and cannot exceed the operation limits)
                // and hence should be rejected by the validation logic.
                checkAndAddTx(true, account1, 0, 100, 0, 0);
            }
        }
        SECTION("evict 100s and 200s, can't evict self")
        {
            checkAndAddTx(false, account2, 6, 6 * 300, 0, 0);
        }
        SECTION("evict all")
        {
            checkAndAddTx(true, account6, 12, 12 * 500, 0, 11);
            checkMinFeeToFitWithNoEvict(400);
            limiter.resetEvictionState();
            checkMinFeeToFitWithNoEvict(0);
        }
        SECTION("enforce limit")
        {
            checkMinFeeToFitWithNoEvict(0);
            checkAndAddTx(true, account1, 2, 2 * 200, 0, 1);
            // at this point as a transaction of base fee 100 was evicted
            // no transactions of base fee 100 can be accepted
            checkMinFeeToFitWithNoEvict(100);
            // evict some more (300s)
            checkAndAddTx(true, account6, 8, 300 * 8 + 1, 0, 6);
            checkMinFeeToFitWithNoEvict(300);

            // now, reset the min fee requirement
            limiter.resetEvictionState();
            checkMinFeeToFitWithNoEvict(0);
        }
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

    // 3 * 3 = 9 operations limit, 3 * 1 = 3 DEX operations limit.
    TxQueueLimiter limiter(3, *app);

    std::vector<TransactionFrameBasePtr> txs;

    TransactionFrameBasePtr noTx;

    auto checkAndAddTx = [&](TestAccount& account, bool isDex, int ops, int fee,
                             bool expected, int64 expFeeOnFailed,
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
        std::vector<std::pair<TxStackPtr, bool>> txsToEvict;
        auto can = limiter.canAddTx(tx, noTx, txsToEvict);
        REQUIRE(can.first == expected);
        if (can.first)
        {
            int evictedOps = 0;
            limiter.evictTransactions(
                txsToEvict, *tx, [&](TransactionFrameBasePtr const& evict) {
                    // can't evict cheaper transactions (
                    // evict.bid/evict.ops < tx->bid/tx->ops)
                    REQUIRE(bigMultiply(evict->getFeeBid(),
                                        tx->getNumOperations()) <
                            bigMultiply(tx->getFeeBid(),
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
                                           int ops, int opBid,
                                           int expectedEvicted) {
        checkAndAddTx(account, isDex, ops, ops * opBid, false, opBid * ops + 1,
                      0);
        checkAndAddTx(account, isDex, ops, ops * opBid + 1, true, 0,
                      expectedEvicted);
    };

    SECTION("non-DEX transactions only")
    {
        // Fill capacity of 9 ops
        checkAndAddTx(account2, false, 5, 300 * 5, true, 0, 0);
        checkAndAddTx(account2, false, 1, 400, true, 0, 0);
        checkAndAddTx(account1, false, 1, 100, true, 0, 0);
        checkAndAddTx(account1, false, 2, 200 * 2, true, 0, 0);

        // Cannot add transactions anymore without eviction.
        checkAndAddTx(account2, false, 1, 100, false, 101, 0);
        // Evict transactions with high enough bid.
        checkAndAddTx(account2, false, 2, 2 * 200 + 1, true, 0, 3);
    }
    SECTION("DEX transactions only")
    {
        // Fill DEX capacity of 3 ops
        checkAndAddTx(account1, true, 1, 100, true, 0, 0);
        checkAndAddTx(account1, true, 2, 200, true, 0, 0);

        // Cannot add DEX transactions anymore without eviction.
        checkAndAddTx(account2, true, 1, 100, false, 101, 0);
        // Evict DEX transactions with high enough bid.
        checkAndAddTx(account2, true, 3, 3 * 200 + 1, true, 0, 3);
    }
    SECTION("DEX and non-DEX transactions")
    {
        // 3 DEX ops (bid 200)
        checkAndAddTx(account1, true, 3, 200 * 3, true, 0, 0);

        // 1 non-DEX op (bid 100) - fits
        checkAndAddTx(account1, false, 1, 100, true, 0, 0);
        // 1 DEX op (bid 100) - doesn't fit
        checkAndAddTx(account1, true, 1, 100, false, 201, 0);

        // 7 non-DEX ops (bid 200/op + 1) - evict all DEX and non-DEX txs.
        checkAndAddTx(account2, false, 7, 200 * 7 + 1, true, 0, 4);

        // 1 DEX op - while it fits, 200 bid is not enough (as we evicted tx
        // with 200 DEX bid).
        checkAndAddWithIncreasedBid(account1, true, 1, 200, 0);

        // 1 non-DEX op - while it fits, 200 bid is not enough (as we evicted
        // DEX tx with 200 bid before reaching the DEX ops limit).
        checkAndAddWithIncreasedBid(account1, false, 1, 200, 0);
    }

    SECTION("DEX and non-DEX transactions with DEX limit reached")
    {
        // 2 DEX ops (bid 200/op)
        checkAndAddTx(account1, true, 2, 200 * 2, true, 0, 0);

        // 3 non-DEX ops (bid 100/op) - fits
        checkAndAddTx(account1, false, 3, 100 * 3, true, 0, 0);
        // 2 DEX ops (bid 300/op) - fits and evicts the previous DEX tx
        checkAndAddTx(account2, true, 2, 300 * 2, true, 0, 2);

        // 5 non-DEX ops (bid 250/op) - evict non-DEX tx.
        checkAndAddTx(account2, false, 5, 250 * 5, true, 0, 3);

        // 1 DEX op - while it fits, 200 bid is not enough (as we evicted tx
        // with 200 DEX bid).
        checkAndAddWithIncreasedBid(account1, true, 1, 200, 0);

        // 1 non-DEX op - while it fits, 100 bid is not enough (as we evicted
        // non-DEX tx with bid 100, but DEX tx was evicted due to DEX limit).
        checkAndAddWithIncreasedBid(account1, false, 1, 100, 0);
    }

    SECTION("non-DEX transactions evict DEX transactions")
    {
        // Add 9 ops (2 + 1 DEX, 3 + 2 + 1 non-DEX)
        checkAndAddTx(account1, true, 2, 100 * 2, true, 0, 0);
        checkAndAddTx(account1, false, 3, 200 * 3, true, 0, 0);
        checkAndAddTx(account1, true, 1, 300, true, 0, 0);
        checkAndAddTx(account1, false, 2, 400 * 2, true, 0, 0);
        checkAndAddTx(account1, false, 1, 500, true, 0, 0);

        // Evict 2 DEX ops and 3 non-DEX ops.
        checkAndAddWithIncreasedBid(account2, false, 5, 200, 5);
    }

    SECTION("DEX transactions evict non-DEX transactions in DEX slots")
    {
        SECTION("evict only due to global limit")
        {
            // 1 DEX op + 8 non-DEX ops (2 ops in DEX slots).
            checkAndAddTx(account1, true, 1, 200, true, 0, 0);
            checkAndAddTx(account1, false, 6, 400 * 6, true, 0, 0);
            checkAndAddTx(account1, false, 1, 100, true, 0, 0);
            checkAndAddTx(account1, false, 1, 300, true, 0, 0);

            // Evict 1 DEX op and 100/300 non-DEX ops (bids strictly increase)
            checkAndAddWithIncreasedBid(account2, true, 3, 300, 3);
        }
        SECTION("evict due to both global and DEX limits")
        {
            // 2 DEX ops + 7 non-DEX ops (1 op in DEX slots).
            checkAndAddTx(account1, true, 2, 200 * 2, true, 0, 0);
            checkAndAddTx(account1, false, 5, 400 * 6, true, 0, 0);
            checkAndAddTx(account1, false, 1, 100, true, 0, 0);
            checkAndAddTx(account1, false, 1, 150, true, 0, 0);

            SECTION("fill all DEX slots")
            {
                // Evict 2 DEX ops and bid 100 non-DEX op (skip non-DEX 150 bid)
                checkAndAddWithIncreasedBid(account2, true, 3, 200, 3);
            }
            SECTION("fill part of DEX slots")
            {
                // Evict 2 DEX ops and bid 100 non-DEX op (skip non-DEX 150 bid)
                checkAndAddWithIncreasedBid(account2, true, 2, 200, 3);

                SECTION("and add non-DEX tx")
                {
                    // Add a fitting non-DEX tx with at least 100 + 1 bid to
                    // beat the evicted non-DEX tx.
                    checkAndAddWithIncreasedBid(account2, false, 1, 100, 0);
                }
                SECTION("and add DEX tx")
                {
                    // Add a fitting non-DEX tx with at least 200 + 1 bid to
                    // beat the evicted DEX tx.
                    checkAndAddWithIncreasedBid(account2, true, 1, 200, 0);
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
            checkAndAddTx(account2, true, 3, 300 * 3, true, 0, 3);
        }
        SECTION("but evict non-DEX transaction from a different account")
        {
            checkAndAddTx(account1, false, 4, 300 * 4, true, 0, 6);
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

        TransactionQueue tq(*app, 4, 10, 4);
        REQUIRE(tq.tryAdd(transaction(*app, acc1, 1, 1, 100), false) ==
                TransactionQueue::AddResult::ADD_STATUS_PENDING);

        auto checkTxSet = [&](uint32_t ledgerSeq) {
            auto lcl = app->getLedgerManager().getLastClosedLedgerHeader();
            lcl.header.ledgerSeq = ledgerSeq;
            return !tq.getTransactions(lcl.header).empty();
        };

        REQUIRE(checkTxSet(2));
        REQUIRE(!checkTxSet(3));
        REQUIRE(checkTxSet(4));
    }

    SECTION("check a chain of transactions")
    {
        int64_t startingSeq = static_cast<int64_t>(nextLedgerSeq) << 32;
        REQUIRE(acc1.loadSequenceNumber() < startingSeq);
        acc1.bumpSequence(startingSeq - 3);
        REQUIRE(acc1.loadSequenceNumber() == startingSeq - 3);

        TransactionQueue tq(*app, 4, 10, 4);
        for (size_t i = 1; i <= 4; ++i)
        {
            REQUIRE(tq.tryAdd(transaction(*app, acc1, i, 1, 100), false) ==
                    TransactionQueue::AddResult::ADD_STATUS_PENDING);
        }

        auto checkTxSet = [&](uint32_t ledgerSeq, size_t size) {
            auto lcl = app->getLedgerManager().getLastClosedLedgerHeader();
            lcl.header.ledgerSeq = ledgerSeq;
            auto txSet = tq.getTransactions(lcl.header);
            REQUIRE(txSet.size() == size);
            for (size_t i = 1; i <= size; ++i)
            {
                REQUIRE(txSet[i - 1]->getSeqNum() ==
                        static_cast<int64_t>(startingSeq - 3 + i));
            }
        };

        checkTxSet(2, 4);
        checkTxSet(3, 2);
        checkTxSet(4, 4);
    }
}

TEST_CASE("transaction queue with fee-bump", "[herder][transactionqueue]")
{
    VirtualClock clock;
    auto cfg = getTestConfig();
    cfg.FLOOD_TX_PERIOD_MS = 100;
    auto app = createTestApplication(clock, cfg);
    auto const minBalance0 = app->getLedgerManager().getLastMinBalance(0);
    auto const minBalance2 = app->getLedgerManager().getLastMinBalance(2);

    auto root = TestAccount::createRoot(*app);
    auto account1 = root.create("a1", minBalance2);
    auto account2 = root.create("a2", minBalance2);
    auto account3 = root.create("a3", minBalance2);

    SECTION("1 fee bump, fee source same as source")
    {
        TransactionQueueTest test{*app};
        auto tx = transaction(*app, account1, 1, 1, 100);
        auto fb = feeBump(*app, account1, tx, 200);
        test.add(fb, TransactionQueue::AddResult::ADD_STATUS_PENDING);

        for (uint32 i = 0; i <= 3; ++i)
        {
            test.check({{{account1, i, {fb}}, {account2}, {account3}}, {}});
            test.shift();
        }
        test.check({{{account1}, {account2}, {account3}}, {{fb}}});
    }

    SECTION("1 fee bump, fee source distinct from source")
    {
        TransactionQueueTest test{*app};
        auto tx = transaction(*app, account1, 1, 1, 100);
        auto fb = feeBump(*app, account2, tx, 200);
        test.add(fb, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {fb}}, {account2, 0}}, {}});

        for (uint32 i = 1; i <= 3; ++i)
        {
            test.shift();
            test.check({{{account1, i, {fb}}, {account2, 0}, {account3}}, {}});
        }
        test.shift();
        test.check({{{account1}, {account2}, {account3}}, {{fb}}});
    }

    SECTION("2 fee bumps with same fee source but different source, "
            "fee source distinct from source")
    {
        TransactionQueueTest test{*app};
        auto tx1 = transaction(*app, account1, 1, 1, 100);
        auto fb1 = feeBump(*app, account3, tx1, 200);
        test.add(fb1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {fb1}}, {account2}, {account3, 0}}, {}});

        test.shift();
        test.check({{{account1, 1, {fb1}}, {account2}, {account3, 0}}, {}});

        auto tx2 = transaction(*app, account2, 1, 1, 100);
        auto fb2 = feeBump(*app, account3, tx2, 200);
        test.add(fb2, TransactionQueue::AddResult::ADD_STATUS_PENDING);

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
        TransactionQueueTest test{*app};
        auto tx1 = transaction(*app, account1, 1, 1, 100);
        auto fb1 = feeBump(*app, account3, tx1, 200);
        test.add(fb1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {fb1}}, {account2}, {account3, 0}}, {}});

        test.shift();
        test.check({{{account1, 1, {fb1}}, {account2}, {account3, 0}}, {}});

        auto tx2 = transaction(*app, account3, 1, 1, 100);
        test.add(tx2, TransactionQueue::AddResult::ADD_STATUS_PENDING);

        for (uint32 i = 1; i <= 3; ++i)
        {
            test.check(
                {{{account1, i, {fb1}}, {account2}, {account3, i - 1, {tx2}}},
                 {}});
            test.shift();
        }
        test.check({{{account1}, {account2}, {account3, 3, {tx2}}}, {{fb1}}});
        test.shift();
        test.check({{{account1}, {account2}, {account3}}, {{tx2}, {fb1}}});
    }

    SECTION("1 fee bump and 1 transaction with same fee source, "
            "fee source distinct from source, fee bump second")
    {
        TransactionQueueTest test{*app};
        auto tx1 = transaction(*app, account3, 1, 1, 100);
        test.add(tx1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1}, {account2}, {account3, 0, {tx1}}}, {}});

        test.shift();
        test.check({{{account1}, {account2}, {account3, 1, {tx1}}}, {}});

        auto tx2 = transaction(*app, account1, 1, 1, 100);
        auto fb2 = feeBump(*app, account3, tx2, 200);
        test.add(fb2, TransactionQueue::AddResult::ADD_STATUS_PENDING);

        for (uint32 i = 1; i <= 3; ++i)
        {
            test.check(
                {{{account1, i - 1, {fb2}}, {account2}, {account3, i, {tx1}}},
                 {}});
            test.shift();
        }
        test.check(
            {{{account1, 3, {fb2}}, {account2}, {account3, 0}}, {{tx1}}});
        test.shift();
        test.check({{{account1}, {account2}, {account3}}, {{fb2}, {tx1}}});
    }

    SECTION("ban or remove, two fee bumps with same fee source and source, "
            "fee source same as source")
    {
        TransactionQueueTest test{*app};
        auto tx1 = transaction(*app, account1, 1, 1, 100);
        auto fb1 = feeBump(*app, account1, tx1, 200);
        test.add(fb1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {fb1}}, {account2}, {account3}}, {}});

        auto tx2 = transaction(*app, account1, 2, 1, 100);
        auto fb2 = feeBump(*app, account1, tx2, 200);
        test.add(fb2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {fb1, fb2}}, {account2}, {account3}}, {}});

        test.shift();
        test.check({{{account1, 1, {fb1, fb2}}, {account2}, {account3}}, {}});

        SECTION("ban first")
        {
            test.ban({fb1});
            test.check({{{account1}, {account2}, {account3}}, {{fb1, fb2}}});
        }
        SECTION("ban second")
        {
            test.ban({fb2});
            test.check(
                {{{account1, 1, {fb1}}, {account2}, {account3}}, {{fb2}}});
        }

        SECTION("remove first")
        {
            test.removeApplied({fb1});
            test.check(
                {{{account1, 0, {fb2}}, {account2}, {account3}}, {{fb1}, {}}});
        }
        SECTION("remove second")
        {
            test.removeApplied({fb1, fb2});
            test.check(
                {{{account1}, {account2}, {account3}}, {{fb1, fb2}, {}}});
        }
    }

    SECTION("ban first of two fee bumps with same fee source and source, "
            "fee source disinct from source")
    {
        TransactionQueueTest test{*app};
        auto tx1 = transaction(*app, account1, 1, 1, 100);
        auto fb1 = feeBump(*app, account3, tx1, 200);
        test.add(fb1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {fb1}}, {account2}, {account3, 0}}, {}});

        auto tx2 = transaction(*app, account1, 2, 1, 100);
        auto fb2 = feeBump(*app, account3, tx2, 200);
        test.add(fb2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check(
            {{{account1, 0, {fb1, fb2}}, {account2}, {account3, 0}}, {}});

        test.shift();
        test.check({{{account1, 1, {fb1, fb2}}, {account2}, {account3}}, {}});

        SECTION("ban first")
        {
            test.ban({fb1});
            test.check({{{account1}, {account2}, {account3}}, {{fb1, fb2}}});
        }
        SECTION("ban second")
        {
            test.ban({fb2});
            test.check(
                {{{account1, 1, {fb1}}, {account2}, {account3, 0}}, {{fb2}}});
        }

        SECTION("remove first")
        {
            test.removeApplied({fb1});
            test.check(
                {{{account1, 0, {fb2}}, {account2}, {account3}}, {{fb1}, {}}});
        }
        SECTION("remove second")
        {
            test.removeApplied({fb1, fb2});
            test.check(
                {{{account1}, {account2}, {account3}}, {{fb1, fb2}, {}}});
        }
    }

    SECTION("add transaction, fee source has insufficient balance due to fee "
            "bumps")
    {
        TransactionQueueTest test{*app};
        auto tx1 = transaction(*app, account1, 1, 1, 100);
        auto fb1 = feeBump(*app, account3, tx1, minBalance2 - minBalance0 - 1);
        test.add(fb1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {fb1}}, {account2}, {account3, 0}}, {}});

        SECTION("transaction")
        {
            auto tx2 = transaction(*app, account3, 1, 1, 100);
            test.add(tx2, TransactionQueue::AddResult::ADD_STATUS_ERROR);
            REQUIRE(tx2->getResultCode() == txINSUFFICIENT_BALANCE);
            test.check({{{account1, 0, {fb1}}, {account2}, {account3, 0}}, {}});
        }

        SECTION("fee bump with fee source same as source")
        {
            auto tx2 = transaction(*app, account3, 1, 1, 100);
            auto fb2 = feeBump(*app, account3, tx2, 200);
            test.add(fb2, TransactionQueue::AddResult::ADD_STATUS_ERROR);
            REQUIRE(fb2->getResultCode() == txINSUFFICIENT_BALANCE);
            test.check({{{account1, 0, {fb1}}, {account2}, {account3, 0}}, {}});
        }

        SECTION("fee bump with fee source distinct from source")
        {
            auto tx2 = transaction(*app, account2, 1, 1, 100);
            auto fb2 = feeBump(*app, account3, tx2, 200);
            test.add(fb2, TransactionQueue::AddResult::ADD_STATUS_ERROR);
            REQUIRE(fb2->getResultCode() == txINSUFFICIENT_BALANCE);
            test.check({{{account1, 0, {fb1}}, {account2}, {account3, 0}}, {}});
        }
    }

    SECTION("transaction or fee bump duplicates fee bump")
    {
        TransactionQueueTest test{*app};
        auto tx1 = transaction(*app, account1, 1, 1, 100);
        auto fb1 = feeBump(*app, account3, tx1, minBalance2 - minBalance0 - 1);
        test.add(fb1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {fb1}}, {account2}, {account3, 0}}, {}});
        test.add(fb1, TransactionQueue::AddResult::ADD_STATUS_DUPLICATE);
        test.check({{{account1, 0, {fb1}}, {account2}, {account3, 0}}, {}});
        test.add(tx1, TransactionQueue::AddResult::ADD_STATUS_DUPLICATE);
        test.check({{{account1, 0, {fb1}}, {account2}, {account3, 0}}, {}});
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

    auto setupTransactions = [&](TransactionQueueTest& test) {
        std::vector<TransactionFrameBasePtr> txs;
        for (uint32_t i = 1; i <= 3; ++i)
        {
            txs.emplace_back(transaction(*app, account1, i, 1, 200));
            test.add(txs.back(),
                     TransactionQueue::AddResult::ADD_STATUS_PENDING);
        }
        return txs;
    };

    auto setupFeeBumps = [&](TransactionQueueTest& test,
                             TestAccount& feeSource) {
        std::vector<TransactionFrameBasePtr> txs;
        for (uint32_t i = 1; i <= 3; ++i)
        {
            auto tx = transaction(*app, account1, i, 1, 100);
            auto fb = feeBump(*app, feeSource, tx, 400);
            txs.emplace_back(fb);
            test.add(txs.back(),
                     TransactionQueue::AddResult::ADD_STATUS_PENDING);
        }
        return txs;
    };

    auto submitTransactions = [&](TransactionQueueTest& test,
                                  std::vector<TransactionFrameBasePtr> txs) {
        SECTION("lower fee")
        {
            for (uint32_t i = 1; i <= 3; ++i)
            {
                test.add(transaction(*app, account1, i, 1, 199),
                         TransactionQueue::AddResult::ADD_STATUS_ERROR);
                test.check({{{account1, 0, txs}, {account2}}, {}});
            }
        }

        SECTION("higher fee below threshold")
        {
            for (uint32_t i = 1; i <= 3; ++i)
            {
                test.add(transaction(*app, account1, i, 1, 1999),
                         TransactionQueue::AddResult::ADD_STATUS_ERROR);
                test.check({{{account1, 0, txs}, {account2}}, {}});
            }
        }

        SECTION("higher fee at threshold")
        {
            std::vector<std::string> position{"first", "middle", "last"};
            for (uint32_t i = 1; i <= 3; ++i)
            {
                SECTION(position[i - 1] + " transaction")
                {
                    test.add(transaction(*app, account1, i, 1, 2000),
                             TransactionQueue::AddResult::ADD_STATUS_ERROR);
                    test.check({{{account1, 0, txs}, {account2}}, {}});
                }
            }
        }
    };

    auto submitFeeBumps = [&](TransactionQueueTest& test,
                              std::vector<TransactionFrameBasePtr> txs) {
        SECTION("lower fee")
        {
            std::vector<TestAccount> accounts{account1, account2};
            for (auto& feeSource : accounts)
            {
                for (uint32_t i = 1; i <= 3; ++i)
                {
                    auto tx = transaction(*app, account1, i, 1, 100);
                    auto fb = feeBump(*app, feeSource, tx, 399);
                    test.add(fb, TransactionQueue::AddResult::ADD_STATUS_ERROR);
                    auto const& res = fb->getResult();
                    REQUIRE(res.result.code() == txINSUFFICIENT_FEE);
                    REQUIRE(res.feeCharged == 4000);
                    test.check({{{account1, 0, txs}, {account2}}, {}});
                }
            }
        }

        SECTION("higher fee below threshold")
        {
            std::vector<TestAccount> accounts{account1, account2};
            for (auto& feeSource : accounts)
            {
                for (uint32_t i = 1; i <= 3; ++i)
                {
                    auto tx = transaction(*app, account1, i, 1, 100);
                    auto fb = feeBump(*app, feeSource, tx, 3999);
                    test.add(fb, TransactionQueue::AddResult::ADD_STATUS_ERROR);
                    auto const& res = fb->getResult();
                    REQUIRE(res.result.code() == txINSUFFICIENT_FEE);
                    REQUIRE(res.feeCharged == 4000);
                    test.check({{{account1, 0, txs}, {account2}}, {}});
                }
            }
        }

        SECTION("higher fee at threshold")
        {
            std::vector<std::string> position{"first", "middle", "last"};
            for (uint32_t i = 1; i <= 3; ++i)
            {
                auto checkPos = [&](TestAccount& source) {
                    auto tx = transaction(*app, account1, i, 1, 100);
                    auto fb = feeBump(*app, source, tx, 4000);
                    txs[i - 1] = fb;
                    test.add(fb,
                             TransactionQueue::AddResult::ADD_STATUS_PENDING);
                    test.check({{{account1, 0, txs}, {account2}}, {}});
                };
                SECTION(position[i - 1] +
                        " transaction from same source account")
                {
                    checkPos(account1);
                }
                SECTION(position[i - 1] +
                        " transaction from different source account")
                {
                    checkPos(account2);
                }
            }
        }
    };

    SECTION("replace transaction with transaction")
    {
        TransactionQueueTest test{*app};
        auto txs = setupTransactions(test);
        submitTransactions(test, txs);
    }

    SECTION("replace transaction with fee-bump")
    {
        TransactionQueueTest test{*app};
        auto txs = setupTransactions(test);
        submitFeeBumps(test, txs);
    }

    SECTION("replace fee-bump having same source and fee-source with "
            "transaction")
    {
        TransactionQueueTest test{*app};
        auto txs = setupFeeBumps(test, account1);
        submitTransactions(test, txs);
    }

    SECTION("replace fee-bump having different source and fee-source with "
            "transaction")
    {
        TransactionQueueTest test{*app};
        auto txs = setupFeeBumps(test, account2);
        submitTransactions(test, txs);
    }

    SECTION("replace fee-bump having same source and fee-source with fee-bump")
    {
        TransactionQueueTest test{*app};
        auto txs = setupFeeBumps(test, account1);
        submitFeeBumps(test, txs);
    }

    SECTION("replace fee-bump having different source and fee-source with "
            "fee-bump")
    {
        TransactionQueueTest test{*app};
        auto txs = setupFeeBumps(test, account2);
        submitFeeBumps(test, txs);
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

    auto tx1a = root.tx({payment(root, 1)});
    root.loadSequenceNumber();
    auto tx1b = root.tx({payment(root, 2)});
    auto tx2 = root.tx({payment(root, 3)});
    auto tx3 = root.tx({payment(root, 4)});
    auto tx4 = root.tx({payment(root, 5)});

    herder.recvTransaction(tx1a, false);
    herder.recvTransaction(tx2, false);
    herder.recvTransaction(tx3, false);

    {
        auto const& lcl = lm.getLastClosedLedgerHeader();
        auto ledgerSeq = lcl.header.ledgerSeq + 1;

        root.loadSequenceNumber();
        auto txSet = TxSetFrame::makeFromTransactions({tx1b, tx2}, *app, 0, 0);
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
    REQUIRE(herder.recvTransaction(tx4, false) ==
            TransactionQueue::AddResult::ADD_STATUS_PENDING);
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
