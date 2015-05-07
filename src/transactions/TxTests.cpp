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
#include "transactions/PaymentOpFrame.h"
#include "transactions/ChangeTrustOpFrame.h"
#include "transactions/CreateOfferOpFrame.h"
#include "transactions/SetOptionsOpFrame.h"
#include "transactions/AllowTrustOpFrame.h"
#include "transactions/InflationOpFrame.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;
namespace stellar
{
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

SequenceNumber
getAccountSeqNum(SecretKey const& k, Application& app)
{
    AccountFrame account;
    REQUIRE(AccountFrame::loadAccount(k.getPublicKey(), account,
                                      app.getDatabase()));
    return account.getSeqNum();
}

uint64_t
getAccountBalance(SecretKey const& k, Application& app)
{
    AccountFrame account;
    REQUIRE(AccountFrame::loadAccount(k.getPublicKey(), account,
                                      app.getDatabase()));
    return account.getBalance();
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
    e.tx.maxLedger = UINT32_MAX;
    e.tx.minLedger = 0;
    e.tx.maxFee = 12;
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
    op.body.changeTrustOp().line.type(ISO4217);
    strToCurrencyCode(op.body.changeTrustOp().line.isoCI().currencyCode,
                      currencyCode);
    op.body.changeTrustOp().line.isoCI().issuer = to.getPublicKey();

    return transactionFromOperation(from, seq, op);
}

TransactionFramePtr
createAllowTrust(SecretKey& from, SecretKey& trustor, SequenceNumber seq,
                 std::string const& currencyCode, bool authorize)
{
    Operation op;

    op.body.type(ALLOW_TRUST);
    op.body.allowTrustOp().trustor = trustor.getPublicKey();
    op.body.allowTrustOp().currency.type(ISO4217);
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
createPaymentTx(SecretKey& from, SecretKey& to, SequenceNumber seq,
                int64_t amount)
{
    Operation op;
    op.body.type(PAYMENT);
    op.body.paymentOp().amount = amount;
    op.body.paymentOp().destination = to.getPublicKey();
    op.body.paymentOp().sendMax = INT64_MAX;
    op.body.paymentOp().currency.type(NATIVE);

    return transactionFromOperation(from, seq, op);
}

void
applyPaymentTx(Application& app, SecretKey& from, SecretKey& to,
               SequenceNumber seq, int64_t amount, PaymentResultCode result)
{
    TransactionFramePtr txFrame;

    AccountFrame fromAccount;
    AccountFrame toAccount;
    bool beforeToExists = AccountFrame::loadAccount(
        to.getPublicKey(), toAccount, app.getDatabase());

    REQUIRE(AccountFrame::loadAccount(from.getPublicKey(), fromAccount,
                                      app.getDatabase()));

    txFrame = createPaymentTx(from, to, seq, amount);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader());
    txFrame->apply(delta, app);

    checkTransaction(*txFrame);
    auto txResult = txFrame->getResult();
    auto innerCode = PaymentOpFrame::getInnerCode(txResult.result.results()[0]);
    REQUIRE(innerCode == result);

    REQUIRE(txResult.feeCharged == app.getLedgerManager().getTxFee());

    AccountFrame toAccountAfter;
    bool afterToExists = AccountFrame::loadAccount(
        to.getPublicKey(), toAccountAfter, app.getDatabase());

    if (!(innerCode == PAYMENT_SUCCESS || innerCode == PAYMENT_SUCCESS_MULTI))
    {
        // check that the target account didn't change
        REQUIRE(beforeToExists == afterToExists);
        if (beforeToExists && afterToExists)
        {
            REQUIRE(memcmp(&toAccount.getAccount(),
                           &toAccountAfter.getAccount(),
                           sizeof(AccountEntry)) == 0);
        }
    }
    else
    {
        REQUIRE(afterToExists);
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
                      SequenceNumber seq, int64_t amount,
                      std::vector<Currency>* path)
{
    Operation op;
    op.body.type(PAYMENT);
    op.body.paymentOp().amount = amount;
    op.body.paymentOp().currency = ci;
    op.body.paymentOp().destination = to.getPublicKey();
    op.body.paymentOp().sendMax = INT64_MAX;
    if (path)
    {
        for (auto const& cur : *path)
        {
            op.body.paymentOp().path.push_back(cur);
        }
    }

    return transactionFromOperation(from, seq, op);
}

Currency
makeCurrency(SecretKey& issuer, std::string const& code)
{
    Currency currency;
    currency.type(ISO4217);
    currency.isoCI().issuer = issuer.getPublicKey();
    strToCurrencyCode(currency.isoCI().currencyCode, code);
    return currency;
}

PaymentResult
applyCreditPaymentTx(Application& app, SecretKey& from, SecretKey& to,
                     Currency& ci, SequenceNumber seq, int64_t amount,
                     PaymentResultCode result, std::vector<Currency>* path)
{
    TransactionFramePtr txFrame;

    txFrame = createCreditPaymentTx(from, to, ci, seq, amount, path);

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
            REQUIRE(memcmp(&offerEntry, &offerResult.offer(),
                           sizeof(OfferEntry)) == 0);
            REQUIRE(offerEntry.price == price);
            REQUIRE(memcmp(&offerEntry.takerGets, &takerGets,
                           sizeof(Currency)) == 0);
            REQUIRE(memcmp(&offerEntry.takerPays, &takerPays,
                           sizeof(Currency)) == 0);
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
