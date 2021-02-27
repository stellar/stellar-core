// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "herder/Herder.h"
#include "herder/HerderImpl.h"
#include "herder/SurgePricingUtils.h"
#include "herder/TransactionQueue.h"
#include "herder/TxQueueLimiter.h"
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
            REQUIRE(accountTransactionQueueInfo.mBroadcastQueueOps ==
                    accountTransactionQueueInfo.mQueueSizeOps);
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
        TxQueueLimiter limiter(3, app->getLedgerManager());

        REQUIRE(limiter.maxQueueSizeOps() == 12);

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
                    bool can = limiter.canAddTx(tx, noTx).first;
                    REQUIRE(can);
                    limiter.addTransaction(tx);
                    txs.emplace_back(tx);
                }
            }
            REQUIRE(limiter.size() == 11);
        };
        // act \ base fee   400 300 200  100
        //  1                2   1    0   0
        //  2                1   1    2   0
        //  3                0   1    1   0
        //  4                0   0    1   0
        //  5                0   0    0   1
        // total             3   3    4   1 --> 11 (free = 1)
        setup({{account1, 1, {{1, 400}, {1, 300}, {1, 400}}},
               {account2, 1, {{1, 400}, {1, 300}, {2, 400}}},
               {account3, 1, {{1, 300}, {1, 200}}},
               {account4, 1, {{1, 200}}},
               {account5, 1, {{1, 100}}}});
        auto checkAndAddTx = [&](bool expected, TestAccount& account, int ops,
                                 int fee, int64 expFeeOnFailed) {
            auto tx = transaction(*app, account, 1000, 1, fee, ops);
            auto can = limiter.canAddTx(tx, noTx);
            REQUIRE(expected == can.first);
            if (can.first)
            {
                bool evicted = limiter.evictTransactions(
                    tx->getNumOperations(),
                    [&](TransactionFrameBasePtr const& evict) {
                        // can't evict cheaper transactions
                        auto cmp3 = feeRate3WayCompare(evict, tx);
                        REQUIRE(cmp3 < 0);
                        // can't evict self
                        bool same = evict->getSourceID() == tx->getSourceID();
                        REQUIRE(!same);
                        limiter.removeTransaction(evict);
                    });
                REQUIRE(evicted);
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
        auto checkTxBoundary = [&](TestAccount& account, int ops, int bfee) {
            auto txFee1 = bfee * (ops + 1);
            checkAndAddTx(false, account, ops + 1, txFee1, txFee1 + 1);
            checkAndAddTx(true, account, ops, bfee * ops, 0);
        };
        auto getBaseFeeRate = [](TxQueueLimiter const& limiter) {
            auto fr = limiter.getMinFeeNeeded();
            return fr.second == 0
                       ? 0ll
                       : bigDivide(fr.first, 1, fr.second, Rounding::ROUND_UP);
        };

        SECTION("evict nothing")
        {
            checkTxBoundary(account1, 1, 100);
            REQUIRE(limiter.size() == 11);
            REQUIRE(getBaseFeeRate(limiter) == 0);
            // can't evict transaction with the same base fee
            checkAndAddTx(false, account1, 2, 100 * 2, 2 * 100 + 1);
            REQUIRE(limiter.size() == 11);
            REQUIRE(getBaseFeeRate(limiter) == 0);
        }
        SECTION("evict 100s")
        {
            checkTxBoundary(account1, 2, 200);
            REQUIRE(limiter.size() == 10);
        }
        SECTION("evict 100s and 200s")
        {
            checkTxBoundary(account6, 6, 300);
            REQUIRE(limiter.size() == 6);
            REQUIRE(getBaseFeeRate(limiter) == 200);
        }
        SECTION("evict 100s and 200s, can't evict self")
        {
            checkAndAddTx(false, account2, 6, 6 * 300, 0);
        }
        SECTION("evict all")
        {
            checkAndAddTx(true, account6, 12, 12 * 500, 0);
            REQUIRE(limiter.size() == 0);
            REQUIRE(getBaseFeeRate(limiter) == 400);
            limiter.resetMinFeeNeeded();
            REQUIRE(getBaseFeeRate(limiter) == 0);
        }
        SECTION("enforce limit")
        {
            REQUIRE(getBaseFeeRate(limiter) == 0);
            checkAndAddTx(true, account1, 2, 2 * 200, 0);
            REQUIRE(limiter.size() == 10);
            // at this point as a transaction of base fee 100 was evicted
            // no transactions of base fee 100 can be accepted
            REQUIRE(getBaseFeeRate(limiter) == 100);
            checkAndAddTx(false, account1, 1, 100, 101);
            // but higher fee can
            checkAndAddTx(true, account1, 1, 200, 0);
            REQUIRE(limiter.size() == 10);
            REQUIRE(getBaseFeeRate(limiter) == 100);
            // evict some more (300s)
            checkAndAddTx(true, account6, 8, 300 * 8 + 1, 0);
            REQUIRE(limiter.size() == 4);
            REQUIRE(getBaseFeeRate(limiter) == 300);
            checkAndAddTx(false, account1, 1, 300, 301);

            // now, reset the min fee requirement
            limiter.resetMinFeeNeeded();
            REQUIRE(getBaseFeeRate(limiter) == 0);
            checkAndAddTx(true, account1, 1, 100, 0);
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

    SECTION("check a single transaction")
    {
        int64_t startingSeq = static_cast<int64_t>(4) << 32;
        REQUIRE(acc1.loadSequenceNumber() < startingSeq);
        acc1.bumpSequence(startingSeq - 1);
        REQUIRE(acc1.loadSequenceNumber() == startingSeq - 1);

        TransactionQueue tq(*app, 4, 10, 4);
        REQUIRE(tq.tryAdd(transaction(*app, acc1, 1, 1, 100)) ==
                TransactionQueue::AddResult::ADD_STATUS_PENDING);

        auto checkTxSet = [&](uint32_t ledgerSeq) {
            auto lcl = app->getLedgerManager().getLastClosedLedgerHeader();
            lcl.header.ledgerSeq = ledgerSeq;
            auto txSet = tq.toTxSet(lcl);
            return !txSet->mTransactions.empty();
        };

        REQUIRE(checkTxSet(2));
        REQUIRE(!checkTxSet(3));
        REQUIRE(checkTxSet(4));
    }

    SECTION("check a chain of transactions")
    {
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

        StellarValue sv = herder.makeStellarValue(txSet->getContentsHash(), 2,
                                                  emptyUpgradeSteps,
                                                  app->getConfig().NODE_SEED);
        herder.getHerderSCPDriver().valueExternalized(ledgerSeq,
                                                      xdr::xdr_to_opaque(sv));
    }

    REQUIRE(tq.toTxSet({})->mTransactions.size() == 1);
    REQUIRE(herder.recvTransaction(tx4) ==
            TransactionQueue::AddResult::ADD_STATUS_PENDING);
    REQUIRE(tq.toTxSet({})->mTransactions.size() == 2);
}
