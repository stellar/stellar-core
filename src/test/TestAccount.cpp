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
        LedgerSnapshot lsg(mApp);
        auto const entry = lsg.load(accountKey(getPublicKey()));
        if (entry)
        {
            mSn = entry.current().data.account().seqNum;
        }
    }
}

uint32_t
TestAccount::getTrustlineFlags(Asset const& asset) const
{
    LedgerSnapshot lsg(mApp);
    auto const trust = lsg.load(trustlineKey(getPublicKey(), asset));
    REQUIRE(trust);
    return trust.current().data.trustLine().flags;
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
TestAccount::getTrustlineBalance(PoolID const& poolID) const
{
    LedgerSnapshot lsg(mApp);
    TrustLineAsset asset(ASSET_TYPE_POOL_SHARE);
    asset.liquidityPoolID() = poolID;
    auto const trustLine = lsg.load(trustlineKey(getPublicKey(), asset));
    REQUIRE(trustLine);
    return trustLine.current().data.trustLine().balance;
}

int64_t
TestAccount::getBalance() const
{
    LedgerSnapshot lsg(mApp);
    auto const entry = lsg.getAccount(getPublicKey());
    return entry.current().data.account().balance;
}

int64_t
TestAccount::getAvailableBalance() const
{
    LedgerSnapshot lsg(mApp);
    auto const entry = lsg.getAccount(getPublicKey());
    return stellar::getAvailableBalance(lsg.getLedgerHeader().current(),
                                        entry.current());
}

uint32_t
TestAccount::getNumSubEntries() const
{
    LedgerSnapshot lsg(mApp);
    auto const entry = lsg.getAccount(getPublicKey());
    return entry.current().data.account().numSubEntries;
}

bool
TestAccount::exists() const
{
    return doesAccountExist(mApp, getPublicKey());
}

TransactionTestFramePtr
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
        LedgerSnapshot lsg(mApp);
        auto const entry = lsg.getAccount(publicKey);
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
        LedgerSnapshot lsg(mApp);
        auto const destAfter = lsg.getAccount(publicKey);
        // check that the target account didn't change
        REQUIRE(!!destBefore == !!destAfter);
        if (destBefore && destAfter)
        {
            REQUIRE(*destBefore == destAfter.current());
        }
        throw;
    }

    {
        LedgerSnapshot lsg(mApp);
        REQUIRE(lsg.getAccount(publicKey));
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

    LedgerSnapshot lsg(mApp);
    REQUIRE(lsg.getAccount(into));
    REQUIRE(!lsg.getAccount(getPublicKey()));
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
TestAccount::changeTrust(ChangeTrustAsset const& asset, int64_t limit)
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
TestAccount::allowTrust(Asset const& asset, PublicKey const& trustor,
                        TrustFlagOp op)
{
    if (op == TrustFlagOp::ALLOW_TRUST)
    {
        applyTx(tx({txtest::allowTrust(trustor, asset, AUTHORIZED_FLAG)}),
                mApp);
    }
    else if (op == TrustFlagOp::SET_TRUST_LINE_FLAGS)
    {
        auto flags = txtest::setTrustLineFlags(AUTHORIZED_FLAG) |
                     txtest::clearTrustLineFlags(
                         AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG);
        applyTx(tx({txtest::setTrustLineFlags(trustor, asset, flags)}), mApp);
    }
    else
    {
        REQUIRE(false);
    }
}

void
TestAccount::denyTrust(Asset const& asset, PublicKey const& trustor,
                       TrustFlagOp op)
{
    if (op == TrustFlagOp::ALLOW_TRUST)
    {
        applyTx(tx({txtest::allowTrust(trustor, asset, 0)}), mApp);
    }
    else if (op == TrustFlagOp::SET_TRUST_LINE_FLAGS)
    {
        auto flags = txtest::clearTrustLineFlags(TRUSTLINE_AUTH_FLAGS);
        applyTx(tx({txtest::setTrustLineFlags(trustor, asset, flags)}), mApp);
    }
    else
    {
        REQUIRE(false);
    }
}

void
TestAccount::allowMaintainLiabilities(Asset const& asset,
                                      PublicKey const& trustor, TrustFlagOp op)
{
    if (op == TrustFlagOp::ALLOW_TRUST)
    {
        applyTx(tx({txtest::allowTrust(
                    trustor, asset, AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG)}),
                mApp);
    }
    else if (op == TrustFlagOp::SET_TRUST_LINE_FLAGS)
    {
        auto flags =
            txtest::setTrustLineFlags(AUTHORIZED_TO_MAINTAIN_LIABILITIES_FLAG) |
            txtest::clearTrustLineFlags(AUTHORIZED_FLAG);
        applyTx(tx({txtest::setTrustLineFlags(trustor, asset, flags)}), mApp);
    }
    else
    {
        REQUIRE(false);
    }
}

void
TestAccount::setTrustLineFlags(
    Asset const& asset, PublicKey const& trustor,
    txtest::SetTrustLineFlagsArguments const& arguments)
{
    applyTx(tx({txtest::setTrustLineFlags(trustor, asset, arguments)}), mApp);
}

TrustLineEntry
TestAccount::loadTrustLine(Asset const& asset) const
{
    return loadTrustLine(assetToTrustLineAsset(asset));
}

TrustLineEntry
TestAccount::loadTrustLine(TrustLineAsset const& asset) const
{
    LedgerSnapshot lsg(mApp);
    LedgerKey key(TRUSTLINE);
    key.trustLine().accountID = getPublicKey();
    key.trustLine().asset = asset;
    return lsg.load(key).current().data.trustLine();
}

bool
TestAccount::hasTrustLine(Asset const& asset) const
{
    return hasTrustLine(assetToTrustLineAsset(asset));
}

bool
TestAccount::hasTrustLine(TrustLineAsset const& asset) const
{
    LedgerSnapshot lsg(mApp);
    LedgerKey key(TRUSTLINE);
    key.trustLine().accountID = getPublicKey();
    key.trustLine().asset = asset;
    return static_cast<bool>(lsg.load(key));
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

    LedgerSnapshot lsg(mApp);
    if (protocolVersionStartsFrom(lsg.getLedgerHeader().current().ledgerVersion,
                                  ProtocolVersion::V_19))
    {
        auto const account = lsg.getAccount(getPublicKey());
        REQUIRE(account);

        auto const& v3 =
            getAccountEntryExtensionV3(account.current().data.account());
        REQUIRE(v3.seqLedger == lsg.getLedgerHeader().current().ledgerSeq);
        REQUIRE(v3.seqTime ==
                lsg.getLedgerHeader().current().scpValue.closeTime);
    }
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

    HashIDPreimage hashPreimage;
    hashPreimage.type(ENVELOPE_TYPE_OP_ID);
    hashPreimage.operationID().sourceAccount = getPublicKey();
    hashPreimage.operationID().seqNum = sn;
    hashPreimage.operationID().opNum = opIndex;

    ClaimableBalanceID balanceID;
    balanceID.v0() = sha256(xdr::xdr_to_opaque(hashPreimage));

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
        LedgerSnapshot lsg(mApp);
        auto const toAccountEntry = lsg.getAccount(destination);
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
            REQUIRE(lsg.getAccount(getPublicKey()));
        }
    }

    auto transaction = tx({payment(destination, amount)});

    try
    {
        applyTx(transaction, mApp);
    }
    catch (...)
    {
        LedgerSnapshot lsg(mApp);
        auto const toAccountAfter = lsg.getAccount(destination);
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

    LedgerSnapshot lsg(mApp);
    auto const toAccountAfter = lsg.getAccount(destination);
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

    return getFirstResult(transaction).tr().pathPaymentStrictReceiveResult();
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

    return getFirstResult(transaction).tr().pathPaymentStrictSendResult();
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

void
TestAccount::liquidityPoolDeposit(PoolID const& poolID, int64_t maxAmountA,
                                  int64_t maxAmountB, Price const& minPrice,
                                  Price const& maxPrice)
{
    applyTx(tx({txtest::liquidityPoolDeposit(poolID, maxAmountA, maxAmountB,
                                             minPrice, maxPrice)}),
            mApp);
}

void
TestAccount::liquidityPoolWithdraw(PoolID const& poolID, int64_t amount,
                                   int64_t minAmountA, int64_t minAmountB)
{
    applyTx(tx({txtest::liquidityPoolWithdraw(poolID, amount, minAmountA,
                                              minAmountB)}),
            mApp);
}

};
