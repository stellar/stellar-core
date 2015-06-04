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

namespace stellar
{

using namespace std;

const uint32_t LoadGenerator::STEP_MSECS = 100;

LoadGenerator::LoadGenerator()
    : mMinBalance(0)
{
    auto root =
        make_shared<AccountInfo>(0, txtest::getRoot(), 1000000000, 0, *this);
    mAccounts.push_back(root);
}

LoadGenerator::~LoadGenerator()
{
}

// Schedule a callback to generateLoad() STEP_MSECS miliseconds from now.
void
LoadGenerator::scheduleLoadGeneration(Application& app,
                                      uint32_t nAccounts,
                                      uint32_t nTxs,
                                      uint32_t txRate)
{
    if (!mLoadTimer)
    {
        mLoadTimer = make_unique<VirtualTimer>(app.getClock());
    }
    mLoadTimer->expires_from_now(std::chrono::milliseconds(STEP_MSECS));
    mLoadTimer->async_wait(
        [this, &app, nAccounts, nTxs, txRate]
        (asio::error_code const& error)
        {
            if (!error)
            {
                this->generateLoad(app, nAccounts, nTxs, txRate);
            }
        });
}

// Generate one "step" worth of load (assuming 1 step per STEP_MSECS) at a
// given target number of accounts and txs, and a given target tx/s rate.
// If work remains after the current step, call scheduleLoadGeneration()
// with the remainder.
void
LoadGenerator::generateLoad(Application& app,
                            uint32_t nAccounts,
                            uint32_t nTxs,
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
    }
    else
    {
        // Emit a log message once per second.
        bool logBoundary = ((nTxs / txRate) != ((nTxs - txPerStep) / txRate));
        if (logBoundary)
        {
            CLOG(INFO, "LoadGen") << "Target rate: "
                                  << txRate << "txs/s, pending: "
                                  << nAccounts << " accounts, "
                                  << nTxs << " payments";
        }

        auto& clock = app.getClock();
        auto start = clock.now();

        size_t creations = 0;
        size_t payments = 0;

        vector<TxInfo> txs;
        for (uint32_t i = 0; i < txPerStep; ++i)
        {
            bool doCreateAccount = false;
            if (mAccounts.size() < 2)
            {
                doCreateAccount = true;
            }
            else if (nAccounts > 0)
            {
                doCreateAccount = rand_flip();
            }
            if (doCreateAccount)
            {
                auto acc = createAccount(mAccounts.size());
                mAccounts.push_back(acc);
                txs.push_back(acc->creationTransaction());
                ++creations;
                if (nAccounts > 0)
                {
                    nAccounts--;
                }
            }
            else
            {
                txs.push_back(createRandomTransaction(0.5));
                ++payments;
                if (nTxs > 0)
                {
                    nTxs--;
                }
            }
        }
        auto created = clock.now();
        auto rejected = 0;

        for (auto& tx : txs)
        {
            if (!tx.execute(app))
            {
                rejected++;
                // Hopefully the rejection was just a bad seq number.
                loadAccount(app, *tx.mFrom);
                loadAccount(app, *tx.mTo);
            }
        }

        if (logBoundary)
        {
            using namespace std::chrono;
            auto executed = clock.now();
            auto step1ms = duration_cast<milliseconds>(created-start).count();
            auto step2ms = duration_cast<milliseconds>(executed-created).count();
            auto totalms = duration_cast<milliseconds>(executed-start).count();
            CLOG(INFO, "LoadGen") << "Step timing: "
                                  << txs.size() << "txs, "
                                  << creations << " creations, "
                                  << payments << " payments, "
                                  << rejected << " rejected, "
                                  << totalms << "ms total = "
                                  << step1ms << "ms build, "
                                  << step2ms << "ms recv, "
                                  << (STEP_MSECS - totalms) << "ms spare";
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
LoadGenerator::createAccount(size_t i)
{
    auto accountName = "Account-" + to_string(i);
    return make_shared<AccountInfo>(i, txtest::getAccount(accountName.c_str()),
                                    0, 0, *this);
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
LoadGenerator::createTransferTransaction(size_t iFrom, size_t iTo, uint64_t amount)
{
    return TxInfo{mAccounts[iFrom], mAccounts[iTo], false, amount};
}

LoadGenerator::TxInfo
LoadGenerator::createRandomTransaction(float alpha)
{
    size_t iFrom, iTo;
    do
    {
        // iFrom = rand_pareto(alpha, mAccounts.size());
        // iTo = rand_pareto(alpha, mAccounts.size());
        iFrom = static_cast<int>(rand_fraction() * mAccounts.size());
        iTo = static_cast<int>(rand_fraction() * mAccounts.size());
    } while (iFrom == iTo);

    uint64_t amount = static_cast<uint64_t>(
        rand_fraction() *
        min(static_cast<uint64_t>(1000),
            (mAccounts[iFrom]->mBalance - mMinBalance) / 3));
    return createTransferTransaction(iFrom, iTo, amount);
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

LoadGenerator::AccountInfo::AccountInfo(size_t id, SecretKey key, uint64_t balance,
                                            SequenceNumber seq,
                                            LoadGenerator& loadGen)
    : mId(id)
    , mKey(key)
    , mBalance(balance)
    , mSeq(seq)
    , mLoadGen(loadGen)
{
}

LoadGenerator::TxInfo
LoadGenerator::AccountInfo::creationTransaction()
{
    return TxInfo{mLoadGen.mAccounts[0], shared_from_this(), true,
                  100 * mLoadGen.mMinBalance +
                      mLoadGen.mAccounts.size() - 1};
}


//////////////////////////////////////////////////////
// TxInfo
//////////////////////////////////////////////////////

bool
LoadGenerator::TxInfo::execute(Application& app)
{
    if (app.getHerder().recvTransaction(createPaymentTx()) ==
        Herder::TX_STATUS_PENDING)
    {
        recordExecution(app.getConfig().DESIRED_BASE_FEE);
        return true;
    }
    return false;
}

TransactionFramePtr
LoadGenerator::TxInfo::createPaymentTx()
{
    TransactionFramePtr res;
    if (mCreate)
    {
        res = txtest::createCreateAccountTx(mFrom->mKey, mTo->mKey,
                                            mFrom->mSeq + 1, mAmount);
    }
    else
    {
        res = txtest::createPaymentTx(mFrom->mKey, mTo->mKey, mFrom->mSeq + 1,
                                      mAmount);
    }
    return res;
}

void
LoadGenerator::TxInfo::recordExecution(uint64_t baseFee)
{
    mFrom->mSeq++;
    mFrom->mBalance -= mAmount;
    mFrom->mBalance -= baseFee;
    mTo->mBalance += mAmount;
}


}
