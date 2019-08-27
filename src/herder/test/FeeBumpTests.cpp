// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/TransactionQueue.h"
#include "lib/catch.hpp"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/SignatureUtils.h"

using namespace stellar;
using namespace stellar::txtest;

static void
testTransactionQueueWithFeeBumps(uint32_t protocolVersion)
{
    Config cfg(getTestConfig());
    cfg.LEDGER_PROTOCOL_VERSION = protocolVersion;
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 50;
    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);
    app->start();

    TransactionQueue queue(*app, 10, 4);

    auto& lm = app->getLedgerManager();
    auto reserve = lm.getLastReserve();
    auto fee = lm.getLastTxFee();

    auto root = TestAccount::createRoot(*app);

    auto getTx = [&](TestAccount& acc, uint32_t fee) {
        return transactionFromOperations(*app, acc.getSecretKey(),
                                         acc.nextSequenceNumber(),
                                         {payment(acc, Asset{}, 1)}, fee);
    };
    auto getMultiTx = [&](TestAccount& acc, uint32_t fee,
                          std::vector<SecretKey> const& keys) {
        TransactionEnvelope e(ENVELOPE_TYPE_TX_V0);
        e.v0().tx.sourceAccountEd25519 = acc.getPublicKey().ed25519();
        e.v0().tx.fee = fee;
        e.v0().tx.seqNum = acc.nextSequenceNumber();
        for (auto const& key : keys)
        {
            e.v0().tx.operations.emplace_back(
                payment(key.getPublicKey(), Asset{}, 1));
            e.v0().tx.operations.back().sourceAccount.activate() =
                key.getPublicKey();
        }
        for (auto const& key : keys)
        {
            e.v0().signatures.emplace_back(SignatureUtils::sign(
                key,
                sha256(xdr::xdr_to_opaque(app->getNetworkID(), ENVELOPE_TYPE_TX,
                                          (uint32_t)0, e.v0().tx))));
        }
        return std::static_pointer_cast<TransactionFrame>(
            TransactionFrameBase::makeTransactionFromWire(app->getNetworkID(),
                                                          e));
    };

    auto check = [&](TestAccount& acc, int64_t maxSeq, int64_t totalFee,
                     int64_t age) {
        auto ai = queue.getAccountTransactionQueueInfo(acc);
        REQUIRE(ai.mMaxSeq == maxSeq);
        REQUIRE(ai.mTotalFees == totalFee);
        REQUIRE(ai.mAge == age);
    };

    SECTION("multiple fee bumps for same inner transaction")
    {
        SECTION("exact duplicate")
        {
            auto accA = root.create("A", 2 * reserve + 10 * fee);
            auto accB = root.create("B", 2 * reserve + 10 * fee);

            auto fb1 = accB.feeBump(getTx(accA, fee));
            accA.loadSequenceNumber();
            auto fb2 = accB.feeBump(getTx(accA, fee));
            REQUIRE(queue.tryAdd(fb1) ==
                    TransactionQueue::AddResult::ADD_STATUS_PENDING);
            check(accA, fb1->getSeqNum(), 0, 0);
            check(accB, 0, 2 * fee, 0);
            REQUIRE(queue.tryAdd(fb2) ==
                    TransactionQueue::AddResult::ADD_STATUS_DUPLICATE);
            check(accA, fb1->getSeqNum(), 0, 0);
            check(accB, 0, 2 * fee, 0);
        }

        SECTION("same fee source higher fee")
        {
            auto accA = root.create("A", 2 * reserve + 10 * fee);
            auto accB = root.create("B", 2 * reserve + 10 * fee);

            auto fb1 = accB.feeBump(getTx(accA, fee));
            accA.loadSequenceNumber();
            auto fb2 = feeBumpFromTransaction(*app, accB.getSecretKey(),
                                              getTx(accA, fee), 2 * fee + 1);
            REQUIRE(queue.tryAdd(fb1) ==
                    TransactionQueue::AddResult::ADD_STATUS_PENDING);
            check(accA, fb1->getSeqNum(), 0, 0);
            check(accB, 0, 2 * fee, 0);
            REQUIRE(queue.tryAdd(fb2) ==
                    TransactionQueue::AddResult::ADD_STATUS_PENDING);
            check(accA, fb2->getSeqNum(), 0, 0);
            check(accB, 0, 4 * fee + 1, 0);
        }

        SECTION("same fee source lower fee")
        {
            auto accA = root.create("A", 2 * reserve + 10 * fee);
            auto accB = root.create("B", 2 * reserve + 10 * fee);

            auto fb1 = feeBumpFromTransaction(*app, accB.getSecretKey(),
                                              getTx(accA, fee), 2 * fee + 1);
            accA.loadSequenceNumber();
            auto fb2 = accB.feeBump(getTx(accA, fee));
            REQUIRE(queue.tryAdd(fb1) ==
                    TransactionQueue::AddResult::ADD_STATUS_PENDING);
            check(accA, fb1->getSeqNum(), 0, 0);
            check(accB, 0, 2 * fee + 1, 0);
            REQUIRE(queue.tryAdd(fb2) ==
                    TransactionQueue::AddResult::ADD_STATUS_PENDING);
            check(accA, fb2->getSeqNum(), 0, 0);
            check(accB, 0, 4 * fee + 1, 0);
        }

        SECTION("different fee source")
        {
            auto accA = root.create("A", 2 * reserve + 10 * fee);
            auto accB = root.create("B", 2 * reserve + 10 * fee);
            auto accC = root.create("C", 2 * reserve + 10 * fee);

            auto fb1 = accB.feeBump(getTx(accA, fee));
            accA.loadSequenceNumber();
            auto fb2 = accC.feeBump(getTx(accA, fee));
            REQUIRE(queue.tryAdd(fb1) ==
                    TransactionQueue::AddResult::ADD_STATUS_PENDING);
            check(accA, fb1->getSeqNum(), 0, 0);
            check(accB, 0, 2 * fee, 0);
            check(accC, 0, 0, 0);
            REQUIRE(queue.tryAdd(fb2) ==
                    TransactionQueue::AddResult::ADD_STATUS_PENDING);
            check(accA, fb2->getSeqNum(), 0, 0);
            check(accB, 0, 2 * fee, 0);
            check(accC, 0, 2 * fee, 0);
        }

        SECTION("fee bump before inner transaction")
        {
            auto accA = root.create("A", 2 * reserve + 10 * fee);
            auto accB = root.create("B", 2 * reserve + 10 * fee);

            auto fb1 = accB.feeBump(getTx(accA, fee));
            accA.loadSequenceNumber();
            auto fb2 = getTx(accA, fee);

            REQUIRE(queue.tryAdd(fb1) ==
                    TransactionQueue::AddResult::ADD_STATUS_PENDING);
            check(accA, fb1->getSeqNum(), 0, 0);
            check(accB, 0, 2 * fee, 0);
            REQUIRE(queue.tryAdd(fb2) ==
                    TransactionQueue::AddResult::ADD_STATUS_PENDING);
            check(accA, fb2->getSeqNum(), fee, 0);
            check(accB, 0, 2 * fee, 0);
        }

        SECTION("fee bump after inner transaction")
        {
            auto accA = root.create("A", 2 * reserve + 10 * fee);
            auto accB = root.create("B", 2 * reserve + 10 * fee);

            auto fb1 = getTx(accA, fee);
            accA.loadSequenceNumber();
            auto fb2 = accB.feeBump(getTx(accA, fee));

            REQUIRE(queue.tryAdd(fb1) ==
                    TransactionQueue::AddResult::ADD_STATUS_PENDING);
            check(accA, fb1->getSeqNum(), fee, 0);
            check(accB, 0, 0, 0);
            REQUIRE(queue.tryAdd(fb2) ==
                    TransactionQueue::AddResult::ADD_STATUS_PENDING);
            check(accA, fb2->getSeqNum(), fee, 0);
            check(accB, 0, 2 * fee, 0);
        }

        SECTION("permuted outer signatures with fee bump")
        {
            auto accA = root.create("A", 2 * reserve + 10 * fee);
            auto accB = root.create("B", 3 * reserve + 10 * fee);

            Signer signer;
            signer.key.type(SIGNER_KEY_TYPE_ED25519);
            signer.key.ed25519() = accA.getPublicKey().ed25519();
            signer.weight = 1;
            accB.setOptions(setLowThreshold(2) | setMedThreshold(2) |
                            setHighThreshold(2) | setSigner(signer));

            auto fb1 = feeBumpFromTransaction(
                *app, accB.getSecretKey(), getTx(accA, fee), 2 * fee,
                {accA.getSecretKey(), accB.getSecretKey()});
            accA.loadSequenceNumber();
            auto fb2 = feeBumpFromTransaction(
                *app, accB.getSecretKey(), getTx(accA, fee), 2 * fee,
                {accB.getSecretKey(), accA.getSecretKey()});

            REQUIRE(queue.tryAdd(fb1) ==
                    TransactionQueue::AddResult::ADD_STATUS_ERROR);
            check(accA, 0, 0, 0);
            check(accB, 0, 0, 0);
            REQUIRE(queue.tryAdd(fb2) ==
                    TransactionQueue::AddResult::ADD_STATUS_PENDING);
            check(accA, fb2->getSeqNum(), 0, 0);
            check(accB, 0, 2 * fee, 0);
        }
    }

    SECTION("multiple fee bumps for inner transaction with different full hash")
    {
        SECTION("fee bump then v0")
        {
            auto accA = root.create("A", 2 * reserve + 10 * fee);
            auto accB = root.create("B", 2 * reserve + 10 * fee);

            auto fb1 = accB.feeBump(getTx(accA, fee));
            accA.loadSequenceNumber();
            auto fb2 = getTx(accA, fee);

            REQUIRE(queue.tryAdd(fb1) ==
                    TransactionQueue::AddResult::ADD_STATUS_PENDING);
            check(accA, fb1->getSeqNum(), 0, 0);
            check(accB, 0, 2 * fee, 0);
            REQUIRE(queue.tryAdd(fb2) ==
                    TransactionQueue::AddResult::ADD_STATUS_PENDING);
            check(accA, fb2->getSeqNum(), fee, 0);
            check(accB, 0, 2 * fee, 0);
        }

        SECTION("v0 then fee bump")
        {
            auto accA = root.create("A", 2 * reserve + 10 * fee);
            auto accB = root.create("B", 2 * reserve + 10 * fee);

            auto fb1 = getTx(accA, fee);
            accA.loadSequenceNumber();
            auto fb2 = accB.feeBump(getTx(accA, fee));

            REQUIRE(queue.tryAdd(fb1) ==
                    TransactionQueue::AddResult::ADD_STATUS_PENDING);
            check(accA, fb1->getSeqNum(), fee, 0);
            check(accB, 0, 0, 0);
            REQUIRE(queue.tryAdd(fb2) ==
                    TransactionQueue::AddResult::ADD_STATUS_PENDING);
            check(accA, fb2->getSeqNum(), fee, 0);
            check(accB, 0, 2 * fee, 0);
        }

        SECTION("permuted signatures without fee bump")
        {
            auto accA = root.create("A", 2 * reserve + 10 * fee);
            auto accB = root.create("B", 2 * reserve + 10 * fee);

            auto tx1 = getMultiTx(accA, 2 * fee, {accA, accB});
            accA.loadSequenceNumber();
            auto tx2 = getMultiTx(accA, 2 * fee, {accB, accA});

            REQUIRE(queue.tryAdd(tx1) ==
                    TransactionQueue::AddResult::ADD_STATUS_ERROR);
            check(accA, 0, 0, 0);
            REQUIRE(queue.tryAdd(tx2) ==
                    TransactionQueue::AddResult::ADD_STATUS_PENDING);
            check(accA, tx2->getSeqNum(), 2 * fee, 0);
        }

        SECTION("permuted inner signatures with fee bump")
        {
            auto accA = root.create("A", 2 * reserve + 10 * fee);
            auto accB = root.create("B", 2 * reserve + 10 * fee);

            auto fb1 = accA.feeBump(getMultiTx(accA, 2 * fee, {accA, accB}));
            accA.loadSequenceNumber();
            auto fb2 = accA.feeBump(getMultiTx(accA, 2 * fee, {accB, accA}));

            REQUIRE(queue.tryAdd(fb1) ==
                    TransactionQueue::AddResult::ADD_STATUS_ERROR);
            check(accA, 0, 0, 0);
            check(accB, 0, 0, 0);
            REQUIRE(queue.tryAdd(fb2) ==
                    TransactionQueue::AddResult::ADD_STATUS_PENDING);
            check(accA, fb2->getSeqNum(), 3 * fee, 0);
            check(accB, 0, 0, 0);
        }
    }
}

TEST_CASE("transaction queue with fee bumps", "[herder]")
{
    testTransactionQueueWithFeeBumps(12);
}

static void
testTxSetWithFeeBumps(uint32_t protocolVersion)
{
    Config cfg(getTestConfig());
    cfg.LEDGER_PROTOCOL_VERSION = protocolVersion;
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 50;
    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);
    app->start();

    auto& lm = app->getLedgerManager();
    auto reserve = lm.getLastReserve();
    auto fee = lm.getLastTxFee();

    auto root = TestAccount::createRoot(*app);

    auto getTx = [&](TestAccount& acc, uint32_t fee) {
        return transactionFromOperations(*app, acc.getSecretKey(),
                                         acc.nextSequenceNumber(),
                                         {payment(acc, Asset{}, 1)}, fee);
    };

    SECTION("both accounts at fee limit is valid")
    {
        auto txSet =
            std::make_shared<TxSetFrame>(lm.getLastClosedLedgerHeader().hash);

        auto accA = root.create("A", 2 * reserve + 3 * fee);
        auto accB = root.create("B", 2 * reserve + 3 * fee);

        txSet->add(getTx(accA, fee));
        txSet->add(getTx(accA, fee));
        txSet->add(getTx(accB, fee));
        txSet->add(accB.feeBump(getTx(accA, fee)));
        txSet->add(getTx(accA, fee));

        txSet->sortForHash();
        REQUIRE(txSet->checkValid(*app));
    }

    SECTION("source account for inner transaction above fee limit")
    {
        auto accA = root.create("A", 2 * reserve + 3 * fee);
        auto accB = root.create("B", 2 * reserve + 3 * fee);

        for (size_t i = 0; i < 3; ++i)
        {
            accA.loadSequenceNumber();
            accB.loadSequenceNumber();

            auto txSet = std::make_shared<TxSetFrame>(
                lm.getLastClosedLedgerHeader().hash);
            txSet->add(getTx(accA, fee + (i == 0)));
            txSet->add(getTx(accA, fee + (i == 1)));
            txSet->add(getTx(accB, fee));
            txSet->add(accB.feeBump(getTx(accA, fee)));
            txSet->add(getTx(accA, fee + (i == 2)));

            txSet->sortForHash();
            REQUIRE(!txSet->checkValid(*app));

            auto trimmed = txSet->trimInvalid(*app);
            REQUIRE(trimmed.size() == 4);
            REQUIRE(txSet->mTransactions.size() == 1);

            REQUIRE(txSet->checkValid(*app));
        }
    }

    SECTION("source account for outer transaction above fee limit")
    {
        auto accA = root.create("A", 2 * reserve + 3 * fee);
        auto accB = root.create("B", 2 * reserve + 3 * fee);

        for (size_t i = 0; i < 1; ++i)
        {
            accA.loadSequenceNumber();
            accB.loadSequenceNumber();

            auto txSet = std::make_shared<TxSetFrame>(
                lm.getLastClosedLedgerHeader().hash);
            txSet->add(getTx(accA, fee));
            txSet->add(getTx(accA, fee));
            txSet->add(getTx(accB, fee + (i == 0)));
            txSet->add(feeBumpFromTransaction(*app, accB.getSecretKey(),
                                              getTx(accA, fee),
                                              2 * fee + (i == 1)));
            txSet->add(getTx(accA, fee));

            txSet->sortForHash();
            REQUIRE(!txSet->checkValid(*app));

            auto trimmed = txSet->trimInvalid(*app);
            REQUIRE(trimmed.size() == 3);
            REQUIRE(txSet->mTransactions.size() == 2);

            REQUIRE(txSet->checkValid(*app));
        }
    }

    SECTION("fee bumps count as an operation for max size")
    {
        auto accA = root.create("A", 2 * reserve + 10000 * fee);
        std::vector<Operation> ops(49, payment(accA, Asset{}, 1));

        auto txSet =
            std::make_shared<TxSetFrame>(lm.getLastClosedLedgerHeader().hash);
        txSet->add(accA.feeBump(accA.tx(ops)));
        REQUIRE(txSet->checkValid(*app));

        txSet->add(getTx(accA, fee));
        txSet->sortForHash();
        REQUIRE(!txSet->checkValid(*app));
    }

    SECTION("repeated inner transactions are counted properly for max size")
    {
        auto accA = root.create("A", 2 * reserve + 10000 * fee);
        std::vector<Operation> ops1(10, payment(accA, Asset{}, 1));
        std::vector<Operation> ops2(20, payment(accA, Asset{}, 1));

        auto txSet =
            std::make_shared<TxSetFrame>(lm.getLastClosedLedgerHeader().hash);
        txSet->add(accA.tx(ops1));
        for (size_t i = 1; i <= 10; ++i)
        {
            accA.loadSequenceNumber();
            txSet->add(accA.feeBump(accA.tx(ops1), (10 + i) * fee));
        }

        auto seq = accA.nextSequenceNumber();
        txSet->add(accA.tx(ops2, seq));
        for (size_t i = 1; i <= 10; ++i)
        {
            txSet->add(accA.feeBump(accA.tx(ops2, seq), (20 + i) * fee));
        }

        txSet->sortForHash();
        REQUIRE(txSet->sizeOp() == 50);
        REQUIRE(txSet->checkValid(*app));
    }

    SECTION("trim for fee drops transaction with higher sequence number "
            "when no other transactions in the slot remain")
    {
        auto accA = root.create("A", 2 * reserve + fee);
        auto accB = root.create("B", 2 * reserve + 2 * fee);

        auto txSet =
            std::make_shared<TxSetFrame>(lm.getLastClosedLedgerHeader().hash);
        txSet->add(accB.feeBump(getTx(accA, fee)));
        txSet->add(getTx(accA, fee));
        txSet->add(getTx(accB, fee));

        txSet->sortForHash();
        REQUIRE(!txSet->checkValid(*app));
        txSet->trimInvalid(*app);
        REQUIRE(txSet->sizeTx() == 0);
        REQUIRE(txSet->checkValid(*app));
    }

    SECTION("trim for fee does not drop transactions with higher sequence "
            "number when other transactions in the slot remain")
    {
        auto accA = root.create("A", 2 * reserve + 2 * fee);
        auto accB = root.create("B", 2 * reserve + 2 * fee);

        auto txSet =
            std::make_shared<TxSetFrame>(lm.getLastClosedLedgerHeader().hash);
        txSet->add(accB.feeBump(getTx(accA, fee)));
        txSet->add(getTx(accA, fee));
        txSet->add(getTx(accB, fee));
        accA.loadSequenceNumber();
        txSet->add(getTx(accA, fee));

        txSet->sortForHash();
        REQUIRE(!txSet->checkValid(*app));
        txSet->trimInvalid(*app);
        REQUIRE(txSet->sizeTx() == 2);
        REQUIRE(txSet->checkValid(*app));
    }
}

TEST_CASE("txset with fee bumps", "[herder]")
{
    testTxSetWithFeeBumps(12);
}

static void
testSurgePricing(uint32_t protocolVersion)
{
    Config cfg(getTestConfig());
    cfg.LEDGER_PROTOCOL_VERSION = protocolVersion;
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 20;
    VirtualClock clock;
    auto app = createTestApplication(clock, cfg);
    app->start();

    auto& lm = app->getLedgerManager();
    auto reserve = lm.getLastReserve();
    auto fee = lm.getLastTxFee();

    auto root = TestAccount::createRoot(*app);

    auto getTxWithSeq = [&](TestAccount& acc, uint32_t seq, uint32_t fee,
                            size_t nOps) {
        return transactionFromOperations(
            *app, acc.getSecretKey(), seq,
            std::vector<Operation>(nOps, payment(acc, Asset{}, 1)), fee);
    };
    auto getTx = [&](TestAccount& acc, uint32_t fee, size_t nOps) {
        return getTxWithSeq(acc, acc.nextSequenceNumber(), fee, nOps);
    };

    SECTION("one inner transaction with inner transaction first")
    {
        auto txSet =
            std::make_shared<TxSetFrame>(lm.getLastClosedLedgerHeader().hash);
        txSet->add(getTx(root, 100 * fee, 10));
        for (size_t i = 1; i <= 11; ++i)
        {
            root.loadSequenceNumber();
            txSet->add(
                root.feeBump(getTx(root, 100 * fee, 10), (10 + i) * fee));
        }
        txSet->trimInvalid(*app);
        REQUIRE(txSet->sizeTx() == 12);
        txSet->sortForHash();
        txSet->surgePricingFilter(*app);
        REQUIRE(txSet->sizeTx() == 11);
    }

    SECTION("one inner transaction with inner transaction last")
    {
        auto txSet =
            std::make_shared<TxSetFrame>(lm.getLastClosedLedgerHeader().hash);
        txSet->add(getTx(root, 10 * fee, 10));
        for (size_t i = 1; i <= 11; ++i)
        {
            root.loadSequenceNumber();
            txSet->add(
                root.feeBump(getTx(root, 10 * fee, 10), (100 + i) * fee));
        }
        txSet->trimInvalid(*app);
        REQUIRE(txSet->sizeTx() == 12);
        txSet->sortForHash();
        txSet->surgePricingFilter(*app);
        REQUIRE(txSet->sizeTx() == 11);
    }

    SECTION("fee bump has lower fee than next transaction")
    {
        auto txSet =
            std::make_shared<TxSetFrame>(lm.getLastClosedLedgerHeader().hash);
        txSet->add(root.feeBump(getTx(root, 100 * fee, 9), 150 * fee));
        txSet->add(getTx(root, 200 * fee, 10));
        txSet->add(getTx(root, 2 * fee, 2));

        txSet->trimInvalid(*app);
        REQUIRE(txSet->sizeTx() == 3);
        txSet->sortForHash();
        txSet->surgePricingFilter(*app);
        REQUIRE(txSet->sizeTx() == 2);
        REQUIRE(txSet->sizeOp() == 20);
    }

    SECTION("inner transaction has lower fee than next transaction")
    {
        auto txSet =
            std::make_shared<TxSetFrame>(lm.getLastClosedLedgerHeader().hash);
        txSet->add(getTx(root, 100 * fee, 10));
        txSet->add(getTx(root, 200 * fee, 10));
        txSet->add(getTx(root, fee, 1));

        txSet->trimInvalid(*app);
        REQUIRE(txSet->sizeTx() == 3);
        txSet->sortForHash();
        txSet->surgePricingFilter(*app);
        REQUIRE(txSet->sizeTx() == 2);
        REQUIRE(txSet->sizeOp() == 20);
    }

    SECTION("fee bump has higher bid but lower fee than inner transaction")
    {
        auto txSet =
            std::make_shared<TxSetFrame>(lm.getLastClosedLedgerHeader().hash);
        txSet->add(getTx(root, 100 * fee, 19));
        txSet->add(getTx(root, fee, 1));
        root.loadSequenceNumber();
        txSet->add(root.feeBump(getTx(root, 100 * fee, 19), 100 * fee + 1));

        txSet->trimInvalid(*app);
        REQUIRE(txSet->sizeTx() == 3);
        txSet->sortForHash();
        txSet->surgePricingFilter(*app);
        REQUIRE(txSet->sizeTx() == 2);
        REQUIRE(txSet->sizeOp() == 20);
    }
}

TEST_CASE("surge pricing with fee bumps", "[herder]")
{
    testSurgePricing(12);
}
