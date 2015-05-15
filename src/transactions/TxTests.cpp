// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Application.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "crypto/Base58.h"
#include "transactions/TxTests.h"
#include "util/types.h"
#include "transactions/TransactionFrame.h"
#include "ledger/LedgerDelta.h"
#include "transactions/PathPaymentOpFrame.h"
#include "transactions/PaymentOpFrame.h"
#include "transactions/ChangeTrustOpFrame.h"
#include "transactions/CreateAccountOpFrame.h"
#include "transactions/CreateOfferOpFrame.h"
#include "transactions/SetOptionsOpFrame.h"
#include "transactions/AllowTrustOpFrame.h"
#include "transactions/InflationOpFrame.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;
namespace stellar
{
using xdr::operator==;

namespace txtest
{

SecretKey
getRoot()
{
    std::string b58SeedStr =
        toBase58Check(VER_SEED, "allmylifemyhearthasbeensearching");
    return SecretKey::fromBase58Seed(b58SeedStr);
}

SecretKey
getAccount(const char* n)
{
    // stretch name to 32 bytes
    std::string name(n);
    while (name.size() < 32)
        name += '.';
    std::string b58SeedStr = toBase58Check(VER_SEED, name);
    return SecretKey::fromBase58Seed(b58SeedStr);
}

AccountFrame::pointer
loadAccount(SecretKey const& k, Application& app, bool mustExist)
{
    AccountFrame::pointer res =
        AccountFrame::loadAccount(k.getPublicKey(), app.getDatabase());
    if (mustExist)
    {
        REQUIRE(res);
    }
    return res;
}

void
requireNoAccount(SecretKey const& k, Application& app)
{
    AccountFrame::pointer res = loadAccount(k, app, false);
    REQUIRE(!res);
}

SequenceNumber
getAccountSeqNum(SecretKey const& k, Application& app)
{
    AccountFrame::pointer account;
    account = loadAccount(k, app);
    return account->getSeqNum();
}

uint64_t
getAccountBalance(SecretKey const& k, Application& app)
{
    AccountFrame::pointer account;
    account = loadAccount(k, app);
    return account->getBalance();
}

void
checkTransaction(TransactionFrame& txFrame)
{
    REQUIRE(txFrame.getResult().feeCharged == 10); // default fee
    REQUIRE((txFrame.getResultCode() == txSUCCESS ||
             txFrame.getResultCode() == txFAILED));
}

TransactionFramePtr
transactionFromOperation(SecretKey& from, SequenceNumber seq,
                         Operation const& op)
{
    TransactionEnvelope e;

    e.tx.sourceAccount = from.getPublicKey();
    e.tx.fee = 10;
    e.tx.seqNum = seq;
    e.tx.operations.push_back(op);

    TransactionFramePtr res = TransactionFrame::makeTransactionFromWire(e);

    res->addSignature(from);

    return res;
}

TransactionFramePtr
createChangeTrust(SecretKey& from, SecretKey& to, SequenceNumber seq,
                  std::string const& currencyCode, int64_t limit)
{
    Operation op;

    op.body.type(CHANGE_TRUST);
    op.body.changeTrustOp().limit = limit;
    op.body.changeTrustOp().line.type(CURRENCY_TYPE_ALPHANUM);
    strToCurrencyCode(op.body.changeTrustOp().line.alphaNum().currencyCode,
                      currencyCode);
    op.body.changeTrustOp().line.alphaNum().issuer = to.getPublicKey();

    return transactionFromOperation(from, seq, op);
}

TransactionFramePtr
createAllowTrust(SecretKey& from, SecretKey& trustor, SequenceNumber seq,
                 std::string const& currencyCode, bool authorize)
{
    Operation op;

    op.body.type(ALLOW_TRUST);
    op.body.allowTrustOp().trustor = trustor.getPublicKey();
    op.body.allowTrustOp().currency.type(CURRENCY_TYPE_ALPHANUM);
    strToCurrencyCode(op.body.allowTrustOp().currency.currencyCode(),
                      currencyCode);
    op.body.allowTrustOp().authorize = authorize;

    return transactionFromOperation(from, seq, op);
}

void
applyAllowTrust(Application& app, SecretKey& from, SecretKey& trustor,
                SequenceNumber seq, std::string const& currencyCode,
                bool authorize, AllowTrustResultCode result)
{
    TransactionFramePtr txFrame;
    txFrame = createAllowTrust(from, trustor, seq, currencyCode, authorize);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader());
    txFrame->apply(delta, app);

    checkTransaction(*txFrame);
    REQUIRE(AllowTrustOpFrame::getInnerCode(
                txFrame->getResult().result.results()[0]) == result);
}

TransactionFramePtr
createCreateAccountTx(SecretKey& from, SecretKey& to, SequenceNumber seq,
                      int64_t amount)
{
    Operation op;
    op.body.type(CREATE_ACCOUNT);
    op.body.createAccountOp().startingBalance = amount;
    op.body.createAccountOp().destination = to.getPublicKey();

    return transactionFromOperation(from, seq, op);
}

void
applyCreateAccountTx(Application& app, SecretKey& from, SecretKey& to,
                     SequenceNumber seq, int64_t amount,
                     CreateAccountResultCode result)
{
    TransactionFramePtr txFrame;

    AccountFrame::pointer fromAccount, toAccount;
    toAccount = loadAccount(to, app, false);

    fromAccount = loadAccount(from, app);

    txFrame = createCreateAccountTx(from, to, seq, amount);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader());
    txFrame->apply(delta, app);

    checkTransaction(*txFrame);
    auto txResult = txFrame->getResult();
    auto innerCode =
        CreateAccountOpFrame::getInnerCode(txResult.result.results()[0]);
    REQUIRE(innerCode == result);

    REQUIRE(txResult.feeCharged == app.getLedgerManager().getTxFee());

    AccountFrame::pointer toAccountAfter;
    toAccountAfter = loadAccount(to, app, false);

    if (innerCode != CREATE_ACCOUNT_SUCCESS)
    {
        // check that the target account didn't change
        REQUIRE(!!toAccount == !!toAccountAfter);
        if (toAccount && toAccountAfter)
        {
            REQUIRE(toAccount->getAccount() == toAccountAfter->getAccount());
        }
    }
    else
    {
        REQUIRE(toAccountAfter);
    }
}

TransactionFramePtr
createPaymentTx(SecretKey& from, SecretKey& to, SequenceNumber seq,
                int64_t amount)
{
    Operation op;
    op.body.type(PAYMENT);
    op.body.paymentOp().amount = amount;
    op.body.paymentOp().destination = to.getPublicKey();
    op.body.paymentOp().currency.type(CURRENCY_TYPE_NATIVE);

    return transactionFromOperation(from, seq, op);
}

void
applyPaymentTx(Application& app, SecretKey& from, SecretKey& to,
               SequenceNumber seq, int64_t amount, PaymentResultCode result)
{
    TransactionFramePtr txFrame;

    AccountFrame::pointer fromAccount, toAccount;
    toAccount = loadAccount(to, app, false);

    fromAccount = loadAccount(from, app);

    txFrame = createPaymentTx(from, to, seq, amount);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader());
    txFrame->apply(delta, app);

    checkTransaction(*txFrame);
    auto txResult = txFrame->getResult();
    auto innerCode = PaymentOpFrame::getInnerCode(txResult.result.results()[0]);
    REQUIRE(innerCode == result);

    REQUIRE(txResult.feeCharged == app.getLedgerManager().getTxFee());

    AccountFrame::pointer toAccountAfter;
    toAccountAfter = loadAccount(to, app, false);

    if (innerCode != PAYMENT_SUCCESS)
    {
        // check that the target account didn't change
        REQUIRE(!!toAccount == !!toAccountAfter);
        if (toAccount && toAccountAfter)
        {
            REQUIRE(toAccount->getAccount() == toAccountAfter->getAccount());
        }
    }
    else
    {
        REQUIRE(toAccount);
        REQUIRE(toAccountAfter);
    }
}

void
applyChangeTrust(Application& app, SecretKey& from, SecretKey& to,
                 SequenceNumber seq, std::string const& currencyCode,
                 int64_t limit, ChangeTrustResultCode result)
{
    TransactionFramePtr txFrame;

    txFrame = createChangeTrust(from, to, seq, currencyCode, limit);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader());
    txFrame->apply(delta, app);

    checkTransaction(*txFrame);
    REQUIRE(ChangeTrustOpFrame::getInnerCode(
                txFrame->getResult().result.results()[0]) == result);
}

TransactionFramePtr
createCreditPaymentTx(SecretKey& from, SecretKey& to, Currency& ci,
                      SequenceNumber seq, int64_t amount)
{
    Operation op;
    op.body.type(PAYMENT);
    op.body.paymentOp().amount = amount;
    op.body.paymentOp().currency = ci;
    op.body.paymentOp().destination = to.getPublicKey();

    return transactionFromOperation(from, seq, op);
}

Currency
makeCurrency(SecretKey& issuer, std::string const& code)
{
    Currency currency;
    currency.type(CURRENCY_TYPE_ALPHANUM);
    currency.alphaNum().issuer = issuer.getPublicKey();
    strToCurrencyCode(currency.alphaNum().currencyCode, code);
    return currency;
}

PaymentResult
applyCreditPaymentTx(Application& app, SecretKey& from, SecretKey& to,
                     Currency& ci, SequenceNumber seq, int64_t amount,
                     PaymentResultCode result)
{
    TransactionFramePtr txFrame;

    txFrame = createCreditPaymentTx(from, to, ci, seq, amount);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader());
    txFrame->apply(delta, app);

    checkTransaction(*txFrame);

    auto& firstResult = getFirstResult(*txFrame);

    PaymentResult res = firstResult.tr().paymentResult();
    auto resCode = res.code();
    REQUIRE(resCode == result);
    return res;
}

TransactionFramePtr
createPathPaymentTx(SecretKey& from, SecretKey& to, Currency const& sendCur,
                    int64_t sendMax, Currency const& destCur,
                    int64_t destAmount, SequenceNumber seq,
                    std::vector<Currency>* path)
{
    Operation op;
    op.body.type(PATH_PAYMENT);
    PathPaymentOp& ppop = op.body.pathPaymentOp();
    ppop.sendCurrency = sendCur;
    ppop.sendMax = sendMax;
    ppop.destCurrency = destCur;
    ppop.destAmount = destAmount;
    ppop.destination = to.getPublicKey();
    if (path)
    {
        for (auto const& cur : *path)
        {
            ppop.path.push_back(cur);
        }
    }

    return transactionFromOperation(from, seq, op);
}

PathPaymentResult
applyPathPaymentTx(Application& app, SecretKey& from, SecretKey& to,
                   Currency const& sendCur, int64_t sendMax,
                   Currency const& destCur, int64_t destAmount,
                   SequenceNumber seq, PathPaymentResultCode result,
                   std::vector<Currency>* path)
{
    TransactionFramePtr txFrame;

    txFrame = createPathPaymentTx(from, to, sendCur, sendMax, destCur,
                                  destAmount, seq, path);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader());
    txFrame->apply(delta, app);

    checkTransaction(*txFrame);

    auto& firstResult = getFirstResult(*txFrame);

    PathPaymentResult res = firstResult.tr().pathPaymentResult();
    auto resCode = res.code();
    REQUIRE(resCode == result);
    return res;
}

TransactionFramePtr
createOfferOp(uint64 offerId, SecretKey& source, Currency& takerGets,
              Currency& takerPays, Price const& price, int64_t amount,
              SequenceNumber seq)
{
    Operation op;
    op.body.type(CREATE_OFFER);
    op.body.createOfferOp().amount = amount;
    op.body.createOfferOp().takerGets = takerGets;
    op.body.createOfferOp().takerPays = takerPays;
    op.body.createOfferOp().offerID = offerId;
    op.body.createOfferOp().price = price;

    return transactionFromOperation(source, seq, op);
}

static CreateOfferResult
applyCreateOfferHelper(Application& app, LedgerDelta& delta, uint64 offerId,
                       SecretKey& source, Currency& takerGets,
                       Currency& takerPays, Price const& price, int64_t amount,
                       SequenceNumber seq)
{
    uint64_t expectedOfferID = delta.getHeaderFrame().getLastGeneratedID() + 1;
    if (offerId != 0)
    {
        expectedOfferID = offerId;
    }

    TransactionFramePtr txFrame;

    txFrame = createOfferOp(offerId, source, takerGets, takerPays, price,
                            amount, seq);

    txFrame->apply(delta, app);

    checkTransaction(*txFrame);

    auto& results = txFrame->getResult().result.results();

    REQUIRE(results.size() == 1);

    auto& createOfferResult = results[0].tr().createOfferResult();

    if (createOfferResult.code() == CREATE_OFFER_SUCCESS)
    {
        OfferFrame offer;

        auto& offerResult = createOfferResult.success().offer;
        auto& offerEntry = offer.getOffer();

        switch (offerResult.effect())
        {
        case CREATE_OFFER_CREATED:
        case CREATE_OFFER_UPDATED:
            REQUIRE(OfferFrame::loadOffer(source.getPublicKey(),
                                          expectedOfferID, offer,
                                          app.getDatabase()));
            REQUIRE(offerEntry == offerResult.offer());
            REQUIRE(offerEntry.price == price);
            REQUIRE(offerEntry.takerGets == takerGets);
            REQUIRE(offerEntry.takerPays == takerPays);
            break;
        case CREATE_OFFER_DELETED:
            REQUIRE(!OfferFrame::loadOffer(source.getPublicKey(),
                                           expectedOfferID, offer,
                                           app.getDatabase()));
            break;
        default:
            abort();
        }
    }

    return createOfferResult;
}

uint64_t
applyCreateOffer(Application& app, LedgerDelta& delta, uint64 offerId,
                 SecretKey& source, Currency& takerGets, Currency& takerPays,
                 Price const& price, int64_t amount, SequenceNumber seq)
{
    CreateOfferResult const& createOfferRes = applyCreateOfferHelper(
        app, delta, offerId, source, takerGets, takerPays, price, amount, seq);

    REQUIRE(createOfferRes.code() == CREATE_OFFER_SUCCESS);

    auto& success = createOfferRes.success().offer;

    REQUIRE(success.effect() == CREATE_OFFER_CREATED);

    return success.offer().offerID;
}

CreateOfferResult
applyCreateOfferWithResult(Application& app, LedgerDelta& delta, uint64 offerId,
                           SecretKey& source, Currency& takerGets,
                           Currency& takerPays, Price const& price,
                           int64_t amount, SequenceNumber seq,
                           CreateOfferResultCode result)
{
    CreateOfferResult const& createOfferRes = applyCreateOfferHelper(
        app, delta, offerId, source, takerGets, takerPays, price, amount, seq);

    auto res = createOfferRes.code();
    REQUIRE(res == result);

    return createOfferRes;
}

TransactionFramePtr
createSetOptions(SecretKey& source, AccountID* inflationDest,
                 uint32_t* setFlags, uint32_t* clearFlags, Thresholds* thrs,
                 Signer* signer, SequenceNumber seq)
{
    Operation op;
    op.body.type(SET_OPTIONS);

    if (inflationDest)
    {
        op.body.setOptionsOp().inflationDest.activate() = *inflationDest;
    }

    if (setFlags)
    {
        op.body.setOptionsOp().setFlags.activate() = *setFlags;
    }

    if (clearFlags)
    {
        op.body.setOptionsOp().clearFlags.activate() = *clearFlags;
    }

    if (thrs)
    {
        op.body.setOptionsOp().thresholds.activate() = *thrs;
    }

    if (signer)
    {
        op.body.setOptionsOp().signer.activate() = *signer;
    }

    return transactionFromOperation(source, seq, op);
}

void
applySetOptions(Application& app, SecretKey& source, AccountID* inflationDest,
                uint32_t* setFlags, uint32_t* clearFlags, Thresholds* thrs,
                Signer* signer, SequenceNumber seq, SetOptionsResultCode result)
{
    TransactionFramePtr txFrame;

    txFrame = createSetOptions(source, inflationDest, setFlags, clearFlags,
                               thrs, signer, seq);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader());
    txFrame->apply(delta, app);

    checkTransaction(*txFrame);
    REQUIRE(SetOptionsOpFrame::getInnerCode(
                txFrame->getResult().result.results()[0]) == result);
}

TransactionFramePtr
createInflation(SecretKey& from, SequenceNumber seq)
{
    Operation op;
    op.body.type(INFLATION);

    return transactionFromOperation(from, seq, op);
}

OperationResult
applyInflation(Application& app, SecretKey& from, SequenceNumber seq,
               InflationResultCode result)
{
    TransactionFramePtr txFrame = createInflation(from, seq);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader());
    bool res = txFrame->apply(delta, app);

    checkTransaction(*txFrame);
    REQUIRE(InflationOpFrame::getInnerCode(
                txFrame->getResult().result.results()[0]) == result);
    if (res)
    {
        delta.commit();
    }
    return getFirstResult(*txFrame);
}

OperationFrame const&
getFirstOperationFrame(TransactionFrame const& tx)
{
    return *(tx.getOperations()[0]);
}

OperationResult const&
getFirstResult(TransactionFrame const& tx)
{
    return getFirstOperationFrame(tx).getResult();
}

OperationResultCode
getFirstResultCode(TransactionFrame const& tx)
{
    return getFirstOperationFrame(tx).getResultCode();
}

Operation&
getFirstOperation(TransactionFrame& tx)
{
    return tx.getEnvelope().tx.operations[0];
}

void
reSignTransaction(TransactionFrame& tx, SecretKey& source)
{
    tx.getEnvelope().signatures.clear();
    tx.addSignature(source);
}

void
checkAmounts(int64_t a, int64_t b, int64_t maxd)
{
    int64_t d = b - maxd;
    REQUIRE(a >= d);
    REQUIRE(a <= b);
}
}
}
