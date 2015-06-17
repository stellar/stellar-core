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

#include "transactions/TransactionFrame.h"
#include "transactions/PathPaymentOpFrame.h"
#include "transactions/PaymentOpFrame.h"
#include "transactions/ChangeTrustOpFrame.h"
#include "transactions/CreateAccountOpFrame.h"
#include "transactions/ManageOfferOpFrame.h"
#include "transactions/AllowTrustOpFrame.h"

#include "xdrpp/printer.h"

#include "medida/metrics_registry.h"
#include "medida/meter.h"

#include <set>

namespace stellar
{

using namespace std;

// Account amounts are expressed in ten-millionths (10^-7).
static const uint64_t TENMILLION = 10000000;

// Every loadgen account or trustline gets a 1000 unit balance (10^3).
static const uint64_t LOADGEN_ACCOUNT_BALANCE = 1000 * TENMILLION;

// Trustlines are limited to 1000x the balance.
static const uint64_t LOADGEN_TRUSTLINE_LIMIT = 1000 * LOADGEN_ACCOUNT_BALANCE;

// Units of load are is scheduled at 100ms intervals.
const uint32_t LoadGenerator::STEP_MSECS = 100;

LoadGenerator::LoadGenerator() : mMinBalance(0)
{
    // Root account gets enough XLM to create 100 million (10^8) accounts, which
    // thereby uses up 7 + 3 + 8 = 18 decimal digits. Luckily we have 2^63 =
    // 9.2*10^18, so there's room even in 63bits to do this.
    auto root =
        make_shared<AccountInfo>(0, txtest::getRoot(),
                                 100000000ULL * LOADGEN_ACCOUNT_BALANCE,
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

        // One account in 1000 is willing to issue credit / be a gateway. (with
        // the first 3 gateways created immediately)
        if (mGateways.size() < 3 + (mAccounts.size() / 1000))
        {
            acc->mIssuedCurrency = pickRandomCurrency();
            mGateways.push_back(acc);
        }

        // Pick a few gateways to trust, if there are any.
        if (!mGateways.empty())
        {
            size_t n = rand_uniform<size_t>(0, 10);
            for (size_t i = 0; i < n; ++i)
            {
                auto gw = rand_element(mGateways);
                acc->establishTrust(gw);
            }
        }

        // One account in 100 is willing to act as a market-maker; these need to
        // immediately extend trustlines to the units being traded-in.
        if (mGateways.size() > 2 && mMarketMakers.size() < (mAccounts.size() / 100))
        {
            acc->mBuyCredit = rand_element(mGateways);
            do
            {
                acc->mSellCredit = rand_element(mGateways);
            } while (acc->mSellCredit == acc->mBuyCredit);
            acc->mSellCredit->mSellingAccounts.push_back(acc);
            acc->mBuyCredit->mBuyingAccounts.push_back(acc);
            mMarketMakers.push_back(acc);
            acc->establishTrust(acc->mBuyCredit);
            acc->establishTrust(acc->mSellCredit);
        }
        mAccounts.push_back(acc);
        txs.push_back(acc->creationTransaction());
        return true;
    }
    return false;
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
        auto build = buildScope.Stop();

        auto recvScope = recvTimer.TimeScope();
        for (auto& tx : txs)
        {
            if (!tx.execute(app))
            {
                // Hopefully the rejection was just a bad seq number.
                loadAccount(app, *tx.mFrom);
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
            txm.mGateways.set_count(mGateways.size());
            txm.mMarketMakers.set_count(mMarketMakers.size());
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

    if (!from->mTrustLines.empty()
        && rand_flip())
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
    return TxInfo {
        mLoadGen.mAccounts[0],
        shared_from_this(),
        TxInfo::TX_CREATE_ACCOUNT,
        LOADGEN_ACCOUNT_BALANCE
    };
}

void
LoadGenerator::AccountInfo::establishTrust(AccountInfoPtr a)
{
    if (a == shared_from_this())
        return;

    for (auto const& tl : mTrustLines)
    {
        if (tl.mIssuer == a)
            return;
    }
    auto tl = TrustLineInfo {a, LOADGEN_ACCOUNT_BALANCE, LOADGEN_TRUSTLINE_LIMIT};
    mTrustLines.push_back(tl);
    a->mTrustingAccounts.push_back(shared_from_this());
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
    , mGateways(m.NewCounter({"loadgen", "account", "gateways"}))
    , mMarketMakers(m.NewCounter({"loadgen", "account", "marketmakers"}))
{}

void
LoadGenerator::TxMetrics::report()
{
    CLOG(INFO, "LoadGen")
        << "Counts: "
        << mTxnAttempted.count() << " txn, "
        << mTxnRejected.count() << " rej, "
        << mAccountCreated.count() << " acc ("
        << mGateways.count() << " gw, "
        << mMarketMakers.count() << " mm), "
        << mTrustlineCreated.count() << " tl, "
        << mOfferCreated.count() << " offer, "
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
        {
            TransactionEnvelope e;
            std::set<AccountInfoPtr> signingAccounts;

            e.tx.sourceAccount = mFrom->mKey.getPublicKey();
            signingAccounts.insert(mFrom);
            e.tx.seqNum = mFrom->mSeq + 1;

            // Add a CREATE_ACCOUNT op
            Operation createOp;
            createOp.body.type(CREATE_ACCOUNT);
            createOp.body.createAccountOp().startingBalance = mAmount;
            createOp.body.createAccountOp().destination = mTo->mKey.getPublicKey();
            e.tx.operations.push_back(createOp);

            // Add a CHANGE_TRUST op for each of the account's trustlines,
            // and a PAYMENT from the trustline's issuer to the account, to fund it.
            for (auto const& tl : mTo->mTrustLines)
            {
                txm.mTrustlineCreated.Mark();
                Operation trustOp, paymentOp;
                Currency ci = txtest::makeCurrency(tl.mIssuer->mKey,
                                                   tl.mIssuer->mIssuedCurrency);
                trustOp.body.type(CHANGE_TRUST);
                trustOp.sourceAccount.activate() = mTo->mKey.getPublicKey();
                trustOp.body.changeTrustOp().limit = LOADGEN_TRUSTLINE_LIMIT;
                trustOp.body.changeTrustOp().line = ci;

                paymentOp.body.type(PAYMENT);
                paymentOp.sourceAccount.activate() = tl.mIssuer->mKey.getPublicKey();
                paymentOp.body.paymentOp().amount = LOADGEN_ACCOUNT_BALANCE;
                paymentOp.body.paymentOp().currency = ci;
                paymentOp.body.paymentOp().destination = mTo->mKey.getPublicKey();

                e.tx.operations.push_back(trustOp);
                e.tx.operations.push_back(paymentOp);
                signingAccounts.insert(tl.mIssuer);
                signingAccounts.insert(mTo);
            }

            // Add a CREATE_PASSIVE_OFFER op if this account is a market-maker.
            if (mTo->mBuyCredit)
            {
                txm.mOfferCreated.Mark();
                Operation offerOp;
                Currency buyCi = txtest::makeCurrency(mTo->mBuyCredit->mKey,
                                                      mTo->mBuyCredit->mIssuedCurrency);

                Currency sellCi = txtest::makeCurrency(mTo->mSellCredit->mKey,
                                                       mTo->mSellCredit->mIssuedCurrency);

                Price price;
                price.d = 10000;
                uint64_t diff = rand_uniform(1, 200);
                price.n = rand_flip() ? (price.d + diff) : (price.d - diff);

                offerOp.body.type(CREATE_PASSIVE_OFFER);
                offerOp.sourceAccount.activate() = mTo->mKey.getPublicKey();
                offerOp.body.createPassiveOfferOp().amount = LOADGEN_ACCOUNT_BALANCE;
                offerOp.body.createPassiveOfferOp().takerGets = sellCi;
                offerOp.body.createPassiveOfferOp().takerPays = buyCi;
                offerOp.body.createPassiveOfferOp().price = price;
                e.tx.operations.push_back(offerOp);
                signingAccounts.insert(mTo);
            }

            e.tx.fee = 10 * e.tx.operations.size();
            TransactionFramePtr res = TransactionFrame::makeTransactionFromWire(e);
            for (auto a : signingAccounts)
            {
                res->addSignature(a->mKey);
            }
            txs.push_back(res);
        }
        break;

    case TxInfo::TX_TRANSFER_NATIVE:
        txm.mNativePayment.Mark();
        txs.push_back(
            txtest::createPaymentTx(mFrom->mKey, mTo->mKey,
                                    mFrom->mSeq + 1, mAmount));
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
