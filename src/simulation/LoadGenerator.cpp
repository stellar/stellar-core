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

namespace stellar
{

using namespace std;

LoadGenerator::LoadGenerator()
{
    auto root =
        make_shared<AccountInfo>(0, txtest::getRoot(), 1000000000, 0, *this);
    mAccounts.push_back(root);
}

LoadGenerator::~LoadGenerator()
{
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

void
LoadGenerator::TxInfo::execute(Application& app)
{
    if (app.getHerder().recvTransaction(createPaymentTx()) ==
        Herder::TX_STATUS_PENDING)
    {
        recordExecution(app.getConfig().DESIRED_BASE_FEE);
    }
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
