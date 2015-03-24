// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

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
        toBase58Check(VER_SEED, "masterpassphrasemasterpassphrase");
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

    e.tx.account = from.getPublicKey();
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
                  const std::string& currencyCode, int64_t limit)
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
                 const std::string& currencyCode, bool authorize)
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
                SequenceNumber seq, const std::string& currencyCode,
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

    txFrame = createPaymentTx(from, to, seq, amount);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader());
    txFrame->apply(delta, app);

    checkTransaction(*txFrame);
    REQUIRE(PaymentOpFrame::getInnerCode(
                txFrame->getResult().result.results()[0]) == result);
}

void
applyChangeTrust(Application& app, SecretKey& from, SecretKey& to,
                 SequenceNumber seq, const std::string& currencyCode,
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
    op.body.paymentOp().sendMax = INT64_MAX;

    return transactionFromOperation(from, seq, op);
}

Currency
makeCurrency(SecretKey& issuer, const std::string& code)
{
    Currency currency;
    currency.type(ISO4217);
    currency.isoCI().issuer = issuer.getPublicKey();
    strToCurrencyCode(currency.isoCI().currencyCode, code);
    return currency;
}

void
applyCreditPaymentTx(Application& app, SecretKey& from, SecretKey& to,
                     Currency& ci, SequenceNumber seq, int64_t amount,
                     PaymentResultCode result)
{
    TransactionFramePtr txFrame;

    txFrame = createCreditPaymentTx(from, to, ci, seq, amount);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader());
    txFrame->apply(delta, app);

    checkTransaction(*txFrame);
    REQUIRE(PaymentOpFrame::getInnerCode(
                txFrame->getResult().result.results()[0]) == result);
}

TransactionFramePtr
createOfferOp(uint64 offerId, SecretKey& source, 
              Currency& takerGets, Currency& takerPays,
              Price const& price, int64_t amount, 
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
                       SecretKey& source,
                       Currency& takerGets, Currency& takerPays,
                       Price const& price, int64_t amount, SequenceNumber seq)
{
    TransactionFramePtr txFrame;

    txFrame = createOfferOp(offerId, source, takerGets, takerPays, price, amount, seq);

    txFrame->apply(delta, app);

    checkTransaction(*txFrame);

    auto& results = txFrame->getResult().result.results();

    REQUIRE(results.size() == 1);

    return results[0].tr().createOfferResult();
}

uint64_t
applyCreateOffer(Application& app, LedgerDelta& delta, uint64 offerId, 
                 SecretKey& source,
                 Currency& takerGets, Currency& takerPays, Price const& price,
                 int64_t amount, SequenceNumber seq)
{
    uint64_t expectedOfferID = delta.getHeaderFrame().getLastGeneratedID() + 1;

    CreateOfferResult const& createOfferRes = applyCreateOfferHelper(
        app, delta, offerId, source, takerGets, takerPays, price, amount, seq);

    REQUIRE(createOfferRes.code() == CREATE_OFFER_SUCCESS);

    auto& success = createOfferRes.success().offer;

    REQUIRE(success.effect() == CREATE_OFFER_CREATED);

    auto& offerRes = success.offerCreated();
    REQUIRE(offerRes.offerID == expectedOfferID);

    // verify that the created offer is in the database
    OfferFrame offer;
    REQUIRE(OfferFrame::loadOffer(source.getPublicKey(), expectedOfferID, offer,
                                  app.getDatabase()));

    return offerRes.offerID;
}

CreateOfferResult
applyCreateOfferWithResult(Application& app, LedgerDelta& delta,
                           uint64 offerId, 
                           SecretKey& source, Currency& takerGets,
                           Currency& takerPays, Price const& price,
                           int64_t amount, SequenceNumber seq,
                           CreateOfferResultCode result)
{
    CreateOfferResult const& createOfferRes = applyCreateOfferHelper(
        app, delta, offerId, source, takerGets, takerPays, price, amount, seq);

    REQUIRE(createOfferRes.code() == result);

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
}
}
