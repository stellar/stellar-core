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
#include <iomanip>
#include <cmath>

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

LoadGenerator::LoadGenerator() : mMinBalance(0), mLastSecond(0)
{
    // Root account gets enough XLM to create 100 million (10^8) accounts, which
    // thereby uses up 7 + 3 + 8 = 18 decimal digits. Luckily we have 2^63 =
    // 9.2*10^18, so there's room even in 63bits to do this.
    auto root = make_shared<AccountInfo>(
        0, txtest::getRoot(), 100000000ULL * LOADGEN_ACCOUNT_BALANCE, 0, *this);
    mAccounts.push_back(root);
}

LoadGenerator::~LoadGenerator()
{
    clear();
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
                                      uint32_t nTxs, uint32_t txRate,
                                      bool autoRate)
{
    if (!mLoadTimer)
    {
        mLoadTimer = make_unique<VirtualTimer>(app.getClock());
    }
    mLoadTimer->expires_from_now(std::chrono::milliseconds(STEP_MSECS));
    mLoadTimer->async_wait([this, &app, nAccounts, nTxs, txRate, autoRate](
        asio::error_code const& error)
                           {
                               if (!error)
                               {
                                   this->generateLoad(app, nAccounts, nTxs,
                                                      txRate, autoRate);
                               }
                           });
}

bool
LoadGenerator::maybeCreateAccount(uint32_t ledgerNum, vector<TxInfo>& txs)
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
        if (mGateways.size() > 2 &&
            mMarketMakers.size() < (mAccounts.size() / 100))
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

bool
maybeAdjustRate(double target, double actual, uint32_t& rate, bool increaseOk)
{
    if (actual == 0.0)
    {
        actual = 1.0;
    }
    double diff = target - actual;
    double acceptableDeviation = 0.1 * target;
    if (fabs(diff) > acceptableDeviation)
    {
        double pct = diff / actual;
        int32_t incr = static_cast<int32_t>(pct * rate);
        if (incr > 0 && !increaseOk)
        {
            return false;
        }
        LOG(INFO) << (incr > 0 ? "+++ Increasing" : "--- Decreasing")
                  << " auto-tx target rate from " << rate << " to "
                  << rate + incr;
        rate += incr;
        return true;
    }
    return false;
}

void
LoadGenerator::clear()
{
    for (auto& a : mAccounts)
    {
        a->mTrustingAccounts.clear();
        a->mBuyingAccounts.clear();
        a->mSellingAccounts.clear();
        a->mTrustingAccounts.clear();
    }
    mAccounts.clear();
    mGateways.clear();
    mMarketMakers.clear();
}

// Generate one "step" worth of load (assuming 1 step per STEP_MSECS) at a
// given target number of accounts and txs, and a given target tx/s rate.
// If work remains after the current step, call scheduleLoadGeneration()
// with the remainder.
void
LoadGenerator::generateLoad(Application& app, uint32_t nAccounts, uint32_t nTxs,
                            uint32_t txRate, bool autoRate)
{
    updateMinBalance(app);

    if (txRate == 0)
    {
        txRate = 1;
    }

    // txRate is "per second"; we're running one "step" worth which is a
    // fraction of txRate determined by STEP_MSECS. For example if txRate
    // is 200 and STEP_MSECS is 100, then we want to do 20 tx per step.
    uint32_t txPerStep = (txRate * STEP_MSECS / 1000);

    // If we have a very low tx rate (eg. 2/sec) then the previous division will
    // be zero and we'll never issue anything; what we need to do instead is
    // dispatch 1 tx every "few steps" (eg. every 5 steps). We do this by random
    // choice, weighted to the desired frequency.
    if (txPerStep == 0)
    {
        txPerStep = rand_uniform(0U, 1000U) < (txRate * STEP_MSECS) ? 1 : 0;
    }

    if (txPerStep > nTxs)
    {
        // We're done.
        LOG(INFO) << "Load generation complete.";
        app.getMetrics().NewMeter({"loadgen", "run", "complete"}, "run").Mark();
        clear();
    }
    else
    {
        auto& buildTimer =
            app.getMetrics().NewTimer({"loadgen", "step", "build"});
        auto& recvTimer =
            app.getMetrics().NewTimer({"loadgen", "step", "recv"});

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
                std::vector<AccountInfoPtr> accs{tx.mFrom, tx.mTo};
                if (!tx.mPath.empty())
                {
                    accs.insert(accs.end(), tx.mPath.begin(), tx.mPath.end());
                }
                for (auto i : accs)
                {
                    loadAccount(app, i);
                    if (i)
                    {
                        loadAccount(app, i->mBuyCredit);
                        loadAccount(app, i->mSellCredit);
                        for (auto const& tl : i->mTrustLines)
                        {
                            loadAccount(app, tl.mIssuer);
                        }
                    }
                }
            }
        }
        auto recv = recvScope.Stop();

        uint64_t now = static_cast<uint64_t>(
            VirtualClock::to_time_t(app.getClock().now()));
        bool secondBoundary = now != mLastSecond;
        mLastSecond = now;

        if (autoRate && secondBoundary)
        {
            // Automatic tx rate calculation involves taking the temperature
            // of the program and deciding if there's "room" to increase the
            // tx apply rate.
            auto& m = app.getMetrics();
            auto& ledgerCloseTimer = m.NewTimer({"ledger", "ledger", "close"});

            if (ledgerNum > 10 && ledgerCloseTimer.count() > 5)
            {
                // We consider the system "well loaded" at the point where its
                // ledger-close timer has median duration within 10% of 250ms.
                //
                // This is a bit arbitrary but it seems sufficient to
                // empirically differentiate "totally easy" from "starting to
                // struggle". If it's over this point, we reduce load; if it's
                // under this point, we increase load.
                //
                // We also decrease load (but don't increase it) based on ledger
                // age: if the age gets above the herder's timer target, we shed
                // load accordingly because the network is not reaching
                // consensus fast enough.

                double targetLatency = 250.0;
                double actualLatency =
                    ledgerCloseTimer.GetSnapshot().getMedian();

                double targetAge =
                    (double)Herder::EXP_LEDGER_TIMESPAN_SECONDS.count();
                double actualAge =
                    (double)
                        app.getLedgerManager().secondsSinceLastLedgerClose();
                if (app.getConfig().ARTIFICIALLY_ACCELERATE_TIME_FOR_TESTING)
                {
                    targetAge = 1.0;
                }

                LOG(DEBUG)
                    << "Considering auto-tx adjustment, median close time "
                    << actualLatency << "ms, ledger age " << actualAge << "s";

                if (!maybeAdjustRate(targetAge, actualAge, txRate, false))
                {
                    maybeAdjustRate(targetLatency, actualLatency, txRate, true);
                }

                if (txRate > 5000)
                {
                    LOG(WARNING)
                        << "TxRate > 5000, likely metric stutter, resetting";
                    txRate = 10;
                }

                // Unfortunately the timer reservoir size is 1028 by default and
                // we cannot adjust it here, so in order to adapt to load
                // relatively quickly, we clear it out every 5 ledgers.
                ledgerCloseTimer.Clear();
            }
        }

        // Emit a log message once per second.
        if (secondBoundary)
        {
            using namespace std::chrono;

            auto& m = app.getMetrics();
            auto& apply = m.NewTimer({"ledger", "transaction", "apply"});

            auto step1ms = duration_cast<milliseconds>(build).count();
            auto step2ms = duration_cast<milliseconds>(recv).count();
            auto totalms = duration_cast<milliseconds>(build + recv).count();

            uint32_t etaSecs = (uint32_t)(((double)(nTxs + nAccounts)) /
                                          apply.one_minute_rate());
            uint32_t etaHours = etaSecs / 3600;
            uint32_t etaMins = etaSecs % 60;

            CLOG(INFO, "LoadGen")
                << "Tx/s: " << txRate << " target"
                << (autoRate ? " (auto), " : ", ") << std::setprecision(3)
                << apply.one_minute_rate() << " actual (1m EWMA)."
                << " Pending: " << nAccounts << " acct, " << nTxs << " tx."
                << " ETA: " << etaHours << "h" << etaMins << "m";

            CLOG(DEBUG, "LoadGen") << "Step timing: " << totalms
                                   << "ms total = " << step1ms << "ms build, "
                                   << step2ms << "ms recv, "
                                   << (STEP_MSECS - totalms) << "ms spare";

            TxMetrics txm(app.getMetrics());
            txm.mGateways.set_count(mGateways.size());
            txm.mMarketMakers.set_count(mMarketMakers.size());
            txm.report();
        }

        scheduleLoadGeneration(app, nAccounts, nTxs, txRate, autoRate);
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
    auto high =
        app.getHerder().getMaxSeqInPendingTxs(account.mKey.getPublicKey());
    if (high > account.mSeq)
    {
        account.mSeq = high;
    }
    return true;
}

bool
LoadGenerator::loadAccount(Application& app, AccountInfoPtr acc)
{
    if (acc)
    {
        return loadAccount(app, *acc);
    }
    return false;
}

bool
LoadGenerator::loadAccounts(Application& app, std::vector<AccountInfoPtr> accs)
{
    bool loaded = !accs.empty();
    for (auto a : accs)
    {
        if (!loadAccount(app, a))
        {
            loaded = false;
        }
    }
    return loaded;
}

LoadGenerator::TxInfo
LoadGenerator::createTransferNativeTransaction(AccountInfoPtr from,
                                               AccountInfoPtr to,
                                               int64_t amount)
{
    return TxInfo{from, to, TxInfo::TX_TRANSFER_NATIVE, amount};
}

LoadGenerator::TxInfo
LoadGenerator::createTransferCreditTransaction(
    AccountInfoPtr from, AccountInfoPtr to, int64_t amount,
    std::vector<AccountInfoPtr> const& path)
{
    return TxInfo{from, to, TxInfo::TX_TRANSFER_CREDIT, amount, path};
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

bool
acceptablePathExtension(LoadGenerator::AccountInfoPtr from, uint32_t ledgerNum,
                        std::vector<LoadGenerator::AccountInfoPtr> const& path,
                        LoadGenerator::AccountInfoPtr proposed)
{
    SequenceNumber currSeq = static_cast<SequenceNumber>(ledgerNum) << 32;
    if (proposed->mSeq >= currSeq || from == proposed)
    {
        return false;
    }
    for (auto i : path)
    {
        if (i == proposed)
        {
            return false;
        }
    }
    return true;
}

LoadGenerator::AccountInfoPtr
pickMarketMakerForIssuer(LoadGenerator::AccountInfoPtr from, uint32_t ledgerNum,
                         std::vector<LoadGenerator::AccountInfoPtr> const& path,
                         LoadGenerator::AccountInfoPtr issuer)
{
    assert(issuer);
    size_t i = issuer->mBuyingAccounts.size();
    while (i-- != 0)
    {
        auto mm = rand_element(issuer->mBuyingAccounts);
        if (acceptablePathExtension(from, ledgerNum, path, mm))
        {
            assert(mm->mBuyCredit == issuer);
            return mm;
        }
    }
    return nullptr;
}

void
randomPathWalk(LoadGenerator::AccountInfoPtr from, uint32_t ledgerNum,
               std::vector<LoadGenerator::AccountInfoPtr>& path,
               LoadGenerator::AccountInfoPtr& to)
{
    auto issuer =
        (path.empty() ? rand_element(from->mTrustLines).mIssuer : path.back());

    auto mm = pickMarketMakerForIssuer(from, ledgerNum, path, issuer);
    if (mm && rand_flip() && path.size() < 5)
    {
        // We have a market maker -- mm is buying 'issuer' credits -- and we
        // want to let mm buy it and sell credit that someone else trusts;
        // we then see about extending the walk from that someone.
        assert(mm->mSellCredit);
        size_t i = mm->mSellCredit->mTrustingAccounts.size();
        while (i-- != 0)
        {
            auto maybeTo = rand_element(mm->mSellCredit->mTrustingAccounts);
            if (maybeTo != mm &&
                acceptablePathExtension(from, ledgerNum, path, maybeTo))
            {
                path.push_back(issuer);
                to = maybeTo;
                randomPathWalk(from, ledgerNum, path, to);
                return;
            }
        }
    }
    // No market-maker, just find a destination that can accept credits from the
    // issuer we've picked.
    size_t i = issuer->mTrustingAccounts.size();
    while (i-- != 0)
    {
        auto maybeTo = rand_element(issuer->mTrustingAccounts);
        if (maybeTo != from)
        {
            to = maybeTo;
            path.push_back(issuer);
            break;
        }
    }
}

LoadGenerator::AccountInfoPtr
LoadGenerator::pickRandomPath(LoadGenerator::AccountInfoPtr from,
                              uint32_t ledgerNum,
                              std::vector<LoadGenerator::AccountInfoPtr>& path)
{
    size_t i = mAccounts.size();
    auto to = from;
    do
    {
        path.clear();
        randomPathWalk(from, ledgerNum, path, to);
    } while (i-- != 0 && from == to);
    return to;
}

LoadGenerator::TxInfo
LoadGenerator::createRandomTransaction(float alpha, uint32_t ledgerNum)
{
    auto from = pickRandomAccount(mAccounts.at(0), ledgerNum);
    auto amount = rand_uniform<int64_t>(10, 100);

    if (!from->mTrustLines.empty() && rand_flip())
    {
        // Do a credit-transfer to someone else who trusts the credit
        // that we have, or some path between us.
        std::vector<AccountInfoPtr> path;
        auto to = pickRandomPath(from, ledgerNum, path);
        if (to != from && !path.empty())
        {
            return createTransferCreditTransaction(from, to, amount, path);
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
                  TxInfo::TX_CREATE_ACCOUNT, LOADGEN_ACCOUNT_BALANCE};
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
    auto tl =
        TrustLineInfo{a, LOADGEN_ACCOUNT_BALANCE, LOADGEN_TRUSTLINE_LIMIT};
    mTrustLines.push_back(tl);
    a->mTrustingAccounts.push_back(shared_from_this());
}

//////////////////////////////////////////////////////
// TxInfo
//////////////////////////////////////////////////////

LoadGenerator::TxMetrics::TxMetrics(medida::MetricsRegistry& m)
    : mAccountCreated(m.NewMeter({"loadgen", "account", "created"}, "account"))
    , mTrustlineCreated(
          m.NewMeter({"loadgen", "trustline", "created"}, "trustline"))
    , mOfferCreated(m.NewMeter({"loadgen", "offer", "created"}, "offer"))
    , mPayment(m.NewMeter({"loadgen", "payment", "any"}, "payment"))
    , mNativePayment(m.NewMeter({"loadgen", "payment", "native"}, "payment"))
    , mCreditPayment(m.NewMeter({"loadgen", "payment", "credit"}, "payment"))

    , mOneOfferPathPayment(
          m.NewMeter({"loadgen", "payment", "one-offer-path"}, "payment"))
    , mTwoOfferPathPayment(
          m.NewMeter({"loadgen", "payment", "two-offer-path"}, "payment"))
    , mManyOfferPathPayment(
          m.NewMeter({"loadgen", "payment", "many-offer-path"}, "payment"))

    , mTxnAttempted(m.NewMeter({"loadgen", "txn", "attempted"}, "txn"))
    , mTxnRejected(m.NewMeter({"loadgen", "txn", "rejected"}, "txn"))
    , mGateways(m.NewCounter({"loadgen", "account", "gateways"}))
    , mMarketMakers(m.NewCounter({"loadgen", "account", "marketmakers"}))
{
}

void
LoadGenerator::TxMetrics::report()
{
    CLOG(DEBUG, "LoadGen") << "Counts: " << mTxnAttempted.count() << " tx, "
                           << mTxnRejected.count() << " rj, "
                           << mAccountCreated.count() << " ac ("
                           << mGateways.count() << " gw, "
                           << mMarketMakers.count() << " mm), "
                           << mTrustlineCreated.count() << " tl, "
                           << mOfferCreated.count() << " of, "
                           << mPayment.count() << " pa ("
                           << mNativePayment.count() << " na, "
                           << mCreditPayment.count() << " cr, "
                           << mOneOfferPathPayment.count() << " 1p, "
                           << mTwoOfferPathPayment.count() << " 2p, "
                           << mManyOfferPathPayment.count() << " Np)";

    CLOG(DEBUG, "LoadGen") << "Rates/sec (1m EWMA): " << std::setprecision(3)
                           << mTxnAttempted.one_minute_rate() << " tx, "
                           << mTxnRejected.one_minute_rate() << " rj, "
                           << mAccountCreated.one_minute_rate() << " ac, "
                           << mTrustlineCreated.one_minute_rate() << " tl, "
                           << mOfferCreated.one_minute_rate() << " of, "
                           << mPayment.one_minute_rate() << " pa ("
                           << mNativePayment.one_minute_rate() << " na, "
                           << mCreditPayment.one_minute_rate() << " cr, "
                           << mOneOfferPathPayment.one_minute_rate() << " 1p, "
                           << mTwoOfferPathPayment.one_minute_rate() << " 2p, "
                           << mManyOfferPathPayment.one_minute_rate() << " Np)";
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

            static const char* TX_STATUS_STRING[Herder::TX_STATUS_COUNT] = {
                "PENDING", "DUPLICATE", "ERROR"};

            CLOG(INFO, "LoadGen")
                << "tx rejected '" << TX_STATUS_STRING[status]
                << "': " << xdr::xdr_to_string(f->getEnvelope()) << " ===> "
                << xdr::xdr_to_string(f->getResult());
            txm.mTxnRejected.Mark();
            return false;
        }
    }
    recordExecution(app.getConfig().DESIRED_BASE_FEE);
    return true;
}

void
LoadGenerator::TxInfo::toTransactionFrames(
    std::vector<TransactionFramePtr>& txs, TxMetrics& txm)
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
            createOp.body.createAccountOp().destination =
                mTo->mKey.getPublicKey();
            e.tx.operations.push_back(createOp);

            // Add a CHANGE_TRUST op for each of the account's trustlines,
            // and a PAYMENT from the trustline's issuer to the account, to fund
            // it.
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
                paymentOp.sourceAccount.activate() =
                    tl.mIssuer->mKey.getPublicKey();
                paymentOp.body.paymentOp().amount = LOADGEN_ACCOUNT_BALANCE;
                paymentOp.body.paymentOp().currency = ci;
                paymentOp.body.paymentOp().destination =
                    mTo->mKey.getPublicKey();

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
                Currency buyCi = txtest::makeCurrency(
                    mTo->mBuyCredit->mKey, mTo->mBuyCredit->mIssuedCurrency);

                Currency sellCi = txtest::makeCurrency(
                    mTo->mSellCredit->mKey, mTo->mSellCredit->mIssuedCurrency);

                Price price;
                price.d = 10000;
                uint32_t diff = rand_uniform(1, 200);
                price.n = rand_flip() ? (price.d + diff) : (price.d - diff);

                offerOp.body.type(CREATE_PASSIVE_OFFER);
                offerOp.sourceAccount.activate() = mTo->mKey.getPublicKey();
                offerOp.body.createPassiveOfferOp().amount =
                    LOADGEN_ACCOUNT_BALANCE;
                offerOp.body.createPassiveOfferOp().takerGets = sellCi;
                offerOp.body.createPassiveOfferOp().takerPays = buyCi;
                offerOp.body.createPassiveOfferOp().price = price;
                e.tx.operations.push_back(offerOp);
                signingAccounts.insert(mTo);
            }

            e.tx.fee = 10 * static_cast<uint32>(e.tx.operations.size());
            TransactionFramePtr res =
                TransactionFrame::makeTransactionFromWire(e);
            for (auto a : signingAccounts)
            {
                res->addSignature(a->mKey);
            }
            txs.push_back(res);
        }
        break;

    case TxInfo::TX_TRANSFER_NATIVE:
        txm.mPayment.Mark();
        txm.mNativePayment.Mark();
        txs.push_back(txtest::createPaymentTx(mFrom->mKey, mTo->mKey,
                                              mFrom->mSeq + 1, mAmount));
        break;

    case TxInfo::TX_TRANSFER_CREDIT:
    {
        txm.mPayment.Mark();
        std::vector<Currency> currencyPath;
        for (auto acc : mPath)
        {
            assert(!acc->mIssuedCurrency.empty());
            currencyPath.emplace_back(
                txtest::makeCurrency(acc->mKey, acc->mIssuedCurrency));
        }
        assert(!currencyPath.empty());
        if (currencyPath.size() == 1)
        {
            txm.mCreditPayment.Mark();
            txs.emplace_back(txtest::createCreditPaymentTx(
                mFrom->mKey, mTo->mKey, currencyPath.front(), mFrom->mSeq + 1,
                mAmount));
        }
        else
        {
            switch (currencyPath.size())
            {
            case 2:
                txm.mOneOfferPathPayment.Mark();
                break;
            case 3:
                txm.mTwoOfferPathPayment.Mark();
                break;
            default:
                txm.mManyOfferPathPayment.Mark();
                break;
            }

            auto sendCurrency = currencyPath.front();
            auto recvCurrency = currencyPath.back();
            currencyPath.erase(currencyPath.begin());
            currencyPath.pop_back();
            auto sendMax = mAmount * 10;
            txs.emplace_back(txtest::createPathPaymentTx(
                mFrom->mKey, mTo->mKey, sendCurrency, sendMax, recvCurrency,
                mAmount, mFrom->mSeq + 1, &currencyPath));
        }
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
        if (!mPath.empty())
        {
            for (auto& tl : mFrom->mTrustLines)
            {
                if (tl.mIssuer == mPath.front())
                {
                    tl.mBalance -= mAmount;
                }
            }
            for (auto& tl : mTo->mTrustLines)
            {
                if (tl.mIssuer == mPath.back())
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
