// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "main/Config.h"
#include "test/TestAccount.h"
#include "test/TestUtils.h"
#include "test/TxTests.h"
#include "test/test.h"
#include "transactions/TransactionUtils.h"
#include "util/Logging.h"

using namespace stellar;
using namespace stellar::txtest;

static uint32_t
getNextLedgerSeq(Application& app)
{
    LedgerTxn ltx(app.getLedgerTxnRoot());
    return ltx.loadHeader().current().ledgerSeq + 1;
}

static uint64_t
getNextSeqNum(Application& app, SecretKey const& key)
{
    LedgerTxn ltx(app.getLedgerTxnRoot());
    auto ltxe = stellar::loadAccount(ltx, key.getPublicKey());
    return ltxe.current().data.account().seqNum + 1;
};

static TransactionFramePtr
createAccountsTx(Application& app, SecretKey const& src, uint64_t seqNum,
                 int64_t balance, std::vector<SecretKey>::const_iterator& begin,
                 std::vector<SecretKey>::const_iterator const& end)
{
    assert(begin != end);
    std::vector<Operation> ops;
    for (; ops.size() < 100 && begin != end; ++begin)
    {
        ops.emplace_back(createAccount(begin->getPublicKey(), balance));
    }
    return transactionFromOperations(app, src, seqNum, ops);
};

static void
createAccounts(Application& app, SecretKey const& src, int64_t balance,
               std::vector<SecretKey> const& dest)
{
    size_t nBegin = app.getLedgerTxnRoot().countObjects(ACCOUNT);
    CLOG(ERROR, "Tx") << "BEGIN - Creating " << dest.size() << " accounts";

    std::vector<TransactionFramePtr> txs;
    size_t nOps = 0;

    uint64_t seqNum = getNextSeqNum(app, src);
    for (auto iter = dest.cbegin(); iter != dest.cend();)
    {
        txs.emplace_back(
            createAccountsTx(app, src, seqNum++, balance, iter, dest.cend()));
        nOps += txs.back()->getEnvelope().tx.operations.size();
        if (nOps + 100 > 5000)
        {
            closeLedgerOn(app, getNextLedgerSeq(app), 1, 1, 2019, txs);
            txs.clear();
            nOps = 0;
        }
    }
    if (!txs.empty())
    {
        closeLedgerOn(app, getNextLedgerSeq(app), 1, 1, 2019, txs);
    }

    CLOG(ERROR, "Tx") << "END - Creating " << dest.size() << " accounts";
    size_t delta = app.getLedgerTxnRoot().countObjects(ACCOUNT) - nBegin;
    CLOG(ERROR, "Tx") << "Created " << delta << " accounts";
}

static TransactionFramePtr
createTrustLinesTx(Application& app, SecretKey const& src,
                   std::vector<Asset> const& assets)
{
    assert(!assets.empty() && assets.size() <= 100);
    std::vector<Operation> ops;
    for (auto const& asset : assets)
    {
        ops.emplace_back(changeTrust(asset, INT64_MAX));
    }
    return transactionFromOperations(app, src, getNextSeqNum(app, src), ops);
};

static void
createTrustLines(Application& app, std::vector<SecretKey> const& accounts,
                 std::vector<Asset> const& assets)
{
    size_t nBegin = app.getLedgerTxnRoot().countObjects(TRUSTLINE);
    size_t nCreate = accounts.size() * assets.size();
    CLOG(ERROR, "Tx") << "BEGIN - Creating " << nCreate << " trustlines";

    std::vector<TransactionFramePtr> txs;
    size_t nOps = 0;

    for (auto const& acc : accounts)
    {
        txs.emplace_back(createTrustLinesTx(app, acc, assets));
        nOps += txs.back()->getEnvelope().tx.operations.size();
        if (nOps + assets.size() > 5000)
        {
            closeLedgerOn(app, getNextLedgerSeq(app), 1, 1, 2019, txs);
            txs.clear();
            nOps = 0;
        }
    }
    if (!txs.empty())
    {
        closeLedgerOn(app, getNextLedgerSeq(app), 1, 1, 2019, txs);
    }

    CLOG(ERROR, "Tx") << "END - Creating " << nCreate << " trustlines";
    size_t delta = app.getLedgerTxnRoot().countObjects(TRUSTLINE) - nBegin;
    CLOG(ERROR, "Tx") << "Created " << delta << " trustlines";
};

static TransactionFramePtr
sendPaymentsTx(Application& app, SecretKey const& src, SecretKey const& dest,
               std::map<Asset, int64_t> const& amountByAsset, uint64_t seqNum)
{
    assert(!amountByAsset.empty() && amountByAsset.size() <= 100);
    std::vector<Operation> ops;
    for (auto const& kv : amountByAsset)
    {
        ops.emplace_back(payment(dest.getPublicKey(), kv.first, kv.second));
    }
    return transactionFromOperations(app, src, seqNum, ops);
};

static void
sendPayments(Application& app, SecretKey const& src,
             std::vector<SecretKey> const& dest,
             std::map<Asset, int64_t> const& amountByAsset)
{
    size_t nSend = dest.size() * amountByAsset.size();
    CLOG(ERROR, "Tx") << "BEGIN - Sending " << nSend << " payments";

    std::vector<TransactionFramePtr> txs;
    size_t nOps = 0;

    uint64_t seqNum = getNextSeqNum(app, src);
    for (auto const& acc : dest)
    {
        txs.emplace_back(
            sendPaymentsTx(app, src, acc, amountByAsset, seqNum++));
        nOps += txs.back()->getEnvelope().tx.operations.size();
        if (nOps + amountByAsset.size() > 5000)
        {
            closeLedgerOn(app, getNextLedgerSeq(app), 1, 1, 2019, txs);
            txs.clear();
            nOps = 0;
        }
    }
    if (!txs.empty())
    {
        closeLedgerOn(app, getNextLedgerSeq(app), 1, 1, 2019, txs);
    }

    CLOG(ERROR, "Tx") << "END - Sending " << nSend << " payments";
};

static TransactionFramePtr
createOffersTx(Application& app, SecretKey const& src, Asset const& selling,
               Asset const& buying, int64_t amount, size_t nOffers,
               uint64_t seqNum)
{
    assert(nOffers <= 100);
    std::vector<Operation> ops;
    for (size_t i = 0; i < nOffers; ++i)
    {
        ops.emplace_back(manageOffer(0, selling, buying, Price{1, 1}, amount));
    }
    return transactionFromOperations(app, src, seqNum, ops);
};

static void
createOffers(Application& app, std::vector<SecretKey> const& src,
             Asset const& selling, Asset const& buying, int64_t amount,
             size_t nOffers)
{
    assert(nOffers <= 998);

    std::vector<TransactionFramePtr> txs;
    size_t nOps = 0;

    for (auto const& acc : src)
    {
        uint64_t seqNum = getNextSeqNum(app, acc);
        for (size_t i = 0; i < nOffers;)
        {
            size_t n = std::min<size_t>(100, nOffers - i);
            i += n;

            txs.emplace_back(
                createOffersTx(app, acc, selling, buying, amount, n, seqNum++));
            nOps += n;
            if (nOps + 100 > 5000)
            {
                closeLedgerOn(app, getNextLedgerSeq(app), 1, 1, 2019, txs);
                txs.clear();
                nOps = 0;
            }
        }
    }

    if (!txs.empty())
    {
        closeLedgerOn(app, getNextLedgerSeq(app), 1, 1, 2019, txs);
        txs.clear();
    }
};

static void
runBenchmark(size_t nAccounts, size_t nOffersPerAccount, size_t nOffersPerCross,
             Config::TestDbMode dbMode)
{
    size_t const NUM_OFFERS = nAccounts * nOffersPerAccount;
    REQUIRE(NUM_OFFERS % nOffersPerCross == 0);
    size_t const NUM_CROSSING_OFFERS = NUM_OFFERS / nOffersPerCross;

    VirtualClock clock;
    Config cfg = getTestConfig(0, dbMode);
    cfg.TESTING_UPGRADE_MAX_TX_SET_SIZE = 5000;
    cfg.ENTRY_CACHE_SIZE = 1000000;
    auto app = createTestApplication(clock, cfg);
    app->start();

    auto& lm = app->getLedgerManager();
    auto txfee = lm.getLastTxFee();
    auto minBalance1000 = lm.getLastMinBalance(1000) + 5000 * txfee;

    // Get root key
    auto root = getRoot(app->getNetworkID());

    // Create issuer account and prepare assets
    auto issuer = getAccount("issuer");
    createAccounts(*app, root, minBalance1000, {issuer});
    auto cur1 = makeAsset(issuer, "CUR1");
    auto cur2 = makeAsset(issuer, "CUR2");

    // Create mm accounts, trustlines, and offers
    std::vector<SecretKey> mmKeys;
    for (size_t i = 0; i < nAccounts; ++i)
    {
        mmKeys.emplace_back(getAccount("mm" + std::to_string(i)));
    }
    createAccounts(*app, root, minBalance1000, mmKeys);
    createTrustLines(*app, mmKeys, {cur1, cur2});
    sendPayments(*app, issuer, mmKeys, {{cur1, nOffersPerAccount}});
    {
        size_t nBegin = app->getLedgerTxnRoot().countObjects(OFFER);
        CLOG(ERROR, "Tx") << "BEGIN - Creating " << NUM_OFFERS << " offers";
        createOffers(*app, mmKeys, cur1, cur2, 1, nOffersPerAccount);
        CLOG(ERROR, "Tx") << "END - Creating " << NUM_OFFERS << " offers";
        size_t delta = app->getLedgerTxnRoot().countObjects(OFFER) - nBegin;
        CLOG(ERROR, "Tx") << "Created " << delta << " offers";
    }

    // Create crossing account, trustlines, and offer
    auto acc = getAccount("a");
    createAccounts(*app, root, minBalance1000, {acc});
    createTrustLines(*app, {acc}, {cur1, cur2});
    sendPayments(*app, issuer, {acc}, {{cur2, NUM_OFFERS}});
    {
        size_t nBegin = app->getLedgerTxnRoot().countObjects(OFFER);
        CLOG(ERROR, "Tx") << "BEGIN - Crossing " << NUM_OFFERS << " offers";
        createOffers(*app, {acc}, cur2, cur1, nOffersPerCross,
                     NUM_CROSSING_OFFERS);
        CLOG(ERROR, "Tx") << "END - Crossing " << NUM_OFFERS << " offers";
        size_t delta = nBegin - app->getLedgerTxnRoot().countObjects(OFFER);
        CLOG(ERROR, "Tx") << "Crossed " << delta << " offers";
    }
}

static void
runBenchmarkForAllDatabaseTypes(size_t nAccounts, size_t nOffersPerAccount,
                                size_t nOffersPerCross)
{
    SECTION("in memory sqlite")
    {
        runBenchmark(nAccounts, nOffersPerAccount, nOffersPerCross,
                     Config::TESTDB_IN_MEMORY_SQLITE);
    }

    SECTION("on disk sqlite")
    {
        runBenchmark(nAccounts, nOffersPerAccount, nOffersPerCross,
                     Config::TESTDB_ON_DISK_SQLITE);
    }

    SECTION("postgres")
    {
        runBenchmark(nAccounts, nOffersPerAccount, nOffersPerCross,
                     Config::TESTDB_POSTGRESQL);
    }
}

TEST_CASE("cross many offers many per account benchmark",
          "[crossoffersbench][!hide]")
{
    runBenchmarkForAllDatabaseTypes(100, 998, 998);
}

TEST_CASE("cross many offers one per account benchmark",
          "[crossoffersbench][!hide]")
{
    runBenchmarkForAllDatabaseTypes(100000, 1, 1000);
}
