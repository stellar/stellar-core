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

using namespace stellar;
using namespace stellar::txtest;

namespace
{
TransactionFramePtr
transaction(Application& app, TestAccount& account, int sequenceDelta)
{
    return transactionFromOperations(
        app, account, account.getLastSequenceNumber() + sequenceDelta,
        {payment(account.getPublicKey(), 1)});
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
    explicit TransactionQueueTest(Application& app)
        : mTransactionQueue{app, 4, 2}
    {
    }

    void
    add(TransactionFramePtr const& tx, TransactionQueue::AddResult AddResult)
    {
        auto beforeInfo =
            mTransactionQueue.getAccountTransactionQueueInfo(tx->getSourceID());
        if (beforeInfo.mMaxSeq == 0)
        {
            REQUIRE(beforeInfo.mTotalFees == 0);
        }

        REQUIRE(mTransactionQueue.tryAdd(tx) == AddResult);
        auto afterInfo =
            mTransactionQueue.getAccountTransactionQueueInfo(tx->getSourceID());

        if (AddResult == TransactionQueue::AddResult::ADD_STATUS_PENDING)
        {
            REQUIRE(afterInfo.mMaxSeq == tx->getSeqNum());
            REQUIRE(afterInfo.mTotalFees == beforeInfo.mTotalFees + 100);
        }
        else
        {
            REQUIRE(afterInfo == beforeInfo);
        }
    }

    void
    remove(std::vector<TransactionFramePtr> const& toRemove)
    {
        auto size = mTransactionQueue.toTxSet({})->sizeTx();
        mTransactionQueue.remove(toRemove);
        REQUIRE(size - toRemove.size() ==
                mTransactionQueue.toTxSet({})->sizeTx());
    }

    void
    shift(std::vector<TransactionFramePtr> const& removed = {})
    {
        auto size = mTransactionQueue.toTxSet({})->sizeTx();
        auto removedFrom = std::map<AccountID, int>{};
        for (auto tx : removed)
        {
            removedFrom[tx->getSourceID()]++;
        }

        auto beforeInfo =
            std::map<AccountID, TransactionQueue::AccountTxQueueInfo>{};
        for (auto from : removedFrom)
        {
            beforeInfo[from.first] =
                mTransactionQueue.getAccountTransactionQueueInfo(from.first);
            REQUIRE(beforeInfo[from.first].mMaxSeq > 0);
        }

        mTransactionQueue.shift();
        for (auto from : removedFrom)
        {
            auto afterInfo =
                mTransactionQueue.getAccountTransactionQueueInfo(from.first);
            if (afterInfo.mMaxSeq == 0)
            {
                REQUIRE(afterInfo.mTotalFees == 0);
            }
            else
            {
                REQUIRE(afterInfo.mMaxSeq == beforeInfo[from.first].mMaxSeq);
                REQUIRE(afterInfo.mTotalFees ==
                        beforeInfo[from.first].mTotalFees - from.second * 100);
            }
        }
        REQUIRE(size - removed.size() ==
                mTransactionQueue.toTxSet({})->sizeTx());
    }

    void
    check(std::vector<TransactionFramePtr> const& expected = {},
          std::vector<TransactionFramePtr> const& banned = {})
    {
        auto txSet = mTransactionQueue.toTxSet({});
        auto expectedTxSet = TxSetFrame{{}};
        for (auto const& tx : expected)
        {
            expectedTxSet.add(tx);
        }

        REQUIRE(txSet->sortForApply() == expectedTxSet.sortForApply());

        REQUIRE(mTransactionQueue.countBanned(0) +
                    mTransactionQueue.countBanned(1) ==
                banned.size());
        for (auto const& tx : banned)
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

    auto txSeqA1T0 = transaction(*app, account1, 0);
    auto txSeqA1T1 = transaction(*app, account1, 1);
    auto txSeqA1T2 = transaction(*app, account1, 2);
    auto txSeqA1T3 = transaction(*app, account1, 3);
    auto txSeqA1T4 = transaction(*app, account1, 4);
    auto txSeqA2T1 = transaction(*app, account2, 1);
    auto txSeqA2T2 = transaction(*app, account2, 2);

    SECTION("small sequence number")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T0, TransactionQueue::AddResult::ADD_STATUS_ERROR);
        test.check();
    }

    SECTION("big sequence number")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T2, TransactionQueue::AddResult::ADD_STATUS_ERROR);
        test.check();
    }

    SECTION("good sequence number")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({txSeqA1T1});
    }

    SECTION("good sequence number, same twice")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_DUPLICATE);
        test.check({txSeqA1T1});
    }

    SECTION("good then big sequence number")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.add(txSeqA1T3, TransactionQueue::AddResult::ADD_STATUS_ERROR);
        test.check({txSeqA1T1});
    }

    SECTION("good then good sequence number")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.add(txSeqA1T2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({txSeqA1T1, txSeqA1T2});
    }

    SECTION("good sequence number, same twice with shift")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.shift();
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_DUPLICATE);
        test.check({txSeqA1T1});
    }

    SECTION("good then big sequence number, with shift")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.shift();
        test.add(txSeqA1T3, TransactionQueue::AddResult::ADD_STATUS_ERROR);
        test.check({txSeqA1T1});
    }

    SECTION("good then good sequence number, with shift")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.shift();
        test.add(txSeqA1T2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({txSeqA1T1, txSeqA1T2});
    }

    SECTION("good sequence number, same twice with double shift")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.shift();
        test.shift();
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_DUPLICATE);
        test.check({txSeqA1T1});
    }

    SECTION("good then big sequence number, with double shift")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.shift();
        test.shift();
        test.add(txSeqA1T3, TransactionQueue::AddResult::ADD_STATUS_ERROR);
        test.check({txSeqA1T1});
    }

    SECTION("good then good sequence number, with double shift")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.shift();
        test.shift();
        test.add(txSeqA1T2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({txSeqA1T1, txSeqA1T2});
    }

    SECTION("good sequence number, same twice with four shifts, then two more")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.shift();
        test.shift();
        test.shift();
        test.shift({txSeqA1T1});
        test.check({}, {txSeqA1T1});
        test.add(txSeqA1T1,
                 TransactionQueue::AddResult::ADD_STATUS_TRY_AGAIN_LATER);
        test.check({}, {txSeqA1T1});
        test.shift();
        test.shift();
        test.check({}, {});
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({txSeqA1T1});
    }

    SECTION("good then big sequence number, with four shifts")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.shift();
        test.shift();
        test.shift();
        test.shift({txSeqA1T1});
        test.check({}, {txSeqA1T1});
        test.add(txSeqA1T3, TransactionQueue::AddResult::ADD_STATUS_ERROR);
        test.check({}, {txSeqA1T1});
    }

    SECTION("good then small sequence number, with four shifts")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.shift();
        test.shift();
        test.shift();
        test.shift({txSeqA1T1});
        test.check({}, {txSeqA1T1});
        test.add(txSeqA1T0, TransactionQueue::AddResult::ADD_STATUS_ERROR);
        test.check({}, {txSeqA1T1});
    }

    SECTION("invalid transaction")
    {
        TransactionQueueTest test{*app};
        test.add(invalidTransaction(*app, account1, 1),
                 TransactionQueue::AddResult::ADD_STATUS_ERROR);
        test.check();
    }

    SECTION("multiple good sequence numbers, with four shifts")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.add(txSeqA1T2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.add(txSeqA1T3, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.add(txSeqA1T4, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({txSeqA1T1, txSeqA1T2, txSeqA1T3, txSeqA1T4});
        test.shift();
        test.shift();
        test.shift();
        test.shift({txSeqA1T1, txSeqA1T2, txSeqA1T3, txSeqA1T4});
        test.check({}, {txSeqA1T1, txSeqA1T2, txSeqA1T3, txSeqA1T4});
    }

    SECTION("multiple good sequence numbers, with shifts between")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.shift();
        test.add(txSeqA1T2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.shift();
        test.add(txSeqA1T3, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.shift();
        test.add(txSeqA1T4, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({txSeqA1T1, txSeqA1T2, txSeqA1T3, txSeqA1T4});
        test.shift({txSeqA1T1});
        test.check({txSeqA1T2, txSeqA1T3, txSeqA1T4}, {txSeqA1T1});
        test.shift({txSeqA1T2});
        test.check({txSeqA1T3, txSeqA1T4}, {txSeqA1T1, txSeqA1T2});
        test.shift({txSeqA1T3});
        test.check({txSeqA1T4}, {txSeqA1T2, txSeqA1T3});
        test.shift({txSeqA1T4});
        test.check({}, {txSeqA1T3, txSeqA1T4});
    }

    SECTION(
        "multiple good sequence numbers, different accounts, with four shifts")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.add(txSeqA2T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.add(txSeqA1T2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.add(txSeqA2T2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({txSeqA1T1, txSeqA2T1, txSeqA1T2, txSeqA2T2});
        test.shift();
        test.shift();
        test.shift();
        test.shift({txSeqA1T1, txSeqA2T1, txSeqA1T2, txSeqA2T2});
        test.check({}, {txSeqA1T1, txSeqA2T1, txSeqA1T2, txSeqA2T2});
    }

    SECTION("multiple good sequence numbers, different accounts, with shifts "
            "between")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.shift();
        test.add(txSeqA2T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.shift();
        test.add(txSeqA1T2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.shift();
        test.add(txSeqA2T2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({txSeqA1T1, txSeqA2T1, txSeqA1T2, txSeqA2T2});
        test.shift({txSeqA1T1});
        test.check({txSeqA2T1, txSeqA1T2, txSeqA2T2}, {txSeqA1T1});
        test.shift({txSeqA2T1});
        test.check({txSeqA1T2, txSeqA2T2}, {txSeqA1T1, txSeqA2T1});
        test.shift({txSeqA1T2});
        test.check({txSeqA2T2}, {txSeqA2T1, txSeqA1T2});
        test.shift({txSeqA2T2});
        test.check({}, {txSeqA1T2, txSeqA2T2});
    }

    SECTION("multiple good sequence numbers, different accounts, with remove")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.add(txSeqA2T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.add(txSeqA1T2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.add(txSeqA2T2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({txSeqA1T1, txSeqA2T1, txSeqA1T2, txSeqA2T2});
        test.remove({txSeqA1T1, txSeqA2T2});
        test.check({txSeqA2T1, txSeqA1T2});
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_ERROR);
        test.add(txSeqA2T2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({txSeqA2T1, txSeqA1T2, txSeqA2T2});
        test.remove({txSeqA2T1, txSeqA1T2});
        test.check({txSeqA2T2});
        test.remove({txSeqA2T2});
        test.check();
    }
}
