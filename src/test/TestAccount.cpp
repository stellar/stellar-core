// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "TestAccount.h"

#include "ledger/LedgerEntryReference.h"
#include "ledger/LedgerState.h"
#include "lib/catch.hpp"
#include "main/Application.h"
#include "test/TestExceptions.h"
#include "test/TxTests.h"
#include "transactions/TransactionUtils.h"

namespace stellar
{

using namespace txtest;

SequenceNumber
TestAccount::loadSequenceNumber()
{
    mSn = 0;
    return getLastSequenceNumber();
}

void
TestAccount::updateSequenceNumber()
{
    if (mSn == 0)
    {
        LedgerState ls(mApp.getLedgerStateRoot());
        auto account = stellar::loadAccount(ls, getPublicKey());
        if (account)
        {
            mSn = account.getSeqNum();
        }
    }
}

int64_t
TestAccount::getBalance() const
{
    LedgerState ls(mApp.getLedgerStateRoot());
    auto account = stellar::loadAccount(ls, getPublicKey());
    REQUIRE(account);
    return account.getBalance();
}

bool
TestAccount::exists() const
{
    LedgerState ls(mApp.getLedgerStateRoot());
    return stellar::loadAccount(ls, getPublicKey());
}

TransactionFramePtr
TestAccount::tx(std::vector<Operation> const& ops, SequenceNumber sn)
{
    if (sn == 0)
    {
        sn = nextSequenceNumber();
    }

    return transactionFromOperations(mApp, getSecretKey(), sn, ops);
}

Operation
TestAccount::op(Operation operation)
{
    operation.sourceAccount.activate() = getPublicKey();
    return operation;
}

TestAccount
TestAccount::createRoot(Application& app)
{
    auto secretKey = getRoot(app.getNetworkID());
    return TestAccount{app, secretKey};
}

std::shared_ptr<AccountEntry>
static loadAccountEntry(Application& app, AccountID const& key)
{
    LedgerState ls(app.getLedgerStateRoot());
    auto ler = stellar::loadAccount(ls, key);
    if (ler)
    {
        return std::make_shared<AccountEntry>(ler.account());
    }
    return nullptr;
}

std::shared_ptr<AccountEntry>
static loadAccountEntry(Application& app, SecretKey const& key)
{
    return loadAccountEntry(app, key.getPublicKey());
}

TestAccount
TestAccount::create(SecretKey const& secretKey, uint64_t initialBalance)
{
    auto toCreate = loadAccountEntry(mApp, secretKey);
    auto self = loadAccountEntry(mApp, getSecretKey());
    REQUIRE(self);

    try
    {
        applyTx(tx({createAccount(secretKey.getPublicKey(), initialBalance)}),
                mApp);
    }
    catch (...)
    {
        auto toCreateAfter = loadAccountEntry(mApp, secretKey);
        // check that the target account didn't change
        REQUIRE(!!toCreate == !!toCreateAfter);
        if (toCreate && toCreateAfter)
        {
            REQUIRE(*toCreate == *toCreateAfter);
        }
        throw;
    }

    REQUIRE(loadAccountEntry(mApp, secretKey));
    return TestAccount{mApp, secretKey};
}

TestAccount
TestAccount::create(std::string const& name, uint64_t initialBalance)
{
    return create(getAccount(name.c_str()), initialBalance);
}

void
TestAccount::merge(PublicKey const& into)
{
    applyTx(tx({accountMerge(into)}), mApp);

    LedgerState ls(mApp.getLedgerStateRoot());
    REQUIRE(stellar::loadAccount(ls, into));
    REQUIRE(!stellar::loadAccount(ls, getPublicKey()));
}

void
TestAccount::inflation()
{
    applyTx(tx({txtest::inflation()}), mApp);
}

Asset
TestAccount::asset(std::string const& name)
{
    return txtest::makeAsset(*this, name);
}

void
TestAccount::changeTrust(Asset const& asset, int64_t limit)
{
    applyTx(tx({txtest::changeTrust(asset, limit)}), mApp);
}

void
TestAccount::allowTrust(Asset const& asset, PublicKey const& trustor)
{
    applyTx(tx({txtest::allowTrust(trustor, asset, true)}), mApp);
}

void
TestAccount::denyTrust(Asset const& asset, PublicKey const& trustor)
{
    applyTx(tx({txtest::allowTrust(trustor, asset, false)}), mApp);
}

void
TestAccount::setOptions(AccountID* inflationDest, uint32_t* setFlags,
                        uint32_t* clearFlags, ThresholdSetter* thrs,
                        Signer* signer, std::string* homeDomain)
{
    applyTx(tx({txtest::setOptions(inflationDest, setFlags, clearFlags, thrs,
                                   signer, homeDomain)}),
            mApp);
}

void
TestAccount::manageData(std::string const& name, DataValue* value)
{
    applyTx(tx({txtest::manageData(name, value)}), mApp);

    LedgerState ls(mApp.getLedgerStateRoot());
    auto data = loadData(ls, getPublicKey(), name);
    if (value)
    {
        REQUIRE(data != nullptr);
        REQUIRE(data->entry()->data.data().dataValue == *value);
    }
    else
    {
        REQUIRE(data == nullptr);
    }
}

void
TestAccount::bumpSequence(SequenceNumber to)
{
    applyTx(tx({txtest::bumpSequence(to)}), mApp, false);
}

uint64_t
TestAccount::manageOffer(uint64_t offerID, Asset const& selling,
                         Asset const& buying, Price const& price,
                         int64_t amount, ManageOfferEffect expectedEffect)
{
    return applyManageOffer(mApp, offerID, getSecretKey(), selling, buying,
                            price, amount, nextSequenceNumber(),
                            expectedEffect);
}

uint64_t
TestAccount::createPassiveOffer(Asset const& selling, Asset const& buying,
                                Price const& price, int64_t amount,
                                ManageOfferEffect expectedEffect)
{
    return applyCreatePassiveOffer(mApp, getSecretKey(), selling, buying, price,
                                   amount, nextSequenceNumber(),
                                   expectedEffect);
}

void
TestAccount::pay(PublicKey const& destination, int64_t amount)
{
    auto toAccount = loadAccountEntry(mApp, destination);
    auto fromAccount = loadAccountEntry(mApp, getPublicKey());
    auto transaction = tx({payment(destination, amount)});

    try
    {
        applyTx(transaction, mApp);
    }
    catch (...)
    {
        auto toAccountAfter = loadAccountEntry(mApp, destination);
        // check that the target account didn't change
        REQUIRE(!!toAccount == !!toAccountAfter);
        if (toAccount && toAccountAfter &&
            !(fromAccount->accountID == toAccount->accountID))
        {
            REQUIRE(*toAccount == *toAccountAfter);
        }
        throw;
    }

    auto toAccountAfter = loadAccountEntry(mApp, destination);
    REQUIRE(toAccount);
    REQUIRE(toAccountAfter);
}

void
TestAccount::pay(PublicKey const& destination, Asset const& asset,
                 int64_t amount)
{
    applyTx(tx({payment(destination, asset, amount)}), mApp);
}

PathPaymentResult
TestAccount::pay(PublicKey const& destination, Asset const& sendCur,
                 int64_t sendMax, Asset const& destCur, int64_t destAmount,
                 std::vector<Asset> const& path, Asset* noIssuer)
{
    auto transaction = tx({pathPayment(destination, sendCur, sendMax, destCur,
                                       destAmount, path)});
    try
    {
        applyTx(transaction, mApp);
    }
    catch (ex_PATH_PAYMENT_NO_ISSUER&)
    {
        REQUIRE(noIssuer);
        REQUIRE(*noIssuer == transaction->getResult()
                                 .result.results()[0]
                                 .tr()
                                 .pathPaymentResult()
                                 .noIssuer());
        throw;
    }

    REQUIRE(!noIssuer);

    return getFirstResult(*transaction).tr().pathPaymentResult();
}
};
