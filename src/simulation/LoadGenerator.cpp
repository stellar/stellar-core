// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "simulation/LoadGenerator.h"
#include "main/Config.h"
#include "transactions/TxTests.h"
#include "herder/Herder.h"
#include "ledger/LedgerManager.h"
#include "util/Logging.h"
#include "util/Math.h"
#include "util/types.h"
#include "util/Timer.h"
#include "util/make_unique.h"

#include "xdrpp/printer.h"

#include "medida/metrics_registry.h"
#include "medida/meter.h"

namespace stellar
{

using namespace std;

// Currency amounts are expressed in ten-millionths.
static const uint64_t TENMILLION = 10000000;

// Units of load are is scheduled at 100ms intervals.
const uint32_t LoadGenerator::STEP_MSECS = 100;

LoadGenerator::LoadGenerator() : mMinBalance(0)
{
    auto root = make_shared<AccountInfo>(0, txtest::getRoot(), 100 * TENMILLION,
                                         0, *this);
    mAccounts.push_back(root);
}

LoadGenerator::~LoadGenerator()
{
}

std::string
LoadGenerator::pickRandomCurrency()
{
    static std::vector<std::string> const sCurrencies = {
        "USD", "EUR", "JPY", "CNY", "GBP"
                                    "AUD",
        "CAD", "THB", "MXN", "DKK", "IDR", "XBT", "TRY", "PLN", "HUF"};
    return rand_element(sCurrencies);
}

// Schedule a callback to generateLoad() STEP_MSECS miliseconds from now.
void
LoadGenerator::scheduleLoadGeneration(Application& app, uint32_t nAccounts,
                                      uint32_t nTxs, uint32_t txRate)
{
    if (!mLoadTimer)
    {
        mLoadTimer = make_unique<VirtualTimer>(app.getClock());
    }
    mLoadTimer->expires_from_now(std::chrono::milliseconds(STEP_MSECS));
    mLoadTimer->async_wait(
        [this, &app, nAccounts, nTxs, txRate](asio::error_code const& error)
        {
            if (!error)
            {
                this->generateLoad(app, nAccounts, nTxs, txRate);
            }
        });
}


bool
LoadGenerator::maybeCreateAccount(uint32_t ledgerNum, vector<TxInfo> &txs)
{
    if (mAccounts.size() < 2 || rand_flip())
    {
        auto acc = createAccount(mAccounts.size(), ledgerNum);
        if (rand_uniform(0, 1000) == 0)
        {
            // One account in 1000 is willing to issue credit / be a gateway.
            acc->mIssuedCurrency = pickRandomCurrency();
            mGateways.push_back(acc);
        }
        if (mGateways.size() > 2 && rand_uniform(0, 100) == 0)
        {
            // One account in 100 is willing to act as a market-maker.
            acc->mBuyCredit = rand_element(mGateways);
            do
            {
                acc->mSellCredit = rand_element(mGateways);
            } while (acc->mSellCredit != acc->mBuyCredit);
            acc->mSellCredit->mSellingAccounts.push_back(acc);
            acc->mBuyCredit->mBuyingAccounts.push_back(acc);
            mMarketMakers.push_back(acc);
            mNeedOffer.push_back(acc);
        }
        mAccounts.push_back(acc);
        txs.push_back(acc->creationTransaction());
        return true;
    }
    return false;
}

void
LoadGenerator::fundPendingTrustlines(uint32_t ledgerNum, std::vector<TxInfo> &txs)
{
    std::vector<AccountInfoPtr> remainder;
    for (auto i : mNeedFund)
    {
        bool allFunded = true;
        for (auto& tl : i->mTrustLines)
        {
            if (tl.mBalance != 0)
                continue;
            if (tl.mLedgerEstablished >= ledgerNum)
            {
                allFunded = false;
            }
            else
            {
                txs.push_back(createTransferCreditTransaction(tl.mIssuer, i,
                                                              100 * TENMILLION,
                                                              tl.mIssuer));
            }
        }
        if (!allFunded)
            remainder.push_back(i);
    }
    mNeedFund = remainder;
}

void
LoadGenerator::createPendingOffers(uint32_t ledgerNum, std::vector<TxInfo> &txs)
{
    std::vector<AccountInfoPtr> remainder;
    SequenceNumber seq = static_cast<SequenceNumber>(ledgerNum) << 32;

    for (auto i : mNeedOffer)
    {
        assert(i->mSellCredit);
        assert(i->mBuyCredit);
        if (i->mSeq >= seq ||
            i->mSellCredit->mSeq >= seq ||
            i->mBuyCredit->mSeq >= seq)
        {
            remainder.push_back(i);
            continue;
        }
        txs.push_back(createEstablishOfferTransaction(i));
    }
    mNeedOffer = remainder;
}

// Generate one "step" worth of load (assuming 1 step per STEP_MSECS) at a
// given target number of accounts and txs, and a given target tx/s rate.
// If work remains after the current step, call scheduleLoadGeneration()
// with the remainder.
void
LoadGenerator::generateLoad(Application& app, uint32_t nAccounts, uint32_t nTxs,
                            uint32_t txRate)
{
    updateMinBalance(app);

    // txRate is "per second"; we're running one "step" worth which is a
    // fraction of txRate determined by STEP_MSECS. For example if txRate
    // is 200 and STEP_MSECS is 100, then we want to do 20 tx per step.
    uint32_t txPerStep = (txRate * STEP_MSECS / 1000);
    if (txPerStep > nTxs)
    {
        // We're done.
        LOG(INFO) << "Load generation complete.";
        app.getMetrics().NewMeter({"loadgen", "run", "complete"}, "run").Mark();
    }
    else
    {
        auto& buildTimer = app.getMetrics().NewTimer({"loadgen", "step", "build"});
        auto& recvTimer = app.getMetrics().NewTimer({"loadgen", "step", "recv"});

        uint32_t ledgerNum = app.getLedgerManager().getLedgerNum();
        vector<TxInfo> txs;

        auto buildScope = buildTimer.TimeScope();
        for (uint32_t i = 0; i < txPerStep; ++i)
        {
            if (maybeCreateAccount(ledgerNum, txs))
            {
                if (nAccounts > 0)
                {
                    nAccounts--;
                }
            }
            else
            {
                txs.push_back(createRandomTransaction(0.5, ledgerNum));
                if (nTxs > 0)
                {
                    nTxs--;
                }
            }
        }
        fundPendingTrustlines(ledgerNum, txs);
        createPendingOffers(ledgerNum, txs);
        auto build = buildScope.Stop();

        auto recvScope = recvTimer.TimeScope();
        for (auto& tx : txs)
        {
            if (!tx.execute(app))
            {
                // Hopefully the rejection was just a bad seq number.
                loadAccount(app, *tx.mFrom);
                loadAccount(app, *tx.mTo);
            }
        }
        auto recv = recvScope.Stop();

        // Emit a log message once per second.
        if (((nTxs / txRate) != ((nTxs - txPerStep) / txRate)))
        {
            using namespace std::chrono;

            auto step1ms = duration_cast<milliseconds>(build).count();
            auto step2ms = duration_cast<milliseconds>(recv).count();
            auto totalms = duration_cast<milliseconds>(build+recv).count();
            CLOG(INFO, "LoadGen") << "Target rate: "
                                  << txRate << "txs/s, pending: "
                                  << nAccounts << " accounts, "
                                  << nTxs << " payments";

            CLOG(INFO, "LoadGen") << "Step timing: "
                                  << totalms << "ms total = "
                                  << step1ms << "ms build, "
                                  << step2ms << "ms recv, "
                                  << (STEP_MSECS - totalms) << "ms spare";

            TxMetrics txm(app.getMetrics());
            txm.mPendingOffers.set_count(mNeedOffer.size());
            txm.mPendingFunds.set_count(mNeedFund.size());
            txm.report();
        }

        scheduleLoadGeneration(app, nAccounts, nTxs, txRate);
    }
}

void
LoadGenerator::updateMinBalance(Application& app)
{
    auto b = app.getLedgerManager().getMinBalance(0);
    if (b > mMinBalance)
    {
        mMinBalance = b;
    }
}

LoadGenerator::AccountInfoPtr
LoadGenerator::createAccount(size_t i, uint32_t ledgerNum)
{
    auto accountName = "Account-" + to_string(i);
    return make_shared<AccountInfo>(
        i, txtest::getAccount(accountName.c_str()), 0,
        (static_cast<SequenceNumber>(ledgerNum) << 32), *this);
}

vector<LoadGenerator::AccountInfoPtr>
LoadGenerator::createAccounts(size_t n)
{
    vector<AccountInfoPtr> result;
    for (size_t i = 0; i < n; i++)
    {
        auto account = createAccount(mAccounts.size());
        mAccounts.push_back(account);
        result.push_back(account);
    }
    return result;
}

vector<LoadGenerator::TxInfo>
LoadGenerator::accountCreationTransactions(size_t n)
{
    vector<TxInfo> result;
    for (auto account : createAccounts(n))
    {
        result.push_back(account->creationTransaction());
    }
    return result;
}

bool
LoadGenerator::loadAccount(Application& app, AccountInfo& account)
{
    AccountFrame::pointer ret;
    ret = AccountFrame::loadAccount(account.mKey.getPublicKey(),
                                    app.getDatabase());
    if (!ret)
    {
        return false;
    }

    account.mBalance = ret->getBalance();
    account.mSeq = ret->getSeqNum();
    return true;
}

LoadGenerator::TxInfo
LoadGenerator::createTransferNativeTransaction(AccountInfoPtr from,
                                               AccountInfoPtr to,
                                               int64_t amount)
{
    return TxInfo{from, to, TxInfo::TX_TRANSFER_NATIVE, amount};
}

LoadGenerator::TxInfo
LoadGenerator::createTransferCreditTransaction(AccountInfoPtr from,
                                               AccountInfoPtr to,
                                               int64_t amount,
                                               AccountInfoPtr issuer)
{
    return TxInfo{from, to, TxInfo::TX_TRANSFER_CREDIT, amount, issuer};
}

LoadGenerator::TxInfo
LoadGenerator::createEstablishTrustTransaction(AccountInfoPtr from,
                                               AccountInfoPtr issuer)
{
    return TxInfo{from, nullptr, TxInfo::TX_ESTABLISH_TRUST, 0, issuer};
}

LoadGenerator::TxInfo
LoadGenerator::createEstablishOfferTransaction(AccountInfoPtr from)
{
    return TxInfo { from, nullptr, TxInfo::TX_ESTABLISH_OFFER };
}

LoadGenerator::AccountInfoPtr
LoadGenerator::pickRandomAccount(AccountInfoPtr tryToAvoid, uint32_t ledgerNum)
{
    SequenceNumber currSeq = static_cast<SequenceNumber>(ledgerNum) << 32;
    size_t i = mAccounts.size();
    while (i-- != 0)
    {
        auto n = rand_element(mAccounts);
        if (n->mSeq < currSeq && n != tryToAvoid)
        {
            return n;
        }
    }
    return tryToAvoid;
}

LoadGenerator::AccountInfoPtr
LoadGenerator::pickRandomSharedTrustAccount(AccountInfoPtr from,
                                            uint32_t ledgerNum,
                                            AccountInfoPtr& issuer)
{
    SequenceNumber currSeq = static_cast<SequenceNumber>(ledgerNum) << 32;
    size_t i = mAccounts.size();
    while (i-- != 0)
    {
        auto const& tl = rand_element(from->mTrustLines);
        issuer = tl.mIssuer;
        auto to = rand_element(tl.mIssuer->mTrustingAccounts);
        if (to->mSeq < currSeq && to != from && tl.mBalance != 0)
        {
            return to;
        }
    }
    issuer = nullptr;
    return from;
}

LoadGenerator::TxInfo
LoadGenerator::createRandomTransaction(float alpha, uint32_t ledgerNum)
{
    auto from = pickRandomAccount(mAccounts.at(0), ledgerNum);
    auto amount = rand_uniform<int64_t>(10, 100);

    // Establish up to 5 trustlines on each account.
    if (from->mTrustLines.size() < 5 &&
        !mGateways.empty() &&
        rand_flip())
    {
        auto gw = rand_element(mGateways);
        assert(!gw->mIssuedCurrency.empty());
        auto tl = TrustLineInfo{gw, ledgerNum, 0, 1000 * TENMILLION};
        from->mTrustLines.push_back(tl);
        gw->mTrustingAccounts.push_back(from);
        mNeedFund.push_back(from);
        return createEstablishTrustTransaction(from, gw);
    }
    else if (!from->mTrustLines.empty() && rand_flip())
    {
        // Do a credit-transfer to someone else who trusts the credit
        // that we have.
        AccountInfoPtr issuer;
        auto to = pickRandomSharedTrustAccount(from, ledgerNum, issuer);
        if (issuer)
        {
            return createTransferCreditTransaction(from, to, amount, issuer);
        }
    }
    auto to = pickRandomAccount(from, ledgerNum);
    return createTransferNativeTransaction(from, to, amount);
}

vector<LoadGenerator::TxInfo>
LoadGenerator::createRandomTransactions(size_t n, float paretoAlpha)
{
    vector<TxInfo> result;
    for (size_t i = 0; i < n; i++)
    {
        result.push_back(createRandomTransaction(paretoAlpha));
    }
    return result;
}

//////////////////////////////////////////////////////
// AccountInfo
//////////////////////////////////////////////////////

LoadGenerator::AccountInfo::AccountInfo(size_t id, SecretKey key,
                                        int64_t balance, SequenceNumber seq,
                                        LoadGenerator& loadGen)
    : mId(id), mKey(key), mBalance(balance), mSeq(seq), mLoadGen(loadGen)
{
}

LoadGenerator::TxInfo
LoadGenerator::AccountInfo::creationTransaction()
{
    return TxInfo{mLoadGen.mAccounts[0], shared_from_this(),
                  TxInfo::TX_CREATE_ACCOUNT,
                  100 * mLoadGen.mMinBalance +
                      static_cast<int64_t>(mLoadGen.mAccounts.size() - 1)};
}

//////////////////////////////////////////////////////
// TxInfo
//////////////////////////////////////////////////////

LoadGenerator::TxMetrics::TxMetrics(medida::MetricsRegistry& m)
    : mAccountCreated(m.NewMeter({"loadgen", "account", "created"}, "account"))
    , mTrustlineCreated(m.NewMeter({"loadgen", "trustline", "created"}, "trustline"))
    , mOfferCreated(m.NewMeter({"loadgen", "offer", "created"}, "offer"))
    , mNativePayment(m.NewMeter({"loadgen", "payment", "native"}, "payment"))
    , mCreditPayment(m.NewMeter({"loadgen", "payment", "credit"}, "payment"))
    , mTxnAttempted(m.NewMeter({"loadgen", "txn", "attempted"}, "txn"))
    , mTxnRejected(m.NewMeter({"loadgen", "txn", "rejected"}, "txn"))
    , mPendingFunds(m.NewCounter({"loadgen", "funds", "pending"}))
    , mPendingOffers(m.NewCounter({"loadgen", "offers", "pending"}))
{}

void
LoadGenerator::TxMetrics::report()
{
    CLOG(INFO, "LoadGen")
        << "Counts: "
        << mTxnAttempted.count() << " txn, "
        << mTxnRejected.count() << " rej, "
        << mAccountCreated.count() << " acc, "
        << mTrustlineCreated.count() << " tl ("
        << mPendingFunds.count() << " pending funds), "
        << mOfferCreated.count() << " offer ("
        << mPendingOffers.count() << " pending), "
        << mNativePayment.count() << " native, "
        << mCreditPayment.count() << " credit";

    CLOG(INFO, "LoadGen")
        << "Rates/sec (1min EWMA): "
        << mTxnAttempted.one_minute_rate() << " txn, "
        << mTxnRejected.one_minute_rate() << " rej, "
        << mAccountCreated.one_minute_rate() << " acc, "
        << mTrustlineCreated.one_minute_rate() << " tl, "
        << mOfferCreated.one_minute_rate() << " offer, "
        << mNativePayment.one_minute_rate() << " native, "
        << mCreditPayment.one_minute_rate() << " credit";
}

bool
LoadGenerator::TxInfo::execute(Application& app)
{
    std::vector<TransactionFramePtr> txfs;
    TxMetrics txm(app.getMetrics());
    toTransactionFrames(txfs, txm);
    for (auto f : txfs)
    {
        txm.mTxnAttempted.Mark();
        auto status = app.getHerder().recvTransaction(f);
        if (status != Herder::TX_STATUS_PENDING)
        {

            static const char* TX_STATUS_STRING[Herder::TX_STATUS_COUNT] =
                {"PENDING", "DUPLICATE", "ERROR"};

            CLOG(INFO, "LoadGen") << "tx rejected '"
                                  << TX_STATUS_STRING[status]
                                  << "': "
                                  << xdr::xdr_to_string(f->getEnvelope())
                                  << " ===> "
                                  << xdr::xdr_to_string(f->getResult());
            txm.mTxnRejected.Mark();
            return false;
        }
    }
    recordExecution(app.getConfig().DESIRED_BASE_FEE);
    return true;
}

void
LoadGenerator::TxInfo::toTransactionFrames(std::vector<TransactionFramePtr> &txs,
                                           TxMetrics& txm)
{
    switch (mType)
    {
    case TxInfo::TX_CREATE_ACCOUNT:
        txm.mAccountCreated.Mark();
        txs.push_back(
            txtest::createCreateAccountTx(mFrom->mKey, mTo->mKey,
                                          mFrom->mSeq + 1, mAmount));
        break;

    case TxInfo::TX_TRANSFER_NATIVE:
        txm.mNativePayment.Mark();
        txs.push_back(
            txtest::createPaymentTx(mFrom->mKey, mTo->mKey,
                                    mFrom->mSeq + 1, mAmount));
        break;

    case TxInfo::TX_ESTABLISH_TRUST:
    {
        txm.mTrustlineCreated.Mark();
        assert(!mIssuer->mIssuedCurrency.empty());
        txs.push_back(txtest::createChangeTrust(
            mFrom->mKey, mIssuer->mKey, mFrom->mSeq + 1,
            mIssuer->mIssuedCurrency, 10000 * TENMILLION));
    }
    break;

    case TxInfo::TX_ESTABLISH_OFFER:
    {
        txm.mOfferCreated.Mark();
        Price price;
        price.d = 10000;
        uint64_t diff = rand_uniform(1, 200);
        price.n = rand_flip() ? (price.d + diff) : (price.d - diff);

        uint64_t amount = 10000 * TENMILLION;

        assert(!mFrom->mSellCredit->mIssuedCurrency.empty());
        assert(!mFrom->mBuyCredit->mIssuedCurrency.empty());
        Currency sellCurrency =
            txtest::makeCurrency(mFrom->mSellCredit->mKey,
                                 mFrom->mSellCredit->mIssuedCurrency);
        Currency buyCurrency =
            txtest::makeCurrency(mFrom->mBuyCredit->mKey,
                                 mFrom->mBuyCredit->mIssuedCurrency);

        txs.push_back(
            txtest::createPassiveOfferOp(mFrom->mKey,
                                         sellCurrency,
                                         buyCurrency,
                                         price, amount,
                                         mFrom->mSeq));
    }
    break;

    case TxInfo::TX_TRANSFER_CREDIT:
    {
        txm.mCreditPayment.Mark();
        assert(!mIssuer->mIssuedCurrency.empty());
        Currency ci =
            txtest::makeCurrency(mIssuer->mKey, mIssuer->mIssuedCurrency);
        txs.push_back(txtest::createCreditPaymentTx(mFrom->mKey, mTo->mKey, ci,
                                                    mFrom->mSeq + 1, mAmount));
    }
    break;

    default:
        assert(false);
    }
}

void
LoadGenerator::TxInfo::recordExecution(int64_t baseFee)
{
    mFrom->mSeq++;
    mFrom->mBalance -= baseFee;
    if (mFrom && mTo)
    {
        if (mIssuer)
        {
            for (auto& tl : mFrom->mTrustLines)
            {
                if (tl.mIssuer == mIssuer)
                {
                    tl.mBalance -= mAmount;
                }
            }
            for (auto& tl : mTo->mTrustLines)
            {
                if (tl.mIssuer == mIssuer)
                {
                    tl.mBalance += mAmount;
                }
            }
        }
        else
        {
            mFrom->mBalance -= mAmount;
            mTo->mBalance += mAmount;
        }
    }
}
}
