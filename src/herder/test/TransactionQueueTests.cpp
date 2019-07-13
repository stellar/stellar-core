// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "herder/TransactionQueue.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "util/Timer.h"

#include <lib/catch.hpp>
#include <numeric>

using namespace stellar;
using namespace stellar::txtest;

namespace
{
TransactionFramePtr
transaction(Application& app, TestAccount& account, int sequenceDelta,
            int amount, int fee)
{
    return transactionFromOperations(
        app, account, account.getLastSequenceNumber() + sequenceDelta,
        {payment(account.getPublicKey(), amount)}, fee);
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
            int mAge;
            std::vector<TransactionFramePtr> mAccountTransactions;
        };

        struct BannedState
        {
            std::vector<TransactionFramePtr> mBanned0;
            std::vector<TransactionFramePtr> mBanned1;
        };

        std::vector<AccountState> mAccountStates;
        BannedState mBannedState;
    };

    explicit TransactionQueueTest(Application& app)
        : mTransactionQueue{app, 4, 2}
    {
    }

    void
    add(TransactionFramePtr const& tx, TransactionQueue::AddResult AddResult)
    {
        REQUIRE(mTransactionQueue.tryAdd(tx) == AddResult);
    }

    void
    removeAndReset(std::vector<TransactionFramePtr> const& toRemove)
    {
        auto size = mTransactionQueue.toTxSet({})->sizeTx();
        mTransactionQueue.removeAndReset(toRemove);
        REQUIRE(size - toRemove.size() >=
                mTransactionQueue.toTxSet({})->sizeTx());
    }

    void
    ban(std::vector<TransactionFramePtr> const& toRemove)
    {
        auto size = mTransactionQueue.toTxSet({})->sizeTx();
        mTransactionQueue.ban(toRemove);
        REQUIRE(size - toRemove.size() >=
                mTransactionQueue.toTxSet({})->sizeTx());
    }

    void
    shift()
    {
        mTransactionQueue.shift();
    }

    void
    check(const TransactionQueueState& state)
    {
        auto txSet = mTransactionQueue.toTxSet({});
        auto expectedTxSet = TxSetFrame{{}};
        for (auto const& accountState : state.mAccountStates)
        {
            auto& txs = accountState.mAccountTransactions;
            auto fees =
                std::accumulate(std::begin(txs), std::end(txs), 0,
                                [](int fee, TransactionFramePtr const& tx) {
                                    return fee + tx->getFeeBid();
                                });
            auto seqNum = txs.empty() ? 0 : txs.back()->getSeqNum();
            auto accountTransactionQueueInfo =
                mTransactionQueue.getAccountTransactionQueueInfo(
                    accountState.mAccountID);
            REQUIRE(accountTransactionQueueInfo.mTotalFees == fees);
            REQUIRE(accountTransactionQueueInfo.mMaxSeq == seqNum);
            REQUIRE(accountTransactionQueueInfo.mAge == accountState.mAge);

            for (auto& tx : accountState.mAccountTransactions)
            {
                expectedTxSet.add(tx);
            }
        }
        REQUIRE(txSet->sortForApply() == expectedTxSet.sortForApply());
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

TEST_CASE("TransactionQueue", "[herder][TransactionQueue]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
    auto const minBalance2 = app->getLedgerManager().getLastMinBalance(2);

    auto root = TestAccount::createRoot(*app);
    auto account1 = root.create("a1", minBalance2);
    auto account2 = root.create("a2", minBalance2);

    auto txSeqA1T0 = transaction(*app, account1, 0, 1, 100);
    auto txSeqA1T1 = transaction(*app, account1, 1, 1, 200);
    auto txSeqA1T2 = transaction(*app, account1, 2, 1, 300);
    auto txSeqA1T1V2 = transaction(*app, account1, 1, 2, 400);
    auto txSeqA1T2V2 = transaction(*app, account1, 2, 2, 500);
    auto txSeqA1T3 = transaction(*app, account1, 3, 1, 600);
    auto txSeqA1T4 = transaction(*app, account1, 4, 1, 700);
    auto txSeqA2T1 = transaction(*app, account2, 1, 1, 800);
    auto txSeqA2T2 = transaction(*app, account2, 2, 1, 900);

    SECTION("small sequence number")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T0, TransactionQueue::AddResult::ADD_STATUS_ERROR);
        test.check({{{account1}, {account2}}, {}});
    }

    SECTION("big sequence number")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T2, TransactionQueue::AddResult::ADD_STATUS_ERROR);
        test.check({{{account1}, {account2}}, {}});
    }

    SECTION("good sequence number")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}, {}});
    }

    SECTION("good sequence number, same twice")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}, {}});
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_DUPLICATE);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}, {}});
    }

    SECTION("good then big sequence number")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}, {}});
        test.add(txSeqA1T3, TransactionQueue::AddResult::ADD_STATUS_ERROR);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}, {}});
    }

    SECTION("good then good sequence number")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}, {}});
        test.add(txSeqA1T2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1, txSeqA1T2}}, {account2}}, {}});
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
        test.removeAndReset({txSeqA1T1, txSeqA2T2});
        test.check({{{account1, 0, {txSeqA1T2}}, {account2, 0, {txSeqA2T1}}}});
        test.removeAndReset({txSeqA1T2});
        test.check({{{account1}, {account2, 0, {txSeqA2T1}}}});
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2, 0, {txSeqA2T1}}}});
        test.add(txSeqA2T2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}},
                     {account2, 0, {txSeqA2T1, txSeqA2T2}}}});
        test.removeAndReset({txSeqA2T1});
        test.check({{{account1, 0, {txSeqA1T1}}, {account2, 0, {txSeqA2T2}}}});
        test.removeAndReset({txSeqA2T2});
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}});
        test.removeAndReset({txSeqA1T1});
        test.check({{{account1}, {account2}}});
    }

    SECTION("multiple good sequence numbers, different accounts, with ban")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.add(txSeqA2T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.add(txSeqA1T2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.add(txSeqA2T2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.shift();
        test.ban({txSeqA1T1, txSeqA2T2});
        test.check({{{account1}, {account2, 1, {txSeqA2T1}}},
                    {{txSeqA1T1, txSeqA1T2, txSeqA2T2}}});
        test.add(txSeqA1T1,
                 TransactionQueue::AddResult::ADD_STATUS_TRY_AGAIN_LATER);
        test.check({{{account1}, {account2, 1, {txSeqA2T1}}},
                    {{txSeqA1T1, txSeqA1T2, txSeqA2T2}}});
    }
}
