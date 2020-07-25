// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "herder/Herder.h"
#include "herder/HerderImpl.h"
#include "herder/TransactionQueue.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/SignatureUtils.h"
#include "transactions/TransactionUtils.h"
#include "util/Timer.h"

#include <lib/catch.hpp>
#include <numeric>

using namespace stellar;
using namespace stellar::txtest;

namespace
{
TransactionFrameBasePtr
transaction(Application& app, TestAccount& account, int64_t sequenceDelta,
            int64_t amount, uint32_t fee)
{
    return transactionFromOperations(
        app, account, account.getLastSequenceNumber() + sequenceDelta,
        {payment(account.getPublicKey(), amount)}, fee);
}

TransactionFrameBasePtr
feeBump(Application& app, TestAccount& feeSource, TransactionFrameBasePtr tx,
        int64_t fee)
{
    REQUIRE(tx->getEnvelope().type() == ENVELOPE_TYPE_TX);
    TransactionEnvelope fb(ENVELOPE_TYPE_TX_FEE_BUMP);
    fb.feeBump().tx.feeSource = toMuxedAccount(feeSource);
    fb.feeBump().tx.fee = fee;
    fb.feeBump().tx.innerTx.type(ENVELOPE_TYPE_TX);
    fb.feeBump().tx.innerTx.v1() = tx->getEnvelope().v1();

    auto hash = sha256(xdr::xdr_to_opaque(
        app.getNetworkID(), ENVELOPE_TYPE_TX_FEE_BUMP, fb.feeBump().tx));
    fb.feeBump().signatures.emplace_back(SignatureUtils::sign(feeSource, hash));
    return TransactionFrameBase::makeTransactionFromWire(app.getNetworkID(),
                                                         fb);
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
        REQUIRE(mTransactionQueue.tryAdd(tx) == AddResult);
    }

    void
    removeApplied(std::vector<TransactionFrameBasePtr> const& toRemove)
    {
        auto size = mTransactionQueue.toTxSet({})->sizeTx();
        mTransactionQueue.removeApplied(toRemove);
        REQUIRE(size - toRemove.size() >=
                mTransactionQueue.toTxSet({})->sizeTx());
    }

    void
    ban(std::vector<TransactionFrameBasePtr> const& toRemove)
    {
        auto txSetBefore = mTransactionQueue.toTxSet({});
        // count the number of transactions from `toRemove` already included
        auto inPoolCount = std::count_if(
            toRemove.begin(), toRemove.end(),
            [&](TransactionFrameBasePtr const& tx) {
                auto const& txs = txSetBefore->mTransactions;
                return std::any_of(txs.begin(), txs.end(),
                                   [&](TransactionFrameBasePtr const& tx2) {
                                       return tx2->getFullHash() ==
                                              tx->getFullHash();
                                   });
            });
        mTransactionQueue.ban(toRemove);
        auto txSetAfter = mTransactionQueue.toTxSet({});
        REQUIRE(txSetBefore->sizeTx() - inPoolCount >= txSetAfter->sizeTx());
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
        auto txSet = mTransactionQueue.toTxSet({});
        for (auto const& tx : txSet->mTransactions)
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

        auto expectedTxSet = TxSetFrame{{}};
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
            totOps += accountTransactionQueueInfo.mQueueSizeOps;

            for (auto& tx : accountState.mAccountTransactions)
            {
                expectedTxSet.add(tx);
            }
        }

        REQUIRE(txSet->sizeOp() == mTransactionQueue.getQueueSizeOps());
        REQUIRE(totOps == mTransactionQueue.getQueueSizeOps());

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
    auto cfg = getTestConfig();
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 4;
    auto app = createTestApplication(clock, cfg);
    auto const minBalance2 = app->getLedgerManager().getLastMinBalance(2);

    auto root = TestAccount::createRoot(*app);
    auto account1 = root.create("a1", minBalance2);
    auto account2 = root.create("a2", minBalance2);
    auto account3 = root.create("a3", minBalance2);

    auto txSeqA1T0 = transaction(*app, account1, 0, 1, 100);
    auto txSeqA1T1 = transaction(*app, account1, 1, 1, 200);
    auto txSeqA1T2 = transaction(*app, account1, 2, 1, 300);
    auto txSeqA1T1V2 = transaction(*app, account1, 1, 2, 400);
    auto txSeqA1T2V2 = transaction(*app, account1, 2, 2, 500);
    auto txSeqA1T3 = transaction(*app, account1, 3, 1, 600);
    auto txSeqA1T4 = transaction(*app, account1, 4, 1, 700);
    auto txSeqA2T1 = transaction(*app, account2, 1, 1, 800);
    auto txSeqA2T2 = transaction(*app, account2, 2, 1, 900);
    auto txSeqA3T1 = transaction(*app, account3, 1, 1, 100);

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
        test.add(txSeqA1T2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1, txSeqA1T2}}, {account2}}, {}});
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_DUPLICATE);
        test.check({{{account1, 0, {txSeqA1T1, txSeqA1T2}}, {account2}}, {}});
        test.add(txSeqA1T2, TransactionQueue::AddResult::ADD_STATUS_DUPLICATE);
        test.check({{{account1, 0, {txSeqA1T1, txSeqA1T2}}, {account2}}, {}});
    }

    SECTION("good then small sequence number")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}, {}});
        test.add(txSeqA1T3, TransactionQueue::AddResult::ADD_STATUS_ERROR);
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
        test.removeApplied({txSeqA1T1, txSeqA2T2});
        test.check({{{account1, 0, {txSeqA1T2}}, {account2}}});
        test.removeApplied({txSeqA1T2});
        test.check({{{account1}, {account2}}});
        test.add(txSeqA1T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}});
        test.add(txSeqA2T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}}, {account2, 0, {txSeqA2T1}}}});
        test.add(txSeqA2T2, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        test.check({{{account1, 0, {txSeqA1T1}},
                     {account2, 0, {txSeqA2T1, txSeqA2T2}}}});
        test.removeApplied({txSeqA2T1});
        test.check({{{account1, 0, {txSeqA1T1}}, {account2, 0, {txSeqA2T2}}}});
        test.removeApplied({txSeqA2T2});
        test.check({{{account1, 0, {txSeqA1T1}}, {account2}}});
        test.removeApplied({txSeqA1T1});
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
    SECTION("many transactions hit the pool limit")
    {
        TransactionQueueTest test{*app};
        test.add(txSeqA3T1, TransactionQueue::AddResult::ADD_STATUS_PENDING);
        std::vector<TransactionFrameBasePtr> a3Txs({txSeqA3T1});
        std::vector<TransactionFrameBasePtr> banned;

        for (int i = 2; i <= 10; i++)
        {
            auto txSeqA3Ti = transaction(*app, account3, i, 1, 100);
            if (i <= 8)
            {
                test.add(txSeqA3Ti,
                         TransactionQueue::AddResult::ADD_STATUS_PENDING);
                a3Txs.emplace_back(txSeqA3Ti);
            }
            else
            {
                test.add(
                    txSeqA3Ti,
                    TransactionQueue::AddResult::ADD_STATUS_TRY_AGAIN_LATER);
                banned.emplace_back(txSeqA3Ti);
                test.check({{{account3, 0, a3Txs}}, {banned}});
            }
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

    closeLedgerOn(*app, 2, 1, 1, 2020);
    closeLedgerOn(*app, 3, 1, 1, 2020);

    int64_t startingSeq = static_cast<int64_t>(4) << 32;
    REQUIRE(acc1.loadSequenceNumber() < startingSeq);
    acc1.bumpSequence(startingSeq - 3);
    REQUIRE(acc1.loadSequenceNumber() == startingSeq - 3);

    TransactionQueue tq(*app, 4, 10, 4);
    for (size_t i = 1; i <= 4; ++i)
    {
        REQUIRE(tq.tryAdd(transaction(*app, acc1, i, 1, 100)) ==
                TransactionQueue::AddResult::ADD_STATUS_PENDING);
    }

    auto checkTxSet = [&](uint32_t ledgerSeq, size_t size) {
        auto lcl = app->getLedgerManager().getLastClosedLedgerHeader();
        lcl.header.ledgerSeq = ledgerSeq;
        auto txSet = tq.toTxSet(lcl);
        REQUIRE(txSet->mTransactions.size() == size);
        for (size_t i = 1; i <= size; ++i)
        {
            REQUIRE(txSet->mTransactions[i - 1]->getSeqNum() ==
                    static_cast<int64_t>(startingSeq - 3 + i));
        }
    };

    checkTxSet(2, 4);
    checkTxSet(3, 2);
    checkTxSet(4, 4);
}

TEST_CASE("transaction queue with fee-bump", "[herder][transactionqueue]")
{
    VirtualClock clock;
    auto app = createTestApplication(clock, getTestConfig());
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

        for (int i = 0; i <= 3; ++i)
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

        for (int i = 1; i <= 3; ++i)
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

        for (int i = 1; i <= 3; ++i)
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

        for (int i = 1; i <= 3; ++i)
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

        for (int i = 1; i <= 3; ++i)
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
            test.check({{{account1, 0, {fb2}}, {account2}, {account3}}, {}});
        }
        SECTION("remove second")
        {
            test.removeApplied({fb1, fb2});
            test.check({{{account1}, {account2}, {account3}}, {}});
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
            test.check({{{account1, 0, {fb2}}, {account2}, {account3}}, {}});
        }
        SECTION("remove second")
        {
            test.removeApplied({fb1, fb2});
            test.check({{{account1}, {account2}, {account3}}, {}});
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
    auto app = createTestApplication(clock, getTestConfig());
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
                SECTION(position[i - 1] +
                        " transaction from same source account")
                {
                    auto tx = transaction(*app, account1, i, 1, 100);
                    auto fb = feeBump(*app, account1, tx, 4000);
                    txs[i - 1] = fb;
                    test.add(fb,
                             TransactionQueue::AddResult::ADD_STATUS_PENDING);
                    test.check({{{account1, 0, txs}, {account2}}, {}});
                }
                SECTION(position[i - 1] +
                        " transaction from different source account")
                {
                    auto tx = transaction(*app, account1, i, 1, 100);
                    auto fb = feeBump(*app, account2, tx, 4000);
                    txs[i - 1] = fb;
                    test.add(fb,
                             TransactionQueue::AddResult::ADD_STATUS_PENDING);
                    test.check({{{account1, 0, txs}, {account2}}, {}});
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
    app->start();

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

    herder.recvTransaction(tx1a);
    herder.recvTransaction(tx2);
    herder.recvTransaction(tx3);

    {
        auto const& lcl = lm.getLastClosedLedgerHeader();
        auto ledgerSeq = lcl.header.ledgerSeq + 1;

        auto txSet = std::make_shared<TxSetFrame>(lcl.hash);
        root.loadSequenceNumber();
        txSet->add(tx1b);
        txSet->add(tx2);
        herder.getPendingEnvelopes().putTxSet(txSet->getContentsHash(),
                                              ledgerSeq, txSet);

        StellarValue sv{txSet->getContentsHash(), 2,
                        xdr::xvector<UpgradeType, 6>{}, STELLAR_VALUE_BASIC};
        herder.getHerderSCPDriver().valueExternalized(ledgerSeq,
                                                      xdr::xdr_to_opaque(sv));
    }

    REQUIRE(tq.toTxSet({})->mTransactions.size() == 1);
    REQUIRE(herder.recvTransaction(tx4) ==
            TransactionQueue::AddResult::ADD_STATUS_PENDING);
    REQUIRE(tq.toTxSet({})->mTransactions.size() == 2);
}
