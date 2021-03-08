// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "test/TestAccount.h"
#include "crypto/SHA.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/TrustLineWrapper.h"
#include "main/Application.h"
#include "test/TestExceptions.h"
#include "test/TxTests.h"
#include "transactions/TransactionUtils.h"

#include <lib/catch.hpp>

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
        LedgerTxn ltx(mApp.getLedgerTxnRoot());
        auto entry = stellar::loadAccount(ltx, getPublicKey());
        if (entry)
        {
            mSn = entry.current().data.account().seqNum;
        }
    }
}
int64_t
TestAccount::getTrustlineBalance(Asset const& asset) const
{
    LedgerTxn ltx(mApp.getLedgerTxnRoot());
    auto trustLine = stellar::loadTrustLine(ltx, getPublicKey(), asset);
    REQUIRE(trustLine);
    return trustLine.getBalance();
}

int64_t
TestAccount::getBalance() const
{
    LedgerTxn ltx(mApp.getLedgerTxnRoot());
    auto entry = stellar::loadAccount(ltx, getPublicKey());
    return entry.current().data.account().balance;
}

int64_t
TestAccount::getAvailableBalance() const
{
    LedgerTxn ltx(mApp.getLedgerTxnRoot());
    auto entry = stellar::loadAccount(ltx, getPublicKey());
    auto header = ltx.loadHeader();

    return stellar::getAvailableBalance(header, entry);
}

bool
TestAccount::exists() const
{
    return doesAccountExist(mApp, getPublicKey());
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
    operation.sourceAccount.activate() = toMuxedAccount(getPublicKey());
    return operation;
}

TestAccount
TestAccount::createRoot(Application& app)
{
    auto secretKey = getRoot(app.getNetworkID());
    return TestAccount{app, secretKey};
}

TestAccount
TestAccount::create(SecretKey const& secretKey, uint64_t initialBalance)
{
    auto publicKey = secretKey.getPublicKey();

    std::unique_ptr<LedgerEntry> destBefore;
    {
        LedgerTxn ltx(mApp.getLedgerTxnRoot());
        auto entry = stellar::loadAccount(ltx, publicKey);
        if (entry)
        {
            destBefore = std::make_unique<LedgerEntry>(entry.current());
        }
    }

    try
    {
        applyTx(tx({createAccount(publicKey, initialBalance)}), mApp);
    }
    catch (...)
    {
        LedgerTxn ltx(mApp.getLedgerTxnRoot());
        auto destAfter = stellar::loadAccount(ltx, publicKey);
        // check that the target account didn't change
        REQUIRE(!!destBefore == !!destAfter);
        if (destBefore && destAfter)
        {
            REQUIRE(*destBefore == destAfter.current());
        }
        throw;
    }

    {
        LedgerTxn ltx(mApp.getLedgerTxnRoot());
        REQUIRE(stellar::loadAccount(ltx, publicKey));
    }
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

    LedgerTxn ltx(mApp.getLedgerTxnRoot());
    REQUIRE(stellar::loadAccount(ltx, into));
    REQUIRE(!stellar::loadAccount(ltx, getPublicKey()));
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
TestAccount::allowTrust(Asset const& asset, PublicKey const& trustor,
                        uint32_t flag)
{
    applyTx(tx({txtest::allowTrust(trustor, asset, flag)}), mApp);
}

void
TestAccount::allowTrust(Asset const& asset, PublicKey const& trustor)
{
    applyTx(tx({txtest::allowTrust(trustor, asset, AUTHORIZED_FLAG)}), mApp);
}

void
TestAccount::denyTrust(Asset const& asset, PublicKey const& trustor)
{
    applyTx(tx({txtest::allowTrust(trustor, asset, 0)}), mApp);
}

void
TestAccount::allowMaintainLiabilities(Asset const& asset,
                                      PublicKey const& trustor)
{
    applyTx(tx({txtest::allowTrust(trustor, asset,
                                   AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG)}),
            mApp);
}

TrustLineEntry
TestAccount::loadTrustLine(Asset const& asset) const
{
    LedgerTxn ltx(mApp.getLedgerTxnRoot());
    LedgerKey key(TRUSTLINE);
    key.trustLine().accountID = getPublicKey();
    key.trustLine().asset = asset;
    return ltx.load(key).current().data.trustLine();
}

bool
TestAccount::hasTrustLine(Asset const& asset) const
{
    LedgerTxn ltx(mApp.getLedgerTxnRoot());
    LedgerKey key(TRUSTLINE);
    key.trustLine().accountID = getPublicKey();
    key.trustLine().asset = asset;
    return (bool)ltx.load(key);
}

void
TestAccount::setOptions(SetOptionsArguments const& arguments)
{
    applyTx(tx({txtest::setOptions(arguments)}), mApp);
}

void
TestAccount::manageData(std::string const& name, DataValue* value)
{
    applyTx(tx({txtest::manageData(name, value)}), mApp);

    LedgerTxn ls(mApp.getLedgerTxnRoot());
    auto data = stellar::loadData(ls, getPublicKey(), name);
    if (value)
    {
        REQUIRE(data);
        REQUIRE(data.current().data.data().dataValue == *value);
    }
    else
    {
        REQUIRE(!data);
    }
}

void
TestAccount::bumpSequence(SequenceNumber to)
{
    applyTx(tx({txtest::bumpSequence(to)}), mApp, false);
}

ClaimableBalanceID
TestAccount::createClaimableBalance(Asset const& asset, int64_t amount,
                                    xdr::xvector<Claimant, 10> const& claimants)
{
    auto transaction =
        tx({txtest::createClaimableBalance(asset, amount, claimants)});
    applyTx(transaction, mApp);

    auto returnedBalanceID = transaction->getResult()
                                 .result.results()[0]
                                 .tr()
                                 .createClaimableBalanceResult()
                                 .balanceID();

    // validate balanceID returned is what we expect
    REQUIRE(returnedBalanceID == getBalanceID(0));

    LedgerTxn ltx(mApp.getLedgerTxnRoot());
    auto entry = stellar::loadClaimableBalance(ltx, returnedBalanceID);
    REQUIRE(entry);

    auto const& claimableBalance = entry.current().data.claimableBalance();
    REQUIRE(claimableBalance.asset == asset);
    REQUIRE(claimableBalance.amount == amount);
    REQUIRE(claimableBalance.balanceID == returnedBalanceID);
    REQUIRE((entry.current().ext.v() == 1 &&
             entry.current().ext.v1().sponsoringID));
    REQUIRE(*entry.current().ext.v1().sponsoringID == getPublicKey());

    return returnedBalanceID;
}

void
TestAccount::claimClaimableBalance(ClaimableBalanceID const& balanceID)
{
    applyTx(tx({txtest::claimClaimableBalance(balanceID)}), mApp);

    LedgerTxn ltx(mApp.getLedgerTxnRoot());
    REQUIRE(!stellar::loadClaimableBalance(ltx, balanceID));
}

ClaimableBalanceID
TestAccount::getBalanceID(uint32_t opIndex, SequenceNumber sn)
{
    if (sn == 0)
    {
        sn = getLastSequenceNumber();
    }

    OperationID operationID;
    operationID.type(ENVELOPE_TYPE_OP_ID);
    operationID.id().sourceAccount = toMuxedAccount(getPublicKey());
    operationID.id().seqNum = sn;
    operationID.id().opNum = opIndex;

    ClaimableBalanceID balanceID;
    balanceID.v0() = sha256(xdr::xdr_to_opaque(operationID));

    return balanceID;
}

int64_t
TestAccount::manageOffer(int64_t offerID, Asset const& selling,
                         Asset const& buying, Price const& price,
                         int64_t amount, ManageOfferEffect expectedEffect)
{
    return applyManageOffer(mApp, offerID, getSecretKey(), selling, buying,
                            price, amount, nextSequenceNumber(),
                            expectedEffect);
}

int64_t
TestAccount::manageBuyOffer(int64_t offerID, Asset const& selling,
                            Asset const& buying, Price const& price,
                            int64_t amount, ManageOfferEffect expectedEffect)
{
    return applyManageBuyOffer(mApp, offerID, getSecretKey(), selling, buying,
                               price, amount, nextSequenceNumber(),
                               expectedEffect);
}

int64_t
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
    std::unique_ptr<LedgerEntry> toAccount;
    {
        LedgerTxn ltx(mApp.getLedgerTxnRoot());
        auto toAccountEntry = stellar::loadAccount(ltx, destination);
        toAccount =
            toAccountEntry
                ? std::make_unique<LedgerEntry>(toAccountEntry.current())
                : nullptr;
        if (destination == getPublicKey())
        {
            REQUIRE(toAccountEntry);
        }
        else
        {
            REQUIRE(stellar::loadAccount(ltx, getPublicKey()));
        }
    }

    auto transaction = tx({payment(destination, amount)});

    try
    {
        applyTx(transaction, mApp);
    }
    catch (...)
    {
        LedgerTxn ltx(mApp.getLedgerTxnRoot());
        auto toAccountAfter = stellar::loadAccount(ltx, destination);
        // check that the target account didn't change
        REQUIRE(!!toAccount == !!toAccountAfter);
        if (toAccount && toAccountAfter &&
            !(toAccount->data.account().accountID ==
              toAccountAfter.current().data.account().accountID))
        {
            REQUIRE(*toAccount == toAccountAfter.current());
        }
        throw;
    }

    LedgerTxn ltx(mApp.getLedgerTxnRoot());
    auto toAccountAfter = stellar::loadAccount(ltx, destination);
    REQUIRE(toAccount);
    REQUIRE(toAccountAfter);
}

void
TestAccount::pay(PublicKey const& destination, Asset const& asset,
                 int64_t amount)
{
    applyTx(tx({payment(destination, asset, amount)}), mApp);
}

PathPaymentStrictReceiveResult
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
    catch (ex_PATH_PAYMENT_STRICT_RECEIVE_NO_ISSUER&)
    {
        REQUIRE(noIssuer);
        REQUIRE(*noIssuer == transaction->getResult()
                                 .result.results()[0]
                                 .tr()
                                 .pathPaymentStrictReceiveResult()
                                 .noIssuer());
        throw;
    }

    REQUIRE(!noIssuer);

    return getFirstResult(*transaction).tr().pathPaymentStrictReceiveResult();
}

PathPaymentStrictSendResult
TestAccount::pathPaymentStrictSend(PublicKey const& destination,
                                   Asset const& sendCur, int64_t sendAmount,
                                   Asset const& destCur, int64_t destMin,
                                   std::vector<Asset> const& path,
                                   Asset* noIssuer)
{
    auto transaction = tx({txtest::pathPaymentStrictSend(
        destination, sendCur, sendAmount, destCur, destMin, path)});

    try
    {
        applyTx(transaction, mApp);
    }
    catch (ex_PATH_PAYMENT_STRICT_SEND_NO_ISSUER&)
    {
        REQUIRE(noIssuer);
        REQUIRE(*noIssuer == transaction->getResult()
                                 .result.results()[0]
                                 .tr()
                                 .pathPaymentStrictSendResult()
                                 .noIssuer());
        throw;
    }

    REQUIRE(!noIssuer);

    return getFirstResult(*transaction).tr().pathPaymentStrictSendResult();
}

void
TestAccount::clawback(PublicKey const& from, Asset const& asset, int64_t amount)
{
    applyTx(tx({txtest::clawback(from, asset, amount)}), mApp);
}

void
TestAccount::clawbackClaimableBalance(ClaimableBalanceID const& balanceID)
{
    applyTx(tx({txtest::clawbackClaimableBalance(balanceID)}), mApp);

    LedgerTxn ltx(mApp.getLedgerTxnRoot());
    REQUIRE(!stellar::loadClaimableBalance(ltx, balanceID));
}
};
