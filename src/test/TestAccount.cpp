// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "test/TestAccount.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "main/Application.h"
#include "test/TestExceptions.h"
#include "test/TxTests.h"
#include "transactions/SignatureUtils.h"
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
TestAccount::getBalance() const
{
    LedgerTxn ltx(mApp.getLedgerTxnRoot());
    auto entry = stellar::loadAccount(ltx, getPublicKey());
    return entry.current().data.account().balance;
}

bool
TestAccount::exists() const
{
    return doesAccountExist(mApp, getPublicKey());
}

DecoratedSignature
TestAccount::getSignature(TransactionFramePtr const& tx) const
{
    return SignatureUtils::sign(*this, tx->getContentsHash());
}

void
TestAccount::sign(TransactionFramePtr const& tx) const
{
    tx->addSignature(getSignature(tx));
}

Transaction
TestAccount::rawTx(std::vector<Operation> const& ops, SequenceNumber sn,
                   int fee)
{
    if (sn == 0)
    {
        sn = nextSequenceNumber();
    }

    auto tx = Transaction{};
    tx.sourceAccount = getPublicKey();
    tx.fee = fee != 0
                 ? fee
                 : static_cast<uint32_t>(
                       (ops.size() * mApp.getLedgerManager().getLastTxFee()) &
                       UINT32_MAX);
    tx.seqNum = sn;
    std::copy(std::begin(ops), std::end(ops),
              std::back_inserter(tx.operations));

    return tx;
}

TransactionFramePtr
TestAccount::tx(Transaction const& tx) const
{
    auto result = unsignedTx(tx);
    sign(result);
    return result;
}

TransactionFramePtr
TestAccount::tx(std::vector<Operation> const& ops, SequenceNumber sn, int fee)
{
    auto res = unsignedTx(ops, sn, fee);
    sign(res);
    return res;
}

TransactionFramePtr
TestAccount::unsignedTx(Transaction const& tx) const
{
    auto env = TransactionEnvelope{};
    env.tx = tx;
    return TransactionFrame::makeTransactionFromWire(mApp.getNetworkID(), env);
}

TransactionFramePtr
TestAccount::unsignedTx(std::vector<Operation> const& ops, SequenceNumber sn,
                        int fee)
{
    auto env = TransactionEnvelope{};
    env.tx = rawTx(ops, sn, fee);
    return TransactionFrame::makeTransactionFromWire(mApp.getNetworkID(), env);
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
TestAccount::allowTrust(Asset const& asset, PublicKey const& trustor)
{
    applyTx(tx({txtest::allowTrust(trustor, asset, true)}), mApp);
}

void
TestAccount::denyTrust(Asset const& asset, PublicKey const& trustor)
{
    applyTx(tx({txtest::allowTrust(trustor, asset, false)}), mApp);
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

int64_t
TestAccount::manageOffer(int64_t offerID, Asset const& selling,
                         Asset const& buying, Price const& price,
                         int64_t amount, ManageOfferEffect expectedEffect)
{
    auto getIdPool = [&]() {
        LedgerTxn ltx(mApp.getLedgerTxnRoot());
        return ltx.loadHeader().current().idPool;
    };
    auto lastGeneratedID = getIdPool();
    auto expectedofferID = lastGeneratedID + 1;
    if (offerID != 0)
    {
        expectedofferID = offerID;
    }

    auto op = txtest::manageOffer(offerID, selling, buying, price, amount);
    auto txFrame = tx({op});

    try
    {
        applyTx(txFrame, mApp);
    }
    catch (...)
    {
        REQUIRE(getIdPool() == lastGeneratedID);
        throw;
    }

    auto& results = txFrame->getResult().result.results();

    REQUIRE(results.size() == 1);

    auto& manageSellOfferResult = results[0].tr().manageSellOfferResult();

    auto& offerResult = manageSellOfferResult.success().offer;

    switch (offerResult.effect())
    {
    case MANAGE_OFFER_CREATED:
    case MANAGE_OFFER_UPDATED:
    {
        LedgerTxn ltx(mApp.getLedgerTxnRoot());
        auto offer = stellar::loadOffer(ltx, getPublicKey(), expectedofferID);
        REQUIRE(offer);
        auto& offerEntry = offer.current().data.offer();
        REQUIRE(offerEntry == offerResult.offer());
        REQUIRE(offerEntry.price == price);
        REQUIRE(offerEntry.selling == selling);
        REQUIRE(offerEntry.buying == buying);
    }
    break;
    case MANAGE_OFFER_DELETED:
    {
        LedgerTxn ltx(mApp.getLedgerTxnRoot());
        REQUIRE(!stellar::loadOffer(ltx, getPublicKey(), expectedofferID));
    }
    break;
    default:
        abort();
    }

    auto& success = manageSellOfferResult.success().offer;
    REQUIRE(success.effect() == expectedEffect);
    return success.effect() != MANAGE_OFFER_DELETED ? success.offer().offerID
                                                    : 0;
}

int64_t
TestAccount::manageBuyOffer(int64_t offerID, Asset const& selling,
                            Asset const& buying, Price const& price,
                            int64_t amount, ManageOfferEffect expectedEffect)
{
    auto getIdPool = [&]() {
        LedgerTxn ltx(mApp.getLedgerTxnRoot());
        return ltx.loadHeader().current().idPool;
    };
    auto lastGeneratedID = getIdPool();
    auto expectedofferID = lastGeneratedID + 1;
    if (offerID != 0)
    {
        expectedofferID = offerID;
    }

    auto op = txtest::manageBuyOffer(offerID, selling, buying, price, amount);
    auto txFrame = tx({op});

    try
    {
        applyTx(txFrame, mApp);
    }
    catch (...)
    {
        REQUIRE(getIdPool() == lastGeneratedID);
        throw;
    }

    auto& results = txFrame->getResult().result.results();
    REQUIRE(results.size() == 1);
    auto& manageBuyOfferResult = results[0].tr().manageBuyOfferResult();
    auto& success = manageBuyOfferResult.success().offer;

    REQUIRE(success.effect() == expectedEffect);
    switch (success.effect())
    {
    case MANAGE_OFFER_CREATED:
    case MANAGE_OFFER_UPDATED:
    {
        LedgerTxn ltx(mApp.getLedgerTxnRoot());
        auto offer = stellar::loadOffer(ltx, getPublicKey(), expectedofferID);
        REQUIRE(offer);
        auto& offerEntry = offer.current().data.offer();
        REQUIRE(offerEntry == success.offer());
        REQUIRE(offerEntry.price == Price{price.d, price.n});
        REQUIRE(offerEntry.selling == selling);
        REQUIRE(offerEntry.buying == buying);
    }
    break;
    case MANAGE_OFFER_DELETED:
    {
        LedgerTxn ltx(mApp.getLedgerTxnRoot());
        REQUIRE(!stellar::loadOffer(ltx, getPublicKey(), expectedofferID));
    }
    break;
    default:
        abort();
    }

    return success.effect() != MANAGE_OFFER_DELETED ? success.offer().offerID
                                                    : 0;
}

int64_t
TestAccount::createPassiveOffer(Asset const& selling, Asset const& buying,
                                Price const& price, int64_t amount,
                                ManageOfferEffect expectedEffect)
{
    auto getIdPool = [&]() {
        LedgerTxn ltx(mApp.getLedgerTxnRoot());
        return ltx.loadHeader().current().idPool;
    };
    auto lastGeneratedID = getIdPool();
    auto expectedofferID = lastGeneratedID + 1;

    auto op = txtest::createPassiveOffer(selling, buying, price, amount);
    auto txFrame = tx({op});

    try
    {
        applyTx(txFrame, mApp);
    }
    catch (...)
    {
        REQUIRE(getIdPool() == lastGeneratedID);
        throw;
    }

    auto& results = txFrame->getResult().result.results();

    REQUIRE(results.size() == 1);

    auto& createPassiveSellOfferResult =
        results[0].tr().manageSellOfferResult();

    if (createPassiveSellOfferResult.code() == MANAGE_SELL_OFFER_SUCCESS)
    {
        auto& offerResult = createPassiveSellOfferResult.success().offer;

        switch (offerResult.effect())
        {
        case MANAGE_OFFER_CREATED:
        case MANAGE_OFFER_UPDATED:
        {
            LedgerTxn ltx(mApp.getLedgerTxnRoot());
            auto offer =
                stellar::loadOffer(ltx, getPublicKey(), expectedofferID);
            REQUIRE(offer);
            auto& offerEntry = offer.current().data.offer();
            REQUIRE(offerEntry == offerResult.offer());
            REQUIRE(offerEntry.price == price);
            REQUIRE(offerEntry.selling == selling);
            REQUIRE(offerEntry.buying == buying);
            REQUIRE((offerEntry.flags & PASSIVE_FLAG) != 0);
        }
        break;
        case MANAGE_OFFER_DELETED:
        {
            LedgerTxn ltx(mApp.getLedgerTxnRoot());
            REQUIRE(!stellar::loadOffer(ltx, getPublicKey(), expectedofferID));
        }
        break;
        default:
            abort();
        }
    }

    auto& success = createPassiveSellOfferResult.success().offer;

    REQUIRE(success.effect() == expectedEffect);

    return success.effect() == MANAGE_OFFER_CREATED ? success.offer().offerID
                                                    : 0;
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
